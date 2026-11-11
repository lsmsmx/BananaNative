// SPIR-V rewrites the replay makes at pipeline time (plain C, no Vulkan: unit-tested on the Mac by dm/host/tests/t_android_spvpatch.c).
//
// vka_spv_vin_retype: vertex inputs fed integer formats. The translator declares every vertex input a vec4 of floats (the library's vin_types say float for all of
// Donkey Kong Bananza's 65,846 inputs) and a shader that uses one as integer bits bit-casts it. On Maxwell an attribute of an integer format (UINT8 … SINT32) puts the
// integer, widened by its own sign, into the 32-bit register the shader reads, so those bits are what the bit-cast must see (bone indices, packed data). Vulkan gives a float
// input only a float, normalized or scaled format, and a USCALED stand-in hands the shader the bits of 5.0 instead of 5. So for a pipeline whose integer attributes feed float
// inputs, the vertex module is rewritten: those inputs become uvec4 / ivec4 (Input variables of the integer vector type), and each load of one, or of a component through an
// access chain, is followed by an OpBitcast back to the float type the body expects. The bits are then Maxwell's, and the attribute keeps its own integer format.
// nvn_mtl.m reaches the same bits by converting such attributes on the CPU into float32 bits (vconv_int); the Apple chain's dmsh_vin is the reverse case (integer inputs fed float formats).
#include "vka_spvpatch.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

enum { OpExtInst = 12, OpName = 5, OpMemberName = 6, OpEntryPoint = 15, OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23, OpTypePointer = 32, OpFunction = 54,
       OpVariable = 59, OpLoad = 61, OpAccessChain = 65, OpInBoundsAccessChain = 66, OpDecorate = 71, OpMemberDecorate = 72, OpBitcast = 124,
       OpDecorateId = 332, OpDecorateString = 5632, OpMemberDecorateString = 5633 };
enum { DecLocation = 30, SCInput = 1 };
enum { OpConstantFalse = 42, OpSpecConstantTrue = 48, OpSpecConstantFalse = 49, OpSpecConstant = 50, OpSpecConstantComposite = 51, OpSpecConstantOp = 52, DecSpecId = 1 };

typedef struct { uint32_t *w; size_t n, cap; } vec_t;
static int put(vec_t *v, uint32_t x) {
    if (v->n == v->cap) { size_t c = v->cap ? 2 * v->cap : 4096; uint32_t *p = realloc(v->w, c * 4); if (!p) return 0; v->w = p; v->cap = c; }
    v->w[v->n++] = x; return 1;
}
static int put_n(vec_t *v, const uint32_t *x, size_t n) { for (size_t i = 0; i < n; i++) if (!put(v, x[i])) return 0; return 1; }

uint32_t *vka_spv_vin_retype(const uint32_t *w, size_t nw, uint32_t umask, uint32_t smask, size_t *out_nw, char *err, size_t errn) {
#define FAIL(...) do { snprintf(err, errn, __VA_ARGS__); goto fail; } while (0)
    uint32_t *cls = NULL; vec_t o = {0};   // cls[id]: 0 untouched, 1 a retyped input or a chain into one, as uint, 2 as int (bit 4: the id is a pointer to a vec4)
    if (nw < 5 || w[0] != 0x07230203) { snprintf(err, errn, "not SPIR-V"); return NULL; }
    uint32_t bound = w[3];
    if (!(cls = calloc(bound, 4))) { snprintf(err, errn, "out of memory"); return NULL; }
    if ((umask | smask) == 0 || (umask & smask)) FAIL("no input, or an input both uint and int");
    // Pass 1: locations, the types, the Input variables
    uint32_t *loc = calloc(bound, 4); if (!loc) FAIL("out of memory");
    for (uint32_t i = 0; i < bound; i++) loc[i] = ~0u;
    uint32_t t_f32 = 0, t_u32 = 0, t_i32 = 0, t_v4f = 0, t_v4u = 0, t_v4i = 0, p_in_v4f = 0;
    uint32_t p_in[2][2] = {{0}};   // [uint/int][scalar/vec4] Input pointers that exist
    size_t first_fn = nw;
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > nw) { free(loc); FAIL("bad instruction at word %zu", i); }
        if (op == OpDecorate && len >= 4 && w[i + 2] == DecLocation && w[i + 1] < bound) loc[w[i + 1]] = w[i + 3];
        else if (op == OpTypeFloat && len >= 3 && w[i + 2] == 32) t_f32 = w[i + 1];
        else if (op == OpTypeInt && len >= 4 && w[i + 2] == 32) { if (w[i + 3]) t_i32 = w[i + 1]; else t_u32 = w[i + 1]; }
        else if (op == OpTypeVector && len >= 4 && w[i + 3] == 4) {
            if (w[i + 2] == t_f32 && t_f32) t_v4f = w[i + 1]; else if (w[i + 2] == t_u32 && t_u32) t_v4u = w[i + 1]; else if (w[i + 2] == t_i32 && t_i32) t_v4i = w[i + 1]; }
        else if (op == OpTypePointer && len >= 4 && w[i + 2] == SCInput) {
            uint32_t t = w[i + 3];
            if (t == t_v4f && t) p_in_v4f = w[i + 1];
            for (int c = 0; c < 2; c++) { if (t == (c ? t_i32 : t_u32) && t) p_in[c][0] = w[i + 1]; if (t == (c ? t_v4i : t_v4u) && t) p_in[c][1] = w[i + 1]; }
        }
        else if (op == OpFunction && first_fn == nw) first_fn = i;
        i += len;
    }
    if (!t_f32 || !t_v4f || !p_in_v4f) { free(loc); FAIL("no vec4 float input type"); }
    // New ids for the types the module lacks
    uint32_t nb = bound;
    int need_c[2] = {(umask != 0), (smask != 0)};
    uint32_t t_s[2] = {t_u32, t_i32}, t_v[2] = {t_v4u, t_v4i};
    uint32_t mk_s[2] = {0}, mk_v[2] = {0}, mk_p[2][2] = {{0}};
    for (int c = 0; c < 2; c++) if (need_c[c]) {
        if (!t_s[c]) t_s[c] = mk_s[c] = nb++;
        if (!t_v[c]) t_v[c] = mk_v[c] = nb++;
        if (!p_in[c][0]) p_in[c][0] = mk_p[c][0] = nb++;
        if (!p_in[c][1]) p_in[c][1] = mk_p[c][1] = nb++;
    }
    // The input variables to retype: Location in a mask, type pointer-to-vec4-float
    uint32_t moved[32]; int nmoved = 0, found = 0;
    for (size_t i = 5; i < first_fn;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (op == OpVariable && len >= 4 && w[i + 3] == SCInput && w[i + 2] < bound && w[i + 1] == p_in_v4f) {
            uint32_t id = w[i + 2], l = loc[id];
            if (l < 32 && ((umask | smask) >> l & 1)) { cls[id] = (umask >> l & 1 ? 1 : 2) | 16; if (nmoved < 32) moved[nmoved++] = id; found |= 1u << l; }
        }
        i += len;
    }
    free(loc);
    if ((uint32_t)found != (umask | smask)) FAIL("inputs %#x are not vec4 float Input variables", (umask | smask) & ~(uint32_t)found);
    // Pass 2: copy, retyping. The globals up to the first function, minus the moved variables; then the new types and the variables; then the functions
    if (!put_n(&o, w, 5)) FAIL("out of memory");
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16; const uint32_t *in = w + i;
        if (i == first_fn) {
            for (int c = 0; c < 2; c++) if (need_c[c]) {
                if (mk_s[c]) { uint32_t x[4] = {4u << 16 | OpTypeInt, mk_s[c], 32, (uint32_t)c}; if (!put_n(&o, x, 4)) FAIL("out of memory"); }
                if (mk_v[c]) { uint32_t x[4] = {4u << 16 | OpTypeVector, mk_v[c], t_s[c], 4}; if (!put_n(&o, x, 4)) FAIL("out of memory"); }
                if (mk_p[c][0]) { uint32_t x[4] = {4u << 16 | OpTypePointer, mk_p[c][0], SCInput, t_s[c]}; if (!put_n(&o, x, 4)) FAIL("out of memory"); }
                if (mk_p[c][1]) { uint32_t x[4] = {4u << 16 | OpTypePointer, mk_p[c][1], SCInput, t_v[c]}; if (!put_n(&o, x, 4)) FAIL("out of memory"); }
            }
            for (int k = 0; k < nmoved; k++) { uint32_t x[4] = {4u << 16 | OpVariable, p_in[(cls[moved[k]] & 3) - 1][1], moved[k], SCInput}; if (!put_n(&o, x, 4)) FAIL("out of memory"); }
        }
        if (op == OpVariable && len >= 4 && w[i + 2] < bound && cls[w[i + 2]] && i < first_fn) { i += len; continue; }   // re-emitted above
        if ((op == OpAccessChain || op == OpInBoundsAccessChain) && len >= 4 && w[i + 3] < bound && cls[w[i + 3]]) {
            int c = (cls[w[i + 3]] & 3) - 1; uint32_t rt = w[i + 1], r = w[i + 2];
            // The chain's result: a component (pointer to float) or the vector itself (no index)
            if (r >= bound) FAIL("bad id");
            int vec = len == 4;
            if (!vec && len != 5) FAIL("an access chain of %u indexes into a retyped input", len - 4);
            (void)rt;
            cls[r] = (uint32_t)(c + 1) | (vec ? 16 : 0);
            if (!put_n(&o, in, len)) FAIL("out of memory");
            o.w[o.n - len + 1] = p_in[c][vec];
            i += len; continue;
        }
        if (op == OpLoad && len >= 4 && w[i + 3] < bound && cls[w[i + 3]]) {
            int c = (cls[w[i + 3]] & 3) - 1, vec = (cls[w[i + 3]] & 16) != 0;
            uint32_t ft = w[i + 1], r = w[i + 2], tmp = nb++;
            if (!put_n(&o, in, len)) FAIL("out of memory");
            o.w[o.n - len + 1] = vec ? t_v[c] : t_s[c]; o.w[o.n - len + 2] = tmp;
            uint32_t x[4] = {4u << 16 | OpBitcast, ft, r, tmp}; if (!put_n(&o, x, 4)) FAIL("out of memory");
            i += len; continue;
        }
        // Any other use of a retyped variable or chain (a call argument, a copy): not understood, refused. Debug names, decorations and the entry point's interface are fine
        if (op != OpName && op != OpMemberName && op != OpEntryPoint && op != OpDecorate && op != OpMemberDecorate && op != OpDecorateId && op != OpDecorateString && op != OpMemberDecorateString
            && i >= first_fn)
            for (uint32_t k = 1; k < len; k++) if (!(op == OpExtInst && k == 4) && w[i + k] < bound && cls[w[i + k]] && op != OpVariable) FAIL("opcode %u uses a retyped input (id %u)", op, w[i + k]);
        if (!put_n(&o, in, len)) FAIL("out of memory");
        i += len;
    }
    o.w[3] = nb;
    free(cls);
    *out_nw = o.n; return o.w;
fail:
    free(cls); free(o.w);
    return NULL;
#undef FAIL
}

// vka_spv_subgroup_single: subgroup operations in a stage the device has none for. The Adreno 840 lists subgroup operations for the fragment and compute stages only
// (VkPhysicalDeviceSubgroupProperties::supportedStages 0xf0), and the translator emits them wherever the Maxwell code votes or shuffles: in Donkey Kong Bananza four geometry
// shaders (the terrain's culling shader 5d537f7630ec3671 among them) count each warp's surviving triangles with a ballot, let one lane add them to the chunk's indirect draw
// record and hand the base to the others with a shuffle. The driver takes the module and gives no meaningful lanes, so triangles were dropped or written over each other:
// black holes in the cave walls. Rewritten as subgroups of one invocation, which every warp-level algorithm must already handle (a warp with one active lane): the
// invocation's id is 0, the subgroup size 1, a ballot is its predicate in bit 0, the equal / less-or-equal / greater-or-equal masks are bit 0, the less / greater masks 0,
// a shuffle or broadcast returns its own value, all / any return the predicate, all-equal and elect true. Ballot counts, finds and inverse ballots are refused (none met)
enum { OpTypeBool = 20, OpConstantTrue = 41, OpConstant = 43, OpConstantComposite = 44, OpCompositeConstruct = 80, OpCopyObject = 83, OpSelect = 169,
       OpGNUElect = 333, OpGNUAll = 334, OpGNUAny = 335, OpGNUAllEqual = 336, OpGNUBroadcast = 337, OpGNUBroadcastFirst = 338, OpGNUBallot = 339,
       OpGNUShuffle = 345, OpGNUShuffleXor = 346, OpGNUShuffleUp = 347, OpGNUShuffleDown = 348 };
enum { DecBuiltIn = 11, BISubgroupSize = 36, BISubgroupLocalInvocationId = 41, BIEq = 4416, BIGe = 4417, BIGt = 4418, BILe = 4419, BILt = 4420 };
static int bi_kind(uint32_t b) {   // 1 = value 0 (id, lt, gt), 2 = value 1 (size, eq, le, ge), 0 = not a subgroup built-in
    switch (b) { case BISubgroupLocalInvocationId: case BILt: case BIGt: return 1; case BISubgroupSize: case BIEq: case BILe: case BIGe: return 2; default: return 0; }
}
uint32_t *vka_spv_subgroup_single(const uint32_t *w, size_t nw, size_t *out_nw, int *changed, char *err, size_t errn) {
#define FAIL(...) do { snprintf(err, errn, __VA_ARGS__); goto fail; } while (0)
    uint8_t *bi = NULL, *chain = NULL; int32_t *cval = NULL; vec_t o = {0}; *changed = 0;
    if (nw < 5 || w[0] != 0x07230203) { snprintf(err, errn, "not SPIR-V"); return NULL; }
    uint32_t bound = w[3], nb = bound;
    bi = calloc(bound, 1); chain = calloc(bound, 1); cval = malloc((size_t)bound * 4);
    if (!bi || !chain || !cval) FAIL("out of memory");
    for (uint32_t i = 0; i < bound; i++) cval[i] = -1;
    uint32_t t_u32 = 0, t_v4u = 0, t_bool = 0; size_t first_fn = nw; int any = 0;
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > nw) FAIL("bad instruction at word %zu", i);
        if (op == OpDecorate && len >= 4 && w[i + 2] == DecBuiltIn && w[i + 1] < bound && bi_kind(w[i + 3])) bi[w[i + 1]] = (uint8_t)bi_kind(w[i + 3]) | (w[i + 3] >= BIEq ? 4 : 0);   // bit 2: a vec4 mask
        else if (op == OpTypeInt && len >= 4 && w[i + 2] == 32 && !w[i + 3]) t_u32 = w[i + 1];
        else if (op == OpTypeBool && len >= 2) t_bool = w[i + 1];
        else if (op == OpTypeVector && len >= 4 && w[i + 3] == 4 && w[i + 2] == t_u32 && t_u32) t_v4u = w[i + 1];
        else if (op == OpConstant && len >= 4 && w[i + 2] < bound) cval[w[i + 2]] = (int32_t)w[i + 3];
        else if (op == OpFunction && first_fn == nw) first_fn = i;
        else if (op >= OpGNUElect && op <= OpGNUShuffleDown) any = 1;
        i += len;
    }
    for (uint32_t i = 0; i < bound && !any; i++) any = bi[i] != 0;
    if (!any) { free(bi); free(chain); free(cval); return NULL; }   // nothing to do
    if (!t_u32) FAIL("no 32-bit unsigned type");
    uint32_t mk_v4 = t_v4u ? 0 : nb++, mk_bool = t_bool ? 0 : nb++; if (!t_v4u) t_v4u = mk_v4; if (!t_bool) t_bool = mk_bool;
    uint32_t c0 = nb++, c1 = nb++, cv0 = nb++, cv1 = nb++, ctrue = nb++;   // uint 0, uint 1, uvec4(0), uvec4(1, 0, 0, 0), true
    if (!put_n(&o, w, 5)) FAIL("out of memory");
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16; const uint32_t *in = w + i;
        if (i == first_fn) {
            if (mk_bool) { uint32_t x[2] = {2u << 16 | OpTypeBool, mk_bool}; if (!put_n(&o, x, 2)) FAIL("out of memory"); }
            if (mk_v4) { uint32_t x[4] = {4u << 16 | OpTypeVector, mk_v4, t_u32, 4}; if (!put_n(&o, x, 4)) FAIL("out of memory"); }
            uint32_t x[] = {4u << 16 | OpConstant, t_u32, c0, 0, 4u << 16 | OpConstant, t_u32, c1, 1, 7u << 16 | OpConstantComposite, t_v4u, cv0, c0, c0, c0, c0,
                            7u << 16 | OpConstantComposite, t_v4u, cv1, c1, c0, c0, c0, 3u << 16 | OpConstantTrue, t_bool, ctrue};
            if (!put_n(&o, x, sizeof x / 4)) FAIL("out of memory");
        }
        if (i >= first_fn) {
            if ((op == OpAccessChain || op == OpInBoundsAccessChain) && len >= 4 && w[i + 3] < bound && bi[w[i + 3]]) {   // a component of a mask
                if (len != 5 || w[i + 4] >= bound || cval[w[i + 4]] < 0) FAIL("an access chain into a subgroup mask with a non-constant index");
                int k = bi[w[i + 3]] & 3; chain[w[i + 2]] = (uint8_t)(k == 2 && cval[w[i + 4]] == 0 ? 2 : 1);   // bit 0 only for eq / le / ge
                i += len; continue;   // dropped: its loads become constants
            }
            if (op == OpLoad && len >= 4 && w[i + 3] < bound && (bi[w[i + 3]] || chain[w[i + 3]])) {
                uint32_t v = chain[w[i + 3]] ? (chain[w[i + 3]] == 2 ? c1 : c0) : (bi[w[i + 3]] & 4) ? ((bi[w[i + 3]] & 3) == 2 ? cv1 : cv0) : ((bi[w[i + 3]] & 3) == 2 ? c1 : c0);
                uint32_t x[4] = {4u << 16 | OpCopyObject, w[i + 1], w[i + 2], v}; if (!put_n(&o, x, 4)) FAIL("out of memory");
                i += len; *changed = 1; continue;
            }
            if (op >= OpGNUElect && op <= OpGNUShuffleDown) {
                uint32_t rt = w[i + 1], r = w[i + 2], v;
                switch (op) {
                case OpGNUElect: case OpGNUAllEqual: v = ctrue; break;
                case OpGNUAll: case OpGNUAny: case OpGNUBroadcast: case OpGNUBroadcastFirst: case OpGNUShuffle: case OpGNUShuffleXor: case OpGNUShuffleUp: case OpGNUShuffleDown:
                    if (len < 5) FAIL("short subgroup instruction %u", op);
                    v = w[i + 4]; break;
                case OpGNUBallot: {
                    if (len < 5 || rt != t_v4u) FAIL("a ballot of an unexpected type");
                    uint32_t tmp = nb++;
                    uint32_t x[] = {6u << 16 | OpSelect, t_u32, tmp, w[i + 4], c1, c0, 7u << 16 | OpCompositeConstruct, t_v4u, r, tmp, c0, c0, c0};
                    if (!put_n(&o, x, sizeof x / 4)) FAIL("out of memory");
                    i += len; *changed = 1; continue; }
                default: FAIL("subgroup instruction %u not rewritten", op);
                }
                uint32_t x[4] = {4u << 16 | OpCopyObject, rt, r, v}; if (!put_n(&o, x, 4)) FAIL("out of memory");
                i += len; *changed = 1; continue;
            }
        }
        if (!put_n(&o, in, len)) FAIL("out of memory");
        i += len;
    }
    o.w[3] = nb;
    free(bi); free(chain); free(cval);
    *out_nw = o.n; return o.w;
fail:
    free(bi); free(chain); free(cval); free(o.w);
    return NULL;
#undef FAIL
}

// vka_spv_gs_claim: a geometry shader that writes memory, made to write it once per primitive. The Adreno 840 runs a geometry shader several times for each primitive (three
// times for a triangle in and three vertices out, counted with an atomic add per gl_PrimitiveIDIn), each run executing the whole body: whatever the shader writes to memory
// is written that many times. Donkey Kong Bananza's terrain culling shader 5d537f7630ec3671 added each surviving triangle 3 times to its chunk's indirect draw record and wrote
// its indices 3 times, past the chunk's index buffer into the next chunk's: black holes in the cave walls. Vulkan says nothing of how many times a geometry shader runs, so the
// module claims its primitive first: one word per primitive in a storage buffer the replay binds at set 0 binding `binding` (VKA_GS_CLAIM_BINDING, zeroed for each draw), set by an atomic exchange
// at gl_PrimitiveIDIn; only the run that finds it 0 writes memory. Every store and copy into Uniform / StorageBuffer / PhysicalStorageBuffer / Image memory, image write and
// atomic is put under that condition (its block split around it); the other runs see an atomic's result as the memory's value (a compare-exchange its comparator, so a
// compare-exchange loop ends). What the shader emits is left alone: every run emits as before. Returns the count of memory writes (0: nothing to do, *out NULL), -1 with err
// on failure; with out and a count above 0, the rewritten module (malloc'd, *out_nw words)
enum { OpUndef = 1, OpExecutionMode = 16, OpTypeRuntimeArray = 29, OpTypeStruct = 30, OpTypeForwardPointer = 39,
       OpFunctionEnd = 56, OpFunctionParameter = 55, OpImageTexelPointer = 60, OpStore = 62, OpCopyMemory = 63, OpCopyMemorySized = 64, OpPtrAccessChain = 67,
       OpInBoundsPtrAccessChain = 70, OpImageWrite = 99, OpConvertUToPtr = 120, OpIEqual = 170, OpPhi = 245, OpLoopMerge = 246, OpSelectionMerge = 247, OpLabel = 248,
       OpBranch = 249, OpBranchConditional = 250, OpSwitch = 251, OpKill = 252, OpReturn = 253, OpReturnValue = 254, OpUnreachable = 255, OpTerminateInvocation = 4416,
       OpAtomicStore = 228, OpAtomicExchange = 229, OpAtomicCompareExchange = 230, OpAtomicCompareExchangeWeak = 231, OpAtomicXor = 242,
       OpAtomicFMinEXT = 5614, OpAtomicFMaxEXT = 5615, OpAtomicFAddEXT = 6035, OpDecorationGroup = 73, OpGroupDecorate = 74, OpGroupMemberDecorate = 75 };
enum { SCUniform = 2, SCPrivate = 6, SCImage = 11, SCStorageBuffer = 12, SCPhysicalStorageBuffer = 5349, DecBufferBlock = 3, DecArrayStride = 6, DecOffset = 35,
       DecBinding = 33, DecDescriptorSet = 34, BIPrimitiveId = 7 };
static int sc_mem(uint32_t sc) { return sc == SCUniform || sc == SCImage || sc == SCStorageBuffer || sc == SCPhysicalStorageBuffer; }
static int is_term(uint32_t op) { return op == OpBranch || op == OpBranchConditional || op == OpSwitch || op == OpKill || op == OpReturn || op == OpReturnValue || op == OpUnreachable || op == OpTerminateInvocation; }
static int is_annot(uint32_t op) { return op == OpDecorate || op == OpMemberDecorate || op == OpDecorationGroup || op == OpGroupDecorate || op == OpGroupMemberDecorate || op == OpDecorateId || op == OpDecorateString || op == OpMemberDecorateString; }
// One pass of the rewrite, run twice: the first records in rec where each split block of the input ends, the second gives that to the phis (last) that name such a block
// as their parent (the new ids are the same in both passes)
static int claim_pass(const uint32_t *w, size_t nw, uint32_t binding, const uint32_t *last, uint32_t *rec, vec_t *o, int *found, char *err, size_t errn) {
#define FAIL(...) do { snprintf(err, errn, __VA_ARGS__); goto fail; } while (0)
    uint32_t bound = w[3], nb = bound;
    uint32_t *psc = calloc(bound, 4), *ty = calloc(bound, 4);   // psc[pointer type]: storage class + 1; ty[id]: the type of a pointer-valued id
    if (!psc || !ty) FAIL("out of memory");
    uint32_t t_bool = 0, t_u32 = 0, t_i32 = 0, p_in_i32 = 0, p_priv_bool = 0, p_uni_u32 = 0, main_fn = 0, prim_var = 0;
    size_t first_fn = nw, first_annot = nw, ep_at = nw;
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > nw) FAIL("bad instruction at word %zu", i);
        if ((op == OpTypePointer || op == OpTypeForwardPointer) && len >= 3 && w[i + 1] < bound) psc[w[i + 1]] = w[i + 2] + 1;
        if (op == OpTypePointer && len >= 4) {
            if (w[i + 2] == SCInput && w[i + 3] && w[i + 3] == t_i32) p_in_i32 = w[i + 1];
            if (w[i + 2] == SCPrivate && w[i + 3] && w[i + 3] == t_bool) p_priv_bool = w[i + 1];
            if (w[i + 2] == SCUniform && w[i + 3] && w[i + 3] == t_u32) p_uni_u32 = w[i + 1];
        }
        else if ((op == OpVariable || op == OpAccessChain || op == OpInBoundsAccessChain || op == OpPtrAccessChain || op == OpInBoundsPtrAccessChain || op == OpConvertUToPtr ||
                  op == OpBitcast || op == OpCopyObject || op == OpImageTexelPointer || op == OpFunctionParameter) && len >= 3 && w[i + 2] < bound) ty[w[i + 2]] = w[i + 1];
        if (op == OpTypeBool) t_bool = w[i + 1];
        else if (op == OpTypeInt && len >= 4 && w[i + 2] == 32) { if (w[i + 3]) t_i32 = w[i + 1]; else t_u32 = w[i + 1]; }
        else if (op == OpEntryPoint && len >= 3) { if (main_fn) FAIL("more than one entry point"); main_fn = w[i + 2]; ep_at = i; }
        else if (op == OpDecorate && len >= 4 && w[i + 2] == DecBuiltIn && w[i + 3] == BIPrimitiveId) prim_var = w[i + 1];
        else if (op == OpExecutionMode && len >= 4 && w[i + 2] == 0 && w[i + 3] > 1) FAIL("%u invocations a primitive: they would share its claim", w[i + 3]);   // Invocations
        if (is_annot(op) && first_annot == nw) first_annot = i;
        if (op == OpFunction && first_fn == nw) first_fn = i;
        i += len;
    }
    if (!main_fn || first_fn == nw || !t_u32) FAIL("no entry point, function or 32-bit unsigned type");
    if (first_annot > first_fn) first_annot = first_fn;
    if (prim_var && (!p_in_i32 || ty[prim_var] != p_in_i32)) FAIL("a PrimitiveId input that is not a 32-bit int");
    // New ids: types missing, the claim buffer's types and variable, the constants, the guard variable
    uint32_t mk_bool = t_bool ? 0 : nb++, mk_i32 = t_i32 ? 0 : nb++; if (!t_bool) t_bool = mk_bool; if (!t_i32) t_i32 = mk_i32;
    uint32_t mk_pin = p_in_i32 && !mk_i32 ? 0 : nb++, mk_ppb = p_priv_bool && !mk_bool ? 0 : nb++, mk_pu = p_uni_u32 ? 0 : nb++;
    if (mk_pin) p_in_i32 = mk_pin; if (mk_ppb) p_priv_bool = mk_ppb; if (mk_pu) p_uni_u32 = mk_pu;
    uint32_t mk_prim = prim_var ? 0 : nb++; if (!prim_var) prim_var = mk_prim;
    uint32_t rta = nb++, st = nb++, pst = nb++, cvar = nb++, c0 = nb++, c1 = nb++, gvar = nb++;
    if (!put_n(o, w, 5)) FAIL("out of memory");
    uint32_t cur = 0, cur_orig = 0; int in_fn = 0, in_main_entry = 0;
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16; const uint32_t *in = w + i;
        if (i == ep_at && !mk_prim) { if (!put_n(o, in, len)) FAIL("out of memory"); i += len; continue; }
        if (i == ep_at) {   // the PrimitiveId input joins the interface
            if (!put(o, (len + 1) << 16 | OpEntryPoint) || !put_n(o, in + 1, len - 1) || !put(o, prim_var)) FAIL("out of memory");
            i += len; continue;
        }
        if (i == first_annot) {
            uint32_t x[] = {4u << 16 | OpDecorate, rta, DecArrayStride, 4, 3u << 16 | OpDecorate, st, DecBufferBlock, 5u << 16 | OpMemberDecorate, st, 0, DecOffset, 0,
                            4u << 16 | OpDecorate, cvar, DecDescriptorSet, 0, 4u << 16 | OpDecorate, cvar, DecBinding, binding};
            if (!put_n(o, x, sizeof x / 4)) FAIL("out of memory");
            if (mk_prim) { uint32_t y[] = {4u << 16 | OpDecorate, mk_prim, DecBuiltIn, BIPrimitiveId}; if (!put_n(o, y, 4)) FAIL("out of memory"); }
        }
        if (i == first_fn) {
            if (mk_bool) { uint32_t x[] = {2u << 16 | OpTypeBool, mk_bool}; if (!put_n(o, x, 2)) FAIL("out of memory"); }
            if (mk_i32) { uint32_t x[] = {4u << 16 | OpTypeInt, mk_i32, 32, 1}; if (!put_n(o, x, 4)) FAIL("out of memory"); }
            if (mk_pin) { uint32_t x[] = {4u << 16 | OpTypePointer, mk_pin, SCInput, t_i32}; if (!put_n(o, x, 4)) FAIL("out of memory"); }
            if (mk_ppb) { uint32_t x[] = {4u << 16 | OpTypePointer, mk_ppb, SCPrivate, t_bool}; if (!put_n(o, x, 4)) FAIL("out of memory"); }
            if (mk_pu) { uint32_t x[] = {4u << 16 | OpTypePointer, mk_pu, SCUniform, t_u32}; if (!put_n(o, x, 4)) FAIL("out of memory"); }
            if (mk_prim) { uint32_t x[] = {4u << 16 | OpVariable, p_in_i32, mk_prim, SCInput}; if (!put_n(o, x, 4)) FAIL("out of memory"); }
            uint32_t x[] = {3u << 16 | OpTypeRuntimeArray, rta, t_u32, 3u << 16 | OpTypeStruct, st, rta, 4u << 16 | OpTypePointer, pst, SCUniform, st,
                            4u << 16 | OpVariable, pst, cvar, SCUniform, 4u << 16 | OpConstant, t_u32, c0, 0, 4u << 16 | OpConstant, t_u32, c1, 1,
                            4u << 16 | OpVariable, p_priv_bool, gvar, SCPrivate};
            if (!put_n(o, x, sizeof x / 4)) FAIL("out of memory");
        }
        if (op == OpFunction) { in_fn = 1; in_main_entry = w[i + 2] == main_fn ? 1 : 0; }
        else if (op == OpFunctionEnd) in_fn = 0;
        if (op == OpLabel) { cur = cur_orig = w[i + 1]; if (!put_n(o, in, len)) FAIL("out of memory"); i += len; continue; }
        if (in_main_entry == 1 && op != OpVariable && cur) {   // the claim, after the entry block's variables
            uint32_t pid = nb++, ptr = nb++, old = nb++, first = nb++;
            uint32_t x[] = {4u << 16 | OpLoad, t_i32, pid, prim_var, 6u << 16 | OpAccessChain, p_uni_u32, ptr, cvar, c0, pid,
                            7u << 16 | OpAtomicExchange, t_u32, old, ptr, c1, c0, c1, 5u << 16 | OpIEqual, t_bool, first, old, c0, 3u << 16 | OpStore, gvar, first};
            if (!put_n(o, x, sizeof x / 4)) FAIL("out of memory");
            in_main_entry = 2;
        }
        if (op == OpPhi && last) {   // a parent block that was split ends in its last piece
            if (!put_n(o, in, 3)) FAIL("out of memory");
            for (uint32_t k = 3; k + 1 < len; k += 2) { uint32_t par = in[k + 1]; if (!put(o, in[k]) || !put(o, par < bound && last[par] ? last[par] : par)) FAIL("out of memory"); }
            i += len; continue;
        }
        #define SC(id) ((id) < bound && ty[id] < bound && psc[ty[id]] ? psc[ty[id]] - 1 : 0u)
        int drop = 0, atomic = 0;
        if (in_fn && (op == OpStore || op == OpCopyMemory || op == OpCopyMemorySized || op == OpAtomicStore) && len >= 2 && sc_mem(SC(in[1]))) drop = 1;
        else if (in_fn && op == OpImageWrite) drop = 1;
        else if (in_fn && ((op >= OpAtomicExchange && op <= OpAtomicXor) || op == OpAtomicFMinEXT || op == OpAtomicFMaxEXT || op == OpAtomicFAddEXT) && len >= 4 && sc_mem(SC(in[3]))) atomic = 1;
        if (drop || atomic) {
            (*found)++;
            for (size_t j = i; j < nw;) { uint32_t oj = w[j] & 0xffff; if (oj == OpLoopMerge) FAIL("a memory write in a loop header block"); if (is_term(oj)) break; j += w[j] >> 16; }
            uint32_t g = nb++, a = nb++, l = nb++, fb = 0;
            if (atomic) {
                uint32_t sc = SC(in[3]);
                if ((op == OpAtomicCompareExchange || op == OpAtomicCompareExchangeWeak) && len >= 9) fb = in[8];
                else { fb = nb++;
                    if (sc == SCImage) { uint32_t x[] = {3u << 16 | OpUndef, in[1], fb}; if (!put_n(o, x, 3)) FAIL("out of memory"); }
                    else if (sc == SCPhysicalStorageBuffer) { uint32_t x[] = {6u << 16 | OpLoad, in[1], fb, in[3], 2, 4}; if (!put_n(o, x, 6)) FAIL("out of memory"); }   // Aligned 4
                    else { uint32_t x[] = {4u << 16 | OpLoad, in[1], fb, in[3]}; if (!put_n(o, x, 4)) FAIL("out of memory"); } }
            }
            uint32_t x[] = {4u << 16 | OpLoad, t_bool, g, gvar, 3u << 16 | OpSelectionMerge, l, 0, 4u << 16 | OpBranchConditional, g, a, l, 2u << 16 | OpLabel, a};
            if (!put_n(o, x, sizeof x / 4)) FAIL("out of memory");
            if (atomic) { uint32_t r = in[2]; if (!put(o, in[0]) || !put(o, in[1]) || !put(o, nb) || !put_n(o, in + 3, len - 3)) FAIL("out of memory");
                uint32_t ra = nb++, y[] = {2u << 16 | OpBranch, l, 2u << 16 | OpLabel, l, 7u << 16 | OpPhi, in[1], r, ra, a, fb, cur};
                if (!put_n(o, y, sizeof y / 4)) FAIL("out of memory"); }
            else { if (!put_n(o, in, len)) FAIL("out of memory"); uint32_t y[] = {2u << 16 | OpBranch, l, 2u << 16 | OpLabel, l}; if (!put_n(o, y, 4)) FAIL("out of memory"); }
            cur = l; if (rec && cur_orig < bound) rec[cur_orig] = l;
            i += len; continue;
        }
        #undef SC
        if (!put_n(o, in, len)) FAIL("out of memory");
        i += len;
    }
    o->w[3] = nb;
    free(psc); free(ty);
    return 0;
fail:
    free(psc); free(ty);
    return -1;
#undef FAIL
}
int vka_spv_gs_claim(const uint32_t *w, size_t nw, uint32_t binding, uint32_t **out, size_t *out_nw, char *err, size_t errn) {
    if (out) *out = NULL;
    if (nw < 5 || w[0] != 0x07230203) { snprintf(err, errn, "not SPIR-V"); return -1; }
    uint32_t *rec = calloc(w[3], 4); vec_t o = {0}; int found = 0;
    if (!rec) { snprintf(err, errn, "out of memory"); return -1; }
    if (claim_pass(w, nw, binding, NULL, rec, &o, &found, err, errn)) { free(rec); free(o.w); return -1; }
    if (found && out) {
        free(o.w); o = (vec_t){0}; int f2 = 0;
        if (claim_pass(w, nw, binding, rec, NULL, &o, &f2, err, errn)) { free(rec); free(o.w); return -1; }
        *out = o.w; *out_nw = o.n; o.w = NULL;
    }
    free(rec); free(o.w);
    return found;
}

// vka_spv_fout_retype: float colour outputs drawn into integer targets. On Maxwell / Ampere a colour output is a 32-bit register per component and the target's format decides
// how its bits are read: a pixel shader the translator types as float (its outputs computed with float instructions, or moved through float registers) still writes the bits
// it holds into a UINT / SINT target. Vulkan leaves a float output to an integer attachment undefined, and the library's MOD_UINT / MOD_UINT_MIXED variants exist only for
// shaders whose outputs come from integer bits or constants (vk_android_build.py ps_variants). So for a pipeline where a float output meets an integer attachment, the module's
// Output variables at those locations (a float or a float vector) become the unsigned / signed integer type of as many components, each store into one (whole, or a component
// through an access chain) stores the OpBitcast of its value, and each load of one is bit-cast back. The bits reach the target unchanged, as on the Switch.
// Mario Kart World's fragment dcb0504c844f1ecc (DrivePassShaderDefault, paired with task-less mesh programs: by their shape ShadingRateWrite) writes its UINT target 0 this way
enum { SCOutput = 3 };
uint32_t *vka_spv_fout_retype(const uint32_t *w, size_t nw, uint32_t umask, uint32_t smask, size_t *out_nw, char *err, size_t errn) {
#define FAIL(...) do { snprintf(err, errn, __VA_ARGS__); goto fail; } while (0)
    uint32_t *cls = NULL, *loc = NULL, *ty = NULL; vec_t o = {0};   // cls[id]: 1 uint / 2 int, bit 4 the whole variable (else a component chain); ty: for each float type id its components (1..4), 0 none
    if (nw < 5 || w[0] != 0x07230203) { snprintf(err, errn, "not SPIR-V"); return NULL; }
    uint32_t bound = w[3];
    if ((umask | smask) == 0 || (umask & smask)) { snprintf(err, errn, "no output, or an output both uint and int"); return NULL; }
    if (!(cls = calloc(bound, 4)) || !(loc = calloc(bound, 4)) || !(ty = calloc(bound, 4))) FAIL("out of memory");
    for (uint32_t i = 0; i < bound; i++) loc[i] = ~0u;
    uint32_t t_f32 = 0, t_int[2] = {0}, t_ivec[2][5] = {{0}}, p_out_int[2][5] = {{0}};   // [uint/int][components]; the existing types
    uint32_t *ptee = calloc(bound, 4); if (!ptee) FAIL("out of memory");                // Output pointer type → its pointee
    size_t first_fn = nw;
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > nw) { free(ptee); FAIL("bad instruction at word %zu", i); }
        if (op == OpDecorate && len >= 4 && w[i + 2] == DecLocation && w[i + 1] < bound) loc[w[i + 1]] = w[i + 3];
        else if (op == OpTypeFloat && len >= 3 && w[i + 2] == 32 && w[i + 1] < bound) { t_f32 = w[i + 1]; ty[t_f32] = 1; }
        else if (op == OpTypeInt && len >= 4 && w[i + 2] == 32) t_int[w[i + 3] ? 1 : 0] = w[i + 1];
        else if (op == OpTypeVector && len >= 4 && w[i + 3] >= 2 && w[i + 3] <= 4 && w[i + 1] < bound) {
            if (w[i + 2] == t_f32 && t_f32) ty[w[i + 1]] = w[i + 3];
            for (int c = 0; c < 2; c++) if (w[i + 2] == t_int[c] && t_int[c]) t_ivec[c][w[i + 3]] = w[i + 1];
        }
        else if (op == OpTypePointer && len >= 4 && w[i + 2] == SCOutput && w[i + 1] < bound) {
            ptee[w[i + 1]] = w[i + 3];
            for (int c = 0; c < 2; c++) { if (w[i + 3] == t_int[c] && t_int[c]) p_out_int[c][1] = w[i + 1]; for (int n = 2; n <= 4; n++) if (w[i + 3] == t_ivec[c][n] && t_ivec[c][n]) p_out_int[c][n] = w[i + 1]; }
        }
        else if (op == OpFunction && first_fn == nw) first_fn = i;
        i += len;
    }
    for (int c = 0; c < 2; c++) t_ivec[c][1] = t_int[c];
    // The output variables to retype: Location in a mask, a pointer to a float or float vector
    uint32_t moved[32], mcomp[32]; int nmoved = 0; uint32_t found = 0, need[2] = {0};   // need[c]: bit n = the integer type of n components (and its pointers) is used
    for (size_t i = 5; i < first_fn;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (op == OpVariable && len >= 4 && w[i + 3] == SCOutput && w[i + 1] < bound && w[i + 2] < bound) {
            uint32_t id = w[i + 2], l = loc[id], pt = ptee[w[i + 1]], n = pt < bound ? ty[pt] : 0;
            if (l < 32 && ((umask | smask) >> l & 1)) {
                if (!n) { free(ptee); FAIL("output %u is not a float or float vector", l); }
                if (len > 4) { free(ptee); FAIL("output %u has an initializer", l); }   // (the translator emits none; re-emitted without it, unwritten components would change)
                if (nmoved == 32) { free(ptee); FAIL("too many outputs"); }
                int c = umask >> l & 1 ? 0 : 1;
                cls[id] = (uint32_t)(c + 1) | 16; mcomp[nmoved] = n; moved[nmoved++] = id;
                found |= 1u << l; need[c] |= 1u << n | 2u;   // (the scalar for component chains)
            }
        }
        i += len;
    }
    free(ptee);
    if (found != (umask | smask)) FAIL("outputs %#x are not float Output variables", (umask | smask) & ~found);
    // New ids for the types the module lacks
    uint32_t nb = bound, mk_t[2][5] = {{0}}, mk_p[2][5] = {{0}};
    for (int c = 0; c < 2; c++) for (int n = 1; n <= 4; n++) if (need[c] >> n & 1) {
        if (!t_ivec[c][n]) t_ivec[c][n] = mk_t[c][n] = nb++;
        if (!p_out_int[c][n]) p_out_int[c][n] = mk_p[c][n] = nb++;
    }
    if ((need[0] && !t_ivec[0][1]) || (need[1] && !t_ivec[1][1])) FAIL("no scalar type");
    // Pass 2: copy, retyping (the new types and the moved variables go right before the first function, as in vka_spv_vin_retype)
    if (!put_n(&o, w, 5)) FAIL("out of memory");
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16; const uint32_t *in = w + i;
        if (i == first_fn) {
            for (int c = 0; c < 2; c++) {
                if (mk_t[c][1]) { uint32_t x[4] = {4u << 16 | OpTypeInt, mk_t[c][1], 32, (uint32_t)c}; if (!put_n(&o, x, 4)) FAIL("out of memory"); }
                for (int n = 2; n <= 4; n++) if (mk_t[c][n]) { uint32_t x[4] = {4u << 16 | OpTypeVector, mk_t[c][n], t_ivec[c][1], (uint32_t)n}; if (!put_n(&o, x, 4)) FAIL("out of memory"); }
                for (int n = 1; n <= 4; n++) if (mk_p[c][n]) { uint32_t x[4] = {4u << 16 | OpTypePointer, mk_p[c][n], SCOutput, t_ivec[c][n]}; if (!put_n(&o, x, 4)) FAIL("out of memory"); }
            }
            for (int k = 0; k < nmoved; k++) { uint32_t x[4] = {4u << 16 | OpVariable, p_out_int[(cls[moved[k]] & 3) - 1][mcomp[k]], moved[k], SCOutput}; if (!put_n(&o, x, 4)) FAIL("out of memory"); }
        }
        if (op == OpVariable && len >= 4 && w[i + 2] < bound && cls[w[i + 2]] && i < first_fn) { i += len; continue; }   // re-emitted above
        if ((op == OpAccessChain || op == OpInBoundsAccessChain) && len >= 4 && w[i + 3] < bound && cls[w[i + 3]]) {
            int c = (cls[w[i + 3]] & 3) - 1; uint32_t r = w[i + 2];
            if (r >= bound) FAIL("bad id");
            if (len != 5 || !(cls[w[i + 3]] & 16)) FAIL("an access chain of %u indexes into a retyped output", len - 4);
            cls[r] = (uint32_t)(c + 1);   // a component: pointer to the integer scalar
            if (!put_n(&o, in, len)) FAIL("out of memory");
            o.w[o.n - len + 1] = p_out_int[c][1];
            i += len; continue;
        }
        if (op == OpStore && len >= 3 && w[i + 1] < bound && cls[w[i + 1]]) {
            uint32_t p = w[i + 1], c = (cls[p] & 3) - 1, n = 1;
            if (cls[p] & 16) { for (int k = 0; k < nmoved; k++) if (moved[k] == p) n = mcomp[k]; }
            uint32_t tmp = nb++;
            uint32_t x[4] = {4u << 16 | OpBitcast, t_ivec[c][n], tmp, w[i + 2]}; if (!put_n(&o, x, 4)) FAIL("out of memory");
            if (!put_n(&o, in, len)) FAIL("out of memory");
            o.w[o.n - len + 2] = tmp;
            i += len; continue;
        }
        if (op == OpLoad && len >= 4 && w[i + 3] < bound && cls[w[i + 3]]) {
            uint32_t p = w[i + 3], c = (cls[p] & 3) - 1, n = 1, ft = w[i + 1], r = w[i + 2], tmp = nb++;
            if (cls[p] & 16) { for (int k = 0; k < nmoved; k++) if (moved[k] == p) n = mcomp[k]; }
            if (!put_n(&o, in, len)) FAIL("out of memory");
            o.w[o.n - len + 1] = t_ivec[c][n]; o.w[o.n - len + 2] = tmp;
            uint32_t x[4] = {4u << 16 | OpBitcast, ft, r, tmp}; if (!put_n(&o, x, 4)) FAIL("out of memory");
            i += len; continue;
        }
        if (op != OpName && op != OpMemberName && op != OpEntryPoint && op != OpDecorate && op != OpMemberDecorate && op != OpDecorateId && op != OpDecorateString && op != OpMemberDecorateString
            && i >= first_fn)
            for (uint32_t k = 1; k < len; k++) if (!(op == OpExtInst && k == 4) && w[i + k] < bound && cls[w[i + k]]) FAIL("opcode %u uses a retyped output (id %u)", op, w[i + k]);
        if (!put_n(&o, in, len)) FAIL("out of memory");
        i += len;
    }
    o.w[3] = nb;
    free(cls); free(loc); free(ty);
    *out_nw = o.n; return o.w;
fail:
    free(cls); free(loc); free(ty); free(o.w);
    return NULL;
#undef FAIL
}

// vka_spv_spec_freeze: Qualcomm's Adreno 840 driver (Vulkan 1.4, driver 2150932524) reads SubgroupLocalInvocationId as 0 in every invocation of a compute or mesh shader
// whose module has a specialization constant (any SpecId, used or not, with or without VkSpecializationInfo), and the subgroup masks derived from it (Eq/Ge/Gt/Le/Lt) as
// those of lane 0; the subgroup operations themselves (shuffle, ballot, scans) still use the real lanes, and pixel shaders are not affected. Such a module with its
// specialization constants made plain constants reads its lanes right: SpecId spec_id takes value (a bool one: nonzero is true), the others keep their defaults, and the SpecId
// decorations are dropped. A malloc'd copy (*out_nw words); NULL when the module reads no lane built-in or has no SpecId (nothing to do), or has a specialization constant
// composite or operation (not folded here)
uint32_t *vka_spv_spec_freeze(const uint32_t *w, size_t nw, uint32_t spec_id, uint32_t value, size_t *out_nw) {
    if (nw < 5 || w[0] != 0x07230203) return NULL;
    enum { MAXSPEC = 16 }; uint32_t sid[MAXSPEC], sval[MAXSPEC]; int ns = 0, lane = 0;
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > nw) return NULL;
        if (op == OpDecorate && len >= 4 && w[i + 2] == DecSpecId) { if (ns == MAXSPEC) return NULL; sid[ns] = w[i + 1]; sval[ns++] = w[i + 3]; }
        else if (op == OpDecorate && len >= 4 && w[i + 2] == DecBuiltIn && (w[i + 3] == BISubgroupLocalInvocationId || (w[i + 3] >= BIEq && w[i + 3] <= BILt))) lane = 1;
        else if (op == OpSpecConstantComposite || op == OpSpecConstantOp) return NULL;
        i += len;
    }
    if (!lane || !ns) return NULL;
    uint32_t *o = malloc(nw * 4); if (!o) return NULL;
    size_t n = 0; memcpy(o, w, 5 * 4); n = 5;
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (op == OpDecorate && len >= 4 && w[i + 2] == DecSpecId) { i += len; continue; }
        memcpy(o + n, w + i, len * 4);
        if ((op == OpSpecConstantTrue || op == OpSpecConstantFalse || op == OpSpecConstant) && len >= 3) {
            int k = 0; while (k < ns && sid[k] != w[i + 2]) k++;
            int mine = k < ns && sval[k] == spec_id;
            if (op == OpSpecConstant) { o[n] = len << 16 | OpConstant; if (mine && len == 4) o[n + 3] = value; }
            else o[n] = len << 16 | (mine ? (value ? OpConstantTrue : OpConstantFalse) : op == OpSpecConstantTrue ? OpConstantTrue : OpConstantFalse);
        }
        n += len; i += len;
    }
    *out_nw = n; return o;
}

// vka_spv_img_retype: shader images and textures the translator types as float, bound to textures of an integer format. On Maxwell / Ampere a formatted surface access
// (SULD.P / SUST.P) and a texture fetch move 32-bit registers, and the bound texture's format decides how their bits are read and written: a float typed shader that packs RGB9E5
// or R11G11B10 texels into integer bits and stores them through an R32_UINT texture view writes those bits (Ocarina of Time's agl_ndsr_vr_light compute shader a335c1db6270c355),
// and one that samples an R16G16_UINT, R32_UINT or RGBA8_UINT view gets the integers in its registers. The translator knows an image's format and a texture's class only from
// the shader facts, and without them (no PC pairing) it declares every formatted image a float image of unknown format and every texture a float texture. Vulkan leaves another
// class than the view's undefined, and a float view of the same size in its place (vka_tex.c class_twin) converts: an R16G16 or R8G8B8A8 one as floats or normalized values,
// and even an R32 float view may flush the bits that make a denormal or NaN. So for a pipeline that binds integer textures there, the module's UniformConstant image and
// sampled image variables at set 0 bindings base + n (n in umask / smask) become images of uint / int sampled type, each read, fetch, sample or gather is bit-cast back to
// the float vector the body expects and each texel written is the bit-cast of its float vector: the bits pass unchanged, as on the Switch. Taken: non-arrayed bindings of an
// unknown-format float storage image, a float sampled image or a combined float image and sampler (not a depth image); any other use of them (a depth comparison, a sparse
// read, an atomic through a texel pointer, a copy, a call argument) refuses the module
enum { OpConstantNull = 46, OpTypeImage = 25, OpTypeSampledImage = 27, OpSampledImage = 86, OpImageSampleImplicitLod = 87, OpImageSampleExplicitLod = 88,
       OpImageSampleProjImplicitLod = 91, OpImageSampleProjExplicitLod = 92, OpImageFetch = 95, OpImageGather = 96, OpImageRead = 98, OpImage = 100,
       OpImageQuerySizeLod = 103, OpImageQuerySize = 104, OpImageQueryLod = 105, OpImageQueryLevels = 106, OpImageQuerySamples = 107, SCUniformConstant = 0 };
static int texel_read_op(uint32_t op) {   // a read whose result is the texel vector (operand 3 the image or sampled image)
    return op == OpImageSampleImplicitLod || op == OpImageSampleExplicitLod || op == OpImageSampleProjImplicitLod || op == OpImageSampleProjExplicitLod
        || op == OpImageFetch || op == OpImageGather || op == OpImageRead;
}
uint32_t *vka_spv_img_retype(const uint32_t *w, size_t nw, uint32_t base, uint64_t umask, uint64_t smask, size_t *out_nw, char *err, size_t errn) {
#define FAIL(...) do { snprintf(err, errn, __VA_ARGS__); goto fail; } while (0)
    uint32_t *cls = NULL, *bind = NULL, *set = NULL, *ty = NULL, *rt = NULL, *ptee = NULL, *si_of = NULL, *limg = NULL; vec_t o = {0};
    // cls[id]: 1 uint / 2 int (bit 4: a retyped variable, else a loaded image; bit 5: a sampled image); ty[id]: components of a float scalar / vector type; rt[id]: an id's
    // result type; si_of[id]: the image type of sampled image type id
    if (nw < 5 || w[0] != 0x07230203) { snprintf(err, errn, "not SPIR-V"); return NULL; }
    uint32_t bound = w[3];
    if ((umask | smask) == 0 || (umask & smask)) { snprintf(err, errn, "no image, or an image both uint and int"); return NULL; }
    if (!(cls = calloc(bound, 4)) || !(bind = calloc(bound, 4)) || !(set = calloc(bound, 4)) || !(ty = calloc(bound, 4)) || !(rt = calloc(bound, 4)) || !(ptee = calloc(bound, 4))
        || !(si_of = calloc(bound, 4)) || !(limg = calloc(bound, 4))) FAIL("out of memory");
    for (uint32_t i = 0; i < bound; i++) bind[i] = set[i] = ~0u;
    enum { MAXIMG = 16 };
    struct { uint32_t f[8]; } img[MAXIMG]; uint32_t img_id[MAXIMG]; int nimg = 0;   // every OpTypeImage: its 8 operand words after the result (sampled type .. format)
    uint32_t t_f32 = 0, t_int[2] = {0}, t_ivec[2][5] = {{0}};
    size_t first_fn = nw;
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > nw) FAIL("bad instruction at word %zu", i);
        if (op == OpDecorate && len >= 4 && w[i + 1] < bound) { if (w[i + 2] == DecBinding) bind[w[i + 1]] = w[i + 3]; else if (w[i + 2] == DecDescriptorSet) set[w[i + 1]] = w[i + 3]; }
        else if (op == OpTypeFloat && len >= 3 && w[i + 2] == 32 && w[i + 1] < bound) { t_f32 = w[i + 1]; ty[t_f32] = 1; }
        else if (op == OpTypeInt && len >= 4 && w[i + 2] == 32) t_int[w[i + 3] ? 1 : 0] = w[i + 1];
        else if (op == OpTypeVector && len >= 4 && w[i + 3] >= 2 && w[i + 3] <= 4 && w[i + 1] < bound) {
            if (w[i + 2] == t_f32 && t_f32) ty[w[i + 1]] = w[i + 3];
            for (int c = 0; c < 2; c++) if (w[i + 2] == t_int[c] && t_int[c]) t_ivec[c][w[i + 3]] = w[i + 1];
        }
        else if (op == OpTypeImage && len >= 9 && w[i + 1] < bound) { if (nimg == MAXIMG) FAIL("more than %d image types", MAXIMG); img_id[nimg] = w[i + 1]; memset(img[nimg].f, 0, 32); memcpy(img[nimg].f, w + i + 2, 7 * 4); nimg++; }
        else if (op == OpTypeSampledImage && len >= 3 && w[i + 1] < bound) si_of[w[i + 1]] = w[i + 2];
        else if (op == OpTypePointer && len >= 4 && w[i + 2] == SCUniformConstant && w[i + 1] < bound) ptee[w[i + 1]] = w[i + 3];
        else if (op == OpFunction && first_fn == nw) first_fn = i;
        if ((i >= first_fn || op == OpConstant || op == OpConstantComposite || op == OpConstantNull || op == OpUndef) && len >= 3 && w[i + 1] < bound && w[i + 2] < bound
            && op != OpStore && op != OpImageWrite && op != OpDecorate && op != OpName && op != OpFunction) rt[w[i + 2]] = w[i + 1];
        i += len;
    }
    for (int c = 0; c < 2; c++) t_ivec[c][1] = t_int[c];
    // The variables to retype: set 0, binding base + n of a mask, a pointer to a float, non-arrayed, unknown-format storage image (Sampled 2), a float sampled image (Sampled 1)
    // or a sampled image type of one (a combined image and sampler)
    enum { MAXV = 64 };
    uint32_t vid[MAXV], vimg[MAXV]; int nv = 0; uint64_t found = 0;
    for (size_t i = 5; i < first_fn;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (op == OpVariable && len >= 4 && w[i + 3] == SCUniformConstant && w[i + 2] < bound && w[i + 1] < bound) {
            uint32_t id = w[i + 2], b = bind[id];
            if (set[id] == 0 && b >= base && b - base < 64 && ((umask | smask) >> (b - base) & 1)) {
                uint32_t pt = ptee[w[i + 1]]; int comb = 0;
                if (pt && pt < bound && si_of[pt]) { pt = si_of[pt]; comb = 1; }
                int k = 0; while (k < nimg && img_id[k] != pt) k++;
                if (k == nimg) FAIL("binding %u is not an image (or an array of them)", b);
                const uint32_t *f = img[k].f;   // sampled type, dim, depth, arrayed, ms, sampled, format
                int storage = f[5] == 2 && f[6] == 0 && !comb, sampled = f[5] == 1 && f[2] != 1;
                if (f[0] != t_f32 || !(storage || sampled)) FAIL("binding %u is not a float storage image of unknown format or a float texture", b);
                if (nv == MAXV) FAIL("too many images");
                int c = umask >> (b - base) & 1 ? 0 : 1;
                cls[id] = (uint32_t)(c + 1) | 16 | (comb ? 32 : 0); vid[nv] = id; vimg[nv++] = (uint32_t)k;
                found |= 1ull << (b - base);
            }
        }
        i += len;
    }
    if (found != (umask | smask)) FAIL("bindings %#llx are not image or texture variables", (unsigned long long)((umask | smask) & ~found));
    // New ids: the integer scalar / vector types the module lacks, an integer twin of each image type taken (or the existing one), its sampled image type (for a texture),
    // and pointers to them
    uint32_t nb = bound, mk_t[2][5] = {{0}}, need[2] = {0};
    for (int k = 0; k < nv; k++) need[(cls[vid[k]] & 3) - 1] = 1;
    for (int c = 0; c < 2; c++) if (need[c]) for (int n = 1; n <= 4; n++) if (!t_ivec[c][n]) t_ivec[c][n] = mk_t[c][n] = nb++;
    uint32_t twin[MAXIMG][2] = {{0}}, mk_twin[MAXIMG][2] = {{0}}, ptwin[MAXIMG][2] = {{0}}, mk_ptwin[MAXIMG][2] = {{0}};
    uint32_t stwin[MAXIMG][2] = {{0}}, mk_stwin[MAXIMG][2] = {{0}}, pstwin[MAXIMG][2] = {{0}}, mk_pstwin[MAXIMG][2] = {{0}};
    for (int k = 0; k < nv; k++) {
        int c = (cls[vid[k]] & 3) - 1, m = (int)vimg[k], comb = (cls[vid[k]] & 32) != 0;
        if (!twin[m][c]) {
            for (int j = 0; j < nimg; j++) if (img[j].f[0] == t_int[c] && t_int[c] && !memcmp(img[j].f + 1, img[m].f + 1, 6 * 4)) twin[m][c] = img_id[j];
            if (!twin[m][c]) twin[m][c] = mk_twin[m][c] = nb++;
            if (img[m].f[5] == 1) {   // a texture: its sampled image type (OpSampledImage of a separate one makes it too)
                for (uint32_t x = 0; x < bound && !mk_twin[m][c] && !stwin[m][c]; x++) if (si_of[x] == twin[m][c]) stwin[m][c] = x;
                if (!stwin[m][c]) stwin[m][c] = mk_stwin[m][c] = nb++;
            }
        }
        uint32_t (*pt)[2] = comb ? pstwin : ptwin, (*mk)[2] = comb ? mk_pstwin : mk_ptwin, pointee = comb ? stwin[m][c] : twin[m][c];
        if (pt[m][c]) continue;
        for (uint32_t p = 0; p < bound && !pt[m][c]; p++) if (ptee[p] == pointee && pointee < bound) pt[m][c] = p;
        if (!pt[m][c]) pt[m][c] = mk[m][c] = nb++;
    }
    // Pass 2: copy, retyping (the new types and the moved variables go right before the first function)
    if (!put_n(&o, w, 5)) FAIL("out of memory");
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16; const uint32_t *in = w + i;
        if (i == first_fn) {
            for (int c = 0; c < 2; c++) {
                if (mk_t[c][1]) { uint32_t x[4] = {4u << 16 | OpTypeInt, mk_t[c][1], 32, (uint32_t)c}; if (!put_n(&o, x, 4)) FAIL("out of memory"); }
                for (int n = 2; n <= 4; n++) if (mk_t[c][n]) { uint32_t x[4] = {4u << 16 | OpTypeVector, mk_t[c][n], t_ivec[c][1], (uint32_t)n}; if (!put_n(&o, x, 4)) FAIL("out of memory"); }
            }
            for (int m = 0; m < nimg; m++) for (int c = 0; c < 2; c++) {
                if (mk_twin[m][c]) { uint32_t x[9] = {9u << 16 | OpTypeImage, mk_twin[m][c], t_ivec[c][1]}; memcpy(x + 3, img[m].f + 1, 6 * 4); if (!put_n(&o, x, 9)) FAIL("out of memory"); }
                if (mk_stwin[m][c]) { uint32_t x[3] = {3u << 16 | OpTypeSampledImage, mk_stwin[m][c], twin[m][c]}; if (!put_n(&o, x, 3)) FAIL("out of memory"); }
                if (mk_ptwin[m][c]) { uint32_t x[4] = {4u << 16 | OpTypePointer, mk_ptwin[m][c], SCUniformConstant, twin[m][c]}; if (!put_n(&o, x, 4)) FAIL("out of memory"); }
                if (mk_pstwin[m][c]) { uint32_t x[4] = {4u << 16 | OpTypePointer, mk_pstwin[m][c], SCUniformConstant, stwin[m][c]}; if (!put_n(&o, x, 4)) FAIL("out of memory"); }
            }
            for (int k = 0; k < nv; k++) { int c = (cls[vid[k]] & 3) - 1; uint32_t x[4] = {4u << 16 | OpVariable, cls[vid[k]] & 32 ? pstwin[vimg[k]][c] : ptwin[vimg[k]][c], vid[k], SCUniformConstant};
                                           if (!put_n(&o, x, 4)) FAIL("out of memory"); }
        }
        if (op == OpVariable && len >= 4 && w[i + 2] < bound && cls[w[i + 2]] && i < first_fn) { i += len; continue; }   // re-emitted above
        if (op == OpLoad && len >= 4 && w[i + 3] < bound && (cls[w[i + 3]] & 16) && w[i + 2] < bound) {
            uint32_t v = w[i + 3], c = (cls[v] & 3) - 1; int k = 0; while (vid[k] != v) k++;
            cls[w[i + 2]] = (c + 1) | (cls[v] & 32); limg[w[i + 2]] = vimg[k] + 1;
            if (!put_n(&o, in, len)) FAIL("out of memory");
            o.w[o.n - len + 1] = cls[v] & 32 ? stwin[vimg[k]][c] : twin[vimg[k]][c];
            i += len; continue;
        }
        if ((op == OpSampledImage || op == OpImage) && len >= 4 && w[i + 3] < bound && cls[w[i + 3]] && w[i + 2] < bound) {   // a texture and a sampler combined, or the image of a combined one
            uint32_t x3 = w[i + 3], c = (cls[x3] & 3) - 1, m = limg[x3] - 1;
            if (!limg[x3] || (op == OpSampledImage) == ((cls[x3] & 32) != 0)) FAIL("opcode %u on a retyped image of the other kind (id %u)", op, x3);
            if (op == OpSampledImage && !stwin[m][c]) FAIL("a storage image combined with a sampler (id %u)", x3);
            cls[w[i + 2]] = (c + 1) | (op == OpSampledImage ? 32 : 0); limg[w[i + 2]] = m + 1;
            if (!put_n(&o, in, len)) FAIL("out of memory");
            o.w[o.n - len + 1] = op == OpSampledImage ? stwin[m][c] : twin[m][c];
            i += len; continue;
        }
        if (texel_read_op(op) && len >= 5 && w[i + 3] < bound && cls[w[i + 3]]) {
            uint32_t c = (cls[w[i + 3]] & 3) - 1, ft = w[i + 1], r = w[i + 2], n = ft < bound ? ty[ft] : 0, tmp = nb++;
            if (!n) FAIL("a read of a retyped image into a non-float type %u", ft);
            if (!put_n(&o, in, len)) FAIL("out of memory");
            o.w[o.n - len + 1] = t_ivec[c][n]; o.w[o.n - len + 2] = tmp;
            uint32_t x[4] = {4u << 16 | OpBitcast, ft, r, tmp}; if (!put_n(&o, x, 4)) FAIL("out of memory");
            i += len; continue;
        }
        if (op == OpImageWrite && len >= 4 && w[i + 1] < bound && cls[w[i + 1]]) {
            uint32_t c = (cls[w[i + 1]] & 3) - 1, tx = w[i + 3], ft = tx < bound ? rt[tx] : 0, n = ft && ft < bound ? ty[ft] : 0, tmp = nb++;
            if (!n) FAIL("a write of a non-float texel (id %u) into a retyped image", tx);
            uint32_t x[4] = {4u << 16 | OpBitcast, t_ivec[c][n], tmp, tx}; if (!put_n(&o, x, 4)) FAIL("out of memory");
            if (!put_n(&o, in, len)) FAIL("out of memory");
            o.w[o.n - len + 3] = tmp;
            i += len; continue;
        }
        int query = op == OpImageQuerySize || op == OpImageQuerySizeLod || op == OpImageQueryLevels || op == OpImageQuerySamples || op == OpImageQueryLod;   // (the image operand only: their results are integers, or the LOD)
        if (!query && op != OpName && op != OpMemberName && op != OpEntryPoint && op != OpDecorate && op != OpMemberDecorate && op != OpDecorateId && op != OpDecorateString
            && op != OpMemberDecorateString && i >= first_fn)
            for (uint32_t k = 1; k < len; k++) if (!(op == OpExtInst && k == 4) && w[i + k] < bound && cls[w[i + k]]) FAIL("opcode %u uses a retyped image (id %u)", op, w[i + k]);
        if (!put_n(&o, in, len)) FAIL("out of memory");
        i += len;
    }
    o.w[3] = nb;
    free(cls); free(bind); free(set); free(ty); free(rt); free(ptee); free(si_of); free(limg);
    *out_nw = o.n; return o.w;
fail:
    free(cls); free(bind); free(set); free(ty); free(rt); free(ptee); free(si_of); free(limg); free(o.w);
    return NULL;
#undef FAIL
}

// vka_spv_instance: an instanced variant of a vertex or pixel shader (vka_draw.c's instanced runs, DM_VK_INSTANCE). Draws that repeat one mesh with only one uniform
// block's offset changed become one instanced draw: every read of the block at `binding` (the translator's vec4 data[4096] constant buffer) goes to data[i + d],
// d (vec4 units) a uint vertex input at location `loc`, fed per instance by the replay, and passed on as a flat uint output at the same location; in the pixel shader a
// flat uint input at `loc`. Checked against a Python reference built on SPIRV-Tools (for Donkey Kong Bananza's 14 most used
// pairs both give the same module byte for byte once reassembled, and spirv-val accepts it), made at pipeline time. Where a read of the block could not be moved (a chain with one index, a load of the whole
// block, the block handed to a function) this refuses the shader instead
enum { OpTypeVoid = 19, OpTypeForwardPointerX = 39, OpFunctionCall = 57, OpIAdd = 128, DecFlat = 14, BIInstanceIndex = 43 };
static int no_result(uint32_t op) {   // function-body instructions without a result type (the rest carry [type, result] in words 1-2)
    return op == OpStore || op == OpBranch || op == OpBranchConditional || op == OpSwitch || op == OpReturn || op == OpReturnValue || op == OpKill || op == OpUnreachable
        || op == OpSelectionMerge || op == OpLoopMerge || op == OpLabel || op == OpCopyMemory || op == OpCopyMemorySized || op == OpImageWrite || op == OpAtomicStore
        || op == OpTerminateInvocation || op == OpFunctionEnd || op == 224 || op == 225 || op == 8 || op == 317 || op == 5380 || op == 218 || op == 219;
}
uint32_t *vka_spv_instance(const uint32_t *w, size_t nw, uint32_t binding, uint32_t loc, size_t *out_nw, char *err, size_t errn) {
#undef FAIL
#define FAIL(...) do { snprintf(err, errn, __VA_ARGS__); goto fail; } while (0)
    uint32_t *type_of = NULL; vec_t o = {0};
    if (nw < 5 || w[0] != 0x07230203) { snprintf(err, errn, "not SPIR-V"); return NULL; }
    uint32_t bound = w[3];
    if (!(type_of = calloc(bound, 4))) { snprintf(err, errn, "out of memory"); return NULL; }
    // Pass 1: the stage and entry point, the block's variable, the int types, the places the additions go, the result type of every value an index can be
    int model = -1; uint32_t entry = 0, var = 0, t_u = 0, t_s = 0;
    size_t first_type = 0, first_dec = 0, first_fn = 0, last_gvar = 0;
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > nw) FAIL("bad instruction at word %zu", i);
        if (op == OpEntryPoint && model < 0 && len >= 3) { model = (int)w[i + 1]; entry = w[i + 2]; }
        else if (op == OpDecorate && len >= 4 && w[i + 2] == DecBinding && w[i + 3] == binding) var = w[i + 1];
        else if (op == OpDecorate && len >= 4 && w[i + 2] == DecLocation && w[i + 3] == loc) FAIL("location %u is taken", loc);
        else if (op == OpDecorate && len >= 4 && w[i + 2] == DecBuiltIn && w[i + 3] == BIInstanceIndex) FAIL("the shader reads its instance index");
        else if (op == OpTypeInt && len >= 4 && w[i + 2] == 32) { if (w[i + 3] && !t_s) t_s = w[i + 1]; else if (!w[i + 3] && !t_u) t_u = w[i + 1]; }
        if (op == OpDecorate && !first_dec) first_dec = i;
        if (op >= OpTypeVoid && op < OpTypeForwardPointerX && !first_type) first_type = i;
        if (op == OpFunction && !first_fn) first_fn = i;
        if (op == OpVariable && !first_fn) last_gvar = i;
        if ((op == OpConstant || op == OpSpecConstant) && len >= 3 && w[i + 2] < bound) type_of[w[i + 2]] = w[i + 1];
        else if (first_fn && op != OpFunction && !no_result(op) && len >= 3 && w[i + 2] < bound) type_of[w[i + 2]] = w[i + 1];
        i += len;
    }
    if (model != 0 && model != 4) FAIL("only vertex and pixel shaders (execution model %d)", model);
    if (!var) FAIL("no variable at binding %u", binding);
    if (!first_type || !first_dec || !first_fn || !last_gvar) FAIL("no types, decorations, functions or global variables");
    // New ids in the tool's order: uint (when missing), the Input pointer, the input; for a vertex shader the Output pointer and the output
    uint32_t nid = bound, u_new = 0;
    if (!t_u) t_u = u_new = nid++;
    uint32_t p_in = nid++, v_in = nid++, p_out = 0, v_out = 0;
    if (model == 0) { p_out = nid++; v_out = nid++; }
    // Pass 2: the module with the additions; per function the input loaded once (after the entry block's variables) and every chain into the block moved by it
    if (!put_n(&o, w, 5)) FAIL("out of memory");
    int in_fn = 0, need_load = 0; uint32_t fn = 0, ld = 0, ldi = 0, moved = 0;
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (i == first_dec) {
            uint32_t d[] = {4u << 16 | OpDecorate, v_in, DecLocation, loc, 3u << 16 | OpDecorate, v_in, DecFlat};
            if (model == 0) { uint32_t dv[] = {4u << 16 | OpDecorate, v_in, DecLocation, loc, 4u << 16 | OpDecorate, v_out, DecLocation, loc, 3u << 16 | OpDecorate, v_out, DecFlat};
                              if (!put_n(&o, dv, 11)) FAIL("out of memory"); }
            else if (!put_n(&o, d, 7)) FAIL("out of memory");
        }
        if (op == OpFunction) { in_fn = 1; fn = w[i + 2]; need_load = 0; if (!put_n(&o, w + i, len)) FAIL("out of memory"); i += len; continue; }
        if (op == OpLabel && in_fn && !need_load && !ld) { need_load = 1; if (!put_n(&o, w + i, len)) FAIL("out of memory"); i += len; continue; }
        if (in_fn && need_load == 1 && op != OpVariable) {   // the entry block's loads, after its variables
            ld = nid++; ldi = t_s ? nid++ : 0;
            uint32_t l[] = {4u << 16 | OpLoad, t_u, ld, v_in}; if (!put_n(&o, l, 4)) FAIL("out of memory");
            if (ldi) { uint32_t b[] = {4u << 16 | OpBitcast, t_s, ldi, ld}; if (!put_n(&o, b, 4)) FAIL("out of memory"); }
            if (v_out && fn == entry) { uint32_t s[] = {3u << 16 | OpStore, v_out, ld}; if (!put_n(&o, s, 3)) FAIL("out of memory"); }
            need_load = 2;
        }
        if ((op == OpAccessChain || op == OpInBoundsAccessChain) && len >= 4 && w[i + 3] == var) {
            if (len < 6) FAIL("a chain into the block with one index");
            uint32_t idx = w[i + 5], it = idx < bound ? type_of[idx] : 0, add = nid++;
            if (it != t_u && !(it == t_s && t_s)) FAIL("an index of a type other than a 32-bit int");
            uint32_t a[] = {5u << 16 | OpIAdd, it, add, idx, it == t_u ? ld : ldi}; if (!put_n(&o, a, 5)) FAIL("out of memory");
            size_t at = o.n; if (!put_n(&o, w + i, len)) FAIL("out of memory");
            o.w[at + 5] = add; moved++; i += len; continue;
        }
        if ((op == OpLoad && len >= 4 && w[i + 3] == var) || (op == OpStore && len >= 3 && w[i + 1] == var) || (op == OpCopyObject && len >= 4 && w[i + 3] == var)
            || ((op == OpPtrAccessChain || op == OpInBoundsPtrAccessChain) && len >= 4 && w[i + 3] == var)) FAIL("the block used other than through chains");
        if (op == OpFunctionCall) for (uint32_t k = 4; k < len; k++) if (w[i + k] == var) FAIL("the block handed to a function");
        if (op == OpFunctionEnd) { in_fn = 0; ld = ldi = 0; }
        if (op == OpEntryPoint) {   // the interface gets the new variables
            size_t at = o.n; if (!put_n(&o, w + i, len) || !put(&o, v_in) || (v_out && !put(&o, v_out))) FAIL("out of memory");
            o.w[at] = (len + 1 + (v_out != 0)) << 16 | OpEntryPoint; i += len; continue;
        }
        if (!put_n(&o, w + i, len)) FAIL("out of memory");
        if (i == first_type) {
            if (u_new) { uint32_t t[] = {4u << 16 | OpTypeInt, u_new, 32, 0}; if (!put_n(&o, t, 4)) FAIL("out of memory"); }
            uint32_t p[] = {4u << 16 | OpTypePointer, p_in, SCInput, t_u}; if (!put_n(&o, p, 4)) FAIL("out of memory");
            if (p_out) { uint32_t q[] = {4u << 16 | OpTypePointer, p_out, SCOutput, t_u}; if (!put_n(&o, q, 4)) FAIL("out of memory"); }
        }
        if (i == last_gvar) {
            uint32_t v[] = {4u << 16 | OpVariable, p_in, v_in, SCInput}; if (!put_n(&o, v, 4)) FAIL("out of memory");
            if (v_out) { uint32_t x[] = {4u << 16 | OpVariable, p_out, v_out, SCOutput}; if (!put_n(&o, x, 4)) FAIL("out of memory"); }
        }
        i += len;
    }
    if (!moved) FAIL("no reads of the block");
    o.w[3] = nid;
    free(type_of); *out_nw = o.n; return o.w;
fail:
    free(type_of); free(o.w); return NULL;
#undef FAIL
}

// vka_spv_gs_link: the pixel shader's inputs a geometry shader does not write. A passthrough geometry shader (SPH word 0 bit 24) hands the rasterizer its input primitive
// with every vertex attribute as the vertex shader wrote it; the device has no SPV_NV_geometry_shader_passthrough, so the translator emits the vertices itself
// (EmitterContext gs_passthrough false) and copies the attributes the next stage reads, which it learns only from a linked pixel shader (AttributeUsage.PassthroughAttributes,
// set by TranslatorContext.SetNextStage). The library translates each shader alone, so those copies are missing: DKB's terrain culling geometry shader 5d537f7630ec3671
// writes only gl_Position while the ground's pixel shader 7f7781d3621f0ce6 reads location 19. Turnip links that (the input reads undefined values); Qualcomm's Adreno
// driver (build 718a07726a) refuses the whole pipeline with VK_ERROR_UNKNOWN (-13), so the breakable ground was never drawn. Here, at pipeline time, where the three modules
// are known: each scalar or vector pixel input (32-bit float, int or uint) at a location the geometry shader has no output for gets an output, written before every
// OpEmitVertex from the vertex shader's output at that location for the vertex being emitted (the gl_in index of that vertex's position read, as the translator writes it
// right before; else vertex 0), or a null value when the vertex shader does not write that location either (undefined on the hardware too). *copied / *nulled: the locations.
// NULL with err empty: nothing to add
enum { OpTypeArrayL = 28, OpConstantNullL = 46, OpEmitVertexL = 218, OpEmitStreamVertexL = 220, OpMemberDecorateL = 72, BIPositionL = 0, DecComponentL = 2 };
typedef struct { const uint32_t *w; size_t n; uint32_t bound, *op, *a1, *a2, *a3; } spvmod_t;   // per id declared before the first function: its opcode and operands (smod_open)
static int smod_open(spvmod_t *m, const uint32_t *w, size_t n) {
    memset(m, 0, sizeof *m); m->w = w; m->n = n;
    if (n < 5 || w[0] != 0x07230203) return 0;
    m->bound = w[3];
    if (!(m->op = calloc(m->bound, 4)) || !(m->a1 = calloc(m->bound, 4)) || !(m->a2 = calloc(m->bound, 4)) || !(m->a3 = calloc(m->bound, 4))) return 0;
    for (size_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) return 0;
        if (op == OpFunction) break;
        // a type: its operands after the result; a variable or constant: its type, then the operands after the result (storage class; value)
        uint32_t r = 0, a[3] = {0};
        if (op == OpTypeFloat || op == OpTypeInt || op == OpTypeVector || op == OpTypePointer || op == OpTypeArrayL || op == OpTypeStruct) {
            r = w[i + 1]; for (int k = 0; k < 3; k++) a[k] = 2u + k < len ? w[i + 2 + k] : 0; }
        else if ((op == OpVariable || op == OpConstant) && len >= 4) { r = w[i + 2]; a[0] = w[i + 1]; a[1] = w[i + 3]; a[2] = len > 4 ? w[i + 4] : 0; }
        if (r && r < m->bound) { m->op[r] = op; m->a1[r] = a[0]; m->a2[r] = a[1]; m->a3[r] = a[2]; }
        i += len;
    }
    return 1;
}
static void smod_close(spvmod_t *m) { free(m->op); free(m->a1); free(m->a2); free(m->a3); }
// A type's signature: kind (1 float, 2 int, 3 uint, all 32-bit) << 4 | components; 0 = not a 32-bit scalar or vector
static uint32_t smod_sig(const spvmod_t *m, uint32_t t) {
    if (t >= m->bound) return 0;
    uint32_t n = 1;
    if (m->op[t] == OpTypeVector) { n = m->a2[t]; t = m->a1[t]; if (t >= m->bound || n < 2 || n > 4) return 0; }
    if (m->op[t] == OpTypeFloat && m->a1[t] == 32) return 1u << 4 | n;
    if (m->op[t] == OpTypeInt && m->a1[t] == 32) return (m->a2[t] ? 2u : 3u) << 4 | n;
    return 0;
}
// The user Input or Output variables (sc) by Location: sig[loc] their type's signature (0 none, 0xff a location whose variable has another type or a Component)
static void smod_io(const spvmod_t *m, uint32_t sc, uint8_t sig[32]) {
    memset(sig, 0, 32);
    uint8_t *comp = calloc(m->bound, 1); uint32_t *loc = malloc(m->bound * 4); if (!comp || !loc) { free(comp); free(loc); return; }
    for (uint32_t i = 0; i < m->bound; i++) loc[i] = ~0u;
    for (size_t i = 5; i < m->n;) {
        uint32_t op = m->w[i] & 0xffff, len = m->w[i] >> 16;
        if (op == OpFunction) break;
        if (op == OpDecorate && len >= 4 && m->w[i + 1] < m->bound) { if (m->w[i + 2] == DecLocation) loc[m->w[i + 1]] = m->w[i + 3]; else if (m->w[i + 2] == DecComponentL) comp[m->w[i + 1]] = 1; }
        i += len;
    }
    for (uint32_t v = 0; v < m->bound; v++) {
        if (m->op[v] != OpVariable || m->a2[v] != sc || loc[v] >= 32) continue;
        uint32_t pt = m->a1[v], s = pt < m->bound && m->op[pt] == OpTypePointer ? smod_sig(m, m->a2[pt]) : 0;
        sig[loc[v]] = comp[v] || !s ? 0xff : (uint8_t)s;
    }
    free(comp); free(loc);
}
uint32_t *vka_spv_gs_link(const uint32_t *gw, size_t gn, const uint32_t *vw, size_t vn, const uint32_t *fw, size_t fn, uint32_t *copied, uint32_t *nulled, size_t *out_nw, char *err, size_t errn) {
#define FAIL(...) do { snprintf(err, errn, __VA_ARGS__); goto fail; } while (0)
    spvmod_t G, Vm, F; vec_t o = {0}, decl = {0}, annot = {0}; uint8_t *isconst = NULL;   // isconst: 2 a Position block, then 1 an integer constant
    *copied = *nulled = 0; err[0] = 0;
    int okg = smod_open(&G, gw, gn), okv = smod_open(&Vm, vw, vn), okf = smod_open(&F, fw, fn);
    if (!okg || !okv || !okf) FAIL("not SPIR-V (%s)", !okg ? "geometry" : !okv ? "vertex" : "pixel");
    uint8_t fin[32], vout[32], gout[32]; smod_io(&F, SCInput, fin); smod_io(&Vm, SCOutput, vout); smod_io(&G, SCOutput, gout);
    uint32_t need = 0;
    for (int l = 0; l < 32; l++) if (fin[l] && fin[l] != 0xff && !gout[l]) need |= 1u << l;
    if (!need) goto none;
    // gl_in: the Input array of a block whose member 0 is BuiltIn Position; its length; constant ids (indexes taken from its access chains)
    uint32_t glin = 0, nvert = 0, ep_at = 0, first_fn = 0, first_annot = 0;
    if (!(isconst = calloc(G.bound, 1))) FAIL("out of memory");   // first: the blocks with a Position member 0 (gl_PerVertex in and out), then the integer constants
    for (size_t i = 5; i < gn;) {
        uint32_t op = gw[i] & 0xffff, len = gw[i] >> 16;
        if (op == OpFunction) { first_fn = (uint32_t)i; break; }
        if (op == OpEntryPoint) ep_at = (uint32_t)i;
        if (is_annot(op) && !first_annot) first_annot = (uint32_t)i;
        if (op == OpMemberDecorateL && len >= 5 && gw[i + 2] == 0 && gw[i + 3] == DecBuiltIn && gw[i + 4] == BIPositionL && gw[i + 1] < G.bound) isconst[gw[i + 1]] = 2;
        i += len;
    }
    if (!first_fn || !ep_at || !first_annot) FAIL("no function, entry point or decoration");
    for (uint32_t v = 0; v < G.bound; v++) {
        if (G.op[v] != OpVariable || G.a2[v] != SCInput) continue;
        uint32_t pt = G.a1[v], at = pt < G.bound ? G.a2[pt] : 0;
        if (at < G.bound && G.op[at] == OpTypeArrayL && G.a1[at] < G.bound && isconst[G.a1[at]] == 2 && G.a2[at] < G.bound && G.op[G.a2[at]] == OpConstant) { glin = v; nvert = G.a2[G.a2[at]]; }
    }
    if (!glin || !nvert || nvert > 6) FAIL("no gl_in array");
    for (uint32_t v = 0; v < G.bound; v++) isconst[v] = G.op[v] == OpConstant && G.a1[v] < G.bound && G.op[G.a1[v]] == OpTypeInt;
    // Types: reused when the module has them (a second declaration of a scalar, vector or pointer type is invalid), else declared before the first function
    uint32_t nb = G.bound;
    #define FIND(cond) ({ uint32_t _r = 0; for (uint32_t _t = 1; _t < G.bound && !_r; _t++) if (cond) _r = _t; _r; })
    uint32_t sc_t[4] = {0}, vec_t_[4][5] = {{0}};
    uint32_t mkp_sc[64], mkp_pt[64], mkp_id[64]; int nmkp = 0;   // pointer types made here
    uint32_t outv[32] = {0}, inv[32] = {0}, pin[32] = {0}, nul[32] = {0}, ty[32] = {0};
    for (int l = 0; l < 32; l++) if (need >> l & 1) {
        uint32_t s = fin[l], k = s >> 4, n = s & 15;
        if (!sc_t[k]) {
            sc_t[k] = k == 1 ? FIND(G.op[_t] == OpTypeFloat && G.a1[_t] == 32) : FIND(G.op[_t] == OpTypeInt && G.a1[_t] == 32 && G.a2[_t] == (k == 2));
            if (!sc_t[k]) { sc_t[k] = nb++; uint32_t x[4] = {(k == 1 ? 3u : 4u) << 16 | (k == 1 ? OpTypeFloat : OpTypeInt), sc_t[k], 32, k == 2}; if (!put_n(&decl, x, k == 1 ? 3 : 4)) FAIL("out of memory"); }
        }
        uint32_t t = sc_t[k];
        if (n > 1) {
            if (!vec_t_[k][n]) { vec_t_[k][n] = FIND(G.op[_t] == OpTypeVector && G.a1[_t] == sc_t[k] && G.a2[_t] == n);
                                 if (!vec_t_[k][n]) { vec_t_[k][n] = nb++; uint32_t x[4] = {4u << 16 | OpTypeVector, vec_t_[k][n], sc_t[k], n}; if (!put_n(&decl, x, 4)) FAIL("out of memory"); } }
            t = vec_t_[k][n];
        }
        ty[l] = t;
        for (int pass = 0; pass < 2; pass++) {   // the Output pointer, then (a copy) the Input pointer to the same type
            uint32_t sc = pass ? SCInput : SCOutput;
            if (pass && vout[l] != s) break;
            uint32_t p = FIND(G.op[_t] == OpTypePointer && G.a1[_t] == sc && G.a2[_t] == t);
            for (int q = 0; q < nmkp && !p; q++) if (mkp_sc[q] == sc && mkp_pt[q] == t) p = mkp_id[q];
            if (!p) { if (nmkp == 64) FAIL("too many types"); p = nb++; mkp_sc[nmkp] = sc; mkp_pt[nmkp] = t; mkp_id[nmkp++] = p; uint32_t x[4] = {4u << 16 | OpTypePointer, p, sc, t}; if (!put_n(&decl, x, 4)) FAIL("out of memory"); }
            if (!pass) { outv[l] = nb++; uint32_t x[4] = {4u << 16 | OpVariable, p, outv[l], SCOutput}; if (!put_n(&decl, x, 4)) FAIL("out of memory"); }
            else pin[l] = p;
        }
        uint32_t d[4] = {4u << 16 | OpDecorate, outv[l], DecLocation, (uint32_t)l}; if (!put_n(&annot, d, 4)) FAIL("out of memory");
        if (pin[l]) {   // uint length, the array type, its Input pointer, the variable
            uint32_t u = FIND(G.op[_t] == OpTypeInt && G.a1[_t] == 32 && G.a2[_t] == 0);
            if (!u) { if (!sc_t[3]) { sc_t[3] = nb++; uint32_t x[4] = {4u << 16 | OpTypeInt, sc_t[3], 32, 0}; if (!put_n(&decl, x, 4)) FAIL("out of memory"); } u = sc_t[3]; }
            uint32_t len_c = nb++, arr = nb++, parr = nb++; inv[l] = nb++;
            uint32_t x[] = {4u << 16 | OpConstant, u, len_c, nvert, 4u << 16 | OpTypeArrayL, arr, t, len_c, 4u << 16 | OpTypePointer, parr, SCInput, arr, 4u << 16 | OpVariable, parr, inv[l], SCInput};
            if (!put_n(&decl, x, sizeof x / 4)) FAIL("out of memory");
            uint32_t y[4] = {4u << 16 | OpDecorate, inv[l], DecLocation, (uint32_t)l}; if (!put_n(&annot, y, 4)) FAIL("out of memory");
            *copied |= 1u << l;
        } else { nul[l] = nb++; uint32_t x[3] = {3u << 16 | OpConstantNullL, t, nul[l]}; if (!put_n(&decl, x, 3)) FAIL("out of memory"); *nulled |= 1u << l; }
    }
    #undef FIND
    uint32_t zero = 0;   // vertex 0 when no gl_in read names the vertex: a uint constant
    { uint32_t u = sc_t[3]; if (!u) for (uint32_t t = 1; t < G.bound && !u; t++) if (G.op[t] == OpTypeInt && G.a1[t] == 32 && G.a2[t] == 0) u = t;
      if (!u) { u = sc_t[3] = nb++; uint32_t x[4] = {4u << 16 | OpTypeInt, u, 32, 0}; if (!put_n(&decl, x, 4)) FAIL("out of memory"); }
      zero = nb++; uint32_t x[4] = {4u << 16 | OpConstant, u, zero, 0}; if (!put_n(&decl, x, 4)) FAIL("out of memory"); }
    // Copy: annotations before the first one, declarations before the first function, the entry point's interface grown, the copies before each vertex emitted
    if (!put_n(&o, gw, 5)) FAIL("out of memory");
    uint32_t vtx = zero;
    for (size_t i = 5; i < gn;) {
        uint32_t op = gw[i] & 0xffff, len = gw[i] >> 16; const uint32_t *in = gw + i;
        if (i == first_annot && !put_n(&o, annot.w, annot.n)) FAIL("out of memory");
        if (i == first_fn && !put_n(&o, decl.w, decl.n)) FAIL("out of memory");
        if (i == ep_at) {
            size_t at = o.n; if (!put_n(&o, in, len)) FAIL("out of memory");
            for (int l = 0; l < 32; l++) { if (outv[l] && !put(&o, outv[l])) FAIL("out of memory"); if (inv[l] && !put(&o, inv[l])) FAIL("out of memory"); }
            o.w[at] = (uint32_t)(o.n - at) << 16 | OpEntryPoint; i += len; continue;
        }
        if (op == OpFunction) vtx = zero;
        if ((op == OpAccessChain || op == OpInBoundsAccessChain) && len >= 5 && gw[i + 3] == glin && gw[i + 4] < G.bound && isconst[gw[i + 4]]) vtx = gw[i + 4];
        if (op == OpEmitVertexL || op == OpEmitStreamVertexL)
            for (int l = 0; l < 32; l++) {
                if (!outv[l]) continue;
                uint32_t val = nul[l];
                if (inv[l]) { uint32_t p = nb++; val = nb++; uint32_t x[] = {5u << 16 | OpAccessChain, pin[l], p, inv[l], vtx, 4u << 16 | OpLoad, ty[l], val, p}; if (!put_n(&o, x, 9)) FAIL("out of memory"); }
                uint32_t s[3] = {3u << 16 | OpStore, outv[l], val}; if (!put_n(&o, s, 3)) FAIL("out of memory");
            }
        if (!put_n(&o, in, len)) FAIL("out of memory");
        i += len;
    }
    o.w[3] = nb;
    smod_close(&G); smod_close(&Vm); smod_close(&F); free(decl.w); free(annot.w); free(isconst);
    *out_nw = o.n; return o.w;
none:
fail:
    smod_close(&G); smod_close(&Vm); smod_close(&F); free(decl.w); free(annot.w); free(isconst); free(o.w);
    *copied = *nulled = 0;
    return NULL;
#undef FAIL
}

// A pixel shader's colour outputs by location: the components it can write (bit c of comp[loc], loc < 8), for the colour write mask (vka_draw.c). A whole-variable store
// writes all of the variable's components, a chain with a constant index its component, any other use of the variable (a chain with a variable index, a copy, a call) all
// of them; a component the variable does not have (a vec3 output at an RGBA target) is never written. 0 = done; -1 when the module is not read (outputs split by Component
// decorations, not SPIR-V): the caller then masks nothing more than before
int vka_spv_fout_comps(const uint32_t *w, size_t nw, uint8_t comp[8]) {
    enum { DecComponent = 2 };
    memset(comp, 0, 8);
    if (nw < 5 || w[0] != 0x07230203) return -1;
    uint32_t bound = w[3];
    uint32_t *loc = calloc(bound, 4), *ty = calloc(bound, 4), *ptee = calloc(bound, 4), *cval = calloc(bound, 4); uint8_t *isc = calloc(bound, 1), *var = calloc(bound, 1);
    int rc = -1;
    if (!loc || !ty || !ptee || !cval || !isc || !var) goto out;
    for (uint32_t i = 0; i < bound; i++) loc[i] = ~0u;
    uint32_t t_f32 = 0, ncomp_of[8] = {0}, var_of[8] = {0};
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > nw) goto out;
        if (op == OpDecorate && len >= 4 && w[i + 1] < bound) { if (w[i + 2] == DecLocation) loc[w[i + 1]] = w[i + 3]; else if (w[i + 2] == DecComponent) goto out; }
        else if (op == OpTypeFloat && len >= 3 && w[i + 2] == 32 && w[i + 1] < bound) { t_f32 = w[i + 1]; ty[t_f32] = 1; }
        else if (op == OpTypeInt && len >= 4 && w[i + 2] == 32 && w[i + 1] < bound) ty[w[i + 1]] = 1;   // (an integer output: its components count the same)
        else if (op == OpTypeVector && len >= 4 && w[i + 2] < bound && ty[w[i + 2]] && w[i + 1] < bound) ty[w[i + 1]] = w[i + 3];
        else if (op == OpTypePointer && len >= 4 && w[i + 2] == SCOutput && w[i + 1] < bound) ptee[w[i + 1]] = w[i + 3];
        else if (op == OpConstant && len >= 4 && w[i + 2] < bound) { isc[w[i + 2]] = 1; cval[w[i + 2]] = w[i + 3]; }
        else if (op == OpVariable && len >= 4 && w[i + 3] == SCOutput && w[i + 1] < bound && w[i + 2] < bound) {
            uint32_t id = w[i + 2], l = loc[id], pt = ptee[w[i + 1]], n = pt < bound ? ty[pt] : 0;
            if (l < 8 && n >= 1 && n <= 4) { var[id] = 1; var_of[l] = id; ncomp_of[l] = n; if (len > 4) comp[l] = (uint8_t)((1u << n) - 1); }   // (an initializer writes them all)
        }
        i += len;
    }
    (void)t_f32;
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (op != OpVariable && op != OpDecorate && op != OpName && op != OpEntryPoint && op != OpLoad && op != OpConstant && op > 17) {   // (not the literals of debug, mode and constant instructions)
            int chain = (op == OpAccessChain || op == OpInBoundsAccessChain) && len >= 5;
            for (uint32_t k = 1; k < len; k++) {
                uint32_t id = w[i + k]; if (id >= bound || !var[id]) continue;
                uint32_t l = loc[id], all = (1u << ncomp_of[l]) - 1;
                if (chain && k == 3) { uint32_t x = w[i + 4]; comp[l] |= (uint8_t)(x < bound && isc[x] && cval[x] < ncomp_of[l] ? 1u << cval[x] : all); }
                else comp[l] |= (uint8_t)all;
            }
        }
        i += len;
    }
    (void)var_of;
    rc = 0;
out:
    free(loc); free(ty); free(ptee); free(cval); free(isc); free(var);
    return rc;
}

// vka_spv_pool_fb: texture-use feedback for the texture pool. A shader whose texture handle the translator could not trace to a slot reads the NVN texture pool, the
// update-after-bind sampled-image array at set 1 binding 0 (dmsh_abi_vk.h DMSH_VK_POOL_TEX_BINDING), at the handle's texture ID: agl / gsys lit materials read their
// lightmaps that way, from a handle in a material record in a storage buffer. The host cannot see those IDs (no draw binds them), so it could only make an image for every
// registered texture (DM_VK_EAGER_TEX=1: Ocarina of Time's 6,000 images, issue #97) or for none (the lightmaps read 0: black walls). With this rewrite the GPU says which:
// before each access chain into the pool it loads word id >> 5 of a storage buffer (set `fb_set` binding `fb_binding`, a Block of uint words) and, when bit id & 31 is
// clear, sets it with an atomic OR; vka_tex.c reads the bits back after each submission and makes the images of the IDs read. The same index read again in the same block
// gets no second test. In a loop header block (whose first instructions must stay the header's) the atomic OR is done without the test. The idea and the bitmap's layout
// follow Anka Native's texture-use feedback (MIT, copyright 2026 seedds and 2026 EddizEge: vk_android_spv.py tex_feedback), which rewrites the library's modules at build time; here
// the replay rewrites them at pipeline time, so no library changes. A pixel shader with a store loses its early depth test (its stores must not happen for fragments the
// test would kill): VKA_PFB_EARLY adds EarlyFragmentTests where that gives the same result, which the module decides here (no discard, demote or terminate, no depth, sample
// mask or stencil export, no other memory write; or one discard, the alpha test's under the alpha_spec constant, which the pipeline turns off) except for alpha to coverage
// and the alpha test, which the pipeline decides (vka_tex.c vka_tex_pfb_module gives a pipeline that needs late tests the module without the mode)
enum { PF_BitwiseAnd = 199, PF_ShiftRightLogical = 194, PF_ShiftLeftLogical = 196, PF_AtomicOr = 241, PF_Demote = 5380, PF_EarlyFragmentTests = 9, PF_DepthReplacing = 12,
       PF_BISampleMask = 20, PF_BIFragDepth = 22, PF_BIStencilRef = 5014, PF_Fragment = 4, PF_GLCompute = 5 };
// Word k of an instruction of opcode op (length len) is a literal, not an id, in the instructions a function body has that carry literals among ids (others: none)
static int pf_lit(uint32_t op, uint32_t k) {
    switch (op) {
    case OpExtInst: return k == 4;
    case OpLoad: return k >= 4;
    case OpStore: return k >= 3;
    case 79: return k >= 5;                         // OpVectorShuffle: its components
    case 81: return k >= 4;                         // OpCompositeExtract: its indices
    case 82: return k >= 5;                         // OpCompositeInsert
    case OpSelectionMerge: return k == 2;
    case OpLoopMerge: return k >= 3;
    case OpBranchConditional: return k >= 4;        // branch weights
    case OpSwitch: return k >= 3 && !((k - 3) & 1);
    case 87: case 88: case 91: case 92: case 95: case 98: return k == 5;   // image operands' mask (the operands after it are ids)
    case 89: case 90: case 93: case 94: case 96: case 97: return k == 6;
    default: return 0;
    }
}
static int pf_mem_write(uint32_t op) { return op == OpStore || op == OpCopyMemory || op == OpCopyMemorySized || (op >= 228 && op <= 242) || op == 5614 || op == 5615 || op == 6035; }
static int pf_pass(const uint32_t *w, size_t nw, uint32_t set, uint32_t tex_binding, uint32_t fb_set, uint32_t fb_binding, unsigned flags, uint32_t alpha_spec, const uint32_t *last, uint32_t *rec, vec_t *o,
                   int *sites, int *early, char *err, size_t errn) {
#define FAIL(...) do { snprintf(err, errn, __VA_ARGS__); goto fail; } while (0)
    uint32_t bound = w[3], nb = bound;
    uint32_t *psc = calloc(bound, 4), *rt = calloc(bound, 4), *dset = malloc(bound * 4), *dbind = malloc(bound * 4), *ity = calloc(bound, 4), *bi = calloc(bound, 4), *vsc = calloc(bound, 4);
    uint8_t *pool = calloc(bound, 1);
    if (!psc || !rt || !dset || !dbind || !ity || !bi || !vsc || !pool) FAIL("out of memory");
    memset(dset, 0xff, bound * 4); memset(dbind, 0xff, bound * 4);
    uint32_t t_u32 = 0, t_bool = 0, p_uni_u32 = 0, model = UINT32_MAX;
    const uint32_t scb = w[1] >= 0x10300 ? SCStorageBuffer : SCUniform, blk = w[1] >= 0x10300 ? 2u : DecBufferBlock;   // (1.3 on: StorageBuffer and Block; before: Uniform and BufferBlock)
    int vtx = 0, kill = 0, mem_writes = 0, has_early = 0, depth_mode = 0, nsites = 0, aspec = 0;
    size_t first_fn = nw, first_annot = nw, last_ep = nw;
    for (size_t i = 5; i < nw;) {   // what the rewrite needs: types, the pool's variables, the sites, and whether a pixel shader's tests can run early
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > nw) FAIL("bad instruction at word %zu", i);
        if (op == OpEntryPoint && len >= 3) { last_ep = i; if (model == UINT32_MAX) model = w[i + 1]; else if (model != w[i + 1]) FAIL("entry points of two execution models"); if (w[i + 1] >= 1 && w[i + 1] <= 3) vtx = 1; if (w[i + 1] == 0) vtx = 1; }
        else if (op == OpExecutionMode && len >= 3) { if (w[i + 2] == PF_EarlyFragmentTests) has_early = 1; if (w[i + 2] == PF_DepthReplacing || (w[i + 2] >= 14 && w[i + 2] <= 16)) depth_mode = 1; }
        else if (op == OpDecorate && len >= 4 && w[i + 1] < bound) {
            if (w[i + 2] == DecDescriptorSet) dset[w[i + 1]] = w[i + 3];
            else if (w[i + 2] == DecBinding) dbind[w[i + 1]] = w[i + 3];
            else if (w[i + 2] == DecBuiltIn) bi[w[i + 1]] = w[i + 3] + 1;
            else if (w[i + 2] == DecSpecId && w[i + 3] == alpha_spec) aspec = 1;
        }
        else if (op == OpTypeInt && len >= 4 && w[i + 1] < bound) { ity[w[i + 1]] = w[i + 2] | (w[i + 3] ? 0x100u : 0); if (w[i + 2] == 32 && !w[i + 3] && !t_u32) t_u32 = w[i + 1]; }
        else if (op == OpTypeBool && len >= 2 && !t_bool) t_bool = w[i + 1];
        else if ((op == OpTypePointer || op == OpTypeForwardPointer) && len >= 3 && w[i + 1] < bound) {
            psc[w[i + 1]] = w[i + 2] + 1;
            if (op == OpTypePointer && len >= 4 && w[i + 2] == scb && w[i + 3] && w[i + 3] == t_u32 && !p_uni_u32) p_uni_u32 = w[i + 1];
        }
        else if (op == OpVariable && len >= 4 && w[i + 2] < bound) { rt[w[i + 2]] = w[i + 1]; vsc[w[i + 2]] = w[i + 3] + 1; if (dset[w[i + 2]] == set && dbind[w[i + 2]] == tex_binding) pool[w[i + 2]] = 1; }
        else if ((op == OpConstant || op == OpSpecConstant || op == OpConstantNull || op == OpUndef) && len >= 3 && w[i + 2] < bound) rt[w[i + 2]] = w[i + 1];
        if (is_annot(op) && first_annot == nw) first_annot = i;
        if (op == OpFunction && first_fn == nw) first_fn = i;
        if (first_fn < nw) {   // a function's body
            if (op == OpKill || op == OpTerminateInvocation || op == PF_Demote) kill++;
            if (op == OpImageWrite) mem_writes = 1;
            else if (pf_mem_write(op) && len >= 2) { uint32_t p = op == OpStore || op == OpCopyMemory || op == OpCopyMemorySized || op == 228 ? w[i + 1] : len >= 4 ? w[i + 3] : 0;
                uint32_t sc = p < bound && rt[p] < bound && psc[rt[p]] ? psc[rt[p]] - 1 : SCUniform;   // (a pointer of unknown class: counted as memory)
                if (sc_mem(sc)) mem_writes = 1; }
            if (!no_result(op) && op != OpLabel && op != OpFunction && len >= 3 && w[i + 2] < bound) rt[w[i + 2]] = w[i + 1];
            else if (op == OpFunction && len >= 3 && w[i + 2] < bound) rt[w[i + 2]] = w[i + 1];
            int site = (op == OpAccessChain || op == OpInBoundsAccessChain) && len >= 4 && w[i + 3] < bound && pool[w[i + 3]];
            if (site) {
                if (len != 5) FAIL("a texture pool access chain with %u indices", len - 4);
                uint32_t x = w[i + 4], t = x < bound ? rt[x] : 0;
                if (!t || t >= bound || (ity[t] & 0xff) != 32) FAIL("a texture pool index that is not a 32-bit integer");
                nsites++;
            }
            for (uint32_t k = 1; k < len; k++) if (w[i + k] < bound && pool[w[i + k]] && !(site && k == 3) && !pf_lit(op, k)) FAIL("the texture pool used by opcode %u, not a one-index access chain", op);
        }
        i += len;
    }
    if (!nsites) { free(psc); free(rt); free(dset); free(dbind); free(ity); free(bi); free(vsc); free(pool); return 0; }
    if (model == UINT32_MAX || first_fn == nw) FAIL("no entry point or function");
    if (vtx && !(flags & VKA_PFB_VTX_STORES)) FAIL("a vertex-pipeline stage on a device without vertexPipelineStoresAndAtomics");
    int exports = depth_mode;
    for (uint32_t id = 1; id < bound; id++) if (bi[id] && vsc[id] == SCOutput + 1 && (bi[id] - 1 == PF_BIFragDepth || bi[id] - 1 == PF_BISampleMask || bi[id] - 1 == PF_BIStencilRef)) exports = 1;
    // Early tests give the result late ones do: kind 1 always, kind 2 when the alpha test is off (its one discard, under the alpha_spec constant, the only one), 0 never
    int kind = model != PF_Fragment || has_early || exports || mem_writes ? 0 : !kill ? 1 : kill == 1 && aspec ? 2 : 0;
    int add_early = (flags & VKA_PFB_EARLY) && kind;
    if (first_annot > first_fn) first_annot = first_fn;
    // New ids, in the same order in both passes: missing types, the buffer's types and variable, the constants; then each site's
    uint32_t mk_u32 = t_u32 ? 0 : nb++, mk_bool = t_bool ? 0 : nb++; if (!t_u32) t_u32 = mk_u32; if (!t_bool) t_bool = mk_bool;
    uint32_t mk_pu = p_uni_u32 && !mk_u32 ? 0 : nb++; if (mk_pu) p_uni_u32 = mk_pu;
    uint32_t rta = nb++, st = nb++, pst = nb++, fbv = nb++, c0 = nb++, c1 = nb++, c5 = nb++, c31 = nb++, cmask = nb++;
    if (!put_n(o, w, 5)) FAIL("out of memory");
    uint32_t cur = 0, cur_orig = 0, done[16]; int ndone = 0, in_fn = 0;
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16; const uint32_t *in = w + i;
        if (op == OpEntryPoint && w[1] >= 0x10400) {   // SPIR-V 1.4 on: every global variable the entry point uses is in its interface
            if (!put(o, (len + 1) << 16 | OpEntryPoint) || !put_n(o, in + 1, len - 1) || !put(o, fbv)) FAIL("out of memory");
            if (i == last_ep && add_early) { uint32_t x[] = {3u << 16 | OpExecutionMode, in[2], PF_EarlyFragmentTests}; if (!put_n(o, x, 3)) FAIL("out of memory"); }
            i += len; continue;
        }
        if (i == first_annot) {
            uint32_t x[] = {4u << 16 | OpDecorate, rta, DecArrayStride, 4, 3u << 16 | OpDecorate, st, blk, 5u << 16 | OpMemberDecorate, st, 0, DecOffset, 0,
                            4u << 16 | OpDecorate, fbv, DecDescriptorSet, fb_set, 4u << 16 | OpDecorate, fbv, DecBinding, fb_binding};
            if (!put_n(o, x, sizeof x / 4)) FAIL("out of memory");
        }
        if (i == first_fn) {
            if (mk_u32) { uint32_t x[] = {4u << 16 | OpTypeInt, mk_u32, 32, 0}; if (!put_n(o, x, 4)) FAIL("out of memory"); }
            if (mk_bool) { uint32_t x[] = {2u << 16 | OpTypeBool, mk_bool}; if (!put_n(o, x, 2)) FAIL("out of memory"); }
            if (mk_pu) { uint32_t x[] = {4u << 16 | OpTypePointer, mk_pu, scb, t_u32}; if (!put_n(o, x, 4)) FAIL("out of memory"); }
            uint32_t x[] = {3u << 16 | OpTypeRuntimeArray, rta, t_u32, 3u << 16 | OpTypeStruct, st, rta, 4u << 16 | OpTypePointer, pst, scb, st,
                            4u << 16 | OpVariable, pst, fbv, scb, 4u << 16 | OpConstant, t_u32, c0, 0, 4u << 16 | OpConstant, t_u32, c1, 1,
                            4u << 16 | OpConstant, t_u32, c5, 5, 4u << 16 | OpConstant, t_u32, c31, 31, 4u << 16 | OpConstant, t_u32, cmask, 0xFFFFFu};
            if (!put_n(o, x, sizeof x / 4)) FAIL("out of memory");
        }
        if (op == OpFunction) in_fn = 1;
        else if (op == OpFunctionEnd) in_fn = 0;
        if (op == OpEntryPoint && i == last_ep && add_early) {   // (before 1.4: the mode after the last entry point)
            if (!put_n(o, in, len)) FAIL("out of memory");
            uint32_t x[] = {3u << 16 | OpExecutionMode, in[2], PF_EarlyFragmentTests}; if (!put_n(o, x, 3)) FAIL("out of memory");
            i += len; continue;
        }
        if (op == OpLabel) { cur = cur_orig = in[1]; ndone = 0; if (!put_n(o, in, len)) FAIL("out of memory"); i += len; continue; }
        if (op == OpPhi && last) {   // a parent block that was split ends in its last piece
            if (!put_n(o, in, 3)) FAIL("out of memory");
            for (uint32_t k = 3; k + 1 < len; k += 2) { uint32_t par = in[k + 1]; if (!put(o, in[k]) || !put(o, par < bound && last[par] ? last[par] : par)) FAIL("out of memory"); }
            i += len; continue;
        }
        if (in_fn && (op == OpAccessChain || op == OpInBoundsAccessChain) && len == 5 && in[3] < bound && pool[in[3]]) {
            uint32_t x = in[4]; int seen = 0;
            for (int k = 0; k < ndone; k++) if (done[k] == x) seen = 1;
            if (!seen) {
                if (ndone < 16) done[ndone++] = x;
                (*sites)++;
                int header = 0;
                for (size_t j = i; j < nw;) { uint32_t oj = w[j] & 0xffff; if (oj == OpLoopMerge) header = 1; if (is_term(oj)) break; j += w[j] >> 16; }
                uint32_t iu = x;
                if (rt[x] != t_u32) { iu = nb++; uint32_t y[] = {4u << 16 | OpBitcast, t_u32, iu, x}; if (!put_n(o, y, 4)) FAIL("out of memory"); }
                uint32_t m = nb++, wi = nb++, bt = nb++, bit = nb++, p = nb++;
                uint32_t y[] = {5u << 16 | PF_BitwiseAnd, t_u32, m, iu, cmask, 5u << 16 | PF_ShiftRightLogical, t_u32, wi, m, c5, 5u << 16 | PF_BitwiseAnd, t_u32, bt, m, c31,
                                5u << 16 | PF_ShiftLeftLogical, t_u32, bit, c1, bt, 6u << 16 | OpAccessChain, p_uni_u32, p, fbv, c0, wi};
                if (!put_n(o, y, sizeof y / 4)) FAIL("out of memory");
                if (header) { uint32_t r = nb++, z[] = {7u << 16 | PF_AtomicOr, t_u32, r, p, c1, c0, bit}; if (!put_n(o, z, 7)) FAIL("out of memory"); }   // (scope Device, semantics None)
                else {
                    uint32_t old = nb++, has = nb++, clr = nb++, a = nb++, l = nb++, r = nb++;
                    uint32_t z[] = {4u << 16 | OpLoad, t_u32, old, p, 5u << 16 | PF_BitwiseAnd, t_u32, has, old, bit, 5u << 16 | OpIEqual, t_bool, clr, has, c0,
                                    3u << 16 | OpSelectionMerge, l, 0, 4u << 16 | OpBranchConditional, clr, a, l, 2u << 16 | OpLabel, a,
                                    7u << 16 | PF_AtomicOr, t_u32, r, p, c1, c0, bit, 2u << 16 | OpBranch, l, 2u << 16 | OpLabel, l};
                    if (!put_n(o, z, sizeof z / 4)) FAIL("out of memory");
                    cur = l; if (rec && cur_orig < bound) rec[cur_orig] = l;
                }
            }
        }
        if (!put_n(o, in, len)) FAIL("out of memory");
        i += len;
    }
    (void)cur;
    o->w[3] = nb;
    if (early) *early = add_early ? kind : 0;
    free(psc); free(rt); free(dset); free(dbind); free(ity); free(bi); free(vsc); free(pool);
    return 1;
fail:
    free(psc); free(rt); free(dset); free(dbind); free(ity); free(bi); free(vsc); free(pool);
    return -1;
#undef FAIL
}
uint32_t *vka_spv_pool_fb(const uint32_t *w, size_t nw, uint32_t set, uint32_t tex_binding, uint32_t fb_set, uint32_t fb_binding, unsigned flags, uint32_t alpha_spec, size_t *out_nw, int *sites, int *early, char *err, size_t errn) {
    int s0 = 0, e0 = 0; if (!sites) sites = &s0; if (!early) early = &e0;
    *sites = 0; *early = 0; if (errn) err[0] = 0;
    if (nw < 5 || w[0] != 0x07230203) { snprintf(err, errn, "not SPIR-V"); return NULL; }
    uint32_t *rec = calloc(w[3], 4); vec_t o = {0};
    if (!rec) { snprintf(err, errn, "out of memory"); return NULL; }
    int r = pf_pass(w, nw, set, tex_binding, fb_set, fb_binding, flags, alpha_spec, NULL, rec, &o, sites, early, err, errn);
    if (r <= 0) { free(rec); free(o.w); *sites = 0; return NULL; }   // (0: no pool read, err empty)
    free(o.w); o = (vec_t){0}; *sites = 0;
    r = pf_pass(w, nw, set, tex_binding, fb_set, fb_binding, flags, alpha_spec, rec, NULL, &o, sites, early, err, errn);
    free(rec);
    if (r <= 0) { free(o.w); *sites = 0; return NULL; }
    *out_nw = o.n;
    return o.w;
}
