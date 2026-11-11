"""Earlier rewriter files of identical output (dm/tools/rewriter_equiv, scripts/pack_android.py EQUIV): a pack whose code key was made with one counts as up to
date, in pack_android.py (rewrite_keys: the module is kept, not rewritten) and in the launcher (RewriteKey.java with android/app/src/jvmtest's RewriteKeyJvmTest:
the same keys, so Home shows no "other tools" chip); unrelated bytes are refused. Also checks the folder's files: each an earlier version of a REWRITER file,
named <file>.<commit>.
The key hashes game.tsv's build rows only (pack_android.py ROWS, build_rows: the part the rewriter reads); a key of the earlier formula (the whole game.tsv) counts
with the profile's game.tsv or with the pack's copy when that copy's build rows are the profile's (Formula): a pack whose game.tsv differs only in other rows is
accepted, a differing build row, rewriter file, module or patches.py is refused, a pack without game.tsv is judged as before. The JVM part is skipped without a JDK."""
import hashlib
import re
import shutil
import subprocess
import sys
import tempfile
import types
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'scripts'))
import pack_android  # noqa: E402

SRC = REPO / 'android/app/src/main/java/dev/nxnative/android/RewriteKey.java'
TEST = REPO / 'android/app/src/jvmtest/java/dev/nxnative/android/RewriteKeyJvmTest.java'
GAME = '0100000000000000'


def key_with(tree, files, src, gdir, game_tsv=None, v1=False):   # the key over the given rewriter bytes, as a pack made with them carries; v1: the earlier formula (whole game.tsv)
    h = hashlib.sha256()
    for name, b in files: h.update(name.encode() + b'\0' + b)
    for p in [src, gdir / 'patches.py']: h.update(p.name.encode() + b'\0' + p.read_bytes())
    gt = (gdir / 'game.tsv').read_bytes() if game_tsv is None else game_tsv
    h.update(b'game.tsv\0' + gt if v1 else b'game.tsv build rows\0' + pack_android.build_rows(gt))
    return h.hexdigest() + ' target=android'


def java_keys(t, tree, mod, pack_game=None):   # RewriteKey.keys on the JVM (RewriteKeyJvmTest)
    classes = t / 'classes'
    if not classes.is_dir(): subprocess.run(['javac', '-Xlint:all', '-Werror', '-d', str(classes), str(SRC), str(TEST)], check=True, capture_output=True, text=True)
    r = subprocess.run(['java', *([f'-Dpack.game={pack_game}'] if pack_game else []), '-cp', str(classes), 'dev.nxnative.android.RewriteKeyJvmTest', str(tree), str(mod),
                        f'dm/games/{GAME}/', 'main', *pack_android.REWRITER], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    return r.stdout.split('\n')[:-1]


class Tree(unittest.TestCase):   # a tree of rewriter files (two with earlier equivalents), a profile, a module
    def setUp(self):
        self.t = Path(tempfile.mkdtemp(prefix='rwkey_'))
        self.tree = self.t / 'tree'
        tools = self.tree / 'dm/tools'; (tools / 'rewriter_equiv').mkdir(parents=True)
        for f in pack_android.REWRITER: (tools / f).write_bytes(b'current ' + f.encode())
        (tools / 'rewriter_equiv/patch_nso.py.aaaaaaaaaaaa').write_bytes(b'old patch_nso')
        (tools / 'rewriter_equiv/nso.py.bbbbbbbbbbbb').write_bytes(b'old nso')
        (tools / 'rewriter_equiv/README').write_bytes(b'not a rewriter file')   # no <file>.<commit> of a REWRITER file: ignored
        self.gdir = self.tree / 'dm/games' / GAME; self.gdir.mkdir(parents=True)
        (self.gdir / 'game.tsv').write_text('name\tTest\n'); (self.gdir / 'patches.py').write_text('# patches\n')
        self.mod = self.t / 'main'; self.mod.write_bytes(bytes(range(256)) * 300)
        self.cur = [(f, b'current ' + f.encode()) for f in pack_android.REWRITER]
        self.saved = pack_android.REPO, pack_android.EQUIV
        pack_android.REPO, pack_android.EQUIV = self.tree, tools / 'rewriter_equiv'

    def tearDown(self):
        pack_android.REPO, pack_android.EQUIV = self.saved
        shutil.rmtree(self.t, ignore_errors=True)

    def swap(self, name, b): return [(f, b if f == name else x) for f, x in self.cur]



class Equiv(Tree):
    def py_keys(self): return pack_android.rewrite_keys(self.mod, types.SimpleNamespace(dir=self.gdir))

    def test_python(self):
        keys = self.py_keys()
        self.assertEqual(keys[0], pack_android.rewrite_key(self.mod, types.SimpleNamespace(dir=self.gdir)))
        self.assertEqual(len(keys), 8)   # current, old patch_nso.py, old nso.py, both; each in both formulas
        self.assertIn(key_with(self.tree, self.swap('patch_nso.py', b'old patch_nso'), self.mod, self.gdir), keys)
        self.assertIn(key_with(self.tree, self.swap('nso.py', b'old nso'), self.mod, self.gdir), keys)
        self.assertNotIn(key_with(self.tree, self.swap('patch_nso.py', b'unrelated bytes'), self.mod, self.gdir), keys)
        self.assertNotIn(key_with(self.tree, self.swap('patch_nso.py', b'not a rewriter file'), self.mod, self.gdir), keys)

    def test_rewrite_keeps_an_equivalent_pack(self):
        out = self.t / 'pack'; out.mkdir()
        (out / 'main.patched.bin').write_bytes(b'code')
        (out / 'main.android.key').write_text(key_with(self.tree, self.swap('patch_nso.py', b'old patch_nso'), self.mod, self.gdir) + '\n')
        pack_android.rewrite(sys.executable, {'main': self.mod}, out, 1, types.SimpleNamespace(dir=self.gdir, id=GAME))
        self.assertEqual((out / 'main.patched.bin').read_bytes(), b'code')   # kept: no rewrite ran

    @unittest.skipUnless(shutil.which('javac') and shutil.which('java'), 'no JDK')
    def test_java_same_keys(self):
        jk = java_keys(self.t, self.tree, self.mod)
        self.assertEqual(sorted(jk), sorted(self.py_keys()))
        self.assertEqual(jk[0], self.py_keys()[0])
        self.assertIn(key_with(self.tree, self.swap('patch_nso.py', b'old patch_nso'), self.mod, self.gdir), jk)
        self.assertNotIn(key_with(self.tree, self.swap('patch_nso.py', b'unrelated bytes'), self.mod, self.gdir), jk)


PROFILE = 'name\tTest\nbuild\tv1\t1.0.0\tmain=aa\tsdk=bb\nmodules\tmain sdk\ngpu_timestamps\tsegment\n'


class Formula(Tree):
    """The key hashes game.tsv's build rows; an earlier key (the whole game.tsv) counts with the profile's or with the pack's copy of the same build rows"""
    def setUp(self):
        super().setUp()
        (self.gdir / 'game.tsv').write_text(PROFILE)
        self.pack = self.t / 'pack'; self.pack.mkdir()

    def old_key(self, gt): return key_with(self.tree, self.cur, self.mod, self.gdir, gt.encode(), v1=True)

    def accepted(self, key, pack_gt):   # (Python, Java) verdicts on a key, the pack's game.tsv pack_gt (None: the pack has none)
        (self.pack / 'game.tsv').unlink(missing_ok=True)
        if pack_gt is not None: (self.pack / 'game.tsv').write_text(pack_gt)
        py = key in pack_android.rewrite_keys(self.mod, types.SimpleNamespace(dir=self.gdir), self.pack)
        if not (shutil.which('javac') and shutil.which('java')): return py, py
        return py, key in java_keys(self.t, self.tree, self.mod, self.pack / 'game.tsv' if pack_gt is not None else None)

    def test_build_rows(self):
        self.assertEqual(pack_android.build_rows(b'# c\nbuild \t v1\t1.0.0 \tmain=aa\r\nname\tX\nbuilds\tno\n'), b'build\tv1\t1.0.0\tmain=aa\n')
        self.assertEqual(pack_android.build_rows(b'build\tv1\t1.0.0\tmain=aa\nfps_decouple\tsead \nfps_limit\t30\n'), b'build\tv1\t1.0.0\tmain=aa\nfps_decouple\tsead\n')   # the opt-in the rewriter reads
        self.assertEqual(pack_android.build_rows(PROFILE.encode()), pack_android.build_rows(PROFILE.replace('segment', 'none').encode() + b'graphics_scale\t1\n'))

    def test_current_key_ignores_other_rows(self):
        k = pack_android.rewrite_key(self.mod, types.SimpleNamespace(dir=self.gdir))
        self.assertEqual(k, key_with(self.tree, self.cur, self.mod, self.gdir))
        (self.gdir / 'game.tsv').write_text(PROFILE + 'android_window\t1280x720\n')
        self.assertEqual(pack_android.rewrite_key(self.mod, types.SimpleNamespace(dir=self.gdir)), k)
        (self.gdir / 'game.tsv').write_text(PROFILE.replace('main=aa', 'main=ac'))
        self.assertNotEqual(pack_android.rewrite_key(self.mod, types.SimpleNamespace(dir=self.gdir)), k)

    def test_old_key_other_rows_differ(self):   # the profile gained host rows since the pack was made: accepted through the pack's copy
        old = PROFILE.replace('gpu_timestamps\tsegment\n', '')
        self.assertEqual(self.accepted(self.old_key(old), old), (True, True))
        self.assertEqual(self.accepted(self.old_key(old), None), (False, False))   # no copy: as before, refused
        self.assertEqual(self.accepted(self.old_key(old), PROFILE), (False, False))   # a copy the key was not made with

    def test_old_key_profile_unchanged(self):   # no copy needed when the profile is the same file
        self.assertEqual(self.accepted(self.old_key(PROFILE), None), (True, True))

    def test_old_key_build_row_differs(self):
        old = PROFILE.replace('main=aa', 'main=ac')
        self.assertEqual(self.accepted(self.old_key(old), old), (False, False))

    def test_old_key_other_inputs_differ(self):
        old = PROFILE.replace('segment', 'none')
        for files, mod, pp in ((self.swap('patch_nso.py', b'unrelated bytes'), self.mod, None), (self.cur, None, None), (self.cur, self.mod, b'# other patches\n')):
            m = self.mod if mod else self.t / 'other'; m.write_bytes(m.read_bytes() if mod else b'another module')
            g = self.gdir
            if pp: g = self.t / 'g2'; g.mkdir(exist_ok=True); (g / 'patches.py').write_bytes(pp); (g / 'game.tsv').write_text(old)
            self.assertEqual(self.accepted(key_with(self.tree, files, m, g, old.encode(), v1=True), old), (False, False))

    def test_rewrite_keeps_and_upgrades(self):   # pack_android: the module is kept, its key brought to the current formula before the copy is replaced
        old = PROFILE.replace('segment', 'none')
        (self.pack / 'main.patched.bin').write_bytes(b'code'); (self.pack / 'game.tsv').write_text(old)
        (self.pack / 'main.android.key').write_text(self.old_key(old) + '\n')
        g = types.SimpleNamespace(dir=self.gdir, id=GAME)
        pack_android.upgrade_keys({'main': self.mod}, self.pack, g)
        self.assertEqual((self.pack / 'main.android.key').read_text().strip(), pack_android.rewrite_key(self.mod, g))
        (self.pack / 'main.android.key').write_text(self.old_key(old) + '\n')
        pack_android.rewrite(sys.executable, {'main': self.mod}, self.pack, 1, g)
        self.assertEqual((self.pack / 'main.patched.bin').read_bytes(), b'code')   # kept: no rewrite ran
        self.assertEqual((self.pack / 'main.android.key').read_text().strip(), pack_android.rewrite_key(self.mod, g))

    def test_upgrade_leaves_other_keys(self):
        (self.pack / 'main.android.key').write_text('other target=android\n')
        pack_android.upgrade_keys({'main': self.mod}, self.pack, types.SimpleNamespace(dir=self.gdir, id=GAME))
        self.assertEqual((self.pack / 'main.android.key').read_text(), 'other target=android\n')


class Folder(unittest.TestCase):
    def test_files(self):
        d = REPO / 'dm/tools/rewriter_equiv'
        for p in d.iterdir():
            m = re.fullmatch(r'(.+)\.([0-9a-f]{7,40})', p.name)
            self.assertIsNotNone(m, f'{p.name}: not <REWRITER file>.<commit>')
            self.assertIn(m.group(1), pack_android.REWRITER, p.name)
            self.assertNotEqual(p.read_bytes(), (REPO / 'dm/tools' / m.group(1)).read_bytes(), f'{p.name} is the current file')
            r = subprocess.run(['git', '-C', str(REPO), 'show', f'{m.group(2)}:dm/tools/{m.group(1)}'], capture_output=True)
            if r.returncode == 0: self.assertEqual(r.stdout, p.read_bytes(), f'{p.name} is not that commit\'s {m.group(1)}')


if __name__ == '__main__':
    unittest.main()
