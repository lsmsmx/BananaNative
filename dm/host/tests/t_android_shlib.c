// The Android Vulkan replay's shader library reader and binding walker (dm/gfx/vk_android/vka_shlib.c, vka_bind.c), on the Mac: a library built here by hand (header checks,
// lookups, a raw and a zstd module, refusals), the walker against hand-made NVN state, and, when it is on this machine, the real Donkey Kong Bananza library: every entry's
// records walked, modules decompressed. The real library is game-derived and private: DM_VK_SHLIB=<path to dmsh_vk.lib> names it (the work folder's
// android-shaders/lib/v-<DMSH_VK_CHAIN>/dmsh_vk.lib); without it that part is skipped. DM_VK_SHLIB_ALL=1 decompresses every module (about 1.7 GB of SPIR-V) instead of a sample
#include "../../gfx/vk_android/vka_shlib.h"
#include "../../gfx/vk_android/vka_bind.h"
#include "../../gfx/vk_android/vka_abi_compat.h"
#include "../../gfx/vk_android/vka_gml.h"
#include "../../gfx/nvn/dmsh_chain.h"
#include "../../gfx/nvn/dmsh_chain_vk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---------- A library made here ----------
static const uint8_t ZMOD[] = {   // zstd -19 of 64 words: the SPIR-V header (0x07230203, 0x00010300, 0, 1, 0), then 3 i for i < 59
    0x28,0xb5,0x2f,0xfd,0x64,0x00,0x00,0xad,0x03,0x00,0x22,0x0f,0x1b,0x1c,0x50,0xdb,0x66,0x50,0xd7,0x75,0x5d,0xd7,0x75,0x5d,0xd7,0x75,0x5d,0xd7,0x75,0x5d,0xd7,0x75,0x5d,0xd7,
    0xeb,0xbe,0xef,0xfb,0x09,0x52,0xcb,0x14,0xdf,0x5d,0xf7,0xdc,0x71,0xbf,0xdd,0xf6,0xda,0x69,0x9f,0x5d,0xf6,0xd8,0x61,0x7f,0xdd,0xf5,0xd6,0x59,0x5f,0x5d,0xf5,0xd4,0x51,0x3f,
    0xdd,0xf4,0xd2,0x49,0x1f,0x5d,0xf4,0xd0,0x41,0xff,0xdc,0xf3,0xce,0x39,0xdf,0x5c,0xf3,0xcc,0x31,0xbf,0xdc,0xf2,0xca,0x29,0x9f,0x5c,0xf2,0xc8,0x21,0x7f,0xdc,0xf1,0xc6,0x17,
    0x57,0x3c,0x71,0xc4,0x0f,0x37,0xbc,0x70,0xc2,0x07,0x0f,0xfc,0xeb,0xf5,0xbe,0x02,0x06,0xe0,0x0b,0x02,0x00,0x60,0xb1,0x05,0xb8,0x26,0xcc,0x02,0x7b};

typedef struct { uint8_t *p; size_t n; } img_t;
static void put_str(char f[16], const char *s) { memset(f, 0, 16); memcpy(f, s, strlen(s) < 16 ? strlen(s) : 16); }
static img_t make_lib(void) {
    enum { NE = 2, NB = 3, NM = 2 };
    size_t eo = 128, bo = eo + NE * 64, mo = bo + NB * 24, blo = (mo + NM * 16 + 15) & ~(size_t)15;
    size_t raw = 20, zo = 32, c1o = (zo + sizeof ZMOD + 15) & ~(size_t)15, blob = c1o + 16;
    img_t L = {calloc(1, blo + blob), blo + blob};
    dmsh_vk_header *h = (dmsh_vk_header *)L.p;
    h->magic = DMSH_VK_LIB_MAGIC; h->version = DMSH_VK_LIB_VERSION;
    put_str(h->chain, DMSH_CHAIN); put_str(h->vk_chain, DMSH_VK_CHAIN); put_str(h->game, "0100000000001000"); put_str(h->inputs, "test");
    h->entry_count = NE; h->entry_off = (uint32_t)eo; h->bind_count = NB; h->bind_off = (uint32_t)bo; h->mod_count = NM; h->mod_off = (uint32_t)mo; h->blob_off = blo; h->blob_size = blob;
    dmsh_vk_entry *e = (dmsh_vk_entry *)(L.p + eo);
    e[0] = (dmsh_vk_entry){.key = 0x0011223344556677ull, .stage = DMSH_VK_STAGE_VS, .mod_count = 1, .mod = 0, .bind = 0, .bind_count = 1, .vin_mask = 3, .flags = DMSH_VK_F_GL_DEPTH};
    e[1] = (dmsh_vk_entry){.key = 0xa011223344556677ull, .stage = DMSH_VK_STAGE_FS, .mod_count = 1, .mod = 1, .bind = 1, .bind_count = 2, .c1_off = (uint32_t)c1o, .c1_len = 16, .fout_mask = 1};
    dmsh_vk_bind *b = (dmsh_vk_bind *)(L.p + bo);
    b[0] = (dmsh_vk_bind){.kind = DMSH_VK_KIND_CBUF, .desc = 6, .binding = 0, .count = 1, .reg = 0, .soff = 0xFFFF};
    b[1] = (dmsh_vk_bind){.kind = DMSH_VK_KIND_TEX, .desc = 1, .binding = 4 * 64, .count = 1, .reg = 1, .skind = DMSH_VK_KIND_SAMP_TEX, .sreg = 1, .dim = 1, .soff = 0};
    b[2] = (dmsh_vk_bind){.kind = DMSH_VK_KIND_C1, .desc = 6, .binding = 4 * 64 + 1, .count = 1, .reg = -1, .soff = 0xFFFF};
    dmsh_vk_module *m = (dmsh_vk_module *)(L.p + mo);
    m[0] = (dmsh_vk_module){.off = 0, .size = (uint32_t)raw, .stored = (uint32_t)raw, .codec = DMSH_VK_CODEC_RAW};
    m[1] = (dmsh_vk_module){.off = (uint32_t)zo, .size = 256, .stored = sizeof ZMOD, .codec = DMSH_VK_CODEC_ZSTD};
    uint32_t hdr[5] = {0x07230203u, 0x00010300u, 0, 1, 0}; memcpy(L.p + blo, hdr, sizeof hdr);
    memcpy(L.p + blo + zo, ZMOD, sizeof ZMOD);
    for (int i = 0; i < 16; i++) L.p[blo + c1o + i] = (uint8_t)(0x40 + i);
    return L;
}

static void test_made(void) {
    img_t I = make_lib(); vka_shlib L; char err[256];
    CHECK(vka_shlib_check(&L, I.p, I.n, DMSH_CHAIN, DMSH_VK_CHAIN, "0100000000001000", NULL, err, sizeof err), "made library refused: %s", err);
    CHECK(!vka_shlib_check(&L, I.p, I.n, "000000000000", DMSH_VK_CHAIN, NULL, NULL, err, sizeof err) && strstr(err, "translator chain"), "chain mismatch: %s", err);
    CHECK(!vka_shlib_check(&L, I.p, I.n, DMSH_CHAIN, "000000000000", NULL, NULL, err, sizeof err) && strstr(err, "Vulkan chain"), "vk chain mismatch: %s", err);
    CHECK(!vka_shlib_check(&L, I.p, I.n, DMSH_CHAIN, DMSH_VK_CHAIN, "040074A01BF12000", NULL, err, sizeof err) && strstr(err, "game"), "game mismatch: %s", err);
    CHECK(!vka_shlib_check(&L, I.p, I.n - 1, NULL, NULL, NULL, NULL, err, sizeof err), "a cut file passed");
    // Another version is taken by the ABI its header records (an older library while the delta update runs), only with that exact ABI; one without the field never
    dmsh_vk_header *H = (dmsh_vk_header *)I.p;
    CHECK(!vka_shlib_check(&L, I.p, I.n, DMSH_CHAIN, "000000000000", NULL, DMSH_VK_ABI, err, sizeof err) && strstr(err, "ABI none"), "a library without abi taken by ABI: %s", err);
    memcpy(H->abi, DMSH_VK_ABI, sizeof H->abi);
    CHECK(vka_shlib_check(&L, I.p, I.n, "000000000000", "000000000000", "0100000000001000", DMSH_VK_ABI, err, sizeof err) && L.by_abi, "same ABI refused: %s", err);
    CHECK(vka_shlib_check(&L, I.p, I.n, DMSH_CHAIN, DMSH_VK_CHAIN, NULL, DMSH_VK_ABI, err, sizeof err) && !L.by_abi, "own version marked by_abi");
    CHECK(!vka_shlib_check(&L, I.p, I.n, DMSH_CHAIN, "000000000000", NULL, NULL, err, sizeof err), "another version taken without an ABI asked");
    CHECK(!vka_shlib_check(&L, I.p, I.n, DMSH_CHAIN, "000000000000", NULL, "0123456789ab", err, sizeof err) && strstr(err, "Vulkan chain"), "another ABI taken: %s", err);
    CHECK(!vka_shlib_check(&L, I.p, I.n, DMSH_CHAIN, "000000000000", "040074A01BF12000", DMSH_VK_ABI, err, sizeof err) && strstr(err, "game"), "same ABI, other game taken: %s", err);
    memset(H->abi, 0, sizeof H->abi);
    vka_shlib_check(&L, I.p, I.n, NULL, NULL, NULL, NULL, err, sizeof err);
    uint8_t md[8] = {0xa0, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
    const dmsh_vk_entry *fs = vka_shlib_find(&L, vka_shlib_key(md)), *vs = vka_shlib_find(&L, 0x0011223344556677ull);
    CHECK(fs && fs->stage == DMSH_VK_STAGE_FS && vs && vs->stage == DMSH_VK_STAGE_VS, "lookup");
    CHECK(!vka_shlib_find(&L, 0x0011223344556676ull) && !vka_shlib_find(&L, 0xffffffffffffffffull) && !vka_shlib_find(&L, 0), "lookup of absent keys");
    size_t nb = 0; uint32_t *w = vka_shlib_spirv(&L, vka_shlib_module(&L, vs, DMSH_VK_MOD_BASE), &nb, err, sizeof err);
    CHECK(w && nb == 20 && w[0] == 0x07230203u, "raw module: %s", err); free(w);
    w = vka_shlib_spirv(&L, vka_shlib_module(&L, fs, DMSH_VK_MOD_BASE), &nb, err, sizeof err);
    CHECK(w && nb == 256 && w[0] == 0x07230203u && w[5] == 0 && w[63] == 3 * 58, "zstd module: %s", err); free(w);
    CHECK(!vka_shlib_module(&L, fs, 1), "variant 1 does not exist");
    const uint8_t *c1 = vka_shlib_c1(&L, fs); CHECK(c1 && c1[0] == 0x40 && c1[15] == 0x4f, "c1 bytes");
    // A zstd module whose recorded size is wrong is refused, not overrun
    dmsh_vk_module bad = *vka_shlib_module(&L, fs, 0); bad.size = 128;
    w = vka_shlib_spirv(&L, &bad, &nb, err, sizeof err); CHECK(!w, "a short destination was accepted"); free(w);
    // Out-of-order entries are refused
    dmsh_vk_entry *e = (dmsh_vk_entry *)(I.p + 128); uint64_t k = e[0].key; e[0].key = e[1].key + 1;
    CHECK(!vka_shlib_check(&L, I.p, I.n, NULL, NULL, NULL, NULL, err, sizeof err) && strstr(err, "order"), "order: %s", err);
    e[0].key = k;
    // NVN2 task and mesh entries (stages 6 and 7) are accepted; a stage past them is refused
    e[0].stage = DMSH_VK_STAGE_TS; CHECK(vka_shlib_check(&L, I.p, I.n, NULL, NULL, NULL, NULL, err, sizeof err), "task entry refused: %s", err);
    e[0].stage = DMSH_VK_STAGE_MS; CHECK(vka_shlib_check(&L, I.p, I.n, NULL, NULL, NULL, NULL, err, sizeof err), "mesh entry refused: %s", err);
    e[0].stage = DMSH_VK_STAGE_MS + 1; CHECK(!vka_shlib_check(&L, I.p, I.n, NULL, NULL, NULL, NULL, err, sizeof err) && strstr(err, "stage"), "stage 8 accepted: %s", err);
    e[0].stage = DMSH_VK_STAGE_VS;
    // Offsets past 4 GiB (Pokemon Pokopia's 9.8 GB blob) take their high bits from c1_off_hi / off_hi; in this small file they point past the blob and are refused
    dmsh_vk_module *mm = (dmsh_vk_module *)(I.p + 128 + 2 * 64 + 3 * 24);
    mm[1].off_hi = 1;
    CHECK(!vka_shlib_check(&L, I.p, I.n, NULL, NULL, NULL, NULL, err, sizeof err) && strstr(err, "past the blob"), "module high offset bits ignored: %s", err);
    CHECK(vka_shlib_mod_off(&mm[1]) == (1ull << 32 | mm[1].off), "module offset %llx", (unsigned long long)vka_shlib_mod_off(&mm[1]));
    mm[1].off_hi = 0;
    e[1].c1_off_hi = 2;
    CHECK(!vka_shlib_check(&L, I.p, I.n, NULL, NULL, NULL, NULL, err, sizeof err) && strstr(err, "c1 bytes past the blob"), "c1 high offset bits ignored: %s", err);
    CHECK(vka_shlib_c1_off(&e[1]) == (2ull << 32 | e[1].c1_off), "c1 offset %llx", (unsigned long long)vka_shlib_c1_off(&e[1]));
    e[1].c1_off_hi = 0;
    CHECK(vka_shlib_check(&L, I.p, I.n, NULL, NULL, NULL, NULL, err, sizeof err), "library refused after the high bits were cleared: %s", err);
    free(I.p);
}

// The entry hash of the pipeline cache's stamp (vka_pso.c, vka_pckeep.h): the same for an entry whose bytes are the same in another library, wherever they lie and
// whatever its header says (a restamped library, a delta update's reused entry); another for a changed module or c1 byte
static void test_entry_hash(void) {
    img_t I = make_lib(); vka_shlib L; char err[256];
    CHECK(vka_shlib_check(&L, I.p, I.n, NULL, NULL, NULL, NULL, err, sizeof err), "made library refused: %s", err);
    uint64_t h0 = vka_shlib_entry_hash(&L, &L.ent[0]), h1 = vka_shlib_entry_hash(&L, &L.ent[1]);
    CHECK(h0 && h1 && h0 != h1, "entry hashes %llx %llx", (unsigned long long)h0, (unsigned long long)h1);
    // Restamped: another header (inputs, ABI), the same entries
    img_t R = {malloc(I.n), I.n}; memcpy(R.p, I.p, I.n); dmsh_vk_header *rh = (dmsh_vk_header *)R.p; put_str(rh->inputs, "restamped"); memset(rh->abi, '7', sizeof rh->abi);
    vka_shlib LR; CHECK(vka_shlib_check(&LR, R.p, R.n, NULL, NULL, NULL, NULL, err, sizeof err), "restamped library refused: %s", err);
    CHECK(vka_shlib_entry_hash(&LR, &LR.ent[0]) == h0 && vka_shlib_entry_hash(&LR, &LR.ent[1]) == h1, "a restamped library changed the entry hashes");
    // Moved: the blob's bytes 16 further on (module and c1 offsets with them)
    const dmsh_vk_header *h = (const dmsh_vk_header *)I.p; size_t blo = (size_t)h->blob_off, blob = (size_t)h->blob_size;
    img_t M = {calloc(1, I.n + 16), I.n + 16}; memcpy(M.p, I.p, blo); memcpy(M.p + blo + 16, I.p + blo, blob);
    dmsh_vk_header *mh = (dmsh_vk_header *)M.p; mh->blob_size += 16;
    dmsh_vk_module *mm = (dmsh_vk_module *)(M.p + mh->mod_off); for (uint32_t i = 0; i < mh->mod_count; i++) mm[i].off += 16;
    dmsh_vk_entry *me = (dmsh_vk_entry *)(M.p + mh->entry_off); me[1].c1_off += 16;
    vka_shlib LM; CHECK(vka_shlib_check(&LM, M.p, M.n, NULL, NULL, NULL, NULL, err, sizeof err), "moved library refused: %s", err);
    CHECK(vka_shlib_entry_hash(&LM, &LM.ent[0]) == h0 && vka_shlib_entry_hash(&LM, &LM.ent[1]) == h1, "moving the blob changed the entry hashes");
    // Changed: one byte of the pixel shader's stored module, then one of its c1 bytes; the vertex shader stays
    size_t zo = (size_t)((const dmsh_vk_module *)(I.p + h->mod_off))[1].off;
    R.p[blo + zo + 20] ^= 1;
    CHECK(vka_shlib_entry_hash(&LR, &LR.ent[1]) != h1 && vka_shlib_entry_hash(&LR, &LR.ent[0]) == h0, "a changed module byte");
    R.p[blo + zo + 20] ^= 1; R.p[blo + ((const dmsh_vk_entry *)(I.p + h->entry_off))[1].c1_off] ^= 1;
    CHECK(vka_shlib_entry_hash(&LR, &LR.ent[1]) != h1, "a changed c1 byte");
    free(I.p); free(R.p); free(M.p);
}

// ---------- The walker ----------
static uint8_t g_mem[4096];   // "guest memory" at address 0x10000 for the read callback
static int rd(void *ctx, uint64_t a, void *out, size_t n) { (void)ctx; if (a < 0x10000 || a + n > 0x10000 + sizeof g_mem) return 0; memcpy(out, g_mem + (a - 0x10000), n); return 1; }

static void test_walk(void) {
    static vka_bstate s; memset(&s, 0, sizeof s);
    s.ubo[1][0] = (vka_range){0x10000, 256}; s.ubo[1][2] = (vka_range){0x10100, 64};
    s.ssbo[1][3] = (vka_range){0x20040, 128}; s.ssbo[1][4] = (vka_range){0x20010, 16};
    s.tex[1][1] = 7 | 3u << 20; s.tex[1][DMSH_SEPTEX_SLOT0 + 2] = 9; s.samp[1][5] = 11u << 20; s.img[1][2] = 13;
    s.tex_base = 100; s.samp_base = 200; s.base_instance = 61; s.samples = 4;
    uint32_t h = 5 | 6u << 20; memcpy(g_mem + 0x10, &h, 4);   // a handle in uniform slot 0 at byte 0x10
    uint64_t sa = 0x30000; memcpy(g_mem + 0x20, &sa, 8);
    dmsh_vk_bind b[] = {
        {.kind = DMSH_VK_KIND_CBUF, .desc = 6, .binding = 256, .count = 1, .reg = 2, .soff = 0xFFFF},
        {.kind = DMSH_VK_KIND_CBUF, .desc = 6, .binding = 257, .count = 1, .reg = 9, .soff = 0xFFFF},   // not bound
        {.kind = DMSH_VK_KIND_TEX, .desc = 1, .binding = 258, .count = 1, .reg = 1, .skind = DMSH_VK_KIND_SAMP_TEX, .sreg = 1, .dim = 1},
        {.kind = DMSH_VK_KIND_TEX, .desc = 1, .binding = 259, .count = 1, .reg = DMSH_SEPTEX_SLOT0 + 2, .skind = DMSH_VK_KIND_SAMP, .sreg = 5, .dim = 1},
        {.kind = DMSH_VK_KIND_TEX_UBO, .desc = 1, .binding = 260, .count = 1, .reg = 0, .off = 0x10, .skind = DMSH_VK_KIND_SAMP_UBO, .sreg = 0, .soff = 0x10, .dim = 1},
        {.kind = DMSH_VK_KIND_SBUF, .desc = 7, .binding = 261, .count = 1, .reg = 3, .flags = DMSH_VK_BF_RW},
        {.kind = DMSH_VK_KIND_SBUF_UBO, .desc = 7, .binding = 262, .count = 1, .reg = 0, .off = 0x20},
        {.kind = DMSH_VK_KIND_IMG, .desc = 3, .binding = 263, .count = 1, .reg = 2, .flags = DMSH_VK_BF_RW},
        {.kind = DMSH_VK_KIND_DRVCB, .desc = 6, .binding = 264, .count = 1, .reg = 0},
        {.kind = DMSH_VK_KIND_TEX_C2A, .desc = 1, .binding = 265, .count = 3, .off = DMSH_C2_TEX, .skind = DMSH_VK_KIND_SAMP_C2A, .soff = DMSH_C2_SAMP + 8 * 5, .dim = 1},
        {.kind = DMSH_VK_KIND_TEX_POOL, .desc = 2, .binding = 0, .set = 1},
        {.kind = DMSH_VK_KIND_SUPPORT, .desc = 6, .binding = 266, .count = 1, .reg = -1},
    };
    dmsh_vk_entry e = {.stage = DMSH_VK_STAGE_FS, .bind_count = sizeof b / sizeof *b};
    vka_res r[32]; char why[200] = ""; int pool = 0;
    int n = vka_walk(&s, 1, &e, b, 0, 64, rd, NULL, r, 32, &pool, why, sizeof why);
    CHECK(n == 13, "walk: %d results (%s)", n, why);
    if (n == 13) {
        CHECK(r[0].what == VKA_R_BUF && r[0].addr == 0x10100 && r[0].size == 64 && r[0].binding == 256, "cbuf 2");
        CHECK(r[1].what == VKA_R_NULL, "unbound cbuf reads null");
        CHECK(r[2].what == VKA_R_TEX && r[2].tid == 7 && r[2].sid == 3, "combined slot 1: %d/%d", r[2].tid, r[2].sid);
        CHECK(r[3].what == VKA_R_TEX && r[3].tid == 9 && r[3].sid == 11, "separate texture 2 + sampler 5: %d/%d", r[3].tid, r[3].sid);
        CHECK(r[4].what == VKA_R_TEX && r[4].tid == 105 && r[4].sid == 206, "handle in a uniform buffer takes the pool bases: %d/%d", r[4].tid, r[4].sid);
        CHECK(r[5].what == VKA_R_BUF && r[5].addr == 0x20040 && r[5].size == 128, "storage buffer");
        CHECK(r[6].what == VKA_R_BUF && r[6].addr == 0x30000 && r[6].size == 0, "storage buffer address from a uniform buffer");
        CHECK(r[7].what == VKA_R_IMG && r[7].tid == 13, "image slot 2");
        CHECK(r[8].what == VKA_R_DRV && r[8].bank == 0, "c0");
        CHECK(r[9].what == VKA_R_NULL && r[10].what == VKA_R_TEX && r[10].tid == 7 && r[10].sid == (3 | 11) && r[10].elem == 1 && r[11].what == VKA_R_NULL,
              "c2 array: %d %d/%d %d", r[9].what, r[10].tid, r[10].sid, r[11].what);
        CHECK(r[12].what == VKA_R_SUPPORT, "support");
        CHECK(pool, "a pool record marks the draw as using set 1");
    }
    // An array of handles in a uniform buffer (BindlessToArray, Fire Emblem Fortune's Weave's c[14][8 i + 0x340] cube maps): element k is the handle at the base + 8k, its sampler from the same word
    {
        uint32_t hs[3] = {21 | 31u << 20, 22 | 32u << 20, 23 | 33u << 20};
        for (int k = 0; k < 3; k++) memcpy(g_mem + 0x40 + 8 * k, &hs[k], 4);
        dmsh_vk_bind ab = {.kind = DMSH_VK_KIND_TEX_UBO, .desc = 1, .binding = 256, .count = 3, .reg = 0, .off = 0x40, .skind = DMSH_VK_KIND_SAMP_UBO, .sreg = 0, .soff = 0x40, .dim = 2};
        dmsh_vk_entry ea = {.stage = DMSH_VK_STAGE_FS, .bind_count = 1};
        int na = vka_walk(&s, 1, &ea, &ab, 0, 64, rd, NULL, r, 32, NULL, why, sizeof why);
        CHECK(na == 3 && r[0].tid == 121 && r[0].sid == 231 && r[1].tid == 122 && r[1].sid == 232 && r[2].tid == 123 && r[2].sid == 233 && r[2].elem == 2,
              "uniform buffer handle array: %d, %d/%d %d/%d %d/%d", na, r[0].tid, r[0].sid, r[1].tid, r[1].sid, r[2].tid, r[2].sid);
        vka_texu_elem = 0;   // DM_VK_TEXU_ELEM=0: every element from the base, as before
        na = vka_walk(&s, 1, &ea, &ab, 0, 64, rd, NULL, r, 32, NULL, why, sizeof why);
        CHECK(na == 3 && r[1].tid == 121 && r[2].sid == 231, "DM_VK_TEXU_ELEM=0: %d/%d", r[1].tid, r[2].sid);
        vka_texu_elem = 1;
    }
    // A storage buffer off the device's alignment is refused (SHADERS.md open problem 2)
    dmsh_vk_bind un = {.kind = DMSH_VK_KIND_SBUF, .desc = 7, .binding = 256, .count = 1, .reg = 4};
    dmsh_vk_entry e1 = {.stage = DMSH_VK_STAGE_FS, .bind_count = 1};
    n = vka_walk(&s, 1, &e1, &un, 0, 64, rd, NULL, r, 32, NULL, why, sizeof why);
    CHECK(n == VKA_WALK_REFUSE && strstr(why, "aligned"), "unaligned storage buffer, library without ssbo_align: %d %s", n, why);
    n = vka_walk(&s, 1, &e1, &un, 0, 16, rd, NULL, r, 32, NULL, why, sizeof why);
    CHECK(n == 1 && r[0].addr == 0x20010, "the same buffer on a 16-byte device");
    n = vka_walk(&s, 1, &e1, &un, DMSH_VK_SSBO_ALIGN, 64, rd, NULL, r, 32, NULL, why, sizeof why);   // a library built with ssbo_align: bound rounded down, the range grown
    CHECK(n == 1 && r[0].addr == 0x20000 && r[0].size == 32, "storage buffer rounded down to ssbo_align: %#llx +%llu", (unsigned long long)r[0].addr, (unsigned long long)r[0].size);
    // Shared memory past the device's limit (DMSH_VK_KIND_SHARED_SPILL): the record's slice bytes come back as the size; a record without them is refused
    dmsh_vk_bind sp = {.kind = DMSH_VK_KIND_SHARED_SPILL, .desc = 7, .binding = 3, .count = 1, .flags = DMSH_VK_BF_RW, .reg = -1, .bytes = 49152};
    dmsh_vk_entry ec = {.stage = DMSH_VK_STAGE_CS, .bind_count = 1, .shared = 0};
    n = vka_walk(&s, 5, &ec, &sp, DMSH_VK_SSBO_ALIGN, 64, rd, NULL, r, 32, NULL, why, sizeof why);
    CHECK(n == 1 && r[0].what == VKA_R_SHARED_SPILL && r[0].size == 49152 && r[0].binding == 3, "shared memory spill: %d what %d size %llu (%s)", n, n > 0 ? r[0].what : -1, n > 0 ? (unsigned long long)r[0].size : 0ull, why);
    sp.bytes = 0;
    CHECK(vka_walk(&s, 5, &ec, &sp, DMSH_VK_SSBO_ALIGN, 64, rd, NULL, r, 32, NULL, why, sizeof why) == VKA_WALK_REFUSE, "shared memory spill without slice bytes refused");
    dmsh_vk_bind unk = {.kind = DMSH_VK_KIND_UNKNOWN, .binding = 256};
    CHECK(vka_walk(&s, 1, &e1, &unk, 0, 64, rd, NULL, r, 32, NULL, why, sizeof why) == VKA_WALK_REFUSE, "unknown kind refused");

    // Driver segments
    static uint8_t d[DMSH_DRV_SIZE];
    vka_drv_fill(&s, 1, 0, 0, d);
    uint64_t a; uint32_t sz, bi; float ps;
    memcpy(&a, d + DMSH_C0_SSBO_PS + 16 * 3, 8); memcpy(&sz, d + DMSH_C0_SSBO_PS + 16 * 3 + 8, 4); CHECK(a == 0x20040 && sz == 128, "c0 pixel storage table");
    memcpy(&bi, d + DMSH_C0_BASE_INSTANCE, 4); memcpy(&ps, d + DMSH_C0_POINT_SIZE, 4); CHECK(bi == 61 && ps == 2047.0f, "c0 base instance / point size");
    CHECK(d[DMSH_C0_SAMPLE_TABLE] == 0x26 && d[DMSH_C0_SAMPLE_TABLE + 3] == 0xEA, "4-sample table");
    s.ubo[5][7] = (vka_range){0xabc00, 144}; s.dispatch[0] = 3; s.dispatch[1] = 4; s.dispatch[2] = 5;
    vka_drv_fill(&s, 5, 0, 0, d);
    memcpy(&a, d + DMSH_C0_CS_UBO + 16 * 7, 8); uint32_t dz; memcpy(&dz, d + DMSH_C0_DISPATCH + 8, 4); CHECK(a == 0xabc00 && dz == 5, "c0 compute tables");
    static const uint8_t zero12[12]; CHECK(!memcmp(d + DMSH_C0_DISPATCH_NVN2, zero12, 12), "NVN1 compute: c0 + 4..0xF stay 0");
    s.nvn2 = 1; vka_drv_fill(&s, 5, 0, 0, d);
    uint32_t g[3], g200[3], c00; memcpy(g, d + DMSH_C0_DISPATCH_NVN2, 12); memcpy(g200, d + DMSH_C0_DISPATCH, 12); memcpy(&c00, d, 4);
    CHECK(g[0] == 3 && g[1] == 4 && g[2] == 5 && g200[0] == 3 && g200[2] == 5 && c00 == 0, "NVN2 compute: counts at c0 + 4 and 0x200, c0 + 0 stays 0: %u,%u,%u", g[0], g[1], g[2]);
    vka_drv_fill(&s, 1, 0, 0, d); memcpy(g, d + DMSH_C0_DISPATCH_NVN2, 12); CHECK(!memcmp(g, zero12, 12), "NVN2 pixel stage: no counts at c0 + 4");
    s.nvn2 = 0;
    vka_drv_fill(&s, 1, 2, 0, d);
    uint32_t w; memcpy(&w, d + DMSH_C2_TEX + 8, 4); CHECK(w == (7 | 3u << 20), "c2 combined slot 1");
    memcpy(&w, d + DMSH_C2_IMG + 16, 4); CHECK(w == 13, "c2 image slot 2");
    memcpy(&w, d + DMSH_C2_SAMP + 40, 4); CHECK(w == 11u << 20, "c2 sampler slot 5");
    s.tex[1][1] = 107 | 203u << 20;
    vka_drv_fill(&s, 1, 2, 1, d); memcpy(&w, d + DMSH_C2_TEX + 8, 4); CHECK(w == (7 | 3u << 20), "c2 of a pool reader holds the game's own IDs: %#x", w);
    s.push[0][4] = 0x5a; vka_drv_fill(&s, 0, DMSH_PUSH_BANK, 0, d); CHECK(d[4] == 0x5a && vka_drv_size(DMSH_PUSH_BANK) == DMSH_PUSH_SIZE, "push block");
    static uint8_t sup[65536]; vka_support_fill(sup, sizeof sup); float one; memcpy(&one, sup + 192 + 16 * 72, 4); CHECK(one == 1.0f && sup[0] == 0, "support buffer");
    CHECK(vka_vk_stage(1) == DMSH_VK_STAGE_FS && vka_vk_stage(2) == DMSH_VK_STAGE_GS && vka_nvn_stage(DMSH_VK_STAGE_TCS) == 3 && vka_nvn_stage(vka_vk_stage(4)) == 4, "stage maps");
    CHECK(vka_vk_stage(6) == DMSH_VK_STAGE_TS && vka_vk_stage(7) == DMSH_VK_STAGE_MS && vka_nvn_stage(DMSH_VK_STAGE_TS) == 6 && vka_nvn_stage(DMSH_VK_STAGE_MS) == 7
          && vka_vk_stage(8) == -1 && vka_nvn_stage(8) == -1 && VKA_NSTAGE == 8, "task / mesh stage maps");
    for (int st = 0; st < VKA_NSTAGE; st++) CHECK(vka_nvn_stage(vka_vk_stage(st)) == st, "stage map round trip %d", st);
    // NVN2 task and mesh stages: their storage buffer tables at c0 0x670 and 0x770 (the task table's last row ends where the mesh table starts), the graphics words beside them
    for (int r = 0; r < 16; r++) { s.ssbo[6][r] = (vka_range){0x60000 + 0x100 * (uint64_t)r, 0x40 + (uint64_t)r}; s.ssbo[7][r] = (vka_range){0x70000 + 0x100 * (uint64_t)r, 0x80 + (uint64_t)r}; }
    vka_drv_fill(&s, 6, 0, 0, d);
    memcpy(&a, d + DMSH_C0_SSBO_TS + 16 * 15, 8); memcpy(&sz, d + DMSH_C0_SSBO_TS + 16 * 15 + 8, 4); CHECK(a == 0x60f00 && sz == 0x4f, "c0 task storage table: %#llx %u", (unsigned long long)a, sz);
    memcpy(&a, d + DMSH_C0_SSBO_MS, 8); memcpy(&bi, d + DMSH_C0_BASE_INSTANCE, 4); CHECK(a == 0 && bi == 61, "task stage: no mesh table, base instance");
    vka_drv_fill(&s, 7, 0, 0, d);
    memcpy(&a, d + DMSH_C0_SSBO_MS + 16 * 2, 8); memcpy(&sz, d + DMSH_C0_SSBO_MS + 16 * 2 + 8, 4); CHECK(a == 0x70200 && sz == 0x82, "c0 mesh storage table");
    memcpy(&a, d + DMSH_C0_SSBO_TS, 8); CHECK(a == 0, "mesh stage: no task table");
    CHECK(DMSH_C0_SSBO_TS + 16 * DMSH_C0_SSBO_SLOTS <= DMSH_C0_SSBO_MS && DMSH_C0_SSBO_MS + 16 * DMSH_C0_SSBO_SLOTS <= DMSH_DRV_SIZE, "task / mesh tables inside the segment");
    CHECK(vka_walk(&s, 7, &e1, &un, DMSH_VK_SSBO_ALIGN, 64, rd, NULL, r, 32, NULL, why, sizeof why) == 1, "walk of a mesh-stage shader");
    CHECK(vka_walk(&s, 8, &e1, &un, DMSH_VK_SSBO_ALIGN, 64, rd, NULL, r, 32, NULL, why, sizeof why) == VKA_WALK_REFUSE, "NVN stage 8 refused");
}

// ---------- The real library ----------
static int rd_zero(void *ctx, uint64_t a, void *out, size_t n) { (void)ctx; (void)a; memset(out, 0, n); return 1; }
static void test_real(void) {
    const char *p = getenv("DM_VK_SHLIB");
    if (!p || access(p, R_OK)) { printf("t_android_shlib: no real library (DM_VK_SHLIB=%s): skipped\n", p ? p : "unset"); return; }
    vka_shlib L; char err[256];
    if (!vka_shlib_open(&L, p, DMSH_CHAIN, DMSH_VK_CHAIN, NULL, NULL, err, sizeof err)) { CHECK(0, "real library refused: %s", err); return; }
    char game[17]; vka_shlib_str(L.h->game, game);
    int all = getenv("DM_VK_SHLIB_ALL") != NULL;
    static vka_bstate s; memset(&s, 0, sizeof s);   // every slot bound, at 256-byte aligned addresses
    for (int st = 0; st < VKA_NSTAGE; st++) {
        for (int r = 0; r < 16; r++) { s.ubo[st][r] = (vka_range){0x80000000ull + (uint64_t)(st * 16 + r) * 0x10000, 0x10000}; s.ssbo[st][r] = (vka_range){0x90000000ull + (uint64_t)(st * 16 + r) * 0x10000, 0x1000}; }
        for (int r = 0; r < VKA_NTEX; r++) s.tex[st][r] = (uint64_t)(1 + r) | (uint64_t)(1 + r % 64) << 20;
        for (int r = 0; r < 128; r++) s.samp[st][r] = (uint64_t)(1 + r % 64) << 20;
        for (int r = 0; r < 32; r++) s.img[st][r] = 300 + r;
    }
    s.samples = 1;
    unsigned n = L.h->entry_count, refused = 0, nvpass = 0, decoded = 0, maxd[8] = {0}, nts = 0, nms = 0, nprim = 0, nmsg = 0; uint64_t spv = 0;
    static vka_res res[4096];
    for (unsigned i = 0; i < n; i++) {
        const dmsh_vk_entry *e = &L.ent[i];
        CHECK(vka_shlib_find(&L, e->key) == e, "lookup of entry %u", i);
        int nvn = vka_nvn_stage(e->stage);
        char why[200] = ""; int pool = 0;
        int k = vka_walk(&s, nvn, e, vka_shlib_binds(&L, e), L.h->ssbo_align, 64, rd_zero, NULL, res, 4096, &pool, why, sizeof why);
        if (k < 0) { refused++; if (refused < 4) printf("  %016llx refused: %s\n", (unsigned long long)e->key, why); continue; }
        CHECK(nvn >= 0, "%016llx: stage %u", (unsigned long long)e->key, e->stage);
        if (nvn < 0) continue;
        if ((unsigned)k > maxd[e->stage]) maxd[e->stage] = (unsigned)k;
        nts += e->stage == DMSH_VK_STAGE_TS; nms += e->stage == DMSH_VK_STAGE_MS; nprim += (e->flags & DMSH_VK_F_MESH_PRIM) != 0;
        nmsg += e->stage >= DMSH_VK_STAGE_TS && (e->flags & DMSH_VK_F_SUBGROUP);
        // (the build sets it on a pixel shader that reads per-primitive inputs and on a mesh shader that writes per-primitive outputs: vk_android_build.py)
        if (e->flags & DMSH_VK_F_MESH_PRIM) CHECK(e->stage == DMSH_VK_STAGE_FS || e->stage == DMSH_VK_STAGE_MS, "%016llx: per-primitive values on stage %u", (unsigned long long)e->key, e->stage);
        if (e->stage == DMSH_VK_STAGE_TS || e->stage == DMSH_VK_STAGE_MS) CHECK(e->local[0] && e->local[0] * e->local[1] * e->local[2] <= 256, "%016llx: workgroup %ux%ux%u", (unsigned long long)e->key, e->local[0], e->local[1], e->local[2]);
        // set-0 ranges: compute the vertex range, task the tess control range, mesh the vertex range (dmsh_abi_vk.h)
        int lo = e->stage == DMSH_VK_STAGE_CS || e->stage == DMSH_VK_STAGE_MS ? 0 : e->stage == DMSH_VK_STAGE_TS ? DMSH_VK_STAGE_TCS * DMSH_VK_STAGE_BINDINGS : e->stage * DMSH_VK_STAGE_BINDINGS;
        for (int j = 0; j < k; j++) {
            CHECK(res[j].binding >= lo && res[j].binding < lo + DMSH_VK_STAGE_BINDINGS, "%016llx: binding %u outside stage %u's range", (unsigned long long)e->key, res[j].binding, e->stage);
            CHECK(res[j].what != VKA_R_NULL || res[j].desc == 6 /* a uniform slot past what the state binds */, "%016llx binding %u reads nothing with every slot bound (desc %u)",
                  (unsigned long long)e->key, res[j].binding, res[j].desc);
            if (res[j].what == VKA_R_TEX) CHECK(res[j].desc == 1 || res[j].desc == 2 || res[j].desc == 0, "texture result in descriptor type %u", res[j].desc);
            if (res[j].what == VKA_R_BUF) CHECK(res[j].desc == 6 || res[j].desc == 7, "buffer result in descriptor type %u", res[j].desc);
        }
        if (e->flags & DMSH_VK_F_NV_PASSTHROUGH) nvpass++;
        if (all || i % 97 == 0 || e->stage == DMSH_VK_STAGE_GS || e->stage == DMSH_VK_STAGE_CS || e->stage >= DMSH_VK_STAGE_TS || (e->flags & DMSH_VK_F_MESH_PRIM)) {
            const dmsh_vk_module *m = vka_shlib_module(&L, e, DMSH_VK_MOD_BASE);
            CHECK(m != NULL, "%016llx has no base module", (unsigned long long)e->key);
            size_t nb = 0; uint32_t *w = m ? vka_shlib_spirv(&L, m, &nb, err, sizeof err) : NULL;
            CHECK(w != NULL, "%016llx: %s", (unsigned long long)e->key, err);
            if (w) { decoded++; spv += nb; }
            free(w);
        }
    }
    unsigned nvar[4] = {0}, nalpha = 0; for (unsigned i = 0; i < L.h->mod_count; i++) if (L.mod[i].variant < 4) nvar[L.mod[i].variant]++;
    for (unsigned i = 0; i < n; i++) nalpha += (L.ent[i].flags & DMSH_VK_F_ALPHA_TEST) != 0;
    printf("t_android_shlib: subgroup_size %u, ssbo_align %u; modules base %u uint %u uint_mixed %u dual %u; %u alpha-test shaders\n", L.h->subgroup_size, L.h->ssbo_align, nvar[0], nvar[1], nvar[2], nvar[3], nalpha);
    printf("t_android_shlib: real library %s (game %s): %u entries, %u walked (%u refused), %u NV passthrough, most descriptors per stage vs %u gs %u fs %u cs %u ts %u ms %u; %u task, %u mesh (%u with subgroup operations), %u with per-primitive values; %u modules decompressed (%.1f MB of SPIR-V)\n",
           p, game, n, n - refused, refused, nvpass, maxd[0], maxd[3], maxd[4], maxd[5], maxd[6], maxd[7], nts, nms, nmsg, nprim, decoded, spv / 1e6);
    CHECK(refused == 0, "%u entries refused with every slot bound and aligned", refused);
    CHECK(nmsg == nts + nms, "%u of %u task / mesh shaders with subgroup operations (the phone has them in both stages: a change here is worth a look)", nmsg, nts + nms);
    for (unsigned i = 0; i < n; i++) {   // every variant module decompresses, and a mixed module comes with its targets
        const dmsh_vk_entry *e = &L.ent[i];
        if (e->uint_mask) CHECK(vka_shlib_module(&L, e, DMSH_VK_MOD_UINT_MIXED) != NULL, "%016llx has uint_mask %#x and no mixed module", (unsigned long long)e->key, e->uint_mask);
        for (unsigned m = 1; m < e->mod_count; m++) { size_t nb; uint32_t *w = vka_shlib_spirv(&L, &L.mod[e->mod + m], &nb, err, sizeof err); CHECK(w != NULL, "%016llx variant: %s", (unsigned long long)e->key, err); free(w); }
    }
    vka_shlib_close(&L);
}

// ---------- Older ABIs played while the update runs (vka_abi_compat.h) ----------
static void test_compat(void) {
    // The table was checked against this host's ABI; when DMSH_VK_ABI moves, its rows are checked again and VKA_ABI_COMPAT_FOR raised (until then no row is used)
    CHECK(!strcmp(VKA_ABI_COMPAT_FOR, DMSH_VK_ABI), "vka_abi_compat.h was checked against ABI %s, this tree's is %s: check its rows against the headers' diff", VKA_ABI_COMPAT_FOR, DMSH_VK_ABI);
    // DMSH_VK_ABI_COMPAT (the launcher's list) is the table's ABIs in order, and no row is this host's own ABI
    char list[256] = ""; size_t n = 0;
    for (int i = 0; i < VKA_ABI_COMPAT_N; i++) {
        CHECK(strlen(vka_abi_compat[i].abi) == 12 && strspn(vka_abi_compat[i].abi, "0123456789abcdef") == 12, "row %d: abi %s", i, vka_abi_compat[i].abi);
        CHECK(strcmp(vka_abi_compat[i].abi, DMSH_VK_ABI), "row %d is the host's own ABI", i);
        n += (size_t)snprintf(list + n, sizeof list - n, "%s%s", i ? " " : "", vka_abi_compat[i].abi);
    }
    CHECK(!strcmp(list, DMSH_VK_ABI_COMPAT), "DMSH_VK_ABI_COMPAT \"%s\", the table's rows \"%s\"", DMSH_VK_ABI_COMPAT, list);
    CHECK(vka_abi_compat_of("674db0a4ff79") == (int)VKA_ABI_LOG_PAGES, "674db0a4ff79: differences %d", vka_abi_compat_of("674db0a4ff79"));
    CHECK(vka_abi_compat_of(DMSH_VK_ABI) == -1 && vka_abi_compat_of("0123456789ab") == -1, "a row that is no older ABI");

    // A library of another version with the older ABI is taken by the host's ABI (by_abi 2, its differences), not by another ABI asked, and never as its own version
    img_t I = make_lib(); vka_shlib L; char err[256]; dmsh_vk_header *H = (dmsh_vk_header *)I.p;
    memcpy(H->abi, "674db0a4ff79", 12);
    CHECK(vka_shlib_check(&L, I.p, I.n, DMSH_CHAIN, "000000000000", "0100000000001000", DMSH_VK_ABI, err, sizeof err) && L.by_abi == 2 && L.differs == VKA_ABI_LOG_PAGES,
          "the older ABI refused or unmarked: %s (by_abi %d, differs %#x)", err, L.by_abi, L.differs);
    CHECK(!vka_shlib_check(&L, I.p, I.n, DMSH_CHAIN, "000000000000", NULL, "0123456789ab", err, sizeof err), "the older ABI taken for another host ABI");
    CHECK(!vka_shlib_check(&L, I.p, I.n, DMSH_CHAIN, "000000000000", NULL, NULL, err, sizeof err), "the older ABI taken with no ABI asked");
    CHECK(!vka_shlib_check(&L, I.p, I.n, DMSH_CHAIN, "000000000000", "040074A01BF12000", DMSH_VK_ABI, err, sizeof err) && strstr(err, "game"), "the older ABI, other game taken: %s", err);
    memcpy(H->abi, "0123456789ab", 12);
    CHECK(!vka_shlib_check(&L, I.p, I.n, DMSH_CHAIN, "000000000000", NULL, DMSH_VK_ABI, err, sizeof err) && strstr(err, "Vulkan chain"), "an unknown ABI taken: %s", err);
    memcpy(H->abi, DMSH_VK_ABI, 12);
    CHECK(vka_shlib_check(&L, I.p, I.n, DMSH_CHAIN, "000000000000", NULL, DMSH_VK_ABI, err, sizeof err) && L.by_abi == 1 && !L.differs, "the host's ABI: by_abi %d, differs %#x", L.by_abi, L.differs);
    free(I.p);
}

// ---------- The write log's two formats (vka_gml.h), written as the modules write them ----------
static void gml_store_bitmap(uint32_t *w, int entry) { w[entry >> 5] |= 1u << (entry & 31); }   // GlobalToStorage since a319e773: the entry's bit (a load, then an atomic or)
static void gml_store_pages(uint32_t *w, uint64_t addr) {   // before it: the page into the hash set, 8 compare-and-swap probes, word 0 when none is free
    uint32_t page = (uint32_t)(addr >> 32) << 20 | (uint32_t)addr >> 12, hash = (page * 0x9e3779b1u) >> (32 - VKA_GML_PAGES_LOG2);
    for (int p = 0; p < 8; p++) { uint32_t *slot = &w[((hash + p) & ((1u << VKA_GML_PAGES_LOG2) - 1)) + 1]; if (*slot == 0 || *slot == page) { *slot = page; return; } }
    w[0] = 1;
}
typedef struct { int entries[64], ne; uint64_t pages[2048]; int np; } gml_seen;
static void seen_entry(void *a, int k) { gml_seen *s = a; if (s->ne < 64) s->entries[s->ne] = k; s->ne++; }
static void seen_page(void *a, uint64_t p) { gml_seen *s = a; if (s->np < 2048) s->pages[s->np] = p; s->np++; }
static int has_page(const gml_seen *s, uint64_t p) { for (int i = 0; i < s->np && i < 2048; i++) if (s->pages[i] == p) return 1; return 0; }
static void test_gml(void) {
    CHECK(vka_gml_bytes(0) == DMSH_GMEM_MAP_ENTRIES / 8 && vka_gml_bytes(VKA_ABI_LOG_PAGES) == 4100 && VKA_GML_MAX_BYTES == 4100, "log sizes %u %u %d",
          vka_gml_bytes(0), vka_gml_bytes(VKA_ABI_LOG_PAGES), (int)VKA_GML_MAX_BYTES);
    static uint32_t w[VKA_GML_MAX_BYTES / 4]; gml_seen s;
    // The bitmap: every entry bit but entry 0's, high entries too (past the old table's 4096), in order
    memset(w, 0, sizeof w); memset(&s, 0, sizeof s);
    int want[] = {1, 31, 32, 159, 4095, 4096, DMSH_GMEM_MAP_ENTRIES - 1};
    for (unsigned i = 0; i < sizeof want / sizeof want[0]; i++) { gml_store_bitmap(w, want[i]); gml_store_bitmap(w, want[i]); }
    gml_store_bitmap(w, 0);
    CHECK(!vka_gml_walk(w, 0, seen_entry, seen_page, &s), "the bitmap reported an overflow");
    CHECK(s.ne == (int)(sizeof want / sizeof want[0]) && !s.np, "the bitmap: %d entries, %d pages", s.ne, s.np);
    for (unsigned i = 0; i < sizeof want / sizeof want[0] && (int)i < s.ne; i++) CHECK(s.entries[i] == want[i], "the bitmap's entry %u: %d, want %d", i, s.entries[i], want[i]);
    // The page-hash set of 674db0a4ff79's modules: each page once, whatever the store's offset in it, guest addresses past 4 GiB too
    memset(w, 0, sizeof w); memset(&s, 0, sizeof s);
    uint64_t pages[] = {0x80001000ull, 0x80002000ull, 0x123456000ull, 0x8000000000ull, 0xfff000ull};
    for (unsigned i = 0; i < sizeof pages / sizeof pages[0]; i++) { gml_store_pages(w, pages[i] + 0x10); gml_store_pages(w, pages[i] + 0xffc); }
    CHECK(!vka_gml_walk(w, VKA_ABI_LOG_PAGES, seen_entry, seen_page, &s), "the page set reported an overflow");
    CHECK(s.np == (int)(sizeof pages / sizeof pages[0]) && !s.ne, "the page set: %d pages, %d entries", s.np, s.ne);
    for (unsigned i = 0; i < sizeof pages / sizeof pages[0]; i++) CHECK(has_page(&s, pages[i]), "the page set lost page %#llx", (unsigned long long)pages[i]);
    // Read as a bitmap, the same words name entries, not those pages: the format must be the library's
    memset(&s, 0, sizeof s); vka_gml_walk(w, 0, seen_entry, seen_page, &s);
    CHECK(!s.np && s.ne > 0, "the page set read as a bitmap: %d entries", s.ne);
    // More pages than slots: word 0 says so, and the slots that were filled are all read
    memset(w, 0, sizeof w); memset(&s, 0, sizeof s);
    for (uint64_t i = 0; i < 2000; i++) gml_store_pages(w, 0x100000000ull + (i << 12));
    int full = vka_gml_walk(w, VKA_ABI_LOG_PAGES, seen_entry, seen_page, &s);
    int filled = 0; for (int k = 1; k <= 1 << VKA_GML_PAGES_LOG2; k++) filled += w[k] != 0;
    CHECK(full && s.np == filled && filled <= 1 << VKA_GML_PAGES_LOG2, "the full page set: overflow %d, %d pages read, %d slots filled", full, s.np, filled);
}

int main(void) {
    test_made();
    test_entry_hash();
    test_compat();
    test_gml();
    test_walk();
    test_real();
    if (g_fail) { printf("t_android_shlib: %d failures\n", g_fail); return 1; }
    printf("t_android_shlib: ok\n");
    return 0;
}
