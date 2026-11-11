// The binding walker of the Vulkan replay: what each of a shader's binding records (dmsh_vk_bind, android/SHADERS.md "Descriptor ABI") is filled from, given the NVN state a draw
// or dispatch sees. It is nvn_mtl.m's encode_args without Metal: the same slots, handles, pool bases and driver segments. Plain C with no Vulkan (the Mac unit test runs it on a
// real library); vka_draw.c turns its answers into descriptors
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../nvn/dmsh_abi.h"
#include "../nvn/dmsh_abi_vk.h"

#define VKA_NTEX (DMSH_SEPTEX_SLOT0 + DMSH_C2_SEP_SLOTS)   // texture slots of a stage: combined 0-127, separate 128-255 (nvn_obj.h NVN_SEPTEX_SLOT0)
#define VKA_NSTAGE 8                                         // NVN stages: 0 vertex, 1 fragment, 2 geometry, 3 tess control, 4 tess evaluation, 5 compute, 6 task, 7 mesh (NVN2)

typedef struct { uint64_t addr, size; } vka_range;
// What the draw path keeps of the NVN bindings, by NVN stage (the stage field of the bind records, as hle_nvn.c records them)
typedef struct {
    vka_range ubo[VKA_NSTAGE][16], ssbo[VKA_NSTAGE][16];
    uint64_t tex[VKA_NSTAGE][VKA_NTEX], samp[VKA_NSTAGE][128], img[VKA_NSTAGE][32];   // handles as bound: texture ID | sampler ID << 20, the pool bases already added (hle_nvn.c bind_handles)
    uint8_t push[2][DMSH_PUSH_SIZE];   // NVN2 pushed constants: graphics, compute
    int tex_base, samp_base;           // the bound pools' bases, for handles read from memory at replay (nvn_mtl.m ubo_tex_id)
    uint32_t base_instance;            // the draw's base instance (c0 + DMSH_C0_BASE_INSTANCE)
    uint32_t dispatch[3];              // the dispatch's group counts (c0 + DMSH_C0_DISPATCH, and with nvn2 also c0 + DMSH_C0_DISPATCH_NVN2)
    uint32_t nvn2;                     // a Switch 2 game (NVN2): its compute reads the group counts at c0 + DMSH_C0_DISPATCH_NVN2. Set by the replay (vka_nvn2), not here: this file stays host-free
    uint32_t samples;                  // the pass's sample count (the pixel stage's sample table)
} vka_bstate;

// NVN stage ↔ ABI stage (DMSH_VK_STAGE_*: vertex 0, tess control 1, tess evaluation 2, geometry 3, fragment 4, compute 5, task 6, mesh 7). NVN2's task and mesh stages are 6 and 7
// in both numberings (nvnCommandBufferDrawMeshTasks; switch2mac nvn_mtl.m NVN_NSTAGE)
static inline int vka_vk_stage(int nvn) { static const int8_t m[8] = {DMSH_VK_STAGE_VS, DMSH_VK_STAGE_FS, DMSH_VK_STAGE_GS, DMSH_VK_STAGE_TCS, DMSH_VK_STAGE_TES, DMSH_VK_STAGE_CS, DMSH_VK_STAGE_TS, DMSH_VK_STAGE_MS}; return nvn >= 0 && nvn < 8 ? m[nvn] : -1; }
static inline int vka_nvn_stage(int vks) { static const int8_t m[8] = {0, 3, 4, 2, 1, 5, 6, 7}; return vks >= 0 && vks < 8 ? m[vks] : -1; }

enum {
    VKA_R_NULL,       // nothing bound there: a null descriptor (or a placeholder of the record's dim)
    VKA_R_BUF,        // guest memory [addr, addr + size) (size 0: to the end of its placed chunk); desc says uniform or storage
    VKA_R_C1,         // the shader's own c1 bytes
    VKA_R_SUPPORT,    // Ryujinx's support buffer
    VKA_R_DRV,        // a driver segment the replay fills (bank 0, 2 or DMSH_PUSH_BANK: vka_drv_fill)
    VKA_R_TEX,        // texture ID tid (0 = none) with sampler ID sid (0 = none), read through a view of shape dim
    VKA_R_IMG,        // image of texture ID tid, storage
    VKA_R_TBUF,       // buffer texture of texture ID tid
    VKA_R_GMEM_MAP,   // the guest → device address table of raw global memory
    VKA_R_GMEM_LOG,   // that memory's write log, cleared per draw or dispatch
    VKA_R_SHARED_SPILL,   // a compute program's shared memory past the device's limit (DMSH_VK_KIND_SHARED_SPILL): size = bytes of one workgroup's slice
};
typedef struct {
    uint8_t what, desc, flags, dim;    // VKA_R_*, VkDescriptorType, DMSH_VK_BF_*, DMSH_VK_DIM_*
    uint16_t binding, elem;            // set 0 binding and array element
    int16_t bank;                      // VKA_R_DRV
    int32_t tid, sid;
    uint64_t addr, size;
} vka_res;

// Reads guest memory for handles kept in uniform buffers (TEX_UBO, SAMP_UBO, SBUF_UBO): 1 = read, 0 = not readable (the value reads as 0)
typedef int (*vka_read_fn)(void *ctx, uint64_t addr, void *out, size_t n);

#define VKA_WALK_REFUSE (-1)
// An array of handles in a uniform buffer (TEX_UBO / SAMP_UBO, count > 1; BindlessToArray): element k is the handle at the array's base + 8k, as nvn_mtl.m fills it.
// 1 (default) = so; 0 = every element from the base (the old walk; DM_VK_TEXU_ELEM=0, live, for A/B). Set by the replay (vka_draw.c walk_all)
extern int vka_texu_elem;
// Walks one stage's binding records (e->bind_count of them from b) for NVN stage nvn_stage. Writes at most max results to out and returns their count, or VKA_WALK_REFUSE with
// *why set when the draw cannot run correctly. Storage buffers of global memory (SBUF, SBUF_UBO, UBO_PTR): with the library's ssbo_align (header; DMSH_VK_SSBO_ALIGN) they are
// bound at their address rounded down to it, the range grown by the difference (the shader adds it back); with 0 (an older library) at the exact address, and one off the
// device's dev_align refuses the draw. An UNKNOWN record refuses it too. *uses_pool is set when the shader reads set 1 (the texture or sampler pool)
int vka_walk(const vka_bstate *s, int nvn_stage, const dmsh_vk_entry *e, const dmsh_vk_bind *b, uint32_t ssbo_align, uint64_t dev_align, vka_read_fn rd, void *rd_ctx,
             vka_res *out, int max, int *uses_pool, char *why, size_t whyn);
// Fills a driver segment as the GPU's driver would (nvn_mtl.m encode_args NVN_RB_DRV0 / DRV2 / PUSH): DMSH_DRV_SIZE bytes for banks 0 and 2, DMSH_PUSH_SIZE for the push bank.
// pool: the shader reads the pools (c2 then holds the handles without the pools' bases, as the game wrote them)
void vka_drv_fill(const vka_bstate *s, int nvn_stage, int bank, int pool, uint8_t *d);
size_t vka_drv_size(int bank);
// The handle the driver's c2 holds at byte off for a stage (nvn_mtl.m c2_handle), and a combined texture slot's handle (tex_slot_h: past slot 31 an image slot shares the word)
uint64_t vka_c2_handle(const vka_bstate *s, int nvn_stage, unsigned off);
uint64_t vka_tex_slot(const vka_bstate *s, int nvn_stage, unsigned reg);
// Ryujinx's support buffer (SupportBuffer.cs: 1,392 bytes): word 0 the alpha test reference (float bits, DMSH_VK_SPEC_ALPHA_FUNC), zeros, render_scale floats at 192 + 16 i set to 1.0
#define VKA_SUPPORT_SIZE 2048
// The support buffer's contents; n bytes
void vka_support_fill(uint8_t *d, size_t n);
