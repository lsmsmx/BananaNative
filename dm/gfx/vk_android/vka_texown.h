// Which texture object owns a serial: the one nvnTextureInitialize ran on (vka_tex.c mtl_tex_init / mtl_tex_release), free of Vulkan so the Mac's unit test
// (dm/host/tests/t_android_texown.c) can check it.
// NVNtexture is plain data, and engines copy it by value and finalize the copy while the original stays in use (Ocarina of Time every few frames, Breath of the Wild about
// 100 a frame, found upstream: nxnative 6121fd75, docs/reference/nvn-objects.md "Finalizing a copy of a texture object"). The copy carries the original's serial, and the
// replay keys its images by serial, so the copy's Finalize let go of the original's image while the original still drew with it: a CPU-filled texture was made and uploaded
// again at its next use, and one the GPU had drawn (a render target, a terrain tile's normal map) came back as zeros or as the stale bytes of its storage.
// A Finalize, or a re-Initialize of a live object, ends the texture unless another object is its owner and still holds the texture's magic and serial (read without faulting:
// the game may have freed and unmapped that object). An owner the game moved away (its old bytes overwritten) no longer holds them, so the move's Finalize ends it.
#ifndef VKA_TEXOWN_H
#define VKA_TEXOWN_H
#include <stdint.h>
#include "../nvn/nvn_hmap.h"

typedef int (*vto_holds_fn)(uintptr_t obj, uint64_t serial);   // whether the object at obj still holds the texture magic and this serial
static inline void vto_init(nvn_hmap *m) { if (!m->klen) nvn_hm_init(m, 8); }
// nvnTextureInitialize on obj gave it serial
static inline void vto_own(nvn_hmap *m, uint64_t serial, const void *obj) { if (!serial || !obj) return; vto_init(m); nvn_hm_put(m, &serial, (void *)obj); }
// A Finalize (or a re-Initialize) of obj for serial: 1 = it ends the texture (the owner's entry is dropped), 0 = obj is a copy and the owner still holds the texture
static inline int vto_release(nvn_hmap *m, uint64_t serial, const void *obj, vto_holds_fn holds) {
    if (!serial || !m->klen) return 1;
    const void *own = nvn_hm_get(m, &serial);
    if (own && own != obj && holds((uintptr_t)own, serial)) return 0;
    nvn_hm_del(m, &serial);
    return 1;
}
#endif
