"""The Android launcher's emulator-frontend entry (FrontendLaunch, android/FRONTENDS.md): what Frontend.java reads from a frontend's intent (file:// URIs, provider
URIs that carry a /storage path, the shared storage's aliases, the card a path names, title IDs), on the JVM with android/app/src/jvmtest's FrontendJvmTest.
Also checks that the manifest exports only the alias, and the alias by the application ID. Skipped without a JDK."""
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
SRC = REPO / 'android/app/src/main/java/dev/nxnative/android'
TEST = REPO / 'android/app/src/jvmtest/java/dev/nxnative/android/FrontendJvmTest.java'
MANIFEST = REPO / 'android/app/src/main/AndroidManifest.xml'


@unittest.skipUnless(shutil.which('javac') and shutil.which('java'), 'no JDK')
class FrontendJvm(unittest.TestCase):
    def test_frontend_paths(self):
        with tempfile.TemporaryDirectory() as d:
            subprocess.run(['javac', '-Xlint:all', '-Werror', '-d', d, str(SRC / 'Frontend.java'), str(TEST)], check=True, capture_output=True, text=True)
            r = subprocess.run(['java', '-cp', d, 'dev.nxnative.android.FrontendJvmTest'], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertTrue(r.stdout.startswith('ok '), r.stdout)


class FrontendManifest(unittest.TestCase):
    def test_aliases(self):
        m = MANIFEST.read_text()
        act = re.search(r'<activity\s[^>]*android:name="\.FrontendLaunch"[^>]*>', m)
        self.assertIsNotNone(act)
        self.assertIn('android:exported="false"', act.group(0))
        # Android 15 checks an explicit intent against an exported component's filters when it has any, and never matches one with no action (Daijisho's
        # -e game {file.path}): the frontends' alias must have none
        launch = re.search(r'<activity-alias\s[^>]*?android:name="\$\{applicationId\}\.Launch"[^>]*?(/>|>)', m)
        self.assertIsNotNone(launch)
        self.assertEqual(launch.group(1), '/>', 'the Launch alias must have no intent filters')
        self.assertIn('android:targetActivity=".FrontendLaunch"', launch.group(0))
        self.assertIn('android:exported="true"', launch.group(0))
        op = re.search(r'<activity-alias\s[^>]*?android:name="\$\{applicationId\}\.Open"[^>]*>(.*?)</activity-alias>', m, re.S)
        self.assertIsNotNone(op)
        self.assertIn('pathPattern', op.group(1))

    def test_category(self):
        m = MANIFEST.read_text()
        app = re.search(r'<application\s[^>]*>', m)
        self.assertIsNotNone(app)
        self.assertIn('android:appCategory="game"', app.group(0))


if __name__ == '__main__':
    unittest.main()
