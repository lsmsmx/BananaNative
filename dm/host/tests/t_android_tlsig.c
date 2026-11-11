// Host-signalled timelines on the Android replay (dm/gfx/vk_android/vka_tlsig.h): the ring keeps queue order and its bound, and each timeline's host signals
// stay strictly increasing (a value at or below one signalled already, by the ring or by the host's own signal of a failed submission, is skipped)
#include <stdio.h>
#include <string.h>
#include "../../gfx/vk_android/vka_tlsig.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static tls_ring r;
int main(void) {
    int a, b; void *tl = &a, *up = &b; tls_ent e;
    CHECK(tls_empty(&r) && !tls_pop(&r, &e), "a new ring is empty");
    // queue order across two timelines, with wrap-around
    for (int round = 0; round < 3; round++) {
        for (int i = 0; i < TLS_N; i++) CHECK(tls_push(&r, i & 1 ? up : tl, 1000u * round + i, (void *)(uintptr_t)(i + 1)), "push %d", i);
        CHECK(tls_full(&r) && !tls_push(&r, tl, 1, NULL), "a full ring refuses");
        for (int i = 0; i < TLS_N; i++) { CHECK(tls_pop(&r, &e), "pop %d", i); CHECK(e.v == 1000u * round + i && e.sem == (i & 1 ? up : tl) && e.fence == (void *)(uintptr_t)(i + 1), "order at %d", i); }
        CHECK(tls_empty(&r), "drained");
    }
    // strictly increasing per timeline, timelines independent
    memset(&r, 0, sizeof r);
    CHECK(tls_take(&r, tl, 5), "first value"); CHECK(!tls_take(&r, tl, 5), "the same value again"); CHECK(!tls_take(&r, tl, 4), "a lower value");
    CHECK(tls_take(&r, up, 1), "another timeline's own values"); CHECK(tls_take(&r, tl, 6), "a higher value");
    tls_note(&r, tl, 9);   // the host signalled 9 itself (a failed submission)
    CHECK(!tls_take(&r, tl, 7) && !tls_take(&r, tl, 9), "values covered by the host's own signal are skipped"); CHECK(tls_take(&r, tl, 10), "and later ones signalled");
    if (fails) { printf("t_android_tlsig: %d FAILED\n", fails); return 1; }
    printf("t_android_tlsig: ok\n"); return 0;
}
