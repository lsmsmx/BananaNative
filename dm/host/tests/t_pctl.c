// pctl free communication offline policy (hle_misc.c, from PR #38): the hooks are looked up by their mangled names in the real swforce registrations, so a missing or misspelled
// takeover fails; no guest module, IPC session or system applet exists in this test
#include "../hle_misc.c"
void dm_applet_enter(const char *name) { (void)name; }   // hle_oe.c: the shop and error stand-ins hold the game out of focus
void dm_applet_leave(void) {}
#include <mach-o/getsect.h>
#include <sys/stat.h>
#include <unistd.h>

static int unexpected, checks, fails;
void *guest_sym(const char *name) { (void)name; unexpected++; return NULL; }
module_t *module_of(const void *p) { (void)p; unexpected++; return NULL; }
void hle_bind_system_event(void *g, int autoclear) { (void)g; (void)autoclear; unexpected++; }
uint32_t dm_sf_host_session(const char *name) { (void)name; unexpected++; return 0; }   // hle_sf.c: the camera library's context (h_cam_Context), not reached here
// hle_fs.c: whether an add-on's DM_AOC_DIR entry opens; here index 2 does not (a .nup without that add-on, a cut-off image), the rest do
static int usable_calls;
int dm_aoc_usable(int index) { usable_calls++; return index != 2; }
// The big-stack take-overs' stubs (HLE_FORCE_BIG) jump here with the implementation in x16; off a guest stack the real entry calls it straight
__asm__(".text\n.p2align 2\n.globl _dm_bigstack_enter\n_dm_bigstack_enter:\n    br x16\n");
extern const struct mach_header_64 _mh_execute_header;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)
static void *hook(const char *name) {
    unsigned long bytes = 0;
    const hle_force_ent *e = (const hle_force_ent *)getsectiondata(&_mh_execute_header, "__DATA", "swforce", &bytes);
    void *fn = NULL; unsigned count = 0;
    for (size_t i = 0; e && i < bytes / sizeof *e; i++) {
        if (strcmp(e[i].name, name)) continue;
        count++; fn = (void *)e[i].fn;
        CHECK(e[i].why && e[i].why[0], "takeover has a reason");
    }
    CHECK(count == 1 && fn, name);
    return count == 1 ? fn : NULL;
}
int main(void) {
    // bool arguments are widened as hle.h requires (only the low 8 bits count); the returns are bools in w0, not nn::Result
    unsigned (*check)(unsigned) = hook("_ZN2nn4pctl32CheckFreeCommunicationPermissionEb");
    unsigned (*begin)(unsigned) = hook("_ZN2nn4pctl25TryBeginFreeCommunicationEb");
    unsigned (*available)(void) = hook("_ZN2nn4pctl28IsFreeCommunicationAvailableEv");
    void (*end)(void) = hook("_ZN2nn4pctl20EndFreeCommunicationEv");
    unsigned (*stereo)(unsigned) = hook("_ZN2nn4pctl27CheckStereoVisionPermissionEb");
    const unsigned args[] = {0, 1, 0x100, 0x101, ~0u};
    if (available) CHECK(available() == 0, "communication unavailable before any permission query");
    for (size_t i = 0; i < sizeof args / sizeof *args; i++) {
        if (check) CHECK(check(args[i]) == 0, "permission query denies communication, with or without UI");
        if (begin) CHECK(begin(args[i]) == 0, "begin agrees with the permission query");
        if (stereo) CHECK(stereo(args[i]) == 0, "stereoscopic 3D not permitted: no VR display, as pl::BeginVrMode answers");
        if (end) end();
        if (check) CHECK(check(args[i]) == 0, "ending communication does not grant permission");
        if (available) CHECK(available() == 0, "permission queries do not make communication available");
    }
    // add-on content (hle_misc.c aoc_load): the numbered entries of DM_AOC_DIR, each asked once whether it opens; one that does not is not owned (Forgejo #30)
    {   char dir[1024], p[1100]; const char *tmp = getenv("TMPDIR"); snprintf(dir, sizeof dir, "%s/t_pctl_aoc.XXXXXX", tmp && *tmp ? tmp : "/private/tmp");
        int (*count)(void) = hook("_ZN2nn3aoc17CountAddOnContentEv");
        int (*list)(int *, int, int) = hook("_ZN2nn3aoc16ListAddOnContentEPiii");
        CHECK(mkdtemp(dir) != NULL, "a DM_AOC_DIR");
        const char *names[] = {"1", "2.nup", "3.img", "notes.txt"};
        for (int i = 0; i < 4; i++) { snprintf(p, sizeof p, "%s/%s", dir, names[i]); if (i) { FILE *f = fopen(p, "w"); if (f) fclose(f); } else mkdir(p, 0755); }
        setenv("DM_AOC_DIR", dir, 1);
        int out[8] = {0};
        if (count) CHECK(count() == 2, "two owned: 1 and 3; 2 does not open, notes.txt is no index");
        if (list) CHECK(list(out, 0, 8) == 2 && out[0] == 1 && out[1] == 3, "the list leaves out the add-on that does not open");
        if (count) CHECK(count() == 2 && usable_calls == 3, "each numbered entry is checked once, at the first query");
        for (int i = 0; i < 4; i++) { snprintf(p, sizeof p, "%s/%s", dir, names[i]); i ? unlink(p) : rmdir(p); }
        rmdir(dir); unsetenv("DM_AOC_DIR");
    }
    CHECK(unexpected == 0, "no guest symbol lookup or event binding");
    printf("pctl: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
