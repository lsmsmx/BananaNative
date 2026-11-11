// Zero-shared image memory (vka_imgmem.c vka_imem_bind_zero, vka_tex.c img_own): the bookkeeping, free of Vulkan so the Mac's unit test (dm/host/tests/t_android_zshare.c)
// can check it. A large image whose content is the GPU's (ST_ZERO) and that nothing has written yet holds zeros; images of one create info (the key: flags with
// VK_IMAGE_CREATE_ALIAS_BIT, format, extent, levels, layers, samples, usage, format list, memory type, size, alignment) are bound at offset 0 of one zeroed allocation,
// which Vulkan's aliasing rules let them read alike. An entry is that allocation (handle, size) and the images bound to it (refs); the allocation is freed with its last image
#ifndef VKA_ZSHARE_H
#define VKA_ZSHARE_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { VKZ_KEY = 128 };
typedef struct { uint8_t key[VKZ_KEY]; uint32_t klen; uint64_t h, size; int refs; } vkz_ent;
typedef struct { vkz_ent *e; int n, cap; uint64_t made, owned; } vkz_tab;   // made: allocations ever made; owned: images that left their entry for memory of their own (vkz_note_own)

// An image is a candidate when it is dense (no residency image), single-sampled, not 3D, not block-compressed, not linear and not a display (window) texture, and its storage
// takes at least min_bytes and at most max_bytes (0: no upper limit). Whether its content is the GPU's (ST_ZERO) is the caller's: only those are bound to shared memory.
// The upper limit (vkz_max): upstream's a319e773 capped every image at 32 MiB against a giant depth duplication on Ocarina of Time, which also left out Mario Kart World's
// 341 MiB color images that the game samples and never writes (1.8.0: two 16384² R8 and four 8192² RG16 ones, about 2 GB with memory of their own: killed on a 12 GB
// device). So a depth or stencil image keeps the cap, and a color image has none on Turnip, where sharing them is measured (2.2 GB saved, a full race). On Qualcomm's driver
// (V@0863, Adreno 740) the GPU hung during loading with the four 16384² R8 images shared, 3 runs of 3, and ran with the cap: other drivers keep it (DM_VK_ZSHARE_MAX_MB)
#define VKZ_CAP (32ull * 1024 * 1024)
static inline uint64_t vkz_max(int depth, int big_color_ok, int64_t env_mb) {   // env_mb: DM_VK_ZSHARE_MAX_MB for color images (< 0: not set; 0: no limit)
    if (depth) return VKZ_CAP;
    if (env_mb >= 0) return (uint64_t)env_mb << 20;
    return big_color_ok ? 0 : VKZ_CAP;
}
static inline int vkz_candidate(uint64_t storage_bytes, uint64_t min_bytes, uint64_t max_bytes, int res, uint32_t samples, int is3d, int compressed, int linear_or_display) {
    return min_bytes && storage_bytes >= min_bytes && (!max_bytes || storage_bytes <= max_bytes) && !res && samples <= 1 && !is3d && !compressed && !linear_or_display;
}
static inline int vkz_find(const vkz_tab *t, const void *key, uint32_t klen) {
    for (int i = 0; i < t->n; i++) if (t->e[i].klen == klen && !memcmp(t->e[i].key, key, klen)) return i;
    return -1;
}
static inline int vkz_find_h(const vkz_tab *t, uint64_t h) { for (int i = 0; i < t->n; i++) if (t->e[i].h == h) return i; return -1; }
// One more image of key: the entry's index (refs counted), or -1 when there is none (the caller allocates and calls vkz_add)
static inline int vkz_ref(vkz_tab *t, const void *key, uint32_t klen) { int i = klen <= VKZ_KEY ? vkz_find(t, key, klen) : -1; if (i >= 0) t->e[i].refs++; return i; }
// A new allocation h of size bytes for key, with its first image: the index, or -1 (no room; the caller frees h and binds the image as before)
static inline int vkz_add(vkz_tab *t, const void *key, uint32_t klen, uint64_t h, uint64_t size) {
    if (klen > VKZ_KEY || !h) return -1;
    if (t->n == t->cap) { int c = t->cap ? 2 * t->cap : 16; vkz_ent *e = (vkz_ent *)realloc(t->e, (size_t)c * sizeof *e); if (!e) return -1; t->e = e; t->cap = c; }
    vkz_ent *e = &t->e[t->n]; memset(e, 0, sizeof *e); memcpy(e->key, key, klen); e->klen = klen; e->h = h; e->size = size; e->refs = 1; t->made++;
    return t->n++;
}
// An image of allocation h let go: 1 when it was the last (the entry is gone; the caller frees h), 0 when others remain, -1 when h is no entry's
static inline int vkz_unref(vkz_tab *t, uint64_t h) {
    int i = vkz_find_h(t, h); if (i < 0) return -1;
    if (--t->e[i].refs > 0) return 0;
    t->e[i] = t->e[--t->n];
    return 1;
}
static inline void vkz_note_own(vkz_tab *t) { t->owned++; }
// The table now: images bound, allocations, their bytes, and the bytes they save (each image beyond an allocation's first would have had its own)
static inline void vkz_stats(const vkz_tab *t, uint64_t *images, uint64_t *allocs, uint64_t *bytes, uint64_t *saved) {
    uint64_t im = 0, by = 0, sv = 0;
    for (int i = 0; i < t->n; i++) { im += (uint64_t)t->e[i].refs; by += t->e[i].size; sv += (uint64_t)(t->e[i].refs - 1) * t->e[i].size; }
    if (images) *images = im; if (allocs) *allocs = (uint64_t)t->n; if (bytes) *bytes = by; if (saved) *saved = sv;
}
#endif
