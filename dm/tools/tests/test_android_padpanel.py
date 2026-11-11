"""The in-game panel with a controller (PadPanel.java with android/app/src/jvmtest's PadPanelJvmTest): the open / close combination (Home, or − and + held), the
stick and the D-pad as directions with repeat, radio and tab steps, and the confirm / back keys from the game's Button mapping; and that GameActivity and SidePanel
use them. The JVM part is skipped without a JDK."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
JAVA = REPO / 'android/app/src/main/java/dev/nxnative/android'
SRC = [JAVA / 'PadPanel.java', JAVA / 'PadMap.java']
TEST = REPO / 'android/app/src/jvmtest/java/dev/nxnative/android/PadPanelJvmTest.java'


class Wiring(unittest.TestCase):
    def test_activity(self):
        t = (JAVA / 'GameActivity.java').read_text()
        self.assertIn('combo.key(', t)                  # every pad key goes through the combination first
        self.assertIn('panel.padMotion(e)', t)          # sticks and the hat move through the open panel, never the game
        self.assertIn('releaseGameInput', t)            # the game's held buttons are released when the panel opens
        self.assertIn('PadPanel.confirmBack(', t)       # confirm / back from the game's mapping

    def test_panel(self):
        t = (JAVA / 'SidePanel.java').read_text()
        for s in ('boolean padKey(KeyEvent e)', 'boolean padMotion(MotionEvent e)', 'requestFocusFromTouch', 'OnGlobalFocusChangeListener', 'TAB_NAMES'):
            self.assertIn(s, t)

    def test_settings(self):
        self.assertIn('PadPanel.OPEN_NAMES', (JAVA / 'MainActivity.java').read_text())
        self.assertIn('panel_pad', (JAVA / 'Prefs.java').read_text())


@unittest.skipUnless(shutil.which('javac') and shutil.which('java'), 'no JDK')
class PadPanelJvm(unittest.TestCase):
    def test_jvm(self):
        with tempfile.TemporaryDirectory() as d:
            c = subprocess.run(['javac', '-Xlint:all', '-Werror', '-encoding', 'UTF-8', '-d', d, *map(str, SRC), str(TEST)], capture_output=True, text=True)
            self.assertEqual(c.returncode, 0, c.stderr)
            r = subprocess.run(['java', '-cp', d, 'dev.nxnative.android.PadPanelJvmTest'], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout.strip(), 'ok')


if __name__ == '__main__':
    unittest.main()
