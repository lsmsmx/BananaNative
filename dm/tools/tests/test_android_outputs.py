"""The Android app's split outputs (android/app/src/ondevice Outputs.java): the split writes into split/ in place and the folder counts as complete only once a
marker is written after every output is synced, so Prepare never renames a folder on shared storage (MediaProvider may refuse that with EPERM,
BananaNative issue #1) and a stop at any point never leaves cut tables accepted. Skipped without a JDK."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
SRC = REPO / 'android/app/src/ondevice/java/dev/nxnative/android/Outputs.java'
TEST = REPO / 'android/app/src/jvmtest/java/dev/nxnative/android/OutputsJvmTest.java'
PACK_SERVICE = REPO / 'android/app/src/ondevice/java/dev/nxnative/android/PackService.java'


@unittest.skipUnless(shutil.which('javac') and shutil.which('java'), 'no JDK')
class SplitOutputs(unittest.TestCase):
    def test_marker(self):
        tmp = Path(tempfile.mkdtemp(prefix='outputs_'))
        try:
            classes = tmp / 'classes'
            subprocess.run(['javac', '-d', str(classes), str(SRC), str(TEST)], check=True)
            work = tmp / 'work'; work.mkdir()
            r = subprocess.run(['java', '-cp', str(classes), 'dev.nxnative.android.OutputsJvmTest', str(work)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn('all ok', r.stdout)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


class NoFolderRename(unittest.TestCase):
    def test_split_not_renamed(self):
        """The split step writes into split/ and marks it, with no folder rename (Files.move of a folder) left in PackService"""
        s = PACK_SERVICE.read_text(encoding='utf-8')
        self.assertIn('Outputs.begin(split)', s)
        self.assertIn('Outputs.finish(split)', s)
        self.assertIn('Outputs.complete(split)', s)
        self.assertNotIn('Files.move(', s)


class OutputsThatWent(unittest.TestCase):
    """BananaNative issue #3: job.json beside the dump outlives the app's pack folder (gone with an uninstall), so a step recorded done is checked against its
    outputs before the pack step uses them, and runs again when they went (OutputsJvmTest checks Outputs.redo and the checks themselves)"""
    def test_wired(self):
        s = PACK_SERVICE.read_text(encoding='utf-8')
        work = s[s.index('private void work()'):s.index('private void finished()')]
        self.assertLess(work.index('dropMissing(j, "before it goes on")'), work.index('while ((i = j.next()) >= 0)'), 'checked before the steps run')
        self.assertIn('j.codeModules(Outputs.codeModules(j.pack()))', work, 'the rewrite records the modules it wrote')
        pack = s[s.index('case "pack": {'):s.index('default:\n                return "unknown step "')]
        self.assertLess(pack.index('dropMissing(j, "before the pack step")'), pack.index('"--keep-code"'), 'the pack step goes back instead of stopping')
        self.assertIn('return "interrupted"', pack)
        job = (SRC.parent / 'Job.java').read_text(encoding='utf-8')
        self.assertIn('USES = {null, {PACK}, null, {PACK}, null}', job)
        self.assertIn('Outputs.codeComplete(pack(), codeModules())', job)
        self.assertIn('Outputs.libraryComplete(shaderCache(), BuildConfig.NX_DMSH_VK_CHAIN)', job)
        fake = (SRC.parent / 'FakeEngine.java').read_text(encoding='utf-8')
        self.assertIn('".android.key"', fake, 'the simulated rewrite writes the key the check looks for')


if __name__ == '__main__':
    unittest.main()
