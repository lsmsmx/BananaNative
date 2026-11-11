#!/bin/zsh
# Builds the on-device packer's native side (android/ONDEVICE.md) on the Mac, the stage the APK takes (build.gradle -PnxPackStage=<work dir>/stage):
#   python: the official CPython 3.14 Android build, Capstone 5 and python-lz4 built with the NDK, libnxpack.so (dm_pack.c) and libzbicdec.so (dm/third_party/zstd-zbic)
#   tools:  the translator (dm/gfx/tools/dmsh_xlate_android/build.sh lib: libdmsh_xlate.so, NativeAOT, .NET 10 SDK) and SPIRV-Tools' validator
#           (build_spirv_tools.sh: libSPIRV-Tools-shared.so), which vk_android_build.py loads through ctypes on the phone
# Usage: build.sh <work dir> [all|python|tools|zbic]   (default all; zbic adds only libzbicdec.so to an existing stage; downloads and intermediate files go into the work dir: put it on the dump drive, not in the home folder)
# → <work dir>/stage/
#     lib/libpython3.14.so, libnxpack.so, libzbicdec.so, libdmsh_xlate.so, libSPIRV-Tools-shared.so   the app's jniLibs (arm64-v8a)
#     python/lib/python3.14/                   the Python home: the standard library with lib-dynload/, site-packages/capstone (+ lib/libcapstone.so), site-packages/lz4
#     bin/dm_pack_cli                          the adb shell test runner (dm_pack_cli.c)
# The sources are pinned by version and SHA-256 (below; SPIRV-Tools by its Vulkan SDK tag); a mismatch stops the build. "python" makes the stage afresh, so run
# "tools" after it (all does both).
set -e
W=${1:?usage: build.sh <work dir> [all|python|tools|zbic]}; WHAT=${2:-all}; HERE=${0:A:h}
mkdir -p $W; W=${W:A}
NDK=${ANDROID_NDK_HOME:-$(ls -d ~/Library/Android/sdk/ndk/* 2>/dev/null | sort -V | tail -1)}
[ -d "$NDK/toolchains/llvm/prebuilt" ] || { echo "[RED] no Android NDK (set ANDROID_NDK_HOME)"; exit 1; }
BIN=$(ls -d $NDK/toolchains/llvm/prebuilt/*/bin | head -1)
CC="$BIN/clang --target=aarch64-linux-android33"
PYV=3.14.6
fetch() {   # fetch <url> <sha256>
  local f=$W/dl/${1:t}; mkdir -p $W/dl
  [ -f $f ] || curl -sSfL -o $f.part $1 && { [ -f $f ] || mv $f.part $f; }
  [ "$(shasum -a 256 $f | cut -d' ' -f1)" = $2 ] || { echo "[RED] $f: SHA-256 mismatch"; exit 1; }
}
S=$W/stage
if [[ $WHAT == all || $WHAT == python ]]; then
fetch https://www.python.org/ftp/python/$PYV/python-$PYV-aarch64-linux-android.tar.gz 38bbe77d3167b5cd554e03b1021324926f09f3825202b065951dd7638e9c37e5
fetch https://files.pythonhosted.org/packages/dd/9c/28b11f64e2425774efb21c206a6e952cfce6e3e2ef3e4b63cdae32ccd8a5/capstone-5.0.7.tar.gz 796bdd69b05fa124fc2aa2e74b9a0b3d4c4e7f3e02add5e583cf2f3bca282ede
fetch https://files.pythonhosted.org/packages/57/51/f1b86d93029f418033dddf9b9f79c8d2641e7454080478ee2aab5123173e/lz4-4.4.5.tar.gz 5f0b9e53c1e82e88c10d7c180069363980136b9d7a8306c4dca4f760d60c39f0
rm -rf $W/src $W/py; mkdir -p $W/src $W/py
tar -xzf $W/dl/python-$PYV-aarch64-linux-android.tar.gz -C $W/py
tar -xzf $W/dl/capstone-5.0.7.tar.gz -C $W/src
tar -xzf $W/dl/lz4-4.4.5.tar.gz -C $W/src
P=$W/py/prefix; INC=$P/include/python3.14; SFX=cpython-314-aarch64-linux-android.so
S=$W/stage; rm -rf $S $W/build; mkdir -p $S/lib $S/bin $S/python/lib $W/build
cp -R $P/lib/python3.14 $S/python/lib/
rm -rf $S/python/lib/python3.14/{test,idlelib,tkinter,turtledemo,ensurepip,lib2to3} $S/python/lib/python3.14/lib-dynload/_ctypes_test.$SFX $S/python/lib/python3.14/lib-dynload/*test*.$SFX
SP=$S/python/lib/python3.14/site-packages; mkdir -p $SP
# Capstone 5.0.7: the C library (AArch64 only: the rewriter disassembles nothing else) by CMake, the Python package as published (it loads capstone/lib/libcapstone.so with ctypes)
cmake -S $W/src/capstone-5.0.7/src -B $W/build/capstone -G Ninja -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-33 \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON -DCAPSTONE_ARCHITECTURE_DEFAULT=OFF -DCAPSTONE_ARM64_SUPPORT=ON -DCAPSTONE_BUILD_TESTS=OFF -DCAPSTONE_BUILD_CSTOOL=OFF \
  -DCAPSTONE_INSTALL=OFF -DCMAKE_SHARED_LINKER_FLAGS="-Wl,-z,max-page-size=16384" > $W/build/capstone.log
ninja -C $W/build/capstone >> $W/build/capstone.log
cp -R $W/src/capstone-5.0.7/capstone $SP/; mkdir -p $SP/capstone/lib
cp -L $W/build/capstone/libcapstone.so $SP/capstone/lib/libcapstone.so
$BIN/llvm-strip --strip-debug $SP/capstone/lib/libcapstone.so   # its debug info names the build folder (this machine's paths); the NDK's own strip, as $CC, on any host
# python-lz4 4.4.5: lz4 and lz4.block (the rewriter's NSO reader needs nothing else), its C extensions against the Android Python
L=$W/src/lz4-4.4.5; mkdir -p $SP/lz4/block
cp $L/lz4/__init__.py $L/lz4/version.py $SP/lz4/
cp $L/lz4/block/__init__.py $SP/lz4/block/
EXT=(-shared -fPIC -O2 -I$INC -I$L/lz4libs -L$P/lib -lpython3.14 -Wl,-z,max-page-size=16384)
$=CC $EXT -o $SP/lz4/_version.$SFX $L/lz4/_version.c $L/lz4libs/lz4.c $L/lz4libs/lz4hc.c
$=CC $EXT -o $SP/lz4/block/_block.$SFX $L/lz4/block/_block.c $L/lz4libs/lz4.c $L/lz4libs/lz4hc.c
# libnxpack.so (dm_pack.c: the C entry and the JNI one, class dev.nxnative.android.PackNative) and the test runner
cp $P/lib/libpython3.14.so $S/lib/
$=CC -shared -fPIC -O2 -Wall -I$INC -o $S/lib/libnxpack.so $HERE/dm_pack.c -L$P/lib -lpython3.14 -Wl,-z,max-page-size=16384
$=CC -O2 -Wall -o $S/bin/dm_pack_cli $HERE/dm_pack_cli.c -L$S/lib -lnxpack -lpython3.14 -Wl,-rpath,'$ORIGIN/../lib'
find $S/python -name __pycache__ -prune -exec rm -rf {} +
fi
if [[ $WHAT == all || $WHAT == python || $WHAT == zbic ]]; then
  # libzbicdec.so (dm/third_party/zstd-zbic: zstd 1.5.7's decoder, BSD, with an MIT reader of ZBIC's entropy tables): the ZBIC segments of system 22.0.0+
  # modules, which nso.py loads through ctypes by its bare name, as vk_android_build.py does libdmsh_xlate.so. An earlier stage's GPLv2 zbic package goes.
  [ -d $S/lib ] || { echo "[RED] $S has no lib/: run build.sh $W python first"; exit 1; }
  rm -rf $S/python/lib/python3.14/site-packages/zbic.*(N) $S/python/lib/python3.14/site-packages/zbic-*.dist-info(N)
  sh $HERE/../../dm/third_party/zstd-zbic/build.sh $S/lib/libzbicdec.so $=CC -Wall
  $BIN/llvm-strip --strip-debug $S/lib/libzbicdec.so
  if grep -qaF "$HOME" $S/lib/libzbicdec.so; then echo "[RED] $S/lib/libzbicdec.so names this machine's home folder"; exit 1; fi
fi
if [[ $WHAT == all || $WHAT == tools ]]; then
  [ -d $S/lib ] || { echo "[RED] $S has no lib/: run build.sh $W python first"; exit 1; }
  G=$HERE/../../dm/gfx/tools/dmsh_xlate_android
  zsh $G/build.sh $W/xlate lib > $W/xlate.log 2>&1 || { tail -20 $W/xlate.log; echo "[RED] the translator did not build (log: $W/xlate.log)"; exit 1; }
  zsh $G/build_spirv_tools.sh $W/spvtools > $W/spvtools.log 2>&1 || { tail -20 $W/spvtools.log; echo "[RED] SPIRV-Tools did not build (log: $W/spvtools.log)"; exit 1; }
  cp $W/xlate/lib/libdmsh_xlate.so $W/spvtools/libSPIRV-Tools-shared.so $S/lib/
  python3 $HERE/../../dm/gfx/tools/xlate_tree.py --check-lib $S/lib/libdmsh_xlate.so || exit 1   # built from this tree (build.gradle checks it again at every APK build)
fi
du -sh $S/lib/* $S/python $S/bin/*
