"""The game window on large screens and under rounded corners (GameOrientation.java, OverlayInset.java with android/app/src/jvmtest's GameOrientationJvmTest): no fixed
orientation on a large screen, where the system letterboxes a landscape-locked window; the overlay's blocks inset by the window's rounded corners (issue #105).
The JVM part is skipped without a JDK."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
JAVA = REPO / 'android/app/src/main/java/dev/nxnative/android'
TESTS = REPO / 'android/app/src/jvmtest/java/dev/nxnative/android'


class Wiring(unittest.TestCase):
    def test_activity_uses_orientation_rule(self):
        t = (JAVA / 'GameActivity.java').read_text()
        self.assertIn('GameOrientation.fixedLandscape(', t)
        self.assertGreaterEqual(t.count('fitOrientation();'), 2)   # onCreate and onConfigurationChanged (a fold)
        self.assertIn('android:screenOrientation="sensorLandscape"', (REPO / 'android/app/src/main/AndroidManifest.xml').read_text())   # phones keep it

    def test_overlay_uses_insets(self):
        t = (JAVA / 'GameActivity.java').read_text()
        self.assertIn('RoundedCorner.POSITION_TOP_RIGHT', t)
        self.assertIn('setOnApplyWindowInsetsListener', t)   # recomputed whenever the insets change
        self.assertIn('fpsBlock.inset(', t)   # the blocks the host draws too, not only the views
        self.assertIn('void inset(', (JAVA / 'Overlay.java').read_text())


@unittest.skipUnless(shutil.which('javac') and shutil.which('java'), 'no JDK')
class ScreenFitJvm(unittest.TestCase):
    def run_jvm(self, src, test, name):
        with tempfile.TemporaryDirectory() as d:
            c = subprocess.run(['javac', '-Xlint:all', '-Werror', '-encoding', 'UTF-8', '-d', d, str(JAVA / src), str(TESTS / test)], capture_output=True, text=True)
            self.assertEqual(c.returncode, 0, c.stderr)
            r = subprocess.run(['java', '-cp', d, 'dev.nxnative.android.' + name], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout.strip(), 'ok')

    def test_orientation(self): self.run_jvm('GameOrientation.java', 'GameOrientationJvmTest.java', 'GameOrientationJvmTest')
    def test_overlay_inset(self): self.run_jvm('OverlayInset.java', 'OverlayInsetJvmTest.java', 'OverlayInsetJvmTest')


if __name__ == '__main__':
    unittest.main()
