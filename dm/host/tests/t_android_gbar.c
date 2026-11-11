// The game's barriers between compute dispatches (dm/gfx/vk_android/vka_gbar.h): a barrier between dispatches becomes a compute-to-compute barrier at the next dispatch,
// with indirect reads when asked for or when that dispatch is indirect; other work after it, or after the narrow barrier in the same stretch, gets compute-to-everything
// before it; a barrier after other work stays full; barriers with nothing between them merge; DM_VK_BAR_NARROW=0 keeps every one full. Then every sequence of up to 8
// commands is replayed through it and checked against NVN's rule: two commands with a barrier between them in the game's stream (or a host full barrier) have, between
// them in the recorded stream, barriers whose source covers the first's stages and whose destination covers each stage the second needs
#include <stdio.h>
#include <string.h>
#include "../../gfx/vk_android/vka_gbar.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static vkgb g; static vkgb_stats s;
static int cmd(int kind, int *ind) { return vkgb_cmd(&g, kind, ind, &s); }
static void fresh(void) { vkgb_reset(&g); memset(&s, 0, sizeof s); int i; cmd(VKGB_C_FULL, &i); }   // a submission's first full barrier

// ---------- The exhaustive check ----------
enum { ST_C = 1, ST_I = 2, ST_X = 4, ST_ALL = 7 };   // compute, indirect-command read, anything else
enum { E_STATE, E_DISP, E_DISP_IND, E_WORK, E_FULL, E_GAME, E_GAME_IND, E_N };
typedef struct { int bar; int src, dst; int kind; int nvn; } item;   // a recorded barrier (src, dst) or command (kind); nvn: NVN orders across this point (a game or host barrier)
static item rec_[64]; static int nrec;
static void put_bar(int src, int dst) { rec_[nrec++] = (item){1, src, dst, 0, 0}; }
static void put_cmd(int kind) { rec_[nrec++] = (item){0, 0, 0, kind, 0}; }
static void put_mark(void) { rec_[nrec++] = (item){2, 0, 0, 0, 1}; }
static void put_vk(int b, int ind) {
    if (b == VKGB_B_NARROW) put_bar(ST_C, ST_C | (ind ? ST_I : 0));
    else if (b == VKGB_B_INDIRECT) put_bar(ST_C, ST_I);
    else if (b == VKGB_B_SETTLE) put_bar(ST_C, ST_ALL);
}
static int src_of(int k) { return k == E_WORK ? ST_ALL : ST_C; }
static int need_of(int k) { return k == E_WORK ? ST_ALL : k == E_DISP_IND ? ST_C | ST_I : ST_C; }
static int check_seq(const int *seq, int n, int on, char *why, size_t wn) {
    vkgb_reset(&g); memset(&s, 0, sizeof s); nrec = 0;
    put_cmd(E_WORK);   // whatever came before the command buffer (vkgb_reset: not known)
    for (int k = 0; k < n; k++) {
        int e = seq[k], ind, b;
        switch (e) {
        case E_STATE: cmd(VKGB_C_STATE, &ind); break;
        case E_DISP: case E_DISP_IND: case E_WORK:
            b = cmd(e == E_DISP ? VKGB_C_DISPATCH : e == E_DISP_IND ? VKGB_C_DISPATCH_IND : VKGB_C_WORK, &ind); put_vk(b, ind); put_cmd(e); break;
        case E_FULL: cmd(VKGB_C_FULL, &ind); put_bar(ST_ALL, ST_ALL); put_mark(); break;
        default:
            put_mark();
            if (!vkgb_game(&g, e == E_GAME_IND ? VKGB_NVN_ORDER_INDIRECT_DATA : 0, on, &s)) { cmd(VKGB_C_FULL, &ind); put_bar(ST_ALL, ST_ALL); }
            break;
        }
    }
    for (int i = 0; i < nrec; i++) {
        if (rec_[i].bar) continue;
        int marked = 0;
        for (int j = i + 1; j < nrec; j++) {
            if (rec_[j].bar == 2) { marked = 1; continue; }
            if (rec_[j].bar || !marked) continue;
            int need = need_of(rec_[j].kind), got = 0;
            for (int m = i + 1; m < j; m++) if (rec_[m].bar == 1 && (rec_[m].src & src_of(rec_[i].kind)) == src_of(rec_[i].kind)) got |= rec_[m].dst;
            if ((got & need) != need) { snprintf(why, wn, "command %d (kind %d) before command %d (kind %d): stages %#x of %#x covered", i, rec_[i].kind, j, rec_[j].kind, got & need, need); return 0; }
        }
    }
    return 1;
}

int main(void) {
    int ind, b;

    // Between two dispatches: compute to compute at the second, nothing at the barrier
    fresh(); cmd(VKGB_C_DISPATCH, &ind);
    CHECK(vkgb_game(&g, 0, 1, &s) == 1, "a barrier between dispatches was not taken");
    CHECK(cmd(VKGB_C_STATE, &ind) == VKGB_B_NONE, "a state command recorded a barrier");
    b = cmd(VKGB_C_DISPATCH, &ind);
    CHECK(b == VKGB_B_NARROW && !ind, "the next dispatch got %d (ind %d), not a compute-to-compute barrier", b, ind);
    CHECK(s.narrow == 1 && s.full == 0, "counts: narrow %llu full %llu", (unsigned long long)s.narrow, (unsigned long long)s.full);

    // NVN's indirect-data bit, or an indirect dispatch: the narrow barrier covers indirect reads
    fresh(); cmd(VKGB_C_DISPATCH, &ind); vkgb_game(&g, VKGB_NVN_ORDER_INDIRECT_DATA, 1, &s);
    b = cmd(VKGB_C_DISPATCH, &ind); CHECK(b == VKGB_B_NARROW && ind, "ORDER_INDIRECT_DATA: %d ind %d", b, ind);
    fresh(); cmd(VKGB_C_DISPATCH, &ind); vkgb_game(&g, 0, 1, &s);
    b = cmd(VKGB_C_DISPATCH_IND, &ind); CHECK(b == VKGB_B_NARROW && ind, "an indirect dispatch: %d ind %d", b, ind);
    // ... and an indirect dispatch after a narrow barrier without them: compute writes to indirect reads, once
    fresh(); cmd(VKGB_C_DISPATCH, &ind); vkgb_game(&g, 0, 1, &s); cmd(VKGB_C_DISPATCH, &ind);
    b = cmd(VKGB_C_DISPATCH_IND, &ind); CHECK(b == VKGB_B_INDIRECT, "an indirect dispatch after the narrow barrier got %d", b);
    b = cmd(VKGB_C_DISPATCH_IND, &ind); CHECK(b == VKGB_B_NONE, "a second indirect dispatch got %d", b);

    // Other work after the pending barrier: compute to everything before it
    fresh(); cmd(VKGB_C_DISPATCH, &ind); vkgb_game(&g, 0, 1, &s);
    b = cmd(VKGB_C_WORK, &ind); CHECK(b == VKGB_B_SETTLE && s.settled == 1, "a copy after the pending barrier got %d", b);
    // ... or after the narrow barrier, in the same stretch: the same, once
    fresh(); cmd(VKGB_C_DISPATCH, &ind); vkgb_game(&g, 0, 1, &s); cmd(VKGB_C_DISPATCH, &ind);
    b = cmd(VKGB_C_WORK, &ind); CHECK(b == VKGB_B_SETTLE && s.owed == 1, "a draw after the narrow barrier got %d", b);
    b = cmd(VKGB_C_WORK, &ind); CHECK(b == VKGB_B_NONE, "the work after it got %d", b);
    // ... and a game barrier after other work is full
    CHECK(vkgb_game(&g, 0, 1, &s) == 0 && s.full == 1, "a barrier after other work was taken");

    // A full barrier covers what is pending or owed
    fresh(); cmd(VKGB_C_DISPATCH, &ind); vkgb_game(&g, 0, 1, &s); cmd(VKGB_C_FULL, &ind);
    CHECK(cmd(VKGB_C_WORK, &ind) == VKGB_B_NONE, "work after a full barrier got a settle");
    fresh(); cmd(VKGB_C_DISPATCH, &ind); vkgb_game(&g, 0, 1, &s); cmd(VKGB_C_DISPATCH, &ind); cmd(VKGB_C_FULL, &ind);
    CHECK(cmd(VKGB_C_WORK, &ind) == VKGB_B_NONE, "work after a full barrier got the owed settle");

    // Merged: a barrier right after another, or right after a full barrier
    fresh(); cmd(VKGB_C_DISPATCH, &ind); vkgb_game(&g, 0, 1, &s); vkgb_game(&g, VKGB_NVN_ORDER_INDIRECT_DATA, 1, &s);
    b = cmd(VKGB_C_DISPATCH, &ind); CHECK(s.merged == 1 && b == VKGB_B_NARROW && ind, "two barriers: merged %llu, %d ind %d", (unsigned long long)s.merged, b, ind);
    fresh(); CHECK(vkgb_game(&g, 0, 1, &s) == 1 && s.merged == 1, "a barrier after a full barrier was not merged");
    CHECK(cmd(VKGB_C_DISPATCH, &ind) == VKGB_B_NONE, "a dispatch after a merged barrier got a barrier");

    // A run of emitters: dispatch, barrier, dispatch, barrier, ...: one narrow barrier each, no full one
    fresh(); for (int k = 0; k < 100; k++) { cmd(VKGB_C_DISPATCH, &ind); vkgb_game(&g, 0, 1, &s); }
    CHECK(s.narrow == 99 && s.full == 0 && s.merged == 0, "a run of 100: narrow %llu full %llu", (unsigned long long)s.narrow, (unsigned long long)s.full);
    CHECK(cmd(VKGB_C_WORK, &ind) == VKGB_B_SETTLE, "the work after the run got no settle");

    // Not known what came before (a new command buffer), or the switch off: full
    vkgb_reset(&g); memset(&s, 0, sizeof s); CHECK(vkgb_game(&g, 0, 1, &s) == 0, "a barrier in a new command buffer before its first full barrier was taken");
    fresh(); cmd(VKGB_C_DISPATCH, &ind); CHECK(vkgb_game(&g, 0, 0, &s) == 0 && s.full == 1, "DM_VK_BAR_NARROW=0 took a barrier");

    // Every sequence of up to 8 commands, with the switch on and off
    int seq[8], total = 0;
    for (int n = 1; n <= 8 && fails < 5; n++) {
        long count = 1; for (int k = 0; k < n; k++) count *= E_N;
        for (long c = 0; c < count && fails < 5; c++) {
            long x = c; for (int k = 0; k < n; k++) { seq[k] = (int)(x % E_N); x /= E_N; }
            for (int on = 0; on < 2; on++) { char why[200]; total++;
                if (!check_seq(seq, n, on, why, sizeof why)) { fails++; printf("FAIL: sequence"); for (int k = 0; k < n; k++) printf(" %d", seq[k]); printf(" (on %d): %s\n", on, why); } }
        }
    }
    printf("%s: t_android_gbar (%d sequences)\n", fails ? "FAILED" : "ok", total);
    return fails != 0;
}
