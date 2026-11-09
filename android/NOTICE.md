# Banana Native: license notice for the Android build

Banana Native (the Android launcher in `android/app` and the host it builds, `libnxhost.so`) is based on [nxnative](https://github.com/seedds/nxnative) by seedds,
which is MIT-licensed ([LICENSE](../LICENSE)).

**The Android build that includes LSFG is GPL-3.0-or-later.** By default the host links Lossless Scaling frame generation, a C port of
[Eden](https://git.eden-emu.dev/eden-emu/eden)'s implementation (itself based on [lsfg-vk](https://github.com/PancakeTAS/lsfg-vk)), in
[`dm/gfx/vk_android/lsfg`](../dm/gfx/vk_android/lsfg/LICENSE.md), licensed GPL-3.0-or-later. An APK or `libnxhost.so` built with it is therefore distributed as a
whole under the GNU General Public License, version 3 or later ([COPYING](../dm/gfx/vk_android/lsfg/COPYING)); whoever passes such an APK on must offer its
complete source (this repository at the commit in the build tag). The MIT files keep their MIT license on their own; the rest of nxnative (the Mac and iPhone
builds) stays MIT. `./gradlew apk -PnxLsfg=false` builds an APK without LSFG; it is still GPL-3.0-or-later as a whole when it carries the on-device packer, whose translator is GPL too (below).

**The shader translator is GPL-3.0-or-later as a whole.** It translates Switch 2 task and mesh programs with
[`InstEmitAmpereMesh.cs`](../dm/third_party/ryujinx/src/Ryujinx.Graphics.Shader/Instructions/InstEmitAmpereMesh.cs), ported from
[switch2mac](https://github.com/azmasm/switch2mac)'s `nx2_lower` (from [Eglantine](https://github.com/djihads80/eglantine)), GPL-3.0-or-later
([COPYING.GPL-3.0](../dm/third_party/ryujinx/COPYING.GPL-3.0)). The on-phone translator `libdmsh_xlate.so` and the Mac `dmsh_xlate_bin` are built with it, so
they are combined works under the GPL (see [dm/third_party/ryujinx/README.md](../dm/third_party/ryujinx/README.md)); the shader libraries they produce are the
games' own shaders translated, not part of this code. The other translator files keep their MIT license. Every APK that carries the on-device packer (`libdmsh_xlate.so`) is
therefore a GPL-3.0-or-later work whatever `nxLsfg` says, and the Mac and iPhone builds stay MIT only as apps: the committed translator `dm/gfx/tools/dmsh_xlate_bin` they run to
build shader libraries is the same GPL combined work.

Some fixes follow other projects' findings without copying their code: [kartnative](https://github.com/pahorema/kartnative) by pahorema (MIT) for
GPU-written sparse textures (`dm/gfx/vk_android/vka_tex.c`) and the per-primitive header of mesh programs (`dm/gfx/tools/shader_split.py`), and
[switch2mac](https://github.com/azmasm/switch2mac) by azmasm (MIT outside `nx2_lower`) for the SPIR-V mesh entry points marked in the translator.. The translator's handling of
atomics in `AstOptimizer.cs`, of else-ifs for SPIR-V and of TMML's negative LOD, and the Android gather bias, follow
[nx2android](https://github.com/shirai91/nx2android) by Tung Pham (MIT, as nxnative: "Copyright (c) 2026 seedds" and the MIT permission notice in
[LICENSE](../LICENSE)), with the TMML finding from yuzu #10234 (no yuzu code); the changes are listed in
[dm/third_party/ryujinx/README.md](../dm/third_party/ryujinx/README.md). Thanks to them.

The on-device packer's Python home (`assets/pack/python.zip`) carries, besides the [CPython](https://www.python.org/) Android build (PSF License),
[Capstone](https://www.capstone-engine.org/) 5.0.7 (BSD-3-Clause) and [python-lz4](https://github.com/python-lz4/python-lz4) 4.4.5 (BSD-3-Clause, with LZ4's BSD-2-Clause
sources). Since 0.2.2 the APK also carries `lib/arm64-v8a/libzbicdec.so`, which decodes the ZBIC segments of system 22.0.0+ modules for the packer's
`dm/tools/nso.py`: [Zstandard](https://github.com/facebook/zstd) 1.5.7's decoder by Meta (BSD; zstd is BSD or GPL-2.0, the BSD terms are taken), unmodified, with
an independent reader of ZBIC's entropy tables and the ZBIC frame magic (MIT, Banana Native), written from a description of the format with no code from any other
ZBIC implementation ([dm/third_party/zstd-zbic](../dm/third_party/zstd-zbic/README.md), zstd's licence in its `LICENSE`). Thanks to
[Atmosphère](https://github.com/Atmosphere-NX/Atmosphere) and Yannik Marchand's [zbic](https://github.com/kinnay/zbic) for discovering and documenting the format.
On a PC or Mac the packer may instead use the zbic package from PyPI (GPL-2.0), which the user installs (`android/environment.yml` lists it); it is not in the APK.

`Lossless.dll` belongs to Lossless Scaling (proprietary, sold separately). It is never in this repository or in an APK: the user supplies their own copy, which the
launcher finds on the phone or lets the user pick, and copies into the app's private files.

Custom GPU drivers are never in this repository or in an APK either. The launcher's **Download…** (`DriverFetch.java`) fetches a driver zip only when the user
taps it, from the public GitHub release pages of [MrPurple666/purple-turnip](https://github.com/MrPurple666/purple-turnip),
[K11MCH1/AdrenoToolsDrivers](https://github.com/K11MCH1/AdrenoToolsDrivers), [whitebelyash/freedreno_turnip-CI](https://github.com/whitebelyash/freedreno_turnip-CI),
[Weab-chan/freedreno_turnip-CI](https://github.com/Weab-chan/freedreno_turnip-CI), [crueter/GameHub-8Elite-Drivers](https://github.com/crueter/GameHub-8Elite-Drivers)
and [StevenMXZ/Adreno-Tools-Drivers](https://github.com/StevenMXZ/Adreno-Tools-Drivers);
thanks to their authors. Each download keeps its own licence: Mesa's Turnip driver is MIT-licensed, Qualcomm's Adreno driver binaries are proprietary.
