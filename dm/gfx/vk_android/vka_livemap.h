// The texture cache's live images (vka_tex.c "Live images"): the list, each image's place in it, and chains by storage address, so that a new texture finds the images of
// its storage (same_shape, gpu_image_over) and a released image leaves the list without a walk over every live image. Before (until 2026-11-10) both walked the whole list:
// O(new textures x live images) on the replay thread, 750-1,100 new textures in one frame beside 2,600 live images in Mario Kart World's race starts.
// The includer defines VKA_LIVE_T as a struct type with the fields `const uint8_t *mem` (the storage address: never changes while the image lives; NULL = none, in no chain),
// `int live_ix` (its place in the list + 1; 0 = not in it) and `VKA_LIVE_T *mnext` (the next one in its address's chain). Not thread-safe: the includer's lock (g_tmu).
// Tested by dm/host/tests/t_android_livemap.c
#ifndef VKA_LIVEMAP_H
#define VKA_LIVEMAP_H
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct { VKA_LIVE_T **b; size_t cap; } vkl_index;   // chain heads, a power of two at least twice the live images; cap 0: no table (every lookup walks the list)

static size_t vkl_slot(const uint8_t *m, size_t cap) { return (size_t)(((uint64_t)(uintptr_t)m * 0x9E3779B97F4A7C15ull) >> 29) & (cap - 1); }
static void vkl_link(vkl_index *x, VKA_LIVE_T *im) {   // at its chain's end: images of one address stay in the order they came
    VKA_LIVE_T **pp = &x->b[vkl_slot(im->mem, x->cap)];
    while (*pp) pp = &(*pp)->mnext;
    im->mnext = NULL; *pp = im;
}
// im into the list (*v, *n, *cap) and its chain. 0: the list could not grow (im is not live)
static int vkl_add(VKA_LIVE_T ***v, int *n, int *cap, vkl_index *x, VKA_LIVE_T *im) {
    if (*n == *cap) {
        int nc = *cap ? *cap * 2 : 256; VKA_LIVE_T **nv = realloc(*v, (size_t)nc * sizeof *nv);
        if (!nv) return 0;
        *v = nv; *cap = nc;
    }
    (*v)[(*n)++] = im; im->live_ix = *n; im->mnext = NULL;
    if (!im->mem) return 1;
    if ((size_t)*n * 2 > x->cap) {   // grown: every live image linked again (the new one with them)
        size_t c = x->cap ? x->cap : 1024; while ((size_t)*n * 2 > c) c *= 2;
        VKA_LIVE_T **nb = calloc(c, sizeof *nb);
        if (nb) { free(x->b); x->b = nb; x->cap = c; for (int i = 0; i < *n; i++) if ((*v)[i]->mem) vkl_link(x, (*v)[i]); return 1; }
        if (!x->cap) return 1;   // (no table and none could be made: lookups walk the list)
    }
    vkl_link(x, im);
    return 1;
}
// im out of the list (the last one takes its place) and out of its chain. 0: it was not live
static int vkl_del(VKA_LIVE_T **v, int *n, vkl_index *x, VKA_LIVE_T *im) {
    int i = im->live_ix - 1;
    if (i < 0 || i >= *n || v[i] != im) return 0;
    v[i] = v[--*n]; v[i]->live_ix = i + 1; im->live_ix = 0;
    if (im->mem && x->cap)
        for (VKA_LIVE_T **pp = &x->b[vkl_slot(im->mem, x->cap)]; *pp; pp = &(*pp)->mnext) if (*pp == im) { *pp = im->mnext; break; }
    im->mnext = NULL;
    return 1;
}
// The live images whose storage starts at mem: the first, then vkl_next (k: the walk's place without a table, where every live image is offered and the caller compares mem)
static VKA_LIVE_T *vkl_first(VKA_LIVE_T *const *v, int n, const vkl_index *x, const uint8_t *mem, int *k) {
    *k = 0;
    if (!x->cap) return n ? v[0] : NULL;
    VKA_LIVE_T *c = x->b[vkl_slot(mem, x->cap)];
    while (c && c->mem != mem) c = c->mnext;
    return c;
}
static VKA_LIVE_T *vkl_next(VKA_LIVE_T *const *v, int n, const vkl_index *x, const VKA_LIVE_T *im, const uint8_t *mem, int *k) {
    if (!x->cap) return ++*k < n ? v[*k] : NULL;
    VKA_LIVE_T *c = im->mnext;
    while (c && c->mem != mem) c = c->mnext;
    return c;
}
#endif
