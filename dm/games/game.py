#!/usr/bin/env python3
# Game profiles: dm/games/<base game title ID>/game.tsv (key <TAB> values..., lines starting with # are comments) + the shader tables (and patches.py, the rewriter's patches of the game's builds) in the same folder. The shader tools, the host build, iOS packaging and the NSP script take each game's constants from here.
# Usage: game.py list (list all profiles); game.py <id> <key> [<default>] (print that key's value, for shell; without a default a missing key is an error); game.py header <id> <out.h> (generate the host's game_profile.h);
#   game.py seed <id> <out.h> (the Android host's vk_prewarm_seed.h: the profile's vk_prewarm.bin, the pipeline keys a first start makes ahead, as bytes; dm/gfx/vk_android/vka_pwlist.h);
#   game.py dir <id> (print the profile folder); game.py link <id> <link> (point a symlink at the profile folder: make cannot take paths with spaces)
#   From Python: import game; g = game.load() (takes DM_GAME; there is no default game: unset is an error that lists the profiles); game.by_title(title ID); g.get / g.words / g.int / g.builds() / g.table(file name)
# DM_GAMES_DIR (folders separated by ':') adds profile folders outside the repository, searched after dm/games: the NSP script writes a provisional profile for a game without one into
#   <NSP folder>/<id>_build/profile/<id>/ and points DM_GAMES_DIR at <id>_build/profile, so the shader tools, the host build and build_ipa.sh all find it.
#   DM_GAMES_FIRST=1 searches DM_GAMES_DIR before dm/games (the NSP script's --provisional: a provisional profile for a game that has one in dm/games, to test the guesses)
import os, re, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent


class Game:
    def __init__(self, d):
        self.dir = Path(d).resolve(); self.id = self.dir.name; self.rows = []
        for line in (self.dir / 'game.tsv').read_text(encoding='utf-8').splitlines():
            if not line.strip() or line.lstrip().startswith('#'): continue
            c = line.split('\t'); self.rows.append((c[0].strip(), [v.strip() for v in c[1:]]))

    def values(self, key):   # every row of this key (keys such as build repeat)
        return [v for k, v in self.rows if k == key]

    def get(self, key, default=None):
        r = self.values(key)
        if r: return '\t'.join(r[0])
        if default is None: raise KeyError(f'{self.dir / "game.tsv"} has no key {key}')
        return default

    def words(self, key, default=None): return self.get(key, default).split()
    def int(self, key): return int(self.get(key), 0)
    def table(self, name): return self.dir / name

    @property
    def title_id(self): return int(self.get('title_id'), 16)

    def builds(self):
        """{version: {'display': display version, module name: build ID, ...}}"""
        out = {}
        for ver, disp, *mods in self.values('build'):
            out[ver] = dict([('display', disp)] + [tuple(m.split('=', 1)) for m in mods])
        return out

    def pairs(self, key):   # "a=b c=d" → [(a, b), (c, d)]
        return [tuple(w.split('=', 1)) for w in self.words(key)]


def extra_dirs():   # profile folders outside the repository (DM_GAMES_DIR), searched after dm/games
    return [Path(p) for p in os.environ.get('DM_GAMES_DIR', '').split(':') if p]


def search_dirs():   # dm/games, then DM_GAMES_DIR; DM_GAMES_FIRST=1 the other way round
    return extra_dirs() + [HERE] if os.environ.get('DM_GAMES_FIRST') else [HERE] + extra_dirs()


def find_dir(name):   # the profile folder of a title ID (the first of search_dirs that has it); a path is taken as is
    if os.sep in str(name): return Path(name)
    return next((d / name for d in search_dirs() if (d / name / 'game.tsv').exists()), HERE / name)


def load(name=None):
    name = name or os.environ.get('DM_GAME')
    if not name:   # there is no default game
        have = ', '.join(f'{g.id} ({g.get("name", "")})' for g in all_games())
        raise SystemExit(f'[RED] DM_GAME is not set: pick a game profile by base game title ID: {have}')
    d = find_dir(name)
    if not (d / 'game.tsv').exists():   # profile folders are named after the base game's title ID (the old names smbw / fe_engage are gone)
        have = ', '.join(f'{g.id} ({g.get("name", "")})' for g in all_games())
        raise SystemExit(f'[RED] no game profile {d / "game.tsv"} (DM_GAME / GAME take the base game title ID: {have})')
    return Game(d)


def all_games():
    out, seen = [], set()
    for root in search_dirs():
        if not root.is_dir(): continue
        for d in sorted(root.iterdir()):
            if (d / 'game.tsv').exists() and d.name not in seen: seen.add(d.name); out.append(Game(d))
    return out


def by_title(tid):
    return next((g for g in all_games() if g.title_id == tid), None)


def by_build(main_id):   # the profile whose build rows register this main build ID (a release of the same build under another title ID: the NSP script's same_build)
    return next((g for g in all_games() if any(b.get('main') == main_id for b in g.builds().values())), None)


def header(g):
    """the host's game_profile.h: the profile constants the host uses, as macros"""
    q = lambda s: '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'
    mods = g.words('modules')
    vers = ', '.join('{' + q(b['main']) + ', ' + q(b['display']) + '}' for b in g.builds().values() if 'main' in b)
    pi = g.get('present_interval', 'game')   # present interval: game = as the game sets it (as on the Switch), display = one frame per display tick
    if pi not in ('game', 'display'): sys.exit(f'{g.id}: present_interval must be game or display: {pi}')
    ft = g.get('fps_target', '0')   # frame rate cap of the rewriter's frame step (FRAME_STEP in the profile's patches.py: Tears of the Kingdom 1.4.3, Breath of the Wild 1.9.0) or of the engine's own frame limit (hle_fpslock.c): 0 = the game's own step, 30 / 60
    if ft not in ('0', '30', '60'): sys.exit(f'{g.id}: fps_target must be 0, 30 or 60: {ft}')
    fl = g.get('fps_limit', '0')   # Android: the host's frame limiter's default (hle_fpslock.c fps_limit_wait, a wait before each present): 0 = none, else at most that many frames a second; DM_FPS_LIMIT and the launcher's choice override it
    if not fl.isdigit() or int(fl) and not 10 <= int(fl) <= 60: sys.exit(f'{g.id}: fps_limit must be 0 (none) or 10 to 60 frames a second: {fl}')
    fh = g.get('fps_hold', 'on')   # whether the frame step may hold a frame longer than the cap's vsyncs when frames come late (hle_nvn.c fstep_tick): on / off
    if fh not in ('on', 'off'): sys.exit(f'{g.id}: fps_hold must be on or off: {fh}')
    utp = g.get('ue_texture_pool', '0')   # Unreal's r.Streaming.PoolSize in MB, raised to this on the Mac (hle_uecvar.c; DM_UE_TEXTURE_POOL overrides, and turns it on for iOS): 0 = the game's own pool
    if not utp.isdigit(): sys.exit(f"{g.id}: ue_texture_pool must be megabytes (0 = the game's own pool): {utp}")
    ufc = g.get('ue_fps_cvar', 't.MaxFPS')   # the Unreal console variable the host writes fps_target into (hle_fpslock.c; found by name by the rewriter, ue_cvar.py NAMES): a game's own frame limit its settings apply to t.MaxFPS
    if not re.fullmatch(r'[A-Za-z][A-Za-z0-9_.]{0,30}', ufc): sys.exit(f'{g.id}: ue_fps_cvar must be a console variable name: {ufc}')
    ps = g.get('present_snap', 'off')   # present snapshots by default (nvn_mtl.m mtl_snap_on; DM_PRESENT_SNAP and the iOS launcher's Frame buffering override it): on / off
    if ps not in ('on', 'off'): sys.exit(f'{g.id}: present_snap must be on or off: {ps}')
    p4 = g.get('pso_mtl4', 'off')   # render pipelines through MTL4Compiler on iOS by default, with the pipeline archive and hybrid pipelines (on by default on the Mac; DM_PSO_MTL4 overrides): on / off
    if p4 not in ('on', 'off'): sys.exit(f'{g.id}: pso_mtl4 must be on or off: {p4}')
    pg = g.words('pso_gpu', '')   # GPU architectures the app gets the pipeline archive made offline for (the NSP script's --gpu, build_ipa.sh): when pso_mtl4 is on the app gets these instead of the default g18p, the A19 / A19 Pro of the iPhone 17 family (e.g. 'applegpu_g19p' for a device of the family after it); unset → the default. Whitespace-separated tokens, with or without the applegpu_ prefix
    if pg and (len(pg) > 4 or any(w == 'none' or not re.fullmatch(r'(applegpu_)?[A-Za-z0-9]+', w) for w in pg)):
        sys.exit(f"{g.id}: pso_gpu takes 1-4 whitespace-separated GPU architectures such as 'g18p applegpu_g19p' (each an optional applegpu_ prefix then an alphanumeric token; there is no none: omit the key for g18p with pso_mtl4 on, or no archive with pso_mtl4 off): {pg}")
    et = g.get('enc_thread', 'off')   # Metal encoding on its own thread on the Mac by default (nvn_rec.h rec_on; on iOS it is on for every game; DM_NVN_ENC_THREAD overrides): on / off
    if et not in ('on', 'off'): sys.exit(f'{g.id}: enc_thread must be on or off: {et}')
    cp = g.get('core_preempt', 'off')   # same-core preemption approximation (hle_thread.c): on / off
    if cp not in ('on', 'off'): sys.exit(f'{g.id}: core_preempt must be on or off: {cp}')
    aba = g.get('alloc_big_align', 'off')   # nn::mem::StandardAllocator::Allocate aligns blocks of 64 KB or more to 64 KB (hle_os.c; Vanillaware's memory pool storage): on / off
    if aba not in ('on', 'off'): sys.exit(f'{g.id}: alloc_big_align must be on or off: {aba}')
    sf = g.get('shared_font', 'bfttf')   # what the shared-font address points at (hle_sdk16.c): bfttf = 8-byte header + TTF, ttf = the TTF alone
    if sf not in ('bfttf', 'ttf'): sys.exit(f'{g.id}: shared_font must be bfttf or ttf: {sf}')
    for rc in g.values('romfs_check'):   # a RomFS file line that tells the version's own data from another version's (the Android launcher's RomfsCheck: Prepare and the card): <path> <line> <expected | main>
        if len(rc) != 3 or not rc[0] or rc[0].startswith('/') or '..' in rc[0].split('/') or not rc[1].isdigit() or int(rc[1]) < 1 or not rc[2].strip() or any(c in ''.join(rc) for c in '|;'):
            sys.exit(f"{g.id}: romfs_check takes <path in the RomFS> <line, from 1> <expected line (no | or ;), or main for the version's main build ID>: {rc}")
    aw = g.get('android_window', '')   # the Android launcher's default Game resolution for this game when the player chose none (Prefs.gameWindow): one of Prefs.WINDOWS
    if aw and aw not in ('1920x1080', 'original', '1280x720'): sys.exit(f'{g.id}: android_window must be 1920x1080, original or 1280x720: {aw}')
    pl = g.get('pad_layout', 'simple')   # the virtual gamepad's first layout (DmGamepad.m; afterwards whatever the player switched to in game is kept): simple = 2D, full = 3D (D-pad, right stick, L3 / R3),
    if pl not in ('simple', 'full', 'touch'): sys.exit(f'{g.id}: pad_layout must be simple, full or touch: {pl}')   # touch = Tap (no control: every touch goes to the game's touch screen)
    sks = g.get('shader_key_skip', '')   # control-section bytes the shader keys hash as zeros, <offset>+<length> (hle_nvn.c keys_hash, shader_split.py key_ctl; Batman: Arkham Knight: GLSLC's compile information, 0x770+0x78)
    m = re.fullmatch(r'(0x[0-9a-fA-F]+|\d+)\+(0x[0-9a-fA-F]+|\d+)', sks) if sks else None
    if sks and (not m or not 0x30 <= int(m[1], 0) or not 0 < int(m[2], 0) or int(m[1], 0) + int(m[2], 0) > 0x880 or (int(m[1], 0) < 0x760 and int(m[1], 0) + int(m[2], 0) > 0x700)):   # not over +0x700..+0x75f, the code area and compute fields the tools read
        sys.exit(f'{g.id}: shader_key_skip takes <offset>+<length> within the first 0x880 bytes of the control section, past its header (+0x30) and clear of +0x700..+0x75f: {sks}')
    sko, skl = (int(m[1], 0), int(m[2], 0)) if m else (0, 0)
    rcd = g.get('render_cond', 'off')   # NVN conditional rendering evaluated at replay, timestamp reports with counter 0 (nvn_mtl.m OP_RENDER_COND): on / off
    if rcd not in ('on', 'off'): sys.exit(f'{g.id}: render_cond must be on or off: {rcd}')
    tfu = g.get('tex_flush_upload', 'off')   # textures written by the CPU into a pool and published with nvnMemoryPoolFlushMappedRange are uploaded like packaged ones (hle_nvn.c): on / off
    if tfu not in ('on', 'off'): sys.exit(f'{g.id}: tex_flush_upload must be on or off: {tfu}')
    tdu = g.get('tex_dirty_upload', 'off')   # with tex_flush_upload: a registered texture the CPU writes after init without any flush is uploaded too (dirty pages, hle_nvn.c nvn_ft_written): on / off
    if tdu not in ('on', 'off') or (tdu == 'on' and tfu != 'on'): sys.exit(f'{g.id}: tex_dirty_upload must be on or off, and needs tex_flush_upload on: {tdu}')
    tds = g.get('tex_dyn_slots', 'off')   # shaders whose texture handle the translator could not trace to one slot: slot indices in c2, the bound slots in the fallback arrays (nvn_mtl.m): on / off
    if tds not in ('on', 'off'): sys.exit(f'{g.id}: tex_dyn_slots must be on or off: {tds}')
    tsf = g.get('tex_soft', 'off')   # large block-compressed packaged textures kept without their finest level (Android: vka_tex.c "Soft textures", vka_soft.h; from plumber-racer-2's nvn_mtl.m tex_soft): on / off
    if tsf not in ('on', 'off'): sys.exit(f'{g.id}: tex_soft must be on or off: {tsf}')
    vcr = g.get('vconv_range', 'off')   # vertex attributes converted on the CPU only over the vertices each draw reads (nvn_mtl.m vc_draw_range): on / off
    if vcr not in ('on', 'off'): sys.exit(f'{g.id}: vconv_range must be on or off: {vcr}')
    mml = g.get('mem_map_limit', 'on')   # MapPhysicalMemory refused beyond the application pool, as on Horizon (hle_svc.c): on / off
    if mml not in ('on', 'off'): sys.exit(f'{g.id}: mem_map_limit must be on or off: {mml}')
    gts = g.get('gpu_timestamps', 'submission')   # counter timestamps (nvn_mtl.m rep_ticks): submission = one time per submission, segment = the GPU start / end of the segment holding the report
    if gts not in ('submission', 'segment'): sys.exit(f'{g.id}: gpu_timestamps must be submission or segment: {gts}')
    rsub = ' '.join(g.get('romfs_substitute', '').split())   # damaged RomFS files (a .zs that is not a zstd stream) served as another image file (hle_fs.c img_serve; DM_ROMFS_SUBST overrides): <path>=<substitute path> ...
    if any(w.count('=') != 1 or w.startswith(('=', '/')) or w.endswith('=') or '=/' in w for w in rsub.split()): sys.exit(f'{g.id}: romfs_substitute takes <path>=<substitute path> pairs, RomFS paths without a leading /: {rsub}')
    dbr = g.get('draw_barriers', 'full')   # how draw passes are ordered (nvn_mtl.m barrier_game): full = each waits for all earlier GPU work, game = by the game's own barriers, so vertex work overlaps the previous pass's fragments
    if dbr not in ('full', 'game'): sys.exit(f'{g.id}: draw_barriers must be full or game: {dbr}')
    om = g.get('op_mode', 'console')   # the operation mode reported at start (hle_oe.c dm_opmode; DM_OPMODE overrides): console = docked, handheld = touch screen in use (Super Mario Galaxy's pointer)
    if om not in ('console', 'handheld'): sys.exit(f'{g.id}: op_mode must be console or handheld: {om}')
    if pl == 'touch' and om != 'handheld': sys.exit(f'{g.id}: pad_layout touch needs op_mode handheld (the touch screen reaches the game only in handheld mode)')
    pm = g.get('perf_mode', 'follow')   # the performance mode reported (hle_oe.c): follow = the operation mode's (Boost docked, Normal handheld), boost = Boost always
    if pm not in ('follow', 'boost'): sys.exit(f'{g.id}: perf_mode must be follow or boost: {pm}')
    dmo = g.get('depth_mode', 'gl')   # the NVN depth mode the game sets, for the pipeline archive made offline (dm/gfx/tools/pso_offline.py; the host follows the game at run time): gl / zero
    if dmo not in ('gl', 'zero'): sys.exit(f'{g.id}: depth_mode must be gl or zero: {dmo}')
    wfn = []   # window size getters, one row per main build: <main build ID> <width fn> <height fn> <the game's own WxH> (loader.c window_apply; DM_WINDOW_SIZE=WxH sets the size, unset = the game's own)
    if g.get('window_fn', ''):
        for r in g.values('window_fn'):
            if len(r) != 4 or not re.fullmatch(r'[0-9a-f]{32}', r[0]) or not all(re.fullmatch(r'0x[0-9a-fA-F]+', x) for x in r[1:3]) or not re.fullmatch(r'\d{1,5}x\d{1,5}', r[3]):
                sys.exit(f"{g.id}: window_fn takes <main build ID (32 hex)> <width function offset> <height function offset> <the game's own WxH>: {r}")
            w, h = map(int, r[3].split('x'))
            if not (0 < w < 65536 and 0 < h < 65536): sys.exit(f'{g.id}: window_fn size must be 1..65535 each: {r[3]}')
            wfn.append('{' + q(r[0]) + f', {r[1]}, {r[2]}, {w}, {h}' + '}')
    # fps_decouple sead: the game's speed follows its measured frame rate through sead's frame-rate controller, which the rewriter gates in main only for a profile with this key (dm/tools/nso_code.py sead_find,
    # hle_fpslock.c sead_rate; pack_android.py build_rows keys it), and the Android launcher (build.gradle decoupleOf: a frame limit with presentation on the host's tick); a build's own place is an OBS row of its patches.py
    for r in g.values('fps_decouple') if g.get('fps_decouple', '') else []:
        if r != ['sead']: sys.exit(f"{g.id}: fps_decouple takes sead (the rewriter finds the controller; a build's own place is an OBS row of the profile's patches.py): {r}")
    # fps_steps: the game's framework runs a whole number of fixed steps each frame and presents at that many vsyncs; the frame rate cap holds 60, 30 or 20 through it (hle_fpslock.c
    # steps_tick). One row per main build: <main build ID> <global offset> <member offsets to the framework, comma-separated, or -> <mode field> <step count field>: the framework is at
    # [main + global], then [that + each member]
    fst = []
    for r in g.values('fps_steps') if g.get('fps_steps', '') else []:
        offs = r[2].split(',') if len(r) == 5 and r[2] != '-' else []
        if len(r) != 5 or not re.fullmatch(r'[0-9a-f]{32}', r[0]) or not all(re.fullmatch(r'0x[0-9a-fA-F]+', x) for x in [r[1], r[3], r[4], *offs]) or len(offs) > 3:
            sys.exit(f"{g.id}: fps_steps takes <main build ID (32 hex)> <global offset> <member offsets, comma-separated (at most 3), or -> <mode field> <step count field>: {r}")
        fst.append('{' + q(r[0]) + f', {r[1]}, {r[3]}, {r[4]}, {len(offs)}, ' + '{' + (', '.join(offs) or '0') + '}}')
    drs = []   # the dynamic resolution controller's places, one row per main build: <main build ID> <params probe> <pick> <budget load> <preset flag> (probe.c drs_install)
    for r in g.values('drs_ctl') if g.get('drs_ctl', '') else []:
        if len(r) != 5 or not re.fullmatch(r'[0-9a-f]{32}', r[0]) or not all(re.fullmatch(r'0x[0-9a-fA-F]+', x) for x in r[1:]):
            sys.exit(f"{g.id}: drs_ctl takes <main build ID (32 hex)> <parameter object probe> <pick> <budget load> <preset flag> (offsets): {r}")
        drs.append('{' + q(r[0]) + ', ' + ', '.join(r[1:]) + '}')
    # Game resolution by the Switch 2 graphics scale (the Android launcher, build.gradle resolutionOf; the host reads DM_GRAPHICS_SCALE and DM_OPMODE, nothing is generated):
    # <the launcher's choice: WxH or original> <scale 0|1|2> <console|handheld>, for a game that sizes its scenes by the scale rather than its window
    if g.get('graphics_scale', ''):
        seen = set()
        for r in g.values('graphics_scale'):
            if len(r) != 3 or not re.fullmatch(r'\d{1,5}x\d{1,5}|original', r[0]) or r[1] not in ('0', '1', '2') or r[2] not in ('console', 'handheld') or r[0] in seen:
                sys.exit(f'{g.id}: graphics_scale takes <WxH | original> <scale 0|1|2> <console | handheld>, one row per choice: {r}')
            seen.add(r[0])
        if wfn: sys.exit(f'{g.id}: graphics_scale and window_fn are two levers for one setting: keep one')
    # Dynamic resolution level lists, one row per main build: <main build ID> <size table offset> <levels> <list offset>:<length>... (loader.c levels_apply): for a game
    # with no window getter, DM_WINDOW_SIZE=WxH caps the render size by lowering the lists (Game resolution as with window_fn: build.gradle resolutionOf)
    rlv = []
    if g.get('res_levels', ''):
        for r in g.values('res_levels'):
            if len(r) < 4 or len(r) > 3 + 12 or not re.fullmatch(r'[0-9a-f]{32}', r[0]) or not re.fullmatch(r'0x[0-9a-fA-F]+', r[1]) or not re.fullmatch(r'\d{1,3}', r[2]) \
                    or not all(re.fullmatch(r'0x[0-9a-fA-F]+:\d{1,2}', x) for x in r[3:]):
                sys.exit(f"{g.id}: res_levels takes <main build ID (32 hex)> <size table offset> <levels> <list offset>:<length> (1 to 12 lists): {r}")
            n = int(r[2])
            if not 2 <= n <= 255: sys.exit(f'{g.id}: res_levels: the size table must have 2..255 levels: {r[2]}')
            ls = [(x.split(':')[0], int(x.split(':')[1])) for x in r[3:]]
            if not all(1 <= ln <= 32 for _, ln in ls): sys.exit(f'{g.id}: res_levels: each list is 1..32 bytes: {r}')
            ls += [('0', 0)] * (12 - len(ls))
            rlv.append('{' + q(r[0]) + f', {r[1]}, {n}, {len(r) - 3}, ' + '{' + ', '.join(f'{{{o}, {ln}}}' for o, ln in ls) + '}}')
        if g.get('graphics_scale', ''): sys.exit(f'{g.id}: graphics_scale and res_levels are two levers for one setting: keep one')
    # Depth-only mesh prepass draws skipped and their colour pass's EQUAL relaxed to LEQUAL on Qualcomm's driver (vka_draw.c mesh_cut; a workaround, open: rendering.md
    # "Depth-only mesh prepass drawn as spikes on Qualcomm's driver"): one row per pair, <prepass mesh shader key> <colour pass mesh shader key>, 16 hex digits each
    mpc = []
    for r in g.values('mesh_prepass_cut'):
        if len(r) != 2 or not all(re.fullmatch(r'[0-9a-f]{16}', x) for x in r) or r[0] == r[1]:
            sys.exit(f'{g.id}: mesh_prepass_cut takes <prepass mesh shader key> <colour pass mesh shader key>, two different keys of 16 lowercase hex digits: {r}')
        mpc.append(f'{{0x{r[0]}ull, 0x{r[1]}ull}}')
    if len(mpc) > 64: sys.exit(f'{g.id}: mesh_prepass_cut: at most 64 pairs')
    api = g.get('device_api', '55.15')   # the NVN API major.minor version nvnDeviceGetInteger reports (hle_nvn.c)
    if not re.fullmatch(r'\d+\.\d+', api): sys.exit(f'{g.id}: device_api must be major.minor: {api}')
    src = f'dm/games/{g.id}' if g.dir.parent == HERE else str(g.dir)
    L = [f'// Generated by dm/games/game.py from {src}/game.tsv; do not edit',
         '#pragma once',
         f'#define DM_GAME_ID {q(g.id)}',
         f'#define DM_GAME_NAME {q(g.get("name"))}',
         f'#define DM_GAME_PROGRAM_ID 0x{int(g.get("program_id"), 16):016X}ull',
         f'#define DM_GAME_MODULES {{{", ".join(map(q, mods))}}}',
         f'#define DM_GAME_NMOD {len(mods)}',
         f'#define DM_GAME_ROMFS_MARKER {q(g.get("romfs_marker"))}',
         f'#define DM_GAME_MEM_RESERVE {g.int("mem_reserve"):#x}ull',
         f'#define DM_GAME_MEM_ALIAS {g.int("mem_alias"):#x}ull',
         f'#define DM_GAME_MEM_STACK {g.int("mem_stack"):#x}ull',
         f'#define DM_GAME_APP_POOL {g.int("app_pool"):#x}ull',
         f'#define DM_GAME_MEM_USED {int(g.get("mem_used", "0x4000000"), 0):#x}ull',   # used memory reported beside the heap (hle_svc.c GetInfo UsedMemorySize)
         f'#define DM_GAME_SYS_RESOURCE {int(g.get("sys_resource", "0x2000000"), 0):#x}ull',   # the NPDM's system resource size (hle_svc.c GetInfo TotalSystemResourceSize; 0 = no virtual address memory)
         f'#define DM_GAME_ARGS {q(g.get("game_args", ""))}',   # game arguments when the command line gives none (main.c; Octopath Traveler 0: Unreal's -SwitchVirtualAddressSpaceSize=)
         f'#define DM_GAME_ENV {q(g.get("game_env", ""))}',   # KEY=VALUE pairs getenv answers (hle_os.c; Absolum: .NET's DOTNET_GCRegionRange)
         f'#define DM_GAME_FASTBOOT_ENV {q(g.get("fastboot_env", ""))}',   # KEY=VALUE host switches DM_FASTBOOT=1 sets for a test run (main.c fastboot_apply; the environment and env.txt win)
         f'#define DM_GAME_MEM_MAP_LIMIT {int(mml == "on")}',
         f'#define DM_GAME_STDALLOC_CAP {int(g.get("stdalloc_cap", "0"), 0):#x}ull',   # StandardAllocator's address space with virtual address memory (hle_svc.c); 0 = what the alias region has room for
         f'#define DM_GAME_DISPLAY_VERSIONS {vers}',
         f'#define DM_GAME_PRESENT_DISPLAY {int(pi == "display")}',
         f'#define DM_GAME_CORE_PREEMPT {int(cp == "on")}',
         f'#define DM_GAME_ALLOC_BIG_ALIGN {int(aba == "on")}',
         f'#define DM_GAME_FPS_TARGET {int(ft)}',
         f'#define DM_GAME_FPS_HOLD {int(fh == "on")}',
         f'#define DM_GAME_FPS_LIMIT {int(fl)}',
         f'#define DM_GAME_UE_TEXTURE_POOL {int(utp)}',
         f'#define DM_GAME_UE_FPS_CVAR {q(ufc)}',
         f'#define DM_GAME_PRESENT_SNAP {int(ps == "on")}',
         f'#define DM_GAME_PSO_MTL4 {int(p4 == "on")}',
         f'#define DM_GAME_ENC_THREAD {int(et == "on")}',
         *([f'#define DM_GAME_SHARED_FONT_TTF {int(sf == "ttf")}'] if g.get('shared_font', '') else []),   # without the key the host decides by the SDK version (hle_sdk16.c)
         f'#define DM_GAME_PAD_LAYOUT {("simple", "full", "touch").index(pl)}',
         f'#define DM_GAME_OPMODE {int(om == "console")}',
         f'#define DM_GAME_PERF_BOOST {int(pm == "boost")}',
         f'#define DM_GAME_TEX_FLUSH_UPLOAD {int(tfu == "on")}',
         f'#define DM_GAME_SHADER_KEY_SKIP_OFF {sko:#x}', f'#define DM_GAME_SHADER_KEY_SKIP_LEN {skl:#x}',
         f'#define DM_GAME_RENDER_COND {int(rcd == "on")}',
         f'#define DM_GAME_TEX_DIRTY_UPLOAD {int(tdu == "on")}',
         f'#define DM_GAME_TEX_DYN_SLOTS {int(tds == "on")}',
         f'#define DM_GAME_TEX_SOFT {int(tsf == "on")}',
         f'#define DM_GAME_GPU_TS_SEGMENT {int(gts == "segment")}',
         f'#define DM_GAME_DRAW_BARRIERS_GAME {int(dbr == "game")}',
         f'#define DM_GAME_VCONV_RANGE {int(vcr == "on")}',
         f'#define DM_GAME_ROMFS_SUBST {q(rsub)}',
         f'#define DM_GAME_WINDOW_FN {{{", ".join(wfn) or "{\"\", 0, 0, 0, 0}"}}}', f'#define DM_GAME_NWINDOW_FN {len(wfn)}',
         '#define DM_GAME_MESH_PREPASS_CUT {' + (', '.join(mpc) or '{0, 0}') + '}', f'#define DM_GAME_NMESH_PREPASS_CUT {len(mpc)}',

         '#define DM_GAME_FPS_STEPS {' + (', '.join(fst) or '{"", 0, 0, 0, 0, {0}}') + '}', f'#define DM_GAME_NFPS_STEPS {len(fst)}',
         f'#define DM_GAME_DRS_CTL {{{", ".join(drs) or "{\"\", 0, 0, 0, 0}"}}}', f'#define DM_GAME_NDRS_CTL {len(drs)}',
         '#define DM_GAME_RES_LEVELS {' + (', '.join(rlv) or '{"", 0, 0, 0, {{0, 0}}}') + '}', f'#define DM_GAME_NRES_LEVELS {len(rlv)}',
         *([f'#define DM_GAME_NVN_API_MAJOR {api.split(".")[0]}', f'#define DM_GAME_NVN_API_MINOR {api.split(".")[1]}'] if g.get('device_api', '') else []),   # without the key: by the SDK version (hle_nvn.c nvn_api)
         '']
    return '\n'.join(L)


SEED = 'vk_prewarm.bin'   # the profile's prewarm seed list (dm/gfx/vk_android/vka_pwlist.h; made by dm/tools/vk_prewarm.py)
def seed_header(g):   # vk_prewarm_seed.h: the seed's bytes for vka_pso.c (none: length 0)
    p = g.dir / SEED
    b = p.read_bytes() if p.is_file() else b''
    L = [f'// Generated by dm/games/game.py seed from {g.id}/{SEED} ({len(b)} bytes): the prewarm seed list (dm/gfx/vk_android/vka_pwlist.h). Do not edit', '#include <stddef.h>',
         f'static const unsigned char dm_vk_prewarm_seed[{max(len(b), 1)}] __attribute__((aligned(8))) = {{']
    for i in range(0, len(b), 32): L.append(''.join(f'{x},' for x in b[i:i + 32]))
    if not b: L.append('0')
    L += ['};', f'static const size_t dm_vk_prewarm_seed_len = {len(b)};', '']
    return '\n'.join(L)


def write_if_changed(out, text):   # written only when it changes: make runs this every time (the profile folder can change under the same GAME), and an unchanged header must not rebuild the host
    out = Path(out); out.parent.mkdir(parents=True, exist_ok=True)
    if not (out.exists() and out.read_text() == text):
        tmp = out.with_name(out.name + '.tmp'); tmp.write_text(text); tmp.replace(out)


if __name__ == '__main__':
    a = sys.argv[1:]
    if a == ['list']:
        for g in all_games(): print(f'{g.id}\t{g.title_id:016X}\t{g.get("name", "")}')
    elif len(a) == 3 and a[0] == 'header':
        write_if_changed(a[2], header(load(a[1])))
    elif len(a) == 3 and a[0] == 'seed':
        write_if_changed(a[2], seed_header(load(a[1])))
    elif len(a) == 2 and a[0] == 'dir':
        print(load(a[1]).dir)
    elif len(a) == 3 and a[0] == 'link':   # link → the profile folder (replaced when it points elsewhere)
        d, ln = load(a[1]).dir, Path(a[2]); ln.parent.mkdir(parents=True, exist_ok=True)
        if not (ln.is_symlink() and Path(os.readlink(ln)) == d):
            if ln.is_symlink() or ln.exists(): ln.unlink()
            ln.symlink_to(d)
    elif len(a) in (2, 3):
        print(load(a[0]).get(*a[1:]))
    else:
        sys.exit(__doc__ or 'usage: game.py list | game.py <id> <key> [<default>] | game.py header <id> <out.h> | game.py seed <id> <out.h> | game.py dir <id> | game.py link <id> <link>')
