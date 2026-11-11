# Fire Emblem Fortune's Weave (`04007EF01F8AE000`)

A Switch 2 game: v131072, SDK 22.2.0 built for the Switch 2 (`Ounce`), NVN2 API 231.40. Intelligent Systems' engine on NintendoWare (G3d, Ui2d, Vfx, Atk), PhysX 5.6, a deferred renderer. Modules `main`, `subsdk2` (movies) and `sdk`; `subsdk0` (DLSS) and `subsdk1` (CUDA) are not loaded. **Plays on the Mac**: the opening movie, the dialogue and the first battle map, whose first turn matches published footage, and its battle scenes in the user's playtest. No IPA has been built.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `mem_alias 0x400000000`, `mem_reserve 0x550000000`, `mem_stack 0x10000000`, `app_pool 0x240000000` | [`game.tsv`](game.tsv) | Splatoon Raiders' split as a starting point; the Switch 2's 9 GB |
| `sys_resource 0x4000000` | `game.tsv` | the NPDM declares 64 MB, so virtual address memory is on |
| `pad_layout full` | `game.tsv` | the game asks for most of the controller at once |
| `zip_store .zst:.mp4:.webm:.metallib` | `game.tsv` | the RomFS's files are compressed already |
| `fps_target 60` | `game.tsv` | the host writes 60 into the game's target frame rate, which the rewriter finds by its readers in any build; see [Frame rate](#frame-rate) |
| `fps_max 120` | `game.tsv` | **Frame rate** offers 120 on the Mac: speed measured below; see [Frame rate](#frame-rate) |
| `depth_mode zero` | `game.tsv` | the game selects near-zero NVN depth; offline pipeline bases must use the matching vertex entry. Without it the app's bundled archive (661.6 MB on an M1 Pro) was refused at every start and the Mac built 872 MB of bases of its own in the background |

No `render_cond`: not tried, and nothing has called for it so far.

## Files

- A dump folder holding only `exefs/` and `romfs/`, with no `base/` layout: a RomFS of 28 GB. The NSOs are ZBIC-compressed, so tools that read them need the `zbic` package in the work folder's venv.
- Shaders: 16,888 in [`shader_map.tsv`](shader_map.tsv), from `Shaders/*.bfsha.zst` and the effect and layout packages.
- Movies: 82, AV1 in WebM at 2560x1440 (AV1 movies through SDK 22's decoder). A Mac without a hardware AV1 decoder (before M3) decodes them with dav1d.

## Shaders

All 16,888 translate (current). One shader has 10 texture handles the translator cannot resolve (`Failed to find handle source for bindless access of type "texture2D"`) and one logs `Invalid reduction type: F32FtzRn`; neither has been traced to anything visible (open).

## Rendering

- The battle map renders at 1024x576 and DLSS, on MetalFX here, takes it to 2048x1152 (NVN2's DLSS on MetalFX), which is scaled to the 3840x2160 window. The network's size follows the display (The display's largest buffer).
- Auto exposure does not meter the scene. `5372c7e7cb7bbda2` writes one luminance into a 16x16 RGBA16F target (134.4 on the first map, log2 7.07), `6d0f6db3f0d396a3` makes a 256-bin histogram of it (the single bin 218), and `a0c632d8b5fa925b` averages its 10th to 90th percentile with a 64-bit division, takes the average through two 256x4 curves and moves a 3x1 R16F image towards the result. The tonemap reads `2^-(image[0] + 3) × 1.44`. On the first map `image` holds 8.96, 18.89, 9.46 with neither of the translator fixes below, and 5.555, 15.195, 6.055 with them, when the picture matches the footage. At a scene change the game clears the image to 1 and dispatches these shaders (Pending clears land before a compute dispatch).
- Ambient occlusion is interleaved: depth into a 4-layer 512x288 array, four triangles picking the layer by primitive ID, then back together at 1024x576 (`[[primitive_id]]` in a specialized Metal 4 pipeline).
- The enemy's checkered look and the pale squares over the ground are in the published footage too: both are the game's.
- Frame time: 33 ms with the window on screen at the game's own 30 fps (16.7 ms at `fps_target 60`, [Frame rate](#frame-rate)), and GPU about 12 ms. A covered window reads 80-180 ms, the host's display-link fallback (`front end: window covered`), which measures nothing.
- Resolved: at Alcineus Bridge on an M1 Pro (macOS 27.2, 1080p output) the scene ran at 4.24 fps, the lighting pass 196.7 ms of a 236 ms frame, while its reads of a uniform buffer's handle table were marked through a null address (Reads of a handle table marked through a null address). PR #1738's host bitmap measured 56.1 fps and 2.35 ms there (on `bd979ce`, chain `3ee52af3ca18`); the chain change that replaced it was not run on an M1 Pro.

## Frame rate

- **Current:** 60 fps (30 in gameplay as shipped). The game's graphics object, a global at `[main+0x21e60d8]`, holds the display rate at +0x1d8 (60), the target frame rate at +0x1dc and the longest present interval at +0x1e0 (5). The present (`main+0x648da0`, through `gxc::NvnDevice`'s present, its vtable's slot 3 at `main+0x5d170`) asks for interval min(+0x1e0, +0x1d8 / min(+0x1d8, +0x1dc)).
- The game sets the target to 60 at `main+0x652960` and `main+0x66af3c` (the title and menus run at 60) and to 30 at `main+0x7bf1c0`, where gameplay starts, which makes it interval 2. 59 readers step the game by 60.0 / target (`main+0x770d5c`, `main+0x11e6310`, `main+0x13a6c94`): 2.0 a frame at 30, 1.0 at 60.
- The rewriter finds the field by those readers and the store of 30, with no build ID (A target frame rate the game steps by; `dm/tools/fps_target.py`: 55 of the 59 readers have the pattern it matches, `FPS_TARGET_PTR 0x21e60d8`, `FPS_TARGET_FIELD 0x1dc` in main's fixups), and the host writes the profile's `fps_target` there on every present where it differs (`hle_fpslock.c`, "target frame rate"). The game then asks for interval 1 itself. On an M4 Pro the battle map holds frame time p50 16.7 ms, p95 17.1 ms, about 2 frames of 300 dropped, in a window. Headless with the found field (2026-11-03, M4 Pro, A every 0.8 s from the title into gameplay): the game sets interval 1 → 2, the host writes 30 → 60, the game sets interval 2 → 1 (`main+0x5d1c8`), p50 16.6-16.7 ms through frame 7200.
- `DM_FPS_TARGET=30` (the settings window's Frame rate) writes 30, menus included, where the game itself chose 60.
- Not measured: game speed at 60 against 30 over the same route (the player saw battles at 60 with nothing reported fast).
- **Current: 120 fps on the Mac when chosen** (120 fps follows the frame step). The present (`main+0x649920`) still asks for interval 1, as 60 / min(60, 120) = 1, and all 60 reads of the target found through the global are floats, so the step takes 0.5. Nothing ties this to a build: the rewriter finds the field in any main, and `fps_max` is not gated. v131072 is the only version tested. The target follows what fits, so frames at 60 step by 1.0 and the game keeps its speed; before that, the same field at a fixed 120 would have run a 70 fps scene at 58%.
  - Speed: from one save on a battle map, the same timed cursor holds (0.3, 0.5 and 1 s, then back) at 120 and 60 stop at the same place on all eight shots: the best match between each pair is a shift of zero pixels (2026-11-04, M6, headless). The user played at 120 and saw the speed as right. The holds are in whole tiles and the rest was judged by eye, so a difference of a few percent would not show.
  - Frame rate: on the map, headless, 107-119 fps with 96-100% of frames at 120 per 1,800 frames (frame time p50 8.3 ms, GPU 5.5 ms, about 840 draws). In the window on the prototype, without the fallback, most of the user's session held 106-117 fps at 600-990 draws, and 65-72 fps where the GPU took 12-13 ms; that stretch would now run at 60. Added lag of 12 ms a frame holds a steady 60 with the target at 60. In a window the host picks the 120 Hz display link by itself (open: the frame rate of this build in a window; the one short run had two other games running on the Mac and measured nothing).
  - Known: `game::telop::ListResultMenuItemBase` (`main+0x17885f8`) adds max(int(step), 1) to a timer each update, 8 times that while an input flag is set, so its In / Out timer runs at double speed at 120. It is a results list's menu item, a matter of looks, and is left as it is (a cut would tie this to the build). Systems that ignore the step altogether would also run fast; none has been seen so far.

## Driving it on the Mac

Keyboard in a window: J K U I are A B X Y, WASD the left stick, Shift + arrows the right stick, Z and C are ZL (threat range) and ZR (settings). The work folder has `win.sh` (a window with Metal's HUD, `run/ctl.txt` as `DM_CTL`), `mash.sh <secs> [dir]` (A every 0.8 s, with shots), `rebuild.sh` (a host copy, `build/fewhost`), `shaders.sh`, and `scripts/shdump.py <key> [ops]` for a shader's raw instructions. Pressing A from boot goes through the movie and the dialogue to the first battle map in about 80 s. Serials shift between runs, so read them from a pass dump; `DM_NVN_PASSDUMP_ALL=1` lists every dispatch's bindings.

## Open

- Nothing past the first map has been played on this build.
- `stage 1 cb4 has no uniform buffer bound`, once a run, with no effect seen.
- MetalFX's jitter sign and DLSS's bias mask are unchecked; the bias mask is only logged. No ghosting has been seen so far.
- An IPA: the 16 GB alias region is over iOS's limit, as for [Splatoon Raiders](../0400380020056000/README.md).
- `dm/games/profile_guess.py` does not recognise the SDK's `Generic` version name.

## Found on this game

- [Scaled shared memory addresses](../../../docs/reference/shaders.md#scaled-shared-memory-addresses) and IMAD.HI adds a register pair, and IMAD.X its carry: the dark map.
- Pending clears land before a compute dispatch: battles and scene changes blown white.
- NVN2 compute reads its workgroup counts at c0 + 4.
- Texture handles of SDK 22's Ampere code: white units.
- A table of handles past the array's length: black characters.
- Reads of a handle table marked through a null address: 4 fps at Alcineus Bridge on an M1 Pro.
- `[[primitive_id]]` in a specialized Metal 4 pipeline: the dot lattice over the ground.
- NVN2's DLSS on MetalFX.
- Bindless shaders taken as depth readers: the battle maps' decals split the G-buffer pass about 31 times a frame.
- [System applets opened by `Show…` calls](../../../docs/reference/services.md#system-applets-opened-by-show-calls): a null read in `nn::applet::CreateLibraryApplet` at the title about 100 s in, the strap guide overload with a `StrapGuideArg`.
- A target frame rate the game steps by: gameplay at 60 fps.
- 120 fps follows the frame step.
- AV1 movies through SDK 22's decoder.
- [The Joy-Con mouse pointer](../../../docs/reference/services.md#the-joy-con-mouse-pointer) and The display's largest buffer.
- The output device notification under its SDK 21 and 22 name: an abort at start without it.
- [An SDK 21 Switch 2 game with a 4.5 GB alias region](../../../docs/reference/memory.md#an-sdk-21-switch-2-game-with-a-45-gb-alias-region): SDK 22's node area.
- The title screen's hand-over to the opening demo finalizes a program its queue still has bound: Finalized programs leave every queue's bindings (resolved).

## Android

- **Playable** (`readiness.tsv`): on the OnePlus Pad 3 the game was played from the intro through many battles to the hub area Dagsion with no significant issue, on Turnip Gen8 V39 and on the stock driver. On the AYN Thor (Adreno 740) pick Turnip (below). The profile is the Mac's, with `fps_target 30` (the game's own gameplay rate, menus included; the in-game panel's cap picks 60) and no `fps_max`, plus three Android keys: `dlss_standin 1`, `subgroup_one_lane 35d12d6f98fa05e2` and `av1_decoder dav1d` ([`dm/games/README.md`](../README.md)).
- The display's largest buffer answers the 1080p screen in handheld mode (`hle_vi.c`), so the game picks its 1080p output. Docked it renders 1024x576 and DLSS takes it to 2048x1152; handheld (`DM_OPMODE=0`) 960x540 to 1920x1080. The game takes its render size from its own resolution tables, not from DLSS's optimal settings: an answer of 1366x768 left it at 1024x576.

### Tested devices

| Device | GPU | Driver | Status |
|---|---|---|---|
| OnePlus Pad 3 | Adreno 830 | Qualcomm 512.800.77 (stock) | **Plays.** Dungeon at 30 fps: 1 frame dropped in 14,400, 4 in 49,200 with the Upscaler switched live; GPU about 14.5 ms. Characters correct. Movies below |
| OnePlus Pad 3 | Adreno 830 | Turnip Gen8 V39 | **Plays.** Played from the intro through many battles to Dagsion. Battle map at 30 fps, GPU about 15 ms. Characters correct |
| AYN Thor | Adreno 740 | **Turnip "T30" (purple-turnip, Mesa 26.3.0-devel)**, recommended | **Plays.** Dungeon at 29-30 fps: 0 dropped in 900 frames in each of two runs, GPU 19.3 ms docked, footprint 3.73-3.76 GB, MemAvailable at least 3.3 GB. Characters lit correctly. Movies: MV00_00 at 30 fps, 0 dropped in 3,300 frames; NR14_00 at about 30 fps, 0 dropped in 1,800 |
| AYN Thor | Adreno 740 | Turnip Gen8 V39 (an A8xx build that runs on the 740) | **Plays.** Dungeon at 30 fps: 0 dropped in 900 frames, GPU 28.8 ms docked, 24.2 ms handheld. Characters correct |
| AYN Thor | Adreno 740 | Qualcomm 512.676.53 (stock) | Runs, with faults: characters and some foliage are lit dark, the dungeon runs at about 25 fps (GPU 36 ms, command recording 26.6 ms). Movies on dav1d: MV00_00 at 30 fps, 1 dropped in 3,300; NR14_00 at 30 fps, 0 dropped in 1,500 |
| AYN Thor | Adreno 740 | Qualcomm v863.1 (Vulkan 1.3.295) | Not recommended: characters correct, but full-screen tile corruption from time to time, and the footprint grows from 3.4 to 5.5 GB. Dungeon 28.2 fps |

- Memory: on the Pad 3 the dungeon takes PSS 3.72-3.78 GB, and MemAvailable falls by 5.25 GB from launch. On the Thor on Turnip Gen8 V39: a footprint of 3.75 GB with MemAvailable at least 3.3 GB; PSS 3.94-3.97 GB on the stock driver, where the driver's own memory (below) is not counted in it.
- The Pad 3's panel ran at 50 Hz in some 30 fps sessions, holding frames for 20 or 40 ms (on-screen p99 39.94 ms), and at 60 Hz in others (33.33 ms); what picks 50 Hz is not known. At 60 fps (`DM_FPS_TARGET=60`) the Pad 3 runs the game (GPU busy p50 14.4 ms with GSR 2, 13.7 ms with FSR 1) but reached 93-96 °C on the GPU within 3 minutes, and a 50 Hz panel shows only about 40 frames a second, so 30 stays the profile's rate.
- Known on the stock 512.676 (open): the cause of the dark characters and foliage is not found. Ruled out by A/B: the light grid's compute shader and the integer view of its 3D image (`DM_VK_TEX_RETYPE=0`, `DM_VK_IMG_RETYPE=0`).

### DLSS on a stand-in

NVN2's DLSS is recorded as `OP_DLSS` (`hle_nvn.c`, "NVN2's DLSS"). Vulkan has no counterpart of the network, so the replay writes the evaluation's output texture with an upscaler of its own (`dm/gfx/vk_android/vka_dlss.c`, picked in `vka_exec.c` `dlss_eval`). Without one the evaluation was dropped, the output never written, and the 3D picture black.

- **GSR 2** (`DM_VK_DLSS=3`, the default): Snapdragon Game Super Resolution 2, the 3-pass version (convert, activate, upscale; Qualcomm's shaders under the BSD 3-Clause licence in [`dm/gfx/vk_android/gsr2/LICENSE`](../../gfx/vk_android/gsr2/LICENSE)), then RCAS. Its history accumulation does the temporal anti-aliasing DLSS did. The default tune, m2, cuts the ghosting of the earlier one: each output pixel takes the motion of its nearest render texel instead of a bilinear one, history is rejected where the motion changes from the last frame (velocity rejection at half strength), the history box shrinks further on moving pixels, and the box of still pixels is capped at 4 (20 upstream). On the Pad 3 it costs 1.76 ms of GPU a frame (m1 1.75 ms, c4 1.79 ms).
- **FSR 1** (`DM_VK_DLSS=2`): EASU and RCAS on the tonemapped input. The game jitters every frame (Halton(2,3), ±0.5 input pixels), which a spatial upscaler shows as crawling edges, so EASU's sampling moves with the frame's jitter (`DM_VK_DLSS_UNJITTER`, default 1); this cuts still-edge crawl by 15-30 %, not to zero. 1.30 ms on the Pad 3. RCAS strength `DM_VK_DLSS_SHARP` (0..100, default 15).
- **Bilinear** (`DM_VK_DLSS=1`): a blit. 0.08 ms.

With `dlss_standin 1` the launcher's Upscaler row (a game's ⋯ Manage › Game settings, and live in the in-game panel's Performance tab) offers the stand-in's modes in place of the generic choices, with its sliders beside it (`DlssTune.java`, `GameUpscaler.java`, `GameSettings.java`):

| Row | Choices | Default | Lever |
|---|---|---|---|
| Upscaler | GSR 2 (smooth images) / FSR 1 (sharp, fast) / Bilinear (fastest, softest) | GSR 2 | `DM_VK_DLSS` 3 / 2 / 1 |
| Motion clarity (GSR 2 only) | Steadiest / Steady / Balanced / Clear / Clearest (may flicker in combat): steps c0..c4, more history on the left (steadier, more ghosting), less ghosting on the right | Balanced (c2 = tune m2) | `DM_VK_GSR2_CLARITY` 0..4; `DM_VK_GSR2_TUNE` forces a row (0 = m1) |
| Temporal sharpness (GSR 2) / FSR sharpness (FSR 1) | Off, 5, 10 … 30 | 15 | `DM_VK_DLSS_SHARP` (the host takes 0..100) |

The stand-in's output then reaches the screen through the present's own scaling, which follows the main screen's (or an earlier per-game) Upscaler choice: SGSR 1 (the default) and FSR 1 give FSR 1 (`DM_VK_FSR=1`), Bilinear and Native bilinear, Nearest neighbor nearest (`DM_VK_PRESENT_NEAREST=1`); no `DM_VK_NDSR` line is sent for the game. Every one is read live (the stand-in's at every evaluation, the screen's at every present), so the panel changes the picture at once. The tune's other fields have diagnostic levers (`DM_VK_GSR2_VREJ`, `_VLO`, `_VK`, `_SFCAP`, `_MVNEAR`, `_BOXMOTW`, `_BSMOV`; `docs/env_switches.tsv`).

### AV1 movies on dav1d

The 82 movies are AV1 at 2560x1440. Android's default sends AV1 to the phone's hardware decoder ([Movie pictures late: decoding on the player's thread, AV1 on the CPU](../../../docs/reference/movies.md#movie-pictures-late-decoding-on-the-players-thread-av1-on-the-cpu-android)), and other games keep it. This game's profile has `av1_decoder dav1d`: dav1d frame-parallel (8 task threads and 5 frames in flight on an 8-core phone), its arm64 assembly built into the Android host, each picture converted to RGBA on the decode thread and written into the image pool's tracked pages in one go (`hle_movie_av1.c`, `hle_codec_video.c`). Measured on the same Thor:

| Decoder | Driver | MV00_00 (opening) | NR14_00 |
|---|---|---|---|
| Hardware (`c2.qti.av1.decoder`) | Turnip | 23 fps for about 30 s, 290 dropped in the first 900 frames, then 30 fps | 22.5-23 fps, 521 dropped in 1,200 |
| Hardware (`c2.qti.av1.decoder`) | stock 512.676 | 24-25 fps, 409 dropped in 1,200 | not measured |
| dav1d frame-parallel | stock 512.676 and purple-turnip T30 | 30 fps, 0-1 dropped in 3,300 | 30 fps, 0 dropped in 1,500-1,800 |

- The hardware decoder's drops come from the game's thread: each long frame took 3,600-7,200 write-protection faults (about 50 ms of CPU) as the picture was copied page by page into the pool's tracked pages.
- Pad 3 (stock 512.800.77, dav1d): MV00_00 went from 16-23 fps to 30 fps with 0 dropped (video lag −9 to +64 ms); NR14_00 from 12.7-20.8 fps to 26.8-30 fps, with 12-22 frames in 300 still dropped in its heaviest stretches. The hardware decoder was not measured on the Pad 3.
- Levers: `DM_AV1_DAV1D_FP=1` / `0` overrides the profile; `DM_DAV1D_THREADS` and `DM_DAV1D_FRAME_DELAY` set the threads and frames in flight; `DM_AV1_RGBA_EARLY=0` converts on the game's thread instead; `DM_MOVIE_RGBA_NEON=0` keeps the scalar RGBA loop.
- Sound: the movies' music, sound effects and voices are separate game streams (`Sound/stream/`), cued by the video's time. With the pictures' own timestamps carried through the decoder, the sound effects and voices play with the movie (fitted against the reference streams on the Pad 3, and present on the Thor on both decoders). Not re-checked by an audio capture since.

### Rendering fixes

- The window textures (3840x2160 RGBA8 sRGB) share one image with an A2B10G10R10 target. The present read them in the image's format whenever the target made the image first: black turned blue and the rest purple or cyan, scene by scene. The present now reads a window texture in its own format (`DM_VK_PRESENT_OWNFMT`).
- An array of texture or sampler handles in a uniform buffer takes element k from base + 8k (2,792 such arrays in 1,097 of the game's shaders; `DM_VK_TEXU_ELEM`).
- A vertex attribute state bind replaces the whole attribute state, as the Metal backend does, so a vertex shader input past the bound count reads the zero buffer instead of an earlier mesh's attribute at the current stream's offset; that drew dark streaks across the class showcase (`DM_VK_VATTR_CLEAR`).
- The light grid's compute shader `35d12d6f98fa05e2` lost the device at its first dispatch, at the title, on the Pad 3's stock driver; on Qualcomm's driver its subgroup operations run as one invocation (`subgroup_one_lane`, `DM_VK_ONE_LANE`); Turnip runs it as built.
- Qualcomm's own driver before 512.800 (the Thor's 512.676): no per-draw vertex input (`VK_EXT_vertex_input_dynamic_state`), which drew skinned characters as long spikes; the vertex input goes into the pipeline instead (`vka_dev.c`, `DM_VK_VIN_DYN`). And no pipeline prewarm by default: each pipeline there appears to hold about 6 MB of driver memory outside the process (MemAvailable against the pipeline count, with the prewarm list's about 650 pipelines in the class showcase against about 320 without it), so it is off there (`DM_VK_PREWARM`). The static vertex input adds pipelines of its own.
- On that driver per-draw vertex input also draws rigid pieces drawn with a skinning shader (bone inputs fed by the zero buffer) as flat blue shapes, and map pieces with an attribute past its stream's stride as black shards. Keeping it for every other draw fixes the picture but saves only about a tenth of the pipelines (about 510 instead of 550 by the dungeon load). So the vertex input stays in the pipeline for every draw on that driver (`DM_VK_VIN_DYN` unset), and the stock driver stays not recommended for play.
