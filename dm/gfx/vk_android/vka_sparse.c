// Sparse residency images: a large texture of a sparse virtual pool (hle_nvn.c) gets memory only behind the 64 KB tiles the game maps (docs/reference/nvn-objects.md
// "Sparse texture arrays made at their full size").
// Before, such a texture was a dense image of every level with a mapped page: Mario Kart World 1.8.0's 8192x8192 x68 ASTC 8x8 arrays with a few mapped tiles in level 0 took
// 1.45 GB each with 128 MiB mapped, and its 6 GB array was refused (-2), at 147 MB of free memory before the first frame.
// NVN's sparse tile is the standard 64 KB block shape (nvn_tex.c sparse_tile_h: 256x256 / 256x128 / 128x128 / 128x64 / 64x64 blocks for 1 / 2 / 4 / 8 / 16 bytes a block), which
// is Vulkan's residencyStandard2DBlockShape: one NVN page of a tiled level is one Vulkan tile. The image is made with VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT (vka_tex.c img_create);
// per tile of its tiled levels a 64 KiB slot of a 64 MiB tile block is bound while the page table has any page of the tile mapped, and unbound when it has none (strict
// non-resident reads: an unbound tile reads 0, as an unmapped page reads on the Switch). Levels Vulkan tiles but NVN packs into its mip tail are bound whole, and the mip tail too.
// Order: binds are queued here and go to the queue with vkQueueBindSparse right before the next submission of the replay or of an upload batch (vks_queue_locked, under V.qmu).
// A bind batch waits for every submission handed to the queue before it (none still reads a tile it unbinds or rebinds): an empty vkQueueSubmit first signals the sparse
// timeline at a value of its own, which by queue order covers everything submitted before (replay parts that signal nothing, upload batches, framegen), after the batch before
// it; the bind waits for that value only and signals the next. Every later submission of the replay and of the upload batches waits for the bind's value, so the tiles are
// bound before the copies that fill them and the draws that read them. A batch that fails keeps its binds (and the slots its unbinds free) for the next one. A slot unbound is reused only by a later batch; a tile block is freed once empty and the batch that emptied it has run. No aliasing (a slot is one tile of one image):
// the phone has no sparseResidencyAliased.
// DM_VK_SPARSE_RES=0: dense images as before (read once)
#if defined(__ANDROID__)
#include "vka.h"
#include <stdlib.h>

#define TILE 65536u
#define BLOCK_TILES 1024u                     // 64 MiB tile blocks
typedef struct { VkDeviceMemory dm; uint32_t type, used; uint64_t bits[BLOCK_TILES / 64]; uint64_t free_at; } tblock;   // free_at: the sparse value after which, empty, it may be freed
struct vks_img {
    VkImage img; uint32_t type, w, h, levels, layers, tiled, mtfl, tw, th;
    uint32_t lofs[16], ntx[16], nty[16], per_layer;   // levels < mtfl: first tile of the level in a layer's run of slot[], tiles across and down; tiles a layer
    uint32_t *slot;                            // per tile of levels < mtfl, layer-major: slot id + 1, 0 = unbound
    uint32_t ntail, tail_each, *tail;          // mip tails (one, or one a layer) of tail_each slots each
    VkSparseImageMemoryRequirements sr;
    uint64_t last;                             // the last sparse value whose batch binds into it (under g_ks)
    uint32_t bound;                            // slots bound (tiles and tails)
};
static pthread_mutex_t g_ks = PTHREAD_MUTEX_INITIALIZER;
static tblock **g_tb; static uint32_t g_ntb, g_ctb;
static VkSemaphore g_stl; static uint64_t g_sval, g_sig;   // the sparse timeline: g_sval = the last bind batch's value handed to the queue, g_sig = the last value anything signals
                                                             // (the empty submissions before each batch take values too: a value >= a batch's means it has run)
typedef struct { vks_img *s; uint64_t seq; VkSparseImageMemoryBind b; } pimg;   // pending binds, with the image they bind into and their order
typedef struct { vks_img *s; uint64_t seq; VkSparseMemoryBind b; } popq;
static uint64_t g_seq;
static pimg *g_pi; static uint32_t g_npi, g_cpi;
static popq *g_po; static uint32_t g_npo, g_cpo;
static uint32_t *g_rel; static uint32_t g_nrel, g_crel;   // slots that pending binds unbind: given back once their batch is queued
static uint64_t g_bytes, g_va, g_nbatch, g_nbinds, g_nimg;
static int grow(void **p, uint32_t *cap, uint32_t n, size_t sz) {
    if (n < *cap) return 1;
    uint32_t c = *cap ? *cap * 2 : 256; void *q = realloc(*p, (size_t)c * sz); if (!q) return 0; *p = q; *cap = c; return 1;
}

int vks_on(void) { static int on = -1; if (on < 0) { const char *e = getenv("DM_VK_SPARSE_RES"); on = V.sparse_res && !(e && atoi(e) == 0); } return on; }

// Sparse address space (sparseAddressSpaceSize: 4095 MiB on the Adreno 840, less than Mario Kart World's arrays span at full size), taken by each image's virtual size
int vks_va_take(uint64_t n) {
    uint64_t lim = V.props.limits.sparseAddressSpaceSize, margin = 64ull << 20;
    pthread_mutex_lock(&g_ks); int ok = lim > margin && g_va + n <= lim - margin; if (ok) g_va += n; pthread_mutex_unlock(&g_ks); return ok;
}
void vks_va_give(uint64_t n) { pthread_mutex_lock(&g_ks); g_va -= n < g_va ? n : g_va; pthread_mutex_unlock(&g_ks); }
uint64_t vks_va_free(void) {
    uint64_t lim = V.props.limits.sparseAddressSpaceSize, margin = 64ull << 20;
    pthread_mutex_lock(&g_ks); uint64_t f = lim > margin + g_va ? lim - margin - g_va : 0; pthread_mutex_unlock(&g_ks); return f;
}

static uint64_t stl_now(void) { uint64_t v = 0; if (vka_hung()) return UINT64_MAX; if (g_stl) vk.vkGetSemaphoreCounterValue(V.dev, g_stl, &v); return v; }
// Empty blocks whose last unbind has run are freed while another block of their type has room (caller holds g_ks)
static void blocks_trim(void) {
    uint64_t now = 0; int got = 0;
    for (uint32_t i = 0; i < g_ntb; i++) {
        tblock *b = g_tb[i]; if (!b || b->used) continue;
        int other = 0; for (uint32_t j = 0; j < g_ntb && !other; j++) other = j != i && g_tb[j] && g_tb[j]->type == b->type && g_tb[j]->used < BLOCK_TILES;
        if (!other) continue;
        if (!got) { now = stl_now(); got = 1; }
        if (b->free_at > now) continue;
        vk.vkFreeMemory(V.dev, b->dm, NULL); free(b); g_tb[i] = NULL; g_bytes -= (uint64_t)BLOCK_TILES * TILE;
    }
}
// A free 64 KiB slot of memory type type: its id + 1, 0 = none (caller holds g_ks)
static uint32_t slot_take(uint32_t type) {
    tblock *b = NULL; uint32_t bi = 0;
    for (uint32_t i = 0; i < g_ntb && !b; i++) if (g_tb[i] && g_tb[i]->type == type && g_tb[i]->used < BLOCK_TILES) { b = g_tb[i]; bi = i; }
    if (!b) {
        for (bi = 0; bi < g_ntb && g_tb[bi]; bi++) {}
        if (bi == g_ntb && !grow((void **)&g_tb, &g_ctb, g_ntb, sizeof *g_tb)) return 0;
        if (!(b = calloc(1, sizeof *b))) return 0;
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = (uint64_t)BLOCK_TILES * TILE, .memoryTypeIndex = type};
        VkResult r = vk.vkAllocateMemory(V.dev, &ai, NULL, &b->dm);
        if (r) { free(b); static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VRED("sparse tile block of 64 MiB not allocated (%d; %.0f MiB of tiles): its tiles stay unbound (read 0)", r, g_bytes / 1048576.0); return 0; }
        b->type = type; g_tb[bi] = b; if (bi == g_ntb) g_ntb++;
        g_bytes += (uint64_t)BLOCK_TILES * TILE; vka_mem_grew((uint64_t)BLOCK_TILES * TILE);
    }
    for (uint32_t w = 0; w < BLOCK_TILES / 64; w++) if (~b->bits[w]) {
        uint32_t k = (uint32_t)__builtin_ctzll(~b->bits[w]); b->bits[w] |= 1ull << k; b->used++;
        return bi * BLOCK_TILES + w * 64 + k + 1;
    }
    return 0;
}
static void slot_give(uint32_t id, uint64_t at) {   // caller holds g_ks; at: the sparse value after which no binding uses it
    id--; tblock *b = id / BLOCK_TILES < g_ntb ? g_tb[id / BLOCK_TILES] : NULL; uint32_t k = id % BLOCK_TILES;
    if (!b || !(b->bits[k / 64] >> (k % 64) & 1)) { VRED_ONCE("sparse tile slot %u given back twice", id); return; }
    b->bits[k / 64] &= ~(1ull << (k % 64)); b->used--; if (at > b->free_at) b->free_at = at;
}
static VkDeviceMemory slot_mem(uint32_t id, VkDeviceSize *off) { id--; *off = (VkDeviceSize)(id % BLOCK_TILES) * TILE; return g_tb[id / BLOCK_TILES]->dm; }

// One tile's bind (id 0: unbind) queued (caller holds g_ks)
static int push_tile(vks_img *s, uint32_t lv, uint32_t ly, uint32_t tx, uint32_t ty, uint32_t id) {
    if (!grow((void **)&g_pi, &g_cpi, g_npi, sizeof *g_pi)) return 0;
    uint32_t lw = s->w >> lv ? s->w >> lv : 1, lh = s->h >> lv ? s->h >> lv : 1, x = tx * s->tw, y = ty * s->th;
    pimg *p = &g_pi[g_npi++]; memset(p, 0, sizeof *p); p->s = s; p->seq = g_seq++;
    p->b.subresource = (VkImageSubresource){VK_IMAGE_ASPECT_COLOR_BIT, lv, ly};
    p->b.offset = (VkOffset3D){(int32_t)x, (int32_t)y, 0};
    p->b.extent = (VkExtent3D){x + s->tw <= lw ? s->tw : lw - x, y + s->th <= lh ? s->th : lh - y, 1};   // a tile at the level's edge ends there
    if (id) p->b.memory = slot_mem(id, &p->b.memoryOffset);
    return 1;
}
static int push_opaque(vks_img *s, VkDeviceSize at, VkDeviceSize n, uint32_t id) {   // caller holds g_ks
    if (!grow((void **)&g_po, &g_cpo, g_npo, sizeof *g_po)) return 0;
    popq *p = &g_po[g_npo++]; memset(p, 0, sizeof *p); p->s = s; p->seq = g_seq++; p->b.resourceOffset = at; p->b.size = n;
    if (id) p->b.memory = slot_mem(id, &p->b.memoryOffset);
    return 1;
}
static uint32_t *tile_at(vks_img *s, uint32_t lv, uint32_t ly, uint32_t tx, uint32_t ty) { return &s->slot[(size_t)ly * s->per_layer + s->lofs[lv] + (size_t)ty * s->ntx[lv] + tx]; }
static int bind_tile(vks_img *s, uint32_t lv, uint32_t ly, uint32_t tx, uint32_t ty) {   // caller holds g_ks: 1 = bound
    uint32_t *t = tile_at(s, lv, ly, tx, ty);
    if (*t) return 1;
    uint32_t id = slot_take(s->type);
    if (!id) return 0;
    if (!push_tile(s, lv, ly, tx, ty, id)) { slot_give(id, 0); return 0; }
    *t = id; s->bound++; return 1;
}

// The residency of a new sparse image: its tile shape (texels, the format's granularity), the levels bound tile by tile (tiled: NVN's tiled levels that Vulkan tiles too).
// Levels from tiled to the mip tail and the mip tail are bound now. NULL: the image cannot be one (the caller makes a dense image)
vks_img *vks_new(VkImage img, uint32_t type, uint32_t w, uint32_t h, uint32_t levels, uint32_t layers, uint32_t tw, uint32_t th, uint32_t tiled) {
    uint32_t n = 0; vk.vkGetImageSparseMemoryRequirements(V.dev, img, &n, NULL);
    VkSparseImageMemoryRequirements sr[4];
    if (n > 4) { VRED_ONCE("sparse image: %u sparse memory requirements (at most 4 handled): dense images instead", n); return NULL; }
    vk.vkGetImageSparseMemoryRequirements(V.dev, img, &n, sr);
    int k = -1; for (uint32_t i = 0; i < n; i++) {
        // Metadata (an aspect the driver keeps beside the texels, e.g. for compression) would need its own binds (VK_SPARSE_MEMORY_BIND_METADATA_BIT); none are made: refused
        if (sr[i].formatProperties.aspectMask & VK_IMAGE_ASPECT_METADATA_BIT) { VRED_ONCE("sparse image: the driver asks for a metadata aspect (%llu bytes): dense images instead", (unsigned long long)sr[i].imageMipTailSize); return NULL; }
        if (sr[i].formatProperties.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) k = (int)i;
    }
    if (k < 0 || sr[k].formatProperties.imageGranularity.width != tw || sr[k].formatProperties.imageGranularity.height != th || (sr[k].formatProperties.flags & VK_SPARSE_IMAGE_FORMAT_NONSTANDARD_BLOCK_SIZE_BIT)) {
        VRED_ONCE("sparse image: the driver's tile (%ux%u, flags %#x) is not NVN's %ux%u: dense images instead", k < 0 ? 0 : sr[k].formatProperties.imageGranularity.width,
                  k < 0 ? 0 : sr[k].formatProperties.imageGranularity.height, k < 0 ? 0 : sr[k].formatProperties.flags, tw, th);
        return NULL;
    }
    vks_img *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->img = img; s->type = type; s->w = w; s->h = h; s->levels = levels; s->layers = layers; s->tw = tw; s->th = th; s->sr = sr[k];
    s->mtfl = sr[k].imageMipTailFirstLod < levels ? sr[k].imageMipTailFirstLod : levels;
    if (s->mtfl > 16) { free(s); return NULL; }
    s->tiled = tiled < s->mtfl ? tiled : s->mtfl;
    for (uint32_t lv = 0; lv < s->mtfl; lv++) {
        uint32_t lw = w >> lv ? w >> lv : 1, lh = h >> lv ? h >> lv : 1;
        s->ntx[lv] = (lw + tw - 1) / tw; s->nty[lv] = (lh + th - 1) / th; s->lofs[lv] = s->per_layer; s->per_layer += s->ntx[lv] * s->nty[lv];
    }
    int single = (sr[k].formatProperties.flags & VK_SPARSE_IMAGE_FORMAT_SINGLE_MIPTAIL_BIT) != 0;
    s->ntail = s->mtfl < levels && sr[k].imageMipTailSize ? (single ? 1 : layers) : 0;
    s->tail_each = (uint32_t)((sr[k].imageMipTailSize + TILE - 1) / TILE);
    s->slot = calloc((size_t)s->per_layer * layers + 1, sizeof *s->slot);
    s->tail = calloc((size_t)s->ntail * s->tail_each + 1, sizeof *s->tail);
    if (!s->slot || !s->tail) { free(s->slot); free(s->tail); free(s); return NULL; }
    int ok = 1;
    pthread_mutex_lock(&g_ks);
    if (!g_stl) {
        VkSemaphoreTypeCreateInfo st = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
        VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &st};
        if (vk.vkCreateSemaphore(V.dev, &sci, NULL, &g_stl)) { g_stl = VK_NULL_HANDLE; ok = 0; VRED("sparse timeline not made: dense images instead"); }
    }
    for (uint32_t ly = 0; ly < layers && ok; ly++)   // levels NVN keeps in its mip tail but Vulkan tiles: bound whole
        for (uint32_t lv = s->tiled; lv < s->mtfl && ok; lv++)
            for (uint32_t ty = 0; ty < s->nty[lv] && ok; ty++) for (uint32_t tx = 0; tx < s->ntx[lv] && ok; tx++) ok = bind_tile(s, lv, ly, tx, ty);
    for (uint32_t t = 0; t < s->ntail && ok; t++)
        for (uint32_t j = 0; j < s->tail_each && ok; j++) {
            uint32_t id = slot_take(type); VkDeviceSize at = sr[k].imageMipTailOffset + t * sr[k].imageMipTailStride + (VkDeviceSize)j * TILE;
            VkDeviceSize len = sr[k].imageMipTailSize - (VkDeviceSize)j * TILE; if (len > TILE) len = TILE;
            ok = id && push_opaque(s, at, len, id);
            if (ok) { s->tail[t * s->tail_each + j] = id; s->bound++; } else if (id) slot_give(id, 0);
        }
    g_nimg++;   // (vks_free counts it out)
    pthread_mutex_unlock(&g_ks);
    if (!ok) { vks_free(s); return NULL; }   // (its pending binds dropped, its slots back)
    return s;
}

// The tile of level lv, layer ly at (tx, ty) bound (want) or unbound, as the page table has it: 1 = bound now. Levels at or past the tiled ones are always bound
int vks_tile(vks_img *s, uint32_t lv, uint32_t ly, uint32_t tx, uint32_t ty, int want) {
    if (lv >= s->tiled) return 1;
    if (ly >= s->layers || tx >= s->ntx[lv] || ty >= s->nty[lv]) return 0;
    pthread_mutex_lock(&g_ks);
    uint32_t *t = tile_at(s, lv, ly, tx, ty); int r;
    if (want) r = bind_tile(s, lv, ly, tx, ty);
    else {
        if (*t && grow((void **)&g_rel, &g_crel, g_nrel, sizeof *g_rel) && push_tile(s, lv, ly, tx, ty, 0)) { g_rel[g_nrel++] = *t; *t = 0; s->bound--; }
        r = *t != 0;
    }
    pthread_mutex_unlock(&g_ks);
    return r;
}
int vks_tile_bound(vks_img *s, uint32_t lv, uint32_t ly, uint32_t tx, uint32_t ty) {
    if (lv >= s->tiled) return 1;
    if (ly >= s->layers || tx >= s->ntx[lv] || ty >= s->nty[lv]) return 0;
    pthread_mutex_lock(&g_ks); int r = *tile_at(s, lv, ly, tx, ty) != 0; pthread_mutex_unlock(&g_ks); return r;
}
uint32_t vks_tiled(const vks_img *s) { return s->tiled; }
uint64_t vks_img_bytes(vks_img *s) { pthread_mutex_lock(&g_ks); uint64_t b = (uint64_t)s->bound * TILE; pthread_mutex_unlock(&g_ks); return b; }

// The image goes (img_destroy, the GPU done with it, before vkDestroyImage): its pending binds are dropped, the batches that bound into it waited for, its slots given back
void vks_free(vks_img *s) {
    if (!s) return;
    pthread_mutex_lock(&g_ks);
    uint32_t j = 0; for (uint32_t i = 0; i < g_npi; i++) if (g_pi[i].s != s) g_pi[j++] = g_pi[i]; g_npi = j;
    j = 0; for (uint32_t i = 0; i < g_npo; i++) if (g_po[i].s != s) g_po[j++] = g_po[i]; g_npo = j;
    if (!g_npi && !g_npo) { for (uint32_t i = 0; i < g_nrel; i++) slot_give(g_rel[i], g_sval); g_nrel = 0; }   // (unbinds of its own tiles only: no batch would give them back)
    uint64_t last = s->last; VkSemaphore sem = g_stl;
    pthread_mutex_unlock(&g_ks);
    if (last && sem && !vka_hung()) {
        VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &sem, .pValues = &last};
        if (vk.vkWaitSemaphores(V.dev, &wi, vka_hang_ns())) VRED("sparse image: bind batch %llu not done in %.1f s: its slots are given back anyway", (unsigned long long)last, vka_hang_ns() / 1e9);
    }
    pthread_mutex_lock(&g_ks);
    uint64_t at = g_sval;   // (an unbind of one of its tiles may still be queued behind: a slot freed by the image's end is not reused before the next batch)
    for (size_t i = 0; i < (size_t)s->per_layer * s->layers; i++) if (s->slot[i]) slot_give(s->slot[i], at);
    for (size_t i = 0; i < (size_t)s->ntail * s->tail_each; i++) if (s->tail[i]) slot_give(s->tail[i], at);
    if (g_nimg) g_nimg--;
    blocks_trim();
    pthread_mutex_unlock(&g_ks);
    free(s->slot); free(s->tail); free(s);
}

// Before a submission of the replay or of an upload batch (caller holds V.qmu): the pending binds to the queue, and the wait the submission adds for the last bind batch
// (*nw grows by one when there was any batch: ws / wv / wst have room for one more)
// By image, then tile (subresource and offset), then the order they were made: of several binds of one tile in a batch only the last is kept (an unbind then a bind, or the reverse)
#define CMPK(x, y) do { if ((x) != (y)) return (x) < (y) ? -1 : 1; } while (0)
static int cmp_img(const void *a, const void *b) {
    const pimg *p = a, *q = b;
    CMPK((uintptr_t)p->s, (uintptr_t)q->s); CMPK(p->b.subresource.arrayLayer, q->b.subresource.arrayLayer); CMPK(p->b.subresource.mipLevel, q->b.subresource.mipLevel);
    CMPK(p->b.offset.y, q->b.offset.y); CMPK(p->b.offset.x, q->b.offset.x); CMPK(p->seq, q->seq); return 0;
}
static int cmp_opq(const void *a, const void *b) { const popq *p = a, *q = b; CMPK((uintptr_t)p->s, (uintptr_t)q->s); CMPK(p->seq, q->seq); return 0; }
void vks_queue_locked(uint32_t *nw, VkSemaphore *ws, uint64_t *wv, VkPipelineStageFlags *wst) {
    pthread_mutex_lock(&g_ks);
    if (!g_stl) { pthread_mutex_unlock(&g_ks); return; }   // no sparse image yet
    if (vka_hung()) { g_npi = g_npo = g_nrel = 0; pthread_mutex_unlock(&g_ks); return; }   // nothing runs any more: the binds dropped (their slots are not reused either)
    if (g_npi || g_npo) {
        qsort(g_pi, g_npi, sizeof *g_pi, cmp_img); qsort(g_po, g_npo, sizeof *g_po, cmp_opq);
        { uint32_t j = 0; for (uint32_t i = 0; i < g_npi; i++) {
              const pimg *p = &g_pi[i], *q = i + 1 < g_npi ? &g_pi[i + 1] : NULL;
              if (q && q->s == p->s && q->b.subresource.arrayLayer == p->b.subresource.arrayLayer && q->b.subresource.mipLevel == p->b.subresource.mipLevel
                  && q->b.offset.x == p->b.offset.x && q->b.offset.y == p->b.offset.y) continue;   // a later bind of the same tile
              g_pi[j++] = *p; }
          g_npi = j; }
        uint32_t ni = 0, no = 0;
        for (uint32_t i = 0; i < g_npi; i++) ni += !i || g_pi[i].s != g_pi[i - 1].s;
        for (uint32_t i = 0; i < g_npo; i++) no += !i || g_po[i].s != g_po[i - 1].s;
        VkSparseImageMemoryBindInfo *ii = calloc(ni + 1, sizeof *ii); VkSparseImageOpaqueMemoryBindInfo *oi = calloc(no + 1, sizeof *oi);
        VkSparseImageMemoryBind *ib = malloc(((size_t)g_npi + 1) * sizeof *ib); VkSparseMemoryBind *ob = malloc(((size_t)g_npo + 1) * sizeof *ob);
        if (ii && oi && ib && ob) {
            // Everything handed to the queue before: an empty submission signals g_sig + 1 once all of it (and the batch before, which it waits for: a timeline's values are
            // signalled in order) is done. vkQueueBindSparse does not wait for earlier submissions by itself, and replay parts and upload batches do not all signal V.tl
            uint64_t pre = g_sig + 1, n = pre + 1;
            VkPipelineStageFlags pst = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            VkTimelineSemaphoreSubmitInfo pti = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, .waitSemaphoreValueCount = g_sig ? 1 : 0, .pWaitSemaphoreValues = &g_sig,
                                                 .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &pre};
            VkSubmitInfo psi = {VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &pti, .waitSemaphoreCount = g_sig ? 1 : 0, .pWaitSemaphores = &g_stl, .pWaitDstStageMask = &pst,
                                .signalSemaphoreCount = 1, .pSignalSemaphores = &g_stl};
            VkResult r = vk.vkQueueSubmit(V.q, 1, &psi, VK_NULL_HANDLE);
            if (!r) {
                g_sig = pre;
                ni = 0; for (uint32_t i = 0; i < g_npi; i++) { ib[i] = g_pi[i].b; if (!i || g_pi[i].s != g_pi[i - 1].s) { ii[ni].image = g_pi[i].s->img; ii[ni].pBinds = &ib[i]; ni++; } ii[ni - 1].bindCount++; }
                no = 0; for (uint32_t i = 0; i < g_npo; i++) { ob[i] = g_po[i].b; if (!i || g_po[i].s != g_po[i - 1].s) { oi[no].image = g_po[i].s->img; oi[no].pBinds = &ob[i]; no++; } oi[no - 1].bindCount++; }
                VkTimelineSemaphoreSubmitInfo ti = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, .waitSemaphoreValueCount = 1, .pWaitSemaphoreValues = &pre, .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &n};
                VkBindSparseInfo bi = {VK_STRUCTURE_TYPE_BIND_SPARSE_INFO, .pNext = &ti, .waitSemaphoreCount = 1, .pWaitSemaphores = &g_stl, .imageOpaqueBindCount = no, .pImageOpaqueBinds = oi,
                                       .imageBindCount = ni, .pImageBinds = ii, .signalSemaphoreCount = 1, .pSignalSemaphores = &g_stl};
                r = vk.vkQueueBindSparse(V.q, 1, &bi, VK_NULL_HANDLE);
            }
            if (r == VK_ERROR_DEVICE_LOST) vka_device_lost("a sparse bind batch");   // (it takes neither lock)
            if (r) { static _Atomic int k; if (atomic_fetch_add(&k, 1) < 8) VRED("sparse bind batch of %u tile and %u tail binds: %s %d (kept for the next submission; what is uploaded into those tiles until then is lost until the game writes them again)",
                                                                                  g_npi, g_npo, g_sig == pre ? "vkQueueBindSparse" : "the submission before it", r); }
            else {
                g_sig = g_sval = n; g_nbatch++; g_nbinds += g_npi + g_npo;
                for (uint32_t i = 0; i < g_npi; i++) g_pi[i].s->last = n;
                for (uint32_t i = 0; i < g_npo; i++) g_po[i].s->last = n;
                for (uint32_t i = 0; i < g_nrel; i++) slot_give(g_rel[i], n);   // reused only by a later batch, which runs after this one
                g_npi = g_npo = g_nrel = 0;
                blocks_trim();
            }
        } else VRED_ONCE("sparse bind batch not built (no memory): the binds wait for the next submission");
        free(ii); free(oi); free(ib); free(ob);
    }
    if (g_sval) { ws[*nw] = g_stl; wv[*nw] = g_sval; wst[*nw] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT; (*nw)++; }
    pthread_mutex_unlock(&g_ks);
}

void vks_stats(char *o, size_t n) {
    pthread_mutex_lock(&g_ks);
    uint64_t used = 0; uint32_t nb = 0; for (uint32_t i = 0; i < g_ntb; i++) if (g_tb[i]) { nb++; used += g_tb[i]->used; }
    snprintf(o, n, "sparse images: %llu, %.0f MiB of address space; tiles %.1f MiB bound in %u blocks (%.0f MiB); %llu bind batches, %llu binds",
             (unsigned long long)g_nimg, g_va / 1048576.0, used * (double)TILE / 1048576.0, nb, g_bytes / 1048576.0, (unsigned long long)g_nbatch, (unsigned long long)g_nbinds);
    pthread_mutex_unlock(&g_ks);
}
uint64_t vks_bytes_nolock(void) { return g_bytes; }
#endif
