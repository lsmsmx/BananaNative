// Pipelines of the Vulkan replay. The device has no extended dynamic state 3 and no shader objects (android/spike results), so a graphics pipeline is keyed on the state those
// would have made dynamic: attachment formats and sample count, blending and write masks per attachment, logic operation, polygon mode, depth clamp, alpha to coverage, the
// topology class and the depth convention (SpecId 0 on the last pre-raster stage), with vertex input in the key only on a device without VK_EXT_vertex_input_dynamic_state.
// Everything else is dynamic (EDS1 / EDS2): viewport and scissor (one of each), cull mode, front face, topology, depth and stencil tests, depth bias, rasterizer discard,
// primitive restart, blend constants. Pipelines and set layouts live as long as the process; the VkPipelineCache goes to the app's files (DM_TEMP, the game's own temp folder)
// so the next start builds from it. Layouts follow dmsh_abi_vk.h: set 0 the per-draw records of every stage (push descriptors when they fit), set 1 the pools (vka_tex.c)
#if defined(__ANDROID__)
#include "vka.h"
#include "vka_pwlist.h"
#include "vk_prewarm_seed.h"   // the profile's seed list (dm/games/<id>/vk_prewarm.bin; dm/games/game.py seed): dm_vk_prewarm_seed, dm_vk_prewarm_seed_len (0: none)
#include "../nvn/nvn_mtl.h"
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <errno.h>

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
// Suspend (mtl_drain, the app going to the background): the prewarm and optimizer threads stop between pipelines until the game submits again (vka_pso_pause(0) at the next
// submission), so a backgrounded app does not keep compiling on the CPU for minutes
static _Atomic int g_pso_paused;
void vka_pso_pause(int on) { atomic_store(&g_pso_paused, on != 0); }
static void pause_point(void) { while (atomic_load_explicit(&g_pso_paused, memory_order_relaxed)) usleep(100000); }
// The keys' hash, 8 bytes a step: the draw path hashes a pipeline key (about 1 KB) per draw, and FNV a byte at a time took 1 us of it on the Adreno 840 phone's cores
static inline uint64_t fnv(const void *p, size_t n, uint64_t h) { return vka_pw_fnv(p, n, h); }
_Static_assert(VKA_PW_TOPO_MAX == VK_PRIMITIVE_TOPOLOGY_PATCH_LIST && VKA_PW_POLY_MAX == VK_POLYGON_MODE_POINT && VKA_PW_LOGIC_MAX == VK_LOGIC_OP_SET
               && VKA_PW_BLENDF_MAX == VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA && VKA_PW_BLENDOP_MAX == VK_BLEND_OP_MAX && VKA_PW_FS_VARIANT_MAX == VKA_FS_RETYPE, "vka_pwlist.h bounds");

// A small map from a byte key to a value (layouts, pipelines): open addressing on the key's hash, keys copied
typedef struct { uint64_t h; void *key; void *val; } ment;
typedef struct { ment *e; size_t cap, n, klen; } kmap;
static void *km_get(kmap *m, const void *key, uint64_t h) {
    if (!m->cap) return NULL;
    for (size_t i = h & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) { if (!m->e[i].key) return NULL; if (m->e[i].h == h && !memcmp(m->e[i].key, key, m->klen)) return m->e[i].val; }
}
static void km_put(kmap *m, const void *key, uint64_t h, void *val) {
    if (2 * (m->n + 1) > m->cap) {
        size_t nc = m->cap ? 2 * m->cap : 256; ment *n = calloc(nc, sizeof *n);
        for (size_t i = 0; i < m->cap; i++) if (m->e[i].key) { size_t j = m->e[i].h & (nc - 1); while (n[j].key) j = (j + 1) & (nc - 1); n[j] = m->e[i]; }
        free(m->e); m->e = n; m->cap = nc;
    }
    size_t j = h & (m->cap - 1); while (m->e[j].key) j = (j + 1) & (m->cap - 1);
    void *k = malloc(m->klen); memcpy(k, key, m->klen);
    m->e[j] = (ment){h, k, val}; m->n++;
}
// The value slot of key, to change an entry in place (NULL: not in the map)
static void **km_slot(kmap *m, const void *key, uint64_t h) {
    if (!m->cap) return NULL;
    for (size_t i = h & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) { if (!m->e[i].key) return NULL; if (m->e[i].h == h && !memcmp(m->e[i].key, key, m->klen)) return &m->e[i].val; }
}

// ---------- Layouts ----------
static VkShaderStageFlags stage_bit(int vks) {
    static const VkShaderStageFlags b[8] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
                                            VK_SHADER_STAGE_GEOMETRY_BIT, VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_COMPUTE_BIT, VK_SHADER_STAGE_TASK_BIT_EXT, VK_SHADER_STAGE_MESH_BIT_EXT};
    return vks >= 0 && vks < 8 ? b[vks] : 0;
}
enum { MAXB = 6 * DMSH_VK_STAGE_BINDINGS };   // (task and mesh shaders take the tess control and vertex ranges: dmsh_abi_vk.h)
typedef struct { uint32_t n; struct { uint16_t binding; uint8_t desc, pad; uint16_t count; uint16_t stages; } b[MAXB]; } lkey;
static kmap g_layouts = {.klen = sizeof(lkey)};
static uint64_t g_nlayouts;

// The layout of a set of shaders, remembered by the shaders themselves: building the bindings key (3 KB) and hashing it was a third of the replay's CPU time at 5,500 draws a frame
typedef struct { vka_shader *sh[VKA_NSTAGE]; uint64_t compute; } skey;
static kmap g_lshort = {.klen = sizeof(skey)};
static int layout_slow(vka_shader *const sh[VKA_NSTAGE], int compute, vka_layout *out);
int vka_layout_of(vka_shader *const sh[VKA_NSTAGE], int compute, vka_layout *out) {
    skey sk; memset(&sk, 0, sizeof sk);
    for (int st = 0; st < VKA_NSTAGE; st++) if ((st == 5) == (compute != 0)) sk.sh[st] = sh[st];
    sk.compute = compute != 0;
    uint64_t h = fnv(&sk, sizeof sk, 1469598103934665603ull);
    pthread_mutex_lock(&g_mu);
    vka_layout *L = km_get(&g_lshort, &sk, h);
    if (L) { *out = *L; pthread_mutex_unlock(&g_mu); return out->layout != VK_NULL_HANDLE; }
    pthread_mutex_unlock(&g_mu);
    memset(out, 0, sizeof *out);
    int ok = layout_slow(sh, compute, out);
    L = malloc(sizeof *L); *L = *out;
    pthread_mutex_lock(&g_mu);
    if (!km_get(&g_lshort, &sk, h)) km_put(&g_lshort, &sk, h, L); else free(L);
    pthread_mutex_unlock(&g_mu);
    return ok;
}
static int layout_slow(vka_shader *const sh[VKA_NSTAGE], int compute, vka_layout *out) {
    lkey k; memset(&k, 0, sizeof k);
    uint32_t ndesc = 0;
    for (int st = 0; st < VKA_NSTAGE; st++) {
        const vka_shader *s = sh[st];
        if (!s || (st == 5) != (compute != 0)) continue;
        for (unsigned i = 0; i < s->e->bind_count; i++) {
            const dmsh_vk_bind *b = &s->b[i];
            if (b->set != DMSH_VK_SET_DRAW || b->kind == DMSH_VK_KIND_TEX_POOL || b->kind == DMSH_VK_KIND_SAMP_POOL) continue;
            if (k.n == MAXB) { VRED_ONCE("more than %d set-0 bindings in one pipeline", MAXB); return 0; }
            k.b[k.n].binding = b->binding; k.b[k.n].desc = b->desc; k.b[k.n].count = b->count ? b->count : 1; k.b[k.n].stages = (uint16_t)stage_bit(s->e->stage);
            ndesc += k.b[k.n].count; k.n++;
        }
        if (s->gs_fx > 0 && k.n < MAXB) {   // the claim words of a geometry shader that writes memory (vka_spv_gs_claim)
            k.b[k.n].binding = VKA_GS_CLAIM_BINDING; k.b[k.n].desc = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; k.b[k.n].count = 1; k.b[k.n].stages = (uint16_t)stage_bit(s->e->stage); ndesc++; k.n++;
        }
    }
    // Sorted by binding, so the same set of records gives the same key whatever the order of stages
    for (uint32_t i = 1; i < k.n; i++) for (uint32_t j = i; j > 0 && k.b[j - 1].binding > k.b[j].binding; j--) { typeof(k.b[0]) t = k.b[j]; k.b[j] = k.b[j - 1]; k.b[j - 1] = t; }
    uint64_t h = fnv(&k, sizeof k, 1469598103934665603ull);
    pthread_mutex_lock(&g_mu);
    vka_layout *L = km_get(&g_layouts, &k, h);
    if (L) { *out = *L; pthread_mutex_unlock(&g_mu); return out->layout != VK_NULL_HANDLE; }
    L = calloc(1, sizeof *L);
    L->ndesc = ndesc; L->push = V.push_desc && ndesc <= V.max_push;
    VkDescriptorSetLayoutBinding *lb = calloc(k.n ? k.n : 1, sizeof *lb);
    for (uint32_t i = 0; i < k.n; i++) lb[i] = (VkDescriptorSetLayoutBinding){k.b[i].binding, (VkDescriptorType)k.b[i].desc, k.b[i].count, k.b[i].stages, NULL};
    VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .flags = L->push ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0, .bindingCount = k.n, .pBindings = lb};
    VkResult r = vk.vkCreateDescriptorSetLayout(V.dev, &li, NULL, &L->set0);
    free(lb);
    if (!r) {
        VkDescriptorSetLayout sets[2] = {L->set0, vka_bindless_layout()};
        VkPipelineLayoutCreateInfo pi = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = sets[1] ? 2 : 1, .pSetLayouts = sets};
        r = vk.vkCreatePipelineLayout(V.dev, &pi, NULL, &L->layout);
    }
    if (r) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VRED("pipeline layout of %u bindings (%u descriptors): %d", k.n, ndesc, r); L->layout = VK_NULL_HANDLE; }
    km_put(&g_layouts, &k, h, L); g_nlayouts++;
    *out = *L;
    pthread_mutex_unlock(&g_mu);
    return out->layout != VK_NULL_HANDLE;
}


// ---------- The pipeline cache ----------
// DM_VK_PCACHE (restart): unset = $DM_TEMP/vk_pipelines[-<driver>].bin; a path = that file; 0 = no file (the run starts empty and keeps nothing: a cold run). Before 2026-11-03 "0"
// was taken as a file named 0 in the working folder, so a second "cold" run read the first one's pipelines back (stutter-agent cold2gpl: 10 MB read, warm)
// The file is kept for one shader library at a time: its stamp <file>.lib (vka_pckeep.h; written with each save) names the library it was saved with (vka_shader_lib_id) and
// lists the shaders whose pipelines it holds with their entry hashes. A file of another library is read when its shaders are mostly byte for byte the same in this one
// (a restamped library, a delta update): the stale shaders it holds (changed or dropped since) may be at most DM_VK_PCACHE_KEEP percent (restart; default 50; 0 = a file of
// another library is never read) of the live ones, counted over every update the file was carried across; otherwise, or with no stamp, the run starts empty and its first
// save replaces the file. Before 2026-11-08 the file kept the shaders of every library ever played, and a driver that deserializes and uploads every shader of a cache when
// it is opened (Mesa's vk_pipeline_cache_load; Turnip gives each its code and private memory) paid for all of them: Ocarina of Time's 72 MB Turnip cache held 1,791 shaders,
// two of them from libraries before the HMMA spill fix with 80 and 54 KB of private memory a fiber, and opening it committed 5.2 GB of GPU memory, after which the game
// was killed by lmkd at its first 3D frames (docs/reference/memory.md "Pipeline cache of earlier shader libraries"). From then until the stamp listed its shaders, every
// library update started empty, and the first start after one waited minutes for its pipelines (docs/reference/memory.md "A shader library update that threw the
// pipeline cache away")
static uint64_t g_pc_lib;   // the library this run's cache file is stamped with (0: none known; no stamp written)
static vka_pck_stamp g_pck;   // the shaders the file was read with that this library has the same, and the stale estimate (the stamp's; written again with this run's)
static uint64_t pck_hash_of(void *ctx, uint64_t key) { (void)ctx; return vka_shader_content(key); }
static VkPipelineCache g_pc; static char g_pc_path[1024], g_pw_path[1100]; static uint64_t g_made, g_saved_at, g_save_ns; static double g_create_ms;
static uint64_t g_ngfx, g_ncs, g_nfail, g_ncs_over, g_create_ns, g_frame_made;   // all pipelines made (frame path, background and prewarm); g_frame_made / g_create_ns: on the frame path only
static uint64_t g_probe_hit, g_probe_miss, g_probe_miss_ns;          // GPL: the cache-only try before a link (VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT)
static _Atomic uint64_t g_save_ns_total, g_hy_draws, g_pw_wait_ns;
static void pw_load(void); static void pw_start(void); static int pw_save(void);
static void *saver_main(void *a);
static pthread_mutex_t g_save_mu = PTHREAD_MUTEX_INITIALIZER;   // one save at a time (the saver thread, a forced save)
static pthread_mutex_t g_sv_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_sv_cv = PTHREAD_COND_INITIALIZER; static int g_sv_want, g_sv_up;
static void pc_open_once(void) {
    const char *e = getenv("DM_VK_PCACHE"), *t = getenv("DM_TEMP"), *home = getenv("HOME");
    // DM_VK_PCACHE_DIR: the same per-driver file name in another directory. Android's /storage/emulated is the FUSE view of /data/media: a 112 MB cache write there goes
    // through the media daemon, while the app's own internal files are ext4/f2fs directly. The driver tag stays, so switching drivers still leaves the other's cache alone
    const char *cd = getenv("DM_VK_PCACHE_DIR");
    if (cd && *cd) { t = cd; static int made; if (!made++) mkdir(cd, 0777); }
    if (e && !strcmp(e, "0")) g_pc_path[0] = 0;
    else if (e && *e) snprintf(g_pc_path, sizeof g_pc_path, "%s", e);
    else if ((t && *t) || (home && *home)) {   // a custom driver (DM_VK_DRIVER) keeps its own file, so switching drivers leaves the system driver's cache alone
        const char *dt = vka_driver_tag();
        snprintf(g_pc_path, sizeof g_pc_path, "%s/vk_pipelines%s%s.bin", t && *t ? t : home, dt ? "-" : "", dt ? dt : "");
    }
    // The prewarm list (pw_*): the pipeline keys of earlier runs, whatever the driver (a key names shaders and state, not driver code). DM_VK_PREWARM_FILE overrides
    const char *pf = getenv("DM_VK_PREWARM_FILE");
    if (pf && *pf) snprintf(g_pw_path, sizeof g_pw_path, "%s", pf);
    else if (e && *e && strcmp(e, "0")) snprintf(g_pw_path, sizeof g_pw_path, "%s.keys", e);
    else if ((t && *t) || (home && *home)) snprintf(g_pw_path, sizeof g_pw_path, "%s/vk_pipelines.keys", t && *t ? t : home);
    void *data = NULL; size_t n = 0;
    FILE *f = g_pc_path[0] ? fopen(g_pc_path, "rb") : NULL;
    if (f) {
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        if (sz > 32 && (data = malloc((size_t)sz)) && fread(data, 1, (size_t)sz, f) == (size_t)sz) n = (size_t)sz;
        fclose(f);
        // The header names the device: a cache of another driver is dropped here (the driver would ignore it too)
        uint32_t hl, ver, vendor, devid; uint8_t uuid[VK_UUID_SIZE];
        if (n) { memcpy(&hl, data, 4); memcpy(&ver, (uint8_t *)data + 4, 4); memcpy(&vendor, (uint8_t *)data + 8, 4); memcpy(&devid, (uint8_t *)data + 12, 4); memcpy(uuid, (uint8_t *)data + 16, VK_UUID_SIZE);
                 if (hl < 32 || ver != VK_PIPELINE_CACHE_HEADER_VERSION_ONE || vendor != V.props.vendorID || devid != V.props.deviceID || memcmp(uuid, V.props.pipelineCacheUUID, VK_UUID_SIZE)) { VLOG("pipeline cache %s is another driver's: started empty", g_pc_path); n = 0; } }
    }
    g_pc_lib = g_pc_path[0] ? vka_shader_lib_id() : 0;
    memset(&g_pck, 0, sizeof g_pck);
    if (n && g_pc_lib) {   // the stamp: the library the file was saved with and the shaders it holds
        char sp[1100]; snprintf(sp, sizeof sp, "%s.lib", g_pc_path);
        char *sb = NULL; long sl = 0; FILE *sf = fopen(sp, "rb");
        if (sf) { fseek(sf, 0, SEEK_END); sl = ftell(sf); fseek(sf, 0, SEEK_SET); if (sl > 0 && sl < (64l << 20) && (sb = malloc((size_t)sl)) && fread(sb, 1, (size_t)sl, sf) != (size_t)sl) sl = 0; fclose(sf); }
        vka_pck_stamp old; vka_pck_parse(sb ? sb : "", sb ? (size_t)sl : 0, &old); free(sb);
        int keep = VKA_ENV("DM_VK_PCACHE_KEEP", 50); size_t live, changed; uint64_t dead; uint64_t t0 = now_ns();
        int d = vka_pck_decide(&old, g_pc_lib, keep, pck_hash_of, NULL, &g_pck, &live, &changed, &dead);
        double ms = (now_ns() - t0) / 1e6;
        switch (d) {
        case VKA_PCK_CARRIED:
            VLOG("pipeline cache %s (%zu bytes) is of another shader library: kept, %zu of the %zu shaders it lists are byte for byte the same in this one (%zu changed or dropped: "
                 "their pipelines are made again), %llu stale shaders in the file in all, at most %d%% of the kept ones allowed (DM_VK_PCACHE_KEEP; %.0f ms)", g_pc_path, n, live, old.n, changed,
                 (unsigned long long)dead, keep, ms); break;
        case VKA_PCK_UNVERIFIED:
            VLOG("pipeline cache %s (%zu bytes) is of another shader library: kept, its stamp (an older host's) names one library but lists no shaders; they are counted from this "
                 "run on", g_pc_path, n); break;
        case VKA_PCK_STALE:
            VLOG("pipeline cache %s (%zu bytes) is of another shader library: started empty, %zu of the %zu shaders it lists are the same in this one, %llu stale (changed or "
                 "dropped by this and earlier updates), over DM_VK_PCACHE_KEEP=%d%% of them; the next save replaces it (%.0f ms)", g_pc_path, n, live, old.n,
                 (unsigned long long)(old.dead + changed), keep, ms); n = 0; break;
        case VKA_PCK_OFF:
            VLOG("pipeline cache %s (%zu bytes) is of another shader library: started empty (DM_VK_PCACHE_KEEP=0; the next save replaces it)", g_pc_path, n); n = 0; break;
        case VKA_PCK_NO_STAMP:
            VLOG("pipeline cache %s (%zu bytes) is not stamped with its shader library: started empty (it may hold every library's shaders, which would all be loaded; the next "
                 "save replaces it)", g_pc_path, n); n = 0; break;
        default: break;
        }
        if (!vka_pck_reads(d)) vka_pck_free(&g_pck);
        vka_pck_free(&old);
    }
    VkPipelineCacheCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, .initialDataSize = n, .pInitialData = n ? data : NULL};
    VkResult r = vk.vkCreatePipelineCache(V.dev, &ci, NULL, &g_pc);
    if (r && n) { ci.initialDataSize = 0; ci.pInitialData = NULL; r = vk.vkCreatePipelineCache(V.dev, &ci, NULL, &g_pc); vka_pck_free(&g_pck); n = 0; }   // (started empty after all)
    if (r) { g_pc = VK_NULL_HANDLE; VRED("vkCreatePipelineCache %d: pipelines are built without a cache", r); }
    else VLOG("pipeline cache %s: %zu bytes read (its stamp lists %zu shaders, %llu stale)", g_pc_path[0] ? g_pc_path : e && !strcmp(e, "0") ? "(none: DM_VK_PCACHE=0)" : "(not kept: no DM_TEMP or HOME)", n,
              g_pck.n, (unsigned long long)g_pck.dead);
    free(data);
    pw_load();
    pthread_t th;
    if (!pthread_create(&th, NULL, saver_main, NULL)) { pthread_detach(th); g_sv_up = 1; }
    pw_start();
}
static void pc_open(void) { static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, pc_open_once); }   // (the prewarm threads it starts call it too: they wait for it)
// The cache and the prewarm list to their files. vkGetPipelineCacheData runs without g_mu (the cache is internally synchronized): holding it there kept the replay thread's
// pipeline lookups waiting for a 17 MB copy once a minute
static void save_now(void) {
    uint64_t t = now_ns();
    uint64_t made = __atomic_load_n(&g_made, __ATOMIC_RELAXED);
    size_t n = 0; void *d = NULL;
    if (g_pc && g_pc_path[0] && made != g_saved_at) {
        // The cache grows between the size query and the copy while prewarm and optimizer threads add pipelines: the copy then answers VK_INCOMPLETE with a partial
        // blob. Tried up to 3 times with a quarter more room; only a complete copy (VK_SUCCESS) is written
        VkResult gr = VK_INCOMPLETE;
        for (int tr = 0; tr < 3 && gr == VK_INCOMPLETE; tr++) {
            size_t q = 0; if (vk.vkGetPipelineCacheData(V.dev, g_pc, &q, NULL) || !q) { gr = VK_ERROR_UNKNOWN; break; }
            n = q + q / 4; free(d); d = malloc(n); if (!d) { gr = VK_ERROR_OUT_OF_HOST_MEMORY; break; }
            gr = vk.vkGetPipelineCacheData(V.dev, g_pc, &n, d);
        }
        if (gr != VK_SUCCESS) { free(d); d = NULL; n = 0; static _Atomic int k; if (atomic_fetch_add(&k, 1) < 4) VRED("pipeline cache not saved: vkGetPipelineCacheData %d after 3 tries (pipelines being added meanwhile); the next save tries again", (int)gr); }
        if (d) {
            char tmp[1100]; snprintf(tmp, sizeof tmp, "%s.tmp", g_pc_path);
            FILE *f = fopen(tmp, "wb");
            if (f && fwrite(d, 1, n, f) == n && !fclose(f) && !rename(tmp, g_pc_path)) {
                g_saved_at = made;
                // Its stamp after it: the shaders it was read with that this library keeps, and those this run made. Written whole or not at all (a stamp that
                // cannot be written is removed, and the next run starts empty)
                char sp[1100], st[1200]; snprintf(sp, sizeof sp, "%s.lib", g_pc_path); snprintf(st, sizeof st, "%s.lib.tmp", g_pc_path);
                size_t nr = 0, tl = 0; vka_pck_ent *run = g_pc_lib ? vka_shader_made(&nr) : NULL;
                char *txt = g_pc_lib ? vka_pck_format(g_pc_lib, g_pck.dead, g_pck.ent, g_pck.n, run, nr, &tl) : NULL; free(run);
                FILE *sf = txt ? fopen(st, "wb") : NULL;
                if (!(sf && fwrite(txt, 1, tl, sf) == tl && !fclose(sf) && !rename(st, sp))) { if (sf) fclose(sf); unlink(st); unlink(sp); }
                free(txt);
            }
            else { if (f) fclose(f); unlink(tmp); static _Atomic int k; if (atomic_fetch_add(&k, 1) < 2) VRED("pipeline cache not written to %s", g_pc_path); d = NULL; n = 0; }
        }
    }
    int keys = pw_save();
    uint64_t dt = now_ns() - t; atomic_fetch_add(&g_save_ns_total, dt);
    if (d || keys > 0) VLOG("pipeline cache: %zu bytes written to %s (%llu pipelines made this run), prewarm list %d keys, %.0f ms (on the saver thread)", n, g_pc_path[0] ? g_pc_path : "(none)",
                            (unsigned long long)made, keys, dt / 1e6);
    free(d);
}
static void *saver_main(void *a) {
    (void)a; pthread_setname_np(pthread_self(), "vk-pcache");
    for (;;) {
        pthread_mutex_lock(&g_sv_mu); while (!g_sv_want) pthread_cond_wait(&g_sv_cv, &g_sv_mu); g_sv_want = 0; pthread_mutex_unlock(&g_sv_mu);
        pthread_mutex_lock(&g_save_mu); save_now(); pthread_mutex_unlock(&g_save_mu);
    }
    return NULL;
}
// From the replay thread at each submission, and after a pipeline that took over 1 s on the frame path (a cold first 3D frame is one submission of a minute or more of
// pipelines, and a run stopped in it saved none of them, so every run started cold again: Ocarina of Time, 2026-11-08): a save is due DM_VK_PCACHE_SAVE_S seconds (default
// 20) after the last one when pipelines were made since; it runs on the saver thread. force 1 (the end of the run, the shader library check): now, on the caller's thread.
// force 2 (a pipeline that took over 1 s): due DM_VK_PCACHE_SAVE_SLOW_S (default 3) after the last save, on the saver thread. With the 20 s rule a slow one made in the last
// 20 s of a run was lost when the process was stopped or killed, and the next run made it again on the frame path: Ocarina of Time's compute pipeline 3d0d6d61bb9076e7,
// 3.1 s in run p-sys1 five seconds after its last save, 2.3 s again in the warm run after it (2026-11-08)
void vka_pso_save(int force) {
    if (!g_pc) return;
    uint64_t t = now_ns();
    if (force != 1) {
        if (__atomic_load_n(&g_made, __ATOMIC_RELAXED) == g_saved_at) return;
        int s = force == 2 ? VKA_ENV("DM_VK_PCACHE_SAVE_SLOW_S", 3) : VKA_ENV("DM_VK_PCACHE_SAVE_S", 20); if (s < 1) s = 1;
        if (t - g_save_ns < (uint64_t)s * 1000000000ull) return;
        g_save_ns = t;
        if (g_sv_up) { pthread_mutex_lock(&g_sv_mu); g_sv_want = 1; pthread_cond_signal(&g_sv_cv); pthread_mutex_unlock(&g_sv_mu); return; }
    }
    g_save_ns = t;
    pthread_mutex_lock(&g_save_mu); save_now(); pthread_mutex_unlock(&g_save_mu);
}
void vka_gpl_line(char *out, size_t n);
void vka_pso_async_line(char *out, size_t n);
void mtl_pso_line(char *out, size_t n) {
    if (!n) return;
    int k = snprintf(out, n, "%llu graphics + %llu compute pipelines (%llu failed, %llu compute (program, layout) pairs not made for shared memory, %llu on the frame path), %llu set layouts, %.0f ms creating", (unsigned long long)g_ngfx, (unsigned long long)g_ncs,
                     (unsigned long long)g_nfail, (unsigned long long)g_ncs_over, (unsigned long long)g_frame_made, (unsigned long long)g_nlayouts, g_create_ms);
    if (V.gpl && k > 0 && (size_t)k + 3 < n) { memcpy(out + k, "; ", 3); vka_gpl_line(out + k + 2, n - (size_t)k - 2); }
    size_t o = strlen(out); char a[200]; vka_pso_async_line(a, sizeof a);
    if (a[0] && o + 2 < n) snprintf(out + o, n - o, "; %s", a);
}

// The long-frame log's counters (hle_nvn.c): pipelines made on the frame path and the time it took, cumulative (it takes each frame's difference). Background and prewarm
// pipelines are not the frame's. Shader modules are made with the shader (vka_shader_make), not counted here
void mtl_create_stats(uint64_t *lib, uint64_t *pso) { pthread_mutex_lock(&g_mu); if (lib) *lib = 0; if (pso) *pso = g_frame_made; pthread_mutex_unlock(&g_mu); }
void mtl_create_ns(uint64_t *lib_ns, uint64_t *pso_ns) { pthread_mutex_lock(&g_mu); if (lib_ns) *lib_ns = 0; if (pso_ns) *pso_ns = g_create_ns; pthread_mutex_unlock(&g_mu); }
// "draws with a hybrid pipeline" of the [longframe] line: on Android, draws recorded with a fast-linked GPL pipeline (before its optimized one took its place); made = pipelines
// linked. (The Mac's hybrid pipelines are its pulling stand-ins; this stub returned 0 on Android until 2026-11-03, whatever GPL did)
static uint64_t g_nlinked_pub;
void mtl_hybrid_stats(uint64_t *draws, uint64_t *made, uint64_t *none) { if (draws) *draws = atomic_load(&g_hy_draws); if (made) *made = __atomic_load_n(&g_nlinked_pub, __ATOMIC_RELAXED); if (none) *none = 0; }
// Pipeline-side attribution for the [longframe] line (vka_extra.c): the saver's time, the frame path's waits for a prewarm thread making the same pipeline
void vka_pso_waits(uint64_t *save_ns, uint64_t *pw_wait_ns) { if (save_ns) *save_ns = atomic_load(&g_save_ns_total); if (pw_wait_ns) *pw_wait_ns = atomic_load(&g_pw_wait_ns); }

// ---------- Graphics pipelines ----------
// The map's value: the pipeline (VK_NULL_HANDLE = failed, its draws skipped), whether it is a fast-linked one, and whether a thread is still making it (busy: the frame path
// waits for that thread instead of making it a second time). p and linked change in place when the optimized pipeline replaces the linked one
typedef struct gval { VkPipeline p; uint8_t linked, busy, pw; pid_t tid; } gval;   // pw, tid: a prewarm thread is making it (busy), that thread (busy_wait)
typedef struct { vka_shader *sh[VKA_NSTAGE]; VkPipelineLayout layout; vka_gkey k; } gkey_full;   // (sh[5], compute, stays NULL)
static kmap g_gfx = {.klen = sizeof(gkey_full)};
static pthread_cond_t g_busy_cv = PTHREAD_COND_INITIALIZER;
// The create info of a graphics pipeline, built in place (its members point at one another): the whole pipeline and each stage library take their parts from it
typedef struct {
    VkPipelineShaderStageCreateInfo ss[5]; uint32_t ns; VkPipelineShaderStageRequiredSubgroupSizeCreateInfo rsg[5];
    VkBool32 gl_true; uint32_t afunc; VkSpecializationMapEntry me, ae; VkSpecializationInfo spec, aspec;
    VkVertexInputBindingDescription vb[33]; VkVertexInputAttributeDescription va[32]; VkVertexInputBindingDivisorDescription vd[33];
    VkPipelineVertexInputDivisorStateCreateInfo vds; VkPipelineVertexInputStateCreateInfo vi; VkPipelineInputAssemblyStateCreateInfo ia; VkPipelineTessellationStateCreateInfo ts;
    VkPipelineViewportStateCreateInfo vp; VkPipelineRasterizationStateCreateInfo rs; VkPipelineMultisampleStateCreateInfo ms; VkPipelineDepthStencilStateCreateInfo ds;
    VkPipelineColorBlendAttachmentState cb[8]; VkPipelineColorBlendStateCreateInfo bs; VkDynamicState dyn[24]; VkPipelineDynamicStateCreateInfo dy;
    VkFormat cf[8]; VkPipelineRenderingCreateInfo ri; VkGraphicsPipelineCreateInfo ci; int tess, mesh;   // mesh: a task / mesh pipeline (pstate_fill)
    VkDescriptorSetLayout set0;   // the layout's set 0 (the library probe's independent-sets layout)
} pstate;

// A VkFormat's class for colour outputs: 1 UINT, 2 SINT, 0 the rest (float, normalized, scaled)
static int vkf_int_class(uint32_t f) {
    static const uint16_t u[] = {13, 20, 27, 34, 41, 48, 55, 62, 68, 74, 81, 88, 95, 98, 101, 104, 107, 110, 113, 116, 119};
    for (size_t i = 0; i < sizeof u / sizeof *u; i++) { if (f == u[i]) return 1; if (f == u[i] + 1u) return 2; }
    return 0;
}
// VKA_FS_RETYPE: the pixel shader's float outputs (fout_types) that it writes (fout_mask) into UINT / SINT attachments of the key: retyped in its module (vka_shader_fout_module)
static void fout_masks(const vka_shader *fs, const vka_gkey *k, uint32_t *um, uint32_t *sm) {
    *um = *sm = 0;
    for (uint32_t i = 0; i < k->ncolor && i < 8; i++) {
        int c = vkf_int_class(k->cfmt[i]);
        if (c && (fs->e->fout_mask >> i & 1) && (fs->e->fout_types >> (2 * i) & 3) == DMSH_VK_TYPE_FLOAT) { if (c == 1) *um |= 1u << i; else *sm |= 1u << i; }
    }
}
// 0 = a stage's module is missing (logged by vka_shader_module)
static int pstate_fill(pstate *P, vka_shader *const sh[VKA_NSTAGE], const vka_layout *lay, const vka_gkey *k) {
    memset(P, 0, sizeof *P);
    // Stages: vertex, tess control, tess evaluation, geometry, fragment; the GL depth constant on the last of the first four. A mesh pipeline (NVN2 task and mesh programs,
    // nvnCommandBufferDrawMeshTasks): task (optional), mesh, fragment, the GL depth constant on the mesh stage; it has no vertex input or input assembly state
    static const int ovtx[5] = {0, 3, 4, 2, 1}, omesh[5] = {6, 7, 1, -1, -1};
    const int mesh = sh[7] != NULL, *order = mesh ? omesh : ovtx, nord = mesh ? 3 : 5;
    int last_pre = -1;
    for (int i = 0; i < nord - 1; i++) if (sh[order[i]]) last_pre = order[i];
    P->gl_true = VK_TRUE; P->me = (VkSpecializationMapEntry){DMSH_VK_SPEC_GL_DEPTH, 0, sizeof(VkBool32)}; P->spec = (VkSpecializationInfo){1, &P->me, sizeof P->gl_true, &P->gl_true};
    P->afunc = k->alpha_func; P->ae = (VkSpecializationMapEntry){DMSH_VK_SPEC_ALPHA_FUNC, 0, sizeof(uint32_t)};   // the alpha test's function on an F_ALPHA_TEST pixel shader
    P->aspec = (VkSpecializationInfo){1, &P->ae, sizeof P->afunc, &P->afunc};
    for (int i = 0; i < nord; i++) {
        vka_shader *s = sh[order[i]]; if (!s) continue;
        int fs = order[i] == 1, gl = k->gl_depth && order[i] == last_pre && (s->e->flags & DMSH_VK_F_GL_DEPTH);
        uint32_t fu = 0, fsm = 0; if (fs && k->fs_variant == VKA_FS_RETYPE) fout_masks(s, k, &fu, &fsm);
        VkShaderModule mod = order[i] == 0 && (k->vin_u | k->vin_s) ? vka_shader_vin_module(s, k->vin_u, k->vin_s) : order[i] == 2 && k->gs_fx ? vka_shader_gs_module(s)
                           : fs && k->fs_variant == VKA_FS_RETYPE ? (fu | fsm ? vka_shader_fout_module(s, fu, fsm) : vka_shader_module(s, DMSH_VK_MOD_BASE))
                           : vka_shader_module(s, fs ? k->fs_variant : DMSH_VK_MOD_BASE);
        if (!mod && order[i] == 2 && k->gs_fx) mod = vka_shader_module(s, DMSH_VK_MOD_BASE);   // no module with the claim (logged): the shader as it is
        if (mod && order[i] == 2 && sh[1]) {   // the pixel shader's inputs the geometry shader does not write, added (vka_shader_gs_link_module); none: the module above
            VkShaderModule lm = vka_shader_gs_link_module(s, sh[0], sh[1], k->gs_fx && mod == vka_shader_gs_module(s));
            if (lm) mod = lm;
        }
        if (!mod) return 0;
        // A task or mesh module that reads its subgroup lanes, with its specialization constants folded in instead of VkSpecializationInfo (vka_shader_spec_module)
        VkShaderModule frz = order[i] >= 6 ? vka_shader_spec_module(s, gl) : VK_NULL_HANDLE;
        if (frz) mod = frz;
        // Float textures and images bound to integer texture views (vka_draw.c tex_masks): the module above with them retyped (vka_shader_tex_module); not with the geometry claim
        int rg = vka_shader_tex_range(s);
        if (rg >= 0 && rg < 5 && (k->tex_u[rg] | k->tex_s[rg])) {
            vka_texmod_key tk = {fs ? (k->fs_variant == VKA_FS_RETYPE ? DMSH_VK_MOD_BASE : k->fs_variant) : DMSH_VK_MOD_BASE, order[i] == 0 ? k->vin_u : 0, order[i] == 0 ? k->vin_s : 0,
                                 fu, fsm, frz ? gl : -1, k->tex_u[rg], k->tex_s[rg]};
            VkShaderModule t = order[i] == 2 && k->gs_fx ? VK_NULL_HANDLE : vka_shader_tex_module(s, &tk);
            if (t) mod = t;
            else VRED_ONCE("%016llx: a stage module with its textures retyped was not made: its integer texture views are read as floats", (unsigned long long)s->key);
        }
        uint32_t n = P->ns++;
        P->ss[n] = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = (VkShaderStageFlagBits)stage_bit(s->e->stage), .module = mod, .pName = "main",
                                                    .pSpecializationInfo = frz ? NULL : gl ? &P->spec : fs && P->afunc && (s->e->flags & DMSH_VK_F_ALPHA_TEST) ? &P->aspec : NULL};
        uint32_t pin = (s->e->flags & DMSH_VK_F_SUBGROUP) ? vka_shader_sg_pin(s->e->stage) : 0;   // subgroup shaders of a stage whose size the device varies: pinned to the library's (vka_shader_sg_pin)
        if (pin) { P->rsg[n] = (VkPipelineShaderStageRequiredSubgroupSizeCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, .requiredSubgroupSize = pin};
                   P->ss[n].pNext = &P->rsg[n]; }
    }
    uint32_t nvb = 0, nva = 0, nvd = 0;
    if (k->vin_static) {
        for (uint32_t i = 0; i < 32; i++) if (k->va[i].used) P->va[nva++] = (VkVertexInputAttributeDescription){i, k->va[i].binding, (VkFormat)k->va[i].fmt, k->va[i].offset};
        for (uint32_t i = 0; i < 33; i++) if (k->vb[i].used) {
            P->vb[nvb++] = (VkVertexInputBindingDescription){i, k->vb[i].stride, k->vb[i].divisor ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
            if (k->vb[i].divisor > 1) P->vd[nvd++] = (VkVertexInputBindingDivisorDescription){i, k->vb[i].divisor};
        }
    }
    P->vds = (VkPipelineVertexInputDivisorStateCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO, .vertexBindingDivisorCount = nvd, .pVertexBindingDivisors = P->vd};
    P->vi = (VkPipelineVertexInputStateCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .pNext = nvd ? &P->vds : NULL, .vertexBindingDescriptionCount = nvb, .pVertexBindingDescriptions = P->vb,
                                                   .vertexAttributeDescriptionCount = nva, .pVertexAttributeDescriptions = P->va};
    P->ia = (VkPipelineInputAssemblyStateCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = (VkPrimitiveTopology)k->topo};
    P->ts = (VkPipelineTessellationStateCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO, .patchControlPoints = 3};
    P->vp = (VkPipelineViewportStateCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};   // one viewport, as nvn_mtl.m draws
    P->rs = (VkPipelineRasterizationStateCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .depthClampEnable = k->depth_clamp && V.depth_clamp,
        .polygonMode = (VkPolygonMode)k->poly_mode, .lineWidth = 1.0f};
    P->ms = (VkPipelineMultisampleStateCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = (VkSampleCountFlagBits)(k->samples ? k->samples : 1), .alphaToCoverageEnable = k->a2c};
    P->ds = (VkPipelineDepthStencilStateCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, .maxDepthBounds = 1.0f};
    for (int i = 0; i < k->ncolor; i++)
        P->cb[i] = (VkPipelineColorBlendAttachmentState){k->att[i].en, (VkBlendFactor)k->att[i].src, (VkBlendFactor)k->att[i].dst, (VkBlendOp)k->att[i].op,
                                                          (VkBlendFactor)k->att[i].srca, (VkBlendFactor)k->att[i].dsta, (VkBlendOp)k->att[i].opa, k->att[i].mask};
    P->bs = (VkPipelineColorBlendStateCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .logicOpEnable = k->logic_en, .logicOp = (VkLogicOp)k->logic_op, .attachmentCount = k->ncolor, .pAttachments = P->cb};
    static const VkDynamicState base[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE,
        VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE,
        VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE, VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        VK_DYNAMIC_STATE_LINE_WIDTH};
    uint32_t nd = 0;
    for (size_t i = 0; i < sizeof base / sizeof *base; i++)   // (a mesh pipeline may not name the primitive topology or restart: it has no input assembly)
        if (!mesh || (base[i] != VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY && base[i] != VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE)) P->dyn[nd++] = base[i];
    if (!k->vin_static && !mesh) P->dyn[nd++] = VK_DYNAMIC_STATE_VERTEX_INPUT_EXT;
    P->dy = (VkPipelineDynamicStateCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = nd, .pDynamicStates = P->dyn};
    for (int i = 0; i < k->ncolor; i++) P->cf[i] = (VkFormat)k->cfmt[i];
    P->ri = (VkPipelineRenderingCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = k->ncolor, .pColorAttachmentFormats = P->cf,
                                            .depthAttachmentFormat = (VkFormat)k->dfmt, .stencilAttachmentFormat = (VkFormat)k->sfmt};
    P->tess = !mesh && (sh[3] || sh[4]); P->mesh = mesh;
    P->ci = (VkGraphicsPipelineCreateInfo){VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = vka_classic_rp() ? P->ri.pNext : &P->ri, .stageCount = P->ns, .pStages = P->ss, .pVertexInputState = mesh ? NULL : &P->vi,
        .pInputAssemblyState = mesh ? NULL : &P->ia,
        .pTessellationState = P->tess ? &P->ts : NULL, .pViewportState = &P->vp, .pRasterizationState = &P->rs, .pMultisampleState = &P->ms, .pDepthStencilState = &P->ds, .pColorBlendState = &P->bs,
        .pDynamicState = &P->dy, .layout = lay->layout};
    if (vka_classic_rp()) { P->ci.renderPass = vka_rp_compat(P->cf, k->ncolor, (VkFormat)k->dfmt, (VkFormat)k->sfmt, k->samples); P->ci.subpass = 0; }   // DM_VK_CLASSIC_RP
    P->set0 = lay->set0;
    return 1;
}
static const char *stage_name(VkShaderStageFlagBits b) { return b == VK_SHADER_STAGE_VERTEX_BIT ? "vertex" : b == VK_SHADER_STAGE_FRAGMENT_BIT ? "fragment" : b == VK_SHADER_STAGE_GEOMETRY_BIT ? "geometry"
                                                              : b == VK_SHADER_STAGE_TASK_BIT_EXT ? "task" : b == VK_SHADER_STAGE_MESH_BIT_EXT ? "mesh" : "tessellation"; }
// Pipelines being made now (whole pipelines, any thread): the watchdog's dump names them (vka_pso_wait_dump), so a stop inside the driver's compiler says which shaders
// (a frame-pointer backtrace ends in the stripped driver). Slots are claimed with a compare-and-swap; a build that finds none full is simply not listed
enum { NBUILD = 16 };
static struct { _Atomic pid_t tid; _Atomic uint64_t t0, vs, fs; const char *_Atomic what; } g_build[NBUILD];
static void vka_pso_wait_dump(void) {
    uint64_t t = now_ns(); int n = 0;
    for (int i = 0; i < NBUILD; i++) { pid_t id = atomic_load(&g_build[i].tid); if (!id) continue; n++;
        fprintf(stderr, "[dm] [vk] %s being made by thread %d for %.1f s: shaders %016llx %016llx\n", atomic_load(&g_build[i].what), (int)id, (t - atomic_load(&g_build[i].t0)) / 1e9,
                (unsigned long long)atomic_load(&g_build[i].vs), (unsigned long long)atomic_load(&g_build[i].fs)); }
    if (!n) fprintf(stderr, "[dm] [vk] no pipeline being made\n");
}
void crash_set_wait_dump(void (*fn)(void));
static void build_reg(void) { crash_set_wait_dump(vka_pso_wait_dump); }
static int build_begin(const char *what, uint64_t vs, uint64_t fs) {
    static pthread_once_t o = PTHREAD_ONCE_INIT; pthread_once(&o, build_reg);
    for (int i = 0; i < NBUILD; i++) { pid_t z = 0; if (atomic_compare_exchange_strong(&g_build[i].tid, &z, -1)) {
        atomic_store(&g_build[i].what, what); atomic_store(&g_build[i].vs, vs); atomic_store(&g_build[i].fs, fs); atomic_store(&g_build[i].t0, now_ns()); atomic_store(&g_build[i].tid, gettid()); return i; } }
    return -1;
}
static void build_end(int i) { if (i >= 0) atomic_store(&g_build[i].tid, 0); }
// The whole pipeline (optimized), with creation feedback (core in 1.3) for the line of a slow one: whether the pipeline cache had it and each stage's share. how: NULL = on the
// frame path (counted as the frame's), else where it was made (" in the background", " (prewarm)")
static VkPipeline full_pipeline(pstate *P, vka_shader *const sh[VKA_NSTAGE], const char *how) {
    VkPipelineCreationFeedback fb_all = {0}, fb_st[5]; memset(fb_st, 0, sizeof fb_st);
    VkPipelineCreationFeedbackCreateInfo fbi = {VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO, .pNext = P->ci.pNext, .pPipelineCreationFeedback = &fb_all,
                                                .pipelineStageCreationFeedbackCount = P->ns, .pPipelineStageCreationFeedbacks = fb_st};
    P->ci.pNext = &fbi;
    uint64_t t0 = now_ns();
    VkPipeline p = VK_NULL_HANDLE;
    const vka_shader *pre0 = P->mesh ? sh[7] : sh[0];
    int bslot = build_begin("whole graphics pipeline", pre0 ? pre0->key : 0, sh[1] ? sh[1]->key : 0);
    VkResult r = vk.vkCreateGraphicsPipelines(V.dev, g_pc, 1, &P->ci, NULL, &p);
    build_end(bslot);
    P->ci.pNext = fbi.pNext;
    uint64_t dns = now_ns() - t0; double ms_ = dns / 1e6;
    pthread_mutex_lock(&g_mu);
    if (r) { p = VK_NULL_HANDLE; g_nfail++; } else { g_ngfx++; g_made++; if (!how) g_frame_made++; }
    g_create_ms += ms_; if (!how) g_create_ns += dns;   // made in the background or by prewarm: not the frame's time
    pthread_mutex_unlock(&g_mu);
    if (!how && !r && ms_ > 1000) vka_pso_save(2);   // a slow one on the frame path: saved within seconds, not at the next submission (see vka_pso_save)
    const vka_shader *pre = P->mesh ? sh[7] : sh[0];   // the vertex shader, or a mesh pipeline's mesh shader
    if (r) {   // every stage named: a geometry or tessellation shader in the pipeline is as likely to be the one the driver's compiler refused (DKB's terrain pipeline, 2026-11-07)
        static _Atomic int n;
        if (atomic_fetch_add(&n, 1) < 16) {
            char mid[96]; size_t o = 0; mid[0] = 0;
            if (!P->mesh && sh[3]) o += (size_t)snprintf(mid + o, sizeof mid - o, " tess control %016llx", (unsigned long long)sh[3]->key);
            if (!P->mesh && sh[4] && o < sizeof mid) o += (size_t)snprintf(mid + o, sizeof mid - o, " tess evaluation %016llx", (unsigned long long)sh[4]->key);
            if (!P->mesh && sh[2] && o < sizeof mid) o += (size_t)snprintf(mid + o, sizeof mid - o, " geometry %016llx", (unsigned long long)sh[2]->key);
            VRED("graphics pipeline (%s %016llx%s fragment %016llx, %u colour targets, topology %u): %d; its draws are skipped", P->mesh ? "mesh" : "vertex",
                 pre ? (unsigned long long)pre->key : 0ull, mid, sh[1] ? (unsigned long long)sh[1]->key : 0ull, P->ri.colorAttachmentCount, P->mesh ? 0u : P->ia.topology, r);
        }
    }
    else if (P->mesh) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 32) VLOG("mesh pipeline: task %016llx mesh %016llx fragment %016llx, %.1f ms%s", sh[6] ? (unsigned long long)sh[6]->key : 0ull,
                                                                                      (unsigned long long)sh[7]->key, sh[1] ? (unsigned long long)sh[1]->key : 0ull, ms_, how ? how : ""); }
    else if (ms_ > 50) {
        static _Atomic int n;
        if (atomic_fetch_add(&n, 1) < 200) {
            char st[160]; size_t o = 0; st[0] = 0;
            for (uint32_t i = 0; i < P->ns && o < sizeof st; i++)
                o += (size_t)snprintf(st + o, sizeof st - o, "%s%s %.0f ms%s", i ? ", " : "", stage_name(P->ss[i].stage), (fb_st[i].flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT) ? fb_st[i].duration / 1e6 : -1.0,
                                      (fb_st[i].flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT) ? " (cached)" : "");
            VLOG("graphics pipeline (%s %016llx fragment %016llx) took %.0f ms%s (pipeline cache %s; %s)", P->mesh ? "mesh" : "vertex", pre ? (unsigned long long)pre->key : 0ull, sh[1] ? (unsigned long long)sh[1]->key : 0ull, ms_, how ? how : "",
                 !(fb_all.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT) ? "?" : (fb_all.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT) ? "hit" : "miss", st[0] ? st : "no stage times");
        }
    }
    return p;
}

// ---------- Graphics pipeline libraries (VK_EXT_graphics_pipeline_library, V.gpl) ----------
// Making a pipeline the cache lacks took 60-160 ms on the Adreno 840's own driver (500 ms at the phone's 1 GHz thermal cap), on the replay thread in the middle of a frame:
// the frame's GPU work waited for the rest of its recording, and the game's frame-rate controller, which times frame start to GPU end, dropped to 20 fps for a moment.
// With libraries the pipeline is linked without optimization from four parts, each kept by its own key: vertex input, the pre-rasterization shaders, the fragment shader
// and the fragment output. Parts recur far more than whole pipelines (a fragment shader with many vertex shaders, the same targets and blending), so a miss usually compiles
// one stage, or none. The optimized whole pipeline is then made on a background thread (into the pipeline cache) and takes the linked one's place in the map; draws recorded
// before that keep the linked one, which stays alive with the process like every pipeline here.
// Found 2026-11-03 (stutter-agent cold2gpl / cold3gpl / gpl-2335 logs): Qualcomm's Adreno 840 driver (build 718a07726a) refused EVERY pre-rasterization library with
// VK_ERROR_UNKNOWN (-13), 0 made in any run: no pipeline was ever linked, each miss paid a vertex-input library, a refused library and then the whole pipeline on the frame
// path (cold3gpl 70 ms a pipeline against 59 without GPL). The first refusal now runs a probe (pre_probe): the same library made with one thing changed at a time, each
// result logged; a change that makes it is adopted for the run (g_pre_adapt), and with none, GPL turns itself off after DM_VK_GPL_GIVEUP refusals (default 3) with no
// pre-rasterization library made, so a miss costs the whole pipeline only
enum { L_VI, L_PRE, L_FS, L_FO, L_PREFS, NLIB };
typedef struct { uint8_t topo; uint32_t vin_static; uint8_t va[sizeof(((vka_gkey *)0)->va)], vb[sizeof(((vka_gkey *)0)->vb)]; } vikey;
typedef struct { VkShaderModule mod[4]; VkShaderStageFlagBits stage[4]; uint32_t pin[4]; uint8_t gl[4]; VkPipelineLayout layout; uint8_t depth_clamp, poly_mode, tess, vin_static; } prekey;
typedef struct { VkShaderModule mod; uint32_t pin, afunc; VkPipelineLayout layout; uint8_t samples, a2c, vin_static; } fskey;
typedef struct { uint32_t cfmt[8], dfmt, sfmt; uint8_t ncolor, samples, a2c, logic_en, logic_op, vin_static, att[8][8]; } fokey;
typedef struct { prekey pre; fskey fs; } prefskey;
static kmap g_lib[NLIB] = {{.klen = sizeof(vikey)}, {.klen = sizeof(prekey)}, {.klen = sizeof(fskey)}, {.klen = sizeof(fokey)}, {.klen = sizeof(prefskey)}};
static uint64_t g_nlib[NLIB], g_nlinked, g_nopt; static double g_lib_ms, g_link_ms;
static const char *const g_lib_name[NLIB] = {"vertex input", "pre-rasterization", "fragment shader", "fragment output", "pre-rasterization + fragment shader"};
enum { A_NOCACHE = 1, A_RETAIN = 2, A_NORI = 4, A_COMBINED = 8 };
static unsigned g_pre_adapt; static int g_pre_probed, g_pre_refused; static _Atomic int g_gpl_off;
static int gpl_on(void) { return V.gpl && !atomic_load(&g_gpl_off) && !vka_classic_rp(); }   // (classic render passes: full pipelines only)
// A library's map value: the library, LIB_REFUSED (the driver refused it: the whole pipeline is made instead) or LIB_BUSY (a thread is making it). Several threads link
// (the replay thread, the DM_VK_PSO_ASYNC threads) and often miss the same part at once (one fragment shader under many vertex shaders): the first claims the key with
// LIB_BUSY and makes it, the others wait for it on g_lib_cv instead of compiling it again (each copy a slow compile, a second map entry and a leaked library). A thread
// holds at most one claim and gives it up before it asks for its next part, so the waits cannot form a cycle
#define LIB_REFUSED ((void *)(uintptr_t)1)
#define LIB_BUSY ((void *)(uintptr_t)2)
static pthread_cond_t g_lib_cv = PTHREAD_COND_INITIALIZER;
static VkPipeline lib_make(int kind, const void *key, size_t klen, VkGraphicsPipelineCreateInfo *ci, VkGraphicsPipelineLibraryFlagsEXT part, int *refused) {
    uint64_t h = fnv(key, klen, 1469598103934665603ull);
    pthread_mutex_lock(&g_mu);
    void *hit;
    while ((hit = km_get(&g_lib[kind], key, h)) == LIB_BUSY) pthread_cond_wait(&g_lib_cv, &g_mu);   // (looked up again: an adaptation may have emptied the map meanwhile)
    if (!hit) km_put(&g_lib[kind], key, h, LIB_BUSY);
    pthread_mutex_unlock(&g_mu);
    if (hit) return hit == LIB_REFUSED ? VK_NULL_HANDLE : (VkPipeline)hit;
    int shp = kind == L_PRE || kind == L_PREFS;
    VkGraphicsPipelineLibraryCreateInfoEXT li = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT, .pNext = shp && (g_pre_adapt & A_NORI) ? NULL : ci->pNext, .flags = part};
    ci->pNext = &li; ci->flags |= VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | (g_pre_adapt & A_RETAIN ? VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT : 0);
    uint64_t t0 = now_ns();
    int bs = build_begin(g_lib_name[kind], 0, 0);
    VkPipeline p = VK_NULL_HANDLE; VkResult r = vk.vkCreateGraphicsPipelines(V.dev, shp && (g_pre_adapt & A_NOCACHE) ? VK_NULL_HANDLE : g_pc, 1, ci, NULL, &p);
    build_end(bs);
    ci->pNext = li.pNext;
    double ms_ = (now_ns() - t0) / 1e6;
    pthread_mutex_lock(&g_mu);
    if (r) p = VK_NULL_HANDLE; else g_nlib[kind]++;
    g_lib_ms += ms_;
    void **slot = km_slot(&g_lib[kind], key, h);   // (gone if an adaptation emptied the map while it was made: then not kept, as it was made without the adaptation)
    if (slot) *slot = p ? (void *)p : LIB_REFUSED;
    pthread_cond_broadcast(&g_lib_cv);
    pthread_mutex_unlock(&g_mu);
    if (r) { if (refused) *refused = r; static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VRED("graphics pipeline library (%s): %d; the whole pipeline is made instead", g_lib_name[kind], r); }
    else if (ms_ > 50) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 100) VLOG("graphics pipeline library (%s) took %.0f ms", g_lib_name[kind], ms_); }
    return p;
}
// Each library names only the dynamic states of its own part (the Adreno 840's driver refused a pre-rasterization library given the whole list, VK_ERROR_UNKNOWN);
// the link takes them from the libraries
static int dyn_part(VkDynamicState d) {
    switch (d) {
    case VK_DYNAMIC_STATE_VERTEX_INPUT_EXT: case VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY: case VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE: return L_VI;
    case VK_DYNAMIC_STATE_VIEWPORT: case VK_DYNAMIC_STATE_SCISSOR: case VK_DYNAMIC_STATE_LINE_WIDTH: case VK_DYNAMIC_STATE_DEPTH_BIAS: case VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE:
    case VK_DYNAMIC_STATE_CULL_MODE: case VK_DYNAMIC_STATE_FRONT_FACE: case VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE: return L_PRE;
    case VK_DYNAMIC_STATE_BLEND_CONSTANTS: return L_FO;
    default: return L_FS;   // depth and stencil tests, depth bounds
    }
}
static void dyn_of(const pstate *P, int part, VkDynamicState *out, VkPipelineDynamicStateCreateInfo *dy) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < P->dy.dynamicStateCount; i++) { int d = dyn_part(P->dyn[i]); if (d == part || (part == L_PREFS && (d == L_PRE || d == L_FS))) out[n++] = P->dyn[i]; }
    *dy = (VkPipelineDynamicStateCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = n, .pDynamicStates = out};
}
// One try of the probe: the library made (and destroyed), its result
static VkResult try_lib(VkGraphicsPipelineCreateInfo ci, VkGraphicsPipelineLibraryFlagsEXT part, VkPipelineCache pc, VkPipelineCreateFlags extra) {
    VkGraphicsPipelineLibraryCreateInfoEXT li = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT, .pNext = ci.pNext, .flags = part};
    ci.pNext = &li; ci.flags |= VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | extra;
    int bs = build_begin("linked graphics pipeline", 0, 0);
    VkPipeline p = VK_NULL_HANDLE; VkResult r = vk.vkCreateGraphicsPipelines(V.dev, pc, 1, &ci, NULL, &p);
    build_end(bs);
    if (!r && p) vk.vkDestroyPipeline(V.dev, p, NULL);
    return r;
}
// The probe at the first refused pre-rasterization library (once a run): the same library with one thing changed at a time, then all the simplifications together. Logged as one
// line; returns the adaptation to use from now on (0 = none of the adoptable ones made it)
static unsigned pre_probe(const VkGraphicsPipelineCreateInfo *ci0, const pstate *P, const VkPipelineShaderStageCreateInfo *pre, uint32_t npre, const VkPipelineShaderStageCreateInfo *fs, VkResult first) {
    char line[900]; size_t o = 0; line[0] = 0;
#define NOTE(name, r) do { if (o < sizeof line) o += (size_t)snprintf(line + o, sizeof line - o, "%s%s %s", o ? ", " : "", name, (r) ? "refused" : "made"); \
                           if ((r) && o < sizeof line) o += (size_t)snprintf(line + o, sizeof line - o, " (%d)", (int)(r)); } while (0)
    VkPipelineCreateFlags RET = VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT;
    VkResult r_same = try_lib(*ci0, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT, g_pc, 0); NOTE("again as made", r_same);
    VkResult r_nc = try_lib(*ci0, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT, VK_NULL_HANDLE, 0); NOTE("no pipeline cache", r_nc);
    VkResult r_ret = try_lib(*ci0, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT, g_pc, RET); NOTE("RETAIN_LINK_TIME_OPTIMIZATION_INFO", r_ret);
    VkGraphicsPipelineCreateInfo c = *ci0; c.pNext = NULL;
    VkResult r_nori = try_lib(c, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT, g_pc, 0); NOTE("no VkPipelineRenderingCreateInfo", r_nori);
    VkViewport vp0 = {0, 0, 64, 64, 0, 1}; VkRect2D sc0 = {{0, 0}, {64, 64}};
    VkPipelineViewportStateCreateInfo vps = P->vp; vps.pViewports = &vp0; vps.pScissors = &sc0;
    c = *ci0; c.pViewportState = &vps; c.pDynamicState = NULL;
    VkResult r_nodyn = try_lib(c, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT, g_pc, 0); NOTE("no dynamic state", r_nodyn);
    VkPipelineShaderStageCreateInfo st[4]; int pinned = 0;
    for (uint32_t i = 0; i < npre; i++) { st[i] = pre[i]; st[i].pSpecializationInfo = NULL; }
    c = *ci0; c.pStages = st;
    VkResult r_nospec = try_lib(c, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT, g_pc, 0); NOTE("no specialization constants", r_nospec);
    for (uint32_t i = 0; i < npre; i++) { st[i] = pre[i]; if (st[i].pNext) pinned = 1; st[i].pNext = NULL; }
    if (pinned) { VkResult r = try_lib(c, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT, g_pc, 0); NOTE("no subgroup size pins", r); }
    if (npre > 1) { c = *ci0; c.stageCount = 1; c.pStages = pre; c.pTessellationState = NULL; VkResult r = try_lib(c, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT, g_pc, 0); NOTE("first stage only", r); }
    {   // The pipeline layout with independent sets (diagnostic: a pipeline of such a layout binds only with it, not adopted)
        VkDescriptorSetLayout sets[2] = {P->set0, vka_bindless_layout()}; VkPipelineLayout il = VK_NULL_HANDLE;
        VkPipelineLayoutCreateInfo pi = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .flags = VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT, .setLayoutCount = sets[1] ? 2 : 1, .pSetLayouts = sets};
        if (P->set0 && !vk.vkCreatePipelineLayout(V.dev, &pi, NULL, &il)) { c = *ci0; c.layout = il; VkResult r = try_lib(c, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT, g_pc, 0); NOTE("independent-sets layout", r); vk.vkDestroyPipelineLayout(V.dev, il, NULL); }
    }
    VkResult r_comb = VK_ERROR_UNKNOWN;
    {   // Pre-rasterization and the fragment shader in one library
        VkPipelineShaderStageCreateInfo both[5]; uint32_t nb = 0; for (uint32_t i = 0; i < npre; i++) both[nb++] = pre[i]; if (fs) both[nb++] = *fs;
        VkDynamicState dl[24]; VkPipelineDynamicStateCreateInfo dy; dyn_of(P, L_PREFS, dl, &dy);
        c = *ci0; c.stageCount = nb; c.pStages = both; c.pMultisampleState = &P->ms; c.pDepthStencilState = &P->ds; c.pDynamicState = &dy;
        r_comb = try_lib(c, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT | VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT, g_pc, 0); NOTE("with the fragment shader", r_comb);
        c.pDynamicState = NULL; c.pViewportState = &vps;
        VkResult r = try_lib(c, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT | VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT, g_pc, 0); NOTE("with the fragment shader, no dynamic state", r);
    }
    {   // Everything at once: no cache, no rendering info, no dynamic state, no specialization constants, no pins, the first stage only
        VkPipelineShaderStageCreateInfo s1 = pre[0]; s1.pSpecializationInfo = NULL; s1.pNext = NULL;
        c = *ci0; c.pNext = NULL; c.pViewportState = &vps; c.pDynamicState = NULL; c.stageCount = 1; c.pStages = &s1; c.pTessellationState = NULL;
        VkResult r = try_lib(c, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT, VK_NULL_HANDLE, RET); NOTE("all simplified", r);
    }
#undef NOTE
    unsigned a = !r_same ? 0 : !r_nc ? A_NOCACHE : !r_ret ? A_RETAIN : !r_nori ? A_NORI : !r_comb ? A_COMBINED : 0;
    VLOG("graphics pipeline library probe (pre-rasterization refused with %d; vertex %s, %u stage%s): %s; %s", first, npre ? stage_name(pre[0].stage) : "-", npre, npre == 1 ? "" : "s", line,
         !r_same ? "the second try made it (a transient refusal)" : a == A_NOCACHE ? "adopted: libraries without the pipeline cache" : a == A_RETAIN ? "adopted: RETAIN_LINK_TIME_OPTIMIZATION_INFO"
         : a == A_NORI ? "adopted: no VkPipelineRenderingCreateInfo" : a == A_COMBINED ? "adopted: one library for pre-rasterization and the fragment shader" : "nothing adoptable");
    return a;
}
// The parts of P, each made once; then the link. VK_NULL_HANDLE = a part failed (the caller makes the whole pipeline)
static VkPipeline gpl_link(pstate *P, const vka_gkey *k) {
    VkDynamicState dl[NLIB][24]; VkPipelineDynamicStateCreateInfo dy[NLIB];
    for (int i = 0; i < NLIB; i++) dyn_of(P, i, dl[i], &dy[i]);
    VkPipeline part[4]; uint32_t nparts = 0;
    {   vikey key; memset(&key, 0, sizeof key); key.topo = k->topo; key.vin_static = k->vin_static;
        if (k->vin_static) { memcpy(key.va, k->va, sizeof key.va); memcpy(key.vb, k->vb, sizeof key.vb); }
        VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pVertexInputState = &P->vi, .pInputAssemblyState = &P->ia, .pDynamicState = &dy[L_VI]};
        if (!(part[nparts++] = lib_make(L_VI, &key, sizeof key, &ci, VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT, NULL))) return VK_NULL_HANDLE; }
    VkPipelineShaderStageCreateInfo pre[4], *fs = NULL; uint32_t npre = 0;
    for (uint32_t i = 0; i < P->ns; i++) { if (P->ss[i].stage == VK_SHADER_STAGE_FRAGMENT_BIT) fs = &P->ss[i]; else if (npre < 4) pre[npre++] = P->ss[i]; }
    VkPipelineRenderingCreateInfo ri0 = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};   // the shader parts take only the view mask (0); the formats are the output's
    prekey pk; memset(&pk, 0, sizeof pk);
    for (uint32_t i = 0; i < npre; i++) { pk.mod[i] = pre[i].module; pk.stage[i] = pre[i].stage; pk.gl[i] = pre[i].pSpecializationInfo != NULL;
                                          pk.pin[i] = pre[i].pNext ? ((const VkPipelineShaderStageRequiredSubgroupSizeCreateInfo *)pre[i].pNext)->requiredSubgroupSize : 0; }
    pk.layout = P->ci.layout; pk.depth_clamp = (uint8_t)P->rs.depthClampEnable; pk.poly_mode = (uint8_t)P->rs.polygonMode; pk.tess = (uint8_t)P->tess; pk.vin_static = (uint8_t)k->vin_static;
    fskey fk; memset(&fk, 0, sizeof fk);
    if (fs) { fk.mod = fs->module; fk.afunc = fs->pSpecializationInfo ? P->afunc : 0; fk.pin = fs->pNext ? ((const VkPipelineShaderStageRequiredSubgroupSizeCreateInfo *)fs->pNext)->requiredSubgroupSize : 0; }
    fk.layout = P->ci.layout; fk.samples = (uint8_t)P->ms.rasterizationSamples; fk.a2c = (uint8_t)P->ms.alphaToCoverageEnable; fk.vin_static = (uint8_t)k->vin_static;
    if (g_pre_adapt & A_COMBINED) {
        prefskey key; memset(&key, 0, sizeof key); key.pre = pk; key.fs = fk;
        VkPipelineShaderStageCreateInfo both[5]; uint32_t nb = 0; for (uint32_t i = 0; i < npre; i++) both[nb++] = pre[i]; if (fs) both[nb++] = *fs;
        VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri0, .stageCount = nb, .pStages = both, .pTessellationState = P->tess ? &P->ts : NULL,
                                           .pViewportState = &P->vp, .pRasterizationState = &P->rs, .pMultisampleState = &P->ms, .pDepthStencilState = &P->ds, .pDynamicState = &dy[L_PREFS], .layout = P->ci.layout};
        if (!(part[nparts++] = lib_make(L_PREFS, &key, sizeof key, &ci, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT | VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT, NULL))) return VK_NULL_HANDLE;
    } else {
        VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri0, .stageCount = npre, .pStages = pre, .pTessellationState = P->tess ? &P->ts : NULL,
                                           .pViewportState = &P->vp, .pRasterizationState = &P->rs, .pDynamicState = &dy[L_PRE], .layout = P->ci.layout};
        int refused = 0;
        VkGraphicsPipelineCreateInfo ci_probe = ci;   // (lib_make chains its own structure onto ci and restores it)
        if (!(part[nparts++] = lib_make(L_PRE, &pk, sizeof pk, &ci, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT, &refused))) {
            if (refused && !__atomic_load_n(&g_nlib[L_PRE], __ATOMIC_RELAXED)) {
                int first = 0;
                pthread_mutex_lock(&g_mu); int nref = ++g_pre_refused; if (!g_pre_probed) { g_pre_probed = 1; first = 1; } pthread_mutex_unlock(&g_mu);
                // DM_VK_GPL_PROBE=1 (default 0 since 2026-11-03): the probe makes ~11-13 libraries synchronously on the replay thread, a 1-6 s hitch in the middle of a frame at
                // every run's first refusal; it is a one-off diagnostic run (android/STUTTER-TEST.md step 1), not something every run should pay for
                if (first && VKA_ENV("DM_VK_GPL_PROBE", 0)) {
                    unsigned a = pre_probe(&ci_probe, P, pre, npre, fs, (VkResult)refused);
                    if (a) { pthread_mutex_lock(&g_mu); g_pre_adapt = a; for (int i = L_PRE; i < NLIB; i += L_PREFS - L_PRE) { free(g_lib[i].e); g_lib[i].e = NULL; g_lib[i].cap = g_lib[i].n = 0; } pthread_mutex_unlock(&g_mu); }   // (the refused pre-rasterization entries: tried again with the adaptation)
                }
                int giveup = VKA_ENV("DM_VK_GPL_GIVEUP", 3);
                if (!g_pre_adapt && giveup > 0 && nref >= giveup && !atomic_exchange(&g_gpl_off, 1))
                    VLOG("graphics pipeline libraries: off for this run: %d pre-rasterization libraries refused and none made (the probe found nothing to adopt); a pipeline the cache lacks is made whole (DM_VK_GPL_GIVEUP=0 keeps trying)", nref);
            }
            return VK_NULL_HANDLE;
        }
        VkGraphicsPipelineCreateInfo cf = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri0, .stageCount = fs ? 1 : 0, .pStages = fs, .pMultisampleState = &P->ms,
                                           .pDepthStencilState = &P->ds, .pDynamicState = &dy[L_FS], .layout = P->ci.layout};
        if (!(part[nparts++] = lib_make(L_FS, &fk, sizeof fk, &cf, VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT, NULL))) return VK_NULL_HANDLE;
    }
    {   fokey key; memset(&key, 0, sizeof key);
        memcpy(key.cfmt, k->cfmt, sizeof key.cfmt); key.dfmt = k->dfmt; key.sfmt = k->sfmt; key.ncolor = k->ncolor; key.samples = (uint8_t)P->ms.rasterizationSamples; key.a2c = k->a2c;
        key.logic_en = k->logic_en; key.logic_op = k->logic_op; key.vin_static = (uint8_t)k->vin_static; memcpy(key.att, k->att, sizeof key.att);
        VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &P->ri, .pMultisampleState = &P->ms, .pColorBlendState = &P->bs, .pDynamicState = &dy[L_FO]};
        if (!(part[nparts++] = lib_make(L_FO, &key, sizeof key, &ci, VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT, NULL))) return VK_NULL_HANDLE; }
    VkPipelineLibraryCreateInfoKHR libs = {VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR, .libraryCount = nparts, .pLibraries = part};
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &libs, .layout = P->ci.layout};   // no LINK_TIME_OPTIMIZATION: the fast link
    uint64_t t0 = now_ns();
    int bs = build_begin("graphics pipeline (no cache)", 0, 0);
    VkPipeline p = VK_NULL_HANDLE; VkResult r = vk.vkCreateGraphicsPipelines(V.dev, VK_NULL_HANDLE, 1, &ci, NULL, &p);
    build_end(bs);
    pthread_mutex_lock(&g_mu); g_link_ms += (now_ns() - t0) / 1e6; if (!r) { g_nlinked++; __atomic_store_n(&g_nlinked_pub, g_nlinked, __ATOMIC_RELAXED); } pthread_mutex_unlock(&g_mu);
    if (r) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VRED("graphics pipeline link: %d; the whole pipeline is made instead", r); return VK_NULL_HANDLE; }
    return p;
}

// The background thread that makes the optimized pipelines, one after another, oldest first; each takes its linked pipeline's place in its map entry
typedef struct opt_job { struct opt_job *next; gval *g; vka_shader *sh[VKA_NSTAGE]; vka_layout lay; vka_gkey k; } opt_job;
static opt_job *g_opt_head, **g_opt_tail = &g_opt_head; static pthread_mutex_t g_opt_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_opt_cv = PTHREAD_COND_INITIALIZER;
static uint64_t g_opt_waiting;
static void *opt_main(void *a) {
    (void)a; pthread_setname_np(pthread_self(), "vk-psopt");
    for (;;) {
        pause_point();
        pthread_mutex_lock(&g_opt_mu);
        while (!g_opt_head) pthread_cond_wait(&g_opt_cv, &g_opt_mu);
        opt_job *j = g_opt_head; g_opt_head = j->next; if (!g_opt_head) g_opt_tail = &g_opt_head;
        pthread_mutex_unlock(&g_opt_mu);
        pstate *P = malloc(sizeof *P);
        VkPipeline p = P && pstate_fill(P, j->sh, &j->lay, &j->k) ? full_pipeline(P, j->sh, " in the background") : VK_NULL_HANDLE;
        pthread_mutex_lock(&g_mu);
        if (p) { __atomic_store_n(&j->g->p, p, __ATOMIC_RELEASE); __atomic_store_n(&j->g->linked, 0, __ATOMIC_RELEASE); g_nopt++; }   // a failure keeps the linked pipeline (logged)
        g_opt_waiting--;
        pthread_mutex_unlock(&g_mu);
        free(P); free(j);
    }
    return NULL;
}
static void opt_queue(gval *g, vka_shader *const sh[VKA_NSTAGE], const vka_layout *lay, const vka_gkey *k) {
    static int ok;
    opt_job *j = malloc(sizeof *j); if (!j) return;
    j->next = NULL; j->g = g; memcpy(j->sh, sh, sizeof j->sh); j->lay = *lay; j->k = *k;
    if (!ok) { pthread_t th; static _Atomic int tried; if (!atomic_exchange(&tried, 1)) { ok = !pthread_create(&th, NULL, opt_main, NULL); if (ok) pthread_detach(th); else VRED("pipeline optimizer thread not started: linked pipelines stay"); } }
    if (!ok) { free(j); return; }
    pthread_mutex_lock(&g_mu); g_opt_waiting++; pthread_mutex_unlock(&g_mu);
    pthread_mutex_lock(&g_opt_mu); *g_opt_tail = j; g_opt_tail = &j->next; pthread_cond_signal(&g_opt_cv); pthread_mutex_unlock(&g_opt_mu);
}
void vka_gpl_line(char *out, size_t n) {
    pthread_mutex_lock(&g_mu);
    if (n) snprintf(out, n, "libraries %llu vertex input / %llu pre-rasterization / %llu fragment shader / %llu fragment output / %llu combined, %.0f ms; %llu linked (%.1f ms), %llu optimized in the background, %llu waiting; "
                    "cache-only tries %llu hit / %llu missed (%.0f ms in the misses)%s",
                    (unsigned long long)g_nlib[L_VI], (unsigned long long)g_nlib[L_PRE], (unsigned long long)g_nlib[L_FS], (unsigned long long)g_nlib[L_FO], (unsigned long long)g_nlib[L_PREFS], g_lib_ms,
                    (unsigned long long)g_nlinked, g_link_ms, (unsigned long long)g_nopt, (unsigned long long)g_opt_waiting, (unsigned long long)g_probe_hit, (unsigned long long)g_probe_miss,
                    g_probe_miss_ns / 1e6, atomic_load(&g_gpl_off) ? "; OFF for this run (pre-rasterization refused)" : "");
    pthread_mutex_unlock(&g_mu);
}

// ---------- The prewarm list (2026-11-03) ----------
// The pipeline keys a run made on its frame path, kept beside the pipeline cache (vk_pipelines.keys) and made again by DM_VK_PREWARM background threads (default 2, nice 10)
// as soon as the next run makes its first pipeline: by the time the game reaches a scene, the pipelines it needs are in the map, and the frame path neither compiles nor
// even reads the cache for them. With the driver's pipeline cache warm each costs 1-4 ms on a background core; with it cold (a new driver) each is a compile, still off the
// frame path. A key names shaders by their library key and the state bytes (vka_gkey), not driver code, so one list serves every driver and survives a shader library
// change (a shader that is no longer in the library drops its records). Records whose shaders are missing are dropped at the next save; the list keeps at most
// DM_VK_PREWARM_KEEP records (default 16384), and a run makes at most DM_VK_PREWARM_MAX of them (default 8192): about 1 KB of host memory each, plus the driver's pipeline
// inst[0] / inst[1]: an instanced run's pipeline (vka_draw.c): its vertex / pixel shader is the variant of key[0] / key[1] for the uniform block in slot inst - 1 of the shader's
// own binding range (binding stage * DMSH_VK_STAGE_BINDINGS + slot; 0: the shader itself).
// Older lists have zeros there; an older build reading a newer list makes the plain pipeline of such a record, which is harmless
// The records' format, their check and the layout fingerprint: vka_pwlist.h
static pthread_mutex_t g_pw_mu = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *g_pw_buf; static size_t g_pw_len, g_pw_cap; static uint32_t g_pw_n; static int g_pw_dirty;   // every record (loaded, then new), under g_pw_mu
static uint64_t *g_pw_set; static size_t g_pw_setcap, g_pw_setn;   // their hashes
static uint8_t *g_pw_load; static size_t g_pw_load_len; static uint32_t g_pw_load_n;   // the loaded records, read by the prewarm threads (not changed after pw_load)
static _Atomic uint32_t g_pw_next, g_pw_next_mesh, g_pw_tix, g_pw_done, g_pw_made, g_pw_had, g_pw_bad; static int g_pw_threads; static uint64_t g_pw_t0; static uint32_t g_pw_seed_n;
static size_t pw_size(const pwrec *r) { return vka_pw_size(r); }
static int pw_set_add(uint64_t h) {   // 1 = new
    if (!h) h = 1;
    if (2 * (g_pw_setn + 1) > g_pw_setcap) {
        size_t nc = g_pw_setcap ? 2 * g_pw_setcap : 1024; uint64_t *n = calloc(nc, sizeof *n); if (!n) return 0;
        for (size_t i = 0; i < g_pw_setcap; i++) if (g_pw_set[i]) { size_t j = g_pw_set[i] & (nc - 1); while (n[j]) j = (j + 1) & (nc - 1); n[j] = g_pw_set[i]; }
        free(g_pw_set); g_pw_set = n; g_pw_setcap = nc;
    }
    size_t j = h & (g_pw_setcap - 1);
    for (; g_pw_set[j]; j = (j + 1) & (g_pw_setcap - 1)) if (g_pw_set[j] == h) return 0;
    g_pw_set[j] = h; g_pw_setn++; return 1;
}
// Every record is kept in memory for the run (the loaded ones first, then the new ones); DM_VK_PREWARM_KEEP applies when the list is written (pw_save keeps the newest).
// The cap was checked here against the count including the loaded records, so a list that had reached it took no new key ever again: it froze
static void pw_append(const pwrec *r, const void *kb) {   // caller holds g_pw_mu
    size_t sz = pw_size(r);
    if (g_pw_len + sz > g_pw_cap) { size_t nc = g_pw_cap ? 2 * g_pw_cap : 65536; while (nc < g_pw_len + sz) nc *= 2; uint8_t *n = realloc(g_pw_buf, nc); if (!n) return; g_pw_buf = n; g_pw_cap = nc; }
    memcpy(g_pw_buf + g_pw_len, r, sizeof *r); memset(g_pw_buf + g_pw_len + sizeof *r, 0, sz - sizeof *r); memcpy(g_pw_buf + g_pw_len + sizeof *r, kb, r->klen);
    g_pw_len += sz; g_pw_n++;
}
// A graphics pipeline's stages as a record's five slots (vka_pwlist.h): a vertex pipeline's are its first five, a mesh pipeline's mesh, fragment and task
static void pw_slots(vka_shader *const sh[VKA_NSTAGE], vka_shader *s5[5], int *kind) {
    if (sh[7]) { *kind = VKA_PW_MESH; s5[0] = sh[7]; s5[1] = sh[1]; s5[2] = sh[6]; s5[3] = s5[4] = NULL; }
    else { *kind = VKA_PW_GFX; for (int i = 0; i < 5; i++) s5[i] = sh[i]; }
}
// A pipeline the frame path made: into the list unless it is there. sh: the record's five slots (vertex: sh[0..4]; mesh: mesh, fragment, task; compute: the shader)
static void pw_note(int kind, vka_shader *const sh[5], const void *kb, uint32_t klen) {
    if (!g_pw_path[0]) return;
    pwrec r; memset(&r, 0, sizeof r); r.kind = (uint8_t)kind; r.klen = klen;
    for (int i = 0; i < 5; i++) {   // an instanced variant: its base key and its binding, so the next run makes it ahead too (its pipeline was a frame-path hitch every run)
        if (!vka_shader_is_variant(sh[i])) { r.key[i] = sh[i] ? sh[i]->key : 0; continue; }
        uint32_t lo = sh[i]->e->stage * DMSH_VK_STAGE_BINDINGS, b = sh[i]->inst_bind - 1;
        if (kind != VKA_PW_GFX || i > 1 || !sh[i]->inst_bind || b < lo || b - lo >= DMSH_VK_STAGE_BINDINGS) return;
        r.key[i] = sh[i]->e->key; r.inst[i] = (uint8_t)(b - lo + 1);
    }
    uint64_t h = vka_pw_hash(&r, kb);
    pthread_mutex_lock(&g_pw_mu);
    if (pw_set_add(h)) { pw_append(&r, kb); g_pw_dirty = 1; }
    pthread_mutex_unlock(&g_pw_mu);
}
// The records of one list into the run's (caller holds g_pw_mu), those already there skipped: the count taken, *skipped those with impossible values. -1 = not read (*why)
static int pw_take(const uint8_t *d, size_t len, uint32_t *skipped, const char **why) {
    size_t o; uint32_t want, n = 0, took = 0; int e = vka_pw_header(d, len, &o, &want);
    if (e) { *why = e == VKA_PW_LAYOUT ? "another key layout (an older or newer host)" : e == VKA_PW_FORMAT ? "another format" : "too short"; return -1; }
    pwrec r; const uint8_t *kb; int ok;
    while (n < want && vka_pw_next(d, len, &o, &r, &kb, &ok)) {
        n++;
        if (!ok) { (*skipped)++; continue; }
        r.bad = 0;
        if (pw_set_add(vka_pw_hash(&r, kb))) { pw_append(&r, kb); took++; }
    }
    return (int)took;
}
// The run's list: this device's own (<cache>.keys, the keys of its earlier runs, in the order they were first made), then the profile's seed list for those it lacks
// (vka_pwlist.h; DM_VK_PREWARM_SEED=0 leaves it out). On a first start the seed is the whole list: the prewarm threads make the pipelines the game's first scenes need
// while the game plays its logos and menus, where before every one was made on the frame path at the first 3D frame (Ocarina of Time: 128 pipelines, 79 s)
static void pw_load(void) {
    if (!g_pw_path[0]) return;
    uint32_t skipped = 0; const char *why = NULL; int own = -1, seed = -1;
    FILE *f = fopen(g_pw_path, "rb");
    uint8_t *d = NULL; long sz = 0;
    if (f) { fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET); d = sz > 0 ? malloc((size_t)sz) : NULL; if (d && fread(d, 1, (size_t)sz, f) != (size_t)sz) { free(d); d = NULL; } fclose(f); }
    pthread_mutex_lock(&g_pw_mu);
    if (!f) VLOG("prewarm list %s: none yet (written as this run makes pipelines)", g_pw_path);
    else if ((own = d ? pw_take(d, (size_t)sz, &skipped, &why) : -1) < 0) { VLOG("prewarm list %s: %s, not used (rewritten as this run makes pipelines)", g_pw_path, d ? why : "unreadable"); g_pw_dirty = 1; }
    if (skipped) { VLOG("prewarm list %s: %u records with impossible values skipped (dropped at the next save)", g_pw_path, skipped); g_pw_dirty = 1; }
    free(d);
    if (dm_vk_prewarm_seed_len && VKA_ENV("DM_VK_PREWARM_SEED", 1)) {
        uint32_t sk = 0; const char *sw = NULL;
        seed = pw_take(dm_vk_prewarm_seed, dm_vk_prewarm_seed_len, &sk, &sw);
        if (seed < 0) VRED("prewarm seed of the profile (vk_prewarm.bin, %u bytes): %s, not used: dm/tools/vk_prewarm.py makes it again from a run of this host", (unsigned)dm_vk_prewarm_seed_len, sw);
        else { g_pw_seed_n = (uint32_t)seed; if (seed) g_pw_dirty = 1; }   // (kept in this device's list from now on: the next save writes them)
    }
    g_pw_load = malloc(g_pw_len ? g_pw_len : 1); if (g_pw_load) { memcpy(g_pw_load, g_pw_buf, g_pw_len); g_pw_load_len = g_pw_len; g_pw_load_n = g_pw_n; }
    pthread_mutex_unlock(&g_pw_mu);
    VLOG("prewarm list %s: %u pipeline keys (%d of earlier runs on this device, %d more from the profile's seed list%s)", g_pw_path, g_pw_load_n, own > 0 ? own : 0, seed > 0 ? seed : 0,
         !dm_vk_prewarm_seed_len ? ": the profile has none" : seed < 0 ? ": not used" : "");
}
static int pw_save(void) {   // the saver thread or a forced save: the list when it changed; -1 = not written
    if (!g_pw_path[0]) return 0;
    pthread_mutex_lock(&g_pw_mu);
    if (!g_pw_dirty) { pthread_mutex_unlock(&g_pw_mu); return 0; }
    // Records the prewarm threads found broken (a shader not in the library any more) are left out
    size_t len = 0; uint32_t n = 0; uint8_t *out = malloc(sizeof(pwhdr2) + g_pw_len);
    if (!out) { pthread_mutex_unlock(&g_pw_mu); return -1; }
    // At most DM_VK_PREWARM_KEEP records, the newest: the oldest loaded ones go first (a key no run has needed for a while)
    uint32_t good = 0; for (size_t o = 0; o < g_pw_len;) { pwrec r; memcpy(&r, g_pw_buf + o, sizeof r); if (!r.bad) good++; o += pw_size(&r); }
    int keep = VKA_ENV("DM_VK_PREWARM_KEEP", 16384); uint32_t drop = keep > 0 && good > (uint32_t)keep ? good - (uint32_t)keep : 0;
    for (size_t o = 0; o < g_pw_len;) { pwrec r; memcpy(&r, g_pw_buf + o, sizeof r); size_t sz = pw_size(&r);
        if (!r.bad) { if (drop) drop--; else { memcpy(out + sizeof(pwhdr2) + len, g_pw_buf + o, sz); len += sz; n++; } } o += sz; }
    g_pw_dirty = 0;
    pthread_mutex_unlock(&g_pw_mu);
    pwhdr2 h; memset(&h, 0, sizeof h); memcpy(h.magic, VKA_PW_MAGIC2, 8); h.gkey_size = sizeof(vka_gkey); h.n = n; h.layout = vka_pw_layout(); memcpy(out, &h, sizeof h);
    char tmp[1200]; snprintf(tmp, sizeof tmp, "%s.tmp", g_pw_path);
    FILE *f = fopen(tmp, "wb");
    int ok = f && fwrite(out, 1, sizeof h + len, f) == sizeof h + len && !fclose(f) && !rename(tmp, g_pw_path);
    if (!ok) { if (f) fclose(f); unlink(tmp); static _Atomic int k; if (atomic_fetch_add(&k, 1) < 2) VRED("prewarm list not written to %s", g_pw_path); pthread_mutex_lock(&g_pw_mu); g_pw_dirty = 1; pthread_mutex_unlock(&g_pw_mu); }
    free(out);
    return ok ? (int)n : -1;
}
static void pw_mark_bad(size_t off) {   // the record at off of the loaded list: dropped at the next save (the loaded records are the first ones of g_pw_buf, in the same order)
    pthread_mutex_lock(&g_pw_mu);
    if (off + sizeof(pwrec) <= g_pw_len) { g_pw_buf[off + offsetof(pwrec, bad)] = 1; g_pw_dirty = 1; }
    pthread_mutex_unlock(&g_pw_mu);
}
// Why a pipeline the frame path made was not made ahead (the first DM_VK_PREWARM_WHY of them, 64; 0 = none): the loaded list's records with the same shaders, and the
// fields in which the nearest of them differs from the key asked for. "no record with these shaders" = new to the list (an area or effect no earlier run reached: prewarm
// cannot help, only GPL or a faster compile); fields named = the list held a pipeline of the same shaders in another state (a state the key keeps that earlier runs did
// not meet, or a field the key should not keep because the pipeline does not depend on it)
// where: " on the frame path", or " in the background" for an asynchronous one (DM_VK_PSO_ASYNC; ms then counts from the miss)
static void pw_explain(int kind, vka_shader *const sh[5], const vka_gkey *k, uint32_t klen, double ms, int linked, const char *where) {
    static _Atomic int nlog; int lim = VKA_ENV("DM_VK_PREWARM_WHY", 64);
    if (!g_pw_load || atomic_load(&nlog) >= lim || atomic_fetch_add(&nlog, 1) >= lim) return;
    static const struct { const char *name; size_t lo, hi; } F[] = {
        {"colour formats", offsetof(vka_gkey, cfmt), offsetof(vka_gkey, dfmt)}, {"depth format", offsetof(vka_gkey, dfmt), offsetof(vka_gkey, sfmt)},
        {"stencil format", offsetof(vka_gkey, sfmt), offsetof(vka_gkey, ncolor)}, {"colour count", offsetof(vka_gkey, ncolor), offsetof(vka_gkey, samples)},
        {"samples", offsetof(vka_gkey, samples), offsetof(vka_gkey, topo)}, {"topology", offsetof(vka_gkey, topo), offsetof(vka_gkey, poly_mode)},
        {"polygon mode", offsetof(vka_gkey, poly_mode), offsetof(vka_gkey, depth_clamp)}, {"depth clamp", offsetof(vka_gkey, depth_clamp), offsetof(vka_gkey, a2c)},
        {"alpha to coverage", offsetof(vka_gkey, a2c), offsetof(vka_gkey, logic_en)}, {"logic op", offsetof(vka_gkey, logic_en), offsetof(vka_gkey, gl_depth)},
        {"GL depth", offsetof(vka_gkey, gl_depth), offsetof(vka_gkey, fs_variant)}, {"fragment variant", offsetof(vka_gkey, fs_variant), offsetof(vka_gkey, alpha_func)},
        {"alpha func", offsetof(vka_gkey, alpha_func), offsetof(vka_gkey, gs_fx)}, {"geometry claim", offsetof(vka_gkey, gs_fx), offsetof(vka_gkey, vin_u)},
        {"vertex input retype", offsetof(vka_gkey, vin_u), offsetof(vka_gkey, tex_u)}, {"texture retype", offsetof(vka_gkey, tex_u), offsetof(vka_gkey, att)}, {"blend", offsetof(vka_gkey, att), offsetof(vka_gkey, vin_static)},
        {"static vertex input", offsetof(vka_gkey, vin_static), sizeof(vka_gkey)}};
    enum { NF = sizeof F / sizeof F[0] };
    uint64_t want[5]; for (int i = 0; i < 5; i++) want[i] = sh[i] ? sh[i]->key : 0;
    int same_sh = 0, best = -1; unsigned bestmask = 0;
    for (size_t o = 0; o + sizeof(pwrec) <= g_pw_load_len;) {
        pwrec r; memcpy(&r, g_pw_load + o, sizeof r); const uint8_t *kb = g_pw_load + o + sizeof r; o += pw_size(&r);
        if (r.kind != kind || memcmp(r.key, want, sizeof want)) continue;
        same_sh++;
        vka_gkey rk; memset(&rk, 0, sizeof rk); memcpy(&rk, kb, r.klen < sizeof rk ? r.klen : sizeof rk);
        const uint8_t *a = (const uint8_t *)&rk, *b = (const uint8_t *)k; int nd = 0; unsigned mask = 0;
        for (int f = 0; f < NF; f++) { size_t hi = F[f].hi < klen ? F[f].hi : klen; if (F[f].lo < hi && memcmp(a + F[f].lo, b + F[f].lo, hi - F[f].lo)) { nd++; mask |= 1u << f; } }
        if (best < 0 || nd < best) { best = nd; bestmask = mask; }
    }
    char d[256] = ""; size_t q = 0;
    for (int f = 0; f < NF && q < sizeof d; f++) if (bestmask >> f & 1) q += (size_t)snprintf(d + q, sizeof d - q, "%s%s", q ? ", " : "", F[f].name);
    if (!same_sh) VLOG("pipeline made%s (%.1f ms%s), not in the prewarm list: no record with these shaders (%s %016llx fs %016llx; first met in this run)", where,
                       ms, linked ? ", linked" : "", kind == VKA_PW_MESH ? "mesh" : "vs", (unsigned long long)want[0], (unsigned long long)want[1]);
    else VLOG("pipeline made%s (%.1f ms%s), not in the prewarm list: %d records with these shaders, the nearest differs in %s", where,
              ms, linked ? ", linked" : "", same_sh, d[0] ? d : "nothing: the same key (the prewarm threads had not reached it yet)");
}
static gval *gfx_obtain(const gkey_full *fk, uint64_t h, size_t len, vka_shader *const sh[VKA_NSTAGE], const vka_layout *lay, const vka_gkey *k, int prewarm, int *made);
static VkPipeline cs_obtain(vka_shader *cs, const vka_layout *lay, int prewarm, int *made, uint64_t umask, uint64_t smask);
// MemAvailable in MiB (/proc/meminfo; 0 = not known)
static uint64_t mem_avail_mb(void) {
    FILE *f = fopen("/proc/meminfo", "r"); char line[128]; uint64_t kb = 0;
    while (f && fgets(line, sizeof line, f)) if (!strncmp(line, "MemAvailable:", 13)) { kb = strtoull(line + 13, NULL, 10); break; }
    if (f) fclose(f);
    return kb >> 10;
}
static _Atomic uint64_t g_pw_memwait_ns;
static void *pw_main(void *a) {
    (void)a; pthread_setname_np(pthread_self(), "vk-prewarm");
    setpriority(PRIO_PROCESS, 0, 10);   // this thread (Linux: per-thread nice), under the game's threads
    uint32_t max = (uint32_t)VKA_ENV("DM_VK_PREWARM_MAX", 8192);
    size_t *offs = NULL;   // walked once by each thread to find the record of index i: built here (the list is small)
    // The order: mesh pipelines, then compute, then vertex pipelines, each kind in the list's order (the order earlier runs first needed them). A cold mesh pipeline took
    // 3-17 s on Qualcomm's compiler and a compute one 1-3 s, a vertex one about 0.1 s: Ocarina of Time's first 3D frame needs all of them at once, and the frame path makes
    // the cheap ones itself in less time than one slow one takes, so the slow kinds go first (seed v1 in list order left 33 mesh pipelines to the frame path: 102 s).
    // Mesh pipelines are made by one prewarm thread at a time (the first; it joins the others when they are done): four at once took the compiler's memory with them
    // (std::bad_alloc thrown in the driver, the game process aborted 30 s after launch, MemAvailable 6.0 → 2.4 GB in 10 s; run t-sys1c, 2026-11-08)
    uint32_t n = g_pw_load_n, nm = 0, me = atomic_fetch_add(&g_pw_tix, 1); offs = malloc((n ? n : 1) * sizeof *offs);
    if (offs) { static const uint8_t rank[3] = {VKA_PW_MESH, VKA_PW_CS, VKA_PW_GFX}; uint32_t i = 0;
        for (int k = 0; k < 3; k++) for (size_t o = 0, j = 0; j < n; j++) { pwrec r; memcpy(&r, g_pw_load + o, sizeof r); if (r.kind == rank[k] || (k == 2 && r.kind > VKA_PW_MESH)) offs[i++] = o; o += pw_size(&r); }
        for (uint32_t j = 0; j < n; j++) { pwrec r; memcpy(&r, g_pw_load + offs[j], sizeof r); if (r.kind != VKA_PW_MESH) break; nm++; } }
    // Every thread but the first waits while the phone has less than DM_VK_PREWARM_MEM_MB (default 2048) available: compiles take the driver hundreds of MB each, and
    // with the game's first 3D scene loaded the phone had 1.1-1.9 GB left; four threads then ran it out (Turnip run t-tur1c: MemAvailable 0.38 GB, mprotect failing with
    // ENOMEM, a crash at 83 s). The first thread goes on as the frame path would make the same pipelines one at a time anyway
    uint32_t mem_mb = (uint32_t)VKA_ENV("DM_VK_PREWARM_MEM_MB", 2048);
    for (;;) {
        pause_point();
        if (me > 0 && mem_mb) { uint64_t t0 = 0, av; while ((av = mem_avail_mb()) && av < mem_mb && atomic_load(&g_pw_next) < n) { if (!t0) t0 = now_ns(); usleep(250000); pause_point(); }
                                if (t0) atomic_fetch_add(&g_pw_memwait_ns, now_ns() - t0); }
        uint32_t i = me == 0 ? atomic_fetch_add(&g_pw_next_mesh, 1) : nm;   // the mesh thread: the next mesh record while there is one
        if (i >= nm) { uint32_t q = atomic_fetch_add(&g_pw_next, 1); i = nm + q; }
        if (!offs || i >= n || i >= max) break;
        pwrec r; memcpy(&r, g_pw_load + offs[i], sizeof r);
        const uint8_t *kb = g_pw_load + offs[i] + sizeof r;
        vka_shader *sh[VKA_NSTAGE]; memset(sh, 0, sizeof sh); int bad = !vka_pw_rec_ok(&r, kb);
        if (bad) {}
        else if (r.kind == VKA_PW_CS) { if (!(sh[5] = vka_shader_by_key(r.key[0]))) bad = 1; }
        else if (r.kind == VKA_PW_MESH) {   // the frame path's stages of a mesh draw (vka_draw.c): task 6, mesh 7, fragment 1
            if (!(sh[7] = vka_shader_by_key(r.key[0])) || !(sh[1] = vka_shader_by_key(r.key[1])) || (r.key[2] && !(sh[6] = vka_shader_by_key(r.key[2])))) bad = 1;
        }
        else {
            for (int s = 0; s < 5; s++) if (r.key[s] && vka_shader_variant_key(r.key[s])) bad = 2;   // an older list's instanced variant: dropped quietly
            for (int s = 0; s < 5 && !bad; s++) if (r.key[s] && !(sh[s] = vka_shader_by_key(r.key[s]))) bad = 1;
        }
        vka_layout lay;
        if (!bad && !vka_layout_of(sh, r.kind == VKA_PW_CS, &lay)) bad = 1;
        int skip = 0;   // an instanced run's pipeline with instancing off: not made, kept in the list
        for (int s = 0; s < 2 && !bad && !skip && r.kind == VKA_PW_GFX; s++) if (r.inst[s] && !(sh[s] = vka_inst_variant(sh[s], sh[s]->e->stage * DMSH_VK_STAGE_BINDINGS + r.inst[s] - 1u))) skip = 1;
        if (skip) continue;
        int made = 0;
        if (bad) { atomic_fetch_add(&g_pw_bad, 1); pw_mark_bad(offs[i]); }
        else if (r.kind == VKA_PW_CS) { cs_obtain(sh[5], &lay, 1, &made, 0, 0); }
        else {
            vka_gkey k; memset(&k, 0, sizeof k); memcpy(&k, kb, r.klen < sizeof k ? r.klen : sizeof k);
            if (k.vin_static != (uint32_t)!V.vin_dyn) { atomic_fetch_add(&g_pw_bad, 1); }   // a list from a device with the other vertex input (not this one's keys)
            else {
                gkey_full fk; memset(&fk, 0, sizeof fk); memcpy(fk.sh, sh, sizeof fk.sh); fk.layout = lay.layout; fk.k = k;
                size_t len = k.vin_static ? sizeof fk : offsetof(gkey_full, k.va);
                gfx_obtain(&fk, fnv(&fk, len, 1469598103934665603ull), len, sh, &lay, &k, 1, &made);
            }
        }
        if (made) atomic_fetch_add(&g_pw_made, 1); else if (!bad) atomic_fetch_add(&g_pw_had, 1);
        setpriority(PRIO_PROCESS, 0, 10);   // back under the game's threads if a waiting frame path raised it (busy_wait)
    }
    free(offs);
    if (atomic_fetch_add(&g_pw_done, 1) + 1 == (uint32_t)g_pw_threads)
        VLOG("prewarm: %u of %u pipeline keys in %.1f s on %d threads: %u made, %u already there, %u dropped (shaders no longer in the library, another device's keys); the frame path waited %.0f ms for them; "
             "threads waited %.1f s for memory (DM_VK_PREWARM_MEM_MB)",
             (unsigned)(atomic_load(&g_pw_made) + atomic_load(&g_pw_had) + atomic_load(&g_pw_bad)), n, (now_ns() - g_pw_t0) / 1e9, g_pw_threads, (unsigned)atomic_load(&g_pw_made),
             (unsigned)atomic_load(&g_pw_had), (unsigned)atomic_load(&g_pw_bad), atomic_load(&g_pw_wait_ns) / 1e6, atomic_load(&g_pw_memwait_ns) / 1e9);
    return NULL;
}
// DM_VK_PREWARM threads (default 4, at most 4; 2 before 2026-11-08). With the pipeline cache warm a key costs a few milliseconds (297 in 0.1 s on 2 threads); with it
// cold each is a compile of 60-100 ms, some vertex and mesh ones 2-9 s on Qualcomm's compiler, and Ocarina of Time's title scene alone needs about 130, so the list is
// done before the game's first 3D frame only on more threads. They run at nice 10, under the game's threads, during logos and menus that leave most cores idle
static void pw_start(void) {
    int nt = VKA_ENV("DM_VK_PREWARM", 4);
    if (nt <= 0 || !g_pw_load_n) { if (g_pw_load_n) VLOG("prewarm: off (DM_VK_PREWARM=0): %u keys not made ahead", g_pw_load_n); return; }
    if (nt > 4) nt = 4;
    g_pw_t0 = now_ns(); g_pw_threads = 0;
    for (int i = 0; i < nt; i++) { pthread_t th; if (!pthread_create(&th, NULL, pw_main, NULL)) { pthread_detach(th); g_pw_threads++; } }
    VLOG("prewarm: %u pipeline keys of earlier runs made ahead on %d background threads (DM_VK_PREWARM=0 off)", g_pw_load_n, g_pw_threads);
}

// ---------- Graphics pipelines: the lookup ----------
// The entry of a key, made by this thread when no one has it (made = 1). A key another thread is making (busy): the frame path waits for it (the time goes to the [longframe]
// line's prewarm wait), a prewarm thread goes on to its next key
// The frame path waits for another thread making the same key (caller holds g_mu). A prewarm thread runs at nice 10, under the game's threads, so the frame waited for a
// low-priority build (a priority inversion, worse than making it itself): that thread is raised to the waiter's priority for the rest of its build (pw_main puts itself back
// at nice 10 before its next key)
static void busy_wait(gval *g) {
    uint64_t t0 = now_ns();
    if (g->pw && g->tid > 0) { errno = 0; int me = getpriority(PRIO_PROCESS, 0); if (!(me == -1 && errno)) setpriority(PRIO_PROCESS, (id_t)g->tid, me); }
    while (g->busy) pthread_cond_wait(&g_busy_cv, &g_mu);
    atomic_fetch_add(&g_pw_wait_ns, now_ns() - t0);
}
// DM_VK_PSO_ASYNC=N (restart; default 0 = off): a stopgap for frame-path pipelines on slow compilers (Turnip on the Adreno 740 made Donkey Kong Bananza's fragment shader
// libraries in 77-213 ms, 96 pipelines and 13.7 s of frame waits in 40 s of wall punching). The frame path still makes the cache-only try (a fraction of a millisecond when
// the pipeline cache has it); on a miss the entry is made on one of N background threads (at most 4) and its draws are skipped until it is there, as Eden's asynchronous
// shaders do. A skipped draw is missing from that frame only, except where the game keeps what it drew (a texture drawn once and reused): that stays wrong until redrawn.
// Compute and mesh pipelines are still made on the frame path. Open: the cause, slow library compiles on Turnip, is not addressed
typedef struct as_job { struct as_job *next; gval *g; size_t len; vka_shader *sh[VKA_NSTAGE]; vka_layout lay; vka_gkey k; uint64_t t0; } as_job;
static as_job *g_as_head, **g_as_tail = &g_as_head; static pthread_mutex_t g_as_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_as_cv = PTHREAD_COND_INITIALIZER;
static _Atomic uint64_t g_as_queued, g_as_made, g_as_skips, g_as_ns, g_as_lat_max_ns; static _Atomic int g_as_depth, g_as_depth_max;
static void gfx_build(gval *g, size_t len, vka_shader *const sh[VKA_NSTAGE], const vka_layout *lay, const vka_gkey *k, int prewarm, int async, uint64_t t_make);
static void *as_main(void *a) {
    (void)a; pthread_setname_np(pthread_self(), "vk-psoasync");
    setpriority(PRIO_PROCESS, 0, 10);   // under the game's threads, as the prewarm threads: no frame waits for these builds (their draws are skipped instead)
    for (;;) {
        pause_point();
        pthread_mutex_lock(&g_as_mu);
        while (!g_as_head) pthread_cond_wait(&g_as_cv, &g_as_mu);
        as_job *j = g_as_head; g_as_head = j->next; if (!g_as_head) g_as_tail = &g_as_head;
        pthread_mutex_unlock(&g_as_mu);
        uint64_t t0 = now_ns();
        gfx_build(j->g, j->len, j->sh, &j->lay, &j->k, 0, 1, j->t0);
        uint64_t t1 = now_ns(), lat = t1 - j->t0, cur = atomic_load(&g_as_lat_max_ns);
        while (lat > cur && !atomic_compare_exchange_weak(&g_as_lat_max_ns, &cur, lat)) {}
        atomic_fetch_add(&g_as_ns, t1 - t0); atomic_fetch_add(&g_as_made, 1); atomic_fetch_sub(&g_as_depth, 1);
        free(j);
    }
    return NULL;
}
static int g_as_n;
static void as_start(void) {
    int v = VKA_ENV("DM_VK_PSO_ASYNC", 0); v = v < 0 ? 0 : v > 4 ? 4 : v;
    for (int i = 0; i < v; i++) { pthread_t th; if (!pthread_create(&th, NULL, as_main, NULL)) { pthread_detach(th); g_as_n++; } }
    if (v) VLOG("asynchronous pipelines: %d of %d background threads (DM_VK_PSO_ASYNC): a pipeline the cache lacks is made off the frame path and its draws are skipped until then", g_as_n, v);
}
static int as_threads(void) { static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, as_start); return g_as_n; }
static void as_queue(gval *g, size_t len, vka_shader *const sh[VKA_NSTAGE], const vka_layout *lay, const vka_gkey *k, uint64_t t0) {
    as_job *j = malloc(sizeof *j);
    if (!j) { gfx_build(g, len, sh, lay, k, 0, 0, t0); return; }
    j->next = NULL; j->g = g; j->len = len; memcpy(j->sh, sh, sizeof j->sh); j->lay = *lay; j->k = *k; j->t0 = t0;
    int d = atomic_fetch_add(&g_as_depth, 1) + 1, m = atomic_load(&g_as_depth_max);
    while (d > m && !atomic_compare_exchange_weak(&g_as_depth_max, &m, d)) {}
    atomic_fetch_add(&g_as_queued, 1);
    pthread_mutex_lock(&g_as_mu); *g_as_tail = j; g_as_tail = &j->next; pthread_cond_signal(&g_as_cv); pthread_mutex_unlock(&g_as_mu);
}
void vka_pso_async_line(char *out, size_t n) {
    if (!n) return;
    out[0] = 0;
    if (as_threads() <= 0) return;
    snprintf(out, n, "asynchronous: %llu queued, %llu made (%.0f ms in all, longest wait from the miss %.0f ms), deepest queue %d, draws skipped waiting %llu",
             (unsigned long long)atomic_load(&g_as_queued), (unsigned long long)atomic_load(&g_as_made), atomic_load(&g_as_ns) / 1e6, atomic_load(&g_as_lat_max_ns) / 1e6,
             atomic_load(&g_as_depth_max), (unsigned long long)atomic_load(&g_as_skips));
}
static gval *gfx_obtain(const gkey_full *fk, uint64_t h, size_t len, vka_shader *const sh[VKA_NSTAGE], const vka_layout *lay, const vka_gkey *k, int prewarm, int *made) {
    *made = 0;
    int async = !prewarm && !sh[7] && as_threads() > 0;
    pthread_mutex_lock(&g_mu);
    gval *g = km_get(&g_gfx, fk, h);
    if (g) {
        if (g->busy && (prewarm || async)) { pthread_mutex_unlock(&g_mu); return prewarm ? NULL : g; }
        if (g->busy) busy_wait(g);
        pthread_mutex_unlock(&g_mu);
        return g;
    }
    g = calloc(1, sizeof *g); g->busy = 1; g->pw = (uint8_t)(prewarm != 0); g->tid = prewarm ? gettid() : 0; km_put(&g_gfx, fk, h, g);
    pthread_mutex_unlock(&g_mu);
    uint64_t t_make = now_ns();
    if (async) {
        pc_open();
        VkPipeline p = VK_NULL_HANDLE;
        pstate *P = malloc(sizeof *P);
        if (P && gpl_on() && pstate_fill(P, sh, lay, k)) {   // (the cache-only try needs pipelineCreationCacheControl, which GPL is on with)
            P->ci.flags |= VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
            uint64_t t0 = now_ns();
            VkResult r = vk.vkCreateGraphicsPipelines(V.dev, g_pc, 1, &P->ci, NULL, &p);
            uint64_t dns = now_ns() - t0;
            if (r != VK_SUCCESS) p = VK_NULL_HANDLE;
            pthread_mutex_lock(&g_mu);
            if (p) { g_ngfx++; g_made++; g_frame_made++; g_probe_hit++; g_create_ms += dns / 1e6; } else { g_probe_miss++; g_probe_miss_ns += dns; }
            g_create_ns += dns;
            pthread_mutex_unlock(&g_mu);
        }
        free(P);
        if (p) {
            pw_note(VKA_PW_GFX, sh, k, (uint32_t)(len - offsetof(gkey_full, k))); pw_explain(VKA_PW_GFX, sh, k, (uint32_t)(len - offsetof(gkey_full, k)), (now_ns() - t_make) / 1e6, 0, " on the frame path");
            pthread_mutex_lock(&g_mu); __atomic_store_n(&g->p, p, __ATOMIC_RELEASE); __atomic_store_n(&g->busy, 0, __ATOMIC_RELEASE); pthread_cond_broadcast(&g_busy_cv); pthread_mutex_unlock(&g_mu);
            *made = 1;
            return g;
        }
        as_queue(g, len, sh, lay, k, t_make);
        *made = 1;
        return g;
    }
    gfx_build(g, len, sh, lay, k, prewarm, 0, t_make);
    *made = 1;
    return g;
}
// A new entry g (busy) made and published. async: a frame-path miss made on a background thread (as_main): linked from parts like the frame path, its time not the frame's
static void gfx_build(gval *g, size_t len, vka_shader *const sh[VKA_NSTAGE], const vka_layout *lay, const vka_gkey *k, int prewarm, int async, uint64_t t_make) {
    pc_open();
    VkPipeline p = VK_NULL_HANDLE; int linked = 0;
    pstate *P = malloc(sizeof *P);
    if (P && pstate_fill(P, sh, lay, k)) {   // (else logged by vka_shader_module)
        if (async && gpl_on() && !P->mesh) {
            uint64_t t1 = now_ns();
            p = gpl_link(P, k);
            if (p) { linked = 1; pthread_mutex_lock(&g_mu); g_ngfx++; g_create_ms += (now_ns() - t1) / 1e6; pthread_mutex_unlock(&g_mu); }
        }
        else if (!prewarm && !async && gpl_on() && !P->mesh) {   // (a mesh pipeline is made whole: the libraries here are cut for vertex input pipelines)
            // The pipeline cache may already hold the optimized one (made in an earlier run): then it is made at once (a fraction of a millisecond), else linked from parts
            P->ci.flags |= VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
            uint64_t t0 = now_ns();
            int bs = build_begin("graphics pipeline (cache probe)", 0, 0);
            VkResult r = vk.vkCreateGraphicsPipelines(V.dev, g_pc, 1, &P->ci, NULL, &p);
            build_end(bs);
            P->ci.flags &= ~VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
            uint64_t dns = now_ns() - t0;
            if (r == VK_SUCCESS && p) { pthread_mutex_lock(&g_mu); g_ngfx++; g_made++; g_frame_made++; g_probe_hit++; g_create_ms += dns / 1e6; g_create_ns += dns; pthread_mutex_unlock(&g_mu); }
            else {
                // A miss that took long: the driver compiled before answering COMPILE_REQUIRED (the try then costs a compile and saves nothing)
                pthread_mutex_lock(&g_mu); g_probe_miss++; g_probe_miss_ns += dns; g_create_ns += dns; pthread_mutex_unlock(&g_mu);
                p = VK_NULL_HANDLE;
                uint64_t t1 = now_ns();
                p = gpl_link(P, k);
                if (p) { uint64_t d2 = now_ns() - t1; linked = 1; pthread_mutex_lock(&g_mu); g_ngfx++; g_frame_made++; g_create_ms += d2 / 1e6; g_create_ns += d2; pthread_mutex_unlock(&g_mu); }
                else { uint64_t d2 = now_ns() - t1; pthread_mutex_lock(&g_mu); g_create_ns += d2; pthread_mutex_unlock(&g_mu); }   // the refused parts' time is the frame's too
            }
        }
        if (!p && !linked) p = full_pipeline(P, sh, prewarm ? " (prewarm)" : async ? " in the background (asynchronous)" : NULL);
    }
    free(P);
    if (!prewarm) { int kind; vka_shader *s5[5]; pw_slots(sh, s5, &kind); pw_note(kind, s5, k, (uint32_t)(len - offsetof(gkey_full, k)));
                    pw_explain(kind, s5, k, (uint32_t)(len - offsetof(gkey_full, k)), (now_ns() - t_make) / 1e6, linked, async ? " in the background" : " on the frame path"); }
    pthread_mutex_lock(&g_mu);
    __atomic_store_n(&g->linked, (uint8_t)linked, __ATOMIC_RELEASE); __atomic_store_n(&g->p, p, __ATOMIC_RELEASE); __atomic_store_n(&g->busy, 0, __ATOMIC_RELEASE);
    pthread_cond_broadcast(&g_busy_cv);
    pthread_mutex_unlock(&g_mu);
    if (linked) opt_queue(g, sh, lay, k);
}
static inline VkPipeline gval_use(gval *g) {
    if (!g) return VK_NULL_HANDLE;
    if (__atomic_load_n(&g->linked, __ATOMIC_ACQUIRE)) atomic_fetch_add_explicit(&g_hy_draws, 1, memory_order_relaxed);
    VkPipeline p = __atomic_load_n(&g->p, __ATOMIC_ACQUIRE);
    if (!p && __atomic_load_n(&g->busy, __ATOMIC_ACQUIRE)) atomic_fetch_add_explicit(&g_as_skips, 1, memory_order_relaxed);
    return p;
}
VkPipeline vka_gfx_pipeline(vka_shader *const sh[VKA_NSTAGE], const vka_layout *lay, const vka_gkey *k) {
    static gkey_full fk, last; static gval *last_g;
    // With vertex input dynamic state the key's static vertex input (va, vb: two thirds of it) stays zero: only what comes before it is hashed and compared with the
    // previous draw's key (draws in a row often share their pipeline). The entry's pipeline is read at each draw: the optimized one replaces a linked one in place
    size_t len = k->vin_static ? sizeof fk : offsetof(gkey_full, k.va);
    memset(&fk, 0, len);   // (the rest stays zero: vin_static is the device's, the same for every key)
    memcpy(fk.sh, sh, sizeof fk.sh); fk.layout = lay->layout; memcpy(&fk.k, k, len - offsetof(gkey_full, k));
    if (last_g && !memcmp(&fk, &last, len)) return gval_use(last_g);
    int made;
    gval *g = gfx_obtain(&fk, fnv(&fk, len, 1469598103934665603ull), len, sh, lay, k, 0, &made);
    memcpy(&last, &fk, len); last_g = g;
    return gval_use(g);
}

// ---------- Compute pipelines ----------
typedef struct { vka_shader *cs; VkPipelineLayout layout; uint64_t umask, smask; } ckey;   // masks: storage images retyped for integer textures (vka_shader_img_module)
static kmap g_cs = {.klen = sizeof(ckey)};
static VkPipeline cs_obtain(vka_shader *cs, const vka_layout *lay, int prewarm, int *made, uint64_t umask, uint64_t smask) {
    *made = 0;
    if (VKA_ENV("DM_VK_SKIP_NDVE", 1) && vka_is_ndve_key(cs->key)) return VK_NULL_HANDLE;
    VkShaderModule csm = umask | smask ? vka_shader_img_module(cs, umask, smask) : vka_shader_cs_module(cs);
    if (!csm) return VK_NULL_HANDLE;   // (a retyped module that cannot be made: the caller takes the base pipeline)
    ckey k; memset(&k, 0, sizeof k); k.cs = cs; k.layout = lay->layout; k.umask = umask; k.smask = smask;
    uint64_t h = fnv(&k, sizeof k, 1469598103934665603ull);
    pthread_mutex_lock(&g_mu);
    gval *g = km_get(&g_cs, &k, h);
    if (g) {
        if (g->busy && prewarm) { pthread_mutex_unlock(&g_mu); return VK_NULL_HANDLE; }
        if (g->busy) busy_wait(g);
        VkPipeline p = g->p; pthread_mutex_unlock(&g_mu); return p;
    }
    g = calloc(1, sizeof *g); g->busy = 1; g->pw = (uint8_t)(prewarm != 0); g->tid = prewarm ? gettid() : 0; km_put(&g_cs, &k, h, g);
    pthread_mutex_unlock(&g_mu);
    pc_open();
    uint64_t t0 = now_ns();
    uint32_t pin = (cs->e->flags & DMSH_VK_F_SUBGROUP) ? vka_shader_sg_pin(DMSH_VK_STAGE_CS) : 0;
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo rsg = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, .requiredSubgroupSize = pin};
    int spill = 0;   // shared memory in the spill buffer (vka_draw.c spill_dispatch): dispatched in parts with a base workgroup
    for (unsigned i = 0; i < cs->e->bind_count; i++) if (cs->b[i].kind == DMSH_VK_KIND_SHARED_SPILL) spill = 1;
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .flags = spill ? VK_PIPELINE_CREATE_DISPATCH_BASE_BIT : 0, .layout = lay->layout,
        .stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .pNext = pin ? &rsg : NULL, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = csm, .pName = "main"}};
    VkPipeline p = VK_NULL_HANDLE; VkResult r; int over = 0;
    // Shared memory past the device's limit is invalid usage (the driver may refuse, crash or run it with less). The translator keeps a program's shared memory past
    // DMSH_VK_SHARED_MAX in a storage buffer (DMSH_VK_KIND_SHARED_SPILL), and e->shared is then what stays in workgroup memory; a program still over this device's
    // limit (a library built before the spill, or a device under DMSH_VK_SHARED_MAX) is not made and its dispatches are skipped. docs/reference/shaders.md "Compute
    // programs over the device's shared memory". DM_VK_CS_SHARED_OVER=1 makes them anyway (an A/B: what the driver does with them)
    uint32_t smax = V.props.limits.maxComputeSharedMemorySize;
    if (smax && cs->e->shared > smax && !VKA_ENV("DM_VK_CS_SHARED_OVER", 0)) {
        r = VK_ERROR_FEATURE_NOT_PRESENT; over = 1;
        static _Atomic int n; if (atomic_fetch_add(&n, 1) < 16) VRED("compute program %016llx takes %u bytes of shared memory, the device at most %u: not made, its dispatches are skipped (the first 16 are logged; the count is in the pipeline line)", (unsigned long long)cs->key, cs->e->shared, smax);
    } else { int bs = build_begin("compute pipeline", cs->key, 0); r = vk.vkCreateComputePipelines(V.dev, g_pc, 1, &ci, NULL, &p); build_end(bs); }
    pthread_mutex_lock(&g_mu);
    if (over) { p = VK_NULL_HANDLE; g_ncs_over++; } else if (r) { p = VK_NULL_HANDLE; g_nfail++; } else { g_ncs++; g_made++; if (!prewarm) g_frame_made++; }
    { uint64_t dns = now_ns() - t0; g_create_ms += dns / 1e6; if (!prewarm) g_create_ns += dns; }
    g->p = p; g->busy = 0; pthread_cond_broadcast(&g_busy_cv);
    pthread_mutex_unlock(&g_mu);
    if (r && !over) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 16) VRED("compute pipeline %016llx: %d; its dispatches are skipped", (unsigned long long)cs->key, r); }
    { double ms_ = (now_ns() - t0) / 1e6; static _Atomic int n; if (!r && ms_ > 50 && atomic_fetch_add(&n, 1) < 200) VLOG("compute pipeline %016llx took %.0f ms%s", (unsigned long long)cs->key, ms_, prewarm ? " (prewarm)" : "");
      if (!prewarm && !r && ms_ > 1000) vka_pso_save(2); }
    if (!prewarm && !over && !(umask | smask)) { vka_shader *s5[5] = {cs, NULL, NULL, NULL, NULL}; pw_note(VKA_PW_CS, s5, "", 0); }   // (one not made stays out of the prewarm list)
    *made = 1;
    return p;
}
VkPipeline vka_cs_pipeline(vka_shader *cs, const vka_layout *lay) { int made; return cs_obtain(cs, lay, 0, &made, 0, 0); }
VkPipeline vka_cs_pipeline_img(vka_shader *cs, const vka_layout *lay, uint64_t umask, uint64_t smask) { int made; return cs_obtain(cs, lay, 0, &made, umask, smask); }
#endif
