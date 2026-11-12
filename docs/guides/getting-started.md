# Getting started on the phone

This guide sets up a game with the phone alone. A Windows PC or a Mac prepares a game much faster: see
[Preparing a game on a PC or a Mac](pc-mac-packing.md). Back to the [guides](README.md).

## What you need

- Android 12 or newer, a 64-bit phone with a Snapdragon (Adreno) GPU and at least 12 GB of RAM, 16 GB for Mario Kart
  World (`0400C3F00006E000`). Tested on a Snapdragon 8 Elite Gen 5 (Adreno 840) with 16 GB.
- Tested only with the phone's own Qualcomm (system) GPU driver and the latest Turnip; other drivers may not work
  ([GPU drivers](gpu-drivers.md)).
- A phone cooler is recommended, strongly for Mario Kart World (`0400C3F00006E000`).
- Your own dump of the game, made from your own console, as a folder (or a `.nup` file of one). No keys or firmware
  are needed; NSP and XCI files don't work. Mario Kart World (`0400C3F00006E000`) 1.8.0 is a dump with `base/` only.
  ```
  Switch2/
    DonkeyKongBananza/
      base/                  the game (exefs/, romfs/)
      updates/<version>/     its update (exefs/, romfs/), if any
      dlc/<title id>/<n>/    extra content, if any
  ```
- About 1 GB free on the phone for the prepared game (the dump itself is read in place); preparing on the phone needs
  2 GB more while it runs.
- Optional: your own `Lossless.dll` from Lossless Scaling for Windows, for frame generation (not included).

## Steps

1. **Copy your dump** to the phone, for example to `Switch2/DonkeyKongBananza`.
2. **Install the app.** Download the APK from [Releases](https://git.eden-emu.dev/moceure/BananaNative/releases) and
   install it. Open **Banana Native**.
3. **Allow file access.** The **Allow file access** card has an **Allow** button: it opens Android's settings, where
   you turn on **All files access** for Banana Native, then come back. (You can also skip it: a `.nup` file is then
   used where it is, and a dump folder is copied into the app's own storage. See [Storage access](storage-access.md).)
4. **Add the game.** Tap **Add game**, choose **A folder (with base/, and updates/ if any)** or **A .nup file**, and
   pick your dump. Without All files access the choices are **A .nup file (used where it is)** and **A dump folder
   (copied into the app: …)**.
5. **Prepare.** The app checks the dump and asks **Prepare …?** Tap **Prepare**. Preparing rewrites the game's code
   for Android and translates its shaders; it takes about an hour per game on the phone. It runs in the background with a notification, and **Pause** / **Resume** on its card stop and continue
   it. Keep the phone charging. Nothing is uploaded. If the screen going off stops it, turn on Settings › **Keep the
   screen on while preparing**.
6. **Play.** When the card says it is ready, tap **Play**.

A computer is recommended for Mario Kart World (`0400C3F00006E000`).

## Next

- Settings (the gear at the top of Home) holds the defaults for every game: **Overlay**, **On-screen controls**,
  **Resolution**, **Frame rate cap**, **Frame generation**, **Button mapping** and more. A game's own settings
  (its ⋯ button: the **Performance** and **Game** tabs) override them.
- Back up your saves now and then: [Saves](saves.md).
- Something wrong: [Troubleshooting](troubleshooting.md).
