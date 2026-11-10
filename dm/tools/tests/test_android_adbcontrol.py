"""The launcher's adb control commands (android/TESTING.md, AdbControl.java with android/app/src/jvmtest's AdbControlJvmTest): MainActivity is exported, so a play
or release build takes them only while Settings › ADB control (debug) is on; a test build always does. The list of control extras is checked against every extra
the launcher's command handlers read, so a new command cannot skip the gate. The JVM part is skipped without a JDK."""
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
JAVA = REPO / 'android/app/src'
SRC = JAVA / 'main/java/dev/nxnative/android/AdbControl.java'
TEST = JAVA / 'jvmtest/java/dev/nxnative/android/AdbControlJvmTest.java'
# The handlers MainActivity.fromIntent calls (and start(), which reads no_pause from the launcher's intent)
HANDLERS = [JAVA / 'main/java/dev/nxnative/android/MainActivity.java', JAVA / 'main/java/dev/nxnative/android/GpuDriver.java',
            JAVA / 'main/java/dev/nxnative/android/PackUi.java', JAVA / 'ondevice/java/dev/nxnative/android/Ondevice.java']
NOT_CONTROL = {'frontend_game'}   # FrontendLaunch's hand-over: what the exported .Launch alias already does for any app
EXTRA = re.compile(r'(?:get\w*Extra|hasExtra)\("([a-z_]+)"')


def listed():
    m = re.search(r'EXTRAS = .*?asList\((.*?)\)\);', SRC.read_text(encoding='utf-8'), re.S)
    return re.findall(r'"([a-z_]+)"', m.group(1))


class Sources(unittest.TestCase):
    def test_every_handler_extra_is_gated(self):
        read = set()
        for f in HANDLERS: read |= set(EXTRA.findall(f.read_text(encoding='utf-8')))
        self.assertEqual(sorted(read - NOT_CONTROL - set(listed())), [], 'extras the launcher reads that AdbControl.EXTRAS does not gate')

    def test_gate_runs_before_the_handlers(self):
        s = (JAVA / 'main/java/dev/nxnative/android/MainActivity.java').read_text(encoding='utf-8')
        body = s[s.index('private void fromIntent(Intent i) {'):]
        self.assertLess(body.index('i = gated(i);'), body.index('Ondevice.fromIntent'))
        self.assertIn('setIntent(gated(getIntent()))', s)


@unittest.skipUnless(shutil.which('javac') and shutil.which('java'), 'no JDK')
class Gate(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix='adbcontrol_'))
        subprocess.run(['javac', '-Xlint:all', '-Werror', '-encoding', 'UTF-8', '-d', str(cls.tmp / 'classes'), str(SRC), str(TEST)], check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls): shutil.rmtree(cls.tmp, ignore_errors=True)

    def java(self, *a):
        r = subprocess.run(['java', '-cp', str(self.tmp / 'classes'), 'dev.nxnative.android.AdbControlJvmTest', *a], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        return r.stdout.rstrip('\n').split(' ', 1)[1] if ' ' in r.stdout.strip() else ''

    def test_allowed(self):
        self.assertEqual(self.java('allowed', '0', '0'), 'true')    # the test app: always
        self.assertEqual(self.java('allowed', '0', '1'), 'true')
        self.assertEqual(self.java('allowed', '1', '0'), 'false')   # play / release: off by default
        self.assertEqual(self.java('allowed', '1', '1'), 'true')

    def test_asked(self):
        self.assertEqual(self.java('asked'), '')
        self.assertEqual(self.java('asked', 'frontend_game', 'title_id'), '')   # a frontend's launch is not a command
        self.assertEqual(self.java('asked', 'gpu_driver_zip', 'game', 'frontend_game'), 'game,gpu_driver_zip')
        self.assertEqual(self.java('asked', 'pack_import', 'pack_title'), 'pack_title,pack_import')

    def test_extras_match_source(self):
        self.assertEqual(self.java('extras').split(','), listed())


if __name__ == '__main__':
    unittest.main()
