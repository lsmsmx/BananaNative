"""Which gamepad is player 1 on Android (PadOrder.java with android/app/src/jvmtest's PadOrderJvmTest; issues #42, #62): an external controller before a phone's own
gamepad-like device, which gets a player only from its input; the in-game panel's Player 1 choice. The host's side, pad_slot_first, is checked in dm/host/tests/t_hid.c.
The JVM part is skipped without a JDK."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
SRC = REPO / 'android/app/src/main/java/dev/nxnative/android/PadOrder.java'
TEST = REPO / 'android/app/src/jvmtest/java/dev/nxnative/android/PadOrderJvmTest.java'
ACTIVITY = REPO / 'android/app/src/main/java/dev/nxnative/android/GameActivity.java'


class Wiring(unittest.TestCase):
    def test_activity_uses_it(self):
        t = ACTIVITY.read_text()
        self.assertIn('order.start(now)', t)
        self.assertIn('order.plays(e.getDeviceId())', t)   # keys and axes of a device without a player wait for PadOrder.active
        self.assertNotIn('Native.deviceAdded(id, d.getName(), false)', t)   # every device goes through PadOrder

    def test_bindings(self):
        self.assertIn('Java_dev_nxnative_android_Native_deviceFirst', (REPO / 'android/app/src/main/cpp/nxlaunch.c').read_text())
        self.assertIn('"deviceFirst", "(ILjava/lang/String;)I"', (REPO / 'dm/host/android/jni_android.c').read_text())


@unittest.skipUnless(shutil.which('javac') and shutil.which('java'), 'no JDK')
class PadOrderJvm(unittest.TestCase):
    def test_jvm(self):
        with tempfile.TemporaryDirectory() as d:
            c = subprocess.run(['javac', '-Xlint:all', '-Werror', '-encoding', 'UTF-8', '-d', d, str(SRC), str(TEST)], capture_output=True, text=True)
            self.assertEqual(c.returncode, 0, c.stderr)
            r = subprocess.run(['java', '-cp', d, 'dev.nxnative.android.PadOrderJvmTest'], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout.strip(), 'ok')


if __name__ == '__main__':
    unittest.main()
