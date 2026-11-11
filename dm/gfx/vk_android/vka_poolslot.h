// What each slot of the pool tables holds (vka_tex.c "What each slot of the pool tables holds"): a slot is written only when the view it would get differs from the one it
// holds, judged by the view handle and its image's serial (a view handle may be reused once its image is gone; a serial never is). Pure C, no Vulkan: the unit test
// dm/host/tests/t_android_poolslot.c checks it
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const void *v; uint64_t serial; } vkps_ent;
typedef struct { vkps_ent *e; uint32_t n; uint64_t writes, changes; } vkps;   // one table (a binding of the pool set)

// Slot id will hold view v of the image with serial: 1 = write it (another view than it holds, or nothing known), 0 = it holds that view already. *old: the view it held
// (NULL: none known); a write over a known view counts as a change (a slot rewritten under parts that may read it)
static inline int vkps_put(vkps *t, uint32_t id, const void *v, uint64_t serial, const void **old) {
    if (old) *old = NULL;
    if (!v) return 0;
    if (id >= t->n) {
        uint32_t n = t->n ? t->n : 1024; while (n <= id) n *= 2;
        vkps_ent *a = realloc(t->e, (size_t)n * sizeof *a); if (!a) return 1;
        memset(a + t->n, 0, (size_t)(n - t->n) * sizeof *a); t->e = a; t->n = n;
    }
    vkps_ent *s = &t->e[id];
    if (s->v == v && s->serial == serial) return 0;
    if (old) *old = s->v;
    if (s->v) t->changes++;
    s->v = v; s->serial = serial; t->writes++;
    return 1;
}
static inline void vkps_free(vkps *t) { free(t->e); memset(t, 0, sizeof *t); }
