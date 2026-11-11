<p align="center"><img src="../android/images/logo.svg" alt="Banana Native logo" width="128"></p>

# Banana Native

Banana Native runs Switch 2 games from your own dumps on Android phones. Like Wine, it emulates no hardware: the
game's own ARM64 code runs on the phone's CPU, Banana Native supplies the system services it calls and translates its
graphics to Vulkan. It is an Android port of [nxnative](https://github.com/seedds/nxnative) by seedds.

> [!NOTE]
> Unofficial and experimental. Not affiliated with Nintendo, nxnative or Eden. The project contains no Nintendo code,
> firmware or keys and decrypts nothing: it reads only a dump of a game you own, made from your own console. Most of the
> code was written with an AI coding assistant (Claude) and tested by hand. [More](../docs/players/about.md).

## Games

| Game | Version | Status | Notes |
|---|---|---|---|
| Donkey Kong Bananza (`040074A01BF12000`) | update 458752 | Supported | 30 fps |
| Mario Kart World (`0400C3F00006E000`) | 1.8.0 | Supported | a phone cooler is recommended |
| Pokémon Pokopia (`0400A3A01EE4C000`) | update 655360 (Ver. 2.0.1) | Supported | about 60 fps |
| The Legend of Zelda: Ocarina of Time (`0400486024DEC000`) | 2.0 | Experimental | plays, with visual glitches (too bright, mesh foliage missing); system driver only |
| Splatoon Raiders (`0400380020056000`) | update 65536 | Experimental | boots and runs, but not playable yet (very low frame rate) |

The app tells you if your dump is another version. Splatoon Raiders is not playable yet: it boots and runs at a very low
frame rate, and ships so that people can test it and help. Ocarina of Time: only version 2.0 is tested; the first starts are slow
(minutes of loading, then low fps and black frames) until the GPU driver's pipeline cache fills. [Experimental games](../docs/guides/experimental-games.md) has their known problems and how to help.

## What you need

- An Android 13+ phone with a Snapdragon (Adreno) GPU and at least 12 GB of RAM, 16 GB for Mario Kart World
  (`0400C3F00006E000`). Tested on a Snapdragon 8 Elite Gen 5 with 16 GB, with the phone's own Qualcomm (system) driver
  and Turnip.
- Your own dump of the game, as a folder (or a `.nup` file of one): `base/`, and `updates/<version>/` and `dlc/` if you
  have them, each with `exefs/` and `romfs/`. NSP and XCI files don't work.
- Recommended: a Windows PC or a Mac, and a phone cooler.

## Setup

Each game is prepared once into a **pack** (its code rewritten for Android, its shaders translated for Vulkan). A
computer does this much faster than the phone.

First install the APK from [Releases](https://github.com/moceure/BananaNative/releases) (or the mirror on [git.eden-emu.dev](https://git.eden-emu.dev/moceure/BananaNative/releases)) and allow **All files access**.

**PC (Windows) or Mac** ([guide](../docs/guides/pc-mac-packing.md)). Mac: tested. Windows: new and not yet tested on a
real PC — please report how it goes.

1. Download this repository and double-click `scripts\bananative.bat` (Windows) or `scripts/bananative.command`
   (Mac). The first time, it installs its tools (about 400 MB) into a folder you pick.
2. Pick the game (its folder or `.nup` file) and click **Prepare**.
3. Copy the game folder to the phone, and the pack next to it: `<title ID>-work/android-pack`.
4. In the app, tap **Add game** and pick the game folder. It finds the pack. (Or use **Import pack…** with the pack
   folder.)

**Phone only** ([guide](../docs/guides/getting-started.md)): copy the game folder to the phone, tap **Add game**, then
**Prepare**. It runs in the background and can be paused; keep the phone on the charger. The first Prepare takes
about 20–45 minutes (mostly translating shaders). Prepare is incremental: later runs reuse existing shaders and
only rewrite code when its key changes (a few minutes). A computer is recommended for Mario Kart World (`0400C3F00006E000`).

## Problems

- Settings › **Readiness check** says what your phone is missing and fixes most items.
- A game's ⋯ button › **Share log** shares its log.
- Open an [issue](https://github.com/moceure/BananaNative/issues/new/choose) with the form; questions go to
  [Discussions](https://github.com/moceure/BananaNative/discussions). More: [Troubleshooting](../docs/guides/troubleshooting.md).

> [!IMPORTANT]
> The official places for Banana Native are this GitHub repository and its mirror on
> [git.eden-emu.dev](https://git.eden-emu.dev/moceure/BananaNative): download the APK only from their releases. Nobody
> from the project will ask you for money, keys or game files.

## Guides

[Getting started](../docs/guides/getting-started.md) ·
[PC or Mac](../docs/guides/pc-mac-packing.md) ·
[Storage access](../docs/guides/storage-access.md) ·
[Saves](../docs/guides/saves.md) ·
[Packs and app updates](../docs/guides/packs-and-updates.md) ·
[GPU drivers](../docs/guides/gpu-drivers.md) ·
[Troubleshooting](../docs/guides/troubleshooting.md) ·
[Experimental games](../docs/guides/experimental-games.md)

## More

[Features](../docs/players/features.md) ·
[Performance](../docs/players/performance.md) ·
[How it works](../docs/players/how-it-works.md) ·
[About the project](../docs/players/about.md) ·
[Videos and device reports](../docs/players/videos.md) ·
[History](../docs/players/history.md)

## Contributing

Each game's note says what is known and what is open ([games table](../dm/games/README.md#games)); working on the code:
[docs/agents](../docs/agents/README.md), [android/README.md](../android/README.md). The experimental games need the
most help: see the issues marked
[help wanted](https://github.com/moceure/BananaNative/issues?q=is%3Aissue+is%3Aopen+label%3A%22help+wanted%22).

## Licence and credits

The Android app is **GPL-3.0-or-later** as a whole; nxnative's own files keep their MIT licence. See [android/NOTICE.md](../android/NOTICE.md) and the [credits](../README.md#licence-and-credits).

Use games you own. Please don't ask for games, keys or dumps.
