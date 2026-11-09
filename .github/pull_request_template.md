<!-- Keep a PR to one change. AGENTS.md and CONTRIBUTING.md say where findings go; link to them instead of repeating them here. -->

## What and why

<!-- The symptom or the need, the cause, and the change. Link the issue (Fixes #...) and the reference entry or game note it adds. -->

## Checked

<!-- Delete what does not apply. Say what was not checked too. How to check a change: docs/agents/06-dev-loop.md, "Checking a change for Android". -->
- Unit tests: <!-- e.g. make -C dm/host/tests GAME=<id> run; make -C dm/gfx/tests GAME=<id> run -->
- Python tests: <!-- e.g. python3 -m unittest discover -s dm/tools/tests (includes the launcher's JVM tests, test_android_*.py) -->
- Phone runs on the test app: <!-- game, title ID and version; about 20 s of play past the level load; new [RED] lines, crashes, frame times -->
- GPU drivers: <!-- the system driver and a Turnip driver (DM_VK_DRIVER), with versions -->
- Phone, chip, RAM and Android version: <!-- e.g. Snapdragon 8 Elite Gen 5, 16 GB, Android 16 -->
- Measurements before and after, where the change is about speed, memory, power or heat

## Docs

- [ ] The finding is in its game note or its `docs/reference/<subsystem>.md` entry, and `python3 dm/tools/docs_symptoms.py` was run (the symptom index)
- [ ] A chain tool changed: `dmsh_chain.py --header` (and `vk_android_build.py --header`) was run and `dm/gfx/nvn/dmsh_chain.h` / `dmsh_chain_vk.h` are part of the change
- [ ] No game files, keys or data extracted from a game in the change or the attachments
- [ ] Source only: no binaries, nothing under `dm/third_party/`, no change to `dmsh_chain.lock`; the branch starts from the current `android` ([CONTRIBUTING.md](../CONTRIBUTING.md))
