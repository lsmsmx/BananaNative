# libadrenotools (vendored)

[libadrenotools](https://github.com/bylaws/libadrenotools) by Billy Laws (BSD 2-Clause, `LICENSE`), as Eden uses it
(`eden-emulator/libadrenotools` at `8ba23b42d7`), with [liblinkernsbypass](https://github.com/bylaws/liblinkernsbypass)
(`aa3975893d`, BSD 2-Clause, `lib/linkernsbypass/LICENSE`) in `lib/linkernsbypass/`.

Only the files the Android host needs to load a custom Vulkan driver (`ADRENOTOOLS_DRIVER_CUSTOM`) are here:
`src/driver.cpp`, `include/adrenotools/{driver,priv}.h`, the hook libraries' sources `src/hook/{hook_impl.cpp,hook_impl.h,hook_impl_params.h,main_hook.c}`
and the kgsl UAPI header `src/hook/kgsl.h` (Linux UAPI, `GPL-2.0 WITH Linux-syscall-note`, as its SPDX line says). Left out: the BCn enabler, the file-redirect and
GSL-allocation hooks, the tools. All unmodified but `src/hook/main_hook.c`, which exports one addition: a no-op `AIBinder_Class_setTransactionCodeToFunctionNameMap`,
the Android 16 (API 36) Binder NDK symbol that Qualcomm's 891.x drivers' helper library `vkb891.qti.hardware.hexlp-V2-ndk.so` imports; on Android 15 the system's
`libbinder_ndk.so` lacks it, the relocation failed and the driver fell back to the system driver. `libmain_hook.so` is loaded `RTLD_GLOBAL` into the driver's namespace,
so the stub satisfies the import (Forgejo PR #32).

`android/host/CMakeLists.txt` builds `libnxadreno.so` (driver.cpp + linkernsbypass) and the two hook libraries `libhook_impl.so` and `libmain_hook.so`. The host
(`dm/gfx/vk_android/vka_driver.c`) opens `libnxadreno.so` only when `DM_VK_DRIVER` names a driver, so the system driver path loads none of this code
(linkernsbypass's constructor changes the protection of `dlopen`'s page).
