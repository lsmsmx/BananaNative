// A failed mprotect in the dirty-page tracker (nvn_dirty.c; docs/reference/memory.md "A failed write protection hid CPU writes"). The tracker used to mark pages protected
// whatever mprotect answered: a page left writable never faulted, so a CPU write after the upload was missed and the texture kept its first upload. On the phone mprotect
// fails with ENOMEM once the process reaches vm.max_map_count; this test makes it fail on demand (t_mprotect below), with no effect or after changing the first page only
// (Linux applies a protection mapping by mapping, so a failed call may have changed part of the range).
// Expected values do not depend on the implementation: after a protection that failed, the pages must be writable (a read() into them succeeds, no fault loop), and a texture over
// them must count as written at every check; once a protection succeeds again, writes are seen as before. After an unprotection that failed, the page must still count as
// protected (a write faults instead of being missed), and the failures are counted.
#include <stdatomic.h>
#define mprotect t_mprotect
#include "../nvn/nvn_dirty.c"
#undef mprotect
int mprotect(void *, size_t, int);
#include <stdio.h>

module_t *g_mod;
int g_nmod;
module_t *module_of(const void *p) { (void)p; return NULL; }
void crash_install(void);

static int g_fail;
#define EXPECT(name, want, got) do { \
    long long want_ = (long long)(want), got_ = (long long)(got); \
    printf("%-64s expected %-4lld -> got %-4lld %s\n", name, want_, got_, want_ == got_ ? "ok" : "FAIL"); \
    if (want_ != got_) g_fail++; \
} while (0)

// 0 = pass through; 1 = fail calls asking for that protection with no effect; 2 = change the first page, then fail
static int g_mode, g_mode_prot;
int t_mprotect(void *a, size_t n, int prot) {
    if (g_mode && prot == g_mode_prot) {
        if (g_mode == 2 && n > (size_t)getpagesize()) mprotect(a, (size_t)getpagesize(), prot);
        errno = ENOMEM; return -1;
    }
    return mprotect(a, n, prot);
}
static int g_pipe[2];
static int writable(uint8_t *p) {   // a read() into the page: the kernel answers EFAULT for a read-only page (no signal, so no fault-handler loop)
    char c = 'x'; if (write(g_pipe[1], &c, 1) != 1) return -1;
    ssize_t r = read(g_pipe[0], p, 1);
    if (r != 1) { char d; (void)read(g_pipe[0], &d, 1); return 0; }
    return 1;
}
static int count_state(uint8_t *mem, size_t len, int want) {
    range_t *r = find_span((uintptr_t)mem, len); if (!r) return -1;
    int n = 0; for (uintptr_t i = ((uintptr_t)mem - r->base) >> g_shift; i <= ((uintptr_t)mem + len - 1 - r->base) >> g_shift; i++) n += atomic_load(&r->state[i]) == want;
    return n;
}

int main(void) {
    crash_install();
    if (pipe(g_pipe)) { perror("pipe"); return 2; }
    size_t pg = (size_t)getpagesize(), len = 4 * pg;
    uint8_t *m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m == MAP_FAILED) { perror("mmap"); return 2; }
    nvn_dirty_add_range(m, len);
    uint64_t f0; int e0; nvn_dirty_mp_fail(&f0, &e0);

    // 1. Protection fails with no effect
    uint64_t s = nvn_dirty_now(); g_mode = 1; g_mode_prot = PROT_READ; nvn_dirty_protect(m, len); g_mode = 0;
    EXPECT("1 failed protection: pages NONE (as mprotect left them)", 4, count_state(m, len, PG_NONE));
    EXPECT("1 failed protection: page writable", 1, writable(m + pg));
    EXPECT("1 failed protection: texture counts as written", 1, nvn_dirty_since(m, len, s));
    s = nvn_dirty_now();
    EXPECT("1 ... and again at the next check (no write in between)", 1, nvn_dirty_since(m, len, s));
    m[2 * pg] = 1;   // a direct CPU write: no fault (the page is writable), must not hang
    EXPECT("1 ... after a CPU write", 1, nvn_dirty_since(m, len, s));
    uint64_t f1; int e1; nvn_dirty_mp_fail(&f1, &e1);
    EXPECT("1 failures counted", 1, f1 > f0);
    EXPECT("1 last errno ENOMEM", ENOMEM, e1);

    // 2. The next protection succeeds: one more upload (the stamp), then tracked as any other
    s = nvn_dirty_now(); nvn_dirty_protect(m, len);
    EXPECT("2 protection succeeds: pages PROT", 4, count_state(m, len, PG_PROT));
    EXPECT("2 counts as written once (pages restamped)", 1, nvn_dirty_since(m, len, s));
    s = nvn_dirty_now();
    EXPECT("2 clean afterwards", 0, nvn_dirty_since(m, len, s));
    m[pg + 8] = 2;   // faults, unprotected and stamped by the handler
    EXPECT("2 a CPU write is seen", 1, nvn_dirty_since(m, len, s));

    // 3. Protection fails after changing the first page (partial): every page writable again, the texture counts as written
    prep_write(m, len);
    s = nvn_dirty_now(); g_mode = 2; g_mode_prot = PROT_READ; nvn_dirty_protect(m, len); g_mode = 0;
    EXPECT("3 partial failure: first page writable again", 1, writable(m));
    EXPECT("3 partial failure: pages NONE", 4, count_state(m, len, PG_NONE));
    m[0] = 3;
    s = nvn_dirty_now();
    EXPECT("3 texture counts as written", 1, nvn_dirty_since(m, len, s));

    // 4. Unprotection fails: the page stays PROT (it is still read-only), nothing is lost
    s = nvn_dirty_now(); nvn_dirty_protect(m, len); s = nvn_dirty_now();
    g_mode = 1; g_mode_prot = PROT_READ | PROT_WRITE; prep_write(m + pg, 16); g_mode = 0;
    EXPECT("4 failed unprotection: page still read-only", 0, writable(m + pg));
    EXPECT("4 failed unprotection: its state stays PROT", 4, count_state(m, len, PG_PROT));
    prep_write(m + pg, 16);   // succeeds now
    EXPECT("4 retried unprotection: page writable", 1, writable(m + pg));
    EXPECT("4 ... and counted as written", 1, nvn_dirty_since(m, len, s));

    printf("t_dirty_mpfail: %s\n", g_fail ? "FAILED" : "all passed");
    return g_fail != 0;
}
