// The host's floating-point environment on the Vulkan replay's side (dm/gfx/vk_android/vka_fpenv.h): a thread that starts with the game's flush-to-zero FPCR runs with
// the host's (0) after vka_fp_enter, and the diagnostic (DM_VK_GUEST_FPCR=1) leaves it; vka_fp_leave puts the caller's back; flush-to-zero really changes a result
// (a denormal product), so the test reads the effect and not only the register; the pipeline cache's file names carry the generation
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "../../gfx/vk_android/vka_fpenv.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define GAME_FPCR 0x1000000ull   // Ocarina of Time's threads: FZ

static float denormal_product(void) { volatile float a = 1e-20f, b = 1e-20f; return a * b; }   // 1e-40: a denormal; 0 under flush-to-zero

typedef struct { uint64_t inherited, entered; int kept; } thr_out;   // (kept is compared on the thread: a compare under flush-to-zero flushes its inputs too)
static void *replay_thread(void *arg) {   // a thread of the replay: what vka_fp_host does at its start
    thr_out *o = arg;
    o->inherited = vka_fpcr_get();
    vka_fp_enter(0);
    o->entered = vka_fpcr_get();
    o->kept = denormal_product() != 0.0f;
    return NULL;
}

int main(void) {
#if defined(__aarch64__)
    const uint64_t start = vka_fpcr_get();
    CHECK(vka_fp_guest_env(NULL) == 0 && vka_fp_guest_env("") == 0 && vka_fp_guest_env("0") == 0, "unset, empty or 0: the host's FPCR");
    CHECK(vka_fp_guest_env("1") == 1, "1: the diagnostic keeps the game's");

    // The game's FPCR on this thread, as a gate handler sees it
    vka_fpcr_set(GAME_FPCR);
    CHECK(vka_fpcr_get() == GAME_FPCR, "FPCR set to the game's: %#llx", (unsigned long long)vka_fpcr_get());
    CHECK(denormal_product() == 0.0f, "flush-to-zero flushes the denormal product");

    // Around a compile on the game's thread
    uint64_t was = vka_fp_enter(0);
    CHECK(was == GAME_FPCR, "enter returns the caller's FPCR: %#llx", (unsigned long long)was);
    CHECK(vka_fpcr_get() == VKA_HOST_FPCR, "the host's FPCR inside: %#llx", (unsigned long long)vka_fpcr_get());
    CHECK(denormal_product() != 0.0f, "the host's FPCR keeps the denormal");
    vka_fp_leave(was);
    CHECK(vka_fpcr_get() == GAME_FPCR, "leave puts the game's back: %#llx", (unsigned long long)vka_fpcr_get());

    // The diagnostic leaves it
    was = vka_fp_enter(1);
    CHECK(was == GAME_FPCR && vka_fpcr_get() == GAME_FPCR, "DM_VK_GUEST_FPCR=1 keeps the game's FPCR");
    vka_fp_leave(was);

    // Nested: a compile inside an inline replay
    uint64_t outer = vka_fp_enter(0), inner = vka_fp_enter(0);
    CHECK(outer == GAME_FPCR && inner == VKA_HOST_FPCR, "nested enter: %#llx %#llx", (unsigned long long)outer, (unsigned long long)inner);
    vka_fp_leave(inner);
    CHECK(vka_fpcr_get() == VKA_HOST_FPCR, "the inner leave keeps the host's");
    vka_fp_leave(outer);
    CHECK(vka_fpcr_get() == GAME_FPCR, "the outer leave puts the game's back");

    // A replay thread created from the game's thread (Linux copies FPCR on clone; Darwin may not: the inherited value is printed, the result checked)
    thr_out o = {0}; pthread_t th;
    CHECK(!pthread_create(&th, NULL, replay_thread, &o) && !pthread_join(th, NULL), "thread");
    printf("t_android_fpenv: a new thread inherited FPCR %#llx here (Linux/Android: the creator's)\n", (unsigned long long)o.inherited);
    CHECK(o.entered == VKA_HOST_FPCR, "the replay thread runs with the host's FPCR: %#llx", (unsigned long long)o.entered);
    CHECK(o.kept, "and keeps denormals");
    CHECK(vka_fpcr_get() == GAME_FPCR, "the creating thread keeps its own");
    vka_fpcr_set(start);
#else
    printf("t_android_fpenv: not arm64, the FPCR checks skipped\n");
#endif

    // The pipeline cache's names: the generation goes last, before .bin; the name before generations is the one removed
    char b[256];
    vka_pcache_name(b, sizeof b, "/t", NULL, VKA_PCACHE_GEN); CHECK(!strcmp(b, "/t/vk_pipelines-fp0.bin"), "system driver: %s", b);
    vka_pcache_name(b, sizeof b, "/t", "Turnip_v32", VKA_PCACHE_GEN); CHECK(!strcmp(b, "/t/vk_pipelines-Turnip_v32-fp0.bin"), "custom driver: %s", b);
    vka_pcache_name(b, sizeof b, "/t", NULL, ""); CHECK(!strcmp(b, "/t/vk_pipelines.bin"), "the generation before (system): %s", b);
    vka_pcache_name(b, sizeof b, "/t", "Turnip_v32", ""); CHECK(!strcmp(b, "/t/vk_pipelines-Turnip_v32.bin"), "the generation before (custom): %s", b);

    printf("t_android_fpenv: %s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
