// Slots shared by the physical memory behind a sparse tile (vka_sparse.c share_take): a tile whose 16 pages are one run of physical memory is the same tile at every place
// the game maps it, so one 64 KiB host slot is bound at all of them (as on hardware, where every place is the same memory). Keyed by the run's first physical address, the
// VkFormat and the tile shape (a 64 KiB block reads the same through any texture of that format: Vulkan's standard 2D block shape) and whether the image is GPU-drawn (a drawn
// image is never uploaded into, so it shares only with drawn ones; a sampled one re-uploads at each new place). Entries are compared whole, so one physical run can hold
// slots in several formats. Open addressing with tombstones: a live entry always has key != 0 (a page-table address), so key 0 means "empty".
#ifndef VKA_PSHARE_H
#define VKA_PSHARE_H
#include <stdint.h>
#include <stdlib.h>

typedef struct { uint64_t key; uint32_t id, refs, fmt, flat; uint8_t drawn, dead; } vkp_ent;   // id: the slot, as slot_take gave it out; dead: a tombstone (probing goes on past it)
typedef struct { vkp_ent *e; uint32_t n, cap; } vkp_tab;                                      // n: live entries and tombstones (the load factor)

static uint32_t vkp_hash(uint64_t k, uint32_t cap) { uint64_t h = (k >> 12) * 0x9E3779B97F4A7C15ull; return (uint32_t)((h ^ (h >> 29)) & (cap - 1)); }

// The entry of this (physical run, format, tile shape, class): its index, or -1
static int vkp_at(const vkp_tab *t, uint64_t key, uint32_t fmt, uint32_t flat, int drawn) {
    if (!t->cap) return -1;
    for (uint32_t i = vkp_hash(key, t->cap), n = 0; n <= t->cap; i = (i + 1) & (t->cap - 1), n++) {
        const vkp_ent *e = &t->e[i];
        if (!e->key && !e->dead) return -1;
        if (!e->dead && e->key == key && e->fmt == fmt && e->flat == flat && e->drawn == (uint8_t)(drawn != 0)) return (int)i;
    }
    return -1;
}

// One place more on this entry (the caller binds the slot again): its index, or -1 when there is none
static int vkp_ref(vkp_tab *t, uint64_t key, uint32_t fmt, uint32_t flat, int drawn) {
    int i = vkp_at(t, key, fmt, flat, drawn);
    if (i < 0) return -1;
    t->e[i].refs++;
    return i;
}

// A new entry for a slot the caller took out (id): its index, or -1 with no memory
static int vkp_add(vkp_tab *t, uint64_t key, uint32_t fmt, uint32_t flat, int drawn, uint32_t id) {
    if ((t->n + 1) * 2 > t->cap) {   // grow (the tombstones left behind)
        uint32_t cap = t->cap ? t->cap * 2 : 1024; vkp_ent *e = (vkp_ent *)calloc(cap, sizeof *e);
        if (!e) return -1;
        uint32_t n = 0;
        for (uint32_t i = 0; i < t->cap; i++) if (t->e[i].key && !t->e[i].dead) {
            uint32_t j = vkp_hash(t->e[i].key, cap); while (e[j].key) j = (j + 1) & (cap - 1);
            e[j] = t->e[i]; n++;
        }
        free(t->e); t->e = e; t->cap = cap; t->n = n;
    }
    uint32_t i = vkp_hash(key, t->cap); while (t->e[i].key && !t->e[i].dead) i = (i + 1) & (t->cap - 1);
    t->e[i] = (vkp_ent){key, id, 1, fmt, flat, (uint8_t)(drawn != 0), 0};
    t->n++;
    return (int)i;
}

// One place fewer: 1 = it was the last one (the caller gives the slot back), 0 = still shared, -1 = there was no entry
static int vkp_put(vkp_tab *t, uint64_t key, uint32_t fmt, uint32_t flat, int drawn) {
    int i = vkp_at(t, key, fmt, flat, drawn);
    if (i < 0) return -1;
    vkp_ent *e = &t->e[i];
    if (e->refs > 1) { e->refs--; return 0; }
    e->id = 0; e->refs = 0; e->dead = 1;
    return 1;
}

// Live entries, the places bound through them, and the slots they saved over one slot per place (any out pointer may be NULL)
static void vkp_stats(const vkp_tab *t, uint64_t *entries, uint64_t *places, uint64_t *saved) {
    uint64_t ne = 0, np = 0;
    for (uint32_t i = 0; i < t->cap; i++) if (t->e[i].key && !t->e[i].dead) { ne++; np += t->e[i].refs; }
    if (entries) *entries = ne;
    if (places) *places = np;
    if (saved) *saved = np - ne;
}
#endif
