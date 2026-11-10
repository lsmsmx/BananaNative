# Contributing

Issues and pull requests are welcome. This page says how a change gets into Banana Native, the Android compatibility layer. What the code does and where things are is in [AGENTS.md](AGENTS.md), [android/README.md](android/README.md) and [docs/development.md](docs/development.md).

Banana Native maintains the Android path only: the launcher (`android/app`), the Android host (`android/host`, `dm/host`), the Vulkan backend (`dm/gfx/vk_android`), the shader chain and the tools. The Mac and iOS path (the Metal host, Mac apps and IPAs) is [nxnative](https://github.com/seedds/nxnative)'s: send changes and reports about it there. A change here must not break the Mac tools the shader chain uses, but it does not need to be built or tested on Mac or iOS.

## Reporting a problem

Open an issue with the **Android bug report** form. It asks for what a problem is traced with: the phone, chip, RAM and Android version, the GPU driver (system or Turnip, and its version), the game's title ID and version, the app version, and the log of the run.

- **The log.** Tap a game's ⋯ › **Share log** (that game's newest run), or Settings › **Share log**. After "The game stopped unexpectedly", the dialog's **Share log** also sends a short digest of the run. With `adb`, the logs are in `/sdcard/Android/data/<app>/files/logs/`; for a crash, add `adb logcat -d -b crash` ([android/TESTING.md](android/TESTING.md#7-send-the-log-back)).
- **A summary.** `python3 scripts/log_digest.py <log, folder of logs or zip>` prints about 50-80 lines: the device and driver, the stage the run reached, the crash, the `[RED]` lines and the frame rate. It leaves out key-like lines.
- Look at what you attach before you attach it.

Questions, ideas and reports that a game runs on your phone go in the **Question, idea or development topic** form.

## Never game files or keys

Never commit or attach game files, keys, title keys or anything extracted from a game: RomFS, code modules, shaders, packs, dumps. Game files and their extractions stay with the game's dump folder ([AGENTS.md](AGENTS.md)). Logs, screenshots, title IDs, build IDs and offsets are fine. Do not add anything that asks for keys or explains how to obtain games.

## Before you start

- Read [AGENTS.md](AGENTS.md): it lists where to start for each kind of task, and the rules for profiles, per-build knowledge and the shader translation chain.
- A crash, a hang or a wrong picture: search the [symptom index](docs/agents/symptoms.md) for the text in the log first.
- Work on one game: read its note, `dm/games/<id>/README.md`, from the [games table](dm/games/README.md#games). [Experimental games](docs/guides/experimental-games.md) lists the open problems.
- Prefer a fix that holds for every game to a game-specific rule, and fix a bug at its cause, not by hiding the symptom ([AGENTS.md](AGENTS.md)). Driver differences key on what the driver reports (`driverID`, features), never on its name.
- Performance work (frame pacing, power, heat, memory, load times) counts as much as correctness.

## A pull request

- **One change per PR, off the current `android` branch.** Rebase on `android` rather than merging it in. A branch forked before a history rewrite carries old copies of public commits and cannot be merged, whatever the merge button says: start a new branch from the current `android` and cherry-pick your own commits onto it.
- **Commits.** Each commit message says what changed and why, in plain English sentences, as the history does: the symptom, the cause, the fix, and what was measured or checked.
- **Source only.** No binaries of any kind: no translator DLLs (`dm/gfx/tools/dmsh_xlate_bin`), no `.so`, APK, GPU driver, shader library or prewarm seed. The maintainer builds every binary from source. A translator change is the `.cs` diff plus the shaders it fixes.
- **The shader chain.** Do not change `dm/gfx/tools/dmsh_chain.lock` or the chain headers (`dm/gfx/nvn/dmsh_chain.h`, `dmsh_chain_vk.h`): they are regenerated here, because a new chain version means a shader rebuild for every player. Say which games' shaders your change affects.
- **Nothing under `dm/third_party/`.** Vendored code is pinned at a version ([dm/third_party/README.md](dm/third_party/README.md)); describe the change you need and the maintainer applies it as a documented version bump.
- **Per-game values** go in the game's profile (`dm/games/<id>/`), not in code. Fixed addresses are gated by the module's build ID ([AGENTS.md](AGENTS.md)). A new environment switch gets its row in `docs/env_switches.tsv`.
- **Where findings go.** A fact about one game goes in its note (`dm/games/<id>/README.md`). A general mechanism gets an entry in `docs/reference/<subsystem>.md` (Symptom, Cause, Fix, Found on), followed by `python3 dm/tools/docs_symptoms.py`. A new game takes its profile, its note and its row in the games table ([dm/games/README.md](dm/games/README.md)).
- **Language.** English in code, comments, log messages and the app's text. Lines in the log for something unimplemented or wrong start with `[RED] `.
- **The PR text.** Fill in the template: what and why, what was checked (tests, games, phone, drivers), and which docs changed.

## Checking a change

There is no CI on the public repository: run the checks yourself and say in the PR which ran ([Checking a change for Android](docs/agents/06-dev-loop.md#checking-a-change-for-android); by what the change touches: [Before you open a pull request](docs/agents/06-dev-loop.md#7-before-you-open-a-pull-request)).

1. **Unit tests**: `make -C dm/host/tests GAME=<id> run`, and `make -C dm/gfx/tests GAME=<id> run` when the change reaches graphics or the shader chain. The maintainer runs them on macOS, so new tests must pass there: no `-no-pie`, no AddressSanitizer leak detection, no Linux-only flags such as `MAP_FIXED_NOREPLACE`.
2. **Python tests**: `DM_GAME=<id> python3 -m unittest discover -s dm/tools/tests`. They include the launcher's JVM tests (`test_android_*.py`) and the docs check (`test_docs.py`).
3. **Phone runs** with the test app (`dev.nxnative.android`, [android/TESTING.md](android/TESTING.md)): about 20 s of play past the level load, on the system driver **and** on a Turnip driver ([Custom GPU drivers](android/README.md#custom-gpu-drivers)). Name the games, their versions, the phone and both drivers, and the numbers before and after when the change is about speed, memory, power or heat. Use a copied save, never your only one ([Saves](android/TESTING.md#saves)).

Say what was not checked too.

## Large changes

- **Agree on the scope first.** Open an issue before writing a change that adds a device class or a GPU vendor, adds a second implementation of an existing path, or runs to more than about 1,000 lines. A large PR built on an older `android` can rarely be reviewed or merged, however well it was tested.
- **One concern per PR.** A launcher feature, a host change and one game's fixes are separate PRs, each on the current `android`.
- **Supported phones** are the ones the [README](README.md) names: Android 13 or later, a Snapdragon (Adreno) GPU, and at least 12 GB of RAM. Supporting other GPUs or smaller phones is the maintainer's decision, so ask in an issue first: it adds code paths that every later change has to keep working and that few contributors can test.

## How changes get in

The maintainer reviews every PR against [the review checklist](docs/agents/07-pr-review.md), builds and tests it on the current tree, runs it on the phone, and merges it into `android`, so the PR shows as merged and its author as a contributor. The maintainer may ask for a split, or rebase a PR that conflicts with `android`. A merged change reaches players with the next release.
