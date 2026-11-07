// Sparse tile slots shared by physical memory (dm/gfx/vk_android/vka_pshare.h; vka_sparse.c share_take): places that map the same physical run share one slot, other
// formats, tile shapes and classes do not, the entry's last place gives the slot back, and growth and tombstones keep every live entry reachable
#include <stdio.h>
#include <stdlib.h>
#include "../../gfx/vk_android/vka_pshare.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define FMT_A 37u
#define FMT_B 139u
#define FLAT256 (256u << 16 | 256u)
#define FLAT128 (128u << 16 | 128u)

// A place binding the slot of this physical run: what vka_sparse.c share_take does (the slot id; *next_id is taken when the table has no entry)
static uint32_t place(vkp_tab *t, uint64_t key, uint32_t fmt, uint32_t flat, int drawn, uint32_t *next_id) {
    int i = vkp_ref(t, key, fmt, flat, drawn);
    if (i >= 0) return t->e[i].id;
    uint32_t id = (*next_id)++;
    CHECK(vkp_add(t, key, fmt, flat, drawn, id) >= 0, "add");
    return id;
}
static int unplace(vkp_tab *t, uint64_t key, uint32_t fmt, uint32_t flat, int drawn) { return vkp_put(t, key, fmt, flat, drawn) == 1; }

int main(void) {
    uint32_t id = 1; vkp_tab t = {0};
    const uint64_t P0 = 0x7f0000000000ull, P1 = 0x7f0000001000ull;   // two 64 KiB runs of physical memory

    // One run mapped at three places: one slot, and every place gets it
    uint32_t s1 = place(&t, P0, FMT_A, FLAT256, 0, &id), s2 = place(&t, P0, FMT_A, FLAT256, 0, &id), s3 = place(&t, P0, FMT_A, FLAT256, 0, &id);
    CHECK(s1 == s2 && s2 == s3, "three places of one run share one slot (%u %u %u)", s1, s2, s3);
    CHECK(id == 2, "one slot taken for them, not three (next id %u)", id);

    // Another run has its own slot, and so has the same run in another format, tile shape or class (a 64 KiB block does not read the same)
    CHECK(place(&t, P1, FMT_A, FLAT256, 0, &id) != s1, "another run: its own slot");
    CHECK(place(&t, P0, FMT_B, FLAT256, 0, &id) != s1, "another format: its own slot");
    CHECK(place(&t, P0, FMT_A, FLAT128, 0, &id) != s1, "another tile shape: its own slot");
    uint32_t d1 = place(&t, P0, FMT_A, FLAT256, 1, &id);
    CHECK(d1 != s1, "a GPU-drawn place: its own slot (a drawn image is never uploaded into)");
    CHECK(place(&t, P0, FMT_A, FLAT256, 1, &id) == d1, "two drawn places of one run share");

    // The slot goes with the last place
    CHECK(!unplace(&t, P0, FMT_A, FLAT256, 0), "three places: the slot stays");
    CHECK(!unplace(&t, P0, FMT_A, FLAT256, 0), "two places: the slot stays");
    CHECK(unplace(&t, P0, FMT_A, FLAT256, 0), "the last place gives the slot back");
    CHECK(vkp_at(&t, P0, FMT_A, FLAT256, 0) < 0, "the entry is gone");
    CHECK(vkp_ref(&t, P0, FMT_A, FLAT256, 0) < 0, "no entry left to add a place to");

    // The same run bound again: a new slot (the tombstone is not resurrected)
    uint32_t before = id, s5 = place(&t, P0, FMT_A, FLAT256, 0, &id);
    CHECK(id == before + 1 && s5 != s1, "the run bound again takes a new slot");

    // Stats: live entries, the places bound through them, and the slots they saved
    uint64_t ne = 0, np = 0, sv = 0; vkp_stats(&t, &ne, &np, &sv);
    CHECK(ne == 5 && np == 6 && sv == 1, "stats: %llu entries, %llu places, %llu slots saved", (unsigned long long)ne, (unsigned long long)np, (unsigned long long)sv);
    uint64_t ne0 = ne, np0 = np;

    // 4,000 runs placed, every third one given back at once: growth and tombstones, every live entry still reachable
    static uint32_t slots[4000]; static uint64_t keys[4000];
    for (uint32_t i = 0; i < 4000; i++) {
        keys[i] = 0x800000000000ull + (uint64_t)i * 0x10000ull;
        slots[i] = place(&t, keys[i], FMT_A, FLAT256, (int)(i & 1), &id);
        if (i % 3 == 0) unplace(&t, keys[i], FMT_A, FLAT256, (int)(i & 1));
    }
    int bad = 0, miss = 0;
    for (uint32_t i = 0; i < 4000; i++) {
        int j = vkp_at(&t, keys[i], FMT_A, FLAT256, (int)(i & 1));
        if (j < 0) { if (i % 3 != 0) miss++; continue; }
        if (i % 3 == 0) bad++; else if (t.e[j].id != slots[i]) bad++;
    }
    CHECK(bad == 0 && miss == 0, "every live entry of 4,000 is reachable after growth and tombstones (%d wrong, %d missing)", bad, miss);
    vkp_stats(&t, &ne, &np, &sv);
    CHECK(ne == ne0 + 2666 && np == np0 + 2666, "the 1,334 removed ones are gone: %llu entries, %llu places", (unsigned long long)ne, (unsigned long long)np);

    // Each of their last places gives the slot back, and nothing of theirs stays live
    uint32_t back = 0;
    for (uint32_t i = 0; i < 4000; i++) if (i % 3 != 0) back += (uint32_t)unplace(&t, keys[i], FMT_A, FLAT256, (int)(i & 1));
    CHECK(back == 2666, "every slot given back once (%u)", back);
    vkp_stats(&t, &ne, &np, &sv);
    CHECK(ne == ne0 && np == np0, "only the first table's entries are live (%llu, %llu)", (unsigned long long)ne, (unsigned long long)np);
    free(t.e);

    if (fails) printf("%d checks failed\n", fails); else printf("ok\n");
    return fails != 0;
}
