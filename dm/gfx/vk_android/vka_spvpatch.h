// SPIR-V rewrites made by the replay at pipeline time (vka_spvpatch.c). Plain C, no Vulkan
#pragma once
#include <stddef.h>
#include <stdint.h>
// The vertex module w (nw words) with the vec4 float Input variables at the locations of umask retyped as uvec4 and those of smask as ivec4, every load of them bit-cast back
// to float: the shader then sees the integer register bits Maxwell's fetch gives an integer format. A malloc'd copy (*out_nw words), or NULL with the reason in err
uint32_t *vka_spv_vin_retype(const uint32_t *w, size_t nw, uint32_t umask, uint32_t smask, size_t *out_nw, char *err, size_t errn);
// Subgroup operations and built-ins rewritten as subgroups of one invocation, for a stage the device runs none in (vka_spvpatch.c). A malloc'd copy (*out_nw words,
// *changed = 1), NULL with err empty when the module has none, NULL with the reason in err when it cannot be rewritten
uint32_t *vka_spv_subgroup_single(const uint32_t *w, size_t nw, size_t *out_nw, int *changed, char *err, size_t errn);
// A geometry shader's memory writes, counted (the return; -1 with err on failure) and, with out and a count above 0, the module with each of them made only by the run of
// the shader that first claims its primitive in the storage buffer at set 0 binding (zeroed for each draw, a word per primitive), malloc'd (*out_nw words): vka_spvpatch.c
int vka_spv_gs_claim(const uint32_t *w, size_t nw, uint32_t binding, uint32_t **out, size_t *out_nw, char *err, size_t errn);
// A pixel shader's float colour outputs at the locations of umask / smask as unsigned / signed integers of their bits (each store bit-cast), for integer attachments: malloc'd
// (*out_nw words), NULL with the reason in err (vka_spvpatch.c)
uint32_t *vka_spv_fout_retype(const uint32_t *w, size_t nw, uint32_t umask, uint32_t smask, size_t *out_nw, char *err, size_t errn);
// A compute-like module's specialization constants made plain constants, SpecId spec_id taking value, when it reads a subgroup lane built-in (Qualcomm's driver reads the
// lane as 0 in such a module that has any specialization constant): malloc'd (*out_nw words), NULL when there is nothing to do or it cannot (vka_spvpatch.c)
uint32_t *vka_spv_spec_freeze(const uint32_t *w, size_t nw, uint32_t spec_id, uint32_t value, size_t *out_nw);
// The module's float storage images of unknown format at set 0 bindings base + n (n in umask / smask) as images of uint / int sampled type, each read and written texel
// bit-cast, for integer textures bound to them (a formatted surface access moves register bits the texture's format reads): malloc'd (*out_nw words), NULL with the reason in err
uint32_t *vka_spv_img_retype(const uint32_t *w, size_t nw, uint32_t base, uint64_t umask, uint64_t smask, size_t *out_nw, char *err, size_t errn);
// An instanced variant of a vertex or pixel shader: reads of the uniform block at `binding` moved by a per-instance offset (a uint vertex input at `loc`, passed flat to
// the pixel shader), malloc'd (*out_nw words), NULL with the reason in err (vka_spvpatch.c)
uint32_t *vka_spv_instance(const uint32_t *w, size_t nw, uint32_t binding, uint32_t loc, size_t *out_nw, char *err, size_t errn);
// A geometry shader with outputs added for the pixel shader's inputs it does not write, each copied from the vertex shader's output at the same location for the vertex
// emitted (or null when the vertex shader has none): the passthrough copies a shader translated alone lacks (vka_spvpatch.c). A malloc'd copy (*out_nw words; *copied /
// *nulled the locations added); NULL with err empty when the pixel shader reads nothing more, NULL with the reason in err when it cannot be made
uint32_t *vka_spv_gs_link(const uint32_t *gw, size_t gn, const uint32_t *vw, size_t vn, const uint32_t *fw, size_t fn, uint32_t *copied, uint32_t *nulled, size_t *out_nw, char *err, size_t errn);
// A pixel shader's colour outputs: the components each location's stores can write (bit c of comp[loc]), for the colour write mask: 0, or -1 when the module is not read
int vka_spv_fout_comps(const uint32_t *w, size_t nw, uint8_t comp[8]);
// Texture-use feedback for the texture pool (vka_spvpatch.c vka_spv_pool_fb): before each read of the pool (a one-index access chain into the sampled-image array at set
// `set` binding `tex_binding`) the module sets bit (id & 31) of word id >> 5 of a storage buffer at set `fb_set` binding `fb_binding`, id the index's low 20 bits: an atomic
// OR when a load finds the bit clear. flags: VKA_PFB_VTX_STORES (the device has vertexPipelineStoresAndAtomics: vertex, tessellation and geometry modules may take it),
// VKA_PFB_EARLY (a pixel shader whose tests can give the same result early gets EarlyFragmentTests: its stores would otherwise move its depth test after shading).
// alpha_spec: the SpecId of the alpha test's function (DMSH_VK_SPEC_ALPHA_FUNC). A malloc'd copy (*out_nw words; *sites the reads given code; *early 0 no mode added,
// 1 added and right for every pipeline without alpha to coverage, 2 added and right only with the alpha test off too); NULL with err empty when the module reads no pool,
// NULL with the reason in err when it cannot take the code (the module is then used as it is: its pool reads report nothing)
enum { VKA_PFB_VTX_STORES = 1, VKA_PFB_EARLY = 2 };
uint32_t *vka_spv_pool_fb(const uint32_t *w, size_t nw, uint32_t set, uint32_t tex_binding, uint32_t fb_set, uint32_t fb_binding, unsigned flags, uint32_t alpha_spec, size_t *out_nw, int *sites, int *early, char *err, size_t errn);
