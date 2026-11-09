// Images that start at a finer texture level than 0 (vka_img.base): a sparse texture's unmapped levels (vka_tex.c sparse_base) and soft textures (profile tex_soft, below).
// The rules, free of Vulkan so the Mac's unit test (dm/host/tests/t_android_soft.c) can check them.
//
// Soft textures (profile tex_soft; DM_VK_TEX_SOFT overrides): a large block-compressed texture the game loaded from its files is kept without its finest level. Its image is
// that of level 1 (half the size each way, one level less, sampled through the same normalized coordinates: the level of detail is one lower, which is the level the image
// numbers from 0), so about three quarters of its bytes go. Ported from plumber-racer-2 (MIT, copyright seedds: nvn_mtl.m tex_soft, tex_soft_level, soft_harden), where it
// made Mario Kart World's race fit an iPhone 18 Pro. A candidate: a packaged layout (content from the game's files), 2D or 2D array, single-sampled, not linear, not in a
// sparse virtual pool (those follow the game's mappings: sparse_base), three levels or more, at least 512 texels on its long side and 256 on its short one, and a
// block-compressed format (block width 2 or more). A texture the game copies from, or copies into at its finest level, goes whole for good (vksoft_set, keyed by its storage:
// the game copies source textures into its terrain arrays, and with that copy skipped Mario Kart World's road was purple)
#ifndef VKA_SOFT_H
#define VKA_SOFT_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t w, h, levels, samples, block_w;
    int has_layout, has_mem, is_2d, sparse_pool, linear;   // is_2d: target 2D or 2D array
} vksoft_tex;
static inline int vksoft_candidate(const vksoft_tex *t) {
    uint32_t lo = t->w < t->h ? t->w : t->h, hi = t->w < t->h ? t->h : t->w;
    return t->has_layout && t->has_mem && t->is_2d && !t->sparse_pool && !t->linear && t->samples <= 1 && t->levels >= 3 && hi >= 512 && lo >= 256 && t->block_w >= 2;
}
// Texture level lv as a level of an image that starts at texture level base and has n levels: 0 when the image does not hold it (a soft texture's level 0, a sparse
// texture's unmapped levels), else *lv is the image's level
static inline int vksoft_lv(uint32_t base, uint32_t n, uint32_t *lv) { if (*lv < base) return 0; *lv -= base; return *lv < n; }
// A view's texture levels [l0, l0 + nl) as the image's: a view that starts at a left-out level starts at the image's level 0 (the finest data there is), with the left-out
// levels cut off its count (at least one level is left)
static inline void vksoft_view(uint32_t base, uint32_t *l0, uint32_t *nl) {
    if (*l0 < base) { uint32_t cut = base - *l0; *nl = *nl > cut ? *nl - cut : 1; *l0 = 0; } else *l0 -= base;
}
// The bytes a soft texture leaves out: its level 0 in every layer (NVN storage bytes, which the image's size is close to). level1: where level 1 starts in a layer
static inline uint64_t vksoft_saved(uint64_t level1, uint32_t layers) { return level1 * (layers ? layers : 1); }

// The storage addresses of textures that go whole (a set; 0 is never a key)
typedef struct { uint64_t *k; size_t cap, n; } vksoft_set;
static inline size_t vksoft_slot(uint64_t k, size_t cap) { return (size_t)((k * 0x9E3779B97F4A7C15ull) >> 17) & (cap - 1); }
static inline int vksoft_has(const vksoft_set *s, uint64_t k) {
    if (!s->cap || !k) return 0;
    for (size_t i = vksoft_slot(k, s->cap);; i = (i + 1) & (s->cap - 1)) { if (s->k[i] == k) return 1; if (!s->k[i]) return 0; }
}
static inline int vksoft_add(vksoft_set *s, uint64_t k) {   // 1 = new, 0 = there already (or no room: k 0, no memory)
    if (!k || vksoft_has(s, k)) return 0;
    if ((s->n + 1) * 2 > s->cap) {
        size_t oc = s->cap, nc = oc ? oc * 2 : 256; uint64_t *o = s->k, *nk = (uint64_t *)calloc(nc, sizeof *nk);
        if (!nk) return 0;
        s->k = nk; s->cap = nc; s->n = 0;
        for (size_t i = 0; i < oc; i++) if (o[i]) { size_t j = vksoft_slot(o[i], nc); while (nk[j]) j = (j + 1) & (nc - 1); nk[j] = o[i]; s->n++; }
        free(o);
    }
    size_t i = vksoft_slot(k, s->cap); while (s->k[i]) i = (i + 1) & (s->cap - 1);
    s->k[i] = k; s->n++;
    return 1;
}
#endif
