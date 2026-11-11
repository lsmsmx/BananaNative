# libnxhost.so: the Horizon host (dm/host) and the parts of dm/gfx that are not Metal, for Android arm64-v8a, API 33+ (android/PORTING.md).
# Configure with the NDK's toolchain file and a game profile (there is no default game, as in dm/host/Makefile):
#   cmake -S android/host -B <build> -G Ninja -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=33 -DGAME=<title ID>
#   cmake --build <build>
# The launcher APK builds one host per game instead: -DNX_GAMES=<title ID>,<title ID>,… makes libnxhost_<title ID>.so for each (the same sources, each with its own
# profile header), and no plain nxhost or nxreplay (android/app/src/main/cpp/CMakeLists.txt links a libnxlaunch_<title ID>.so to each)
# The Apple-only sources are left out. NVN replays on Vulkan (dm/gfx/vk_android); audio output, AAC, H.264 and input are Android implementations (dm/host/android,
# dm/host/plat/movie_avc_android.c). The rest of their entry points come from dm/gfx/nvn/android_stub.c, which logs [RED] lines (what the Vulkan replay does not do yet,
# Vulkan and OpenGL games on Metal).
cmake_minimum_required(VERSION 3.22)
project(nxhost C ASM)

if(NOT ANDROID OR NOT CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
  message(FATAL_ERROR "libnxhost is built for Android arm64-v8a only: use the NDK toolchain file with -DANDROID_ABI=arm64-v8a")
endif()
if(NX_GAMES)
  string(REPLACE "," ";" NX_GAME_LIST "${NX_GAMES}")
elseif(NOT GAME)
  message(FATAL_ERROR "GAME is not set: -DGAME=<base game title ID> (the profiles are in dm/games), or -DNX_GAMES=<title ID>,… for one host per game")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)

get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(HOST ${ROOT}/dm/host)
set(GFX ${ROOT}/dm/gfx)
set(TP ${ROOT}/dm/third_party)
set(GEN ${CMAKE_CURRENT_BINARY_DIR}/gen)

# Mach-O assembly → ELF assembly (macho_asm_to_elf.py), so the Apple sources stay as they are
set(ASM_IN ${HOST}/entry.S ${GFX}/nvn/nvn_entry.S ${GFX}/nvn/nvn_thunks.S)
set(ASM_OUT)
foreach(s ${ASM_IN})
  get_filename_component(n ${s} NAME_WE)
  set(o ${GEN}/asm/${n}.S)
  add_custom_command(OUTPUT ${o}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/macho_asm_to_elf.py ${s} ${o}
    DEPENDS ${s} ${CMAKE_CURRENT_SOURCE_DIR}/macho_asm_to_elf.py)
  list(APPEND ASM_OUT ${o})
endforeach()

# The host: every C file of dm/host but the Apple-only ones (the same wildcard as the Makefile, so a new hle_*.c needs no change here)
file(GLOB HOST_C CONFIGURE_DEPENDS ${HOST}/*.c)
list(REMOVE_ITEM HOST_C ${HOST}/hle_movie_avc.c)   # VideoToolbox: dm/host/plat/movie_avc_android.c stands in
list(APPEND HOST_C ${HOST}/plat/plat_android.c ${HOST}/plat/movie_avc_android.c ${HOST}/plat/front_android.c)
# Audio (AAudio, in hle_audio.c), AAC and H.264 (AMediaCodec), input, motion, rumble and the JNI bindings: dm/host/android
file(GLOB HOST_ANDROID_C CONFIGURE_DEPENDS ${HOST}/android/*.c)
list(APPEND HOST_C ${HOST_ANDROID_C})
# dm/gfx without Metal: the NVN front (hle_nvn.c and its C helpers), nn::vi, nvdrv, the CPU texture code; android_stub.c answers for nvn_mtl.m, hle_vk.m, hle_gl.m and the other .m files
file(GLOB GFX_C CONFIGURE_DEPENDS ${GFX}/*.c ${GFX}/nvn/*.c ${GFX}/tex/*.c ${GFX}/vk_android/*.c)   # vk_android: the NVN → Vulkan replay
# Lossless Scaling frame generation (dm/gfx/vk_android/lsfg, ported from Eden): GPL-3.0-or-later, so a libnxhost.so built with it (the default) is GPL-3.0-or-later as a whole
# (android/README.md, android/NOTICE.md). -DNX_LSFG=OFF (Gradle -PnxLsfg=false) leaves it out: the rest of nxnative is MIT, and vka_framegen.c then reports "not in this build"
option(NX_LSFG "Lossless Scaling frame generation (GPL-3.0-or-later)" ON)
if(NX_LSFG)
  file(GLOB LSFG_C CONFIGURE_DEPENDS ${GFX}/vk_android/lsfg/*.c)
  list(APPEND GFX_C ${LSFG_C})
endif()

# libvpx (VP8 / VP9 movies), as the Makefile builds it: its arm64 configuration is plain NEON C
file(GLOB_RECURSE VPX_C CONFIGURE_DEPENDS ${TP}/libvpx/*.c)
add_library(vpx STATIC ${VPX_C})
target_include_directories(vpx PRIVATE ${TP}/libvpx ${TP}/libvpx/config)
target_compile_options(vpx PRIVATE -O2 -w)

# zstd's decompressor (one file, dm/third_party/zstd): the Vulkan replay's shader library stores its SPIR-V modules as zstd frames (vka_shlib.c)
add_library(zstddec STATIC ${TP}/zstd/zstddeclib.c)
target_compile_options(zstddec PRIVATE -O2 -w -fvisibility=hidden)

# dav1d (AV1 movies, dm/third_party/dav1d): C sources and per-bit-depth wrappers
file(GLOB DAV1D_C CONFIGURE_DEPENDS ${TP}/dav1d/src/*.c ${TP}/dav1d/config/tmpl/*.c)
list(FILTER DAV1D_C EXCLUDE REGEX "_tmpl\\.c$")
list(APPEND DAV1D_C ${TP}/dav1d/src/arm/cpu.c)
# and its arm64 assembly (the NEON / dotprod / i8mm / SVE kernels, picked at run time by src/arm/cpu.c), with dav1d_config/config.h ahead of the vendored config, whose non-Apple
# block turns the assembly off: C only, a 2560x1440 AV1 frame of Fire Emblem Fortune's Weave's heaviest movie (NR14_00, 50-73 Mbit/s) took 18-21 ms of the submitting thread on the Pad 3
file(GLOB DAV1D_S CONFIGURE_DEPENDS ${TP}/dav1d/src/arm/64/*.S)
list(FILTER DAV1D_S EXCLUDE REGEX "(_tmpl|/util)\\.S$")
add_library(dav1d STATIC ${DAV1D_C} ${DAV1D_S})
target_include_directories(dav1d PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/dav1d_config ${TP}/dav1d ${TP}/dav1d/include ${TP}/dav1d/config)
target_compile_options(dav1d PRIVATE -O2 -w -fvisibility=hidden)
target_compile_definitions(dav1d PRIVATE _GNU_SOURCE)

# One host: target <tgt> with the profile of <game> compiled in. The game profile header, as dm/host/Makefile makes it (game.py header), goes into gen/<game>
function(nxhost_add tgt game)
  set(GAME_H ${GEN}/${game}/game_profile.h)
  add_custom_command(OUTPUT ${GAME_H}
    COMMAND ${Python3_EXECUTABLE} ${ROOT}/dm/games/game.py header ${game} ${GAME_H}
    DEPENDS ${ROOT}/dm/games/${game}/game.tsv ${ROOT}/dm/games/game.py
    COMMENT "game profile ${game}")
  # The profile's prewarm seed list (vk_prewarm.bin, when it has one) as bytes for the Vulkan replay (vka_pso.c pw_load; dm/gfx/vk_android/vka_pwlist.h). A target that runs at
  # every build (a seed file added after CMake configured would not be seen by a DEPENDS list); game.py writes the header only when it changes
  set(SEED_H ${GEN}/${game}/vk_prewarm_seed.h)
  add_custom_target(${tgt}_seed COMMAND ${Python3_EXECUTABLE} ${ROOT}/dm/games/game.py seed ${game} ${SEED_H} BYPRODUCTS ${SEED_H} COMMENT "prewarm seed ${game}")
  add_library(${tgt} SHARED ${HOST_C} ${GFX_C} ${ASM_OUT} ${GAME_H} ${SEED_H})
  add_dependencies(${tgt} ${tgt}_seed)
  target_include_directories(${tgt} PRIVATE ${GEN}/${game} ${GFX}/nvn/include ${TP}/vulkan-headers/include ${TP}/libvpx ${TP}/libvpx/config ${TP}/dav1d/include ${TP}/stb_vorbis ${HOST})
  # DM_NUP_ZSTD: compressed files of a NUP container are decompressed with zstddec (dm/host/nup_image.h)
  target_compile_definitions(${tgt} PRIVATE _GNU_SOURCE DM_NUP_ZSTD $<$<BOOL:${NX_LSFG}>:NX_LSFG>)
  # As the Mac host (-O1 -g -Wall -Wno-unused-function). Hidden visibility: the assembly reaches C symbols with adrp, which a shared library allows only for symbols it binds itself
  # -Wno-format: the host prints uint64_t with %ll* (unsigned long long on Darwin, unsigned long on Linux: the same 64 bits)
  target_compile_options(${tgt} PRIVATE -O1 -g -Wall -Wno-unused-function -Wno-format -fvisibility=hidden)
  # The replay's draw path runs per draw (thousands a frame): -O2 there (the rest stays at the Mac host's -O1, a later option wins)
  file(GLOB VKA_C ${GFX}/vk_android/*.c)
  set_source_files_properties(${VKA_C} PROPERTIES COMPILE_OPTIONS -O2)
  target_link_options(${tgt} PRIVATE -Wl,-Bsymbolic -Wl,--no-undefined -Wl,--error-limit=0 -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/libnxhost.map)
  set_target_properties(${tgt} PROPERTIES LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/libnxhost.map)
  target_link_libraries(${tgt} PRIVATE vpx dav1d zstddec aaudio mediandk android log m dl z)   # libvulkan.so is opened at run time (vka_dev.c)
endfunction()

if(NX_GAMES)
  foreach(g ${NX_GAME_LIST})
    if(NOT g MATCHES "^[0-9A-F]+$" OR NOT EXISTS ${ROOT}/dm/games/${g}/game.tsv)
      message(FATAL_ERROR "NX_GAMES: ${g} is not a profile in dm/games")
    endif()
    nxhost_add(nxhost_${g} ${g})
  endforeach()
else()
  nxhost_add(nxhost ${GAME})
endif()

# Custom GPU drivers (DM_VK_DRIVER, android/README.md "Custom GPU drivers"): libadrenotools and liblinkernsbypass (BSD-2-Clause, dm/third_party/libadrenotools), as Eden loads Turnip.
# libnxadreno.so is the loader; libhook_impl.so and libmain_hook.so are the hooks it loads into a linker namespace of its own (they must keep these names). The host opens
# libnxadreno.so only when DM_VK_DRIVER names a driver (vka_driver.c): linkernsbypass's constructor patches dlopen's page, so the system driver path loads none of this
enable_language(CXX)
set(ADT ${TP}/libadrenotools)
add_library(nx_linkernsbypass STATIC ${ADT}/lib/linkernsbypass/android_linker_ns.cpp ${ADT}/lib/linkernsbypass/elf_soname_patcher.cpp)
target_include_directories(nx_linkernsbypass PUBLIC ${ADT}/lib/linkernsbypass)
target_compile_options(nx_linkernsbypass PRIVATE -O2 -w -fPIC)
add_library(nxadreno SHARED ${ADT}/src/driver.cpp)
target_include_directories(nxadreno PRIVATE ${ADT}/include ${ADT})
target_compile_options(nxadreno PRIVATE -O2 -w)
target_link_libraries(nxadreno PRIVATE nx_linkernsbypass android log dl)
add_library(hook_impl SHARED ${ADT}/src/hook/hook_impl.cpp)
target_include_directories(hook_impl PRIVATE ${ADT}/include)
target_compile_options(hook_impl PRIVATE -O2 -w)
set_target_properties(hook_impl PROPERTIES CXX_VISIBILITY_PRESET hidden)
target_link_libraries(hook_impl PRIVATE nx_linkernsbypass android log dl)
add_library(main_hook SHARED ${ADT}/src/hook/main_hook.c)
target_include_directories(main_hook PRIVATE ${ADT}/include)
target_compile_options(main_hook PRIVATE -O2 -w)
target_link_options(main_hook PRIVATE -Wl,-z,global)
target_link_libraries(main_hook PRIVATE hook_impl)

# nxreplay: the frame replayer, an executable for adb shell (pushed beside libnxhost.so, run with LD_LIBRARY_PATH; android/TESTING.md "Frame capture and replay"). Gradle does not
# build it (the APK does not carry it): cmake --build <build> --target nxreplay
if(NOT NX_GAMES)
  add_executable(nxreplay EXCLUDE_FROM_ALL nxreplay.c)
  target_link_libraries(nxreplay PRIVATE nxhost)
endif()
