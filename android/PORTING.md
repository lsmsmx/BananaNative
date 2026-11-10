# Porting the host to Android

This page tracks the Android port of nxnative's CPU / HLE host: what is Apple-bound, what replaces it, how the rewritten guest code finds its thread slots on Bionic, what is still a stub, and the order of the next pieces. `libnxhost.so` builds and links for arm64-v8a, API 33+, and the Mac and iPhone builds behave as before. The test APK is built from the merge of three branches: the launcher and bring-up fixes (`android-host`), audio, movies and input (`android-av`) and the NVN → Vulkan replay (`android-vk`); the shader side (`android-shaders`) is not in it. The packer (`scripts/pack_android.py`) makes Donkey Kong Bananza's pack. [README.md](README.md) describes the pieces and [TESTING.md](TESTING.md) the tests on the phone. Nothing has run on the phone yet.

## Status

| | |
|---|---|
| `libnxhost.so` | builds and links with NDK 28.2 (`--no-undefined`) against `aaudio`, `mediandk`, `android`, `log`; `libvulkan.so` is opened at run time: 7.8 MB with debug info, 1.46 MB stripped. Exports `nxhost_main`, the launcher interface `nxhost_*` (input, touch, motion, rumble, lifecycle in `dm/host/android/input_android.h`; `nxhost_set_window` and `nxhost_vk_selftest` in `dm/gfx/vk_android/vk_android.h`) and `JNI_OnLoad` |
| NVN → Vulkan | [`dm/gfx/vk_android`](../dm/gfx/vk_android): device, placed guest memory and pools, textures and bindless tables, the replay of clears / copies / barriers / fences, the present, and draws and dispatches on the game's SPIR-V library ([SHADERS.md](SHADERS.md#what-the-replay-does)). The self-test (TESTING.md, Test 2) passed on the phone before draws; with draws it passes its draw, dispatch and read-back checks on the Android emulator. No game frame drawn yet; see [The NVN → Vulkan replay](#the-nvn--vulkan-replay) |
| Audio, movies, input | AAudio output, AAC and H.264 on `AMediaCodec`, gamepads, touch, phone motion, rumble and the lifecycle in `dm/host/android`; see [Audio, video and input on Android](#audio-video-and-input-on-android). Built, pure parts unit-tested on the Mac, not run on a phone |
| Mac host | builds as before; the host unit tests pass (`make -C dm/host/tests GAME=010015100B514000 run` on each branch, and `GAME=040074A01BF12000` on the merge, with `t_android_av` and `t_android_input`), and the CPU-only graphics tests pass |
| iPhone | every host and graphics C file compiles for `arm64-apple-ios26.0` (syntax check; no IPA built) |
| Rewriter | `patch_nso.py --target android`; verified with Capstone 5 on Donkey Kong Bananza 458752: `--target apple` is byte-identical to the default on main and sdk, and every android slot sequence has its `ldr` (see [Tests](#tests)) |
| Launcher | `android/app/out/nxnative-android.apk` (1.5 MB): game list, folder pickers, All files access, the host on its own 8 MB thread, log file + logcat, Share log; the game's `SurfaceView` to `nxhost_set_window`, gamepads, touch, motion, lifecycle, audio focus and route to the host's C interface; **Graphics self-test**. Started on an API 36 emulator up to the loader with a synthetic pack; the self-test ran there up to its presents (no placed maps on the emulator, so it fails its checks) |
| Pack | `scripts/pack_android.py`; Donkey Kong Bananza's is 59 MB in `<dump folder>/040074A01BF12000-work/android-pack` |
| Phone | not run. The facts about the device come from the Vulkan probe of `android/spike` on a Honor PNM-N49 (Adreno 840, Android 17, 4K pages, 39-bit user address space) |

Build:

```sh
NDK=$HOME/Library/Android/sdk/ndk/28.2.13676358
cmake -S android/host -B <build folder> -G Ninja -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
      -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=33 -DGAME=040074A01BF12000
cmake --build <build folder>          # → <build folder>/libnxhost.so
```

`GAME` picks the profile, as `make GAME=` does for the Mac host. Keep the build folder outside the repository.

## The platform layer

[`dm/host/plat/plat.h`](../dm/host/plat/plat.h) names every service whose system interface differs between Apple's kernels and Linux. The rest of `dm/host` is plain C and POSIX.

- **Apple:** [`plat_apple.h`](../dm/host/plat/plat_apple.h) implements the layer as static inline wrappers over the exact calls the host made before. The Makefile, the Xcode project and the unit tests link as they did, with no new object.
- **Android:** [`plat_android.c`](../dm/host/plat/plat_android.c) implements it on Bionic.
- **Frameworks:** the subsystems that are whole Apple frameworks (CoreAudio output, AAC through AudioToolbox, the Mach power and thread-policy diagnostics in `hle_nvn.c`) stay in their files behind `#if PLAT_APPLE`, with an Android branch that logs `[RED]` and fails the way the Apple call fails.
- **Platform macros:** `PLAT_APPLE`, `PLAT_ANDROID`, `PLAT_MAC` and `PLAT_IOS` replace `TARGET_OS_*` in the files the Android build compiles.

The Android build leaves out three kinds of files and links Android code or stubs in their place:

- `hle_movie_avc.c` (VideoToolbox) → [`dm/host/plat/movie_avc_android.c`](../dm/host/plat/movie_avc_android.c) (`AMediaCodec`);
- `front_mac.m` → [`dm/host/plat/front_android.c`](../dm/host/plat/front_android.c), which also defines `nxhost_main`;
- every Objective-C file of `dm/gfx` (`nvn_mtl.m`, `nvn_refl.m`, `host_msl.m`, `hle_vk.m`, `hle_gl.m`, `nvn_deswizzle_gpu.m`) and `memstat.m` / `haptics_apple.m` → the Vulkan replay [`dm/gfx/vk_android`](../dm/gfx/vk_android) for the `mtl_*` interface, and [`dm/gfx/nvn/android_stub.c`](../dm/gfx/nvn/android_stub.c) for the rest.

Assembly is not edited. [`android/host/macho_asm_to_elf.py`](host/macho_asm_to_elf.py) turns `entry.S`, `nvn_entry.S` and the generated `nvn_thunks.S` into ELF at build time. It drops the leading underscore, maps `@PAGE` / `@PAGEOFF` to `sym` / `:lo12:sym` and `.zerofill` to a `.bss` block, and sends `bl _mach_absolute_time` to `plat_abs_time`. The inline assembly in `hle.h` and `hle_bigstack.c` uses `PLAT_ASM_SYM` / `PLAT_ASM_PAGE` / `PLAT_ASM_HIDDEN`. The HLE tables use `PLAT_TABLE(name)`, which is a Mach-O `__DATA,name` section or an ELF section that `plat_table` reads through `__start_` / `__stop_`.

## Apple dependencies and their replacements

Status column: **done** is implemented on Android in this milestone; **stub** logs `[RED] … not yet on Android` and fails safely; **off** is a Mac-only diagnostic that Android leaves out.

| File | Apple API | Android replacement | Status |
|---|---|---|---|
| `hle_os.c`, `nvn_dirty.c`, `hle_nvn.c`, `nvn_entry.S` | `mach_absolute_time`, `mach_timebase_info`, `mach_wait_until` | `clock_gettime(CLOCK_MONOTONIC)`, timebase 1/1, `clock_nanosleep(TIMER_ABSTIME)` (`plat_abs_time`, `plat_timebase`, `plat_wait_until`) | done |
| `hle_fs.c`, `hle_time.c`, `hle_nvn.c` | `clock_gettime_nsec_np` | `clock_gettime` (`plat_clock_ns`) | done |
| `main.c` | `sysctl(KERN_PROC_PID)` process start | `/proc/self/stat` starttime against `CLOCK_BOOTTIME` (`plat_process_age_ns`) | done |
| `main.c` | `CFBundleGetValueForInfoDictionaryKey("DMCommit")` | `DM_BUILD_COMMIT`, which the launcher sets from the commit Gradle stamps into the APK (`plat_build_commit`) | done |
| `hle_thread.c`, `probe.c` | `thread_suspend`, `thread_get_state`, `thread_resume` | `tgkill` with `SIGRTMIN + 2`: the handler copies the registers out of its `ucontext_t` and waits on a futex until the resume. One thread at a time, with a 500 ms timeout (`plat_thread_suspend` / `_get_regs` / `_resume`) | done |
| `hle_thread.c`, `probe.c` | `task_threads`, `mach_thread_self`, `pthread_mach_thread_np` | `/proc/self/task`, `gettid`, `pthread_gettid_np` (`plat_threads`, `plat_thread_self`, `plat_thread_of`) | done |
| `hle_thread.c`, `hle_nvn.c` | `thread_info(THREAD_BASIC_INFO)` run state and CPU time | `/proc/self/task/<tid>/stat` state letter; the thread's CPU clock `(~tid << 3) \| 6`; `getrusage(RUSAGE_THREAD)` | done |
| `hle_thread.c`, `hle_audio.c`, `hle_movie.c`, `hle_sdk16.c`, `hle_nvn.c` | `pthread_set_qos_class_self_np`, `pthread_get_qos_class_np`, `pthread_override_qos_class_*` | `setpriority` on the thread ID: interactive −4 (`THREAD_PRIORITY_DISPLAY`), initiated 0, utility 10. No override (an app cannot lend priority) | done (no override) |
| `hle_thread.c`, `hle_movie.c` | `pthread_setname_np(name)` | `pthread_setname_np(self, name)`, cut to 15 bytes | done |
| `hle_svc.c` | `pthread_get_stackaddr_np`, `pthread_get_stacksize_np` | `pthread_getattr_np` + `pthread_attr_getstack` | done |
| `hle_movie.c` | `pthread_cond_timedwait_relative_np` | `pthread_cond_clockwait(CLOCK_MONOTONIC)` | done |
| `crash.c`, `probe.c`, `hle_thread.c`, `hle_cheats.c`, `hle_fpslock.c`, `hle_uecvar.c`, `hle_svc.c`, `hle_movie.c`, `hle_nvn.c` | `vm_read_overwrite`, `vm_write` (kernel copies that fail instead of faulting) | `process_vm_readv` / `process_vm_writev` on our own pid; a pipe copy for reads if seccomp refuses them (`plat_mem_read`, `plat_mem_write`) | done |
| `hle_fs.c`, `hle_uemalloc.c`, `probe.c` | `vm_region_64`, `mach_vm_region` | `/proc/self/maps` (`plat_mem_region`) | done |
| `hle_cheats.c`, `probe.c` | `mach_vm_remap` (a writable alias of main's code, the write watch) | a second `mmap` of a memfd, which needs memfd-backed modules in the loader (`plat_mem_alias`) | stub: cheats write data only |
| `hle_nvn.c` | `vm_remap` (NVN virtual pools aliasing physical pools) | the placed Vulkan memory design below: pools are sub-ranges of guest RAM, a virtual pool is a second placed mapping | stub: physical pools done (`vka_mem.c`), virtual pools have no GPU side |
| `loader.c` | `vm_remap` of signed `__TEXT` sections (iOS embedding) | not needed: Android takes the macOS path (mmap RW, copy, mprotect RX), which the probe found allowed | done |
| `crash.c`, `probe.c`, `hle_count.c`, `hle_cheats.c` | `sys_icache_invalidate` | `__builtin___clear_cache` | done |
| `crash.c`, `probe.c`, `hle_thread.c` | `uc_mcontext->__ss` / `__ns` / `__es` | `uc_mcontext.regs` / `sp` / `pc` / `pstate`; the `fpsimd_context` and `esr_context` records of `__reserved` (`PLAT_UC_*`, `plat_uc_fpsimd`, `plat_uc_esr`) | done |
| `loader.c`, `hle.c`, `hle_nvn.c` | `getsectiondata(&_mh_execute_header, "__DATA", …)` | ELF sections `swhle`, `swforce`, `swnvn` bounded by `__start_` / `__stop_` (`plat_table`) | done |
| `loader.c`, `hle_ro.c`, `save_sync.c`, `hle_nvn.c` | CommonCrypto SHA-256 | a FIPS 180-4 implementation in `plat_android.c` (`plat_sha256_*`). No AES or other hash is used by the host | done |
| `hle_nvn.c` | `CFDictionary` (program keys) | an open-addressing pointer map (`plat_ptrmap_*`) | done |
| `hle_uemalloc.c` | `malloc_create_zone`, `malloc_zone_*`, `malloc_size`, `malloc_good_size` | the process heap (Scudo) and `malloc_usable_size` (`plat_zone_*`) | done |
| `hle_audio.c`, `tests/t_audio_spsc.c` | `dispatch_semaphore` | POSIX `sem_t`, `sem_clockwait` (`plat_sem_*`) | done |
| `hle_cheats.c` | `dispatch_queue` with blocks | a worker thread with a job list; the blocks became functions with a context (`plat_queue_*`) | done |
| `hle_lifecycle.c` | `dispatch_after`, `DISPATCH_SOURCE_TYPE_SIGNAL` | a sleeping thread; a self-pipe signal handler and a reader thread (`plat_after_ns`, `plat_on_signal`) | done |
| `hle_fs.c`, `save_sync.c` | `clonefile` | a full copy of the tree with xattrs and times: no copy-on-write on ext4 / f2fs (`plat_clone_tree`) | done (slower) |
| `hle_fs.c`, `save_sync.c` | `renamex_np(RENAME_SWAP)` | `renameat2(RENAME_EXCHANGE)` (`plat_rename_swap`) | done |
| `hle_fs.c`, `save_sync.c` | `getxattr` / `setxattr` / `removexattr` with `XATTR_NOFOLLOW`, `ENOATTR` | `lgetxattr` / `lsetxattr` / `lremovexattr` in the `user.` namespace, `ENODATA` | done |
| `hle_fdstat.c` | `fcntl(F_GETPATH)`, `getdtablesize` | `readlink(/proc/self/fd/N)`, `sysconf(_SC_OPEN_MAX)` | done |
| `save_sync.c` | `sysctlbyname("hw.model")`, `st_mtimespec` | `ro.product.manufacturer` + `ro.product.model`, `st_mtime` | done |
| `hle_hid.c` | `mergesort` (BSD) | a stable merge sort (`plat_sort_stable`) | done |
| `hle_audio.c` | AudioToolbox `AudioUnit` (DefaultOutput / RemoteIO) | AAudio (`AAudioStreamBuilder`, a data callback that calls the existing `pull_device`) | done (not run on a phone) |
| `hle_movie.c` | AudioToolbox `AudioConverter` (AAC) | `AMediaCodec` `audio/mp4a-latm` with csd-0 = the AudioSpecificConfig (`dm/host/android/aac_android.c`) | done (not run on a phone) |
| `hle_movie_avc.c`, `hle_codec_video.c` | VideoToolbox H.264 | `AMediaCodec` `video/avc` with ByteBuffer output, copied out as NV12 / RGBA through the `mavc_*` interface (`movie_avc_android.c`) | done (not run on a phone) |
| `nvn_capture.c` | `compression_encode_buffer` (LZFSE) | captures are written uncompressed (a Mac capture's LZFSE chunks do not decode) | done (raw) |
| `hle_nvn.c` | `task_info(TASK_VM_INFO)` footprint | `/proc/self/statm` resident set (`plat_footprint_bytes`) | done |
| `hle_nvn.c` | `task_info(TASK_POWER_INFO_V2)`, `hw.nperflevels`, `thread_selfcounts` | none yet (`perf_event_open` for cycles and instructions) | off |
| `hle_nvn.c` | `thread_policy_set(THREAD_TIME_CONSTRAINT_POLICY)` | none for an app (`SCHED_FIFO` needs a permission); the present thread keeps its nice value | off |
| `front_mac.m`, `pad_*.h`, `motion_*.h`, `haptics_apple.m` | AppKit / UIKit, GameController, CoreHaptics, CoreMotion, IOKit HID | a JNI shim from the Activity: `InputDevice` / `MotionEvent` / `KeyEvent` into `dm_input.h` and `dm_touch_set`, `ASensorManager` for motion, a rumble callback for `Vibrator` / `VibratorManager` (`dm/host/android/input_android.c`; the launcher calls it from `nxlaunch.c`) | done (not run on a phone); the launcher does not draw on-screen controls or vibrate yet |
| `front_mac.m` | the window, `CAMetalLayer`, the text input box, the progress screen | a `SurfaceView` → `ANativeWindow` → `VkSurfaceKHR` (`nxhost_set_window`, `vka_present.c`); Android dialogs over JNI | window: done on the replay side; text input and progress screen: stub |
| `memstat.m` | `malloc_zone_statistics` | `mallinfo` | not built |
| `nvn_mtl.m` and the other Metal files | Metal, MetalFX | the NVN → Vulkan replay, `dm/gfx/vk_android` | memory, textures, clears, copies, fences, present: done; draws, dispatches: done; frame captures and their replay on the phone: done (`vka_capture.c`, `vka_replay.c`); CPU texel reads, statistics: stub |

ABI notes:

- **Arguments:** Android uses standard AAPCS64, which is the Switch's own convention. Variadic arguments travel in registers and callers do not widen narrow arguments. The host's habit of declaring narrow parameters as `int` and reading variadics by hand stays correct; nothing depends on Apple's variant.
- **libc passthrough:** the table in `hle_table.c` maps guest calls straight to Bionic. The `struct tm` layout is the same on LP64 Bionic as on Darwin.
- **Signals:** ART's `libsigchain` sits in front of `sigaction`. The host's `SIGSEGV`, `SIGBUS`, `SIGILL`, `SIGTRAP` and `SIGABRT` handlers become chained handlers: a fault goes to ART's fault manager first (implicit null checks, stack overflow, the userfaultfd GC's `SIGBUS`, all about ART's own threads and code) and reaches `crash.c` when ART declines it. sigchain installs its handler with `SA_ONSTACK`, so the alternate stack still holds. Guest threads, and the thread `nxhost_main` runs on, are plain pthreads that never attach to ART. After its report `crash.c` hands the signal on to the handler it replaced (debuggerd's), so the phone also writes a tombstone. Checked on an API 36 emulator: an abort in the host printed the host's report, then debuggerd's backtrace through `libnxhost.so`. Still to check on the phone: a write-protect fault from `nvn_dirty.c` must reach `crash.c` and return.
- **Heap pointer tagging:** Bionic's Scudo puts a tag in the top byte of malloc'd pointers (0xb4) on Android 11+. The host hands malloc'd blocks to the guest (its TLS block and thread-local region, the libc passthrough), so the manifest sets `allowNativeHeapPointerTagging="false"` and `nxhost_main` sets `M_HEAP_TAGGING_LEVEL_NONE` before anything else.

## Thread slots

Facts from the probe:

- `tpidr_el0` is Bionic's thread pointer and can be written at EL0.
- `tpidrro_el0` reads 0 on every thread and traps on write. The Linux kernel sets it for compat tasks only.
- `x18` is the shadow call stack register of platform code.
- `cntfrq_el0` is 19.2 MHz.

The rewriter already takes every guest use of these registers out of the guest's hands:

- `x18` lives in slot K18;
- `mrs` / `msr tpidr_el0` use slot KTLS;
- `mrs tpidrro_el0` uses slot KTLR;
- `svc` and `mrs cntpct_el0` / `cntvct_el0` go to the host gate through slot KGATE.

The guest therefore never reads or writes a real `tpidr_el0`, `tpidrro_el0` or `x18`. Eden's NCE approach of swapping the guest's `tpidr_el0` in and out around every host call is not needed, and it would break Bionic in every signal handler that runs in between. The real `x18` keeps Bionic's shadow call stack pointer the whole time, so host code built with SCS stays safe. The real `tpidr_el0` keeps Bionic's TCB, so errno, the stack guard at `tp + 0x28` and `pthread_self` keep working inside the host.

All that changes on Android is where the four slots live:

| | Apple (`--target apple`, the default) | Android (`--target android`) |
|---|---|---|
| Slot base | `tpidrro_el0` = the pthread TSD array | `[tpidr_el0 + 0x10]` = Bionic's `TLS_SLOT_APP` (TCB word 2, left to applications since Android 10), which holds this thread's `plat_slots_t` |
| Key page entry (`install_key_page`) | pthread key × 8 | the slot's offset in `plat_slots_t`: K18 0, KTLS 8, KTLR 0x10, KGATE 0x18 |
| Reading the base into `xT` | `mrs xT, tpidrro_el0` (`0xD53BD060 \| T`) | `mrs xT, tpidr_el0` (`0xD53BD040 \| T`); `ldr xT, [xT, #0x10]` (`0xF9400800 \| T << 5 \| T`) |
| Then, as before | `adrp xS, keypage`; `ldr xS, [xS, #idx × 8]`; `add xT, xT, xS`; then `ldr` / `str` through `xT` | the same |
| Host side | `pthread_getspecific` / `pthread_setspecific` on `g_key18` … (`dm_slot_get` / `dm_slot_set`) | `plat_slots()->slot18` …: `plat_slots_own` allocates the block on a thread's first use, puts it into TCB word 2 and frees it at thread exit through a pthread key destructor |
| Fixups header | none | `# TARGET_ANDROID 1`; each loader refuses a module of the other target |
| Start-up check (`tsd_check`) | write K18 through the key, read it back at `tpidrro_el0 + entry` | write K18, read it back at `[tpidr_el0 + 0x10] + entry`. A block in that word without the host's magic number stops the host with a `[RED]` line (another library uses the slot) |

All 13 slot sequences of `patch_nso.py` read the base through `emit_tp`. The android form is one instruction longer. No stub depends on a fixed length: the only fixed layout, the `br x18` landing pads of `site_br`, starts 24 words after the stub, and the longest android prologue takes 22 of them.

Hazards and their handling:

- **No red zone on Linux.** The rewritten sequences park `x16`, `x17` or a temporary up to 24 bytes below `sp` (`stur` / `ldur`, as on Apple's 128-byte red zone). Linux writes a signal frame directly below `sp`, so every signal that can hit a guest thread must run on the alternate signal stack. The host's handlers already pass `SA_ONSTACK`: `crash.c`'s, the pause signal in `hle_thread.c`, and the new suspend and `plat_on_signal` handlers. Bionic gives every thread a `sigaltstack`, and `crash_thread_init` installs one per guest thread. ART's own handlers (GC suspend, profiling) must not reach guest threads: those are host pthreads that never attach to the VM, so only process-directed signals can land there. If one shows up on the phone, change the rewriter to keep its temporaries in the slot block instead of below `sp`.
- **The counter frequency.** `cntpct` / `cntvct` reads already go through the gate. `guest_ticks` (`hle_os.c`) answers 19.2 MHz or, for an Ounce SDK, 31.25 MHz from the host clock, so no scaling is needed in the rewriter. A guest `mrs cntfrq_el0` is not rewritten on either platform. It reads 24 MHz on Apple silicon and 19.2 MHz on this phone, and the games seen so far take the frequency from the SDK instead. The survey's census of `cntfrq` reads will show whether a game reads it.
- **Threads that run guest code** get their block in `guest_thread_init` (`dm_slot_set`) before any guest instruction runs, as on Apple. A thread that never called it has 0 in TCB word 2, and its first guest slot access faults at a small address: that names the missing init. A host read (`dm_slot_get`) never allocates and reads NULL on such a thread, as an unset pthread key does on Apple, so signal handlers may read the slots on any thread.

## Executable memory and the address space

The probe allows memfd RW + RX views at a fixed address, anonymous RWX memory and RW → RX `mprotect`. The loader keeps the macOS path: an anonymous RW mapping, a copy of the image and an `mprotect` of the text and patch areas to RX, with no code signing step. The user address space is 39 bits (512 GiB), and `0x4000000000` is free. `plat_mem_reserve` puts the guest block that `hle_svc.c` reserves (`mem_reserve`, 9.75 GiB for Donkey Kong Bananza) there with `MAP_FIXED_NOREPLACE | MAP_NORESERVE`, so the placed Vulkan memory of the next steps lands at known addresses; if that range is taken it tries the next 64 GiB boundaries, always below `0x8000000000` (`DM_GUEST_BASE=<hex>` picks another start), and names the block `nxnative guest` in `/proc/self/maps`. Every start is 2 MiB-aligned (`DM_GUEST_BASE` must be, and the fallback the kernel picks when no fixed address is free is trimmed to 2 MiB), so the heap and alias regions start where placed Vulkan memory can be mapped ([The NVN → Vulkan replay](#the-nvn--vulkan-replay)).

## Stubs in this build

Each stub logs once and then fails, or reports "nothing to do":

- **Graphics.** The replay (`dm/gfx/vk_android`) draws and dispatches; it logs once each for what it does not do yet: the shader variants, transform feedback, texel buffers, DrawTexture and the rest listed in [SHADERS.md](SHADERS.md#what-the-replay-does), colour clears with a partial channel mask (stencil clears with a write mask are drawn), buffer copies of packed depth-stencil formats, depth upload from guest memory, texture buffers, sampler reduction modes, pools outside placed guest RAM. `dm/gfx/nvn/android_stub.c` keeps 31 functions: statistics (`mtl_*_stats`, `mtl_gpu_ms`, `mtl_bw` …), `mtl_read_texels` (CPU texel reads), `mtl_ub_present`, `mtl_pso_prebuild_tool`, and `dm_gfx_*` (screenshots and dumps); frame capture is `vka_capture.c` ([TESTING.md](TESTING.md#frame-capture-and-replay-on-the-phone)). Vulkan games (`hle_vk.m`) and OpenGL games (`hle_gl.m`) are not built at all: their imports resolve to the host's named unimplemented stubs.
- **Front end.** `front_android.c`: `front_run` runs the guest on the calling thread; the window goes from the launcher straight to the replay (`nxhost_set_window`). There is no progress screen. Text input: see [The software keyboard](#the-software-keyboard).
- **Audio and movies.** Implemented ([Audio, video and input on Android](#audio-video-and-input-on-android)); the microphone and AV1 are absent, as on every platform.
- **Memory aliases.** `plat_mem_alias` has no writable alias of guest pages, so cheats write data only and the probe write watch unprotects the page instead. NVN virtual pools are kept in the page table only.
- **Diagnostics.** The power line, the frame-step CPU-share reading and the present thread's time-constraint policy are off.
- **Input, motion and haptics.** Implemented in the host. The launcher forwards gamepads, touch, motion and the lifecycle, and draws the on-screen controls (`TouchControls.java`: its own layout in dp through `nxhost_touch_controls_set`, the host's hit tests and state; shown without a gamepad, a top-centre button to hide them, held for opacity / size / vibration). It installs no rumble callback.

## The software keyboard

The keyboard applet (`hle_swkbd.c`: `ShowKeyboard`, and the inline keyboard's `Appear`) asks the front end's `front_text_input`. On Android that is `android_text_input` (`dm/host/android/input_android.c`), which calls the launcher's callback (`nxhost_text_input_callback`, `input_android.h`) and blocks the guest thread that opened the keyboard until it returns, as on the Mac and iOS. Without a callback it returns the no-window answer (cancel).

- **Host.** While the box is up, every pad and touch the game saw held is let go (the box takes the window's focus, so the game screen never gets the key-up or touch-up of what opened it) and `nxhost_input_key` / `_axes` / `nxhost_input_touch` are dropped, so nothing typed or pressed in the box reaches the game. The guest clock is not frozen for `ShowKeyboard` (the game's other threads run on, as on the Switch; `hle_swkbd.c` says why); the inline keyboard freezes it around its own call. The minimum length (`ShowKeyboardArg` +0x3b0, `g_swkbd_min_chars`) goes to the callback; the host cuts the text to the maximum in UTF-16 units.
- **Shim.** `nxlaunch.c` installs the callback at `Native.start`. The guest thread does not enter the VM: a thread of the shim, attached for the call and detached after it, calls `TextInput.ask(byte[] title, byte[] sub, byte[] guide, byte[] initial, int max, int min)` while the guest thread waits in `pthread_join`. Strings cross as UTF-8 byte arrays (`NewStringUTF` takes modified UTF-8). `null` back is a cancel.
- **Launcher.** `TextInput` shows an `AlertDialog` on the UI thread over the game's screen: the header as its title ("Enter text" when empty), the sub and guide texts as its message (and the guide as the field's hint), the initial text in a single-line field limited to the maximum length, the soft keyboard up (no full-screen extract editor). OK, Done on the soft keyboard or Enter on a physical keyboard confirm it, once the text has the minimum length (OK stays disabled until then, as the applet's). Cancel, Back or B on a controller cancel it, as the console's keyboard always allows: the game decides what a cancel means (an optional box closes its menu, a box the game needs is shown again). A tap outside does nothing, so a stray touch never cancels. Before the box, the host answered every request with an instant cancel, so a game that needs the text asked again forever (a soft-lock). A gamepad moves between the field and the buttons (A presses the focused one). A screen recreated while the box is up (`GameActivity.onDestroy` without finishing, then `onCreate`) shows it again with the text typed so far; a screen closed for good (the game stopped) answers cancel with a `[RED] keyboard: …` line, so the host never waits for a box nobody can see. The host logs each answer (`swkbd: title … → OK '…'`).

## Tests

- **Host unit tests:** `make -C dm/host/tests GAME=010015100B514000 run` passes. Two tests follow the new names: `t_symbols` fakes `plat_table` instead of `getsectiondata`, and `t_audio_spsc` uses `plat_sem_*`.
- **CPU-only graphics tests:** the CI list in `.github/workflows/tests.yml` passes (`t_perf_order --cpu`, `t_gfx_suspend`, …).
- **Python tool tests:** with Capstone 5.0.7 and lz4 in the environment of [README.md](README.md) (Python 3.12), `DM_GAME=010015100B514000 python -m unittest discover -s dm/tools/tests` runs 867 tests: no failures, and the 73 `No module named 'capstone'` errors are gone. The 6 errors left are `No module named 'compression'` (Python 3.14's `compression.zstd`, in `test_size_prefixed_frames` and `test_bea_members`), which a Python 3.14 environment does not have. `test_nro_header` and `test_rewrite_parallel` pass.
- **Rewriter byte identity:** on Donkey Kong Bananza 458752, `patch_nso.py` with and without `--target apple` gives byte-identical files (`patched.bin` and every table) for both `sdk` and `main`. With `--target android` the fixups gain `# TARGET_ANDROID 1` and the patch area has no `mrs tpidrro_el0` left: each of the apple output's `mrs xT, tpidrro_el0` reads (17,181 in sdk, 29,288 in main, the rewriter's `slot_refs`) is `mrs xT, tpidr_el0; ldr xT, [xT, #0x10]` followed by the same `adrp` / `ldr` / `add` tail with a key page index 0–3, and the text outside the patch area reads no system register.
- **Platform layer on Android:** a test linked with `plat_android.c` (not in the repository) passes on an API 36 arm64 emulator (kernel 6.12, 4K pages): the 9.75 GiB guest block at `0x4000000000`, a second block at the next 64 GiB boundary, 1 GiB mapped RW inside it, the slots read through `[[tpidr_el0 + 0x10] + entry]` as the rewritten code reads them, a new thread's slots read NULL without allocating, a write fault handled on the alternate stack with the word 16 bytes below `sp` intact, suspend / registers / resume of a running thread, and `process_vm_readv` failing on reserved memory instead of faulting.
- **Launcher:** on the same emulator with a synthetic pack (a made-up `main.npdm` and an empty module, no game files), the launcher added the game from the adb intent, started the game process, logged to the file and to logcat, and the host's abort on the missing fixups went through `crash.c` to debuggerd's tombstone while the launcher kept running.
- **Merged APK on the emulator:** the API 36 emulator (SwiftShader Vulkan 1.3, no `VK_EXT_map_memory_placed`) ran the launcher's Graphics self-test: device, window, a 2400x1080 FIFO swapchain, the red-to-magenta cycle with the white square in a 16:9 letterbox for 600 frames, then FAIL with code 2, since without placed maps the GPU cannot see the guest pool and both texel read-backs read 0. That is the expected emulator result; it checks the UI, the log and the present path.
- **Shader library and binding walker on the Mac:** `make -C dm/host/tests build/t_android_shlib && DM_VK_SHLIB=<work folder>/android-shaders/lib/v-<V>/dmsh_vk.lib dm/host/tests/build/t_android_shlib` checks the reader and walker on a library made in the test (header refusals, lookups, a raw and a zstd module, the c0 / c2 / push fills, unaligned storage buffers refused), then the real DKB library: all 22,852 entries looked up and walked with every slot bound (none refused, every binding in its stage's range), a sample of modules decompressed (`DM_VK_SHLIB_ALL=1`: all 22,852, 1.75 GB of SPIR-V, 2.6 s). Without `DM_VK_SHLIB` that part is skipped.
- **Draws on the emulator:** the extended graphics self-test on an API 35 arm64 emulator (gfxstream over SwiftShader: no placed maps, push descriptors, vertex input dynamic state or null descriptors, so the fallbacks ran) with the Khronos validation layer: no validation errors; the clear, square and event read-backs, the triangle's centre texel (`ffff00ff`) and the dispatch's storage words are right, and the screen shows the turning checkered triangle with the alpha-tested cells cut out; it ends FAIL (1) for the missing placed maps only. A run of `DM_VK_SHLIB_CHECK` with DKB's library there did not finish its first compute pipeline in ten minutes (the emulator compiles on the host's SwiftShader): the library check is for the phone. Two fixes came out of it: the pool tables now leave room under the update-after-bind limits for set 0, and pipelines use one static-count viewport (that emulator drew nothing with viewport-with-count dynamic state).
- **Game on the emulator:** Donkey Kong Bananza 458752 with its pack on an API 36.1 arm64 emulator (no placed maps): all eight stages of TESTING.md, the title menu's frame loop, and with scripted presses the first level's opening, 11,700 frames in 5.5 minutes without a crash ([TESTING.md, Emulator](TESTING.md#emulator)).
- **Phone:** not run yet ([TESTING.md](TESTING.md)).

## The NVN → Vulkan replay

[`dm/gfx/vk_android`](../dm/gfx/vk_android) stands in for `nvn_mtl.m` behind the same `mtl_*` interface (`nvn_mtl.h`, `nvn_obj.h`), so `hle_nvn.c` records the game's commands into its own command memory as before and the replay walks those records at submit. It is C, it loads every Vulkan entry point through `vkGetInstanceProcAddr` from a `dlopen`ed `libvulkan.so`, and it uses the Vulkan headers of `dm/third_party` (VK_HEADER_VERSION 363; the NDK's are 1.3.275 and lack `VK_EXT_map_memory_placed`).

| File | What |
|---|---|
| `vk_android.h` | the launcher's interface: `nxhost_set_window`, `nxhost_vk_selftest` |
| `vka.h` | the internal interface between the files below |
| `vka_dev.c` | instance, device (Vulkan 1.3, every supported feature but robustness), the one graphics + compute queue, the timeline semaphore that fences submissions, the completion thread, scratch memory |
| `vka_mem.c` | guest RAM as placed Vulkan memory, the pool table, `mtl_addr`, the `hle_svc.c` hooks |
| `vka_tex.c` | NVN formats → `VkFormat`, images by texture serial, uploads from guest memory (`nvn_tex.c` layout, `tex/nvn_deswizzle.c`), the update-after-bind texture / sampler / image tables |
| `vka_exec.c` | `mtl_sub_begin` / `mtl_exec` / `mtl_sub_commit`: the record walk, clears, copies, events, counters, fences, per-queue state |
| `vka_shlib.c`, `vka_bind.c` | the shader library reader and the binding walker (what each binding record is filled from, the driver segments): plain C, unit-tested on the Mac (`t_android_shlib`) |
| `vka_shader.c` | the game's library, its modules, programs resolved per stage |
| `vka_pso.c` | set-0 layouts, graphics and compute pipelines, the pipeline cache file |
| `vka_draw.c` | the draw state, the render pass, set 0 per draw, dynamic state, draws and dispatches |
| `vka_present.c` | the window, the FIFO swapchain, the present thread, display timing |
| `vka_selftest.c`, `selftest/` | `nxhost_vk_selftest`, and its three shaders (GLSL for the descriptor ABI, compiled into `vka_selftest_spv.h` by `selftest/spv.py`) |

**Guest memory.** Guest RAM (heap, alias region, module data) is anonymous memory, committed when touched, as on the Mac. Only NVN memory pools, the one thing the GPU reads and writes, are Vulkan memory: when the game initializes a pool (`mtl_pool_add`), its pages become one `VkDeviceMemory` of the DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT | HOST_CACHED type mapped with `VK_MEMORY_MAP_PLACED_BIT_EXT` at the pool's own address, holding the bytes the pages held; when it finalizes the pool (`mtl_pool_del`), anonymous memory with the same bytes and protection takes its place. Each placed range is a segment with one `VkBuffer` over it: `mtl_addr`, `vka_addr` and `vka_addr_run` answer (buffer, offset, device address) within one segment, `vka_mem_chunks` lists the segments for raw global memory's table (up to 4,095), and the device address differs from the CPU address. A pool is normally one segment; the parts of a pool another pool's segment already covers are shared (counted), a pool the driver cannot allocate in one piece is placed in pieces of `maxMemoryAllocationSize`, and when the buffer needs more bytes than the range (no `memoryMapRangePlaced`) the segment takes the following bytes too. Aliased virtual pools (one mapping of a whole physical pool) read the physical pool's segment; sparse virtual pools still have no GPU side: their sampled textures are uploaded through the page table ([Sparse pool textures on the Vulkan replay read through the page table](../docs/reference/nvn-objects.md#sparse-pool-textures-on-the-vulkan-replay-read-through-the-page-table)), as images of only the levels where pages are mapped, each level and layer uploaded when its pages change ([Sparse texture arrays made at their full size](../docs/reference/nvn-objects.md#sparse-texture-arrays-made-at-their-full-size)).

**The swap.** A pool's pages may already hold data (games fill storage before they make the pool), and other threads may touch them, so neither copy may lose a write or show zeros. Placing: the range is made read-only, the new memory is mapped where the driver likes, filled from the range and unmapped, then mapped placed over the range in one `mmap` (readers see the old pages or the new ones, both with the bytes), then the old protection is put back (`/proc/self/maps` gives it per page run). Giving back: the bytes go to fresh anonymous pages elsewhere (all-zero pages are skipped, so they stay uncommitted), the GPU is waited for (the last fence reserved, at most 2 s), the Vulkan memory is unmapped and freed, and the anonymous pages are moved over the range with `mremap`. A guest thread that writes during a swap faults and waits in a fault hook (`dm_fault_hook_register`), the host's own writes wait in the pre-write hook; a fault taken during a swap whose handler runs just after it is retried for 50 ms. The pool is added before `nvn_dirty_add_range` and removed after `nvn_dirty_remove_range`, so the dirty tracker's write protection is never on a range being swapped. `vka_mem_release` (the host giving guest memory back) gives back any segment still in the range first, with a `[RED]` line, since that is a pool the game did not finalize.

The free comes before the anonymous pages: the Adreno 840 driver's `vkFreeMemory` takes the placed range down even after an unmap with `VK_MEMORY_UNMAP_RESERVE_BIT_EXT`. Freeing it once the GPU was past it (`vka_after`), after the anonymous pages had moved in, unmapped the game's heap there, and Donkey Kong Bananza faulted at `main+0x36edbc` on a heap address after its accessibility screen, where it finalizes 16 pools.

The earlier design placed every opened page of the heap and alias regions in 1 GiB chunks at SetHeapSize. Driver memory is committed when it is allocated, so Donkey Kong Bananza's 8.94 GB heap became 9 GB of resident memory at boot and the low-memory killer ended the game at 9.2 GB RSS 25 to 100 s in; a 144 MiB pool across a chunk boundary had no GPU side. With pools only: 1.48 GiB placed at the title, 2.51 GiB in the first level, 6.3 GB RSS (Gfx dev 3.7 GB, the rest the heap the game touched), steady for more than 6 minutes.

**Textures.** An image lives in device memory of its own, not over the pool: Vulkan's optimal tiling is not block-linear. Content from storage follows `nvn_mtl.m`'s rules (`storage_mode` in `vka_tex.c`): a texture with a packaged layout (BNTX assets the game loads into its pools), one the CPU wrote (`mtl_tex_cpu_written`: packaged data, WriteTexels), a linear texture in a pool the CPU can write (Metal makes those on the pool buffer itself) and a block-compressed texture whose storage holds data at its first use are deswizzled from the storage into scratch memory and copied in, then *tracked*: their pages are write-protected (`nvn_dirty.c`, the same tracker as on Metal) and the next submission after a CPU write re-uploads them. Storage in a CPU_NO_ACCESS or CPU_UNCACHED pool, or flushed before the first use, fills a texture once. Every other texture starts cleared to zero. A texture the GPU writes (render target, copy or clear destination, storage image) is GPU content from then on and is never re-uploaded. `DM_VK_TEX_TRACK=0` uploads once with no write protection. Before this, uncompressed packaged textures read zeros: Donkey Kong Bananza's menus had panels and no text. 1D textures are one-row 2D images, as on Metal (the translator declares no 1D image). 3D textures that can be colour attachments are `2D_ARRAY_COMPATIBLE`: render target views, clears and ClearTexture name slices through 2D-array views. Images are found by serial; a texture over the same storage and shape as a live one shares its image, so a clear through one texture object and a present through another meet. 8-bit sRGB formats are stored as their UNORM twin with `MUTABLE_FORMAT`: copies and the present move raw bytes, as the Metal present does (`present_raw_src`), clears encode the colour, views sample with the sRGB decode. Every image stays in `VK_IMAGE_LAYOUT_GENERAL` after its first use, and every replayed op is followed by a full memory barrier: correct, not fast.

**Bindless.** One update-after-bind descriptor set: binding 0 the texture pool (sampled images, 2^20 slots on the Adreno 840), 1 the sampler pool (4096), 2 storage images (2^16). `mtl_reg_tex` / `mtl_reg_image` / `mtl_reg_samp` record the registration; the next submission makes the image and view and writes the slot. Nothing binds the set yet: it is there for the shader side.

**Replay.** One command buffer per submission from a ring of 8. Covered: END / JUMP / CALL, SET_RT, SCISSOR, COPY_STRIDE, CLEAR_COLOR and CLEAR_DS (`vkCmdClearColorImage` / `vkCmdClearDepthStencilImage`, or a dynamic-rendering pass with `vkCmdClearAttachments` for a scissored part), CLEAR_TEX, COPY_B2T / T2B (straight from the chunk buffer, through scratch memory when an offset or stride does not fit Vulkan's rules), COPY_T2T (copy, blit for scaled / mirrored / format-changing copies, resolve for Downsample), COPY_B2B, UPDATE_UB, CLEAR_BUF, BARRIER, SIGNAL_EVENT, REPORT_COUNTER, FENCE_SYNC and RENDER_COND. Events and counter reports are written by the completion thread once the timeline passes the submission, before the fence reads as done, as `nvn_mtl.m`'s `post_complete` does; a samples-passed report reads 1 so occlusion culling keeps everything until draws count. State records go to the queue's draw state (`vka_dstate`, kept per queue across submissions as `nvn_mtl.m`'s S). DRAW and DRAW_IDX open a dynamic-rendering pass over the bound targets if none is open (attachments in GENERAL, load and store), which stays open across draws and state and closes, with a full barrier, at any other record; DISPATCH runs outside it. Images a draw samples for the first time are made (and cleared or uploaded) before the pass opens. Each draw: the program's shaders per stage, a pipeline keyed on the state EDS3 would cover, set 0 from the binding records (push descriptors when they fit), EDS1 / EDS2 / vertex-input dynamic state, the viewport as Metal's with a negative height (the translator writes Metal's y-up clip space), then the draw: direct, indexed (8-bit indices widened when the device lacks them), indirect, quads and line loops expanded into indices on the CPU, indirect-count multi-draws gated by the count the CPU reads. [SHADERS.md](SHADERS.md#what-the-replay-does) has the shader side and what is not done. DRAW_TEX is logged once and skipped. Draws and dispatches run only with placed guest memory: without it (the emulator) the GPU sees no pool and the game's pipeline compiles take minutes there, so they are skipped with one `[RED]` line unless `DM_VK_DRAW=1`; `DM_VK_DRAW=0` turns them off on the phone. Events, counter reports (samples passed read 1) and fences signal whatever happens to the draws.

**Present.** `mtl_present` enqueues the window texture and returns a ticket; the present thread acquires a FIFO swapchain image (R8G8B8A8_UNORM when offered, 3 images, preTransform = the surface's currentTransform when that is a rotation, with the extent in the panel's orientation), copies the crop into the aspect-fitted rectangle (flipped when the window origin is lower left, as on Metal) and presents. Unrotated, the copy is a blit; pre-rotated, it is a draw with dynamic rendering (`vka_present_rot.h`: a full-viewport triangle whose texture coordinates carry the turn, the viewport placed in the turned image), so the compositor does not rotate each frame. `VK_SUBOPTIMAL_KHR` with a new surface rotation (the phone turned over, ROTATE_90 ↔ ROTATE_270) or a new size recreates the swapchain; `DM_VK_PRETRANSFORM=0` leaves rotation to the compositor. The window is told the game's frame rate (`ANativeWindow_setFrameRate`, fixed source; `DM_VK_FRAME_RATE=0` skips it), and the refresh period is read again over the next few hundred presents, since the panel may switch to it. With `VK_GOOGLE_display_timing` each image asks for at least its present period on screen (`desiredPresentTime`; a 30 fps game on a 60 or 120 Hz panel), and the past presentation timings give `mtl_presented_time`. `mtl_display_hz` reads the refresh cycle. There is no display link: `hle_nvn.c` paces its acquires by its own clock, as on a Mac without one.

**The launcher's interface** (`dm/gfx/vk_android/vk_android.h`, exported from `libnxhost.so`):

- `void nxhost_set_window(ANativeWindow *w)`: from any thread, before or after `nxhost_main`. Call it from `surfaceCreated` / `surfaceChanged` with `ANativeWindow_fromSurface`, and with NULL from `surfaceDestroyed`: the NULL call returns once the swapchain and surface are gone (2 s at most). The replay takes its own reference, so the caller may release its own after the call. Without a window, presents are counted and dropped. The first call also brings up the Vulkan device.
- `int nxhost_vk_selftest(ANativeWindow *w, int frames)`: no game needed. Sets the window (NULL keeps the current one), places 64 MiB of guest memory, makes a pool and a 1280x720 RGBA8 window texture in it, and replays hand-written NVN records: a full clear to a colour, a scissored white 128x128 square at (64, 64), a draw of a textured triangle (a 64x64 checkerboard of opaque white and transparent cells registered in the texture pool with a sampler, times a magenta tint from the fragment shader's c1, the transparent cells dropped by the alpha test (GREATER 0.5 through the specialization constant and the support buffer's reference), turning with a rotation from a uniform buffer), a compute dispatch that writes a storage buffer from c0's dispatch size, three texel read-backs into the pool, an event. The shaders are its own (`selftest/*.vert`, `.frag`, `.comp`: GLSL for the descriptor ABI), in a shader library it builds in memory, so the draw goes through the same reader, walker, layouts and pipelines as a game's. It checks the fence, the texels (the clear, the square, the triangle's centre, a dropped cell), the dispatch's output and the event, then presents for `frames` frames with the colour going round the hues every 6 s. Without placed maps (an emulator) it takes a mapped Vulkan allocation where the driver puts it, so everything but the placed check still runs. With `DM_VK_SHLIB=<dmsh_vk.lib>` in its environment it then makes every module and compute pipeline of that library (`vka_shlib_check_all`). Returns 0 when every check passed. Call it on a thread of its own (it blocks for `frames` / 60 s).
- `NXHOST_VK_SELFTEST=<frames>` in the environment at the first `nxhost_set_window` runs the selftest on a thread of its own, with no JNI change.

Switches: `DM_VK_PLACED=0` (memory pools stay anonymous memory: the GPU sees none), `DM_VK_VALIDATE=1` (the Khronos validation layer, when it is in the APK).

**First phone test.** The launcher's **Graphics self-test** runs `nxhost_vk_selftest(window, 600)` ([TESTING.md](TESTING.md#test-2-graphics-self-test)). Expected in logcat (tag of the stderr redirect):

1. `[vk] device Adreno (TM) 840, Vulkan 1.4.295 …; placed maps yes …, update-after-bind yes, display timing yes`;
2. `[vk] window 0x…`, then `[vk] swapchain WxH, 3 images, format 37, FIFO …, refresh 120.00 Hz, display timing` (format 37 is R8G8B8A8_UNORM);
3. `[vk] selftest chunk 0x…+0x4000000 placed (device address 0x…)`, `[vk] memory pool …`, `[vk] bindless tables: 1048576 textures, 4096 samplers, 65536 images`;
4. `[vk] draw path: push descriptors yes (…), vertex input dynamic state yes, null descriptors yes, …` and `[vk] selftest: shaders made; …`;
5. `[vk] selftest: fence 1 done; texel (1200,650) ff0000ff (want ff0000ff), texel (100,100) ffffffff (want ffffffff), event 1 (want 1)`, `[vk] selftest: draw: 1 drawn, 0 skipped; triangle centre texel (640,360) ffff00ff (want ffff00ff …)`, `[vk] selftest: dispatch: 1 run; storage words 5 and 255: 4005, 4255 (want 4005, 4255)`;
6. on screen: the window filled red with a white square near one corner (the bottom left: the origin is lower left) and in the middle a triangle checkered magenta and black, turning once every 6 s; the background cycling red → yellow → green → cyan → blue → magenta every 6 s, for 10 s; letterboxed bars are black when the panel is not 16:9;
7. `[vk] selftest: 600 frames presented to the window; 1 graphics + 1 compute pipelines (0 failed), …; passed`.

Then a game: `[vk] memory pool … placed at device address …` lines for the first pools, `[vk] … GiB of memory pools is placed Vulkan memory (… pools)` at each 256 MiB step, `[vk] shader library …/shaders/v-<V>/dmsh_vk.lib: 22852 shaders …` at the first draw, then `[RED]` lines once each for what a draw meets that is not done yet (refused shaders by key, unaligned storage buffers, variants) and the game's frames. With `DM_VK_SHLIB_CHECK=1` in `env.txt` the run first makes every module and compute pipeline of the library (`[vk] shader library check: …`), which takes a while once and fills the pipeline cache. The things to look for first: whether `mprotect` works on placed memory (`[RED] vk: mprotect on placed Vulkan memory fails` otherwise), how long placing the heap takes, and whether the system kills the process for memory while it does.

## Next steps: NVN → Vulkan replay

The memory design follows the probe. Guest RAM is built from Vulkan allocations placed at guest addresses: `VK_EXT_map_memory_placed` with a `DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT | HOST_CACHED` type, chunks of at most 1 GiB (`maxMemoryAllocationSize`; 3 GiB works, 4 GiB fails) placed back to back inside the reserved guest block. CPU and GPU then see the same bytes with no copy and no flush. An NVN memory pool is a sub-range of that RAM: a pool's buffer and texture addresses become a `VkBuffer` bound at an offset of the chunk that holds it, plus a buffer device address. The device address differs from the CPU address, so a pool-address → (chunk, offset, BDA) table replaces Metal's `mtl_addr`, as nxnative already does on Metal. A virtual pool is a second placed mapping of the same chunks, or sparse binding (`sparseBinding` and `residencyBuffer` / `residencyImage2D` are present). There is no `VK_EXT_memory_budget`, so the host counts its own allocations. Donkey Kong Bananza asks for a 9 GiB heap on a 14.66 GiB phone: start with its heap in placed chunks, and fall back to plain anonymous memory with upload copies for the parts the GPU never reads.

| # | Piece | Files to write | Effort |
|---|---|---|---|
| 1 | NVN memory pools as placed Vulkan memory (guest RAM anonymous), the segment table and address → BDA translation | **done**, run on the phone: `dm/gfx/vk_android/vka_mem.c` (pools only since the 9 GB heap was killed for memory). Left: sparse virtual pools | 1 week |
| 2 | The device, queue, swapchain on an `ANativeWindow`, and the frame loop, with the present / acquire / fence API `hle_nvn.c` calls today (`mtl_present`, `mtl_wait`, `mtl_sub_begin` / `commit`, `mtl_link_*`) | **done** (built, not run): `vka_dev.c`, `vka_present.c`; the launcher passes the surface with `nxhost_set_window` | 1–2 weeks |
| 3 | Command replay: the recorded NVN command stream (`nvn_rec.h` ops, the same 54 ops `nvn_capture.c` walks) to Vulkan command buffers, with dynamic rendering, EDS1/2 and vertex-input dynamic state; pipelines keyed on the state EDS3 would have covered | clears, copies, barriers, events, counters, fences **done** in `vka_exec.c`; draws and dispatches with their state **done** (`vka_draw.c`, `vka_pso.c`; checked on the emulator, not on the phone); left: layout tracking instead of GENERAL plus a barrier per op, render passes merged across clears, transform feedback, DrawTexture | 4–6 weeks |
| 4 | Textures and samplers: NVN pools to `VkImage`, a 2^20-entry update-after-bind descriptor set (or a descriptor buffer) for the texture and sampler pools, BC / ASTC sampled directly | images, uploads, views, samplers and the tables **done** in `vka_tex.c` (images in their own memory, not over the pool); left: depth uploads, texture buffers. Images under 4 MiB share 64 MiB blocks (`vka_imgmem.c`, exact 256-byte units, first fit; a block that cannot be allocated leaves its image dedicated; Donkey Kong Bananza: 7,194 of 7,224 images in 9 blocks, RSS and fps as with one allocation each; `DM_VK_IMGMEM=0`: one allocation per image as before) | 2–3 weeks |
| 5 | Shaders: the chain's SPIR-V before SPIRV-Cross shipped in the pack, with SPIR-V reflection. The offline half is done: `dm/gfx/tools/vk_android_build.py --target vulkan` writes one library per game with the descriptor ABI of `dm/gfx/nvn/dmsh_abi_vk.h` ([SHADERS.md](SHADERS.md)). The loader and binder in the replay are **done** (`vka_shlib.c`, `vka_bind.c`, `vka_shader.c`). Left: the translator answers listed under [open problems](SHADERS.md#open-problems) and the variants. Real geometry, tessellation and transform-feedback stages replace the Metal mesh lowering; no `shaderFloat64`, so the soft-float path stays | `dm/gfx/vk_android/` (loader, binder): **done** | 2–3 weeks |
| 6 | Capture replay on the phone: frames captured on the phone (`DM_NVN_CAPTURE`, `vka_capture.c`), replayed there without the game by `nxreplay` (`vka_replay.c`, run from adb shell) | **done** ([TESTING.md](TESTING.md#frame-capture-and-replay-on-the-phone)); open: the first replay of a capture differs from the live frame, later ones are close | 1 week, early: it tests 3–5 without a game |
| 7 | AAudio output, `AMediaCodec` H.264 and AAC, JNI input | **done** (built, not run): `hle_audio.c`, `dm/host/android`, `movie_avc_android.c`; the launcher's Java forwards through `nxlaunch.c`. Left: on-screen controls drawn by the launcher, the rumble callback | 2 weeks |
| 8 | Memory aliases from memfd-backed modules (cheats' code writes, probe write watch) | `loader.c` (memfd mapping), `plat_mem_alias` | 3 days |

Do them in the order 1, 2, 6, then 3–5 together on the capture replay, then 7 and 8. A first logged frame (a cleared swapchain image after the game's first present) needs 1, 2 and the clear / present ops of 3. That is about 3–4 weeks after the first test APK below.

## Before a first test APK

Target: the launcher APK, the folder picker, and one pack loaded up to the game's first NVN call and its first logged frame (`[nvn] frame 0`, or the first present reaching the stub).

Built: the launcher, the folder access, the pack format and packer, and the start-up fixes below that could be made without the phone (the guest block's address, slot reads, signal chaining, heap pointer tagging). The host start-up on the phone is the user's test ([TESTING.md](TESTING.md)).

The design: one launcher APK, installed once, holds no game code. Packs are built on the Mac from the user's dumps and stored in the work folder beside the dumps, `<dump folder>/<id>-work` (AGENTS.md). A pack holds the NSOs rewritten with `--target android`, their tables and the translated shaders. The RomFS is not in the pack: the app reads RomFS and ExeFS straight from the game folder the user picks on the phone.

| Piece | What | Effort |
|---|---|---|
| Launcher APK | `android/app`, Kotlin, an Activity with a `SurfaceView`. `System.loadLibrary("nxhost")`, then `nxhost_main` on its own thread with an 8 MB stack: guest threads need deep stacks, and `main.c` already runs the guest on the calling thread. Logs go to logcat and `files/log.txt` through a `stderr` redirect | 2–3 days |
| Folder access | The Storage Access Framework tree picker for the game folder and the pack folder, plus All files access (`MANAGE_EXTERNAL_STORAGE`), as Eden SX2A does. The host opens POSIX paths (`DM_ROMFS` may be a folder, `hle_fs.c`), so the picked tree is turned into its `/storage/...` path. `/storage/emulated` is case-insensitive on recent Android, as APFS is: the host's case-collision names (`.dm-case-N`) keep working | 2 days |
| Pack format and packer | `scripts/pack_android.py`: from a dump folder (`scripts/pack_dump.py` already reads `base/`, `updates/<version>/`, `dlc/`), rewrite the newest ExeFS with `--target android` into `<id>-work/android-pack/` (the patched modules, `*.fixups.tsv`, `gates.tsv`, the other rewriter tables, the profile's `game.tsv`) and leave the RomFS out. Shaders come later (step 5). Capstone 5 is needed in a venv on the dump drive | 2 days |
| Host start-up on a phone | `nxhost_main(<pack folder>, …)` with `DM_ROMFS=<game folder>/updates/<version>/romfs`, `DM_SAVE` and `DM_TEMP` in app storage. The first run will find what this build could not check: the slot block in `TLS_SLOT_APP`, process_vm_readv under SELinux, libsigchain and the fault handlers, and the guest block reservation | 3–5 days of bring-up |
| NVN up to the first call | `nvnBootstrapLoader` and the device / queue / window functions reach `hle_nvn.c` with the stubs answering: the first NVN calls are logged, and the frame loop presents to `mtl_present` (a stub) | in the line above |

That is about two weeks to an APK that boots the CPU side of a game and logs its NVN calls with no picture. The APK holds no game code. The user sideloads it and points it at the game and pack folders.

Which game: three dumps are in the dump folder on this Mac.

- **Donkey Kong Bananza** (`DonkeyKongBananza`, base + update 458752 + DLC): has a profile, `dm/games/040074A01BF12000`, and plays on the Mac from exactly this dump layout through `pack_dump.py`. Start with it.
- **Nintendo Switch 2 Welcome Tour:** no profile.
- **Pokémon Pokopia** (`0400A3A01EE4C000`): no profile.

Donkey Kong Bananza's risks on the phone are its 9 GiB heap (`mem_reserve 0x270000000`) on 14.66 GiB of RAM, NVN2 with Ampere shaders, and agl's tensor-core upscaler (HMMA in 16×8×16 tiles, where the phone's cooperative matrices are 64-wide; this can wait for after the first frame). The CPU path is already proven on the Mac, so a failure on the phone points at the port. Welcome Tour is the smaller fallback if the 9 GiB heap does not fit: it first needs a survey and a provisional profile from `build_ipa_from_nsp.py` (AGENTS.md).

## Audio, video and input on Android

This section supersedes the AudioToolbox, VideoToolbox and input rows of the dependency table and the audio, movie and input items of "Stubs in this build". Those three are now Android implementations. They have been built and linked, and their pure parts are unit-tested on the Mac. None of it has run on a phone yet.

| Piece | Where | What it does |
|---|---|---|
| Audio output | `hle_audio.c`, `#else` of `PLAT_APPLE` | One AAudio stream per AudioOut, in place of the AudioUnit. The stream is low latency, shared, PCM16 at 48 kHz, with the output's device channels; six channels are downmixed by `pull_device`, as on Apple. Its data callback pulls exactly the frames AAudio asks for, keeps the watchdog's statistics and honours `DM_AUDIO_MUTE`. The buffer is two bursts and at least 5 ms (`DM_DBG_AU_FRAMES=N` overrides it). If the device will not open at 48 kHz, the stream opens at the device's rate and `av_android.h`'s resampler converts. `au_stop` waits for `STOPPED`, so the silent sink never runs beside a callback. A disconnected stream (headphones, Bluetooth) stops calling back: the watchdog's silent sink takes over within 200 ms, and the retry opens a new stream on the current route. `DM_AUDIO` defaults to 1, as on iOS; `DM_AUDIO=0` selects the silent sink |
| AAC (movies) | `dm/host/android/aac_android.c`, wired into `hle_movie.c`'s `#else` | `AMediaCodec` `audio/mp4a-latm`, with csd-0 = the AudioSpecificConfig (built from the rate and channels when there is none, as on the Mac). Each packet gives at most one AAC frame of output, the size `GetOutputBufferSize` promises; the rest waits for the next packet. A decoder output layout that differs from the track's is channel-mapped (`ch_map`). Float output is converted |
| H.264 | `dm/host/plat/movie_avc_android.c` (replaces the stub, same `mavc_*` interface) | `AMediaCodec` `video/avc` with ByteBuffer output, configured with color-format 21 (NV12), csd-0 / csd-1 from SPS / PPS, and the size read from the SPS (`avc_sps_size`). Pictures are copied out through the output format's `stride`, `slice-height` and crop. Formats 19 / 20 (I420), 21 / 39 / flexible / Qualcomm 32m (NV12) and Qualcomm NV21 are read. RGBA mode converts with BT.601 or BT.709 from `color-standard` and `color-range`. MediaCodec reorders itself, so pictures leave as they come; `flush` sends end-of-stream and drains; `mavc_drop` flushes the codec. A new SPS / PPS makes a new decoder. Both the movie decoder and `nn::codec` (`hle_codec_video.c`) use it. VP8, VP9 and Vorbis stay in software. AV1 is not needed by the movie code (it has no AV1 type); `nn::codec`'s AV1 Initialize fails on every platform |
| Input | `dm/host/android/input_android.c` (+ pure `input_map.c`) | Gamepads: keycodes and axes mapped to `dm_pad_set_player`. Each Android device gets a player through `pad_slots.h`; face buttons map by position by default (`DM_PAD_LABELLED=1` maps by label); the stick dead zone is radial, 0.1 by default (`DM_PAD_DEADZONE`). On-screen controls are a data model (sticks, D-pad, buttons) whose state goes to `dm_input_set`, the touch gamepad's path as on iOS. Other touches inside the game's picture go to `dm_touch_set`, in the lowest free finger slot. Motion: ASensorManager accelerometer and gyroscope at 100 Hz on their own looper thread, turned into the Switch frame for the display rotation, go to `dm_motion_set_switch(0, …)`; phone shakes become `DM_BTN_SHAKE` (iOS's detector, `DM_SHAKE_G`). Rumble: `dm_haptics_init` with a sink that hands the snapshots to a rumble thread, which calls the launcher (optional) |
| JNI | `dm/host/android/jni_android.c` | `JNI_OnLoad` registers the natives below on `dev.nxnative.NxHost` (change `NXHOST_JNI_CLASS`) when the APK has that class. Each method is registered on its own, so a class that declares only some still gets those. The C functions of `input_android.h` are exported too. The nxnative launcher has no such class: `JNI_OnLoad` logs that nothing is bound and returns normally, and the launcher calls the C functions (below) |

### Launcher interface

The nxnative launcher (`android/app`) uses the C functions: its JNI library `nxlaunch.c` includes `input_android.h` and `vk_android.h` and declares each entry point weak, so the launcher and the host share one set of signatures and a change fails at compile time. `GameActivity` makes the calls listed under "What the Activity calls" through `Native` (all in the game's own process, `:game`, so they reach that process's host); on-screen controls are turned off and there is no rumble listener yet. Another launcher may use the JNI class instead.

The Kotlin side, as `JNI_OnLoad` expects it:

```kotlin
package dev.nxnative
object NxHost {
    init { System.loadLibrary("nxhost") }
    @JvmStatic external fun run(args: Array<String>): Int          // nxhost_main(argv[0] = "nxhost", args...); call it on a Thread with an 8 MB stack
    @JvmStatic external fun deviceAdded(device: Int, name: String?, labelled: Boolean): Int   // → player 0..7, -1 when full
    @JvmStatic external fun deviceRemoved(device: Int)
    @JvmStatic external fun devicePlayer(device: Int): Int
    @JvmStatic external fun deviceFirst(device: Int, name: String?): Int   // → 0: the device is player 1 now, -1 when full
    @JvmStatic external fun key(device: Int, keyCode: Int, down: Boolean): Boolean           // true: a pad button, consume the event
    @JvmStatic external fun axes(device: Int, axes: IntArray, values: FloatArray): Boolean
    @JvmStatic external fun deadzone(dz: Float)
    @JvmStatic external fun touch(action: Int, pointerId: Int, x: Float, y: Float)            // x, y normalized to the view
    @JvmStatic external fun touchLayout(viewW: Float, viewH: Float, picX: Float, picY: Float, picW: Float, picH: Float)
    @JvmStatic external fun touchControlsEnable(on: Boolean)
    @JvmStatic external fun touchControls(): FloatArray                                       // 6 per control: kind (0 button, 1 stick, 2 D-pad), buttons, stick, x, y, r
    @JvmStatic external fun touchControlLabels(): Array<String>
    @JvmStatic external fun touchControlsSet(controls: FloatArray?): Int                      // the launcher's own layout, 6 per control as touchControls; null: the default
    @JvmStatic external fun touchControlsState(pressed: LongArray, stick: FloatArray)         // pressed[i]: buttons held by control i; stick: lx, ly, rx, ry (-1..1, y up)
    @JvmStatic external fun motionStart(): Boolean
    @JvmStatic external fun motionStop()
    @JvmStatic external fun motionRotation(rotation: Int)                                     // display.rotation
    @JvmStatic external fun setRumbleListener(listener: Any?)                                 // has fun onRumble(player: Int, intensity: Float, sharpness: Float)
    @JvmStatic external fun rumbleRoute(player: Int, available: Boolean)
    @JvmStatic external fun lifecycle(event: Int)                                             // 0 onPause, 1 onStop, 2 onStart, 3 onResume
    @JvmStatic external fun audioInterrupt(on: Boolean)
    @JvmStatic external fun audioRouteChanged()
    @JvmStatic external fun codecsLog()
}
```

What the Activity calls:

- **Keys.** In `dispatchKeyEvent(e)`, for `e.source` with `SOURCE_GAMEPAD` or `SOURCE_DPAD` from a non-virtual device, call `NxHost.key(e.deviceId, e.keyCode, e.action == ACTION_DOWN)` and return `true` when it returns `true`. `BACK`, `BUTTON_MODE` and keyboard keys are not taken; the launcher keeps them (for a menu, say).
- **Axes.** In `dispatchGenericMotionEvent(e)`, for `SOURCE_JOYSTICK` with `ACTION_MOVE`, pass the axes the device has: those of `AXIS_X, AXIS_Y, AXIS_Z, AXIS_RZ, AXIS_RX, AXIS_RY, AXIS_HAT_X, AXIS_HAT_Y, AXIS_LTRIGGER, AXIS_RTRIGGER, AXIS_GAS, AXIS_BRAKE` for which `e.device.getMotionRange(axis, e.source) != null`, with `e.getAxisValue(axis)`. Pass them in one `NxHost.axes(e.deviceId, axes, values)`; also send the historical samples first if wanted. When a batch has Z / RZ, the host takes the right stick from them and ignores RX / RY.
- **Devices.** Register an `InputManager.InputDeviceListener`. On `onInputDeviceAdded` (and for `inputDeviceIds` at start), for gamepads call `deviceAdded(id, device.name, false)`. On `onInputDeviceRemoved`, call `deviceRemoved(id)`. A device whose first event comes before `deviceAdded` is connected by that event. To put a controller on player 1 (a phone may report its own input devices as gamepads), call `deviceFirst(id, device.name)`: player 1's device takes the controller's old player.
- **Touch.** In the game `SurfaceView`'s `onTouchEvent(e)`, for `ACTION_DOWN` / `ACTION_POINTER_DOWN` / `ACTION_POINTER_UP` / `ACTION_UP`, call `touch(e.actionMasked, e.getPointerId(e.actionIndex), x / width, y / height)` for the action's pointer. For `ACTION_MOVE`, call it for every pointer `i`; for `ACTION_CANCEL`, call it once. Call `touchLayout(width, height, …)` on every layout change with the picture's rectangle in the view. Draw `touchControls()` over the picture (centre `x * width`, `y * height`, radius `r * height`), refreshed from `touchControlsState` while touched. Call `touchControlsEnable(false)` while a gamepad is in use.
- **Motion.** Call `motionStart()` in `onResume`, `motionStop()` in `onPause`, and `motionRotation(display.rotation)` at start and on every configuration change. The phone's gyroscope needs no permission at 100 Hz.
- **Rumble (optional).** Before `run`, call `setRumbleListener(obj)` with `fun onRumble(player: Int, intensity: Float, sharpness: Float)`, and `rumbleRoute(0, vibrator.hasVibrator())`. The callback runs on the host's rumble thread. Intensity 0 means `vibrator.cancel()`. Otherwise play a repeating waveform until the next call, for example `VibrationEffect.createWaveform(longArrayOf(0, 1000), intArrayOf(0, (intensity * 255).roundToInt().coerceIn(1, 255)), 0)`, and map sharpness to the frequency on API 31+ composition, if wanted. For a controller's own motors, call `rumbleRoute(player, true)` with `devicePlayer(id)` and vibrate `inputDevice.vibratorManager`.
- **Lifecycle.** Call `lifecycle(0)` in `onPause`, `lifecycle(1)` in `onStop` (audio streams stop, the silent sink keeps the game's pace, the guest clock and the graphics freeze), `lifecycle(2)` in `onStart`, and `lifecycle(3)` in `onResume`. With `AudioFocusRequest` (`USAGE_GAME`), loss and transient loss are `audioInterrupt(true)` and gain is `audioInterrupt(false)`. From an `AudioDeviceCallback`, a device added or removed is `audioRouteChanged()`, which reopens every stream on the new route; without it, a disconnected stream comes back through the watchdog in about 1.2 s.
- **Diagnostics.** `codecsLog()` logs which decoders exist for `video/avc`, `video/hevc`, VP8, VP9, `video/av01` and AAC. The full list on the phone: `MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos.filter { !it.isEncoder }` with each one's `name`, `supportedTypes` and `isHardwareAccelerated` (API 29). From the Mac, `adb shell dumpsys media.player` lists the codecs on many builds, and the XML lists are under `/vendor/etc/media_codecs*.xml`.

### Build and tests

- `libnxhost.so` builds with NDK 28.2 for API 33, `-DGAME=040074A01BF12000`, and links with `--no-undefined` against `aaudio`, `mediandk` and `android` (with the replay merged: 7.8 MB with debug info, 1.46 MB stripped). It exports `nxhost_main`, the `nxhost_*` launcher interface and `JNI_OnLoad`.
- Mac unit tests, also in `make -C dm/host/tests GAME=… run`:
  - `t_android_av` covers the resampler (equal-rate delay, 48 → 44.1 kHz frame accounting over uneven calls, 48 → 96 kHz interpolation), the channel map (in place, both directions), the SPS size of four libx264 SPSs (1080p crop, 720p, an odd 322×242 crop, interlaced 480), NV12 / I420 layouts with stride, slice and crop into NV12, and NV12 → RGBA (BT.601 / BT.709, limited / full).
  - `t_android_input` covers every key's mapping (sixteen buttons once each), the dead zone and diagonal, trigger hysteresis, the hat, the Z / RZ over RX / RY rule, the default on-screen layout (no overlaps), a launcher layout (`nxin_touch_set_layout`: held touches let go, stick clicks, the default back), sticks, the D-pad, finger slots, letterboxing, cancel, the sensor frame for the four rotations, and shakes.
- Apple paths: `hle_audio.c` and `hle_movie.c` preprocess to the same text as before for `arm64-apple-macos13` and `arm64-apple-ios26.0` and compile without warnings. The full host test suite passes on the Mac.

### Still stubs after this

- the graphics' draws and dispatches (the replay does clears, copies and presents) and the progress screen;
- the microphone (none on any platform);
- AV1 (none on any platform; `video/av01` through the same `mavc`-style wrapper is the way if a game needs it);
- memory aliases and the diagnostics listed above.

On-screen controls: the launcher's layout has LS / RS click buttons (L3 / R3); there is no per-control layout editor (only size and opacity), and they are a Pro Controller layout for every game.

### What to test on the phone

1. Audio:
   - The log line `audio #N AAudio stream: 48000 Hz, 2 channels, burst …, low latency, shared` appears for each output.
   - The watchdog's callback statistics show few underruns.
   - Unplug headphones mid-game. The log shows `the device has not called back`, then the stream reopens and sound continues on the speaker. Then try `audioRouteChanged()` for the same case.
   - Background and foreground: sound stops and comes back, and the game does not stall.
2. AAC and H.264 movies:
   - A game with MP4 movies (Super Mario Odyssey's hint videos, Monster Hunter Rise's AAC) logs `[movie] AVC: c2.qti.avc.decoder …` and `[movie] AVC output: … colour format 0x15 …`.
   - The pictures are upright and uncropped, the colours are right (the RGBA path too), loops and seeks do not freeze, and the last frame of each movie arrives.
   - The AAC line names the decoder. The audio stays in sync, and a mono track with parametric stereo is mapped.
   - If the output colour format is not readable, the `[RED]` line names it.
3. Gamepads:
   - A Bluetooth Xbox or DualSense pad: A / B / X / Y by position, both sticks (check the right stick and that the triggers do not move it), ZL / ZR from analog triggers, the D-pad hat, `+` / `-`.
   - A second pad is player 2.
   - A Switch Pro Controller on hid-nintendo: if A and B come out swapped, set `deviceAdded(…, labelled = true)` for vendor 0x057e.
4. Touch: the on-screen controls (several fingers at once, a stick held while pressing buttons, the hide button, the settings), that they hide when a pad connects and return when it goes, and a game that reads the touchscreen, touched at the corners of the picture, with letterboxing.
5. Motion: tilt in a gyro game in landscape both ways (ROTATION_90 / 270), and shake. With `DM_MOTION_LOG=1` the log shows acc (0, 0, -1) lying flat.
6. Rumble: the listener is called with intensity changes and with 0 at stop and on pause.
