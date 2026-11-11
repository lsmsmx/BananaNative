// The binding walker (vka_bind.h): nvn_mtl.m encode_args' sources for each binding kind, written once for the Vulkan replay and its Mac test
#include "vka_bind.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

_Static_assert(DMSH_C2_TEX + DMSH_C2_ROW * DMSH_C2_TEX_SLOTS == DMSH_C2_SEPTEX && DMSH_C2_SEPTEX + DMSH_C2_ROW * DMSH_C2_SEP_SLOTS == DMSH_C2_SAMP
               && DMSH_C2_SAMP + DMSH_C2_ROW * DMSH_C2_SEP_SLOTS <= DMSH_DRV_SIZE, "NVN driver handle tables");

uint64_t vka_tex_slot(const vka_bstate *s, int st, unsigned reg) {
    return reg >= 32 && reg - 32 < DMSH_C2_IMG_SLOTS && s->img[st][reg - 32] ? s->img[st][reg - 32] : reg < VKA_NTEX ? s->tex[st][reg] : 0;
}
uint64_t vka_c2_handle(const vka_bstate *s, int st, unsigned b) {
    if (b >= DMSH_C2_TEX && b < DMSH_C2_SEPTEX && !((b - DMSH_C2_TEX) % DMSH_C2_ROW)) return vka_tex_slot(s, st, (b - DMSH_C2_TEX) / DMSH_C2_ROW);
    if (b >= DMSH_C2_SEPTEX && b < DMSH_C2_SAMP && !((b - DMSH_C2_SEPTEX) % DMSH_C2_ROW)) return s->tex[st][DMSH_SEPTEX_SLOT0 + (b - DMSH_C2_SEPTEX) / DMSH_C2_ROW];
    if (b >= DMSH_C2_SAMP && b < DMSH_C2_SAMP + DMSH_C2_ROW * DMSH_C2_SEP_SLOTS && !((b - DMSH_C2_SAMP) % DMSH_C2_ROW)) return s->samp[st][(b - DMSH_C2_SAMP) / DMSH_C2_ROW];
    return 0;
}
static uint64_t ubo_word(const vka_bstate *s, int st, int reg, unsigned off, unsigned n, vka_read_fn rd, void *ctx) {
    uint64_t v = 0;
    if (reg < 0 || reg >= 16 || !s->ubo[st][reg].addr || !rd) return 0;
    if (s->ubo[st][reg].size && off + n > s->ubo[st][reg].size) return 0;
    if (!rd(ctx, s->ubo[st][reg].addr + off, &v, n)) return 0;
    return v;
}
int vka_texu_elem = 1;
// Element k's byte offset past an array's base: a uniform buffer's array of 64-bit NVN handles that the shader indexes (BindlessToArray: the host fills element k from base + 8k)
static unsigned ubo_elem(const dmsh_vk_bind *b, unsigned k) { return b->count > 1 && vka_texu_elem ? 8u * k : 0; }
static int ubo_tex_id(const vka_bstate *s, uint32_t h) { int t = (int)(h & 0xfffff); return t ? t + s->tex_base : 0; }
static int ubo_samp_id(const vka_bstate *s, uint32_t h) { int x = (int)(h >> 20); return x ? x + s->samp_base : 0; }

// The sampler half of a combined image sampler (skind, sreg, soff); element k of an array
static int samp_of(const vka_bstate *s, int st, const dmsh_vk_bind *b, unsigned k, vka_read_fn rd, void *ctx) {
    switch (b->skind) {
    case DMSH_VK_KIND_SAMP_TEX: return b->sreg >= 0 && b->sreg < VKA_NTEX ? (int)(s->tex[st][b->sreg] >> 20 & 0xfff) : 0;
    case DMSH_VK_KIND_SAMP: return b->sreg >= 0 && b->sreg < 128 ? (int)(s->samp[st][b->sreg] >> 20 & 0xfff) : 0;
    case DMSH_VK_KIND_SAMP_ID: return b->sreg > 0 ? b->sreg : 0;   // a constant of the shader (nvn_mtl.m NVN_RB_SAMPI): taken as it is
    case DMSH_VK_KIND_SAMP_UBO: return ubo_samp_id(s, (uint32_t)ubo_word(s, st, b->sreg, b->soff + ubo_elem(b, k), 4, rd, ctx));
    case DMSH_VK_KIND_SAMP_C2A: {   // the handle at c2 byte off + 8 k, ORed with the separate sampler word at soff (nvn_mtl.m NVN_RB_SAMPA)
        uint64_t sw = b->soff != 0xFFFF ? vka_c2_handle(s, st, b->soff) : 0;
        return (int)((vka_c2_handle(s, st, b->off + 8u * k) | sw) >> 20 & 0xfff); }
    default: return 0;
    }
}

static int refuse(char *why, size_t n, const char *fmt, ...) {
    if (why && n) { va_list ap; va_start(ap, fmt); vsnprintf(why, n, fmt, ap); va_end(ap); }
    return VKA_WALK_REFUSE;
}

int vka_walk(const vka_bstate *s, int st, const dmsh_vk_entry *e, const dmsh_vk_bind *bs, uint32_t ssbo_align, uint64_t dev_align, vka_read_fn rd, void *ctx,
             vka_res *out, int max, int *uses_pool, char *why, size_t whyn) {
    int n = 0;
    if (st < 0 || st >= VKA_NSTAGE) return refuse(why, whyn, "NVN stage %d", st);
    for (unsigned i = 0; i < e->bind_count; i++) {
        const dmsh_vk_bind *b = &bs[i];
        if (b->kind == DMSH_VK_KIND_UNKNOWN) return refuse(why, whyn, "binding %u has no placed kind", b->binding);
        if (b->set == DMSH_VK_SET_POOL || b->kind == DMSH_VK_KIND_TEX_POOL || b->kind == DMSH_VK_KIND_SAMP_POOL) { if (uses_pool) *uses_pool = 1; continue; }
        unsigned cnt = b->count ? b->count : 1;
        for (unsigned k = 0; k < cnt; k++) {
            if (n >= max) return refuse(why, whyn, "more than %d descriptors", max);
            vka_res r = {VKA_R_NULL, b->desc, b->flags, b->dim, b->binding, (uint16_t)k, 0, 0, 0, 0, 0};
            int reg = b->reg;
            int noslot = (b->flags & DMSH_VK_BF_NOSLOT) || reg < 0;
            switch (b->kind) {
            case DMSH_VK_KIND_CBUF:
                if (!noslot && reg < 16 && s->ubo[st][reg].addr) { r.what = VKA_R_BUF; r.addr = s->ubo[st][reg].addr; r.size = s->ubo[st][reg].size; }
                break;
            case DMSH_VK_KIND_C1: r.what = VKA_R_C1; break;
            case DMSH_VK_KIND_SUPPORT: r.what = VKA_R_SUPPORT; break;
            case DMSH_VK_KIND_DRVCB: r.what = VKA_R_DRV; r.bank = (int16_t)reg; break;
            case DMSH_VK_KIND_SBUF: case DMSH_VK_KIND_UBO_PTR: {
                const vka_range *u = noslot || reg >= 16 ? NULL : b->kind == DMSH_VK_KIND_SBUF ? &s->ssbo[st][reg] : &s->ubo[st][reg];
                if (u && u->addr) { r.what = VKA_R_BUF; r.addr = u->addr; r.size = u->size; }
                break; }
            case DMSH_VK_KIND_SBUF_UBO: {
                uint64_t a = noslot ? 0 : ubo_word(s, st, reg, b->off, 8, rd, ctx);
                if (a) { r.what = VKA_R_BUF; r.addr = a; r.size = 0; }   // the size is not known: to the end of the chunk
                break; }
            case DMSH_VK_KIND_TEX: case DMSH_VK_KIND_TEX_UBO: case DMSH_VK_KIND_TEX_C2A: {
                int tid = b->kind == DMSH_VK_KIND_TEX_UBO ? (noslot ? 0 : ubo_tex_id(s, (uint32_t)ubo_word(s, st, reg, b->off + ubo_elem(b, k), 4, rd, ctx)))
                        : b->kind == DMSH_VK_KIND_TEX_C2A ? (int)(vka_c2_handle(s, st, b->off + 8u * k) & 0xfffff)
                        : noslot ? 0 : (int)(vka_tex_slot(s, st, (unsigned)reg) & 0xfffff);
                int sid = b->skind ? samp_of(s, st, b, k, rd, ctx) : 0;
                if (tid) { r.what = VKA_R_TEX; r.tid = tid; r.sid = sid; }
                break; }
            case DMSH_VK_KIND_TBUF: {
                int tid = noslot ? 0 : (int)(vka_tex_slot(s, st, (unsigned)reg) & 0xfffff);
                if (tid) { r.what = VKA_R_TBUF; r.tid = tid; }
                break; }
            case DMSH_VK_KIND_IMG: {
                int tid = noslot || reg >= 32 ? 0 : (int)(s->img[st][reg] & 0xfffff);
                if (tid) { r.what = VKA_R_IMG; r.tid = tid; }
                break; }
            case DMSH_VK_KIND_SAMP: case DMSH_VK_KIND_SAMP_TEX: case DMSH_VK_KIND_SAMP_ID: case DMSH_VK_KIND_SAMP_UBO: case DMSH_VK_KIND_SAMP_C2A: {
                dmsh_vk_bind sb = *b; sb.skind = b->kind; sb.sreg = b->reg; sb.soff = b->kind == DMSH_VK_KIND_SAMP_C2A ? b->soff : b->off;   // a sampler on its own: its own fields name it
                int sid = noslot && b->kind != DMSH_VK_KIND_SAMP_C2A ? 0 : samp_of(s, st, &sb, k, rd, ctx);
                if (sid) { r.what = VKA_R_TEX; r.sid = sid; }
                break; }
            case DMSH_VK_KIND_GMEM_MAP: r.what = VKA_R_GMEM_MAP; break;
            case DMSH_VK_KIND_GMEM_LOG: r.what = VKA_R_GMEM_LOG; break;
            case DMSH_VK_KIND_SHARED_SPILL:
                if (!b->bytes || b->bytes % 4) return refuse(why, whyn, "binding %u: shared memory spill of %u bytes a slice", b->binding, b->bytes);
                r.what = VKA_R_SHARED_SPILL; r.size = b->bytes; break;
            default: return refuse(why, whyn, "binding %u: kind %u", b->binding, b->kind);
            }
            if (r.what == VKA_R_BUF && r.desc == 7 /* VK_DESCRIPTOR_TYPE_STORAGE_BUFFER */) {
                if (ssbo_align) { uint64_t d = r.addr & (ssbo_align - 1); r.addr -= d; if (r.size) r.size += d; }   // the shader adds the difference back
                else if (dev_align && (r.addr & (dev_align - 1)))
                    return refuse(why, whyn, "storage buffer (binding %u, slot %d) at %#llx is not %llu-byte aligned (a library built before ssbo_align)", b->binding, reg, (unsigned long long)r.addr, (unsigned long long)dev_align);
            }
            out[n++] = r;
        }
    }
    return n;
}

static int c0_ssbo_base(int st) { return st == 0 ? DMSH_C0_SSBO_VS : st == 1 ? DMSH_C0_SSBO_PS : st == 2 ? DMSH_C0_SSBO_GS : st == 5 ? DMSH_C0_SSBO_CS : st == 6 ? DMSH_C0_SSBO_TS : st == 7 ? DMSH_C0_SSBO_MS : -1; }

// The pixel stage's sample table (nvn_mtl.m c0_sample_rows): one byte per sample, x in the low nibble and y in the high one in 1/16 pixel, at Vulkan's standard sample locations
static void c0_sample_rows(uint8_t *d, uint32_t samples) {
    static const uint8_t pos[5][16] = {
        {0x88},
        {0xCC, 0x44},
        {0x26, 0x6E, 0xA2, 0xEA},
        {0x59, 0xB7, 0x9D, 0x35, 0xD3, 0x71, 0xFB, 0x1F},
        {0x99, 0x57, 0xA5, 0x7C, 0x63, 0xDA, 0xBD, 0x3B, 0xE6, 0x18, 0x24, 0xC2, 0x80, 0x4F, 0xFE, 0x01},
    };
    if (samples < 1 || samples > 16 || (samples & (samples - 1))) samples = 1;
    int l = __builtin_ctz(samples);
    uint32_t info[4] = {samples - 1, (uint32_t)l, 0, 0};
    memcpy(d + DMSH_C0_SAMPLE_TABLE, pos[l], DMSH_C0_SAMPLE_BYTES); memcpy(d + DMSH_C0_SAMPLE_INFO, info, sizeof info);
}

size_t vka_drv_size(int bank) { return bank == DMSH_PUSH_BANK ? DMSH_PUSH_SIZE : DMSH_DRV_SIZE; }
void vka_drv_fill(const vka_bstate *s, int st, int bank, int pool, uint8_t *d) {
    if (bank == DMSH_PUSH_BANK) { memcpy(d, s->push[st == 5], DMSH_PUSH_SIZE); return; }
    memset(d, 0, DMSH_DRV_SIZE);
    if (bank == 0) {
        int base = c0_ssbo_base(st);
        if (base >= 0) for (int r = 0; r < DMSH_C0_SSBO_SLOTS; r++) {
            uint64_t a = s->ssbo[st][r].addr; uint32_t n = (uint32_t)s->ssbo[st][r].size;
            memcpy(d + base + DMSH_C0_ROW * r, &a, 8); memcpy(d + base + DMSH_C0_ROW * r + 8, &n, 4);
        }
        if (st != 5) { const float max_point = 2047.0f; memcpy(d + DMSH_C0_POINT_SIZE, &max_point, 4); memcpy(d + DMSH_C0_BASE_INSTANCE, &s->base_instance, 4); }
        if (st == 1) c0_sample_rows(d, s->samples);
        if (st == 5) {
            memcpy(d + DMSH_C0_DISPATCH, s->dispatch, 12);
            // NVN2's compute driver segment keeps the counts at c0 + 4 too (x, y, z), below the per-shader layout the translator remaps from 0x10 (InstEmitAmpere.AMapC0):
            // a shader that indexes by them reads 0 otherwise and every workgroup of a column writes the same entry (upstream nxnative 0a759de7: nvn_mtl.m encode_args_; Fire Emblem Fortune's Weave's light culling)
            if (s->nvn2) memcpy(d + DMSH_C0_DISPATCH_NVN2, s->dispatch, 12);
            for (int r = 0; r < DMSH_C0_CS_UBO_SLOTS; r++) {
                uint64_t a = s->ubo[5][r].addr; uint32_t n = (uint32_t)s->ubo[5][r].size;
                memcpy(d + DMSH_C0_CS_UBO + DMSH_C0_ROW * r, &a, 8); memcpy(d + DMSH_C0_CS_UBO + DMSH_C0_ROW * r + 8, &n, 4);
            }
        }
    } else if (bank == 2) {
        // A shader that reads the pools indexes them with the IDs it reads, which are the bound pool's own: its c2 holds the handles as the game bound them, without the bases
        int tb = pool ? s->tex_base : 0, sb = pool ? s->samp_base : 0;
#define C2_LOCAL(h) ((uint32_t)((((h) & 0xfffff) ? ((h) & 0xfffff) - (uint64_t)tb : 0) | ((((h) >> 20) & 0xfff) ? ((((h) >> 20) & 0xfff) - (uint64_t)sb) << 20 : 0)))
        const uint64_t *tx = s->tex[st], *im = s->img[st];
        for (int r = 0; r < DMSH_C2_IMG_SLOTS; r++) { uint32_t ih = (uint32_t)im[r]; memcpy(d + DMSH_C2_IMG + DMSH_C2_ROW * r, &ih, 4); }
        for (int r = 0; r < DMSH_C2_TEX_SLOTS; r++) {   // a combined slot past 31 is written when it is bound and the image slot it shares is not
            if (r >= 32 && (!tx[r] || (r - 32 < DMSH_C2_IMG_SLOTS && im[r - 32]))) continue;
            uint32_t v = C2_LOCAL(tx[r]); memcpy(d + DMSH_C2_TEX + DMSH_C2_ROW * r, &v, 4);
        }
        for (int r = 0; r < DMSH_C2_SEP_SLOTS; r++) { uint64_t h = tx[DMSH_SEPTEX_SLOT0 + r]; uint32_t v = C2_LOCAL(h); memcpy(d + DMSH_C2_SEPTEX + DMSH_C2_ROW * r, &v, 4); }
        for (int r = 0; r < DMSH_C2_SEP_SLOTS; r++) { uint64_t h = s->samp[st][r]; uint32_t v = C2_LOCAL(h); memcpy(d + DMSH_C2_SAMP + DMSH_C2_ROW * r, &v, 4); }
#undef C2_LOCAL
    }
}

void vka_support_fill(uint8_t *d, size_t n) {
    memset(d, 0, n);
    for (size_t o = 192; o + 4 <= n && o < 192 + 16 * 73; o += 16) { const float one = 1.0f; memcpy(d + o, &one, 4); }   // render_scale[73] (nvn_mtl.m support_buf)
}
