"""The Android shader library's delta update (android/SHADERS.md#delta-updates): vk_android_delta.py's entry hash, manifest and ABI digest, the profiles' committed
manifests (hashes only: the leak check), and the launcher's side of it on the JVM (ShaderLibs.java with android/app/src/jvmtest's ShaderLibsJvmTest: which older library
plays while the update runs, how many shaders an update translates). The libraries here are small ones written with the header layout of dmsh_abi_vk.h. No game files;
the JVM part is skipped without a JDK. The whole delta (a version A library upgraded to B, byte-identical to a full B build) is the Mac simulation in SHADERS.md."""
import ast
import collections
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
import vk_android_delta as delta   # noqa: E402

try: from compression import zstd
except ImportError:
    try: from backports import zstd   # pyright: ignore[reportMissingImports]
    except ImportError: zstd = None

SRC = REPO / 'android/app/src/main/java/dev/nxnative/android/ShaderLibs.java'
TEST = REPO / 'android/app/src/jvmtest/java/dev/nxnative/android/ShaderLibsJvmTest.java'
VK = delta.VK
GAME = '040074A01BF12000'


def spirv(n, seed):   # a SPIR-V-looking module of n words
    return struct.pack('<5I', 0x07230203, 0x00010300, 0, 8, 0) + bytes((seed * 7 + i) & 0xff for i in range(4 * n - 20))


def entry(key, stage=4, nmods=1, nb=1, c1=b'', m0=0, b0=0, c1_off=0):
    return delta.ENTRY.pack(int(key, 16), 0, stage, nmods, 0x40, m0, b0, nb, 0, 1, 1, 1, 1, 0, 0, c1_off, len(c1), 0)


def bind(n): return struct.pack('<BBHBBHhHBBhHHI', 1, 6, 4 * 64 + n, 0, 0, 1, n, 0, 0, 0, -1, 0xFFFF, 0, 0)


def borrowed(*names):
    """Functions of dmsh_build.py by name (vk_android_build.py takes them the same way: borrow)"""
    src = (REPO / 'dm/gfx/tools/dmsh_build.py').read_text()
    ns = {'os': os, 'collections': collections}
    exec(compile(ast.Module(body=[n for n in ast.parse(src).body if isinstance(n, ast.FunctionDef) and n.name in names], type_ignores=[]), 'dmsh_build.py', 'exec'), ns)
    return [ns[n] for n in names]


def profile_keys(prof):
    """The keys a phone builds for a profile folder: delta.phone_keys with the profile's map, selected set and pairs"""
    load_pairs, build_keys = borrowed('load_pairs', 'build_keys')
    mapk = {c[3] for c in (l.rstrip('\n').split('\t') for l in (prof / 'shader_map.tsv').read_text().splitlines()) if len(c) > 4 and c[0].endswith('.bin')}
    seen = prof / delta.SELECTED
    g = type('G', (), {'table': lambda self, t: prof / t})()
    return delta.phone_keys(mapk, seen.read_text() if seen.exists() else None, load_pairs(g, str(prof / 'shader_map.tsv')), build_keys)


def write_lib(path, version, game=GAME, abi='', shaders=(), codec=1):
    """A library with shaders [(key, [SPIR-V modules], c1)], as write_library lays it out (entries sorted, then bindings, modules, the blob 16-aligned)"""
    entries, binds, mods, blob = bytearray(), bytearray(), bytearray(), bytearray()
    nb = nm = 0
    def put(b):
        nonlocal blob
        blob += b'\0' * (-len(blob) % 16); o = len(blob); blob += b; return o
    for key, ms, c1 in sorted(shaders):
        c1o = put(c1) if c1 else 0; m0, b0 = nm, nb
        for v, s in enumerate(ms):
            st = zstd.compress(s) if codec == 1 else s
            mods += delta.MOD.pack(put(st), len(s), len(st), v, codec, 0); nm += 1
        binds += bind(0); nb += 1
        entries += entry(key, nmods=len(ms), c1=c1, m0=m0, b0=b0, c1_off=c1o)
    eo = 128; bo = eo + len(entries); mo = bo + len(binds); blo = (mo + len(mods) + 15) // 16 * 16
    pad = lambda s: s.encode().ljust(16, b'\0')
    head = delta.HEAD.pack(VK['LIB_MAGIC'], VK['LIB_VERSION'], pad('a139922bad5c'), pad(version), pad(game), pad('5dd843bed8d1'), len(shaders), eo, nb, bo, nm, mo,
                           blo, len(blob), 64, 64, abi.encode().ljust(12, b'\0'))
    data = head + entries + binds + mods
    data += b'\0' * (blo - len(data)) + blob
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_bytes(data)


@unittest.skipIf(zstd is None, 'no zstd in this Python')
class EntryHash(unittest.TestCase):
    def setUp(self): self.tmp = Path(tempfile.mkdtemp(prefix='vkdelta_'))
    def tearDown(self): shutil.rmtree(self.tmp, ignore_errors=True)

    def test_hash_ignores_positions_codec_and_header(self):
        a, b = spirv(40, 1), spirv(60, 2)
        sh = [('00000000000000aa', [a], b'\x01' * 16), ('00000000000000bb', [b, spirv(60, 3)], b'')]
        write_lib(self.tmp / 'A.lib', '111111111111', shaders=sh)
        write_lib(self.tmp / 'B.lib', '222222222222', abi='0123456789ab', shaders=sh[::-1] + [('0000000000000001', [spirv(10, 9)], b'\x02' * 8)], codec=0)   # another order of the blob, raw
        _, ha = delta.library_hashes(str(self.tmp / 'A.lib'), zstd)
        L, hb = delta.library_hashes(str(self.tmp / 'B.lib'), zstd)
        self.assertEqual((L.version, L.game, L.abi), ('222222222222', GAME, '0123456789ab'))
        self.assertEqual(ha['00000000000000aa'], hb['00000000000000aa'])
        self.assertEqual(ha['00000000000000bb'], hb['00000000000000bb'])
        self.assertEqual(delta.to_translate(ha, hb), ['0000000000000001'])
        self.assertEqual(delta.Library(str(self.tmp / 'A.lib')).abi, '', 'a library made before the field has no ABI')

    def test_damaged_module_is_a_value_error(self):
        """A damaged module in an older library makes that entry one to translate, never a stop: a frame zstd refuses is a ValueError (ZstdError is none), and one it
        decodes anyway (the frames carry no checksum) gives other SPIR-V, so another hash"""
        write_lib(self.tmp / 'A.lib', '111111111111', shaders=[('00000000000000aa', [spirv(400, 1)], b'')])
        L = delta.Library(str(self.tmp / 'A.lib')); good = L.hash_of(0, zstd)[1]
        off, size, stored, *_ = delta.MOD.unpack_from(L.m, L.mo); L.close()
        outcomes = set()
        for at in range(stored):
            d = bytearray((self.tmp / 'A.lib').read_bytes()) if at == 0 else d
            d0 = bytes(d); d[L.blob_off + off + at] ^= 0x5a; (self.tmp / 'B.lib').write_bytes(bytes(d)); d = bytearray(d0)
            L2 = delta.Library(str(self.tmp / 'B.lib'))
            try: outcomes.add('other hash' if L2.hash_of(0, zstd)[1] != good else 'SAME')
            except ValueError: outcomes.add('refused')
            finally: L2.close()
        self.assertNotIn('SAME', outcomes)
        self.assertIn('refused', outcomes)

    def test_hash_sees_every_part(self):
        e, bd, c1, m = entry('00000000000000aa'), bind(0), b'\x01' * 16, [(0, spirv(40, 1))]
        h = delta.entry_hash(e, bd, c1, m)
        self.assertEqual(h, delta.entry_hash(entry('00000000000000aa', m0=7, b0=9, c1_off=48), bd, c1, m), 'positions in the file are not part of an entry')
        for other in (delta.entry_hash(entry('00000000000000aa', stage=0), bd, c1, m), delta.entry_hash(e, bind(1), c1, m), delta.entry_hash(e, bd, b'\x02' * 16, m),
                      delta.entry_hash(e, bd, c1, [(1, m[0][1])]), delta.entry_hash(e, bd, c1, [(0, spirv(40, 2))]), delta.entry_hash(e, bd, c1, m + [(1, spirv(8, 1))])):
            self.assertNotEqual(h, other)

    def test_high_offset_bits(self):
        """Offsets past 4 GiB (a blob as Pokemon Pokopia's, 9.8 GB) keep their high bits in the module's off_hi and the entry's c1_off_hi: read as part of the offset
        (here they point past this small blob, so the entry is refused), and not part of the hash, as the low bits are not"""
        write_lib(self.tmp / 'A.lib', '111111111111', shaders=[('00000000000000aa', [spirv(40, 1)], b'\x01' * 16)])
        L = delta.Library(str(self.tmp / 'A.lib')); L.entry(0); mo, eo = L.mo, L.eo; L.close()
        for at in (mo + 14, eo + 56):   # off_hi, c1_off_hi
            d = bytearray((self.tmp / 'A.lib').read_bytes()); struct.pack_into('<H', d, at, 1); (self.tmp / 'B.lib').write_bytes(bytes(d))
            L2 = delta.Library(str(self.tmp / 'B.lib'))
            with self.assertRaises(ValueError): L2.entry(0)
            L2.close()
        self.assertEqual(delta.mod_off(0x10, 2), 0x200000010)
        e = bytearray(entry('00000000000000aa', c1_off=0x20)); struct.pack_into('<I', e, 56, 3)
        self.assertEqual(delta.c1_off(e), 0x300000020)
        self.assertEqual(delta.entry_hash(bytes(e), bind(0), b'', []), delta.entry_hash(entry('00000000000000aa'), bind(0), b'', []), 'c1_off_hi is a position in the file')


class Manifest(unittest.TestCase):
    H = {'00000000000000aa': '0123456789abcdef', '00000000000000bb': '-'}

    def setUp(self): self.tmp = Path(tempfile.mkdtemp(prefix='vkmanifest_'))
    def tearDown(self): shutil.rmtree(self.tmp, ignore_errors=True)

    def test_round_trip(self):
        t = delta.format_manifest('76b097613f15', GAME, '5dd843bed8d1', self.H, [('66d1dd2cd5ff', 2322)])
        m = delta.parse_manifest(t)
        self.assertEqual((m['version'], m['game'], m['inputs'], m['from'], m['hashes']), ('76b097613f15', GAME, '5dd843bed8d1', {'66d1dd2cd5ff': 2322}, self.H))

    def test_stamp_rows(self):
        o, n = 'a' * 64, 'b' * 64
        t = delta.format_manifest('76b097613f15', GAME, '5dd843bed8d1', self.H, [('66d1dd2cd5ff', 0)], [('66d1dd2cd5ff', o, n)])
        self.assertEqual(delta.parse_manifest(t)['stamp'], {'66d1dd2cd5ff': (o, n)})
        for bad in (f'stamp\t66d1dd2cd5ff\t{o}', f'stamp\t66d1dd2cd5ff\t{o}\t{n}\t1', f'stamp\t66d1dd2cd5ff\t{o[:-1]}g\t{n}', f'stamp\t/home/x\t{o}\t{n}'):
            self.assertTrue(delta.manifest_problems(t + bad + '\n'), bad)

    def test_stamp_only(self):
        sh = [('00000000000000aa', [spirv(10, 1)], b''), ('00000000000000bb', [spirv(12, 2)], b'')]
        a, b, c = self.tmp / 'a.lib', self.tmp / 'b.lib', self.tmp / 'c.lib'
        write_lib(a, '66d1dd2cd5ff', shaders=sh)
        write_lib(b, '6607c8d19abc', abi='01b52a48538b', shaders=sh)
        self.assertTrue(delta.stamp_only(a, b))
        self.assertEqual(delta.restamped_header(a.read_bytes()[:128], '6607c8d19abc', '01b52a48538b'), b.read_bytes()[:128])
        write_lib(c, '6607c8d19abc', abi='01b52a48538b', shaders=sh[:1] + [('00000000000000bb', [spirv(12, 3)], b'')])
        self.assertFalse(delta.stamp_only(a, c), 'a module differs')
        write_lib(c, '6607c8d19abc', game='0100000000001000', abi='01b52a48538b', shaders=sh)
        self.assertFalse(delta.stamp_only(a, c), 'another game in the header')

    def test_leaks_refused(self):
        t = delta.format_manifest('76b097613f15', GAME, '5dd843bed8d1', self.H)
        for extra in ('/home/someone/game\t1', 'name\tDonkey', '00000000000000cc\t0123456789abcdeg', '00000000000000cc\t0123', 'version\t76b097613f15', 'from\tabc\t1'):
            self.assertTrue(delta.manifest_problems(t + extra + '\n'), extra)
        self.assertTrue(delta.manifest_problems(t.replace('# The Android', '# /home/x The Android')))
        self.assertTrue(delta.manifest_problems(t.replace(f'game\t{GAME}\n', '')))
        with self.assertRaises(ValueError): delta.format_manifest('76b097613f15', GAME, '/home/x/inputs', self.H)

    def test_committed_manifests(self):
        """Every profile's vk_delta.tsv holds keys and hashes only, of that profile's shader map. One of an older version than this tree's is stale (a full build of this
        version gives it again: vk_android_build.py --publish-delta), which a phone takes as no manifest: a full build"""
        found = 0
        for f in sorted(REPO.glob('dm/games/*/vk_delta.tsv')):
            found += 1
            text = f.read_text()
            self.assertEqual(delta.manifest_problems(text), [], f)
            m = delta.parse_manifest(text)
            self.assertEqual(m['game'], f.parent.name, f)
            self.assertEqual(set(m['hashes']), profile_keys(f.parent), f'{f}: not the keys a phone builds (its shader_map.tsv, or its shader_seen.tsv with their pairs)')
            hdr = (REPO / 'dm/gfx/nvn/dmsh_chain_vk.h').read_text()
            if f'"{m["version"]}"' not in hdr: print(f'\n{f} is of version {m["version"]}, older than this tree\'s: phones make a full build until it is published again', file=sys.stderr)
            self.assertNotIn(m['version'], m['from'])


@unittest.skipIf(zstd is None, 'no zstd in this Python')
class KeyList(unittest.TestCase):
    """A key-list build (a profile's selected set, as the phone passes it) with the profile's manifest: what it keeps from older libraries, what it translates and what
    its end checks, for a manifest of the same selection, of another (a shader_seen.tsv changed since it was published) and for older libraries of either"""
    A, B, C, D, E = (f'00000000000000{x}' for x in ('aa', 'bb', 'cc', 'dd', 'ee'))

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='vkkeys_'))
        self.mod = {k: [spirv(20 + i, i)] for i, k in enumerate((self.A, self.B, self.C, self.D, self.E))}
        write_lib(self.tmp / 'new.lib', '222222222222', shaders=[(k, m, b'') for k, m in self.mod.items()])   # the Mac's library of this version
        _, self.h = delta.library_hashes(str(self.tmp / 'new.lib'), zstd)
        # the profile's manifest: its selection A, B, C (D, E are other shaders of the map); C did not build on the Mac
        self.man = delta.parse_manifest(delta.format_manifest('222222222222', GAME, '5dd843bed8d1', {self.A: self.h[self.A], self.B: self.h[self.B], self.C: '-'}))

    def tearDown(self): shutil.rmtree(self.tmp, ignore_errors=True)

    def lib(self, name, version, shaders):
        write_lib(self.tmp / name, version, shaders=shaders)
        return delta.Library(str(self.tmp / name))

    def test_reuse(self):
        """The selection's keys whose entry an older library has as the manifest has it are kept; one whose entry changed is translated; "-" is never taken"""
        old = self.lib('old.lib', '111111111111', [(self.A, self.mod[self.A], b''), (self.B, [spirv(9, 9)], b''), (self.C, self.mod[self.C], b'')])
        keys = [self.A, self.B, self.C]
        cand = delta.reuse_candidates(self.man, keys, set())
        self.assertEqual(cand, [self.A, self.B], 'C did not build on the Mac: translated, never taken')
        got = {k: delta.find_entry(self.man, k, [old], zstd)[0] for k in cand}
        self.assertEqual(got[self.A].version, '111111111111')
        self.assertIsNone(got[self.B], 'B changed in this version: translated')
        L, ent, errs = delta.find_entry(self.man, self.A, [old], zstd)
        self.assertEqual((ent[0], errs), (self.A, []))
        self.assertEqual(delta.reuse_candidates(self.man, keys, {self.A}), [self.B], 'a key with a result already (an earlier run) is not taken again')
        old.close()

    def test_partial_reuse_newest_first(self):
        """Two older libraries: each key from the newest that has it as the manifest has it; a damaged entry is passed over for an older one"""
        new_old = self.lib('v2.lib', '111111111112', [(self.A, [spirv(9, 9)], b''), (self.B, self.mod[self.B], b'')])
        old_old = self.lib('v1.lib', '111111111111', [(self.A, self.mod[self.A], b''), (self.B, self.mod[self.B], b'')])
        libs = [new_old, old_old]
        self.assertEqual(delta.find_entry(self.man, self.A, libs, zstd)[0].version, '111111111111', 'A changed in the newer one: the older one\'s is as the manifest has it')
        self.assertEqual(delta.find_entry(self.man, self.B, libs, zstd)[0].version, '111111111112', 'newest first')
        for L in libs: L.close()
        d = bytearray((self.tmp / 'v2.lib').read_bytes()); L = delta.Library(str(self.tmp / 'v2.lib'))
        _, _, _, _, mods = L.entry(L.index[self.B]); off = delta.MOD.unpack_from(L.m, L.mo + delta.MOD.size * 1)[0]; L.close()
        d[L.blob_off + off:L.blob_off + off + 8] = b'\xff' * 8; (self.tmp / 'v2.lib').write_bytes(bytes(d))
        libs = [delta.Library(str(self.tmp / 'v2.lib')), delta.Library(str(self.tmp / 'v1.lib'))]
        L, _, errs = delta.find_entry(self.man, self.B, libs, zstd)
        self.assertEqual(L.version, '111111111111', 'the damaged newest entry passed over')
        for L in libs: L.close()

    def test_selection_changed(self):
        """A build of another selection than the manifest's (shader_seen.tsv changed, the manifest not published again): only the keys both have are kept and checked;
        a key the manifest lacks is translated and not checked, whatever an older library holds for it; the manifest's other keys do not count"""
        old = self.lib('old.lib', '111111111111', [(k, m, b'') for k, m in self.mod.items()])   # an older library holding every key unchanged
        keys = [self.A, self.E]
        self.assertEqual(delta.coverage(self.man, keys), ([self.A], [self.E]))
        self.assertEqual(delta.reuse_candidates(self.man, keys, set()), [self.A], 'E is not in the manifest: never kept, even with an entry in an older library')
        inm, outm, diff = delta.check(self.man, keys, {self.A: self.h[self.A], self.E: self.h[self.E]})
        self.assertEqual((inm, outm, diff), ([self.A], [self.E], []), 'E built here is not a difference; B (in the manifest, not in this build) is not missing')
        self.assertEqual(delta.not_built_there(self.man, [self.E]), set(), 'a failure of E is not excused by the manifest')
        self.assertEqual(delta.coverage(None, keys), ([], keys))
        old.close()

    def test_check(self):
        """The end of the build: an entry not as the manifest has it, one the manifest has and the library lacks, and "-" not built: as the manifest has it"""
        keys = [self.A, self.B, self.C]
        self.assertEqual(delta.check(self.man, keys, {self.A: self.h[self.A], self.B: self.h[self.B]})[2], [])
        self.assertEqual(delta.check(self.man, keys, {self.A: self.h[self.A]})[2], [(self.B, self.h[self.B], None)])
        self.assertEqual(delta.check(self.man, keys, {self.A: self.h[self.B], self.B: self.h[self.B], self.C: self.h[self.C]})[2],
                         [(self.A, self.h[self.A], self.h[self.B]), (self.C, '-', self.h[self.C])])

    def test_phone_keys(self):
        """The keys a published manifest must hold: the map without a selected set; with one, its keys in the map and their paired pixel shaders in the map"""
        load_pairs, build_keys = borrowed('load_pairs', 'build_keys')
        mapk = {self.A, self.B, self.C, self.D}
        pairs = collections.defaultdict(set, {self.A: {self.D, self.E}})
        self.assertEqual(delta.phone_keys(mapk, None, pairs, build_keys), mapk)
        seen = f'key\tstage\n{self.A}\tvertex\n{self.E}\tvertex\n{self.A}\tvertex\n'
        self.assertEqual(delta.selection(seen, mapk), [self.A], 'only keys of the map, once')
        self.assertEqual(delta.phone_keys(mapk, seen, pairs, build_keys), {self.A, self.D}, 'A with its pixel shader D (E is no shader of the map)')
        with self.assertRaises(ValueError): delta.phone_keys(mapk, 'key\tstage\n', pairs, build_keys)

    def test_selected_profiles(self):
        """Every profile with a selected set: its phone keys are the selection with the pixel shaders its pairs name (Pokopia: 8,161)"""
        for seen in sorted((REPO / 'dm/games').glob('*/' + delta.SELECTED)):
            k = profile_keys(seen.parent)
            self.assertTrue(k and k <= {c[3] for c in (l.split('\t') for l in (seen.parent / 'shader_map.tsv').read_text().splitlines()) if len(c) > 4 and c[0].endswith('.bin')}, seen.parent.name)


class Abi(unittest.TestCase):
    def test_header_current(self):
        self.assertIn(f'#define DMSH_VK_ABI "{delta.abi_digest()}"', (REPO / 'dm/gfx/nvn/dmsh_chain_vk.h').read_text(),
                      'dm/gfx/nvn/dmsh_chain_vk.h is stale: run dm/gfx/tools/vk_android_build.py --header and commit it')

    def test_comments_do_not_count(self):
        d = Path(tempfile.mkdtemp(prefix='vkabi_'))
        try:
            a, b = d / 'dmsh_abi_vk.h', d / 'dmsh_abi.h'
            shutil.copy(delta.ABI_VK, a); shutil.copy(delta.ABI, b)
            base = delta.abi_digest((str(a), str(b)))
            self.assertEqual(base, delta.abi_digest(), 'the digest of a copy is the digest')
            a.write_text(a.read_text().replace('// The Vulkan shader library ABI', '/* a note */\n//   The   Vulkan shader library ABI, reflowed'))
            self.assertEqual(delta.abi_digest((str(a), str(b))), base)
            a.write_text(a.read_text().replace('#define DMSH_VK_ABI_REV 1', '#define DMSH_VK_ABI_REV 2'))
            self.assertNotEqual(delta.abi_digest((str(a), str(b))), base, 'DMSH_VK_ABI_REV moves it')
            shutil.copy(delta.ABI_VK, a); b.write_text(b.read_text() + '\n#define DMSH_SOMETHING_NEW 4\n')
            self.assertNotEqual(delta.abi_digest((str(a), str(b))), base, 'dmsh_abi.h is part of it')
        finally: shutil.rmtree(d, ignore_errors=True)

    def test_compat_table_is_for_this_abi(self):
        """dm/gfx/vk_android/vka_abi_compat.h: the older ABIs the replay and the launcher also play were checked against this tree's ABI (VKA_ABI_COMPAT_FOR), none of
        them is this ABI, and the file is not one the digest reads (a row moves no version)"""
        h = (REPO / 'dm/gfx/vk_android/vka_abi_compat.h').read_text()
        define = lambda n: re.search(rf'#define\s+{n}\s+"([^"]*)"', h).group(1)
        self.assertEqual(define('VKA_ABI_COMPAT_FOR'), delta.abi_digest(),
                         'DMSH_VK_ABI moved: check vka_abi_compat.h\'s rows against the ABI headers\' diff, then raise VKA_ABI_COMPAT_FOR (docs/reference/shaders.md)')
        rows = re.findall(r'\{"([0-9a-f]{12})",', h)
        self.assertEqual(define('DMSH_VK_ABI_COMPAT').split(), rows, 'DMSH_VK_ABI_COMPAT (the launcher\'s list) is the table\'s ABIs in order')
        self.assertNotIn(delta.abi_digest(), rows)
        self.assertNotIn('vka_abi_compat', ' '.join(map(os.path.basename, (delta.ABI_VK, delta.ABI))))
        self.assertIn('674db0a4ff79', rows, 'the ABI before a319e773 (the write log of pages): libraries of it play while their update runs')

    def test_build_uses_these(self):
        src = (REPO / 'dm/gfx/tools/vk_android_build.py').read_text()
        self.assertIn("'tools/vk_android_delta.py'", src, 'vk_android_delta.py decides what a build reuses: it is one of DMSH_VK_CHAIN\'s files')
        self.assertIn('delta.HEAD.pack(', src)


@unittest.skipUnless(shutil.which('javac') and shutil.which('java'), 'no JDK')
@unittest.skipIf(zstd is None, 'no zstd in this Python')
class Launcher(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix='shlibs_'))
        subprocess.run(['javac', '-Xlint:all', '-Werror', '-d', str(cls.tmp / 'classes'), str(SRC), str(TEST)], check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls): shutil.rmtree(cls.tmp, ignore_errors=True)

    def java(self, *a):
        r = subprocess.run(['java', '-Dstdout.encoding=UTF-8', '-cp', str(self.tmp / 'classes'), 'dev.nxnative.android.ShaderLibsJvmTest', *map(str, a)],
                           capture_output=True, text=True, encoding='utf-8')
        self.assertEqual(r.returncode, 0, r.stderr)
        return r.stdout.rstrip('\n').split(' ', 1)[1]

    def test_card_updating_text(self):
        # the game card's state while the background shader update runs (MainActivity.updateTick)
        self.assertEqual(self.java('updating', 1234, 22852, 'about 12 min left', 0), 'Ready \u00b7 updating shaders 1,234 / 22,852 \u00b7 about 12 min left')
        self.assertEqual(self.java('updating', 1234, 22852, '-', 0), 'Ready \u00b7 updating shaders 1,234 / 22,852')
        self.assertEqual(self.java('updating', 0, 0, '-', 0), 'Ready \u00b7 updating shaders')                      # no count from the job yet
        self.assertEqual(self.java('updating', 500, 400, '-', 0), 'Ready \u00b7 updating shaders 400 / 400')         # never more than the total
        self.assertEqual(self.java('updating', 1234, 22852, 'about 12 min left', 1), 'Ready \u00b7 shader update paused 1,234 / 22,852')

    def test_header_as_python_reads_it(self):
        p = self.tmp / 'h/v-222222222222/dmsh_vk.lib'
        write_lib(p, '222222222222', abi='0123456789ab', shaders=[('00000000000000aa', [spirv(10, 1)], b'')])
        L = delta.Library(str(p))
        self.assertEqual(self.java('header', p), '\t'.join([L.chain, L.version, L.game, L.inputs, L.abi]))
        write_lib(p, '222222222222', shaders=[])
        self.assertEqual(self.java('header', p).split('\t')[4], '')
        p.write_bytes(b'not a library' * 20)
        self.assertEqual(self.java('header', p), 'null')

    def test_playable(self):
        pack = self.tmp / 'pack'; abi = delta.abi_digest(); cur = '76b097613f15'
        sh = [('00000000000000aa', [spirv(10, 1)], b'')]
        write_lib(pack / 'shaders/v-111111111111/dmsh_vk.lib', '111111111111', abi=abi, shaders=sh)
        self.assertEqual(self.java('playable', pack, GAME, cur, abi), 'v-111111111111')
        time.sleep(0.02); os.utime(pack / 'shaders/v-111111111111/dmsh_vk.lib', (time.time() - 100, time.time() - 100))
        write_lib(pack / 'shaders/v-222222222222/dmsh_vk.lib', '222222222222', abi=abi, shaders=sh)
        write_lib(pack / 'shaders/v-333333333333/dmsh_vk.lib', '333333333333', abi='0123456789ab', shaders=sh)   # another ABI
        write_lib(pack / 'shaders/v-444444444444/dmsh_vk.lib', '444444444444', shaders=sh)                         # made before the field
        write_lib(pack / 'shaders/v-555555555555/dmsh_vk.lib', '555555555555', game='0100000000001000', abi=abi, shaders=sh)   # another game
        self.assertEqual(self.java('playable', pack, GAME, cur, abi), 'v-222222222222', 'the newest of this game with this ABI')
        self.assertEqual(self.java('playable', pack, GAME, '222222222222', abi), 'v-111111111111', 'never the current version (that is the usual check)')
        self.assertEqual(self.java('playable', pack, GAME, cur, 'unknown'), 'none', 'an APK built without the ABI plays its own version only')
        self.assertEqual(self.java('older', pack, GAME, '222222222222'), '111111111111,333333333333,444444444444')
        self.assertEqual(self.java('playable', self.tmp / 'nothing', GAME, cur, abi), 'none')
        # made before the abi field: not playable by the ABI rule, but the last resort the launcher does not block (with a warning) and the replay loads
        self.assertEqual(self.java('legacy', pack, GAME, cur), 'v-444444444444')
        self.assertEqual(self.java('legacy', pack, GAME, '444444444444'), 'none', 'never the current version')
        self.assertEqual(self.java('legacy', pack, '0100000000001000', cur), 'none', 'never another game\'s')

    def test_playable_older_abi(self):
        """A library of an older ABI the host also plays (vka_abi_compat.h DMSH_VK_ABI_COMPAT): taken only when the pack has none of the APK's own ABI, the newest of them;
        never another game's or the current version; an APK without the list (or with an empty one) does not take it"""
        pack = self.tmp / 'compat'; abi = delta.abi_digest(); cur = '76b097613f15'; old = '674db0a4ff79'; compat = f'{old} 0a0a0a0a0a0a'
        sh = [('00000000000000aa', [spirv(10, 1)], b'')]
        write_lib(pack / 'shaders/v-d64f157175fd/dmsh_vk.lib', 'd64f157175fd', abi=old, shaders=sh)
        write_lib(pack / 'shaders/v-999999999999/dmsh_vk.lib', '999999999999', game='0100000000001000', abi=old, shaders=sh)   # another game
        self.assertEqual(self.java('playable', pack, GAME, cur, abi, compat), 'v-d64f157175fd')
        self.assertEqual(self.java('playable', pack, GAME, cur, abi), 'none', 'without the list')
        self.assertEqual(self.java('playable', pack, GAME, cur, abi, ''), 'none', 'with an empty list (an APK whose table is for another ABI)')
        self.assertEqual(self.java('playable', pack, GAME, cur, abi, '0123456789ab'), 'none', 'another older ABI')
        self.assertEqual(self.java('playable', pack, GAME, 'd64f157175fd', abi, compat), 'none', 'never the current version')
        self.assertEqual(self.java('playable', pack, GAME, cur, 'unknown', compat), 'none', 'an APK built without the ABI plays its own version only')
        os.utime(pack / 'shaders/v-d64f157175fd/dmsh_vk.lib', (time.time() - 200, time.time() - 200))
        write_lib(pack / 'shaders/v-114c794bab3e/dmsh_vk.lib', '114c794bab3e', abi=old, shaders=sh)
        self.assertEqual(self.java('playable', pack, GAME, cur, abi, compat), 'v-114c794bab3e', 'the newest of the older ABI')
        os.utime(pack / 'shaders/v-114c794bab3e/dmsh_vk.lib', (time.time() - 100, time.time() - 100))
        write_lib(pack / 'shaders/v-111111111111/dmsh_vk.lib', '111111111111', abi=abi, shaders=sh)
        os.utime(pack / 'shaders/v-111111111111/dmsh_vk.lib', (time.time() - 300, time.time() - 300))
        self.assertEqual(self.java('playable', pack, GAME, cur, abi, compat), 'v-111111111111', 'one of the APK\'s own ABI first, even when older')
        self.assertEqual(self.java('legacy', pack, GAME, cur), 'none', 'a library of an older ABI is no legacy one')

    def test_restamp(self):
        pack = self.tmp / 'rs'; cur = '6607c8d19abc'; abi = '01b52a48538b'
        sh = [('00000000000000aa', [spirv(10, 1)], b''), ('00000000000000bb', [spirv(40, 2)], b'x' * 33)]
        old = pack / 'shaders/v-66d1dd2cd5ff'; write_lib(old / 'dmsh_vk.lib', '66d1dd2cd5ff', shaders=sh); (old / 'inputs.tsv').write_text('game\t040074A01BF12000\ninputs\t5dd843bed8d1\n')
        mac = self.tmp / 'mac.lib'; write_lib(mac, cur, abi=abi, shaders=sh)
        o, n = delta.sha256_file(old / 'dmsh_vk.lib'), delta.sha256_file(mac)
        self.assertEqual(self.java('restamp', pack, GAME, cur, abi, f'66d1dd2cd5ff:{o}:{"0" * 64}'), 'none', 'a copy that is not the Mac library is never taken')
        self.assertFalse((pack / f'shaders/v-{cur}').exists() or (pack / f'shaders/.restamp-{cur}').exists(), 'and leaves nothing behind')
        self.assertEqual(self.java('restamp', pack, GAME, cur, abi, f'66d1dd2cd5ff:{"0" * 64}:{n}'), 'none', 'an older library that is not the one compared')
        self.assertEqual(self.java('restamp', pack, '0100000000001000', cur, abi, f'66d1dd2cd5ff:{o}:{n}'), 'none', 'another game')
        self.assertEqual(self.java('restamp', pack, GAME, cur, abi, f'111111111111:{o}:{n};66d1dd2cd5ff:{o}:{n}'), '66d1dd2cd5ff')
        new = pack / f'shaders/v-{cur}'
        self.assertEqual((new / 'dmsh_vk.lib').read_bytes(), mac.read_bytes(), 'the Mac library byte for byte')
        self.assertEqual((new / 'inputs.tsv').read_text(), (old / 'inputs.tsv').read_text())
        self.assertTrue((old / 'dmsh_vk.lib').is_file(), 'the older library stays')
        self.assertEqual(self.java('restamp', pack, GAME, cur, abi, f'66d1dd2cd5ff:{o}:{n}'), 'none', 'once only: this version has its library')
        self.assertEqual(self.java('restamp', pack, GAME, cur, abi, ''), 'none')

    def test_selection(self):
        """A profile's shader_seen.tsv: its keys that the map has, sorted, each once (the Mac's selected build: grep the keys, comm with the map's column 4)"""
        d = self.tmp / 'sel'; d.mkdir(exist_ok=True)
        (d / 'shader_map.tsv').write_text('# kind: evidence\npackage\tindex\tstage\tdm_ctl_sha16\tsource\n'
                                          'vs_0.bin\t0\tvs\t00000000000000bb\tx\nps_0.bin\t0\tps\t00000000000000aa\tx\ncs_0.bin\t0\tcs\t00000000000000cc\tx\n')
        self.assertEqual(self.java('selection', d), 'none', 'no shader_seen.tsv: the whole map')
        (d / 'shader_seen.tsv').write_text('# a comment 00000000000000cc\nkey\tstage\n00000000000000bb\tvertex\n00000000000000aa\tpixel\n00000000000000dd\tpixel\n00000000000000bb\tvertex\n')
        self.assertEqual(self.java('selection', d), '00000000000000aa,00000000000000bb', 'only keys of the map, sorted, once')

    def test_absent_optional(self):
        """A split that lacks only keys of the profile's shader_optional.tsv is the profile's (PackService splitMatchesProfile), and the shaders step leaves those out;
        a missing key that is not optional, a key the profile lacks, or no shader_optional.tsv is not"""
        d = self.tmp / 'opt'; d.mkdir(exist_ok=True)
        head = '# kind: evidence\npackage\tindex\tstage\tdm_ctl_sha16\tsource\n'
        rows = lambda *ks: head + ''.join(f'cs_0.bin\t{i}\tcompute\t{k}\tsrc\n' for i, k in enumerate(ks))
        (d / 'shader_map.tsv').write_text(rows('00000000000000aa', '00000000000000bb', '00000000000000cc'))
        split = self.tmp / 'split_map.tsv'
        split.write_text(rows('00000000000000aa', '00000000000000bb'))
        self.assertEqual(self.java('absent', d, split), 'none', 'no shader_optional.tsv')
        (d / 'shader_optional.tsv').write_text('# keys\n00000000000000cc\t1.8.0 as version 0 only\n')
        self.assertEqual(self.java('absent', d, split), '00000000000000cc')
        split.write_text(rows('00000000000000aa'))
        self.assertEqual(self.java('absent', d, split), 'none', 'bb is not optional')
        split.write_text(rows('00000000000000aa', '00000000000000bb', '00000000000000dd'))
        self.assertEqual(self.java('absent', d, split), 'none', 'a key the profile does not have')
        split.write_text(rows('00000000000000aa', '00000000000000bb', '00000000000000cc'))
        self.assertEqual(self.java('absent', d, split), 'none', 'nothing lacking: the byte comparison decides')

    def test_selection_of_profiles(self):
        """Every profile with a selected set: the Java selection is what the Mac's selected build passes (shader_seen.tsv keys in shader_map.tsv)"""
        for seen in sorted((REPO / 'dm/games').glob('*/shader_seen.tsv')):
            mp = seen.parent / 'shader_map.tsv'
            keys = {c[3] for c in (l.rstrip('\n').split('\t') for l in mp.read_text().splitlines()) if len(c) > 3 and not c[0].startswith('#') and c[0] != 'package'}
            want = sorted({l[:16] for l in seen.read_text().splitlines() if re.match(r'[0-9a-f]{16}', l)} & keys)
            self.assertEqual(self.java('selection', seen.parent), ','.join(want), seen.parent.name)
            self.assertEqual(delta.selection(seen.read_text(), keys), want, 'vk_android_delta.py selection, which --publish-delta checks a manifest against')

    def test_translate_and_minutes(self):
        self.assertEqual(self.java('translate', '66d1dd2cd5ff=2322,b430e271aa8a=5000', 'b430e271aa8a,66d1dd2cd5ff'), '2322')
        self.assertEqual(self.java('translate', '66d1dd2cd5ff=2322', 'b430e271aa8a'), '-1')
        self.assertEqual(self.java('translate', '', '66d1dd2cd5ff'), '-1')
        self.assertEqual(self.java('translate', '66d1dd2cd5ff=0', '66d1dd2cd5ff'), '0')
        lo, hi, text = self.java('minutes', 22852).split(' ', 2)
        self.assertTrue(18 <= int(lo) <= 25 and 32 <= int(hi) <= 42, (lo, hi))   # the full step's 20-35 minutes (android/ONDEVICE.md)
        lo2, hi2, _ = self.java('minutes', 2322).split(' ', 2)
        self.assertTrue(int(lo2) < int(lo) and int(hi2) < int(hi) and int(lo2) >= 5)
        self.assertRegex(text, r'^about \d+-\d+ minutes$')
        # A whole Prepare: the rewrite and the shaders step, by the game's shader count (DKB 22,852 within the 30-45 minutes measured and estimated, MKW 11,604 less)
        dkb = [int(x) for x in re.fullmatch(r'about (\d+)-(\d+) minutes', self.java('job', 22852)).groups()]
        mkw = [int(x) for x in re.fullmatch(r'about (\d+)-(\d+) minutes', self.java('job', 11604)).groups()]
        self.assertTrue(dkb[0] >= int(lo) and 40 <= dkb[1] <= 50 and mkw[0] < dkb[0] and mkw[1] < dkb[1], (dkb, mkw))


class RequireAll(unittest.TestCase):
    """DMSH_VK_REQUIRE_ALL (the phone's shaders step) needs every shader the Mac's full build of the same version holds: a shader the profile's manifest has as "-"
    did not build there either and does not fail the step (Mario Kart World 1.8.0: 11,603 of 11,604 by design); without a manifest of this version every shader must build"""
    M = {'hashes': {'00000000000000aa': '0123456789abcdef', '00000000000000bb': '-'}}

    def test_not_built_there(self):
        self.assertEqual(delta.not_built_there(self.M, ['00000000000000bb']), {'00000000000000bb'})
        self.assertEqual(delta.not_built_there(self.M, ['00000000000000aa', '00000000000000bb', '00000000000000cc']), {'00000000000000bb'},
                         'a shader the Mac built, or one the manifest lacks, still fails the step')
        self.assertEqual(delta.not_built_there(None, ['00000000000000bb']), set(), 'no manifest of this version: every shader must build')

    def test_entry_as_manifest(self):
        self.assertTrue(delta.entry_as_manifest('-', None), 'not built here, not built on the Mac: as the manifest has it')
        self.assertTrue(delta.entry_as_manifest(None, None))
        self.assertTrue(delta.entry_as_manifest('0123456789abcdef', '0123456789abcdef'))
        self.assertFalse(delta.entry_as_manifest('-', '0123456789abcdef'), 'built here, not on the Mac: a difference')
        self.assertFalse(delta.entry_as_manifest('0123456789abcdef', None), 'built on the Mac, not here')
        self.assertFalse(delta.entry_as_manifest('0123456789abcdef', 'fedcba9876543210'))

    def test_build_uses_them(self):
        src = (REPO / 'dm/gfx/tools/vk_android_build.py').read_text()
        self.assertIn('delta.not_built_there(_excuse,', src, 'REQUIRE_ALL takes the manifest\'s "-" shaders out of the failures')
        self.assertIn("_excuse = MANIFEST or load_manifest()[0]", src, 'a key list\'s build takes them from the manifest too')
        self.assertIn("MANIFEST, _why = (None, 'DMSH_VK_RAW') if RAW else load_manifest()", src, 'a key list uses the manifest (no "a key list" exception)')
        self.assertIn('delta.check(MANIFEST, keys, _hashes)', src)
        self.assertIn('delta.reuse_candidates(MANIFEST, keys, heads)', src)
        req = next(l for l in src.splitlines() if "os.environ.get('DMSH_VK_REQUIRE_ALL') == '1'" in l and l.startswith('if '))
        self.assertNotIn('want', req, 'REQUIRE_ALL holds for a key list (a selected set) too: Pokopia\'s card said Ready with 3,384 failed shaders')
        self.assertIn('if not entry_as_manifest(', (REPO / 'dm/gfx/tools/vk_android_delta.py').read_text(), 'the delta check takes a "-" shader not built as the manifest has it')

    def test_mkw_manifest(self):
        """Mario Kart World's manifest: its one shader that builds nowhere is "-", so a phone build of that version is complete without it"""
        m = delta.parse_manifest((REPO / 'dm/games/0400C3F00006E000/vk_delta.tsv').read_text())
        self.assertEqual(sorted(k for k, h in m['hashes'].items() if h == '-'), ['078997bf631992c5'])


if __name__ == '__main__':
    unittest.main()
