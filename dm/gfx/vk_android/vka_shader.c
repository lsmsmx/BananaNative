// Shaders of the Vulkan replay: the game's library (android/SHADERS.md), its entries as VkShaderModules, and NVN programs resolved to one shader per stage.
// The library is <pack folder>/shaders/v-<DMSH_VK_CHAIN>/dmsh_vk.lib beside its inputs.tsv (the pack folder is the one the modules came from, dm_module_dir), or the file
// DM_VK_SHLIB names. One whose chain, Vulkan chain or game is not this host's is refused with a [RED] line: the game runs on with no draws. While this version's library is
// not in the pack yet (the app's delta update makes it, android/SHADERS.md#delta-updates), another v-<V>/ of the pack whose header carries this host's DMSH_VK_ABI is played
// instead (the newest): what the replay reads of it is the same; the new library is taken from the next start. Modules are made at a program's first draw (the submit
// thread), not at nvnProgramSetShaders
#if defined(__ANDROID__)
#include "vka.h"
#include "../nvn/nvn_mtl.h"
#include "../nvn/dmsh_chain.h"
#include "../nvn/dmsh_chain_vk.h"
#include "game_profile.h"
#include "vka_spvpatch.h"
#include "plat/plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

static vka_shlib g_lib; static int g_lib_state;   // 0 not tried, 1 loaded, -1 none
static char g_lib_path[1024];                       // the library loaded (vka_shader_lib_path)
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

// How each stage runs the subgroup operations of a library built for sg lanes (DMSH_VK_F_SUBGROUP shaders). A module built for 64 lanes splits ballots and shuffles into
// 32-lane halves, so it is right on 32 or 64 lanes; one built for n lanes, on n. Per stage, in this order: no subgroup operations in the stage on this device = rewritten
// as one-invocation subgroups (single_lane); a size the library is right for that the device may vary and lets a pipeline pin (subgroupSizeControl, the stage in
// requiredSubgroupSizeStages) = pinned to it (VkPipelineShaderStageRequiredSubgroupSizeCreateInfo, vka_pso.c); the device's fixed size right for the library = as built;
// otherwise (a wrong or varying size that cannot be pinned) = one-invocation subgroups too. Mesa Turnip on the Adreno 840 reports 128 lanes (64-128) with fragment and
// compute pinnable: before this, every stage ran the 64-lane modules at up to 128 lanes (black cave walls in DKB); now fragment and compute are pinned to 64 and the
// vertex, tessellation and geometry stages run one-lane subgroups, as on Qualcomm's driver (which has none in those stages). DM_VK_SUBGROUP_PIN (restart): 0 = no pinning,
// 1 = every stage the device lets pin (default), any other value = a VkShaderStageFlags mask of the stages that may pin (0x30: fragment and compute)
static uint32_t g_sg_pin[8]; static uint8_t g_sg_one[8];   // by DMSH_VK_STAGE_*
static const uint32_t g_stage_bit[8] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, VK_SHADER_STAGE_GEOMETRY_BIT,
                                        VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_COMPUTE_BIT, VK_SHADER_STAGE_TASK_BIT_EXT, VK_SHADER_STAGE_MESH_BIT_EXT};
static void sg_plan(unsigned sg) {
    static const char *const nm[8] = {"vertex", "tess control", "tess evaluation", "geometry", "fragment", "compute", "task", "mesh"};
#define SG_OK(n) (sg == 64 ? ((n) == 32 || (n) == 64) : (n) == sg)
    unsigned pin = 0; int pm = VKA_ENV("DM_VK_SUBGROUP_PIN", 1); uint32_t pin_stages = pm == 1 ? V.subgroup_req_stages : V.subgroup_req_stages & (uint32_t)pm;
    if (V.subgroup_min != V.subgroup_max && pin_stages)
        for (unsigned c = sg; c >= 32 && !pin; c /= 2) if (SG_OK(c) && c >= V.subgroup_min && c <= V.subgroup_max) pin = c;
    // Turnip's geometry stage computes subgroup operations wrong at a pinned 64 (T30, Mesa 26.3: DKB's terrain culling GS 5d537f7630ec3671 left the cave walls black; one-lane
    // subgroups there drew them right, with every other stage pinned). Eden leaves geometry out of Turnip's subgroup stages too. DM_VK_SUBGROUP_GS (restart): 0 = the geometry
    // stage runs one-lane subgroups, 1 = as the device reports; default 0 on VK_DRIVER_ID_MESA_TURNIP, 1 on other drivers
    uint32_t stages = V.subgroup_stages;
    if (!VKA_ENV("DM_VK_SUBGROUP_GS", V.driver_id != VK_DRIVER_ID_MESA_TURNIP)) stages &= ~(uint32_t)VK_SHADER_STAGE_GEOMETRY_BIT;
    char q[256]; size_t qn = 0; q[0] = 0; int one_new = 0;
    for (int i = 0; i < 8; i++) {
        if (i >= DMSH_VK_STAGE_TS && !V.mesh) continue;
        uint32_t b = g_stage_bit[i]; g_sg_pin[i] = 0; g_sg_one[i] = 0;
        if (!(stages & b)) g_sg_one[i] = 1;
        else if (pin && (pin_stages & b)) g_sg_pin[i] = pin;
        else if (!SG_OK(V.subgroup_size) || V.subgroup_min != V.subgroup_max) g_sg_one[i] = 1, one_new = 1;
        qn += (size_t)snprintf(q + qn, sizeof q - qn, "%s%s %s", i ? ", " : "", nm[i], g_sg_one[i] ? "one lane" : g_sg_pin[i] ? (pin == 64 ? "pinned 64" : "pinned 32") : "as built");
        if (qn >= sizeof q) qn = sizeof q - 1;
    }
#undef SG_OK
    VLOG("subgroup shaders (built for %u lanes; the device's %u, %u-%u, pinnable in %#x, pinned in %#x): %s", sg, V.subgroup_size, V.subgroup_min, V.subgroup_max, V.subgroup_req_stages, pin ? pin_stages : 0, q);
    if (one_new) VLOG("subgroup shaders: stages whose subgroup size is wrong for the library or varies unpinned run them as one-invocation subgroups");
}
uint32_t vka_shader_sg_pin(int stage) { return stage >= 0 && stage < 8 ? g_sg_pin[stage] : 0; }

// Compute shaders' subgroup shuffles through workgroup memory (the library's DMSH_VK_MOD_SHUFFLE_LDS modules, vk_android_spv.py shuffle_lds). Turnip has no
// arbitrary-index shuffle instruction: ir3 makes each OpGroupNonUniformShuffle a loop of rotates, one pass per distinct (index - lane) in the subgroup, and the translator's
// LDSM / MOVM gathers take up to 24 (agl's upscaler network in DKB, 14a8aaf3e0a9edbd: 10.7 ms a dispatch on the Adreno 840, 2.0 ms with the module through workgroup
// memory). Qualcomm's driver shuffles natively and runs the base module faster (1.7 ms against 1.9). DM_VK_SHUFFLE_LDS (restart): 1 = those modules, 0 = the base ones;
// default 1 on VK_DRIVER_ID_MESA_TURNIP, 0 on other drivers. Needs DMSH_VK_SHARED_MAX bytes of shared memory (the library keeps the modules within it)
static int g_shuffle_lds = -1;
static void shuffle_plan(void) {
    const char *ev = getenv("DM_VK_SHUFFLE_LDS");
    int want = ev && *ev ? atoi(ev) != 0 : V.driver_id == VK_DRIVER_ID_MESA_TURNIP;
    int fits = V.props.limits.maxComputeSharedMemorySize >= DMSH_VK_SHARED_MAX;
    g_shuffle_lds = want && fits;
    VLOG("compute subgroup shuffles: %s", g_shuffle_lds ? "through workgroup memory where the library has the module (DM_VK_SHUFFLE_LDS=0: the instruction)"
         : want ? "the instruction (the device's shared memory is under DMSH_VK_SHARED_MAX)" : "the instruction (DM_VK_SHUFFLE_LDS=1: through workgroup memory)");
}
VkShaderModule vka_shader_cs_module(vka_shader *s) {
    if (g_shuffle_lds < 0) shuffle_plan();
    VkShaderModule m = g_shuffle_lds && vka_shlib_module(s->lib, s->e, DMSH_VK_MOD_SHUFFLE_LDS) ? vka_shader_module(s, DMSH_VK_MOD_SHUFFLE_LDS) : VK_NULL_HANDLE;
    return m ? m : s->mods[DMSH_VK_MOD_BASE];   // (that module not made: logged by vka_shader_module; the base one)
}

const char *vka_shader_lib_path(void) { return g_lib_path; }
unsigned vka_shader_lib_differs(void) { return g_lib_state > 0 ? g_lib.differs : 0; }

static int lib_load(void) {
    if (g_lib_state) return g_lib_state > 0;
    g_lib_state = -1;
    char path[1024], err[256]; const char *e = getenv("DM_VK_SHLIB"); const char *dir = dm_module_dir(); dir = dir && *dir ? dir : ".";
    if (e && *e) snprintf(path, sizeof path, "%s", e);
    else snprintf(path, sizeof path, "%s/shaders/" DMSH_VK_DIR_PREFIX "%s/" DMSH_VK_LIB_FILE, dir, DMSH_VK_CHAIN);
    int have = !access(path, R_OK) && vka_shlib_open(&g_lib, path, DMSH_CHAIN, DMSH_VK_CHAIN, DM_GAME_ID, DMSH_VK_ABI, err, sizeof err);
    if (!have && !(e && *e)) {   // this version's library not in the pack (yet): the newest other v-<V>/ that this host reads the same (its ABI), else the newest of a compatible older ABI
        char why[256]; snprintf(why, sizeof why, "%s", access(path, R_OK) ? "missing" : err);
        char sd[1024], best[1024] = "", cbest[1024] = "", old[1024] = ""; time_t bt = 0, cbt = 0, ot = 0; snprintf(sd, sizeof sd, "%s/shaders", dir);
        DIR *d = opendir(sd); struct dirent *de;
        while (d && (de = readdir(d))) {
            if (strncmp(de->d_name, DMSH_VK_DIR_PREFIX, strlen(DMSH_VK_DIR_PREFIX))) continue;
            char p[1024]; struct stat st; vka_shlib t; char e2[256];
            if (snprintf(p, sizeof p, "%s/%s/" DMSH_VK_LIB_FILE, sd, de->d_name) >= (int)sizeof p || stat(p, &st) || !strcmp(p, path)) continue;
            if (vka_shlib_open(&t, p, DMSH_CHAIN, DMSH_VK_CHAIN, DM_GAME_ID, DMSH_VK_ABI, e2, sizeof e2)) {
                int compat = t.by_abi == 2; vka_shlib_close(&t);
                if (compat) { if (!*cbest || st.st_mtime > cbt) { snprintf(cbest, sizeof cbest, "%s", p); cbt = st.st_mtime; } }
                else if (!*best || st.st_mtime > bt) { snprintf(best, sizeof best, "%s", p); bt = st.st_mtime; }
                continue;
            }
            // A library made before the abi field (all zero): its ABI is not known, so it is the last resort, played with a warning rather than nothing drawn
            // (the user's choice: never block Play on it; the app suggests one more shader update, after which updates are deltas of the same ABI)
            int legacy = vka_shlib_open(&t, p, NULL, NULL, DM_GAME_ID, NULL, e2, sizeof e2);
            if (legacy) { static const char z[12]; legacy = !memcmp(t.h->abi, z, sizeof z); vka_shlib_close(&t); if (!legacy) snprintf(e2, sizeof e2, "another ABI than this host's %s", DMSH_VK_ABI); }
            if (!legacy) { VLOG("shader library %s not usable here: %s", p, e2); continue; }
            if (!*old || st.st_mtime > ot) { snprintf(old, sizeof old, "%s", p); ot = st.st_mtime; }
        }
        if (d) closedir(d);
        if (!*best && *cbest) snprintf(best, sizeof best, "%s", cbest);   // (an older ABI's library: after every one of the host's own ABI)
        if (*best && vka_shlib_open(&g_lib, best, DMSH_CHAIN, DMSH_VK_CHAIN, DM_GAME_ID, DMSH_VK_ABI, err, sizeof err)) {
            char v[17], a[13]; vka_shlib_str(g_lib.h->vk_chain, v); memcpy(a, g_lib.h->abi, 12); a[12] = 0;
            if (g_lib.by_abi == 2) VLOG("shader library %s: this host's version %s is %s (the app's shader update makes it); playing version %s, of the older ABI %s that this host's %s "
                                        "still plays (vka_abi_compat.h: differences %#x)", path, DMSH_VK_CHAIN, why, v, a, DMSH_VK_ABI, g_lib.differs);
            else VLOG("shader library %s: this host's version %s is %s (the app's shader update makes it); playing version %s, the same ABI %s", path, DMSH_VK_CHAIN, why, v, DMSH_VK_ABI);
            snprintf(path, sizeof path, "%s", best); have = 1;
        } else if (!*best && *old && vka_shlib_open(&g_lib, old, NULL, NULL, DM_GAME_ID, NULL, err, sizeof err)) {
            char v[17]; vka_shlib_str(g_lib.h->vk_chain, v);
            VRED("shader library %s: this host's version %s is %s; playing version %s, made before versioned shader updates (no ABI in its header, this host's is %s): "
                 "it may draw wrong. Update the shaders once in the app; later updates are then partial", path, DMSH_VK_CHAIN, why, v, DMSH_VK_ABI);
            snprintf(path, sizeof path, "%s", old); have = 1;
        } else { VRED("no shader library at %s (the pack's shaders/" DMSH_VK_DIR_PREFIX "%s: %s) and none of another version with this host's ABI %s: nothing is drawn", path, DMSH_VK_CHAIN, why, DMSH_VK_ABI); return 0; }
    }
    if (!have) { VRED("shader library %s refused: %s; nothing is drawn", path, access(path, R_OK) ? "missing" : err); return 0; }
    snprintf(g_lib_path, sizeof g_lib_path, "%s", path);
    // The translator's answers in the header against this device: modules built for 64-lane subgroups split ballots and shuffles into 32-lane halves, so they are right on
    // 32 or 64 lanes; storage buffers are bound rounded down to ssbo_align, which the device's alignment must divide (android/SHADERS.md "Translator answers")
    unsigned sg = g_lib.h->subgroup_size ? g_lib.h->subgroup_size : 32, sa = g_lib.h->ssbo_align;
    sg_plan(sg); shuffle_plan();
    if (sa && (sa % V.sb_align)) { VRED("shader library %s refused: built for storage buffers at %u-byte boundaries, the device needs %llu; nothing is drawn", path, sa, (unsigned long long)V.sb_align); vka_shlib_close(&g_lib); return 0; }
    char inputs[17]; vka_shlib_str(g_lib.h->inputs, inputs);
    VLOG("shader library %s: %u shaders, %u modules, %.1f MB (chain %s, Vulkan chain %s, inputs %s; %u-lane subgroups, storage buffers at %u)", path, g_lib.h->entry_count, g_lib.h->mod_count,
         g_lib.size / 1e6, DMSH_CHAIN, DMSH_VK_CHAIN, inputs, sg, sa ? sa : 1);
    g_lib_state = 1;
    const char *ck = getenv("DM_VK_SHLIB_CHECK");
    if (ck && atoi(ck)) vka_shlib_check_all(&g_lib);
    return 1;
}
// The library played as one number: FNV-1a of its header (chains, game, profile inputs, table sizes, blob size, ABI), which every rebuild or shader update changes. 0 = none
uint64_t vka_shader_lib_id(void) {
    if (!lib_load()) return 0;
    const uint8_t *p = (const uint8_t *)g_lib.h; uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < sizeof *g_lib.h; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h ? h : 1;
}

// ---------- Static memory: buffers that live as long as the process ----------
typedef struct { VkBuffer buf; VkDeviceMemory mem; uint8_t *ptr; VkDeviceSize size, used; } stblk;
static stblk g_st;
int vka_static(VkDeviceSize n, VkDeviceSize align, vka_span *out) {
    if (!align) align = 16;
    pthread_mutex_lock(&g_mu);
    VkDeviceSize o = (g_st.used + align - 1) / align * align;
    if (!g_st.buf || o + n > g_st.size) {
        stblk b = {0}; b.size = n > (4u << 20) ? (n + 0xFFFF) & ~(VkDeviceSize)0xFFFF : 4u << 20;
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = b.size, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT};
        VkMemoryRequirements mr; void *p = NULL; uint32_t t = UINT32_MAX;
        if (!vk.vkCreateBuffer(V.dev, &bi, NULL, &b.buf)) {
            vk.vkGetBufferMemoryRequirements(V.dev, b.buf, &mr);
            t = vka_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
            VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = t};
            if (t == UINT32_MAX || vk.vkAllocateMemory(V.dev, &ai, NULL, &b.mem) || vk.vkBindBufferMemory(V.dev, b.buf, b.mem, 0) || vk.vkMapMemory(V.dev, b.mem, 0, VK_WHOLE_SIZE, 0, &p)) p = NULL;
        }
        if (!p) { pthread_mutex_unlock(&g_mu); VRED("static block of %llu bytes not allocated", (unsigned long long)b.size); return 0; }
        b.ptr = p; g_st = b; o = 0;   // the old block stays (its buffers are still bound by shaders)
    }
    g_st.used = o + n; *out = (vka_span){g_st.buf, g_st.ptr + o, o};
    pthread_mutex_unlock(&g_mu);
    return 1;
}

// ---------- Texture pool reads: every module made reports the pool IDs it reads (vka_spv_pool_fb; vka_tex.c "Texture pool reads") ----------
// The hook sits over vkCreateShaderModule, so every module of the library and every variant made from it at pipeline time (vertex inputs, outputs, textures retyped,
// instanced, geometry claim and link, folded constants) carries the code; the replay's own modules read no pool and pass unchanged. A pixel module given
// EarlyFragmentTests (its stores would otherwise move its depth test after shading) gets a twin without them for the pipelines whose tests must stay late: alpha to
// coverage, or the alpha test on when its discard is the module's only one (vka_shader_pfb_pick, vka_pso.c). A module the rewrite refuses is used as it is: its pool
// reads report nothing (counted, logged)
static PFN_vkCreateShaderModule g_csm_next; static PFN_vkDestroyShaderModule g_dsm_next;
typedef struct { VkShaderModule early, late; int kind; } pfb_twin;
static pfb_twin *g_tw; static size_t g_ntw, g_twcap; static pthread_mutex_t g_twmu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t g_pfb_mods, g_pfb_sites, g_pfb_early, g_pfb_blind;
static VkResult VKAPI_CALL csm_pfb(VkDevice d, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a, VkShaderModule *m) {
    unsigned fl = vka_tex_pfb_flags();
    if (!fl || !ci || !ci->pCode || ci->codeSize < 20) return g_csm_next(d, ci, a, m);
    size_t on = 0; int sites = 0, kind = 0; char err[200] = "";
    uint32_t *o = vka_spv_pool_fb(ci->pCode, ci->codeSize / 4, DMSH_VK_SET_POOL, DMSH_VK_POOL_TEX_BINDING, VKA_POOL_FB_SET, VKA_POOL_FB_BINDING, fl, DMSH_VK_SPEC_ALPHA_FUNC, &on, &sites, &kind, err, sizeof err);
    if (!o) {
        if (err[0]) { uint64_t n = atomic_fetch_add(&g_pfb_blind, 1); if (n < 8) VRED("texture pool reads: a module's pool reads report nothing (%s): a texture it alone reads through the pool is not made", err); }
        return g_csm_next(d, ci, a, m);
    }
    VkShaderModuleCreateInfo c2 = *ci; c2.codeSize = on * 4; c2.pCode = o;
    VkResult r = g_csm_next(d, &c2, a, m);
    free(o);
    if (r) return r;
    uint64_t nm = atomic_fetch_add(&g_pfb_mods, 1) + 1; atomic_fetch_add(&g_pfb_sites, (uint64_t)sites);
    if (kind) {   // the twin without EarlyFragmentTests
        atomic_fetch_add(&g_pfb_early, 1);
        size_t ln = 0; int ls = 0, lk = 0; uint32_t *l = vka_spv_pool_fb(ci->pCode, ci->codeSize / 4, DMSH_VK_SET_POOL, DMSH_VK_POOL_TEX_BINDING, VKA_POOL_FB_SET, VKA_POOL_FB_BINDING, fl & ~(unsigned)VKA_PFB_EARLY,
                                                            DMSH_VK_SPEC_ALPHA_FUNC, &ln, &ls, &lk, err, sizeof err);
        VkShaderModule lm = VK_NULL_HANDLE;
        if (l) { c2.codeSize = ln * 4; c2.pCode = l; if (g_csm_next(d, &c2, a, &lm)) lm = VK_NULL_HANDLE; free(l); }
        pthread_mutex_lock(&g_twmu);
        if (lm && g_ntw == g_twcap) { size_t nc = g_twcap ? 2 * g_twcap : 256; pfb_twin *n = realloc(g_tw, nc * sizeof *n); if (n) { g_tw = n; g_twcap = nc; } }
        if (lm && g_ntw < g_twcap) { g_tw[g_ntw++] = (pfb_twin){*m, lm, kind}; lm = VK_NULL_HANDLE; }
        pthread_mutex_unlock(&g_twmu);
        if (lm || !l) {   // no twin: the module without the mode serves every pipeline (late tests: correct, slower)
            if (lm) g_dsm_next(d, lm, a);
            g_dsm_next(d, *m, a); *m = VK_NULL_HANDLE;
            uint32_t *w2 = vka_spv_pool_fb(ci->pCode, ci->codeSize / 4, DMSH_VK_SET_POOL, DMSH_VK_POOL_TEX_BINDING, VKA_POOL_FB_SET, VKA_POOL_FB_BINDING, fl & ~(unsigned)VKA_PFB_EARLY, DMSH_VK_SPEC_ALPHA_FUNC, &ln, &ls, &lk, err, sizeof err);
            if (!w2) return g_csm_next(d, ci, a, m);
            c2.codeSize = ln * 4; c2.pCode = w2; r = g_csm_next(d, &c2, a, m); free(w2);
        }
    }
    if (nm == 1 || !(nm & (nm - 1)))
        VLOG("texture pool reads: %llu modules report the pool IDs they read (%llu reads given the code, %llu pixel modules with early depth tests and a twin without, %llu refused; DM_VK_POOL_FB=0 off)",
             (unsigned long long)nm, (unsigned long long)atomic_load(&g_pfb_sites), (unsigned long long)atomic_load(&g_pfb_early), (unsigned long long)atomic_load(&g_pfb_blind));
    return r;
}
static void VKAPI_CALL dsm_pfb(VkDevice d, VkShaderModule m, const VkAllocationCallbacks *a) {
    VkShaderModule late = VK_NULL_HANDLE;
    if (m) { pthread_mutex_lock(&g_twmu);
             for (size_t i = 0; i < g_ntw; i++) if (g_tw[i].early == m) { late = g_tw[i].late; g_tw[i] = g_tw[--g_ntw]; break; }
             pthread_mutex_unlock(&g_twmu); }
    if (late) g_dsm_next(d, late, a);
    g_dsm_next(d, m, a);
}
void vka_shader_pfb_hook(void) {
    if (!vk.vkCreateShaderModule || vk.vkCreateShaderModule == csm_pfb || !vk.vkDestroyShaderModule) return;
    g_csm_next = vk.vkCreateShaderModule; vk.vkCreateShaderModule = csm_pfb;
    g_dsm_next = vk.vkDestroyShaderModule; vk.vkDestroyShaderModule = dsm_pfb;
}
VkShaderModule vka_shader_pfb_pick(VkShaderModule m, int a2c, int alpha_func) {
    if (!m) return m;
    VkShaderModule r = m;
    pthread_mutex_lock(&g_twmu);
    for (size_t i = 0; i < g_ntw; i++) if (g_tw[i].early == m) { if (a2c || (g_tw[i].kind == 2 && alpha_func)) r = g_tw[i].late; break; }
    pthread_mutex_unlock(&g_twmu);
    return r;
}

// ---------- Shaders by key ----------
typedef struct { uint64_t key; vka_shader *sh; const vka_shlib *lib; } shent;
static shent *g_sh; static size_t g_shcap, g_shn;
static size_t hslot(uint64_t k, size_t cap) { return (size_t)((k * 0x9E3779B97F4A7C15ull) >> 17) & (cap - 1); }
static shent *sh_find(uint64_t key, const vka_shlib *L) {
    if (!g_shcap) return NULL;
    for (size_t i = hslot(key, g_shcap);; i = (i + 1) & (g_shcap - 1)) { if (!g_sh[i].sh) return NULL; if (g_sh[i].key == key && g_sh[i].lib == L) return &g_sh[i]; }
}
static void sh_put(uint64_t key, const vka_shlib *L, vka_shader *s) {
    if (2 * (g_shn + 1) > g_shcap) {
        size_t nc = g_shcap ? 2 * g_shcap : 1024; shent *n = calloc(nc, sizeof *n);
        for (size_t i = 0; i < g_shcap; i++) if (g_sh[i].sh) { size_t j = hslot(g_sh[i].key, nc); while (n[j].sh) j = (j + 1) & (nc - 1); n[j] = g_sh[i]; }
        free(g_sh); g_sh = n; g_shcap = nc;
    }
    size_t j = hslot(key, g_shcap); while (g_sh[j].sh) j = (j + 1) & (g_shcap - 1);
    g_sh[j] = (shent){key, s, L}; g_shn++;
}

static const char *stage_name(int s) { static const char *n[] = {"vertex", "tess control", "tess evaluation", "geometry", "fragment", "compute", "task", "mesh"}; return s >= 0 && s < 8 ? n[s] : "?"; }
// What the replay cannot run yet, decided once per shader: refused shaders keep their entry, so the draws that use them are skipped and counted
static const char *refusal(const vka_shlib *L, const dmsh_vk_entry *e) {
    if (e->flags & (DMSH_VK_F_NV_PASSTHROUGH | DMSH_VK_F_NV_VIEWPORT)) return "NV-only geometry passthrough / viewport mask (a library built before the gs_passthrough answer)";
    if (e->flags & DMSH_VK_F_INTERLOCK) return "fragment shader interlock";
    if (e->stage == DMSH_VK_STAGE_GS && !V.geom) return "a geometry shader on a device without geometryShader";
    if ((e->stage == DMSH_VK_STAGE_TCS || e->stage == DMSH_VK_STAGE_TES) && !V.tess) return "a tessellation shader on a device without tessellationShader";
    // NVN2 task and mesh programs, and the pixel shaders that read their per-primitive outputs (valid only in a mesh pipeline): only with VK_EXT_mesh_shader (vka_dev.c)
    if ((e->stage == DMSH_VK_STAGE_TS || e->stage == DMSH_VK_STAGE_MS || (e->flags & DMSH_VK_F_MESH_PRIM)) && !V.mesh) return "a task / mesh shader (or a pixel shader of their per-primitive outputs) on a device without VK_EXT_mesh_shader (or DM_VK_MESH=0)";
    if (e->stage == DMSH_VK_STAGE_TS && !V.task) return "a task shader on a device with mesh shaders but without taskShader";
    if (e->stage == DMSH_VK_STAGE_TS || e->stage == DMSH_VK_STAGE_MS) {   // the workgroup (the entry's local) within the device's limit
        uint64_t inv = (uint64_t)e->local[0] * e->local[1] * e->local[2];
        if (!inv || inv > (e->stage == DMSH_VK_STAGE_TS ? V.task_inv : V.mesh_inv)) return "a task / mesh workgroup larger than the device's (maxTaskWorkGroupInvocations / maxMeshWorkGroupInvocations)";
    }
    const dmsh_vk_bind *b = vka_shlib_binds(L, e);
    for (unsigned i = 0; i < e->bind_count; i++) if (b[i].kind == DMSH_VK_KIND_UNKNOWN) return "a binding the build could not place";
    return NULL;
}
// A module of a stage sg_plan runs as one lane (no subgroup operations there on this device, or a subgroup size the library is not right for that cannot be pinned) has them
// rewritten as subgroups of one invocation (vka_spv_subgroup_single; Qualcomm's Adreno 840 driver has them in fragment, compute, task and mesh shaders, not in the vertex pipeline's). Logged once per shader key; a module that cannot be rewritten is used as it is, with a red line
static void single_lane(uint64_t key, int stage, uint32_t **w, size_t *nb) {
    if (stage < 0 || stage > 7 || !g_sg_one[stage]) return;   // sg_plan
    // A task or mesh workgroup uses its subgroup operations across the workgroup (output counts, compaction): one-lane subgroups give it wrong results. The Adreno 840's
    // driver has them in both stages at its fixed 64 (subgroupSupportedStages 0xf0), so this is a device the library was not made for
    if (stage >= DMSH_VK_STAGE_TS) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VRED("%s shader %016llx: its subgroup operations run as one lane (the device's subgroups in this stage do not fit the library): its output is likely wrong", stage == DMSH_VK_STAGE_TS ? "task" : "mesh", (unsigned long long)key); }
    size_t on = 0; int ch = 0; char e[256] = ""; uint32_t *o = vka_spv_subgroup_single(*w, *nb / 4, &on, &ch, e, sizeof e);
    if (!o) { if (e[0]) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VRED("shader %016llx: subgroup operations in a stage without them not rewritten: %s", (unsigned long long)key, e); } return; }
    static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VLOG("shader %016llx: subgroup operations rewritten as one-invocation subgroups (%s)", (unsigned long long)key,
                                                            stage == DMSH_VK_STAGE_GS && V.driver_id == VK_DRIVER_ID_MESA_TURNIP ? "Turnip's geometry stage (DM_VK_SUBGROUP_GS)" : V.subgroup_stages & g_stage_bit[stage] ? "the device's subgroup size in this stage is not the library's and cannot be pinned" : "the device has none in this stage");
    free(*w); *w = o; *nb = on * 4;
}
static VkShaderModule spv_module_(const vka_shlib *L, const dmsh_vk_entry *e, const dmsh_vk_module *m, uint64_t key, const char **why, char *err, size_t errn, uint8_t *fcomp) {
    size_t nb = 0; uint32_t *w = vka_shlib_spirv(L, m, &nb, err, errn);
    if (!w) { *why = err; return VK_NULL_HANDLE; }
    if (fcomp && vka_spv_fout_comps(w, nb / 4, fcomp)) memset(fcomp, 0xf, 8);   // a pixel shader's output components (the colour write mask: vka_draw.c)
    if (e->flags & DMSH_VK_F_SUBGROUP) single_lane(key, e->stage, &w, &nb);
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = nb, .pCode = w};
    VkShaderModule mod = VK_NULL_HANDLE; VkResult r = vk.vkCreateShaderModule(V.dev, &ci, NULL, &mod);
    free(w);
    if (r) { snprintf(err, errn, "vkCreateShaderModule %d (variant %u of %016llx)", r, m->variant, (unsigned long long)key); *why = err; return VK_NULL_HANDLE; }
    return mod;
}
static VkShaderModule spv_module(const vka_shlib *L, const dmsh_vk_entry *e, const dmsh_vk_module *m, uint64_t key, const char **why, char *err, size_t errn) { return spv_module_(L, e, m, key, why, err, errn, NULL); }
// An instanced variant (vka_draw.c's instanced runs): the base module with the uniform block at `binding` read at data[i + d] (vka_spv_instance), made from the
// library's SPIR-V; the caller caches it. VK_NULL_HANDLE (the reason logged a few times) when the shader cannot take it
VkShaderModule vka_shader_inst_module(vka_shader *s, uint32_t binding, uint32_t loc) {
    char err[256]; const dmsh_vk_module *m = vka_shlib_module(s->lib, s->e, DMSH_VK_MOD_BASE);
    size_t nb = 0; uint32_t *w = m ? vka_shlib_spirv(s->lib, m, &nb, err, sizeof err) : NULL;
    if (!w) return VK_NULL_HANDLE;
    if (s->e->flags & DMSH_VK_F_SUBGROUP) single_lane(s->key, s->e->stage, &w, &nb);
    size_t on = 0; uint32_t *o = vka_spv_instance(w, nb / 4, binding, loc, &on, err, sizeof err);
    free(w);
    if (!o) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 16) VLOG("instanced runs: shader %016llx (binding %u) takes no variant: %s", (unsigned long long)s->key, binding, err); return VK_NULL_HANDLE; }
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = on * 4, .pCode = o};
    VkShaderModule mod = VK_NULL_HANDLE; VkResult r = vk.vkCreateShaderModule(V.dev, &ci, NULL, &mod);
    free(o);
    if (r) { VRED("instanced runs: variant of %016llx (binding %u): vkCreateShaderModule %d", (unsigned long long)s->key, binding, r); return VK_NULL_HANDLE; }
    return mod;
}
VkShaderModule vka_shader_module(vka_shader *s, int variant) {
    if (variant < 0 || variant > DMSH_VK_MOD_SHUFFLE_LDS) return VK_NULL_HANDLE;
    if (s->mods[variant]) return s->mods[variant];
    const dmsh_vk_module *m = vka_shlib_module(s->lib, s->e, variant);
    if (!m) return VK_NULL_HANDLE;
    const char *why = NULL; char err[256];
    VkShaderModule mod = spv_module(s->lib, s->e, m, s->key, &why, err, sizeof err);
    if (!mod) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VRED("shader %016llx variant %d: %s", (unsigned long long)s->key, variant, why); return VK_NULL_HANDLE; }
    pthread_mutex_lock(&g_mu);
    if (s->mods[variant]) { pthread_mutex_unlock(&g_mu); vk.vkDestroyShaderModule(V.dev, mod, NULL); return s->mods[variant]; }
    s->mods[variant] = mod;
    pthread_mutex_unlock(&g_mu);
    return mod;
}
// A geometry shader that writes memory, with those writes made by one run per primitive (vka_spv_gs_claim: the Adreno 840 runs a geometry shader several times for each
// primitive, each run writing memory). out NULL: only counts them. Made from the base module after the one-invocation subgroup rewrite
static int gs_words(vka_shader *s, uint32_t **out, size_t *on, char *err, size_t errn) {
    const dmsh_vk_module *m = vka_shlib_module(s->lib, s->e, DMSH_VK_MOD_BASE);
    size_t nb = 0; uint32_t *w = m ? vka_shlib_spirv(s->lib, m, &nb, err, errn) : NULL;
    if (!w) return -1;
    if (s->e->flags & DMSH_VK_F_SUBGROUP) single_lane(s->key, s->e->stage, &w, &nb);
    int r = vka_spv_gs_claim(w, nb / 4, VKA_GS_CLAIM_BINDING, out, on, err, errn);
    free(w); return r;
}
VkShaderModule vka_shader_gs_module(vka_shader *s) {
    if (s->gs_fx <= 0) return VK_NULL_HANDLE;
    if (s->gs_claim) return s->gs_claim;
    uint32_t *o = NULL; size_t on = 0; char err[256] = ""; VkShaderModule mod = VK_NULL_HANDLE;
    if (gs_words(s, &o, &on, err, sizeof err) > 0 && o) {
        VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = on * 4, .pCode = o};
        VkResult r = vk.vkCreateShaderModule(V.dev, &ci, NULL, &mod); if (r) { snprintf(err, sizeof err, "vkCreateShaderModule %d", r); mod = VK_NULL_HANDLE; }
    }
    free(o);
    if (!mod) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VRED("geometry shader %016llx: module with the claim not made: %s", (unsigned long long)s->key, err); return VK_NULL_HANDLE; }
    pthread_mutex_lock(&g_mu);
    if (s->gs_claim) { pthread_mutex_unlock(&g_mu); vk.vkDestroyShaderModule(V.dev, mod, NULL); return s->gs_claim; }
    s->gs_claim = mod;
    pthread_mutex_unlock(&g_mu);
    return mod;
}
// A task or mesh shader's base module with its specialization constants made plain constants, the GL depth one (DMSH_VK_SPEC_GL_DEPTH) set to gl, when it reads a subgroup
// lane built-in (vka_spv_spec_freeze: Qualcomm's driver reads the lane as 0 in a compute or mesh module with any specialization constant, and the translated mesh code takes
// its vertex and primitive slots from the lane). Made once per value; VK_NULL_HANDLE: the module needs none, its pipelines take the base module and VkSpecializationInfo
// On by default (DM_VK_SPEC_FOLD=0 turns it off). Right lanes once made the plane stores of the translator's mesh code (a chain of per-component stores into the output
// arrays at a lane-derived vertex) run Qualcomm's compiler out of memory; the translator now stores them into a workgroup copy written to the outputs once (docs/reference/shaders.md,
// "Mesh plane stores into outputs exhaust Qualcomm's compiler"), which needs a library of that chain
VkShaderModule vka_shader_spec_module(vka_shader *s, int gl) {
    if (!VKA_ENV("DM_VK_SPEC_FOLD", 1)) return VK_NULL_HANDLE;
    gl = gl != 0;
    pthread_mutex_lock(&g_mu); VkShaderModule have = s->frz[gl]; int none = s->frz_none; pthread_mutex_unlock(&g_mu);
    if (have || none) return have;
    char err[256] = ""; VkShaderModule mod = VK_NULL_HANDLE;
    const dmsh_vk_module *m = vka_shlib_module(s->lib, s->e, DMSH_VK_MOD_BASE);
    size_t nb = 0; uint32_t *w = m ? vka_shlib_spirv(s->lib, m, &nb, err, sizeof err) : NULL;
    if (w && (s->e->flags & DMSH_VK_F_SUBGROUP)) single_lane(s->key, s->e->stage, &w, &nb);
    size_t on = 0; uint32_t *o = w ? vka_spv_spec_freeze(w, nb / 4, DMSH_VK_SPEC_GL_DEPTH, (uint32_t)gl, &on) : NULL;
    free(w);
    if (o) { VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = on * 4, .pCode = o};
             VkResult r = vk.vkCreateShaderModule(V.dev, &ci, NULL, &mod); free(o);
             if (r) { mod = VK_NULL_HANDLE; static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VRED("%s shader %016llx: module with its specialization constants folded not made (vkCreateShaderModule %d): its subgroup lanes read 0 on Qualcomm's driver", stage_name(s->e->stage), (unsigned long long)s->key, r); }
             else { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 4) VLOG("%s shader %016llx: specialization constants folded into the module (GL depth %d), so its subgroup lanes read right", stage_name(s->e->stage), (unsigned long long)s->key, gl); } }
    pthread_mutex_lock(&g_mu);
    if (!mod) s->frz_none = 1;
    else if (s->frz[gl]) { VkShaderModule had = s->frz[gl]; pthread_mutex_unlock(&g_mu); vk.vkDestroyShaderModule(V.dev, mod, NULL); return had; }
    else s->frz[gl] = mod;
    pthread_mutex_unlock(&g_mu);
    return mod;
}

// A geometry shader's module for one vertex / pixel pair: outputs added for the pixel inputs it does not write (vka_spv_gs_link: the passthrough copies a geometry shader
// translated alone lacks; Qualcomm's driver refuses such a pipeline with -13). Made once per pair (and claim choice), kept for the process. VK_NULL_HANDLE: nothing to add, or
// not made (logged): the caller's module as before
struct vka_gslink { struct vka_gslink *next; uint64_t vs, fs; int claim; VkShaderModule mod; };
static uint32_t *base_words(vka_shader *s, size_t *nb, char *err, size_t errn) {
    const dmsh_vk_module *m = vka_shlib_module(s->lib, s->e, DMSH_VK_MOD_BASE);
    return m ? vka_shlib_spirv(s->lib, m, nb, err, errn) : NULL;
}
VkShaderModule vka_shader_gs_link_module(vka_shader *gs, vka_shader *vs, vka_shader *fs, int claim) {
    if (!gs || !vs || !fs || !VKA_ENV("DM_VK_GS_LINK", 1)) return VK_NULL_HANDLE;
    pthread_mutex_lock(&g_mu);
    for (struct vka_gslink *l = gs->gslink; l; l = l->next) if (l->vs == vs->key && l->fs == fs->key && l->claim == claim) { VkShaderModule m = l->mod; pthread_mutex_unlock(&g_mu); return m; }
    pthread_mutex_unlock(&g_mu);
    char err[256] = ""; size_t gn = 0, vn = 0, fn = 0, on = 0; uint32_t *g = NULL, *o = NULL, copied = 0, nulled = 0; VkShaderModule mod = VK_NULL_HANDLE;
    if (claim) { if (gs_words(gs, &g, &gn, err, sizeof err) <= 0) { free(g); g = NULL; } }   // (gn in words)
    else if ((g = base_words(gs, &gn, err, sizeof err))) { if (gs->e->flags & DMSH_VK_F_SUBGROUP) single_lane(gs->key, gs->e->stage, &g, &gn); gn /= 4; }
    uint32_t *v = g ? base_words(vs, &vn, err, sizeof err) : NULL, *f = v ? base_words(fs, &fn, err, sizeof err) : NULL;
    if (f) o = vka_spv_gs_link(g, gn, v, vn / 4, f, fn / 4, &copied, &nulled, &on, err, sizeof err);
    if (o) { VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = on * 4, .pCode = o};
             VkResult r = vk.vkCreateShaderModule(V.dev, &ci, NULL, &mod); if (r) { snprintf(err, sizeof err, "vkCreateShaderModule %d", r); mod = VK_NULL_HANDLE; } }
    free(g); free(v); free(f); free(o);
    if (mod) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 16) VLOG("geometry shader %016llx: outputs added for pixel shader %016llx's inputs it does not write: locations %#x copied from vertex shader %016llx, %#x null (vka_spv_gs_link)",
                                                                       (unsigned long long)gs->key, (unsigned long long)fs->key, copied, (unsigned long long)vs->key, nulled); }
    else if (err[0]) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VRED("geometry shader %016llx with pixel shader %016llx: inputs it does not write not added (%s); a driver may refuse the pipeline", (unsigned long long)gs->key, (unsigned long long)fs->key, err); }
    pthread_mutex_lock(&g_mu);
    for (struct vka_gslink *l = gs->gslink; l; l = l->next) if (l->vs == vs->key && l->fs == fs->key && l->claim == claim) {
        VkShaderModule had = l->mod; pthread_mutex_unlock(&g_mu); if (mod && mod != had) vk.vkDestroyShaderModule(V.dev, mod, NULL); return had; }
    struct vka_gslink *l = malloc(sizeof *l);
    if (l) { *l = (struct vka_gslink){gs->gslink, vs->key, fs->key, claim, mod}; gs->gslink = l; }
    pthread_mutex_unlock(&g_mu);
    return mod;
}
// The base vertex module with the inputs of umask / smask retyped (vka_spvpatch.c): made once per pair of masks, kept for the process like the base module
struct vka_vinmod { struct vka_vinmod *next; uint32_t u, s; VkShaderModule mod; };
VkShaderModule vka_shader_vin_module(vka_shader *s, uint32_t umask, uint32_t smask) {
    pthread_mutex_lock(&g_mu);
    for (struct vka_vinmod *v = s->vin; v; v = v->next) if (v->u == umask && v->s == smask) { VkShaderModule m = v->mod; pthread_mutex_unlock(&g_mu); return m; }
    pthread_mutex_unlock(&g_mu);
    VkShaderModule mod = VK_NULL_HANDLE; char err[256] = ""; int own = 0;   // own: made here (the base module of the fallback is the shader's, never destroyed)
    const dmsh_vk_module *m = vka_shlib_module(s->lib, s->e, DMSH_VK_MOD_BASE);
    size_t nb = 0; uint32_t *w = m ? vka_shlib_spirv(s->lib, m, &nb, err, sizeof err) : NULL;
    if (w && (s->e->flags & DMSH_VK_F_SUBGROUP)) single_lane(s->key, s->e->stage, &w, &nb);
    size_t on = 0; uint32_t *o = w ? vka_spv_vin_retype(w, nb / 4, umask, smask, &on, err, sizeof err) : NULL;
    if (o) { VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = on * 4, .pCode = o};
             VkResult r = vk.vkCreateShaderModule(V.dev, &ci, NULL, &mod); if (r) { snprintf(err, sizeof err, "vkCreateShaderModule %d", r); mod = VK_NULL_HANDLE; } else own = 1; }
    free(w); free(o);
    if (!mod) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VRED("vertex shader %016llx: inputs %#x (uint) / %#x (int) not retyped: %s; the base module reads them as floats", (unsigned long long)s->key, umask, smask, err);
                mod = vka_shader_module(s, DMSH_VK_MOD_BASE); }
    // Two threads (the frame path and a prewarm or optimizer thread) may have made it at once: the first one in the list wins, the other's module is destroyed
    pthread_mutex_lock(&g_mu);
    for (struct vka_vinmod *o = s->vin; o; o = o->next) if (o->u == umask && o->s == smask) {
        VkShaderModule w = o->mod; pthread_mutex_unlock(&g_mu);
        if (own && mod != w) vk.vkDestroyShaderModule(V.dev, mod, NULL);   // (no pipeline was made from it: it never left this function)
        return w; }
    struct vka_vinmod *v = malloc(sizeof *v); *v = (struct vka_vinmod){s->vin, umask, smask, mod}; s->vin = v;
    pthread_mutex_unlock(&g_mu);
    return mod;
}
// The base pixel module with the float outputs of umask / smask retyped for integer attachments (vka_spvpatch.c vka_spv_fout_retype): made once per pair of masks, as the vertex ones
VkShaderModule vka_shader_fout_module(vka_shader *s, uint32_t umask, uint32_t smask) {
    pthread_mutex_lock(&g_mu);
    for (struct vka_vinmod *v = s->fout; v; v = v->next) if (v->u == umask && v->s == smask) { VkShaderModule m = v->mod; pthread_mutex_unlock(&g_mu); return m; }
    pthread_mutex_unlock(&g_mu);
    VkShaderModule mod = VK_NULL_HANDLE; char err[256] = ""; int own = 0;
    const dmsh_vk_module *m = vka_shlib_module(s->lib, s->e, DMSH_VK_MOD_BASE);
    size_t nb = 0; uint32_t *w = m ? vka_shlib_spirv(s->lib, m, &nb, err, sizeof err) : NULL;
    if (w && (s->e->flags & DMSH_VK_F_SUBGROUP)) single_lane(s->key, s->e->stage, &w, &nb);
    size_t on = 0; uint32_t *o = w ? vka_spv_fout_retype(w, nb / 4, umask, smask, &on, err, sizeof err) : NULL;
    if (o) { VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = on * 4, .pCode = o};
             VkResult r = vk.vkCreateShaderModule(V.dev, &ci, NULL, &mod); if (r) { snprintf(err, sizeof err, "vkCreateShaderModule %d", r); mod = VK_NULL_HANDLE; } else own = 1; }
    free(w); free(o);
    if (!mod) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VRED("pixel shader %016llx: outputs %#x (uint) / %#x (int) not retyped for their integer targets: %s; the base module, whose float outputs those targets do not take", (unsigned long long)s->key, umask, smask, err);
                mod = vka_shader_module(s, DMSH_VK_MOD_BASE); }
    else { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VLOG("pixel shader %016llx: float outputs %#x (uint) / %#x (int) retyped for their integer targets (their bits written as they are)", (unsigned long long)s->key, umask, smask); }
    pthread_mutex_lock(&g_mu);
    for (struct vka_vinmod *x = s->fout; x; x = x->next) if (x->u == umask && x->s == smask) {
        VkShaderModule had = x->mod; pthread_mutex_unlock(&g_mu);
        if (own && mod != had) vk.vkDestroyShaderModule(V.dev, mod, NULL);
        return had; }
    struct vka_vinmod *v = malloc(sizeof *v);
    if (v) { *v = (struct vka_vinmod){s->fout, umask, smask, mod}; s->fout = v; }
    pthread_mutex_unlock(&g_mu);
    return mod;
}
// A compute module with the float storage images at bindings of umask / smask retyped as uint / int images (vka_spvpatch.c vka_spv_img_retype), for integer textures bound
// there: made once per pair of masks from the module the pipeline would use (vka_shader_cs_module's choice). VK_NULL_HANDLE when it cannot be made (logged): the caller
// keeps the base pipeline, whose bound views read the bits as floats (vka_tex.c class_twin)
struct vka_imgmod { struct vka_imgmod *next; uint64_t u, s; VkShaderModule mod; };
VkShaderModule vka_shader_img_module(vka_shader *s, uint64_t umask, uint64_t smask) {
    pthread_mutex_lock(&g_mu);
    for (struct vka_imgmod *v = s->img; v; v = v->next) if (v->u == umask && v->s == smask) { VkShaderModule m = v->mod; pthread_mutex_unlock(&g_mu); return m; }
    pthread_mutex_unlock(&g_mu);
    VkShaderModule mod = VK_NULL_HANDLE; char err[256] = "";
    if (g_shuffle_lds < 0) shuffle_plan();
    int var = s->e->stage == DMSH_VK_STAGE_CS && g_shuffle_lds && vka_shlib_module(s->lib, s->e, DMSH_VK_MOD_SHUFFLE_LDS) ? DMSH_VK_MOD_SHUFFLE_LDS : DMSH_VK_MOD_BASE;
    const dmsh_vk_module *m = vka_shlib_module(s->lib, s->e, var);
    size_t nb = 0; uint32_t *w = m ? vka_shlib_spirv(s->lib, m, &nb, err, sizeof err) : NULL;
    if (w && (s->e->flags & DMSH_VK_F_SUBGROUP)) single_lane(s->key, s->e->stage, &w, &nb);
    size_t on = 0; uint32_t *o = w ? vka_spv_img_retype(w, nb / 4, 0, umask, smask, &on, err, sizeof err) : NULL;   // (compute: stage 0's binding range)
    if (o) { VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = on * 4, .pCode = o};
             VkResult r = vk.vkCreateShaderModule(V.dev, &ci, NULL, &mod); if (r) { snprintf(err, sizeof err, "vkCreateShaderModule %d", r); mod = VK_NULL_HANDLE; } }
    free(w); free(o);
    { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 16) {
        if (mod) VLOG("compute shader %016llx: images and textures at bindings %#llx (uint) / %#llx (int) retyped for the integer textures bound there (their bits read and written as they are)",
                      (unsigned long long)s->key, (unsigned long long)umask, (unsigned long long)smask);
        else VRED("compute shader %016llx: images and textures at bindings %#llx (uint) / %#llx (int) not retyped for their integer textures: %s; the base module, which reads them as floats",
                  (unsigned long long)s->key, (unsigned long long)umask, (unsigned long long)smask, err); } }
    pthread_mutex_lock(&g_mu);
    for (struct vka_imgmod *x = s->img; x; x = x->next) if (x->u == umask && x->s == smask) {
        VkShaderModule had = x->mod; pthread_mutex_unlock(&g_mu);
        if (mod && mod != had) vk.vkDestroyShaderModule(V.dev, mod, NULL);
        return had; }
    struct vka_imgmod *v = malloc(sizeof *v);
    if (v) { *v = (struct vka_imgmod){s->img, umask, smask, mod}; s->img = v; }
    pthread_mutex_unlock(&g_mu);
    return mod;
}
// A graphics stage's module with its float textures and images at the bindings of tex_u / tex_s (of its binding range) retyped as uint / int (vka_spv_img_retype), for
// integer texture views bound there (an R16G16_UINT, R32_UINT or RGBA8_UINT view: the hardware hands the shader their integer bits, and a float view of the same size,
// class_twin, converts them). Made on top of what the pipeline would use: the library variant, the vertex input or pixel output retype, the folded specialization constants.
// Made once per key; VK_NULL_HANDLE when it cannot be made (logged once per shader): the caller keeps the module without it
struct vka_texmod { struct vka_texmod *next; vka_texmod_key k; VkShaderModule mod; };
int vka_shader_tex_range(const vka_shader *s) {
    int st = s->e->stage;
    return st == DMSH_VK_STAGE_TS ? DMSH_VK_STAGE_TCS : st == DMSH_VK_STAGE_MS || st == DMSH_VK_STAGE_CS ? DMSH_VK_STAGE_VS : st;
}
VkShaderModule vka_shader_tex_module(vka_shader *s, const vka_texmod_key *key) {
    vka_texmod_key k; memset(&k, 0, sizeof k); k.variant = key->variant; k.vin_u = key->vin_u; k.vin_s = key->vin_s; k.fout_u = key->fout_u; k.fout_s = key->fout_s;
    k.frz = key->frz; k.tex_u = key->tex_u; k.tex_s = key->tex_s;
    pthread_mutex_lock(&g_mu);
    for (struct vka_texmod *v = s->tex; v; v = v->next) if (!memcmp(&v->k, &k, sizeof k)) { VkShaderModule m = v->mod; pthread_mutex_unlock(&g_mu); return m; }
    pthread_mutex_unlock(&g_mu);
    VkShaderModule mod = VK_NULL_HANDLE; char err[256] = "";
    const dmsh_vk_module *m = vka_shlib_module(s->lib, s->e, k.variant);
    size_t nb = 0; uint32_t *w = m ? vka_shlib_spirv(s->lib, m, &nb, err, sizeof err) : NULL;
    if (!m) snprintf(err, sizeof err, "no module of variant %d", k.variant);
    if (w && (s->e->flags & DMSH_VK_F_SUBGROUP)) single_lane(s->key, s->e->stage, &w, &nb);
    size_t on = 0; uint32_t *o;
    if (w && (k.vin_u | k.vin_s)) { o = vka_spv_vin_retype(w, nb / 4, k.vin_u, k.vin_s, &on, err, sizeof err); free(w); w = o; nb = on * 4; }
    if (w && (k.fout_u | k.fout_s)) { o = vka_spv_fout_retype(w, nb / 4, k.fout_u, k.fout_s, &on, err, sizeof err); free(w); w = o; nb = on * 4; }
    if (w && k.frz >= 0) { o = vka_spv_spec_freeze(w, nb / 4, DMSH_VK_SPEC_GL_DEPTH, (uint32_t)k.frz, &on); if (o) { free(w); w = o; nb = on * 4; } }
    o = w ? vka_spv_img_retype(w, nb / 4, (uint32_t)vka_shader_tex_range(s) * DMSH_VK_STAGE_BINDINGS, k.tex_u, k.tex_s, &on, err, sizeof err) : NULL;
    if (o) { VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = on * 4, .pCode = o};
             VkResult r = vk.vkCreateShaderModule(V.dev, &ci, NULL, &mod); if (r) { snprintf(err, sizeof err, "vkCreateShaderModule %d", r); mod = VK_NULL_HANDLE; } }
    free(w); free(o);
    { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 32) {
        if (mod) VLOG("%s shader %016llx: textures at bindings %#llx (uint) / %#llx (int) of its range retyped for the integer texture views bound there (variant %d; their bits read as they are)",
                      stage_name(s->e->stage), (unsigned long long)s->key, (unsigned long long)k.tex_u, (unsigned long long)k.tex_s, k.variant);
        else VRED("%s shader %016llx: textures at bindings %#llx (uint) / %#llx (int) not retyped for their integer texture views: %s; read through float views of the same size, which convert them",
                  stage_name(s->e->stage), (unsigned long long)s->key, (unsigned long long)k.tex_u, (unsigned long long)k.tex_s, err); } }
    pthread_mutex_lock(&g_mu);
    for (struct vka_texmod *x = s->tex; x; x = x->next) if (!memcmp(&x->k, &k, sizeof k)) {
        VkShaderModule had = x->mod; pthread_mutex_unlock(&g_mu);
        if (mod && mod != had) vk.vkDestroyShaderModule(V.dev, mod, NULL);
        return had; }
    struct vka_texmod *v = malloc(sizeof *v);
    if (v) { v->next = s->tex; v->k = k; v->mod = mod; s->tex = v; }
    pthread_mutex_unlock(&g_mu);
    return mod;
}
int vka_shader_fout_retyped(vka_shader *s, uint32_t umask, uint32_t smask) {
    VkShaderModule m = vka_shader_fout_module(s, umask, smask);
    return m && m != vka_shader_module(s, DMSH_VK_MOD_BASE);
}
vka_shader *vka_shader_make(const vka_shlib *L, uint64_t key) {
    pthread_mutex_lock(&g_mu);
    shent *h = sh_find(key, L);
    if (h) { vka_shader *s = h->sh; pthread_mutex_unlock(&g_mu); return s->bad ? NULL : s; }
    pthread_mutex_unlock(&g_mu);
    vka_shader *s = calloc(1, sizeof *s); s->key = key; s->lib = L;
    const dmsh_vk_entry *e = vka_shlib_find(L, key);
    const char *why = NULL; char err[256] = "";
    if (!e) why = "not in the shader library";
    else {
        s->e = e; s->b = vka_shlib_binds(L, e); s->chash = vka_shlib_entry_hash(L, e);
        why = refusal(L, e);
        const dmsh_vk_module *m = why ? NULL : vka_shlib_module(L, e, DMSH_VK_MOD_BASE);
        if (!why && !m) why = "no base module";
        memset(s->fcomp, 0xf, sizeof s->fcomp);
        if (m) s->mods[DMSH_VK_MOD_BASE] = spv_module_(L, e, m, key, &why, err, sizeof err, e->stage == DMSH_VK_STAGE_FS && VKA_ENV("DM_VK_FOUT_COMPS", 1) ? s->fcomp : NULL);
        if (!why && e->stage == DMSH_VK_STAGE_GS && VKA_ENV("DM_VK_GS_CLAIM", 1)) {   // memory writes: made once per primitive (vka_spv_gs_claim)
            char e2[256] = ""; s->gs_fx = gs_words(s, NULL, NULL, e2, sizeof e2);
            if (s->gs_fx < 0) VRED("geometry shader %016llx: memory writes not put under a claim (%s): drawn as it is, its writes repeated by the device's runs of each primitive", (unsigned long long)key, e2);
            else if (s->gs_fx) VLOG("geometry shader %016llx: %d memory writes, made by one run per primitive (a claim word per primitive at set 0 binding %d)", (unsigned long long)key, s->gs_fx, VKA_GS_CLAIM_BINDING);
        }
        if (!why && e->c1_len) {   // the c1 bytes: a uniform buffer of their own, made once
            if (vka_static(e->c1_len, V.ub_align, &s->c1)) { memcpy(s->c1.ptr, vka_shlib_c1(L, e), e->c1_len); s->c1_len = e->c1_len; }
            else why = "no static memory for its c1 bytes";
        }
    }
    if (why) {
        s->bad = 1;
        static _Atomic int n; if (atomic_fetch_add(&n, 1) < 64) VRED("shader %016llx (%s): %s; its draws are skipped", (unsigned long long)key, e ? stage_name(e->stage) : "?", why);
    }
    pthread_mutex_lock(&g_mu);
    h = sh_find(key, L);
    if (h) { if (s->mods[0]) vk.vkDestroyShaderModule(V.dev, s->mods[0], NULL); free(s); s = h->sh; }   // another thread made it meanwhile
    else sh_put(key, L, s);
    pthread_mutex_unlock(&g_mu);
    return s->bad ? NULL : s;
}

// A shader of the game's library by key, the library loaded if it is not yet (the prewarm threads, vka_pso.c); NULL = no library, not in it, or refused
int vka_shader_variant_key(uint64_t key) {
    pthread_mutex_lock(&g_mu); int ok = lib_load(); pthread_mutex_unlock(&g_mu);
    return ok && !vka_shlib_find(&g_lib, key) && vka_shlib_find(&g_lib, key ^ VKA_INST_KEY_XOR);
}
vka_shader *vka_shader_by_key(uint64_t key) {
    pthread_mutex_lock(&g_mu); int ok = lib_load(); pthread_mutex_unlock(&g_mu);
    return ok ? vka_shader_make(&g_lib, key) : NULL;
}
// The pipeline cache's stamp (vka_pso.c, vka_pckeep.h): key's entry in the game's library as a hash of its bytes (vka_shlib_entry_hash), 0 = not in it or no library
uint64_t vka_shader_content(uint64_t key) {
    pthread_mutex_lock(&g_mu); int ok = lib_load(); pthread_mutex_unlock(&g_mu);
    const dmsh_vk_entry *e = ok ? vka_shlib_find(&g_lib, key) : NULL;
    return e ? vka_shlib_entry_hash(&g_lib, e) : 0;
}
// The game library's shaders this run made (refused ones and instanced variants left out), with their entry hashes: malloc'd, *n of them (NULL, 0 = none)
vka_pck_ent *vka_shader_made(size_t *n) {
    *n = 0;
    pthread_mutex_lock(&g_mu);
    vka_pck_ent *out = g_lib_state > 0 && g_shn ? malloc(g_shn * sizeof *out) : NULL;
    for (size_t i = 0; out && i < g_shcap; i++) {
        const shent *h = &g_sh[i];
        if (h->sh && h->lib == &g_lib && !h->sh->bad && h->sh->e && h->sh->e->key == h->key && h->sh->chash) out[(*n)++] = (vka_pck_ent){h->key, h->sh->chash};
    }
    pthread_mutex_unlock(&g_mu);
    return out;
}

// Every shader of a library through the device's compiler: each module made, each stage's set-0 layout made, each compute shader's pipeline built (graphics pipelines need
// a pair and state, so vertex and pixel shaders stop at the module and layout). DM_VK_SHLIB_CHECK=1 runs it when the game's library loads; the selftest runs it on DM_VK_SHLIB.
// Returns the number of failures; one log line per failure kind and a summary
int vka_shlib_check_all(const vka_shlib *L) {
    unsigned n = L->h->entry_count, made = 0, refused = 0, nolayout = 0, cs = 0, csfail = 0; uint64_t t0 = now_ns();
    for (unsigned i = 0; i < n; i++) {
        const dmsh_vk_entry *e = &L->ent[i];
        vka_shader *s = vka_shader_make(L, e->key);
        if (!s) { refused++; continue; }
        made++;
        for (int v = 1; v <= DMSH_VK_MOD_SHUFFLE_LDS; v++) if (vka_shlib_module(L, e, v) && !vka_shader_module(s, v)) nolayout++;   // the variants through the compiler too (counted as failures)
        vka_shader *sh[VKA_NSTAGE] = {0}; int st = vka_nvn_stage(e->stage); if (st < 0) { nolayout++; continue; } sh[st] = s;
        vka_layout lay;
        if (!vka_layout_of(sh, st == 5, &lay)) { nolayout++; continue; }
        if (st == 5) { uint64_t t = now_ns(); cs++; if (!vka_cs_pipeline(s, &lay)) csfail++;
                       if (cs <= 3 || cs % 25 == 0) VLOG("shader library check: compute pipeline %u (%016llx) %.0f ms; %u / %u shaders", cs, (unsigned long long)e->key, (now_ns() - t) / 1e6, i + 1, n); }
        if ((i + 1) % 2000 == 0) VLOG("shader library check: %u / %u shaders, %.1f s", i + 1, n, (now_ns() - t0) / 1e9);
    }
    VLOG("shader library check: %u shaders, %u modules made, %u refused, %u without a layout, %u compute pipelines (%u failed), %.1f s",
         n, made, refused, nolayout, cs, csfail, (now_ns() - t0) / 1e9);
    vka_pso_save(1);
    return (int)(nolayout + csfail);
}

// ---------- Programs ----------
// By serial (a new serial at every nvnProgramSetShaders; mtl_prog_release drops it): the program's stage keys resolved to shaders, each at the NVN stage its entry names
typedef struct { uint64_t serial; vka_progsh ps; } prent;
static prent *g_pr; static size_t g_prcap, g_prn;
static prent *pr_find(uint64_t s) {
    if (!g_prcap) return NULL;
    for (size_t i = hslot(s, g_prcap);; i = (i + 1) & (g_prcap - 1)) { if (!g_pr[i].serial) return NULL; if (g_pr[i].serial == s) return &g_pr[i]; }
}
static void pr_put(uint64_t s, const vka_progsh *ps) {
    prent *e = pr_find(s); if (e) { e->ps = *ps; return; }
    if (2 * (g_prn + 1) > g_prcap) {
        size_t nc = g_prcap ? 2 * g_prcap : 1024; prent *n = calloc(nc, sizeof *n);
        for (size_t i = 0; i < g_prcap; i++) if (g_pr[i].serial && g_pr[i].serial != ~0ull) { size_t j = hslot(g_pr[i].serial, nc); while (n[j].serial) j = (j + 1) & (nc - 1); n[j] = g_pr[i]; }
        free(g_pr); g_pr = n; g_prcap = nc; size_t live = 0; for (size_t i = 0; i < nc; i++) live += n[i].serial != 0; g_prn = live;
    }
    size_t j = hslot(s, g_prcap); while (g_pr[j].serial && g_pr[j].serial != ~0ull) j = (j + 1) & (g_prcap - 1);
    if (!g_pr[j].serial) g_prn++;
    g_pr[j] = (prent){s, *ps};
}
void vka_prog_put(uint64_t serial, const vka_progsh *ps) { pthread_mutex_lock(&g_mu); pr_put(serial, ps); pthread_mutex_unlock(&g_mu); }
void mtl_prog_release(prog_t *p) {
    if (!p || !p->serial) return;
    pthread_mutex_lock(&g_mu); prent *e = pr_find(p->serial); if (e) { e->serial = ~0ull; memset(&e->ps, 0, sizeof e->ps); } pthread_mutex_unlock(&g_mu);   // a tombstone
}
void mtl_prog_prefetch(const prog_t *p) { (void)p; }   // modules are made at the program's first draw (vka_prog_shaders)

static int mem_read(uint64_t addr, void *out, size_t n) {
    if (vka_addr(addr, n, NULL, NULL, NULL)) { memcpy(out, (const void *)(uintptr_t)addr, n); return 1; }
    return !plat_mem_read(addr, out, n);
}

int vka_prog_shaders(const prog_t *p, vka_progsh *out) {
    memset(out, 0, sizeof *out);
    if (!p || !p->serial) return 0;
    pthread_mutex_lock(&g_mu); prent *e = pr_find(p->serial); if (e) *out = e->ps; pthread_mutex_unlock(&g_mu);
    if (e) return 1;
    pthread_mutex_lock(&g_mu); int ok = lib_load(); pthread_mutex_unlock(&g_mu);
    if (!ok) return 0;
    for (unsigned i = 0; i < p->nstage && i < 6; i++) {   // (a program has at most 6 shaders: hle_nvn.c nvnProgramSetShaders)
        uint8_t md[8];
        if (!prog_key(p, i, md)) continue;
        uint64_t key = vka_shlib_key(md);
        const dmsh_vk_entry *en = vka_shlib_find(&g_lib, key);
        if (!en) {
            out->bad |= VKA_BAD_MISSING;
            static _Atomic int n;
            if (atomic_fetch_add(&n, 1) < 32)
                VRED("shader %016llx (program stage %u) is not in the shader library", (unsigned long long)key, i);
            static const char *shd; static int shd_init;
            if (!shd_init) { shd = getenv("DM_NVN_SHDUMP"); shd_init = 1; if (shd && *shd) mkdir(shd, 0777); }
            if (shd && p->sd[i].control) {
                char path[1024]; FILE *f; uint32_t ncode = 0, nctl = 0;
                nvn_ctl_lens(p->sd[i].control, &nctl, &ncode);
                if (nctl == 0x48) nctl = 0xd8;
                snprintf(path, sizeof path, "%s/%016llx.ctl", shd, (unsigned long long)key);
                if ((f = fopen(path, "wb"))) { fwrite(p->sd[i].control, 1, nctl, f); fclose(f); }
                snprintf(path, sizeof path, "%s/%016llx.code", shd, (unsigned long long)key);
                if (p->sd[i].data && ncode && ncode < (1u << 24)) {
                    void *code = malloc(ncode);
                    if (code && mem_read(p->sd[i].data, code, ncode)) {
                        if ((f = fopen(path, "wb"))) { fwrite(code, 1, ncode, f); fclose(f); }
                    }
                    free(code);
                }
            }
            continue;
        }
        int st = vka_nvn_stage(en->stage);
        if (st < 0) { out->bad |= VKA_BAD_MISSING; continue; }   // (vka_shlib_open refuses a stage past DMSH_VK_STAGE_MS)
        if (out->sh[st]) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VRED("program serial %llu has two %s shaders (%016llx, %016llx): the second is used", (unsigned long long)p->serial, stage_name(en->stage), (unsigned long long)out->sh[st]->key, (unsigned long long)key); }
        out->sh[st] = vka_shader_make(&g_lib, key);
        if (!out->sh[st]) out->bad |= 1u << st;
    }
    vka_prog_put(p->serial, out);
    return 1;
}
#endif
