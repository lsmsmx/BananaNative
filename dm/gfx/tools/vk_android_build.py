#!/usr/bin/env python3
# The Android shader library: original Switch shaders → SPIR-V for Vulkan 1.3 (Adreno 840), no SPIRV-Cross, no Metal. The front half is the Apple chain's, unchanged:
# the same translator (dmsh_xlate_bin, checked against dmsh_chain.lock), the same jobs (dmsh_build.py job_for and its helpers, taken from that file's source, so the
# control section reading stays one copy), the same answer table, plus the device answers only this build gives (ANSWERS: 64-lane subgroups, aligned storage buffer
# bindings, geometry passthrough by the shader). The back half is Vulkan's (android/SHADERS.md): the bindings renumbered into the Vulkan ABI
# (dm/gfx/nvn/dmsh_abi_vk.h), vertex outputs starting as (0, 0, 0, 1) plus the outputs the paired pixel shaders read, a GL-depth constant for the last pre-raster stages,
# the alpha test constant and the integer-output / dual-source modules of pixel shaders, the viewport mask of emulated passthrough kept in the shader,
# reflection from the SPIR-V, every module checked by spirv-val --target-env vulkan1.3, and one library file the replay (dm/gfx/vk_android) maps.
# This tool is not one of DMSH_CHAIN's files: the Apple libraries do not change with it. Its own version DMSH_VK_CHAIN (dm/gfx/nvn/dmsh_chain_vk.h) covers this file,
# vk_android_spv.py, dmsh_abi_vk.h and DMSH_CHAIN itself: it is their hash (the tools digest), or, when vk_android_same.tsv has a row for that digest, the earlier version
# whose libraries a full build with these tools reproduced byte for byte (a change that cannot change the output, such as the result cache below, then keeps every library
# already made valid). A row is added only after that comparison (android/SHADERS.md#versions); a change of output gets no row and moves the version.
# Usage: vk_android_build.py --target vulkan <Switch package dir> <output cache dir> [key...]   (no keys = all; output in <output cache dir>/v-<DMSH_VK_CHAIN>/)
#        vk_android_build.py --header    rewrite dm/gfx/nvn/dmsh_chain_vk.h (after changing any of the files above, or after DMSH_CHAIN changed; commit it)
#        vk_android_build.py --version   print DMSH_VK_CHAIN;  --digest   print the tools digest
#        vk_android_build.py --check <dmsh_vk.lib>   read a library back as the replay does and run spirv-val on every module it holds
# Environment: DM_GAME (required), XLATE (default the in-repo dmsh_xlate_bin), SPIRV_VAL (default spirv-val on PATH), J (concurrency, default 8), WORK (temporary folder),
#   DMSH_VK_LOOSE=1 also writes each module as spv/<key>[_gl].spv beside the library (for spirv-dis)
#   XLATE / SPIRV_VAL naming a shared library (.so / .dylib) call it in this process through ctypes instead of starting a program (the phone, android/ONDEVICE.md): XLATE the
#   translator's libdmsh_xlate (dmsh_xlate_android, export dmsh_xlate_main), SPIRV_VAL SPIRV-Tools' libSPIRV-Tools-shared (spvValidateWithOptions, Vulkan 1.3 environment)
#   DMSH_VK_RESULTS the result folder (default <output>/v-<V>/results), DMSH_VK_BATCH shaders per translator call (default 64), DMSH_VK_PROGRESS a file the progress is also
#   written to (JSON, replaced by rename), DMSH_VK_KEEP=0 deletes the result folder once the library is written, DMSH_VK_XLATE_J translator calls at once (default 1
#   for the library form, J for the program), DMSH_VK_REQUIRE_ALL=1 writes no library unless every shader built (exit 1; a key list's build too) but
#   those the profile's manifest of this version (vk_delta.tsv, a full Mac build) has as "-": every shader the Mac's build holds. A shader the package does not hold stops
#   the build (another game version's split) unless DMSH_VK_REQUIRE_ALL=0 (the phone's Try anyway): then it is left out and counted in one [RED] line (open_packs)
# Resumable: each shader's result (its modules, compressed, and its reflection) is one file <results>/<key>.r, written to a temporary name and renamed, stamped with
#   the tools digest, the profile inputs digest, the validator's version and the key; a rerun skips every key with a valid result (a killed build resumes; a later build of the same version is a cache hit)
#   and the library is written from the result files. Pixel shaders go first; the vertex shaders' padding reads their inputs from the pixel shaders' results.
#   Progress lines on stdout: "progress <done> <total> <stage>" (stage translate, library, done); the total is the shader count from the profile's shader_map.tsv, known
#   before any translation
# The package: each shader is read from the record the profile's shader_map.tsv names, once its hash (shader_split.py skey) is checked to be the key; a record that is not
#   (a package of another split: the map's numbering depends on what was scanned, base, update and DLC RomFS) is looked up by its key in the package's files of its stage,
#   and a key the package does not hold stops the build before any translation (locate)
# Output: dmsh_vk.lib (the library), index.json (the same reflection as JSON, for tools), inputs.tsv (game and profile inputs, as the Apple libraries' stamp),
#   _vk_build.json (each key's status), and a summary on stdout: counts, failure reasons, what the device must support
import ast, collections, concurrent.futures as cf, ctypes, hashlib, json, mmap, os, re, shutil, struct, subprocess, sys, tempfile, threading, time
HERE = os.path.dirname(os.path.abspath(__file__)); GFX = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import dmsh_chain, dmsh_abi as abi, dmsh_ampere, tool_lock, vk_android_spv as spv, vk_android_delta as delta

try: from compression import zstd   # Python 3.14; before it, pip install backports.zstd (shader_split.py needs it too)
except ImportError:
    try: from backports import zstd   # pyright: ignore[reportMissingImports]
    except ImportError: zstd = None
ABI_VK = os.path.join(GFX, 'nvn', 'dmsh_abi_vk.h'); HEADER_VK = os.path.join(GFX, 'nvn', 'dmsh_chain_vk.h')
VK_FILES = ['tools/vk_android_build.py', 'tools/vk_android_spv.py', 'tools/vk_android_delta.py', 'nvn/dmsh_abi_vk.h']
SAME_TSV = os.path.join(HERE, 'vk_android_same.tsv')
vk_values = delta.vk_values


VK = vk_values()


def tools_digest():
    """The hash of the files that make the library: DMSH_CHAIN and VK_FILES. A library version is a tools digest; see vk_version"""
    h = hashlib.sha256(b'dmsh_chain\0' + dmsh_chain.version().encode() + b'\0')
    for f in VK_FILES: h.update(f.encode() + b'\0' + open(os.path.join(GFX, f), 'rb').read() + b'\0')
    return h.hexdigest()[:12]


def same_rows():
    """{tools digest: library version} from vk_android_same.tsv: tool changes whose libraries were shown byte-identical to an earlier version's (a full build compared)"""
    rows = {}
    for line in open(SAME_TSV):
        if line.startswith('#') or not line.strip(): continue
        digest, version = line.rstrip('\n').split('\t')[:2]
        rows[digest] = version
    return rows


def vk_version():
    """DMSH_VK_CHAIN, the library version: the tools digest, or the earlier version vk_android_same.tsv says these tools' libraries are byte-identical to (so a change that
    does not change the output, a build cache say, keeps the libraries already made valid)"""
    d = tools_digest()
    return same_rows().get(d, d)


def header_text(v, a):
    return ('// Generated file (dm/gfx/tools/vk_android_build.py --header): the Android Vulkan shader library version this source tree expects. It covers DMSH_CHAIN\n'
            '// (the translator front half), vk_android_build.py, vk_android_spv.py, vk_android_delta.py and dmsh_abi_vk.h; the replay loads <library dir>/v-<V>/dmsh_vk.lib.\n'
            '// DMSH_VK_ABI: what the replay reads of a library (vk_android_delta.py abi_digest); a library of another version with this ABI is played while its update runs\n'
            f'#define DMSH_VK_CHAIN "{v}"\n'
            f'#define DMSH_VK_ABI "{a}"\n')


def committed_version(name='DMSH_VK_CHAIN'):
    m = re.search(r'#define ' + name + r' "([0-9a-f]+)"', open(HEADER_VK).read()) if os.path.exists(HEADER_VK) else None
    return m.group(1) if m else None


def is_lib(path): return path.endswith(('.so', '.dylib'))


class Done:   # what translate() returns, as subprocess.run's result: the exit code and the translator's output ([RED] <key>: ... lines name the shaders that failed)
    def __init__(self, returncode, stdout, stderr): self.returncode, self.stdout, self.stderr = returncode, stdout, stderr


class XlateLib:
    """The translator as a shared library (dmsh_xlate_android: libdmsh_xlate.so on the phone, libdmsh_xlate.dylib on the Mac): dmsh_xlate_main(argc, argv, log) runs the program's Main
    with the program's arguments in this process, its output (standard output and error) in the file log. ctypes drops the GIL for the call, so the worker threads translate in parallel"""
    def __init__(self, path):
        self.fn = ctypes.CDLL(path).dmsh_xlate_main
        self.fn.argtypes, self.fn.restype = [ctypes.c_int, ctypes.POINTER(ctypes.c_char_p), ctypes.c_char_p], ctypes.c_int

    def __call__(self, args, log):
        argv = (ctypes.c_char_p * len(args))(*[a.encode() for a in args])
        rc = self.fn(len(args), argv, log.encode())
        try:
            text = open(log, errors='replace').read(); os.unlink(log)
        except OSError: text = ''
        return Done(rc, text, text)


class SpirvValLib:
    """SPIRV-Tools' validator as a shared library (libSPIRV-Tools-shared): spvValidateWithOptions with spirv-val's default options in the Vulkan 1.3 environment, the check
    spirv-val --target-env vulkan1.3 makes. A context and an options object per thread; ctypes drops the GIL for the call"""
    class Binary(ctypes.Structure): _fields_ = [('code', ctypes.POINTER(ctypes.c_uint32)), ('wordCount', ctypes.c_size_t)]

    class Diagnostic(ctypes.Structure): _fields_ = [('line', ctypes.c_size_t), ('column', ctypes.c_size_t), ('index', ctypes.c_size_t), ('error', ctypes.c_char_p), ('isTextSource', ctypes.c_bool)]

    def __init__(self, path):
        L = self.lib = ctypes.CDLL(path)
        L.spvParseTargetEnv.argtypes, L.spvParseTargetEnv.restype = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_int)], ctypes.c_bool
        L.spvContextCreate.argtypes, L.spvContextCreate.restype = [ctypes.c_int], ctypes.c_void_p
        L.spvValidatorOptionsCreate.argtypes, L.spvValidatorOptionsCreate.restype = [], ctypes.c_void_p
        L.spvValidateWithOptions.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(self.Binary), ctypes.POINTER(ctypes.POINTER(self.Diagnostic))]
        L.spvValidateWithOptions.restype = ctypes.c_int
        L.spvDiagnosticDestroy.argtypes, L.spvDiagnosticDestroy.restype = [ctypes.POINTER(self.Diagnostic)], None
        env = ctypes.c_int()
        if not L.spvParseTargetEnv(b'vulkan1.3', ctypes.byref(env)): raise OSError(f'{path}: no vulkan1.3 target environment')
        self.env, self.tls = env.value, threading.local()
        L.spvSoftwareVersionDetailsString.argtypes, L.spvSoftwareVersionDetailsString.restype = [], ctypes.c_char_p
        self.version = L.spvSoftwareVersionDetailsString().decode('utf-8', 'replace').strip()   # as spirv-val --version's first line

    def __call__(self, data):
        """SPIR-V bytes → None when valid, else the first line of the error"""
        t = self.tls
        if not hasattr(t, 'ctx'): t.ctx, t.opt = self.lib.spvContextCreate(self.env), self.lib.spvValidatorOptionsCreate()
        words = (ctypes.c_uint32 * (len(data) // 4)).from_buffer_copy(data)
        diag = ctypes.POINTER(self.Diagnostic)()
        r = self.lib.spvValidateWithOptions(t.ctx, t.opt, ctypes.byref(self.Binary(words, len(data) // 4)), ctypes.byref(diag))
        msg = None
        if r != 0: msg = (f'error: line {diag.contents.index}: ' + (diag.contents.error or b'').decode('utf-8', 'replace')) if diag else f'spvValidateWithOptions returned {r}'
        if diag: self.lib.spvDiagnosticDestroy(diag)
        return (msg.splitlines() or ['spirv-val failed'])[0] if msg is not None else None


def spirv_validator(path):
    """→ a function SPIR-V bytes → None when valid for Vulkan 1.3, else the first error line: SPIRV-Tools in this process (a .so / .dylib), else spirv-val as a program"""
    if is_lib(path): return SpirvValLib(path)
    def run(data):
        with tempfile.NamedTemporaryFile(suffix='.spv', dir=os.environ.get('WORK')) as f:
            f.write(data); f.flush()
            r = subprocess.run([path, '--target-env', 'vulkan1.3', f.name], capture_output=True, text=True)
        return None if r.returncode == 0 else ((r.stderr.strip() or r.stdout.strip() or f'exit {r.returncode}').splitlines() or ['spirv-val failed'])[0]
    run.version = (subprocess.run([path, '--version'], capture_output=True, text=True).stdout.strip().splitlines() or ['unknown'])[0]
    return run


def check_translator(xlate=None):
    """The translator half of tool_lock.check_tools: the dlls and the embedded source digest (the Metal compilers are not used here). The library form (an XlateLib: dmsh_xlate_android,
    NativeAOT) has no dlls to fingerprint: its embedded Program.cs digest must be the lock's and the repository's, and its embedded source tree digest (xlate_tree.py) this tree's"""
    want = dict(l.rstrip('\n').split('\t', 1) for l in open(dmsh_chain.LOCK) if '\t' in l and not l.startswith('#'))
    if xlate is not None:
        with tempfile.TemporaryDirectory(dir=os.environ.get('WORK')) as d:
            r = xlate(['--source-sha'], os.path.join(d, 'log.txt'))
            t = xlate(['--tree-sha'], os.path.join(d, 'tree.txt'))
        emb, tree = r.stdout.strip(), t.stdout.strip()
        import xlate_tree   # the whole source tree it was built from: Program.cs alone let a library from before a Ryujinx change pass
        bad = []
        if not (r.returncode == 0 and emb == want.get('xlate_source') == dmsh_chain.xlate_source_sha()):
            bad.append(f'translator library: embedded source {emb or "none, exit code " + str(r.returncode)} / lock {want.get("xlate_source")} / in repo {dmsh_chain.xlate_source_sha()}')
        if not (t.returncode == 0 and tree == xlate_tree.sha()):
            got = tree if t.returncode == 0 and tree else f'unknown (no --tree-sha: exit code {t.returncode}, a library from before it)'
            bad.append(f'translator library: built from source tree {got}, this tree is {xlate_tree.sha()} (dmsh_xlate_android/build.sh builds it again)')
        return bad
    bad = [f'{dll}: lock {want.get(dll)} / actual {tool_lock.sha16(os.path.join(dmsh_chain.XLATE_DIR, dll))}' for dll in ('dmsh_xlate.dll', 'Ryujinx.Graphics.Shader.dll', 'Ryujinx.Common.dll')
           if tool_lock.sha16(os.path.join(dmsh_chain.XLATE_DIR, dll)) != want.get(dll)]
    emb = tool_lock.xlate_embedded()
    if emb.startswith(tool_lock.NO_RUN): bad.append(tool_lock.no_run_message(emb))
    elif emb != want.get('xlate_source') or emb != dmsh_chain.xlate_source_sha(): bad.append(f'translator source: embedded {emb} / lock {want.get("xlate_source")} / in repo {dmsh_chain.xlate_source_sha()}')
    return bad


# dmsh_build.py runs as a script; the functions that read packages and control sections and build the translator's jobs are taken from its source by name, so there is one copy
BUILD = os.path.join(HERE, 'dmsh_build.py')
BORROWED = ('record_index', 'vertex_ab', 'early_fragment_tests', 'tf_layout', 'job_for', 'decode_tex', 'const_sampler', 'decode_ubo_tex', 'c2_array', 'load_pairs', 'build_keys')


def borrow(namespace):
    tree = ast.parse(open(BUILD).read(), BUILD)
    fns = {n.name: n for n in tree.body if isinstance(n, ast.FunctionDef)}
    missing = [n for n in BORROWED if n not in fns]
    if missing: sys.exit(f'[RED] {BUILD} no longer defines {", ".join(missing)}: vk_android_build.py takes them from there')
    exec(compile(ast.Module(body=[fns[n] for n in BORROWED], type_ignores=[]), BUILD, 'exec'), namespace)
    return namespace


# The shader key of a package record, as shader_split.py makes it (and the host's keys_hash): taken from that file's source by name, so the key stays one copy
SPLIT = os.path.join(HERE, 'shader_split.py')
KEY_FNS, KEY_CONSTS = ('nvn2_ctl', 'ctl_len', 'key_ctl', 'skey'), ('NVN2_KEY_CTL', 'CTL_MAGIC')


def borrow_key(key_skip):
    """skey(ctl, code) of shader_split.py, with the profile's shader_key_skip (its KEY_SKIP)"""
    tree = ast.parse(open(SPLIT).read(), SPLIT)
    fns = {n.name: n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in KEY_FNS}
    consts = [n for n in tree.body if isinstance(n, ast.Assign) and any(isinstance(t, (ast.Name, ast.Tuple)) and
              {e.id for e in (t.elts if isinstance(t, ast.Tuple) else [t]) if isinstance(e, ast.Name)} & set(KEY_CONSTS) for t in n.targets)]
    if len(fns) != len(KEY_FNS): sys.exit(f'[RED] {SPLIT} no longer defines {", ".join(sorted(set(KEY_FNS) - set(fns)))}: vk_android_build.py takes them from there')
    ns = {'struct': struct, 'hashlib': hashlib, 'KEY_SKIP': key_skip}
    exec(compile(ast.Module(body=consts + [fns[n] for n in KEY_FNS], type_ignores=[]), SPLIT, 'exec'), ns)
    return ns['skey']


def record_key(rec, skey):
    """The key of one package record ('BTCDBTCD', +0x28 control length, +0x2c code length, from +0x38 the code area then the control section: shader_split.py), or None
    when the record is not one, or holds no shader: a selected split's empty record (shader_split.py EMPTY_RECORD, DM_SPLIT_SELECTED) has no control section, and a
    section shorter than 0x24 bytes does not hold its own length (+0x20), which the key reads"""
    if len(rec) < 0x38 or rec[:8] != b'BTCDBTCD': return None
    tl, cl = struct.unpack_from('<2I', rec, 0x28)
    if tl < 0x24 or 0x38 + cl + tl > len(rec): return None
    return skey(rec[0x38 + cl:0x38 + cl + tl], rec[0x38:0x38 + cl])


def locate(keys, loc, files, skey):
    """Each key's record in the package, found by its hash. The map's (file, record) is where the split that made the profile's shader_map.tsv put it, and another split
    numbers its records otherwise: the map depends on what was scanned (base, update and DLC RomFS), so a package from another split gives every shader another shader's
    code when read by the map's numbers. files: {file name: [records]} of the package (a record = bytes, or a callable giving them). → ({key: (file, record)}, [problems]):
    a key whose record is at the map's place is kept there; otherwise it is taken from any package file of its stage that holds it; a key no file of its stage holds is a problem"""
    want = {k: loc[k] for k in keys}
    found, bad, index = {}, [], None
    for k, (name, i) in want.items():
        recs = files.get(name)
        if recs is not None and 0 <= i < len(recs):
            r = recs[i]
            if record_key(r() if callable(r) else r, skey) == k: found[k] = (name, i); continue
        if index is None:   # the whole package by key, once: only when a record is not where the map says
            index = {}
            for n in sorted(files):
                for j, r in enumerate(files[n]):
                    rk = record_key(r() if callable(r) else r, skey)
                    if rk: index.setdefault((n[:2], rk), (n, j))
        hit = index.get((name[:2], k))
        if hit: found[k] = hit
        else: bad.append(f'{k}: not in the package (the map has it as {name}#{i})')
    return found, bad


def check_library(path, validator, nj):
    """Read a library back as the replay does (dmsh_abi_vk.h): the header, entries sorted by key, every binding and module in range, each module decompressed to SPIR-V of
    its recorded size and valid for spirv-val --target-env vulkan1.3. → [problems]"""
    with open(path, 'rb') as f: d = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)   # mapped as the replay maps it: a large game's library is several GB
    bad = []
    magic, version, chain, vkc, game, inputs, ne, eo, nb, bo, nm, mo, blob_off, blob_size, sg, sa = struct.unpack_from('<II16s16s16s16sIIIIIIQQHH', d, 0)
    if magic != VK['LIB_MAGIC'] or version != VK['LIB_VERSION']: return [f'magic {magic:#x} version {version}']
    if blob_off + blob_size != len(d): bad.append('blob does not end the file')
    keys = [struct.unpack_from('<Q', d, eo + VK['ENTRY_SIZE'] * i)[0] for i in range(ne)]
    if keys != sorted(set(keys)): bad.append('entries not sorted by key, or a key twice')
    jobs_ = []
    for i in range(ne):
        key, _, stage, mc, flags, m0, b0, bc = struct.unpack_from('<QQBBHIIH', d, eo + VK['ENTRY_SIZE'] * i)
        if m0 + mc > nm or b0 + bc > nb or stage > VK['STAGE_MS']: bad.append(f'{key:016x}: entry out of range'); continue
        _e = d[eo + VK['ENTRY_SIZE'] * i:eo + VK['ENTRY_SIZE'] * (i + 1)]
        if delta.c1_off(_e) + struct.unpack_from('<I', _e, 48)[0] > blob_size: bad.append(f'{key:016x}: c1 bytes out of the blob')
        vs = [struct.unpack_from('<IIIBBH', d, mo + VK['MOD_SIZE'] * j)[3] for j in range(m0, m0 + mc)]
        if not vs or vs[0] != VK['MOD_BASE'] or len(set(vs)) != len(vs): bad.append(f'{key:016x}: modules {vs}: the base module first, each variant once')
        for j in range(m0, m0 + mc):
            off, size, stored, variant, codec, off_hi = struct.unpack_from('<IIIBBH', d, mo + VK['MOD_SIZE'] * j)
            off = delta.mod_off(off, off_hi)
            if off + stored > blob_size: bad.append(f'{key:016x}: module out of the blob'); continue
            jobs_.append((f'{key:016x}_{variant}', blob_off + off, stored, size, codec))
    def one(job):
        tag, at, n, size, codec = job
        raw = d[at:at + n]   # read when checked, not all at once
        if codec == VK['CODEC_ZSTD']:
            if zstd is None: return f'{tag}: zstd module and no zstd module in this Python'
            raw = zstd.decompress(raw)
        elif codec != VK['CODEC_RAW']: return f'{tag}: codec {codec}'
        if len(raw) != size or raw[:4] != struct.pack('<I', spv.MAGIC): return f'{tag}: not SPIR-V of {size} bytes'
        err = validator(raw)
        return None if err is None else f'{tag}: ' + err[:160]
    with cf.ThreadPoolExecutor(nj) as ex: bad += [x for x in ex.map(one, jobs_) if x]
    print(f'{path}: {ne} shaders, {len(jobs_)} modules, game {game.rstrip(bytes(1)).decode()}, chain {chain.rstrip(bytes(1)).decode()}, vk chain {vkc.rstrip(bytes(1)).decode()}'
          + f', subgroup size {sg or 32}, storage buffer alignment {sa or "exact"}, modules by variant ' + ', '.join(f'{v} {n}' for v, n in sorted(collections.Counter(int(t.split('_')[1]) for t, *_ in jobs_).items())) + ': '
          + (f'{len(bad)} problems' if bad else 'every module decompresses and passes spirv-val --target-env vulkan1.3'))
    return bad


if __name__ != '__main__': raise ImportError('vk_android_build.py is a script')
argv = sys.argv[1:]
if len(argv) == 2 and argv[0] == '--check':
    _bad = check_library(argv[1], spirv_validator(os.environ.get('SPIRV_VAL') or shutil.which('spirv-val') or sys.exit('[RED] no spirv-val')), int(os.environ.get('J', '8')))
    for _b in _bad[:20]: print('[RED] ' + _b)
    sys.exit(1 if _bad else 0)
if argv == ['--version']: print(vk_version()); sys.exit(0)
if argv == ['--digest']: print(tools_digest()); sys.exit(0)
if argv == ['--header']: open(HEADER_VK, 'w').write(header_text(vk_version(), delta.abi_digest())); print(vk_version()); sys.exit(0)
if argv == ['--abi']: print(delta.abi_digest()); sys.exit(0)
if argv[:1] == ['--publish-delta'] and len(argv) >= 2:
    # The manifest a full build wrote beside its library (<v-V folder>/vk_delta.tsv) into the profile (dm/games/<id>/vk_delta.tsv, which the APK carries): checked to be of
    # this tree's version, the profile's game, inputs and keys (the keys the phone builds, vk_android_delta.py phone_keys: the whole map, or a selected set with its paired
    # pixel shaders: a selected profile's full build is the key-list build of that set), and to hold nothing but keys and hashes (manifest_problems: a home path once leaked
    # through translator binaries). "from" rows: how many shaders a phone with an older library translates, for the profile's previous manifest and each older library named;
    # "stamp" rows: an older library named that differs from this build's library only in the header's version and abi (vk_android_delta.py stamp_only), with both SHA-256s
    _d, _olds = argv[1], argv[2:]
    _g = dmsh_chain.load_game()
    try: _m = delta.parse_manifest(open(os.path.join(_d, delta.MANIFEST)).read())
    except (OSError, ValueError) as _e: sys.exit(f'[RED] {_d}: no manifest a full build wrote: {_e}')
    _mapk = {c[3] for c in (l.rstrip('\n').split('\t') for l in open(_g.table('shader_map.tsv'))) if len(c) > 4 and c[0].endswith('.bin')}
    _seen = _g.table(delta.SELECTED); _ns = borrow({'os': os, 'collections': collections})
    try: _keys = delta.phone_keys(_mapk, _seen.read_text() if _seen.exists() else None, _ns['load_pairs'](_g, str(_g.table('shader_map.tsv'))), _ns['build_keys'])
    except ValueError as _e: sys.exit(f'[RED] {_e}')
    _what = (f'the {len(_keys)} keys of the profile\'s {delta.SELECTED} with their paired pixel shaders' if _seen.exists() else 'the keys of the profile\'s shader_map.tsv')
    _why = [w for w, bad in ((f'version {_m["version"]}, this tree makes {vk_version()}', _m['version'] != vk_version()), (f'game {_m["game"]}', _m['game'] != _g.id),
                             (f'inputs {_m["inputs"]}, the profile\'s are {dmsh_chain.inputs(_g)}', _m['inputs'] != dmsh_chain.inputs(_g)),
                             (f'not {_what} ({len(_keys - set(_m["hashes"]))} missing, {len(set(_m["hashes"]) - _keys)} others)', set(_m['hashes']) != _keys)) if bad]
    if _why: sys.exit(f'[RED] {_d}/{delta.MANIFEST} is not this profile\'s full build of this tree: ' + '; '.join(_why))
    _froms, _stamps = {}, {}
    _new = os.path.join(_d, VK['LIB_FILE'])
    _prof = str(_g.table(delta.MANIFEST))
    try: _prev = delta.parse_manifest(open(_prof).read())
    except FileNotFoundError: _prev = None
    except ValueError as _e: sys.exit(f'[RED] {_prof}: {_e}')
    if _prev and _prev['game'] == _g.id and _prev['version'] != _m['version']: _froms[_prev['version']] = len(delta.to_translate(_prev['hashes'], _m['hashes']))
    if _prev and _prev['version'] == _m['version']: _froms.update(_prev['from']); _stamps.update(_prev['stamp'])   # published again: keep what was measured
    for _o in _olds:
        _L, _hs = delta.library_hashes(_o if _o.endswith('.lib') else os.path.join(_o, VK['LIB_FILE']), zstd, int(os.environ.get('J', '8'))); _L.close()
        if _L.game != _g.id: sys.exit(f'[RED] {_o}: a library of game {_L.game}')
        if _L.version != _m['version']: _froms[_L.version] = len(delta.to_translate(_hs, _m['hashes']))
        _of = _o if _o.endswith('.lib') else os.path.join(_o, VK['LIB_FILE'])
        if _L.version != _m['version'] and os.path.isfile(_new) and delta.stamp_only(_of, _new):   # byte-identity is the rule (a shader that builds nowhere counts in from)
            _stamps[_L.version] = (delta.sha256_file(_of), delta.sha256_file(_new))
    _text = delta.format_manifest(_m['version'], _g.id, _m['inputs'], _m['hashes'], _froms.items(), [(v, *h) for v, h in _stamps.items()])
    open(_prof + '.tmp', 'w').write(_text); os.replace(_prof + '.tmp', _prof)
    print(f'{_prof}: version {_m["version"]}, {len(_m["hashes"])} shaders ({sum(1 for h in _m["hashes"].values() if h == "-")} not built), {len(_text)} bytes; '
          + (', '.join(f'from v-{v}: {n} to translate' for v, n in sorted(_froms.items())) or 'no older version measured')
          + ''.join(f'; v-{v} restamps to this version' for v in sorted(_stamps)))
    sys.exit(0)
if len(argv) < 4 or argv[0] != '--target' or argv[1] not in ('vulkan', 'android'): sys.exit(__doc__ or 'usage: vk_android_build.py --target vulkan <package dir> <output cache dir> [key...] | --header | --version')
dm_dir, cache_root, want = argv[2], argv[3], set(argv[4:])

CHAIN = dmsh_chain.version(); VKV = vk_version(); TOOLS = tools_digest()
if f'"{CHAIN}"' not in open(dmsh_chain.HEADER).read(): sys.exit(f'[RED] {dmsh_chain.HEADER} does not hold the current chain version {CHAIN}: run dmsh_chain.py --header first')
if committed_version() != VKV or committed_version('DMSH_VK_ABI') != delta.abi_digest():
    sys.exit(f'[RED] {HEADER_VK} does not hold the current Vulkan library version {VKV} and ABI {delta.abi_digest()}: run vk_android_build.py --header and commit it')
ABI = delta.abi_digest()
SPIRV_VAL = os.environ.get('SPIRV_VAL') or shutil.which('spirv-val')
if not SPIRV_VAL: sys.exit('[RED] no spirv-val (SPIRV-Tools: brew install spirv-tools, or micromamba install -c conda-forge spirv-tools)')
VALIDATE = spirv_validator(SPIRV_VAL); VAL_ID = VALIDATE.version   # in each result's stamp: another validator builds the results again
XLATE = os.environ.get('XLATE', os.path.join(dmsh_chain.XLATE_DIR, 'dmsh_xlate'))
XLATE_LIB = XlateLib(XLATE) if is_lib(XLATE) else None
WORKER = globals().get("VK_BUILD_WORKER")   # set when this run is one of the build's worker interpreters (DMSH_VK_PROCS, Pool below)
_bad = [] if WORKER else check_translator(XLATE_LIB)   # (the parent checked the same translator)
if _bad: sys.exit('\n'.join('[RED] translator does not match the lock file: ' + b for b in _bad))
GAME = dmsh_chain.load_game(); INPUTS = dmsh_chain.inputs(GAME)
import dmsh_facts
FACTS = dmsh_facts.load()
NS = borrow({'os': os, 're': re, 'mmap': mmap, 'sys': sys, 'struct': struct, 'collections': collections, 'abi': abi, 'dmsh_ampere': dmsh_ampere, 'FACTS': FACTS,
             'TEX_BASE': abi.C2_TEX, 'IMG_BASE': abi.C2_IMG, 'IMG_SLOTS': abi.C2_IMG_SLOTS, 'SEPTEX_BASE': abi.C2_SEPTEX, 'SEPTEX_SLOT0': abi.SEPTEX_SLOT0, 'SAMP_BASE': abi.C2_SAMP})
out = os.path.join(cache_root, VK['DIR_PREFIX'] + VKV); os.makedirs(out, exist_ok=True)
_g = dmsh_chain.read_stamp(out).get('game')
if _g and _g != GAME.id: sys.exit(f'[RED] {out} holds the shader library of game {_g}, not {GAME.id}: give each game its own output directory')

MAP = str(GAME.table('shader_map.tsv')); pc_of, loc = {}, {}
for ln in open(MAP):
    c = ln.rstrip('\n').split('\t')
    if len(c) > 4 and c[0].endswith('.bin'): pc_of.setdefault(c[3], c[4]); loc.setdefault(c[3], (c[0], int(c[1])))
PAIRS = NS['load_pairs'](GAME, dm_dir)
_dw = GAME.table('dual_want.tsv')   # pixel shaders a pipeline blends with output 1 as the second source (dmsh_build.py DUAL_WANT, from the Mac host's DM_NVN_DUAL_WANT)
DUAL_WANT = {l.split()[0] for l in open(_dw) if l.strip() and not l.lstrip().startswith('#')} if _dw.exists() else set()
VS_OF = collections.defaultdict(set)
for _v, _ps in PAIRS.items():
    for _p in _ps: VS_OF[_p].add(_v)

STAGE = {'vs': VK['STAGE_VS'], 'hs': VK['STAGE_TCS'], 'ds': VK['STAGE_TES'], 'gs': VK['STAGE_GS'], 'ps': VK['STAGE_FS'], 'cs': VK['STAGE_CS'], 'ts': VK['STAGE_TS'], 'ms': VK['STAGE_MS']}
BIND_RANGE = {**STAGE, 'cs': VK['STAGE_VS'], 'ts': VK['STAGE_TCS'], 'ms': VK['STAGE_VS']}   # the stage whose set-0 binding range a stage's bindings use (dmsh_abi_vk.h)
SSBO = {'vs': abi.C0_SSBO_VS, 'cs': abi.C0_SSBO_CS, 'gs': abi.C0_SSBO_GS, 'ps': abi.C0_SSBO_PS, 'ts': abi.C0_SSBO_TS, 'ms': abi.C0_SSBO_MS}
# NVN2 mesh programs' per-primitive outputs (shader_split.py mesh_facts: "mesh" after -1, codes 4 + 4 g + c) → their pixel shaders' inputs at those locations
# (vk_android_spv.py per_primitive_inputs). Such a pixel shader must be drawn with mesh programs only: one also paired with a vertex shader is left as it is, with a red line
MESH_PRIM = collections.defaultdict(set); _prim_of = collections.defaultdict(dict)
for _k, _f in FACTS.items():
    _l = _f.get('mesh') or []
    if not _l or not _l[0]: continue   # a task program
    _s = set()
    if -1 in _l[3:]:
        _e = _l.index(-2, _l.index(-1, 3)) if -2 in _l[_l.index(-1, 3):] else len(_l)
        _s = {(c - 4) // 4 for c in _l[_l.index(-1, 3) + 1:_e] if c >= 4}
    for _p in _f.get('pair', ()): MESH_PRIM[_p] |= _s; _prim_of[_p][_k] = _s
for _p, _m in _prim_of.items():   # every mesh program drawn with one pixel shader must write the same inputs per primitive, or one of them mismatches its decorations
    if len({frozenset(s) for s in _m.values()}) > 1: print(f'[RED] pixel shader {_p}: its mesh programs write different inputs per primitive ' + ', '.join(f'{k} {sorted(s)}' for k, s in sorted(_m.items())), flush=True)
MESH_PRIM = collections.defaultdict(set, {p: s for p, s in MESH_PRIM.items() if s})
for _p in [p for p in MESH_PRIM if VS_OF.get(p)]:
    print(f'[RED] pixel shader {_p} is drawn with mesh programs (per-primitive inputs {sorted(MESH_PRIM[_p])}) and with vertex shaders {sorted(VS_OF[_p])[:3]}: its inputs are left per vertex', flush=True)
    del MESH_PRIM[_p]
KIND = {abi.KIND_CBUF: 'CBUF', abi.KIND_C1: 'C1', abi.KIND_DRVCB: 'DRVCB', abi.KIND_SUPPORT: 'SUPPORT', abi.KIND_SBUF: 'SBUF', abi.KIND_SBUF_UBO: 'SBUF_UBO', abi.KIND_UBO_PTR: 'UBO_PTR',
        abi.KIND_TEX: 'TEX', abi.KIND_TBUF: 'TBUF', abi.KIND_IMG: 'IMG', abi.KIND_SAMP: 'SAMP', abi.KIND_SAMP_TEX: 'SAMP_TEX', abi.KIND_SAMP_ID: 'SAMP_ID', abi.KIND_TEX_UBO: 'TEX_UBO',
        abi.KIND_SAMP_UBO: 'SAMP_UBO', abi.KIND_TEX_C2A: 'TEX_C2A', abi.KIND_SAMP_C2A: 'SAMP_C2A', abi.KIND_TEX_POOL: 'TEX_POOL', abi.KIND_SAMP_POOL: 'SAMP_POOL',
        abi.KIND_GMEM_MAP: 'GMEM_MAP', abi.KIND_GMEM_LOG: 'GMEM_LOG', abi.KIND_SHARED_SPILL: 'SHARED_SPILL', abi.KIND_UNKNOWN: 'UNKNOWN'}
KIND_NUM = {k: VK['KIND_' + v] for k, v in KIND.items()}
SLOT_KINDS = {abi.KIND_CBUF, abi.KIND_SAMP, abi.KIND_SAMP_TEX, abi.KIND_TEX, abi.KIND_IMG, abi.KIND_TBUF, abi.KIND_SBUF, abi.KIND_UBO_PTR, abi.KIND_SBUF_UBO, abi.KIND_TEX_UBO, abi.KIND_SAMP_UBO, abi.KIND_SAMP_ID}
POOL_SUFFIX = '_cb0_40000000'   # a handle traced to no slot indexes the pools by its IDs (Program.cs QueryTextureArrayLengthFromPool; dmsh_build.py pool_arrays)


def sampler_half(name):
    """The sampler of a texture name, as dmsh_build.py reflect gives the MSL <name>Smplr member: (kind, reg, off)"""
    ut = NS['decode_ubo_tex'](name)
    if ut: return (abi.KIND_SAMP_ID, ut[1][1], 0) if ut[1][0] == 'id' else (abi.KIND_SAMP_UBO, ut[1][0], ut[1][1])
    d = NS['decode_tex'](name)
    if not d: return abi.KIND_UNKNOWN, None, 0
    t, s, ty = d
    if ty == 3: return abi.KIND_SAMP_ID, NS['const_sampler'](name), 0
    return (abi.KIND_SAMP, s, 0) if ty else (abi.KIND_SAMP_TEX, t, 0)


def classify(stage, r, res, m):
    """A descriptor variable → its binding record (dict): the kind and slot from its name (dmsh_build.py reflect's rules, on the SPIR-V names the MSL members come from)"""
    nm, desc = r['name'], r['desc']
    e = {'name': nm, 'desc': desc, 'count': r['count'], 'reg': None, 'off': 0, 'skind': None, 'sreg': None, 'soff': None, 'flags': 0, 'dim': r['dim'], 'id': r['id'], 'tbind': r['binding']}
    texlike = desc in (spv.VK_COMBINED, spv.VK_SAMPLED_IMAGE, spv.VK_UNIFORM_TEXEL, spv.VK_STORAGE_IMAGE, spv.VK_STORAGE_TEXEL, spv.VK_SAMPLER)
    if nm == 'support_buffer': e['kind'] = abi.KIND_SUPPORT
    elif desc in (spv.VK_UNIFORM_BUFFER,) and re.fullmatch(r'[a-z]+_c(\d+)', nm):
        n = int(re.fullmatch(r'[a-z]+_c(\d+)', nm).group(1))
        if 3 <= n <= 16: e.update(kind=abi.KIND_CBUF, reg=n - 3)
        else: e.update(kind=abi.KIND_C1 if n == 1 else abi.KIND_DRVCB, reg=n)
    elif desc == spv.VK_STORAGE_BUFFER and re.fullmatch(r'[a-z]+_s(\d+)', nm):
        e.update(kind=abi.KIND_SBUF, index=int(re.fullmatch(r'[a-z]+_s(\d+)', nm).group(1)))
    elif texlike and nm.endswith(POOL_SUFFIX) and r['count'] >= 33:
        e.update(kind=abi.KIND_SAMP_POOL if desc == spv.VK_SAMPLER else abi.KIND_TEX_POOL)
        if desc not in (spv.VK_SAMPLER, spv.VK_SAMPLED_IMAGE): e['kind'] = abi.KIND_UNKNOWN   # a pool of combined or storage images: not in the ABI
    elif texlike and NS['decode_ubo_tex'](nm):
        ut, us = NS['decode_ubo_tex'](nm)
        if desc == spv.VK_SAMPLER: e.update(kind=abi.KIND_SAMP_ID, reg=us[1]) if us[0] == 'id' else e.update(kind=abi.KIND_SAMP_UBO, reg=us[0], off=us[1])
        elif desc == spv.VK_UNIFORM_TEXEL: e['kind'] = abi.KIND_UNKNOWN   # a bindless buffer texture: not implemented in the replay (as on Metal)
        else: e.update(kind=abi.KIND_TEX_UBO, reg=ut[0], off=ut[1])
    elif texlike and r['count'] > 1 and NS['c2_array'](nm, 'array<'):
        off, soff = NS['c2_array'](nm, 'array<')
        e.update(kind=abi.KIND_SAMP_C2A if desc == spv.VK_SAMPLER else abi.KIND_TEX_C2A, off=off)
        if desc == spv.VK_COMBINED: e.update(skind=abi.KIND_SAMP_C2A, soff=soff if soff is not None else 0xFFFF, off=off)
        elif soff is not None: e['soff'] = soff
    elif texlike:
        d = NS['decode_tex'](nm)
        if not d: e['kind'] = abi.KIND_UNKNOWN
        elif desc == spv.VK_SAMPLER:
            t, s, ty = d
            e.update(kind=abi.KIND_SAMP_ID, reg=NS['const_sampler'](nm)) if ty == 3 else e.update(kind=abi.KIND_SAMP, reg=s) if ty else e.update(kind=abi.KIND_SAMP_TEX, reg=t)
        else:
            img = re.match(r'[a-z]+_i', nm)
            e.update(kind=abi.KIND_IMG if img else abi.KIND_TBUF if desc == spv.VK_UNIFORM_TEXEL else abi.KIND_TEX, reg=d[0])
            if e['kind'] == abi.KIND_TEX and e['reg'] in res.get('fetch1d', ()): e['flags'] |= VK['BF_FETCH1D']
    else: e['kind'] = abi.KIND_UNKNOWN
    if desc == spv.VK_COMBINED and e['kind'] in (abi.KIND_TEX, abi.KIND_TEX_UBO):
        e['skind'], e['sreg'], e['soff'] = sampler_half(nm)
    return e


def storage_slots(stage, binds, res):
    """Storage buffers: the translator's binding → c0 offset → slot (dmsh_build.py reflect's second half)"""
    base = SSBO.get(stage)
    sb_sorted = sorted(res['sbufs'], key=lambda b: b['binding'])
    for e in binds:
        if e['kind'] != abi.KIND_SBUF: continue
        b = next((x for x in res['sbufs'] if x['binding'] == e['tbind']), None) or (sb_sorted[e['index']] if e['index'] < len(sb_sorted) else None)
        off = b['cb_offset'] * 4 if b and b['cb_slot'] == 0 else None
        if b and b['cb_slot'] == abi.GMEM_MAP_CB_SLOT and b['cb_offset'] == abi.SHARED_SPILL_CB_OFFSET:   # shared memory past ANSWERS shared_max (dmsh_abi_vk.h)
            e.update(kind=abi.KIND_SHARED_SPILL, flags=e['flags'] | VK['BF_RW']); continue
        if b and b['cb_slot'] == abi.GMEM_MAP_CB_SLOT:
            e['kind'] = abi.KIND_GMEM_LOG if b['cb_offset'] == abi.GMEM_LOG_CB_OFFSET else abi.KIND_GMEM_MAP
            if e['kind'] == abi.KIND_GMEM_LOG or any(re.search(r'operation "(Store|Atomic\w*)" traced to no storage buffer', n) for n in res.get('notes', ())): e['flags'] |= VK['BF_RW']
            continue
        if stage == 'cs' and off is not None and abi.C0_CS_UBO <= off < abi.C0_CS_UBO + abi.C0_ROW * abi.C0_CS_UBO_SLOTS and (off - abi.C0_CS_UBO) % abi.C0_ROW == 0:
            e.update(kind=abi.KIND_UBO_PTR, reg=(off - abi.C0_CS_UBO) // abi.C0_ROW)
        elif b and b['cb_slot'] >= 3: e.update(kind=abi.KIND_SBUF_UBO, reg=b['cb_slot'] - 3, off=b['cb_offset'] * 4)
        else: e['reg'] = (off - base) // abi.C0_ROW if off is not None and base is not None and 0 <= off - base < abi.C0_ROW * abi.C0_SSBO_SLOTS and (off - base) % abi.C0_ROW == 0 else None
        if b and b['flags'] & 1: e['flags'] |= VK['BF_RW']


def dim_byte(d):
    if not d: return 0
    v = d['dim'] & VK['DIM_MASK']
    if d['array']: v |= VK['DIM_ARRAY']
    if d['ms']: v |= VK['DIM_MS']
    if d['depth']: v |= VK['DIM_DEPTH']
    if d['type']: v |= VK['DIM_SINT'] if d['type'][1] else VK['DIM_UINT']
    return v


def place(stage, binds, m):
    """Set and binding of each record in the Vulkan ABI, written into the module"""
    base = BIND_RANGE[stage] * VK['STAGE_BINDINGS']; n = 0
    for e in sorted(binds, key=lambda e: e['tbind']):
        if e['kind'] in (abi.KIND_TEX_POOL, abi.KIND_SAMP_POOL):
            e['set'] = VK['SET_POOL']
            e['binding'] = VK['POOL_TEX_BINDING'] if e['kind'] == abi.KIND_TEX_POOL else VK['POOL_SAMP_BINDING']
            e['count'] = VK['POOL_TEX_COUNT'] if e['kind'] == abi.KIND_TEX_POOL else VK['POOL_SAMP_COUNT']
        else:
            if n >= VK['STAGE_BINDINGS']: raise ValueError(f'more than {VK["STAGE_BINDINGS"]} bindings in one stage')
            e['set'], e['binding'] = VK['SET_DRAW'], base + n; n += 1
        m.set_binding(e['id'], e['set'], e['binding'])
        if e['kind'] in SLOT_KINDS and e.get('reg') is None: e['flags'] |= VK['BF_NOSLOT']
        if e['kind'] == abi.KIND_IMG and m_atomic(m, e['id']): e['flags'] |= VK['BF_ATOMIC']
        if e['kind'] == abi.KIND_IMG and m_writes(m, e['id']): e['flags'] |= VK['BF_RW']


def m_atomic(m, vid):   # an image texel pointer on this image: its atomics
    return any(x[0] & 0xffff == spv.OP_IMAGE_TEXEL_POINTER and x[3] == vid for x in m.ins) or \
        any(x[0] & 0xffff == spv.OP_IMAGE_TEXEL_POINTER and x[3] in {y[2] for y in m.ins if y[0] & 0xffff in (spv.OP_ACCESS_CHAIN, spv.OP_IN_BOUNDS_ACCESS_CHAIN) and y[3] == vid} for x in m.ins)


def m_writes(m, vid):   # an OpImageWrite (99) of an image loaded from this variable, or an atomic on it
    loads = {x[2] for x in m.ins if x[0] & 0xffff == spv.OP_LOAD and x[3] == vid}
    return any(x[0] & 0xffff == 99 and x[1] in loads for x in m.ins) or m_atomic(m, vid)


CAP_FLAGS = {'Float64': None, 'GeometryShaderPassthroughNV': 'F_NV_PASSTHROUGH', 'ShaderViewportMaskNV': 'F_NV_VIEWPORT', 'FragmentShaderPixelInterlockEXT': 'F_INTERLOCK',
             'FragmentShaderSampleInterlockEXT': 'F_INTERLOCK', 'FragmentShaderShadingRateInterlockEXT': 'F_INTERLOCK'}


def facts_of(key, stage, m, job, res):
    caps = sorted(spv.CAPS.get(c, str(c)) for c in set(m.caps))
    f = 0
    if job.get('early_fragment_tests'): f |= VK['F_EARLY_Z']
    if any('FP64 lowered' in n for n in res['notes']): f |= VK['F_SOFTFLOAT64']
    if spv.subgroup_used(m): f |= VK['F_SUBGROUP']
    if job.get('tf'): f |= VK['F_TF']
    for c in caps:
        if CAP_FLAGS.get(c): f |= VK[CAP_FLAGS[c]]
    fn = spv.functions_named(m)
    if 'dmsh_hmma16816_f16' in fn: f |= VK['F_HMMA']
    if job.get('no_facts'): f |= VK['F_NO_FACTS']
    vin = {loc: t for loc, (_, t) in m.io(spv.SC_INPUT).items()} if stage == 'vs' else {}
    fout = {loc: t for loc, (_, t) in m.io(spv.SC_OUTPUT).items()} if stage == 'ps' else {}
    def ty(t):
        it = m.int_type(m.scalar_of(t))
        return VK['TYPE_FLOAT'] if not it else VK['TYPE_SINT'] if it[1] else VK['TYPE_UINT']
    iout = spv.int_outputs(m) if stage == 'ps' else set()
    if iout - {l for l, t in fout.items() if ty(t) != VK['TYPE_FLOAT']}: f |= VK['F_INT_OUT']
    return {'flags': f, 'caps': caps, 'exts': sorted(set(m.exts)), 'vin': {l: ty(t) for l, t in vin.items()}, 'fout': {l: ty(t) for l, t in fout.items()},
            'hmma_fns': sorted(n for n in fn if 'mma' in n.lower())}


def drv_reads(binds, m):
    """Reads of the driver segments c0 / c2 and the push block: {bank: {'offsets': byte offsets read at constant indices (a row's component), 'dynamic': computed index}}"""
    out = {}
    for e in binds:
        if e['kind'] == abi.KIND_DRVCB:
            rows, dyn = spv.block_rows(m, e['id'])
            out[str(e['reg'])] = {'offsets': rows, 'dynamic': dyn}
    return out


def validate(data, w, tag):
    err = VALIDATE(data)
    return None if err is None else err[:200]


# Translator calls at once: DMSH_VK_XLATE_J, default 1 for the library form (on the phone threads of one process made the translator slower, not faster:
# android/ONDEVICE.md, The translator on the phone; the other J threads still validate, compress and run the Python half), J for the program form. With worker
# interpreters (DMSH_VK_PROCS) the limit holds for the whole process: the workers share one gate (QueueGate)
XLATE_J = max(1, int(os.environ.get('DMSH_VK_XLATE_J') or (1 if XLATE_LIB else int(os.environ.get('J', '8')))))
XLATE_GATE = threading.Semaphore(XLATE_J)   # (a worker's is its QueueGate, below)


def translate(jobs_list, w, tag):
    jp = os.path.join(w, f'job_{tag}.json'); json.dump(jobs_list, open(jp, 'w'))
    with XLATE_GATE:
        if XLATE_LIB: return XLATE_LIB([jp, w], os.path.join(w, f'log_{tag}.txt'))
        return subprocess.run([XLATE, jp, w], capture_output=True, text=True)


PS_INPUTS = {}   # pixel shader key → the input locations it reads (for its vertex shaders' padding)


def finish(key, w, job, c1):
    stage = job['stage']; sp = os.path.join(w, key + '.spv')
    res = json.load(open(os.path.join(w, key + '.res.json')))
    m = spv.Module(open(sp, 'rb').read())
    if stage == 'vs':   # inputs used only as integer bits: translated again with uint inputs (dmsh_build.py int_inputs), as the vertex input format's class must match
        extra = {l: t for l, t in spv.integer_only_inputs(m).items() if l not in job['attr']}
        if extra:
            job = dict(job, attr={**job['attr'], **extra})
            rx = translate([job], w, 'int_' + key)
            if rx.returncode or not os.path.exists(sp): return 'translation failed', ('integer-input retranslation: ' + rx.stderr.strip())[:160], None
            res = json.load(open(os.path.join(w, key + '.res.json'))); m = spv.Module(open(sp, 'rb').read())
    vcull = stage == 'gs' and spv.private_viewport_mask(m)   # before strip_unused_caps: the capability goes with the output
    dropped = spv.strip_unused_caps(m)
    added = spv.formatless_storage_caps(m)
    n_precise = spv.precise_minmax(m)
    spv.txq_extra_components(m)   # a size query's component past the image's dimensions (vk_android_spv.py txq_extra_components)
    hmma = stage == 'cs' and spv.hmma_lds(m)   # the HMMA helper's calls lowered at their sites, gathers shared (vk_android_spv.py hmma_lds)
    if not hmma: spv.promote_locals(m)   # simplify GlobalToStorage helpers (binary search, CAS, branchless inPool) and promote single-store locals
    if not hmma: spv.loop_pool_search(m)   # the raw helpers' pool table search one loop of one step (vk_android_spv.py loop_pool_search)
    binds = [classify(stage, r, res, m) for r in m.resources()]
    storage_slots(stage, binds, res)
    try: place(stage, binds, m)
    except ValueError as e: return 'reflection failed', str(e)[:160], None
    if not hmma: spv.cache_raw_loads(m, {e['id'] for e in binds if e['kind'] == abi.KIND_GMEM_MAP})   # the raw load helpers' runs read at once from the pool entry kept last (vk_android_spv.py cache_raw_loads)
    pad = []
    if stage == 'vs':
        want_in = set().union(*[PS_INPUTS.get(p, set()) for p in PAIRS.get(key, ())]) if PAIRS.get(key) else set()
        pad = spv.init_outputs(m, want_in)
    elif stage == 'ps': PS_INPUTS[key] = set(m.io(spv.SC_INPUT))
    prim = spv.per_primitive_inputs(m, MESH_PRIM[key]) if stage == 'ps' and key in MESH_PRIM else []
    gl = stage in ('vs', 'ds', 'gs') and spv.gl_depth(m, VK['SPEC_GL_DEPTH'])
    if stage == 'ms':   # the translator adds the constant itself (job gl_depth_spec: a mesh program may not read its outputs back)
        gl = any(d.get(spv.DEC_SPEC_ID) == [VK['SPEC_GL_DEPTH']] for d in m.decos.values())
    alpha = stage == 'ps' and spv.alpha_test(m, VK['SPEC_ALPHA_FUNC'])
    mods = [(VK['MOD_BASE'], m.data())]
    uint_mask = 0
    if stage == 'ps':   # the variants of the same module (dmsh_abi_vk.h DMSH_VK_MOD_*)
        mods_, uint_mask = ps_variants(key, m)
        mods += mods_
    if stage == 'cs':   # its subgroup shuffles through workgroup memory, for drivers without an arbitrary-index shuffle instruction (vk_android_spv.py shuffle_lds)
        x = spv.Module(m.data())
        if spv.shuffle_lds(x, VK['SHARED_MAX']): mods.append((VK['MOD_SHUFFLE_LDS'], x.data()))
    for variant, data in mods:
        err = validate(data, w, f'{key}_{variant}')
        if err: return 'spirv-val failed', err, None
    fa = facts_of(key, stage, m, job, res)
    if hmma: fa['flags'] |= VK['F_HMMA']; fa['hmma_fns'] = [spv.HMMA_NAME]   # (lowered: the helper function is gone)
    if gl: fa['flags'] |= VK['F_GL_DEPTH']
    if alpha: fa['flags'] |= VK['F_ALPHA_TEST']
    if vcull: fa['flags'] |= VK['F_VIEWPORT_CULL']
    if prim or (stage == 'ms' and (res.get('mesh') or {}).get('primitive_locations')): fa['flags'] |= VK['F_MESH_PRIM']
    fa['uint_mask'] = uint_mask; fa['variants'] = [v for v, _ in mods]
    name, k = loc[key]
    shared = job.get('shared') or 0
    sp = res.get('shared_spill')   # shared memory moved to a storage buffer (Program.cs): the slice the shader addresses, and what stays in workgroup memory (the HMMA scratch)
    for e in binds:
        if e['kind'] == abi.KIND_SHARED_SPILL:
            if not sp: e['kind'] = abi.KIND_UNKNOWN; continue   # (the replay then refuses the shader)
            e['bytes'] = sp['bytes']; shared = sp['workgroup']
    refl = {'key': key, 'stage': stage, 'source': f'{name}#{k}', 'pc_paired': pc_of[key], 'c1': c1.hex(), 'local': job.get('local') or [1, 1, 1], 'shared': shared,
            'notes': res['notes'], 'vs_pad': pad, 'mesh': res.get('mesh'), 'ps_prim': prim, 'caps_dropped': dropped, 'caps_added': added, 'precise': n_precise, 'hmma_lds': hmma, 'drv_reads': drv_reads(binds, m), 'tf': job.get('tf'), **fa,
            'bindings': [{kk: v for kk, v in e.items() if kk not in ('id', 'tbind', 'index')} for e in binds]}
    loose = os.environ.get('DMSH_VK_LOOSE') == '1'
    return 'ok', '', (refl, [(variant, len(data), *pack_module(data), data if loose else None) for variant, data in mods])   # compressed here, in parallel


def ps_variants(key, m):
    """A pixel shader's extra modules (dmsh_build.py fs_uint / fs_dual on SPIR-V): MOD_UINT when it writes an output from integer bits or only constants (a clear into an
    integer target), MOD_UINT_MIXED when it also writes float outputs (uint_mask: the integer ones), MOD_DUAL for the profile's dual_want.tsv shaders writing exactly
    colours 0 and 1. → ([(variant, SPIR-V)], uint_mask)"""
    out, mask = [], 0
    fl = {l for l, (_, t) in m.io(spv.SC_OUTPUT).items() if not m.int_type(m.scalar_of(t))}
    from_int = spv.int_outputs(m) & fl
    if from_int or spv.const_written_outputs(m):
        u = spv.Module(m.data())
        if spv.retype_outputs(u, fl): out.append((VK['MOD_UINT'], u.data()))
        if from_int and fl - from_int:
            u = spv.Module(m.data()); done = spv.retype_outputs(u, from_int)
            if done: out.append((VK['MOD_UINT_MIXED'], u.data())); mask = sum(1 << l for l in done if l < 8)
    if key in DUAL_WANT:
        d = spv.Module(m.data())
        if spv.dual_source(d): out.append((VK['MOD_DUAL'], d.data()))
    return out, mask


def key_int(k): return int(k, 16)


ZSTD_LEVEL = 19   # SPIR-V of this translator compresses about 5:1 at 19 (3.5:1 at 3); a trained dictionary gained 1-7 % more and is not used


def pack_module(data):
    """(bytes to store, codec): one zstd frame when that is smaller, else the words as they are"""
    if zstd is None or os.environ.get('DMSH_VK_RAW') == '1': return data, VK['CODEC_RAW']
    c = zstd.compress(data, level=ZSTD_LEVEL)
    return (c, VK['CODEC_ZSTD']) if len(c) < len(data) else (data, VK['CODEC_RAW'])


def entry_parts(key, refl, nmods):
    """One shader's part of the library as the library stores it: (its 64-byte entry with its positions in the file zero, its binding records, its c1 bytes). A result
    reused from an older library (delta) carries them as they were there"""
    if refl.get('reused'): return bytes.fromhex(refl['lib']['entry']), bytes.fromhex(refl['lib']['binds']), bytes.fromhex(refl['lib']['c1'])
    binds = bytearray()
    for e in refl['bindings']:
        s = e.get('soff'); s = 0xFFFF if s is None else s
        binds += struct.pack('<BBHBBHhHBBhHHI', KIND_NUM[e['kind']], e['desc'], e['binding'], e['set'], e['flags'], 0 if e['set'] == VK['SET_POOL'] else min(e['count'], 0xFFFF),
                             -1 if e.get('reg') is None else e['reg'], e.get('off') or 0, KIND_NUM[e['skind']] if e.get('skind') else 0, dim_byte(e.get('dim')),
                             -1 if e.get('sreg') is None else e['sreg'], s, 0, e.get('bytes') or 0)
    c1 = bytes.fromhex(refl['c1'])
    vin_mask = sum(1 << int(l) for l in refl['vin']); vin_types = sum(t << (2 * int(l)) for l, t in refl['vin'].items())
    fout_mask = sum(1 << int(l) for l in refl['fout'] if int(l) < 8); fout_types = sum(t << (2 * int(l)) for l, t in refl['fout'].items() if int(l) < 8)
    lx, ly, lz = (list(refl['local']) + [1, 1, 1])[:3]
    entry = delta.ENTRY.pack(key_int(key), vin_types, STAGE[refl['stage']], nmods, refl['flags'], 0, 0, len(refl['bindings']), fout_types,
                             lx, ly, lz, fout_mask, refl.get('uint_mask', 0), refl['shared'], 0, len(c1), vin_mask)
    return entry, bytes(binds), c1


class Stored:
    """A module's stored bytes left in its result file until the library is written (read_result): a large game's modules (Pokemon Pokopia's: 9.8 GB) are never all in
    memory at once"""
    __slots__ = ('path', 'off', 'n')
    def __init__(self, path, off, n): self.path, self.off, self.n = path, off, n
    def __len__(self): return self.n
    def read(self):
        with open(self.path, 'rb') as f:
            f.seek(self.off); d = f.read(self.n)
        if len(d) != self.n: sys.exit(f'[RED] {self.path} changed while the library was written: run again')
        return d


def stored_bytes(s): return s.read() if isinstance(s, Stored) else s


BLOB_MAX = 1 << 48   # module offsets have 48 bits (off, off_hi), c1 offsets 64 (dmsh_abi_vk.h)


def write_library(results):
    """dmsh_vk.lib: header, entries sorted by key, binding records, module records, blob (dmsh_abi_vk.h). The tables are made first, with each blob piece's place; then
    the pieces are written one at a time, so the blob is never in memory as a whole. Offsets past 4 GiB keep their high bits in c1_off_hi / off_hi, which are 0 in a
    smaller library: that one is written as before"""
    ok = sorted(((k, r) for k, st, _, r in results if st == 'ok'), key=lambda x: key_int(x[0]))
    entries, binds, mods = bytearray(), bytearray(), bytearray(); pieces = []; blen = 0
    nb = nm = 0
    def put(b):
        nonlocal blen
        blen += -blen % 16; off = blen; pieces.append((off, b)); blen += len(b)
        if blen > BLOB_MAX: sys.exit(f'[RED] the library blob passes {BLOB_MAX >> 40} TiB, the most a module record addresses')
        return off
    for key, (refl, modules) in ok:
        entry, bb, c1 = entry_parts(key, refl, len(modules))
        c1_off = put(c1) if c1 else 0
        b0, m0 = nb, nm
        for variant, size, stored, codec, _ in modules:
            off = put(stored)
            mods += struct.pack('<IIIBBH', off & 0xFFFFFFFF, size, len(stored), variant, codec, off >> 32); nm += 1
        binds += bb; nb += len(bb) // VK['BIND_SIZE']
        e = bytearray(entry)
        for o, v in zip(delta.POS, (m0, b0, c1_off & 0xFFFFFFFF, c1_off >> 32)): struct.pack_into('<I', e, o, v)
        entries += e
    hs = VK['HEADER_SIZE']; eo = hs; bo = eo + len(entries); mo = bo + len(binds); blob_off = (mo + len(mods) + 15) // 16 * 16
    pad = lambda s: s.encode()[:16].ljust(16, b'\0')   # NUL padded; a 16-character value (a title ID) has no terminator
    head = delta.HEAD.pack(VK['LIB_MAGIC'], VK['LIB_VERSION'], pad(CHAIN), pad(VKV), pad(GAME.id), pad(INPUTS),
                           len(ok), eo, nb, bo, nm, mo, blob_off, blen, ANSWERS['subgroup_size'], ANSWERS['ssbo_align'], ABI.encode())
    assert len(head) == hs and len(entries) == len(ok) * VK['ENTRY_SIZE'] and len(binds) == nb * VK['BIND_SIZE'] and len(mods) == nm * VK['MOD_SIZE']
    data = head + entries + binds + mods; data += b'\0' * (blob_off - len(data))
    path = os.path.join(out, VK['LIB_FILE']); tmp = path + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(data); pos = 0
        for off, b in pieces:
            b = stored_bytes(b)
            f.write(b'\0' * (off - pos)); f.write(b); pos = off + len(b)
        assert pos == blen and f.tell() == blob_off + blen
        f.flush(); os.fsync(f.fileno())   # whole on storage before it takes the library's name (write_result)
    os.replace(tmp, path)
    return path, blob_off + blen


# ---- the build
# The device answers only this build gives the translator (Program.cs; dmsh_abi_vk.h): Adreno's 64-lane subgroups, storage buffers bound at an aligned-down address, and
# geometry passthrough the shader does itself (no SPV_NV_geometry_shader_passthrough). The Apple jobs carry none of these keys, so their translations stay as they were
# Bits of the gather coordinate bias (Program.cs QueryHostGatherBiasPrecision; Ryujinx TexturePass): the translator moves each gather coordinate by
# 1 / (size x 2^(bits + 1)) texel, 1 / 512 at 8. Adreno's texture unit (the hardware, so Qualcomm's driver and Turnip alike) rounds a gather coordinate that lies just below a
# texel edge to the footprint before it, where Maxwell takes the next one: dark 16x16 tile boxes in Tears of the Kingdom's tiled lighting (Eden #4565, which applies the same
# 1 / 512 on Qualcomm and Turnip; nx2android 44eca7e, a constant 8 in its Android chain). A constant of this target, not a probe of the machine that builds: the library's
# stamp (DMSH_VK_CHAIN) is the tools' digest, so the Mac's build, a phone's build and the delta manifest (vk_delta.tsv) must translate alike. It also holds where it is not
# needed: 8 is Vulkan's usual subTexelPrecisionBits (Adreno, Mali, Turnip), the nudge is half of one such step, so a coordinate on the edge or past it keeps its footprint;
# only one less than half a step below an edge moves to the next footprint, the one Maxwell takes. A GPU that ever needs another value is a new
# target with its own version, not a switch
GATHER_BIAS = 8


ANSWERS = {'subgroup_size': VK['SUBGROUP_SIZE'], 'ssbo_align': VK['SSBO_ALIGN'], 'gs_passthrough': False, 'shared_max': VK['SHARED_MAX'], 'gather_bias': GATHER_BIAS}
keys = NS['build_keys'](want, loc, PAIRS) if want else sorted(loc)
NJ = int(os.environ.get('J', '8')); BATCH = max(1, int(os.environ.get('DMSH_VK_BATCH', '64')))
STAGE_OF = {k: loc[k][0][:2] for k in keys}   # the package file's name gives the stage (dmsh_build.py job_for)
ORDER = [k for k in keys if STAGE_OF[k] != 'vs'] + [k for k in keys if STAGE_OF[k] == 'vs']   # pixel shaders first: their inputs pad their vertex shaders' outputs
RAW = zstd is None or os.environ.get('DMSH_VK_RAW') == '1'
RES = os.environ.get('DMSH_VK_RESULTS') or os.path.join(out, 'results'); os.makedirs(RES, exist_ok=True)
RESULT_MAGIC = b'DMVKRES1'


def result_path(key): return os.path.join(RES, key + '.r')


class ResultCut(Exception):
    """A result file shorter than its head says (read_result)"""


def result_file(path):
    """(the JSON head of a result file, cut): RESULT_MAGIC, the head's length, the JSON, then the modules' stored bytes, which must end the file exactly. The head is None
    when the file is not a whole result of this build: another DMSH_VK_CHAIN, profile inputs, codec setting or key (stale results are rebuilt and replaced), or a file shorter
    or longer than its head says (cut is then True: what a stop left on storage that lost the end of a renamed file, #82). Either way the shader is translated again"""
    try:
        with open(path, 'rb') as f:
            d = f.read(12)
            if len(d) != 12 or d[:8] != RESULT_MAGIC: return None, False
            n = struct.unpack('<I', d[8:])[0]; j = f.read(n); size = os.fstat(f.fileno()).st_size
        if len(j) != n: return None, True
        h = json.loads(j)
        if 12 + n + sum(m[2] for m in h['modules']) != size: return None, True
    except (OSError, ValueError, KeyError, TypeError, IndexError): return None, False
    return (h if (h.get('vk'), h.get('inputs'), h.get('raw'), h.get('val'), h.get('key')) == (TOOLS, INPUTS, RAW, VAL_ID, os.path.basename(path)[:-2]) else None), False


def result_head(path):
    """The JSON head of a whole result file of this build, else None (result_file)"""
    return result_file(path)[0]


def write_result(key, st, err, r, ps_in):
    """One shader's result, as fin() gives it, to <RES>/<key>.r: written to a temporary name, flushed to storage, then renamed, so a killed build or a phone that stops
    leaves a whole file or none (without the fsync a rename on the phone's shared storage can outlive the data: a file cut short under its full name, #82). A result reused
    from an older library (delta) has 'reused': that library's version in its head and its reflection"""
    refl, modules = r if r else (None, [])
    h = {'vk': TOOLS, 'inputs': INPUTS, 'raw': RAW, 'val': VAL_ID, 'key': key, 'stage': STAGE_OF[key], 'st': st, 'err': err, 'ps_inputs': sorted(ps_in) if ps_in is not None else None,
         'refl': refl, 'modules': [[variant, size, len(stored), codec] for variant, size, stored, codec, _ in modules]}
    if refl and refl.get('reused'): h['reused'] = refl['reused']
    j = json.dumps(h, separators=(',', ':')).encode()
    tmp = result_path(key) + f'.{os.getpid()}.tmp'
    with open(tmp, 'wb') as f:
        f.write(RESULT_MAGIC + struct.pack('<I', len(j)) + j)
        for m in modules: f.write(m[2])
        f.flush(); os.fsync(f.fileno())
    os.replace(tmp, result_path(key))


def read_result(key):
    """→ (key, status, error, (reflection, [(variant, size, stored, codec, SPIR-V or None)]) or None), as fin() returns it. The reflection has been through JSON, as index.json
    writes it, so a result read back and a result just made write the same library. The stored bytes stay in the file (Stored) unless DMSH_VK_LOOSE wants the SPIR-V"""
    p = result_path(key)
    with open(p, 'rb') as f:
        d = f.read(12); n = struct.unpack_from('<I', d, 8)[0]; h = json.loads(f.read(n)); o = 12 + n
        end = os.fstat(f.fileno()).st_size
        if not h['refl']: return key, h['st'], h['err'], None
        mods, loose = [], os.environ.get('DMSH_VK_LOOSE') == '1'
        for variant, size, ln, codec in h['modules']:
            if o + ln > end: raise ResultCut(p)   # cut after this run's check (result_file): the caller translates it again
            if loose:
                f.seek(o); stored = f.read(ln)
                mods.append((variant, size, stored, codec, zstd.decompress(stored) if codec == VK['CODEC_ZSTD'] else stored))
            else: mods.append((variant, size, Stored(p, o, ln), codec, None))
            o += ln
    return key, h['st'], h['err'], (h['refl'], mods)


_plock = threading.Lock()
PROGRESS = os.environ.get('DMSH_VK_PROGRESS'); DONE = [0]; TOTAL = [len(keys)]   # TOTAL: the shaders this version translates (all, less those a delta reuses)


def progress(stage, add=0):
    with _plock:
        DONE[0] += add
        print(f'progress {DONE[0]} {TOTAL[0]} {stage}', flush=True)
        if PROGRESS:
            tmp = PROGRESS + '.tmp'
            with open(tmp, 'w') as f: json.dump({'done': DONE[0], 'total': TOTAL[0], 'stage': stage, 'reused': len(keys) - TOTAL[0]}, f)
            os.replace(tmp, PROGRESS)


packs = {}
def index_packs():
    """The shader package's files opened, once: (descriptor, record offsets) each; a record is read when its job is made, no package kept in memory (dmsh_build.py record_index)"""
    if not packs:
        try: _bins = sorted(n for n in os.listdir(dm_dir) if n.endswith('.bin'))
        except OSError as e: sys.exit(f'[RED] shader package {dm_dir}: {e}')
        for _name in _bins: packs[_name] = NS['record_index'](os.path.join(dm_dir, _name))
def _rec(name, k):
    fd, offs = packs[name]
    return os.pread(fd, offs[k + 1] - offs[k], offs[k])
MISSING = set()   # keys the package does not hold, left out of this build (DMSH_VK_REQUIRE_ALL=0: open_packs)
def open_packs(ks):
    """The shader package opened (once) and the records of the shaders ks found: each record checked against its key, and found by it when the package is not the one
    the map was made from (locate). → the keys of ks found. A key the package does not hold stops the build, except with DMSH_VK_REQUIRE_ALL=0 (the phone's Try anyway:
    a game version the profile's map was not made from, which the player was warned may not draw everything): it is left out of this build, as a shader that did not
    build is left out of the library, and counted in one [RED] line"""
    index_packs()
    _files = {n: [(lambda n=n, j=j: _rec(n, j)) for j in range(len(offs) - 1)] for n, (_, offs) in packs.items()}
    _found, _bad = locate(ks, loc, _files, borrow_key(tuple(int(x, 0) for x in GAME.get('shader_key_skip', '0+0').split('+'))))
    if _bad and os.environ.get('DMSH_VK_REQUIRE_ALL') != '0':
        close_packs()
        sys.exit(f'[RED] {len(_bad)} of the {len(ks)} shaders to build are not in the package {dm_dir}: it is not a split of the game version the profile\'s shader_map.tsv was made from '
                 '(split the dump again: pack_android.py does it when the package is missing). ' + '; '.join(_bad[:5]))
    if _bad:
        _gone = {k for k in ks if k not in _found}
        MISSING.update(_gone); ORDER[:] = [k for k in ORDER if k not in MISSING]; TOTAL[0] -= len(_gone)
        print(f'[RED] {len(_gone)} of the {len(ks)} shaders to build are not in the package {dm_dir} (a game version the profile\'s shader_map.tsv was not made from): '
              'the library is written without them (DMSH_VK_REQUIRE_ALL=0). ' + '; '.join(_bad[:5]), flush=True)
    _moved = sum(1 for k in _found if _found[k] != loc[k])
    if _moved: print(f'{_moved} of the {len(ks)} shaders are at other places in this package than in the profile\'s shader_map.tsv (another split of the game): each is read from where its key is', flush=True)
    loc.update(_found)
    return [k for k in ks if k in _found]


def close_packs():
    for _fd, _ in packs.values(): os.close(_fd)
    packs.clear()


def fin(key, bd, job, c1, xerr):
    if key in xerr or not os.path.exists(os.path.join(bd, key + '.spv')): return key, 'translation failed', xerr.get(key, 'no spv'), None
    try: return (key,) + finish(key, bd, job, c1)
    except (ValueError, KeyError, IndexError, StopIteration) as e: return key, 'reflection failed', f'{type(e).__name__}: {e}'[:160], None


def stopped(): return False   # (a worker's says whether the parent wants it to stop: serve)


def run_batch(batch, count=True):
    """Translate a batch of shaders in one translator call, finish each (fin) and write its result file (count: they are in the progress's total)"""
    bd = tempfile.mkdtemp(prefix='b_', dir=w); jobs, c1s = {}, {}
    for key in batch:
        name, k = loc[key]
        jobs[key], c1s[key] = NS['job_for'](key, name, _rec(name, k), bd)
        jobs[key].update(ANSWERS)
        if STAGE_OF[key] == 'ms': jobs[key]['gl_depth_spec'] = VK['SPEC_GL_DEPTH']
    r = translate([jobs[k] for k in batch], bd, batch[0])
    xerr = {}
    for ln in r.stderr.splitlines():
        mm = re.match(r'\[RED\] (\w+): (.*)', ln)
        if mm: xerr[mm.group(1)] = mm.group(2)[:160]
    if r.returncode not in (0, 1):   # the translator itself failed (killed, crashed): nothing of this batch is a result
        shutil.rmtree(bd, ignore_errors=True)
        raise RuntimeError(f'translator exit code {r.returncode} on {len(batch)} shaders from {batch[0]}: ' + (r.stderr.strip().splitlines() or [''])[-1][:200])
    for key in batch:
        if stopped(): break   # a worker told to stop (Pool.close): the shaders left have no result, a rerun translates them
        res = fin(key, bd, jobs[key], c1s[key], xerr)
        if res[1] == 'translation failed' and key not in xerr: continue   # neither a module nor the translator's reason: not a result (a rerun translates it again)
        write_result(key, *res[1:], PS_INPUTS.get(key) if STAGE_OF[key] != 'vs' else None)
    shutil.rmtree(bd, ignore_errors=True)
    if count: progress('translate', len(batch))


# ---- worker interpreters (DMSH_VK_PROCS): the phone's app may not start programs or fork (android/ONDEVICE.md, No subprocesses), and the Python half holds the GIL, so
# the build's other cores come from sub-interpreters of this process (concurrent.interpreters, PEP 734), each with its own GIL. Each worker runs this script as the parent
# does, up to here (the same profile, tables and answers; no translator check, the parent made it), then serves batches: the parent's Pool sends each batch with every
# shader's package record ([file, record, offset, length]: the worker reads it from its own descriptor) and, for vertex shaders, the inputs of their pixel shaders
# (PS_INPUTS, which the parent takes from the result files as before); a worker writes the result files as a thread here does, so the library is the same.
# Messages are JSON strings: jobs [n, batch, {key: [file, record, offset, length]}, {pixel shader: inputs} or None]; results ['ready' | 'ok' | 'stopped' | 'err' | 'fail', n, text]
class Stop(Exception):
    """The parent asked the workers to stop (Pool.close): raised in a worker that waits for the translator gate"""


def worker_message(kind, n, text=''): return json.dumps([kind, n, text])


if WORKER:
    from concurrent.interpreters import QueueEmpty
    _STOP = [False]

    def stopped():
        """The parent put something on the stop queue (Pool.close): the batch in hand ends after the shader in hand, no other batch starts"""
        if not _STOP[0] and not WORKER['stop'].empty(): _STOP[0] = True
        return _STOP[0]

    class QueueGate:
        """XLATE_GATE across the build's interpreters: a queue holding XLATE_J tokens (Pool), one taken for each translator call and put back. Polled, so a worker
        waiting for the translator stops when asked (the queue's own get waits whole seconds only: it takes int(timeout))"""
        def __init__(self, q): self.q = q
        def __enter__(self):
            while True:
                try: self.q.get_nowait(); return
                except QueueEmpty:
                    if stopped(): raise Stop()
                    time.sleep(0.01)
        def __exit__(self, *a): self.q.put(1)
    XLATE_GATE = QueueGate(WORKER['gate'])

    def serve():
        """A worker's loop: WORKER['threads'] threads take batches from the jobs queue and run them (run_batch, no progress: the parent counts) until the parent stops them"""
        global w, _rec
        w = WORKER['w']; jobs, results = WORKER['jobs'], WORKER['results']
        fds, at, flock = {}, {}, threading.Lock()
        def rec(name, k):   # the package record the parent named (its offsets): this interpreter's own descriptor, no record_index of a package of several GB per worker
            off, n = at[(name, k)]
            with flock:
                if name not in fds: fds[name] = os.open(os.path.join(dm_dir, name), os.O_RDONLY)
            return os.pread(fds[name], n, off)
        _rec = rec
        def loop():
            while not stopped():
                try: item = jobs.get(timeout=1)
                except QueueEmpty: continue
                n, batch, where, ps_in = json.loads(item)
                try:
                    for key, (name, k, off, ln) in where.items(): loc[key] = (name, k); at[(name, k)] = (off, ln)
                    if ps_in: PS_INPUTS.update((p, set(v)) for p, v in ps_in.items())
                    run_batch(batch, count=False)
                    results.put(worker_message('stopped' if stopped() else 'ok', n))
                except Stop: results.put(worker_message('stopped', n))
                except BaseException as e:   # the parent raises it (as a thread's exception ended the build before)
                    import traceback
                    results.put(worker_message('err', n, f'{type(e).__name__}: {e}\n' + ''.join(traceback.format_exception(e))[-1500:]))
        ts = [threading.Thread(target=loop) for _ in range(max(1, WORKER['threads']))]
        results.put(worker_message('ready', WORKER['id']))
        for t in ts: t.start()
        for t in ts: t.join()
        for fd in fds.values(): os.close(fd)

    serve()
    raise SystemExit(0)


# ---- delta update (android/SHADERS.md#delta-updates): the profile's manifest of this version gives every shader's entry hash; an entry of an older library of this game
# with that hash is reused as it is (a result file that names its library), and only the other shaders are translated. Every entry of the new library is checked against
# the manifest at the end. A key list (a profile's selected set, as the phone passes it) uses the manifest as a full build does, for the keys it has (vk_android_delta.py
# coverage: the profile's selection as published; keys it lacks, a selection changed since, are translated and not checked). No manifest of this version (a Mac build after
# a tools change, before the manifest is made), no older library or DMSH_VK_RAW: a full build
MANIFEST_PATH = str(GAME.table(delta.MANIFEST))


def load_manifest():
    """(the profile's manifest when it is of this version, game and inputs, else None; why not)"""
    try: m = delta.parse_manifest(open(MANIFEST_PATH).read())
    except FileNotFoundError: return None, f'no {MANIFEST_PATH}'
    except (OSError, ValueError) as e: return None, f'{MANIFEST_PATH}: {e}'
    if (m['version'], m['game'], m['inputs']) != (VKV, GAME.id, INPUTS):
        return None, f'{MANIFEST_PATH} is of version {m["version"]}, game {m["game"]}, inputs {m["inputs"]}; this build is {VKV}, {GAME.id}, {INPUTS}'
    return m, ''


def older_libraries():
    """The libraries a delta may take entries from: DMSH_VK_DELTA_FROM (paths separated by os.pathsep, each a dmsh_vk.lib, a v-<V> folder or a folder of them; the app passes
    the pack's shaders/), else the other v-<V> folders of the output's cache folder; only libraries of this game in a format this tool reads, newest first"""
    roots = [p for p in os.environ.get('DMSH_VK_DELTA_FROM', '').split(os.pathsep) if p] or [cache_root]
    files = []
    for r in roots:
        if os.path.isfile(r): files.append(r); continue
        if os.path.isfile(os.path.join(r, VK['LIB_FILE'])): files.append(os.path.join(r, VK['LIB_FILE'])); continue
        try: subs = os.listdir(r)
        except OSError: continue
        files += [os.path.join(r, n, VK['LIB_FILE']) for n in subs if n.startswith(VK['DIR_PREFIX']) and os.path.isfile(os.path.join(r, n, VK['LIB_FILE']))]
    libs = []
    for f in sorted(set(os.path.realpath(f) for f in files)):
        if os.path.dirname(f) == os.path.realpath(out): continue   # this version's own library (an earlier run's): its entries are results already
        try: L = delta.Library(f)
        except (OSError, ValueError, struct.error) as e: print(f'delta: {f} skipped: {e}', flush=True); continue
        if L.game != GAME.id: L.close(); continue
        libs.append(L)
    return sorted(libs, key=lambda L: -os.stat(L.path).st_mtime)


if not want:   # a build of a key list may run beside a full one: it leaves the other's unfinished files alone
    for _n in os.listdir(RES):   # what a killed build left: unfinished result files
        if _n.endswith('.tmp'):
            try: os.unlink(os.path.join(RES, _n))
            except OSError: pass
heads, _cut = {}, []
for k in keys:
    h, c = result_file(result_path(k))
    if h: heads[k] = h
    elif c: _cut.append(k)
if _cut: print(f'[RED] {len(_cut)} result files are cut short (from {_cut[0]}, a stop that storage did not keep whole): translated again', flush=True)
MANIFEST, _why = (None, 'DMSH_VK_RAW') if RAW else load_manifest()
REUSED = {k for k, h in heads.items() if h.get('reused')}
_in_m, _out_m = delta.coverage(MANIFEST, keys)
if MANIFEST and _out_m:   # a manifest of another key set: only the keys it has are kept and checked
    print(f'delta: {len(_out_m)} of the {len(keys)} shaders of this build are not in the manifest (another selection): translated, not checked, from {_out_m[0]}', flush=True)
if MANIFEST and want and len(_in_m) < len(MANIFEST['hashes']):
    print(f'delta: the manifest has {len(MANIFEST["hashes"]) - len(_in_m)} shaders this key list does not build', flush=True)
if MANIFEST:
    _cand = delta.reuse_candidates(MANIFEST, keys, heads)
    _libs = older_libraries() if _cand else []
    def _reuse(k):
        L, ent, errs = delta.find_entry(MANIFEST, k, _libs, zstd)
        if L is None: return k, None, '; '.join(errs) or None
        _, e, b, c1, mods = ent
        refl = {'stage': STAGE_OF[k], 'reused': L.version, 'lib': {'entry': delta.zeroed(e).hex(), 'binds': b.hex(), 'c1': c1.hex()}}
        write_result(k, 'ok', '', (refl, [(v, size, bytes(st), codec, None) for v, size, st, codec in mods]), None)
        return k, L.version, None
    _from = collections.Counter()
    with cf.ThreadPoolExecutor(NJ) as ex:
        for k, v, err in ex.map(_reuse, [k for k in _cand if any(k in L.index for L in _libs)]):
            if err: print(f'[RED] delta: {k}: the older library\'s entry cannot be read ({err}): translated', flush=True)
            if v:
                _from[v] += 1; REUSED.add(k)
                heads[k] = {'reused': v, 'ps_inputs': None}
    for L in _libs: L.close()
    if not _libs and not REUSED: print('delta: no older library of this game: a full build', flush=True)
    else: print(f'delta: {len(REUSED)} of {len(keys)} shaders kept (' + (', '.join(f'from v-{v}: {n}' for v, n in _from.most_common()) or 'by an earlier run of this build')
                + f'); {len(keys) - len(REUSED)} to translate, {sum(1 for k in keys if k not in heads)} of them left', flush=True)
else: print(f'delta: none ({_why}): a full build', flush=True)
todo = [k for k in ORDER if k not in heads]
TOTAL[0] = len(keys) - len(REUSED); DONE[0] = TOTAL[0] - len(todo)
print(f'translating {len(todo)} shaders of {len(keys)} ({len(keys) - len(todo)} already in {RES}' + (f', {len(REUSED)} of them kept from an older library' if REUSED else '')
      + f'; {NJ} at a time, {BATCH} a translator call)', flush=True)
progress('translate')
_wk = os.environ.get('WORK')
if _wk: w = tempfile.mkdtemp(prefix='dmshvk_', dir=_wk)
else:   # in the result folder: a killed build's leftovers go with the next run
    w = os.path.join(RES, '.work'); shutil.rmtree(w, ignore_errors=True); os.makedirs(w)
# DMSH_VK_PROCS worker interpreters (default 1: this process's threads only). Workers need the library forms of the translator and the validator: a sub-interpreter
# may not start programs. Each worker runs J / PROCS threads (at least one)
PROCS = max(1, int(os.environ.get('DMSH_VK_PROCS') or '1'))
if PROCS > 1 and not (XLATE_LIB and isinstance(VALIDATE, SpirvValLib)):
    print(f'DMSH_VK_PROCS={PROCS}: the translator or the validator is a program, which a worker interpreter cannot start: threads of this process only', flush=True); PROCS = 1
WORKER_SETUP = '''if 1:
    import json, os, sys
    sys.argv, sys.path[:] = json.loads(argv), json.loads(path)
    _env = json.loads(env)   # the parent's os.environ (the phone's runner sets it in Python; a new interpreter's os reads the C environment)
    for _k in set(os.environ) - set(_env): del os.environ[_k]
    for _k, _v in _env.items():
        if os.environ.get(_k) != _v: os.environ[_k] = _v
    sys.stdout = sys.stderr = open(os.devnull, 'w')   # the parent prints the build's lines once
'''
WORKER_RUN = '''if 1:
    import json, runpy, traceback
    try:
        runpy.run_path(script, run_name='__main__', init_globals={'VK_BUILD_WORKER': dict(json.loads(cfg), gate=gate, jobs=jobs, results=results, stop=stop)})
        results.put(json.dumps(['fail', -1, 'the worker ended without serving']))
    except SystemExit as e:
        if e.code not in (0, None): results.put(json.dumps(['fail', -1, f'exit: {e.code}']))
    except BaseException as e:
        results.put(json.dumps(['fail', -1, ''.join(traceback.format_exception(e))[-1500:]]))
'''


class WorkerSetupFailed(Exception):
    """A worker interpreter could not be made or did not reach its loop (a Python without sub-interpreters, a module that refuses them): the build goes on with threads"""


class Pool:
    """n worker interpreters (serve) of t threads each, made one at a time; run() sends batches and waits for them, close() stops the workers after the shaders in hand
    (a Pause: KeyboardInterrupt in this thread) and closes the interpreters. A worker's error ends the build as a thread's did"""
    def __init__(self, n, t):
        self.n, self.t, self.interps, self.threads, self.closed, self.stop = n, t, [], [], False, None
        try:
            from concurrent import interpreters as ci
            self.gate, self.jobs, self.results, self.stop = (ci.create_queue() for _ in range(4))
            for _ in range(XLATE_J): self.gate.put(1)
            env, argv, path = json.dumps(dict(os.environ)), json.dumps(sys.argv), json.dumps(sys.path)
            for i in range(n):
                it = ci.create(); self.interps.append(it)
                it.prepare_main(argv=argv, path=path, env=env, script=os.path.abspath(__file__), cfg=json.dumps({'id': i, 'w': w, 'threads': t}),
                                gate=self.gate, jobs=self.jobs, results=self.results, stop=self.stop)
                it.exec(WORKER_SETUP)   # one at a time: the environment is the process's
            for it in self.interps:
                th = threading.Thread(target=self.serve, args=(it,)); th.start(); self.threads.append(th)
            ready = 0
            while ready < n:
                kind, _, text = self.next()
                if kind == 'fail': raise WorkerSetupFailed(text.strip().splitlines()[-1] if text.strip() else 'no reason')
                ready += kind == 'ready'
        except Exception as e:
            self.close()
            raise WorkerSetupFailed(f'{type(e).__name__}: {e}') from e
        except BaseException:   # a stop while the workers start
            self.close(); raise

    def serve(self, it):
        try: it.exec(WORKER_RUN)
        except Exception as e: self.results.put(worker_message('fail', -1, f'{type(e).__name__}: {e}'))

    def next(self):
        """The next message from the workers, polled (a KeyboardInterrupt reaches this thread between polls)"""
        from concurrent.interpreters import QueueEmpty
        while True:
            try: return json.loads(self.results.get_nowait())
            except QueueEmpty: pass
            if not any(th.is_alive() for th in self.threads):
                try: return json.loads(self.results.get_nowait())
                except QueueEmpty: raise RuntimeError('every shader worker ended') from None
            time.sleep(0.01)

    def run(self, batches, count):
        """Each batch translated by a worker; at most two batches a worker thread are queued, so a stop leaves the others unsent"""
        sent = done = 0
        def send():
            nonlocal sent
            b = batches[sent]; where = {}
            for k in b:
                name, i = loc[k]; offs = packs[name][1]
                where[k] = [name, i, offs[i], offs[i + 1] - offs[i]]
            ps_in = {p: sorted(PS_INPUTS[p]) for k in b for p in PAIRS.get(k, ()) if p in PS_INPUTS} if STAGE_OF[b[0]] == 'vs' else None
            self.jobs.put(json.dumps([sent, b, where, ps_in])); sent += 1
        while sent < min(len(batches), 2 * self.n * self.t): send()
        while done < len(batches):
            kind, i, text = self.next()
            if kind == 'err': raise RuntimeError(f'shader worker, the batch from {batches[i][0]}: {text}')
            if kind == 'fail': raise RuntimeError(f'shader worker ended: {text}')
            if kind != 'ok': raise RuntimeError(f'shader worker: {kind} for the batch from {batches[i][0]}')   # ('stopped': only after close)
            done += 1
            if count: progress('translate', len(batches[i]))
            if sent < len(batches): send()

    def close(self):
        """Stop the workers (each ends after the shader in hand; a rerun translates what has no result) and close their interpreters"""
        if self.closed: return
        self.closed = True
        if self.stop is not None: self.stop.put(1)
        for th in self.threads:
            while th.is_alive(): th.join(0.1)
        for it in self.interps:
            try: it.close()
            except Exception as e: print(f'[RED] shader worker interpreter not closed: {type(e).__name__}: {e}', flush=True)


POOL = []   # the worker pool, made at the first translation with batches for it (translate_keys), closed with the package (the build's finally)


def pool_for(nbatches):
    """The worker pool (made once), or None for this process's threads: DMSH_VK_PROCS 1, a single batch and no pool yet, or workers that could not start"""
    if not POOL and (PROCS < 2 or nbatches < 2): return None
    if not POOL:
        try:
            POOL.append(Pool(min(PROCS, nbatches), max(1, NJ // PROCS)))
            print(f'shader workers: {POOL[0].n} interpreters of {POOL[0].t} threads, {XLATE_J} translator calls at once', flush=True)
        except WorkerSetupFailed as e:
            print(f'[RED] shader workers (DMSH_VK_PROCS={PROCS}) did not start ({e}): threads of this process only', flush=True); POOL.append(None)
    return POOL[0]


def close_pool():
    if POOL and POOL[0]: POOL[0].close()
    POOL.clear()


def translate_keys(ks, count=True):
    """Translate the shaders ks (pixel shaders first, then the vertex shaders their inputs pad) and write their result files"""
    for _phase in ([k for k in ks if STAGE_OF[k] != 'vs'], [k for k in ks if STAGE_OF[k] == 'vs']):
        if _phase and STAGE_OF[_phase[0]] == 'vs':   # the pixel shaders' inputs, from their results (this run's and earlier runs')
            _need = set().union(*[PAIRS.get(k, ()) for k in _phase])
            for k in keys:
                if STAGE_OF[k] != 'vs' and k not in PS_INPUTS:
                    h = result_head(result_path(k))
                    if h and h['ps_inputs'] is not None: PS_INPUTS[k] = set(h['ps_inputs'])
                    elif h and h.get('reused') and k in _need:   # kept from an older library: the inputs its base module reads, as finish() takes them
                        try: _r = read_result(k)[3]
                        except ResultCut:   # cut since the check: translated here, which gives its inputs (finish)
                            print(f'[RED] {result_path(k)} is cut short: translated again', flush=True)
                            os.unlink(result_path(k))
                            if open_packs([k]): run_batch([k], False)
                            continue
                        _b = next(m for m in _r[1] if m[0] == VK['MOD_BASE'])
                        PS_INPUTS[k] = set(spv.Module(delta.spirv(stored_bytes(_b[2]), _b[3], zstd)).io(spv.SC_INPUT))
        batches = [_phase[i:i + BATCH] for i in range(0, len(_phase), BATCH)]
        if not batches: continue
        pool = pool_for(-(-len(ks) // BATCH))
        if pool: pool.run(batches, count); continue
        with cf.ThreadPoolExecutor(NJ) as ex:
            for _ in ex.map(lambda b: run_batch(b, count), batches): pass


def read_all():
    """(every shader's result, the keys whose result files are cut short)"""
    _missing = [k for k in ORDER if not os.path.exists(result_path(k))]
    if _missing: sys.exit(f'[RED] {len(_missing)} shaders have no result (the translator wrote neither a module nor a reason for them, from {_missing[0]}): run again')
    rs, cut = [], []
    for k in ORDER:
        try: rs.append(read_result(k))
        except ResultCut: cut.append(k)
    return rs, cut


try:   # the package descriptors are closed however the translation ends (the phone runs this in a long-lived interpreter)
    if todo: translate_keys(open_packs(todo))
    results, _cut = read_all()
    if _cut:   # a result file cut after the check at the start (#82): translated again, not a stop
        print(f'[RED] {len(_cut)} result files are cut short (from {_cut[0]}): translated again', flush=True)
        for k in _cut: os.unlink(result_path(k))
        translate_keys(open_packs(_cut), False)
        results, _cut = read_all()
        if _cut: sys.exit(f'[RED] {len(_cut)} result files are still cut short after translating them again (from {result_path(_cut[0])}): the storage does not keep whole files')
finally:
    close_pool(); close_packs()
shutil.rmtree(w, ignore_errors=True)
progress('library')
_bad = [r for r in results if r[1] != 'ok']
if _bad and os.environ.get('DMSH_VK_REQUIRE_ALL') == '1':   # the phone's job: a library without every shader the Mac's build of this version holds must not pass as finished
    # a key list (a profile's selected set, shader_seen.tsv) too: the card said Ready with 3,384 of Pokopia's 8,157 failed (docs/reference/shaders.md, A library with failed
    # shaders passed as finished). Only the manifest's "-" keys are excused: a key it lacks (another selection) must build
    _excuse = MANIFEST or load_manifest()[0]   # (DMSH_VK_RAW: no reuse, but the Mac's "-" shaders still did not build there)
    _there = delta.not_built_there(_excuse, [r[0] for r in _bad])   # "-" in the profile's manifest of this version: they did not build in the Mac's full build either
    if _there: print(f'{len(_there)} shaders did not build, as in the Mac\'s build of this version (the profile\'s {delta.MANIFEST} has them as "-"): ' + ', '.join(sorted(_there)[:5]), flush=True)
    _bad = [r for r in _bad if r[0] not in _there]
    for lvl, n in collections.Counter(r[1] for r in _bad).items(): print(f'[RED] {lvl}: {n} shaders, e.g. {next(r for r in _bad if r[1] == lvl)[0]}: ' + next(r for r in _bad if r[1] == lvl)[2])
    if _bad: sys.exit(f'[RED] {len(_bad)} of {len(results)} shaders did not build: no library written (DMSH_VK_REQUIRE_ALL)')
if not want:
    for n in os.listdir(out):
        if n == 'spv': shutil.rmtree(os.path.join(out, n))
lib, size = write_library(results)
if os.environ.get('DMSH_VK_LOOSE') == '1':
    os.makedirs(os.path.join(out, 'spv'), exist_ok=True)
    for key, st, _, r in results:
        if st == 'ok':
            for variant, _, _, _, data in r[1]: open(os.path.join(out, 'spv', key + (f'_{variant}' if variant else '') + '.spv'), 'wb').write(data)
index = {k: r[0] for k, st, _, r in results if st == 'ok'}   # a shader kept from an older library has its stage and that library's version here, not its whole reflection
for v in index.values():
    for b in v.get('bindings', ()): b['kind_num'] = KIND_NUM[b['kind']]
json.dump({'chain': CHAIN, 'vk_chain': VKV, 'game': GAME.id, 'inputs': INPUTS, 'shaders': index}, open(os.path.join(out, 'index.json'), 'w'), separators=(',', ':'))
json.dump([{'key': k, 'st': st, 'err': e} for k, st, e, _ in results], open(os.path.join(out, '_vk_build.json'), 'w'))
# The new library read back and every entry hashed: vk_delta.tsv beside it (what a Mac build gives the profile: vk_android_build.py --publish-delta), and with a manifest of this
# version, each entry checked against it: a mismatch or a shader the manifest has and the library lacks is a [RED] line naming the key (the library is kept: it is what this
# machine's tools made). A full build with a manifest checks the same, so a phone's full build is compared with the Mac's too
progress('check')
_lib, _hashes = delta.library_hashes(lib, zstd, NJ); _lib.close()
_status = {k: st for k, st, _, _ in results}
open(os.path.join(out, delta.MANIFEST + '.tmp'), 'w').write(delta.format_manifest(VKV, GAME.id, INPUTS, {k: _hashes.get(k, '-') for k in keys}))
os.replace(os.path.join(out, delta.MANIFEST + '.tmp'), os.path.join(out, delta.MANIFEST))
if MANIFEST:
    _, _, _diff = delta.check(MANIFEST, keys, _hashes)
    for k, want_h, got_h in _diff:
        if k in MISSING: continue   # not in the package: counted once below (open_packs said so)
        how = 'kept from an older library' if k in REUSED else 'translated here'
        if got_h is None: print(f'[RED] delta check: {k}: not in the library ({_status.get(k)}: {next((r[2] for r in results if r[0] == k), "")[:100]}), the manifest has {want_h}', flush=True)
        elif want_h == '-': print(f'[RED] delta check: {k}: in the library ({how}), the manifest has it as not built', flush=True)
        else: print(f'[RED] delta check: {k}: library entry {got_h} ({how}), the manifest has {want_h}', flush=True)
    print(f'delta check: {len(_in_m) - len(_diff)} of {len(_in_m)} shaders as the manifest has them ({len(REUSED)} kept, {len(keys) - len(REUSED)} translated)'
          + (f', {len(_diff)} differ' if _diff else '') + (f' ({_gone} of them not in the package)' if (_gone := sum(1 for d in _diff if d[0] in MISSING)) else '')
          + (f'; {len(_out_m)} not in the manifest, not checked' if _out_m else ''), flush=True)
if not want:
    dmsh_chain.write_stamp(out, GAME.id, INPUTS)
if os.environ.get('DMSH_VK_KEEP') == '0': shutil.rmtree(RES, ignore_errors=True)
progress('done')

# ---- summary
st = collections.Counter(r[1] for r in results)
okr = [r[3][0] for r in results if r[1] == 'ok' and not r[3][0].get('reused')]   # reflections made here (a kept entry's is in the older library)
if REUSED: print(f'kept from older libraries: {len(REUSED)} shaders (the counts below are of the {len(okr)} translated here)')
print(f'shaders {len(results)}: ok {st["ok"]}, ' + ', '.join(f'{k} {v}' for k, v in st.items() if k != 'ok'))
print('  by stage: ' + ', '.join(f'{s} {n}/{m}' for s, m in sorted(collections.Counter(STAGE_OF[r[0]] for r in results).items())
                                  for n in [sum(1 for r in results if r[1] == 'ok' and STAGE_OF[r[0]] == s)]))
for lvl in [k for k in st if k != 'ok']:
    for e, n in collections.Counter(re.sub(r'\b[0-9a-f]{16}\b|%\d+|\d+', '#', r[2]) for r in results if r[1] == lvl).most_common(10): print(f'  [{lvl}] {n:5d}  {e}')
mods = sum(len(r[3][1]) for r in results if r[1] == 'ok'); raw = sum(m[1] for r in results if r[1] == 'ok' for m in r[3][1])
print(f'modules {mods}, every one valid for spirv-val --target-env vulkan1.3 ({sum(1 for x in okr if x["flags"] & VK["F_GL_DEPTH"])} with the GL depth constant); library {lib}: {size / 1e6:.1f} MB ({raw / 1e6:.1f} MB of SPIR-V before compression)')
fl = collections.Counter(n for x in okr for n, v in VK.items() if n.startswith('F_') and x['flags'] & v)
print('flags: ' + ', '.join(f'{n[2:].lower()} {fl[n]}' for n in sorted(fl)))
_vn = {v: n[4:].lower() for n, v in VK.items() if n.startswith('MOD_')}
print('modules by variant: ' + ', '.join(f'{_vn.get(v, v)} {n}' for v, n in sorted(collections.Counter(v for x in okr for v in x['variants']).items())) +
      f'; translator answers: subgroup size {ANSWERS["subgroup_size"]}, storage buffer alignment {ANSWERS["ssbo_align"]}, geometry passthrough by the shader, '
      f'gather bias {ANSWERS["gather_bias"]} bits')
print('capabilities: ' + ', '.join(f'{c} {n}' for c, n in collections.Counter(c for x in okr for c in x['caps']).most_common()))
print('extensions: ' + (', '.join(f'{c} {n}' for c, n in collections.Counter(c for x in okr for c in x['exts']).most_common()) or 'none'))
kinds = collections.Counter(KIND[b['kind']] + (('+' + KIND[b['skind']]) if b.get('skind') else '') for x in okr for b in x['bindings'])
print('binding kinds: ' + ', '.join(f'{k.lower()} {n}' for k, n in kinds.most_common()))
mx = collections.defaultdict(int)
for x in okr: mx[x['stage']] = max(mx[x['stage']], sum(1 for b in x['bindings'] if b['set'] == VK['SET_DRAW']))
print('most set-0 bindings in one shader: ' + ', '.join(f'{s} {n}' for s, n in sorted(mx.items())) + f' (range {VK["STAGE_BINDINGS"]} a stage)')
noreg = [x for x in okr if any(b['flags'] & VK['BF_NOSLOT'] for b in x['bindings'])]
unk = [x for x in okr if any(b['kind'] == abi.KIND_UNKNOWN or b.get('skind') == abi.KIND_UNKNOWN for b in x['bindings'])]
print(f'shaders with a binding whose slot was not decoded {len(noreg)}, with an unrecognized binding kind {len(unk)}')
for e, n in collections.Counter(f"{KIND[b['kind']]} {b['name'][:28]}" for x in noreg + unk for b in x['bindings'] if b['flags'] & VK['BF_NOSLOT'] or b['kind'] == abi.KIND_UNKNOWN or b.get('skind') == abi.KIND_UNKNOWN).most_common(8): print(f'  {n:5d}  {e}')
print(f'min / max / clamp made NaN-exact (NMin / NMax / NClamp): {sum(x["precise"] for x in okr)} in {sum(1 for x in okr if x["precise"])} shaders')
dr = collections.Counter(b for x in okr for b in x['drv_reads'])
print('driver segment reads: ' + (', '.join(f'c{b} in {n} shaders' for b, n in sorted(dr.items())) or 'none') +
      f'; vertex shaders reading the base instance word c0 + {abi.C0_BASE_INSTANCE:#x}: {sum(1 for x in okr if x["stage"] == "vs" and abi.C0_BASE_INSTANCE in x["drv_reads"].get("0", {}).get("offsets", ()))}' +
      f'; c0 read at a computed index: {sum(1 for x in okr if x["drv_reads"].get("0", {}).get("dynamic"))}')
print(f'64-bit float types left: {sum(1 for x in okr if "Float64" in x["caps"])} shaders; FP64 lowered to FP32 by the translator: {sum(1 for x in okr if x["flags"] & VK["F_SOFTFLOAT64"])}')
print(f'vertex shaders padded for their pixel shaders: {sum(1 for x in okr if x["vs_pad"])}; integer-only inputs retyped: {sum(1 for x in okr for t in x["vin"].values() if t)} inputs')
hm = collections.Counter(n for x in okr for n in x['hmma_fns'])
if hm: print('matrix helpers (cooperative matrix candidates): ' + ', '.join(f'{n} in {c} shaders' for n, c in hm.items()))
if hm: print(f"HMMA helper calls lowered at their sites (hmma_lds): {sum(x.get('hmma_lds') or 0 for x in okr)} in {sum(1 for x in okr if x.get('hmma_lds'))} shaders")
print(f"compute modules with their shuffles through workgroup memory (MOD_SHUFFLE_LDS): {sum(1 for x in okr if VK['MOD_SHUFFLE_LDS'] in x['variants'])}")
print(f'library: game {GAME.id}, inputs {INPUTS}, chain {CHAIN}, vk chain {VKV}' + (f' (tools {TOOLS}, vk_android_same.tsv)' if TOOLS != VKV else ''))
