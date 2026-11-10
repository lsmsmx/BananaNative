// The owner of a texture serial (dm/gfx/vk_android/vka_texown.h): a Finalize of a copy of a texture object ends nothing while the original still holds the texture, in either
// order of the two Finalizes; a re-Initialize of a copy leaves the original's texture; an original whose bytes the game overwrote (moved away) no longer holds it, so the
// copy's Finalize ends it; a serial no Initialize was seen for is ended at once
#include <stdio.h>
#include <string.h>
#include "../../gfx/vk_android/vka_texown.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef struct { uint32_t magic; uint64_t serial; } obj;   // the two fields of a texture object the check reads
#define TEX_MAGIC 0x00584554u
static int holds(uintptr_t a, uint64_t serial) { const obj *o = (const obj *)a; return o->magic == TEX_MAGIC && o->serial == serial; }
static void finalize(obj *o) { o->magic = 0; }

int main(void) {
    nvn_hmap m = {0};
    obj a = {TEX_MAGIC, 7}, b;

    // the copy finalized first: the original keeps its texture, and its own Finalize then ends it
    vto_own(&m, a.serial, &a); b = a;
    CHECK(vto_release(&m, b.serial, &b, holds) == 0, "a copy's Finalize ended the original's texture"); finalize(&b);
    CHECK(vto_release(&m, a.serial, &a, holds) == 1, "the original's Finalize did not end its texture"); finalize(&a);
    CHECK(!nvn_hm_get(&m, &a.serial), "the owner entry outlived the texture");

    // the original finalized first: its texture ends, and the copy's later Finalize has nothing left to keep
    a = (obj){TEX_MAGIC, 8}; vto_own(&m, a.serial, &a); b = a;
    CHECK(vto_release(&m, a.serial, &a, holds) == 1, "the original's Finalize did not end its texture"); finalize(&a);
    CHECK(vto_release(&m, b.serial, &b, holds) == 1, "a copy's Finalize after the original's was kept");

    // a re-Initialize of a live copy (TextureInitialize releases what the object held first): the original's texture stays
    a = (obj){TEX_MAGIC, 9}; vto_own(&m, a.serial, &a); b = a;
    CHECK(vto_release(&m, b.serial, &b, holds) == 0, "re-initializing a copy ended the original's texture");
    b = (obj){TEX_MAGIC, 10}; vto_own(&m, b.serial, &b);
    CHECK(vto_release(&m, 9, &a, holds) == 1 && vto_release(&m, 10, &b, holds) == 1, "the two textures did not end with their own objects");

    // the game moved the object and reused the old bytes: the old place no longer holds the texture, so the moved object's Finalize ends it
    a = (obj){TEX_MAGIC, 11}; vto_own(&m, a.serial, &a); b = a; memset(&a, 0xA5, sizeof a);
    CHECK(vto_release(&m, b.serial, &b, holds) == 1, "the Finalize of a moved texture object was kept");

    // a serial with no Initialize seen (an object the host made itself), and serial 0
    obj c = {TEX_MAGIC, 12};
    CHECK(vto_release(&m, c.serial, &c, holds) == 1, "a texture with no owner entry was kept");
    CHECK(vto_release(&m, 0, &c, holds) == 1, "serial 0 was kept");

    // many owners: a copy's Finalize of each keeps every original (the table grows)
    static obj many[300];
    for (int i = 0; i < 300; i++) { many[i] = (obj){TEX_MAGIC, 1000 + (uint64_t)i}; vto_own(&m, many[i].serial, &many[i]); }
    int kept = 0; for (int i = 0; i < 300; i++) { obj cp = many[i]; kept += vto_release(&m, cp.serial, &cp, holds) == 0; }
    CHECK(kept == 300, "%d of 300 originals kept through their copies' Finalize", kept);
    int ended = 0; for (int i = 0; i < 300; i++) ended += vto_release(&m, many[i].serial, &many[i], holds) == 1;
    CHECK(ended == 300 && m.n == 0, "%d of 300 ended by their own Finalize, %u entries left", ended, m.n);

    printf("t_android_texown: %s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
