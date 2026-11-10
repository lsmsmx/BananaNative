#!/usr/bin/env python3
# A pack for the Android launcher (android/app) from a dumped game folder (the layout scripts/pack_dump.py reads: base/, updates/<version>/, dlc/).
# The pack holds what the host needs beside the game files and nothing the phone can read from the game folder itself:
#   <module>.patched.bin, <module>.fixups.tsv, <module>.gates.tsv and the rewriter's other tables, for every module of the profile, rewritten with patch_nso.py --target android
#   game.tsv     the profile the host was built with (for reference: the host has it compiled in)
#   pack.tsv     what the launcher reads: title ID, version, build IDs, and the RomFS and DLC folders relative to the game folder; the shader library's path in the pack
#   shaders/v-<DMSH_VK_CHAIN>/dmsh_vk.lib, inputs.tsv   the game's shaders as SPIR-V for the Vulkan replay (dm/gfx/tools/vk_android_build.py; android/SHADERS.md), the library
#                of this source tree's version: an up-to-date one in the shader cache is copied, else it is built from the Switch shader package
#   build_src.sha256, rewrite_<module>.log
# The RomFS is not in the pack (the app gives the host <game folder>/<romfs> as DM_ROMFS) and neither is anything else of the dump but the rewritten code.
# The game may be a NUP file instead of a folder (android/NUP.md): its packages are read in place (pack_dump.scan, dm/tools/nup.py), the modules and the RomFS are
# named `<name>.nup:update/<version>/...` (or `:base/...`), the DLC `<name>.nup:addon/<title ID>/romfs`, and pack.tsv's paths are relative to the folder holding the file.
# Writes <game folder>/../<title ID>-work/android-pack/ unless --out names another folder. A repeat run keeps modules whose inputs are unchanged.
# Needs Python with Capstone 5 and lz4 (pyproject.toml, dependency group android-pack). Packs are for the user's own phone: never commit or share one.
# The shader library: <work>/android-shaders/lib/v-<V>/ is reused when its inputs.tsv names this game and its profile's current inputs (a full build writes it last);
# otherwise it is built there from <work>/android-shaders/pkg (shader_split.py's output, as dmsh_build.py takes) by vk_android_build.py, which needs Python 3.14 (or
# backports.zstd), the .NET 10 runtime and spirv-val: --shader-python names that Python (default: the first of python3.14 / python3 on PATH that has zstd). About 10 minutes
# on DKB with -J 12. Without a package (the default folder or --shader-pkg) the dump is split into it first (split_shaders), and the split must hold every shader of the
# profile's shader_map.tsv; vk_android_build.py reads each shader from the package by its key, so a package of another split (base, update and DLC RomFS together) builds the
# same library and one that lacks a shader stops the build. --no-shaders leaves them out.
# --in-process (the phone's packer, android/ONDEVICE.md: an app may not start programs, and its process must not fork): the rewriter runs in this Python, one module after the other
# (runpy, with DM_REWRITE_J=1), and the shader library is not built here (it must be current in the cache, or --no-shaders).
# Each module is rewritten into <out>/.rewrite-<module>/ and moved into the pack when complete, its <module>.android.key last (a module without its key is
# unfinished: the launcher refuses it, the next run rewrites it). Libraries of other versions in the pack's shaders/ are kept.
# --code-only rewrites the modules and stops (pack.tsv, game.tsv and the shaders untouched); --keep-code packs without rewriting (the phone's two steps).
# Usage: pack_android.py <game folder | .nup file> [--version N] [--out DIR] [-j N] [--shader-cache DIR] [--shader-pkg DIR] [--shader-python PY] [-J N] [--no-shaders] [--code-only | --keep-code] [--in-process]   (exit 1 on error)
import argparse, hashlib, os, runpy, shutil, signal, subprocess, sys, time
from pathlib import Path
REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / 'scripts')); sys.path.insert(0, str(REPO / 'dm/games'))
import pack_dump
import game
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
import dmsh_chain

REWRITER = ('patch_nso.py', 'nso.py', 'nso_code.py', 'nso_asm.py', 'nso_match.py', 'sdk_patches.py', 'ue_cvar.py', 'ue_malloc.py', 'ue_region.py', 'loader_census.py')
FORMAT = 1   # pack.tsv format; the launcher refuses another


class Error(Exception): pass


WINDOWS = os.name == 'nt'


def write_lf(path, text):   # a text file with '\n' line ends in UTF-8 on every system (Windows' text mode writes CR LF in its ANSI code page): the launcher reads these bytes
    with open(path, 'w', encoding='utf-8', newline='\n') as f: f.write(text)


def child_env(**kw):
    """The environment of a tool this script starts: this one's and kw; on Windows also PYTHONUTF8=1, so a Python tool's text, its files and its output to the log or
    to this script's output, is UTF-8 there too (the ANSI code page has no '→' and stops the tool), and the environment's Library\\bin first on PATH (a conda
    environment keeps its programs there, spirv-val among them, and they are on PATH only when it is activated)"""
    if not WINDOWS: return dict(os.environ, **kw)
    env = dict(os.environ, PYTHONUTF8='1')
    lib = os.path.join(sys.prefix, 'Library', 'bin')
    if os.path.isdir(lib): env['PATH'] = lib + os.pathsep + env.get('PATH', '')
    return dict(env, **kw)


def windows_job():
    """Windows: this process and every process it starts from here on (the rewriter, the split, the shader build with its translator and spirv-val) in one job that
    the system ends as a whole when this process ends, however it ends. A GUI's stop is then Popen.terminate() on this process alone (TerminateProcess, which runs
    no finally: without the job its tools would go on). A pack stopped at any point is safe: each module is swapped in whole, its key last, and every other file goes
    in by rename. Best effort: where the job cannot be made, the tools are stopped as before (the finally of rewrite, subprocess.run on KeyboardInterrupt)"""
    import ctypes
    from ctypes import wintypes
    class Basic(ctypes.Structure):   # JOBOBJECT_BASIC_LIMIT_INFORMATION
        _fields_ = [('PerProcessUserTimeLimit', ctypes.c_int64), ('PerJobUserTimeLimit', ctypes.c_int64), ('LimitFlags', wintypes.DWORD), ('MinimumWorkingSetSize', ctypes.c_size_t),
                    ('MaximumWorkingSetSize', ctypes.c_size_t), ('ActiveProcessLimit', wintypes.DWORD), ('Affinity', ctypes.c_size_t), ('PriorityClass', wintypes.DWORD), ('SchedulingClass', wintypes.DWORD)]
    class Extended(ctypes.Structure):   # JOBOBJECT_EXTENDED_LIMIT_INFORMATION (its IO_COUNTERS as six 64-bit counts)
        _fields_ = [('Basic', Basic), ('Io', ctypes.c_uint64 * 6), ('ProcessMemoryLimit', ctypes.c_size_t), ('JobMemoryLimit', ctypes.c_size_t),
                    ('PeakProcessMemoryUsed', ctypes.c_size_t), ('PeakJobMemoryUsed', ctypes.c_size_t)]
    try:
        k = ctypes.WinDLL('kernel32', use_last_error=True)
        k.CreateJobObjectW.restype, k.CreateJobObjectW.argtypes = wintypes.HANDLE, [ctypes.c_void_p, wintypes.LPCWSTR]
        k.SetInformationJobObject.restype, k.SetInformationJobObject.argtypes = wintypes.BOOL, [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
        k.AssignProcessToJobObject.restype, k.AssignProcessToJobObject.argtypes = wintypes.BOOL, [wintypes.HANDLE, wintypes.HANDLE]
        k.GetCurrentProcess.restype = wintypes.HANDLE
        job = k.CreateJobObjectW(None, None)   # not inheritable: this process holds the only handle, which closes when it ends
        info = Extended(); info.Basic.LimitFlags = 0x2000   # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        if not job or not k.SetInformationJobObject(job, 9, ctypes.byref(info), ctypes.sizeof(info)) or not k.AssignProcessToJobObject(job, k.GetCurrentProcess()):   # 9: JobObjectExtendedLimitInformation
            print(f'the tools this packer starts are not in a job (Windows error {ctypes.get_last_error()}): a stop by TerminateProcess leaves them running', flush=True)
            return None
        return job   # kept open for this process's life
    except (OSError, AttributeError) as e:
        print(f'the tools this packer starts are not in a job ({e})', flush=True)
        return None


# The rewrite key, <module>.android.key: SHA-256 over name\0bytes of the REWRITER files, the module, the profile's patches.py (when there is one) and the part of the
# profile's game.tsv the rewriter reads: its build rows (patch_nso.py reads them for a module whose build has no CUTS), normalized by build_rows and hashed under the
# name ROWS. The rest of game.tsv is the host's (GPU timing, window size, ...): a change there must not make every pack's code stale.
# Packs made before (the v1 key) hash the whole game.tsv; one counts as current with this APK's game.tsv, or with the copy the pack keeps (<pack>/game.tsv,
# written after the rewrite) when the copy's build rows are the profile's: the key proves the copy is the file it was made with, the rows that the rewrite read
# the same. upgrade_keys rewrites such a key as the current one, before the pack's copy of game.tsv is replaced.
ROWS = 'game.tsv build rows'


def build_rows(b):
    """game.tsv's build rows as patch_nso.py reads them (rows split at tabs, each cell stripped, first cell `build`), one a line; bytes in, bytes out.
    A narrower strip and split than the rewriter's own: two tables that differ for it never come out the same here"""
    rows = []
    for row in b.split(b'\n'):
        c = [x.strip(b' \r\x0b\x0c') for x in row.split(b'\t')]
        if c[0] == b'build': rows.append(b'\t'.join(c) + b'\n')
    return b''.join(rows)


def part(h, name, b): h.update(name.encode() + b'\0' + b)


def rewrite_key(src, g):
    """The key a module rewritten now carries (the current formula, the current rewriter files)"""
    return rewrite_keys(src, g)[0]


# Earlier bytes of a REWRITER file whose rewrite output is the same as the current file's: dm/tools/rewriter_equiv/<file>.<commit> (that commit's version of the
# file). A file goes there ONLY when the rewrite output with it was verified byte-identical to the current one (each game's modules); a pack whose key was made
# with it then counts as up to date (rewrite_keys, the launcher's PackCheck.rewriteKeys), so a change that only saves memory or time does not redo every pack.
EQUIV = REPO / 'dm/tools/rewriter_equiv'


def equiv_files():
    """{REWRITER file: [earlier bytes, ...]} from EQUIV"""
    out = {}
    for p in sorted(EQUIV.glob('*.*')) if EQUIV.is_dir() else []:
        name = p.name.rsplit('.', 1)[0]
        if name in REWRITER: out.setdefault(name, []).append(p.read_bytes())
    return out


def rewrite_keys(src, g, pack=None):
    """Every key that counts as the current rewriter's, rewrite_key's first: for the current REWRITER files and each with earlier equivalent bytes (EQUIV) in
    their place, the current key (ROWS), then the v1 key with the profile's game.tsv, then the v1 key with pack/game.tsv when pack names a pack folder that has
    one whose build rows are the profile's (the same rule as the launcher's RewriteKey.keys)"""
    eq = equiv_files()
    variants = [[]]
    for f in REWRITER:
        cur = (REPO / 'dm/tools' / f).read_bytes()
        variants = [v + [(f, b)] for v in variants for b in [cur] + eq.get(f, [])]
    hs = []
    for v in variants:
        h = hashlib.sha256()
        for name, b in v: part(h, name, b)
        hs.append(h)
    for p in [src] + [p for p in (g.dir / 'patches.py',) if p.is_file()]:
        b = p.read_bytes()
        for h in hs: part(h, p.name, b)
    gt = (g.dir / 'game.tsv').read_bytes()
    tails = [(ROWS, build_rows(gt)), ('game.tsv', gt)]
    pg = pack / 'game.tsv' if pack is not None else None
    if pg is not None and pg.is_file() and (pb := pg.read_bytes()) != gt and build_rows(pb) == build_rows(gt): tails.append(('game.tsv', pb))
    out = []
    for name, b in tails:
        for h in hs:
            t = h.copy(); part(t, name, b); out.append(t.hexdigest() + ' target=android')
    return out


def upgrade_keys(srcs, out, g):
    """Each module's key that counts as current (rewrite_keys with the pack's game.tsv) but is not rewrite_key's is written as rewrite_key's (by rename), so the
    pack stays current when its copy of game.tsv is replaced. Other keys are left as they are"""
    for m, src in srcs.items():
        kf = out / f'{m}.android.key'
        if not kf.is_file(): continue
        have, keys = kf.read_text().strip(), rewrite_keys(src, g, out)
        if have in keys and have != keys[0]:
            tmp = out / f'{m}.android.key.tmp'; write_lf(tmp, keys[0] + '\n'); os.replace(tmp, kf)
            print(f'  {m}: key brought to the current formula (same rewrite)')


def nso_text_size(path):   # the module's code segment, decompressed (NSO header +0x18): what the rewriter's time follows
    with open(path, 'rb') as f: b = f.read(0x20)
    return int.from_bytes(b[0x18:0x1C], 'little') if b[:4] == b'NSO0' else 0


def nso_build_id(path):   # the module's build ID (NSO header +0x40), as the profile's build rows name it
    b = path.read_bytes()[:0x60]
    if b[:4] != b'NSO0': raise Error(f'{path}: not an NSO')
    return b[0x40:0x50].hex()


def unregistered(g, ids):
    """{module: the profile's builds of it} for each module of ids (module → NSO build ID) whose build no build row names; a module no row names is not listed.
    Every row counts (one row a version: an update's and the base's), not only the last"""
    known = {}
    for v in g.builds().values():
        for m, i in v.items():
            if m != 'display': known.setdefault(m, set()).add(i)
    return {m: sorted(known[m]) for m, i in ids.items() if known.get(m) and i not in known[m]}


def dump_modules(g, exe):
    """The profile's modules that a dump loads. All of them, unless the profile registers builds with different module sets (Mario Kart World: 1.8.0 has a subsdk1 that 1.0.0 lacks):
    then the modules of the build row the dump matches (the modules that row names are all there, with its build IDs, and the others are not)"""
    mods = g.words('modules'); have = [m for m in mods if (exe / m).is_file()]
    if len(have) == len(mods): return mods
    for row in g.builds().values():
        want = [m for m in mods if m in row]
        if want and want == have and all(nso_build_id(exe / m) == row[m] for m in want): return have
    return mods   # no row has this set of modules: the caller names what is missing


def optional_keys(g):
    """The keys of the profile's shader_map.tsv that only some of its registered builds have (shader_optional.tsv): a dump may lack them"""
    f = g.dir / 'shader_optional.tsv'
    return {l.split('\t')[0] for l in f.read_text().splitlines() if l and not l.startswith('#')} if f.is_file() else set()


def absent_optional(g, pkg):
    """The keys a split's <package>/_absent.tsv lists (split_shaders), when every one is in the profile's shader_optional.tsv; else (none, or a list naming other keys) empty"""
    f = pkg / '_absent.tsv'
    keys = {l for l in f.read_text().splitlines() if l} if f.is_file() else set()
    return keys if keys and keys <= optional_keys(g) else set()


def nso_zbic(path):   # whether the module's segments are ZBIC (NSO flag bit 7, system 22.0.0 on), which nso.py needs a decoder for (nso.zbic_decoder)
    b = path.read_bytes()[:0x10]   # read_bytes: a module inside a .nup is a path of its own kind, as in nso_build_id
    return b[:4] == b'NSO0' and bool(int.from_bytes(b[0xc:0x10], 'little') & 0x80)


class ProgressTee:
    """A tool's output stream in run_in_process: everything to the file f; its "progress <done> <total> ..." lines also to out (this script's own output, the
    step log Prepare reads), as "progress <n> 1000 code" with the tool's fraction placed in [base, base + width) of the step's 1000"""
    def __init__(self, f, out, base, width): self.f, self.out, self.base, self.width, self.part = f, out, base, width, ''
    def write(self, s):
        self.f.write(s)
        self.part += s
        while '\n' in self.part:
            line, self.part = self.part.split('\n', 1)
            w = line.split()
            if len(w) >= 3 and w[0] == 'progress' and w[1].isdigit() and w[2].isdigit() and int(w[2]):
                print(f'progress {self.base + self.width * min(int(w[1]), int(w[2])) // int(w[2])} 1000 code', file=self.out, flush=True)
        return len(s)
    def flush(self): self.f.flush()
    def isatty(self): return False


def run_in_process(script, args, env, log, progress=None):
    """script run as `python3 script args` would run it, in this process: its sys.argv, env added to os.environ, stdout and stderr to the file log;
    everything put back after it (the modules it imported from outside the standard library are dropped, so the next run starts afresh). → its exit code.
    progress: (base, width) to pass the tool's progress lines on to this script's output (ProgressTee)"""
    saved = (sys.argv, list(sys.path), dict(os.environ), set(sys.modules), sys.stdout, sys.stderr, os.getcwd())
    code = 0
    with open(log, 'w', buffering=1, encoding='utf-8', errors='backslashreplace') as f:
        sys.stdout = sys.stderr = ProgressTee(f, saved[4], *progress) if progress else f
        try:
            os.environ.update(env); sys.argv = [str(script)] + [str(a) for a in args]; sys.path.insert(0, str(Path(script).parent))
            runpy.run_path(str(script), run_name='__main__')
        except SystemExit as e:
            if e.code is None: code = 0
            elif isinstance(e.code, int): code = e.code
            else: print(e.code, file=f); code = 1
        except BaseException:
            import traceback; traceback.print_exc(); code = 1
        finally:
            argv, path, environ, mods, so, se, cwd = saved
            sys.argv, sys.path[:], sys.stdout, sys.stderr = argv, path, so, se
            os.environ.clear(); os.environ.update(environ); os.chdir(cwd)
            std = os.path.realpath(sys.base_prefix) + os.sep
            for m in set(sys.modules) - mods:
                if not os.path.realpath(getattr(sys.modules.get(m), '__file__', None) or '').startswith(std): sys.modules.pop(m, None)
    return code


def staging(out, m):   # where a module is rewritten before it goes into the pack (inside it: same file system, so the swap is renames)
    return out / f'.rewrite-{m}'


def swap_in(out, m, key):
    """A module's rewritten files from staging(out, m) into out, each by rename, then its key file, written last and also by rename. The old key went
    before the rewrite started, so a pack stopped at any point has either the old module with no key, a mix with no key, or the new module with its key:
    a module without its key is one the launcher refuses (PackCheck) and the next run rewrites"""
    st = staging(out, m)
    for f in sorted(st.iterdir()): os.replace(f, out / f.name)
    st.rmdir()
    tmp = out / f'{m}.android.key.tmp'; write_lf(tmp, key + '\n'); os.replace(tmp, out / f'{m}.android.key')


def rewriter_src(src, out, m):
    """The file patch_nso.py reads for module m: src itself, or for a module in a NUP package (`game.nup:update/<v>/exefs/main`) a copy in the pack's
    .nup-src/<module>/ under the module's own name. The rewriter then reads a plain file, so its files (REWRITER, rewrite_key) are the same for a NUP
    file and a folder, and a pack made from either keeps the other's rewritten modules"""
    if '.nup:' not in str(src): return src
    d = out / '.nup-src' / m; d.mkdir(parents=True, exist_ok=True)
    f = d / src.name; f.write_bytes(src.read_bytes()); return f


def rewrite(py, srcs, out, jobs, g, in_process=False):
    """patch_nso.py --target android on each module into out (each its own <module>.* files); a module whose key file matches is kept. Each module is
    rewritten into a staging folder and swapped in when it is complete (swap_in), so a run stopped part way never leaves new code with old tables.
    in_process: each in this process (run_in_process) with one worker, one module after the other"""
    env = child_env(DM_GAME=g.id, DM_REWRITE_J=str(max(1, (os.cpu_count() or 4) // max(1, len(srcs)))))
    todo = {}
    for m, src in srcs.items():
        keys = rewrite_keys(src, g, out); key = keys[0]; kf = out / f'{m}.android.key'
        if (out / f'{m}.patched.bin').is_file() and kf.is_file() and (have := kf.read_text().strip()) in keys:   # the current rewriter's key, an equivalent one's (EQUIV), an earlier formula's
            if have != key: tmp = out / f'{m}.android.key.tmp'; write_lf(tmp, key + '\n'); os.replace(tmp, kf)   # the same rewrite: its key as rewrite_key's (upgrade_keys)
            print(f'  {m}: unchanged, kept'); continue
        todo[m] = (src, key)
    for m in todo: (out / f'{m}.android.key').unlink(missing_ok=True)   # every module to rewrite is unfinished from here on: no mix of old and new modules passes
    if in_process:
        # Prepare's bar (the phone's rewrite step): each module a share of the step's 1000 by the size of its code, the rewriter's progress lines placed in it
        files = {m: rewriter_src(src, out, m) for m, (src, key) in todo.items()}
        size = {m: max(1, nso_text_size(f)) for m, f in files.items()}
        base, total = 0, sum(size.values())
        if todo: print('progress 0 1000 code', flush=True)
        for m, (src, key) in todo.items():
            kf = out / f'{m}.android.key'; kf.unlink(missing_ok=True)
            shutil.rmtree(staging(out, m), ignore_errors=True)
            print(f'  {m}: rewriting {src} (in process)', flush=True)
            t0 = time.monotonic()
            width = 1000 * size[m] // total
            rc = run_in_process(REPO / 'dm/tools/patch_nso.py', [str(files[m]), staging(out, m), m, '--target', 'android'], {'DM_GAME': g.id, 'DM_REWRITE_J': '1'}, out / f'rewrite_{m}.log', (base, width))
            if rc:
                tail = (out / f'rewrite_{m}.log').read_text(errors='replace').splitlines()[-20:]
                raise Error(f'patch_nso.py exit code {rc} on {m}:\n' + '\n'.join(tail))
            swap_in(out, m, key)
            base += width
            print(f'  {m}: done ({time.monotonic() - t0:.1f} s)', flush=True)
        if todo: print('progress 1000 1000 code', flush=True)
        return
    procs = {}
    try:
        pending = list(todo)
        while pending or procs:
            while pending and len(procs) < jobs:
                m = pending.pop(0); kf = out / f'{m}.android.key'; kf.unlink(missing_ok=True); shutil.rmtree(staging(out, m), ignore_errors=True)
                print(f'  {m}: rewriting {todo[m][0]}', flush=True)
                log = open(out / f'rewrite_{m}.log', 'w')
                procs[m] = (subprocess.Popen([py, str(REPO / 'dm/tools/patch_nso.py'), str(rewriter_src(todo[m][0], out, m)), str(staging(out, m)), m, '--target', 'android'], env=env, stdout=log, stderr=subprocess.STDOUT), log)
            for m in list(procs):
                p, log = procs[m]
                if p.poll() is None: continue
                log.close(); del procs[m]
                if p.returncode:
                    tail = (out / f'rewrite_{m}.log').read_text(errors='replace').splitlines()[-20:]
                    raise Error(f'patch_nso.py exit code {p.returncode} on {m}:\n' + '\n'.join(tail))
                swap_in(out, m, todo[m][1])
                print(f'  {m}: done', flush=True)
            if procs: time.sleep(0.2)
    finally:
        for p, log in procs.values():
            if p.poll() is None: p.terminate(); p.wait()
            log.close()


VK_BUILD = REPO / 'dm/gfx/tools/vk_android_build.py'
VK_BUILD_WIN = REPO / 'dm/gfx/tools/vk_android_win.py'   # vk_android_build.py with what Windows needs put around it (that file's header)
SPLIT = REPO / 'dm/gfx/tools/shader_split.py'
VK_LIB, VK_PREFIX = 'dmsh_vk.lib', 'v-'   # dmsh_abi_vk.h DMSH_VK_LIB_FILE / DMSH_VK_DIR_PREFIX


def vk_chain():   # DMSH_VK_CHAIN of this source tree, as the host is compiled with (dm/gfx/nvn/dmsh_chain_vk.h)
    import re
    m = re.search(r'#define DMSH_VK_CHAIN "([0-9a-f]+)"', (REPO / 'dm/gfx/nvn/dmsh_chain_vk.h').read_text())
    if not m: raise Error('dm/gfx/nvn/dmsh_chain_vk.h has no DMSH_VK_CHAIN')
    return m.group(1)


def shader_python(want, split=False):
    """A Python that can run vk_android_build.py (zstd: Python 3.14's compression.zstd or backports.zstd), or with split shader_split.py (zstd and lz4; this one first)"""
    if want: cands = [want]
    elif WINDOWS:   # python.exe is the name there; the python3 / python3.14 a bare Windows has are the Store's stubs (WindowsApps), which open the Store
        cands = [sys.executable] + [p for p in (shutil.which('python3.14'), shutil.which('python')) if p and 'windowsapps' not in p.lower()]
    else: cands = ([sys.executable] if split else []) + [shutil.which('python3.14'), shutil.which('python3'), sys.executable]
    test = 'import sys\ntry: from compression import zstd\nexcept ImportError: from backports import zstd' + ('\nimport lz4.block' if split else '')
    for py in filter(None, cands):
        r = subprocess.run([py, '-c', test], capture_output=True)
        if r.returncode == 0: return py
    raise Error(f'no Python with zstd{" and lz4" if split else ""} for {"shader_split.py" if split else "vk_android_build.py"} among {", ".join(filter(None, cands))}: '
                'use the environment of android/README.md or pass --shader-python')


def lib_stamp(lib_dir):
    """{'game', 'inputs'} of a library directory: its inputs.tsv, else the library's own header (game and inputs fields, dmsh_abi_vk.h), which a library copied
    without its inputs.tsv still carries"""
    st = dmsh_chain.read_stamp(str(lib_dir))
    if st: return st
    try: h = (lib_dir / VK_LIB).open('rb').read(72)
    except OSError: return {}
    return {'game': h[40:56].rstrip(b'\0').decode(errors='replace'), 'inputs': h[56:72].rstrip(b'\0').decode(errors='replace')} if len(h) == 72 else {}


def library_current(lib_dir, g):
    """True when lib_dir holds a finished full library of this game and its profile's current inputs"""
    st = lib_stamp(lib_dir)
    return (lib_dir / VK_LIB).is_file() and st.get('game') == g.id and st.get('inputs') == dmsh_chain.inputs(g)


def map_keys(path):
    """The keys of a shader_map.tsv (column 4 of its package rows)"""
    with open(path) as f: return {c[3] for c in (l.rstrip('\n').split('\t') for l in f) if len(c) > 4 and c[0].endswith('.bin')}


def split_shaders(a, g, vdir, pkg, parts=None):   # parts: the version's (romfs, exefs) when they are not vdir's own (pack_dump.version_part)
    """The Switch shader package from the dump, as the phone's split step makes it (the version's romfs and main, android/app PackService "split"): shader_split.py into
    <pkg> (written beside it, then renamed, so a stopped split leaves no half package). Its tables go beside the package. The library is the profile's (its shader_map.tsv
    decides what is built, and vk_android_build.py finds each of its shaders in the package by key), so the split must hold every shader of the profile's map: one that
    lacks any (a dump of another version, or incomplete) stops here"""
    rd, ed = parts or (vdir / 'romfs', vdir / 'exefs')
    py = shader_python(a.shader_python, split=True)
    tmp = pkg.with_name(pkg.name + '.tmp'); shutil.rmtree(tmp, ignore_errors=True); tmp.mkdir(parents=True)
    print(f'shaders: splitting the shaders out of {rd} into {pkg} with {py} (a few minutes)', flush=True)
    env = child_env(DM_GAME=g.id, DM_SPLIT_TARGETED='1')   # as the phone: only the files the profile's map names, when the build is the profile's and the tables come out the same
    r = subprocess.run([py, str(SPLIT), str(rd), str(ed / 'main'), str(tmp / 'pkg'),
                        *[str(tmp / f) for f in ('shader_map.tsv', 'shader_facts.tsv', 'shader_pairs.tsv', 'pso_pairs.tsv')]], env=env)
    if r.returncode: raise Error(f'shader_split.py exit code {r.returncode}')
    if (g.dir / 'shader_map.tsv').is_file():
        want, have = map_keys(g.dir / 'shader_map.tsv'), map_keys(tmp / 'shader_map.tsv')
        missing = sorted(want - have)
        if hard := [k for k in missing if k not in optional_keys(g)]:
            raise Error(f'the dump\'s shaders lack {len(hard)} of the {len(want)} in the profile\'s shader_map.tsv (e.g. {", ".join(hard[:3])}): the dump is of another version '
                        f'or incomplete, and the profile\'s shader library cannot be built from it (the split is in {tmp})')
        for f in ('shader_map.tsv', 'shader_facts.tsv', 'shader_pairs.tsv'):
            if (g.dir / f).is_file() and (g.dir / f).read_bytes() != (tmp / f).read_bytes():
                print(f'shaders: the dump\'s {f} differs from the profile\'s ({len(have - want)} shaders more); the library is the profile\'s, its shaders found in the package by key')
    shutil.rmtree(pkg, ignore_errors=True); (tmp / 'pkg').rename(pkg)
    absent = sorted(want - have) if (g.dir / 'shader_map.tsv').is_file() else []   # the keys only another registered build has (shader_optional.tsv): listed for a library build to leave out
    if absent:
        (pkg / '_absent.tsv').write_text(''.join(k + '\n' for k in absent))
        print(f'shaders: {len(absent)} of the profile\'s {len(want)} shaders are not in this dump (they belong to another registered build of the game, shader_optional.tsv): listed in {pkg.name}/_absent.tsv')
    for f in tmp.iterdir(): f.replace(pkg.with_name(f.name))
    tmp.rmdir()


def pack_shaders(a, g, work, out, vdir, parts=None):
    """shaders/v-<V>/ in the pack: an up-to-date library from the cache (built there first when it is missing or stale, from the shader package, split from the dump
    first when there is none and --shader-pkg names none). → the library path in the pack, or None"""
    v = vk_chain(); name = VK_PREFIX + v
    cache = (a.shader_cache or work / 'android-shaders' / 'lib').resolve(); lib = cache / name
    dst = out / 'shaders' / name
    if not library_current(lib, g):
        if a.in_process: raise Error(f'no current shader library {lib}: --in-process does not build one yet (vk_android_build.py starts the translator and spirv-val as programs); --no-shaders packs without')
        pkg = (a.shader_pkg or work / 'android-shaders' / 'pkg').resolve()
        if not pkg.is_dir(): split_shaders(a, g, vdir, pkg, parts)   # also into a --shader-pkg folder that is not there yet
        py = shader_python(a.shader_python)
        print(f'shaders: building the Vulkan library {lib} from {pkg} with {py} (J={a.J}; about 10 minutes on DKB)', flush=True)
        (work / 'android-shaders' / 'work').mkdir(parents=True, exist_ok=True)
        env = child_env(DM_GAME=g.id, J=str(a.J), WORK=str(work / 'android-shaders' / 'work'))
        if absent := absent_optional(g, pkg):   # a registered build without some of the map's shaders (shader_optional.tsv): the library is built without them
            print(f'shaders: {len(absent)} of the profile\'s shaders are not in this build of the game (shader_optional.tsv); the library leaves them out '
                  '(the [RED] "not in the package" line below counts them)', flush=True)
            env['DMSH_VK_REQUIRE_ALL'] = '0'
        r = subprocess.run([py, str(VK_BUILD_WIN if WINDOWS else VK_BUILD), '--target', 'vulkan', str(pkg), str(cache)], env=env)
        if r.returncode: raise Error(f'vk_android_build.py exit code {r.returncode} (a package that lacks the profile\'s shaders stops it: delete {pkg} and the split runs again)')
        if not library_current(lib, g): raise Error(f'vk_android_build.py finished but {lib} is not a full library of {g.id} with the current inputs')
    else: print(f'shaders: {lib} is current (game {g.id}, inputs {dmsh_chain.inputs(g)})')
    dst.mkdir(parents=True, exist_ok=True)
    if not (lib / dmsh_chain.STAMP).is_file():   # a library copied without its stamp: written from its header (library_current took it from there)
        st = lib_stamp(lib); dmsh_chain.write_stamp(str(lib), st['game'], st['inputs'])
        if WINDOWS: write_lf(lib / dmsh_chain.STAMP, (lib / dmsh_chain.STAMP).read_text(encoding='utf-8'))   # written in text mode: CR LF there
    for f in (VK_LIB, dmsh_chain.STAMP):
        s_, d_ = lib / f, dst / f
        if d_.is_file() and d_.stat().st_size == s_.stat().st_size and d_.stat().st_mtime_ns == s_.stat().st_mtime_ns: continue
        tmp = d_.with_name(f + '.partial'); shutil.copy2(s_, tmp); os.replace(tmp, d_)   # replace: Windows' rename does not take an existing name
    others = sorted(o.name for o in (out / 'shaders').iterdir() if o.name != name and o.name.startswith(VK_PREFIX) and o.is_dir())
    if others:   # kept: another APK (the test and the play app, an older build) may load one of them from this pack; delete by hand to save the space
        print(f'shaders: kept the libraries of other versions in {out / "shaders"}: {", ".join(others)} (about 350 MB each)')
    return dst / VK_LIB


def commit():   # the packer's commit; DM_PACKER_COMMIT when the repository's tool has none (the phone: no git there, the app passes the commit it was built from)
    try: r = subprocess.run(['git', '-C', str(REPO), 'rev-parse', '--short=12', 'HEAD'], capture_output=True, text=True).stdout.strip()
    except OSError: r = ''
    return r or os.environ.get('DM_PACKER_COMMIT') or 'unknown'


def main():
    ap = argparse.ArgumentParser(description='Pack a dumped game for the Android launcher: its code modules rewritten with --target android, no RomFS')
    ap.add_argument('game', type=Path, help='the game folder (base/exefs, base/romfs or an update\'s romfs, updates/<version>/, dlc/), or a .nup file')
    ap.add_argument('--version', type=int, help='the update version to pack (default: the newest in updates/; 0 = the base game)')
    ap.add_argument('--out', type=Path, help='the pack folder (default: <game folder>/../<title ID>-work/android-pack)')
    ap.add_argument('-j', type=int, default=2, help='modules rewritten at once (default 2)')
    ap.add_argument('--shader-cache', type=Path, help='the folder of Vulkan shader libraries v-<V>/ (default: <title ID>-work/android-shaders/lib)')
    ap.add_argument('--shader-pkg', type=Path, help='the Switch shader package to build the library from, split from the dump into it when it is not there (default: <title ID>-work/android-shaders/pkg)')
    ap.add_argument('--shader-python', help='the Python 3.14 (or one with backports.zstd) that runs vk_android_build.py')
    ap.add_argument('-J', type=int, default=8, help='shaders translated at once when the library is built (default 8)')
    ap.add_argument('--no-shaders', action='store_true', help='make the pack without the shader library')
    ap.add_argument('--code-only', action='store_true', help='only rewrite the modules (the phone\'s rewrite step): pack.tsv, game.tsv and the shaders stay as they are')
    ap.add_argument('--keep-code', action='store_true', help='do not rewrite: the pack\'s modules must be there with their key files (the phone\'s pack step)')
    ap.add_argument('--made-by', help='who makes this pack, written as its made_by row (the phone\'s pack step: "app <version>"; the launcher then updates the pack\'s code by itself when only the tools changed)')
    ap.add_argument('--in-process', action='store_true', help='run the rewriter in this process, no subprocesses (the phone\'s packer); the shader library must be current or left out')
    a = ap.parse_args()
    if WINDOWS:
        for s in (sys.stdout, sys.stderr): s.reconfigure(encoding='utf-8', errors='backslashreplace')   # as the tools' output (child_env): one encoding for the reader
        if not a.in_process: _job = windows_job()   # noqa: F841  (held until the process ends)
        signal.signal(signal.SIGBREAK, signal.default_int_handler)   # CTRL_BREAK_EVENT (a GUI that shares a console): stop as on Ctrl-C, the tools stopped in the finally blocks
    folder = a.game.resolve()
    try:
        try: import capstone, lz4.block   # noqa: F401  (patch_nso.py needs both; checked here so a missing one fails before any work)
        except ImportError as e: raise Error(f'{e.name} is missing in {sys.executable}: see android/README.md for the environment') from None
        if capstone.cs_version()[0] != 5: raise Error(f'Capstone {capstone.__version__}: the rewriter needs Capstone 5')
        app, vers, dlc = pack_dump.scan(folder)
        try: g = game.load(f'{app:016X}')
        except SystemExit: raise Error(f'no profile dm/games/{app:016X}: the Android pack needs one (build_ipa_from_nsp.py makes a provisional one)') from None
        ver = a.version if a.version is not None else max(vers, default=0)
        if ver and ver not in vers: raise Error(f'no updates/{ver} (have: {", ".join(map(str, sorted(vers))) or "none"})')
        vdir = pack_dump.version_dir(folder, vers, ver)
        exe, romfs = pack_dump.version_part(folder, vers, ver, 'exefs'), pack_dump.version_part(folder, vers, ver, 'romfs')   # its own, else the base's
        if not (exe / 'main.npdm').is_file() or not romfs.is_dir(): raise Error(f'{vdir}: no exefs/main.npdm and romfs (neither its own nor the base\'s)')
        if (pid := pack_dump.program_id(exe / 'main.npdm')) != app: raise Error(f'{vdir}: program ID {pid:016X}, the base is {app:016X}')
        mods = dump_modules(g, exe)
        srcs = {m: exe / m for m in mods}
        if missing := [m for m in mods if not srcs[m].is_file()]: raise Error(f'{exe}: no {", ".join(missing)} (the profile\'s modules are {" ".join(mods)})')
        if not a.keep_code and (zb := [m for m in mods if nso_zbic(srcs[m])]):
            sys.path.insert(0, str(REPO / 'dm/tools')); import nso   # its ZBIC decoder, checked here so a missing one fails before any work
            try: ok = nso.zbic_decoder()
            except nso.Error as e: raise Error(f'{" ".join(zb)}: ZBIC-compressed (system 22.0.0+): {e}') from None
            if not ok: raise Error(f'{" ".join(zb)}: ZBIC-compressed (system 22.0.0+), and {sys.executable} has no decoder: {nso.ZBIC_MISSING} (android/README.md)')
        out = (a.out or folder.parent / f'{app:016X}-work' / 'android-pack').resolve()
        try: out.relative_to(REPO); raise Error(f'{out} is inside the repository: packs never go there')
        except ValueError: pass
        out.mkdir(parents=True, exist_ok=True)
        ids = {m: nso_build_id(srcs[m]) for m in mods}
        print(f'{app:016X} {g.get("name", "?")} version {ver}: {vdir}', flush=True)
        for m, known in unregistered(g, ids).items(): print(f'[RED] {m} build {ids[m]} is none of the profile\'s builds ({", ".join(known)}): build-gated cuts do not apply')
        if a.keep_code:
            # each module needs its rewritten code and, written last by the rewrite, its key: a module without the key is a rewrite that did not finish (or one from before keys)
            gaps = [f'{m} ({" and ".join(f for f in (f"{m}.patched.bin", f"{m}.android.key") if not (out / f).is_file())} missing)'
                    for m in mods if not (out / f'{m}.patched.bin').is_file() or not (out / f'{m}.android.key').is_file()]
            if gaps: raise Error(f'--keep-code: the rewrite step has not finished for {", ".join(gaps)} in {out}: run the rewrite step again')
            print('  modules kept as they are (--keep-code)')
        else:
            try: rewrite(sys.executable, srcs, out, max(1, a.j), g, a.in_process)
            finally: shutil.rmtree(out / '.nup-src', ignore_errors=True)   # the module copies a NUP game's rewrite read (rewriter_src)
        if a.code_only: print(f'done: the modules in {out} (--code-only: pack.tsv and the shaders unchanged)'); return
        if a.keep_code: upgrade_keys(srcs, out, g)   # before the copy below replaces the game.tsv an earlier key was made with (a rewrite run did it in rewrite)
        shutil.copyfile(g.dir / 'game.tsv', out / 'game.tsv')
        shaders = None if a.no_shaders else pack_shaders(a, g, folder.parent / f'{app:016X}-work', out, vdir, (romfs, exe))
        top = folder.parent if pack_dump.nup.is_nup(folder) else folder   # a NUP file's paths are `<name>.nup:<package>/...`, relative to its folder
        rel = lambda p: p.relative_to(top).as_posix()
        rows = [('format', str(FORMAT)), ('title_id', f'{app:016X}'), ('name', g.get('name', '')), ('version', str(ver)), ('target', 'android'),
                ('modules', ' '.join(mods)), *[('build', f'{m}={ids[m]}') for m in mods], ('romfs', rel(romfs)),
                *[('aoc', f'{tid - app - 0x1000}\t{rel(p)}') for tid, p in sorted(dlc.items())],
                *([('shaders', shaders.relative_to(out).as_posix())] if shaders else []), ('packer', commit()), *([('made_by', a.made_by)] if a.made_by else [])]
        tmp = out / 'pack.tsv.partial'
        write_lf(tmp, ''.join(f'{k}\t{v}\n' for k, v in rows)); os.replace(tmp, out / 'pack.tsv')
        size = sum(f.stat().st_size for f in out.rglob('*') if f.is_file())
    except (Error, pack_dump.Error) as e: sys.exit(f'[RED] {e}')
    print(f'done: {out} ({size / 2**20:.1f} MiB); RomFS {rel(romfs)} and {len(dlc)} DLC stay in the game folder')


if __name__ == '__main__':
    main()
