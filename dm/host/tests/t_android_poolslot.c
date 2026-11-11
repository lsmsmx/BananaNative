// The pool tables' slots on the Android replay (dm/gfx/vk_android/vka_poolslot.h): a slot is written only when the view it would get is another than it holds, judged by
// the view handle and its image's serial, and a write over a known view counts as a change (a slot rewritten while parts that may read it are on the GPU)
#include "../../gfx/vk_android/vka_poolslot.h"
#include <stdio.h>

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    vkps t = {0}; const void *old = (const void *)1;
    int a, b; const void *va = &a, *vb = &b;
    CHECK(vkps_put(&t, 5, va, 10, &old) == 1 && old == NULL && t.writes == 1 && t.changes == 0, "a first write: written, no change");
    CHECK(vkps_put(&t, 5, va, 10, &old) == 0 && t.writes == 1, "the same view again: not written");
    CHECK(vkps_put(&t, 5, vb, 10, &old) == 1 && old == va && t.changes == 1, "another view of the same image: written, a change");
    CHECK(vkps_put(&t, 5, vb, 11, &old) == 1 && old == vb && t.changes == 2, "the same handle of a new image (a handle reused after its image went): written");
    CHECK(vkps_put(&t, 5, NULL, 0, &old) == 0 && t.e[5].v == vb, "no view: nothing written, the slot keeps what it holds");
    CHECK(vkps_put(&t, 3000000, va, 1, &old) == 1 && t.n > 3000000 && t.e[5].v == vb && t.e[5].serial == 11, "a high ID: the table grows, earlier slots kept");
    CHECK(vkps_put(&t, 4096, va, 1, NULL) == 1 && vkps_put(&t, 4096, va, 1, NULL) == 0, "without old");
    vkps_free(&t);
    if (g_fail) { printf("t_android_poolslot: %d FAILED\n", g_fail); return 1; }
    printf("PASS: pool table slots (vka_poolslot.h): written only when their view changes, changes counted\n");
    return 0;
}
