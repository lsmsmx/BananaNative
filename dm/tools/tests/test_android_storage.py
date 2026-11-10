"""The launcher's Settings > Storage (StorageScan.java with android/app/src/jvmtest's StorageScanJvmTest; StorageUi shows it on the phone): the save guard, what the
scan lists and keeps, Remove game data, and a deletion that leaves every save as it was. Also checks that the file names StorageScan repeats still match the classes
that write them. The JVM part is skipped without a JDK."""
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
SRC = REPO / 'android/app/src/main/java/dev/nxnative/android'
TEST = REPO / 'android/app/src/jvmtest/java/dev/nxnative/android/StorageScanJvmTest.java'


class StorageNames(unittest.TestCase):
    def test_names_match(self):
        scan = (SRC / 'StorageScan.java').read_text()

        def const(text, name):
            m = re.search(r'\b' + name + r'\s*=\s*"([^"]*)"', text)
            self.assertIsNotNone(m, name)
            return m.group(1)
        pz, dz, lp = (SRC / 'PackZip.java').read_text(), (SRC / 'DriverZip.java').read_text(), (SRC / 'LogProvider.java').read_text()
        self.assertEqual(const(scan, 'PACK_PARTIAL'), const(pz, 'PARTIAL'))
        self.assertEqual(const(scan, 'PACK_REPLACED'), const(pz, 'REPLACED'))
        self.assertEqual(const(scan, 'DRIVER_INSTALLING'), const(dz, 'INSTALLING'))
        self.assertEqual(const(scan, 'DRIVER_REPLACED'), const(dz, 'REPLACED'))
        self.assertEqual(const(scan, 'DIGEST'), const(lp, 'DIGEST'))


@unittest.skipUnless(shutil.which('javac') and shutil.which('java'), 'no JDK')
class StorageScanJvm(unittest.TestCase):
    def test_jvm(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            r = subprocess.run(['javac', '-Xlint:all', '-Werror', '-encoding', 'UTF-8', '-d', str(d / 'classes'), str(SRC / 'StorageScan.java'), str(SRC / 'CopyPlan.java'),
                                str(TEST)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            (d / 'run').mkdir()
            r = subprocess.run(['java', '-cp', str(d / 'classes'), 'dev.nxnative.android.StorageScanJvmTest', str(d / 'run')], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertEqual(r.stdout.strip(), 'ok')


if __name__ == '__main__':
    unittest.main()
