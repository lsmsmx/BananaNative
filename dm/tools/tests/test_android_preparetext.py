"""Prepare's words for the player on the JVM (android/app/src/main PrepareText.java, Profile.missing): javac compiles them with a stub BuildConfig and
android/app/src/jvmtest's PrepareTextJvmTest. Main texts carry no file path or tool output (those go to the card's Details); a failure offers Retry, Try anyway or
nothing; the progress line never says "0 of 0"; a job of a game the APK does not include gets one sentence and no Retry. Also that the launcher keeps to it: the job
card hides Retry and Play for such a job, and the service refuses to run it. An APK whose translator does not match its own tools gets its own offer (FIX_UPDATE):
the tool's refusal and PrepareText's match are the same text, and the card hides Retry for that install. Skipped without a JDK (the source checks run anyway)."""
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
SRC = REPO / 'android/app/src/main/java/dev/nxnative/android'
ONDEVICE = REPO / 'android/app/src/ondevice/java/dev/nxnative/android'
TEST = REPO / 'android/app/src/jvmtest/java/dev/nxnative/android/PrepareTextJvmTest.java'
STUB = '''package dev.nxnative.android;
public final class BuildConfig {
    public static final String NX_PROFILES = "040074A01BF12000\\tDonkey Kong Bananza\\t22852\\tabcdef012345\\t458752 main=aa";
}
'''


@unittest.skipUnless(shutil.which('javac') and shutil.which('java'), 'no JDK')
class Words(unittest.TestCase):
    def test_jvm(self):
        with tempfile.TemporaryDirectory(prefix='preparetext_') as t:
            stub = Path(t) / 'BuildConfig.java'
            stub.write_text(STUB)
            subprocess.run(['javac', '-Xlint:all', '-Werror', '-d', t, str(SRC / 'PrepareText.java'), str(SRC / 'Profile.java'), str(stub), str(TEST)], check=True)
            r = subprocess.run(['java', '-cp', t, 'dev.nxnative.android.PrepareTextJvmTest'], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertRegex(r.stdout, r'^ok \d+')


class Wiring(unittest.TestCase):
    def test_service_refuses_a_game_not_included(self):   # the trigger: a remembered job of a game this APK has no profile for ran its steps on Retry
        src = (ONDEVICE / 'PackService.java').read_text(encoding='utf-8')
        work = src[src.index('private void work()'):]
        self.assertLess(work.index('Profile.missing(j.titleId(), j.name())'), work.index('dropOtherSplitScans(j)'), 'checked before any step runs')
        self.assertIn('j.fail(PrepareText.notIncluded(missing))', work)
        self.assertNotIn('j.state("failed"', src, 'every failure goes through Job.fail (short text, Details, the offer)')

    def test_card_offers_only_what_can_work(self):
        src = (ONDEVICE / 'Ondevice.java').read_text(encoding='utf-8')
        self.assertIn('go.setVisibility(noFix || broken ? View.GONE : View.VISIBLE)', src, 'no Retry that cannot succeed')
        self.assertIn('play.setVisibility(noFix ? View.GONE : View.VISIBLE)', src)
        self.assertIn('PrepareText.FIX_ANYWAY.equals(job.fix())', src, 'Try anyway only where the job can go on with a check relaxed')
        self.assertNotIn('" · update " + job.version()', src, 'no raw version number in the card\'s title line')

    def test_translator_mismatch_is_the_tools_text(self):   # the trigger (2026-11-10): an APK with a stale translator library; its card said Tap Retry
        java = (SRC / 'PrepareText.java').read_text(encoding='utf-8')
        m = re.search(r'static final String XLATE_MISMATCH = "([^"]+)";', java)
        self.assertIsNotNone(m, 'PrepareText.XLATE_MISMATCH')
        tool = (REPO / 'dm/gfx/tools/vk_android_build.py').read_text(encoding='utf-8')
        self.assertIn(f"sys.exit('\\n'.join('[RED] {m.group(1)}' + b for b in _bad))", tool, 'check_translator refuses with the text PrepareText matches')
        self.assertIn('if (red.startsWith(XLATE_MISMATCH))', java)

    def test_broken_app_card(self):   # FIX_UPDATE: no Retry while the install it failed under is there; Check for updates where the app has an updater
        card = (ONDEVICE / 'Ondevice.java').read_text(encoding='utf-8')
        self.assertIn('boolean broken = failed && !alive && job.brokenApk(Job.installed(a));', card)
        self.assertIn('update.setVisibility(broken && Updater.available(a) ? View.VISIBLE : View.GONE);', card)
        self.assertIn('Updater.checkNow(a)', card)
        svc = (ONDEVICE / 'PackService.java').read_text(encoding='utf-8')
        self.assertIn('if (PrepareText.FIX_UPDATE.equals(j.fix())) j.failApk(Job.installed(this));', svc)
        job = (ONDEVICE / 'Job.java').read_text(encoding='utf-8')
        self.assertIn('o.remove("fail_apk");', job, 'a new state forgets the install it failed under')
        self.assertIn('o.optLong("fail_apk", -1) == installed', job)

    def test_try_anyway_relaxes_only_named_checks(self):
        src = (ONDEVICE / 'PackService.java').read_text(encoding='utf-8')
        self.assertIn('DumpCheck.check(j.dump(), false, j.tryAnyway())', src)
        self.assertIn('j.tryAnyway() ? null : splitMatchesProfile(j, split)', src)
        self.assertIn('PackEngine.setenv("DMSH_VK_REQUIRE_ALL", j.tryAnyway() ? "0" : "1")', src)


if __name__ == '__main__':
    unittest.main()
