"""The APK build's check of the stage's translator library (dm/gfx/tools/xlate_tree.py --check-lib, android/app/build.gradle nxPackXlateCheck): the library is arm64
Android code, so the check reads the digests NativeAOT keeps in its bytes. The trigger (2026-11-10): a test APK built from a stage made before a translator change
(tree 0295868e4589b4fa in an APK of tree e67cccd60c5f19ae) failed every phone's Prepare at the shaders step. Synthetic libraries: no .NET or NDK needed."""
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
TOOLS = REPO / 'dm/gfx/tools'
sys.path.insert(0, str(TOOLS))
import dmsh_chain  # noqa: E402
import xlate_tree  # noqa: E402

PROGRAM = (TOOLS / 'dmsh_xlate/Program.cs').read_bytes()
CSPROJ = (TOOLS / 'dmsh_xlate/dmsh_xlate.csproj').read_bytes()
TOKEN = b'neutral, PublicKeyToken=b77a5c561934e089\x0b\x08'   # a hex run of the same length in every library: not a tree digest


def lib(tree, program=PROGRAM):
    """bytes laid out as a NativeAOT library holds its resources: src0 (Program.cs), src1 (the csproj), then "tree" (xlate_tree.py's output)"""
    return b'\x7fELF' + b'\0' * 64 + TOKEN + b'\0' * 32 + program + CSPROJ + (tree.encode() + b'\n' if tree else b'') + b'\0\x02\x13'


class CheckLib(unittest.TestCase):
    def run_check(self, data):
        with tempfile.TemporaryDirectory(prefix='xlate_tree_') as d:
            p = os.path.join(d, 'libdmsh_xlate.so')
            Path(p).write_bytes(data)
            return xlate_tree.check_lib(p), subprocess.run([sys.executable, str(TOOLS / 'xlate_tree.py'), '--check-lib', p], capture_output=True, text=True)

    def test_this_tree(self):
        bad, r = self.run_check(lib(xlate_tree.sha()))
        self.assertEqual(bad, [])
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn(xlate_tree.sha(), r.stdout)

    def test_stale_tree(self):   # the trigger: the 0.2.7 stage's library in an APK of a later tree
        bad, r = self.run_check(lib('0295868e4589b4fa'))
        self.assertEqual(len(bad), 1)
        self.assertIn(f'built from source tree 0295868e4589b4fa, this tree is {xlate_tree.sha()}', bad[0])
        self.assertEqual(r.returncode, 1)
        self.assertIn('[RED] stale translator library: ', r.stderr)
        self.assertIn('android/ondevice/build.sh <work dir> tools', r.stderr, 'says what to run')
        self.assertIn('dm/gfx/tools/dmsh_xlate_android/build.sh', r.stderr)

    def test_no_tree_digest(self):   # a library from before --tree-sha
        bad, r = self.run_check(lib(None))
        self.assertEqual(len(bad), 1)
        self.assertIn('unknown (no embedded tree digest', bad[0])
        self.assertEqual(r.returncode, 1)

    def test_embedded_source_differs(self):   # the tree digest matches but the library holds another Program.cs (a hand-edited tree.sha)
        bad, _ = self.run_check(lib(xlate_tree.sha(), b'\x00' + PROGRAM[1:]))
        self.assertTrue(bad and 'Program.cs' in bad[0], bad)

    def test_digest_read_only_where_delimited(self):
        self.assertEqual(xlate_tree.embedded(lib('0295868e4589b4fa')), ['0295868e4589b4fa'])
        self.assertEqual(xlate_tree.embedded(TOKEN + b'0123456789abcdef0\n' + b'x0123456789abcdef\n'), [], 'longer or glued hex runs are not a digest')

    def test_lock_is_this_trees(self):   # the phone also compares the lock's xlate_source with Program.cs: a stale lock is caught here too
        with tempfile.TemporaryDirectory(prefix='xlate_tree_') as d:
            lock = os.path.join(d, 'dmsh_chain.lock')
            Path(lock).write_text(''.join(l if not l.startswith('xlate_source\t') else 'xlate_source\t0000000000000000\n' for l in Path(dmsh_chain.LOCK).read_text().splitlines(True)))
            p = os.path.join(d, 'libdmsh_xlate.so')
            Path(p).write_bytes(lib(xlate_tree.sha()))
            old = dmsh_chain.LOCK
            try:
                dmsh_chain.LOCK = lock
                bad = xlate_tree.check_lib(p)
            finally:
                dmsh_chain.LOCK = old
        self.assertTrue(len(bad) == 1 and 'xlate_source 0000000000000000' in bad[0], bad)

    def test_usage(self):
        r = subprocess.run([sys.executable, str(TOOLS / 'xlate_tree.py'), '--check-lib'], capture_output=True, text=True)
        self.assertNotEqual(r.returncode, 0)
        r = subprocess.run([sys.executable, str(TOOLS / 'xlate_tree.py')], capture_output=True, text=True)
        self.assertEqual(r.stdout.strip(), xlate_tree.sha())


class Wiring(unittest.TestCase):
    def test_gradle_checks_before_packing(self):
        g = (REPO / 'android/app/build.gradle').read_text(encoding='utf-8')
        self.assertIn("tasks.register('nxPackXlateCheck')", g)
        self.assertIn("new ProcessBuilder('python3', tool.path, '--check-lib', lib.path)", g)
        jni = g[g.index("tasks.register('nxPackJni', Sync)"):]
        self.assertIn('dependsOn xlateCheck', jni[:jni.index('}')], 'the library is checked before it is packed')

    def test_stage_build_checks_what_it_copies(self):
        sh = (REPO / 'android/ondevice/build.sh').read_text(encoding='utf-8')
        cp = sh.index('cp $W/xlate/lib/libdmsh_xlate.so')
        self.assertIn('xlate_tree.py --check-lib $S/lib/libdmsh_xlate.so || exit 1', sh[cp:])


if __name__ == '__main__':
    unittest.main()
