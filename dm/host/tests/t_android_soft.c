// Soft textures (dm/gfx/vk_android/vka_soft.h; profile tex_soft, from plumber-racer-2's nvn_mtl.m tex_soft): which textures are candidates, texture levels as a soft (or sparse)
// image's levels for copies and render targets, view ranges, the bytes left out, and the set of storage addresses that stay whole
#include <stdio.h>
#include "../../gfx/vk_android/vka_soft.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    // candidates: a packaged 2048x1024 BC7 texture with 12 levels
    const vksoft_tex bc = {2048, 1024, 12, 1, 4, 1, 1, 1, 0, 0};
    CHECK(vksoft_candidate(&bc), "a packaged 2048x1024 BC7 texture with 12 levels is a candidate");
    vksoft_tex t;
    t = bc; t.w = 512; t.h = 256; CHECK(vksoft_candidate(&t), "512x256 (the smallest long and short sides) is one");
    t = bc; t.w = 256; t.h = 512; CHECK(vksoft_candidate(&t), "256x512 too (the long side may be either)");
    t = bc; t.w = 511; t.h = 511; CHECK(!vksoft_candidate(&t), "under 512 on its long side: not");
    t = bc; t.w = 4096; t.h = 255; CHECK(!vksoft_candidate(&t), "under 256 on its short side: not");
    t = bc; t.levels = 2; CHECK(!vksoft_candidate(&t), "two levels: not (one would be left)");
    t = bc; t.levels = 3; CHECK(vksoft_candidate(&t), "three levels: one");
    t = bc; t.block_w = 1; CHECK(!vksoft_candidate(&t), "an uncompressed format: not");
    t = bc; t.has_layout = 0; CHECK(!vksoft_candidate(&t), "no packaged layout (GPU or CPU content of the game's own): not");
    t = bc; t.has_mem = 0; CHECK(!vksoft_candidate(&t), "no storage: not");
    t = bc; t.is_2d = 0; CHECK(!vksoft_candidate(&t), "not 2D or 2D array (3D, cube): not");
    t = bc; t.sparse_pool = 1; CHECK(!vksoft_candidate(&t), "a sparse pool's texture (its own left-out levels): not");
    t = bc; t.linear = 1; CHECK(!vksoft_candidate(&t), "linear: not");
    t = bc; t.samples = 4; CHECK(!vksoft_candidate(&t), "multisampled: not");

    // texture levels as the image's: a soft image starts at level 1 and has levels - 1
    uint32_t lv;
    lv = 0; CHECK(!vksoft_lv(1, 11, &lv), "level 0 of a soft texture is not in its image (a copy of it is skipped)");
    lv = 1; CHECK(vksoft_lv(1, 11, &lv) && lv == 0, "level 1 is the image's level 0 (%u)", lv);
    lv = 11; CHECK(vksoft_lv(1, 11, &lv) && lv == 10, "the coarsest level is the image's last (%u)", lv);
    lv = 12; CHECK(!vksoft_lv(1, 11, &lv), "past the chain: not");
    lv = 0; CHECK(vksoft_lv(0, 12, &lv) && lv == 0, "a whole image (base 0): the same level");
    lv = 3; CHECK(!vksoft_lv(4, 8, &lv), "a sparse image from level 4: level 3 is left out");

    // view ranges: a view of the whole chain starts at the image's level 0 with one level less
    uint32_t l0 = 0, nl = 12; vksoft_view(1, &l0, &nl); CHECK(l0 == 0 && nl == 11, "whole chain: 0+11 (%u+%u)", l0, nl);
    l0 = 0; nl = 1; vksoft_view(1, &l0, &nl); CHECK(l0 == 0 && nl == 1, "a view of level 0 alone reads level 1 (%u+%u)", l0, nl);
    l0 = 2; nl = 3; vksoft_view(1, &l0, &nl); CHECK(l0 == 1 && nl == 3, "levels 2-4: the image's 1-3 (%u+%u)", l0, nl);
    l0 = 0; nl = 2; vksoft_view(4, &l0, &nl); CHECK(l0 == 0 && nl == 1, "a sparse image from level 4 viewed from 0 with 2 levels: one level is left (%u+%u)", l0, nl);
    l0 = 5; nl = 2; vksoft_view(0, &l0, &nl); CHECK(l0 == 5 && nl == 2, "a whole image: unchanged (%u+%u)", l0, nl);

    // bytes left out: level 0 of every layer
    CHECK(vksoft_saved(4u << 20, 1) == 4u << 20, "one layer");
    CHECK(vksoft_saved(1u << 20, 6) == 6u << 20, "six layers");
    CHECK(vksoft_saved(1u << 20, 0) == 1u << 20, "no layer count: one");

    // the whole-storage set: added once, found, grows past its first table
    vksoft_set s = {0};
    CHECK(!vksoft_has(&s, 0x1000), "empty");
    CHECK(vksoft_add(&s, 0x1000) == 1 && vksoft_add(&s, 0x1000) == 0 && vksoft_has(&s, 0x1000), "added once");
    CHECK(vksoft_add(&s, 0) == 0 && !vksoft_has(&s, 0), "0 is never a key");
    for (uint64_t k = 1; k <= 5000; k++) vksoft_add(&s, 0x40000000ull + k * 0x10000);
    int all = 1; for (uint64_t k = 1; k <= 5000; k++) all &= vksoft_has(&s, 0x40000000ull + k * 0x10000);
    CHECK(all && vksoft_has(&s, 0x1000) && s.n == 5001 && !vksoft_has(&s, 0x40000000ull + 5001 * 0x10000), "5,001 keys after growing (%zu, table %zu)", s.n, s.cap);
    free(s.k);

    if (fails) { printf("t_android_soft: %d failures\n", fails); return 1; }
    printf("t_android_soft: ok\n");
    return 0;
}
