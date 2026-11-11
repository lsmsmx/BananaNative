"""The in-game panel's pure parts (android/app/src/jvmtest's PanelJvmTest): PanelLook's size presets, PictureFit's picture size slider, DlssTune's Upscaler row and slider steps, and PictureFit.rect, the
launcher's twin of the present's vka_fit_rect (dm/gfx/vk_android/vka_present_fit.h): the touch mapping must land where the picture is drawn, so both are run over a
grid of screens, Fit / Stretch, sizes and places and must agree within a pixel. The JVM part is skipped without a JDK, the C part without a C compiler."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
JAVA = REPO / 'android/app/src/main/java/dev/nxnative/android'
SRC = [JAVA / 'PanelLook.java', JAVA / 'PictureFit.java', JAVA / 'DlssTune.java']
TEST = REPO / 'android/app/src/jvmtest/java/dev/nxnative/android/PanelJvmTest.java'
FIT = REPO / 'dm/gfx/vk_android/vka_present_fit.h'
CC = shutil.which('cc') or shutil.which('clang') or shutil.which('gcc')

C_GRID = r'''
#include <stdio.h>
#include "vka_present_fit.h"
int main(void) {
    int views[][2] = {{2400, 1080}, {1920, 1080}, {1200, 900}, {2208, 1840}, {1080, 2400}}, sizes[] = {100, 95, 80, 50, 10, 0}, places[] = {0, 5000, 10000};
    for (int v = 0; v < 5; v++) for (int st = 0; st < 2; st++) for (int s = 0; s < 6; s++) for (int p = 0; p < 3; p++) {
        int r[4]; vka_fit_rect(views[v][0], views[v][1], 1920, 1080, st, sizes[s], places[p], places[p], r);
        printf("%d %d %d %d %d %d %d %d %d\n", views[v][0], views[v][1], st, sizes[s], places[p], r[0], r[1], r[2], r[3]);
    }
    return 0;
}
'''


def rows(text):
    return [list(map(int, line.split())) for line in text.strip().splitlines()]


@unittest.skipUnless(shutil.which('javac') and shutil.which('java'), 'no JDK')
class PanelJvm(unittest.TestCase):
    def compiled(self, d):
        subprocess.run(['javac', '-Xlint:all', '-Werror', '-encoding', 'UTF-8', '-d', d, *map(str, SRC), str(TEST)], check=True, capture_output=True, text=True)

    def test_jvm(self):
        with tempfile.TemporaryDirectory() as d:
            self.compiled(d)
            r = subprocess.run(['java', '-cp', d, 'dev.nxnative.android.PanelJvmTest'], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout.strip(), 'ok')

    @unittest.skipUnless(CC, 'no C compiler')
    def test_same_rect_as_present(self):
        with tempfile.TemporaryDirectory() as d:
            self.compiled(d)
            j = subprocess.run(['java', '-cp', d, 'dev.nxnative.android.PanelJvmTest', 'grid'], capture_output=True, text=True, check=True).stdout
            c_src = Path(d) / 'grid.c'
            c_src.write_text(C_GRID)
            exe = Path(d) / 'grid'
            subprocess.run([CC, '-std=c11', '-I', str(FIT.parent), '-o', str(exe), str(c_src)], check=True, capture_output=True, text=True)
            c = subprocess.run([str(exe)], capture_output=True, text=True, check=True).stdout
            jr, cr = rows(j), rows(c)
            self.assertEqual(len(jr), len(cr))
            for a, b in zip(jr, cr):
                self.assertEqual(a[:5], b[:5])
                self.assertTrue(all(abs(x - y) <= 1 for x, y in zip(a[5:], b[5:])), f'launcher {a[5:]} vs present {b[5:]} for {a[:5]}')


if __name__ == '__main__':
    unittest.main()
