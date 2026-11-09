"""The launcher's digest of a run's log (LogDigest.java with android/app/src/jvmtest's LogDigestJvmTest; ExitWatch's "The game stopped unexpectedly" notice): on the
same synthetic logs and exit records it prints what dm/tools/log_digest.py prints in the sections it keeps (the first line, verdict with its likely cause, crash,
[RED] / error lines, rules), also for a log longer than the part it reads (line numbers counted from the file's start); every regex of dm/tools/log_rules.tsv and
log_noise.tsv compiles under Java's java.util.regex; its key-line patterns are scripts/bug_report.py's; its redaction is the tool's; the notice's likely cause
and doc link; and the APK takes the tables from dm/tools at build time, never from a copy. The JVM parts are skipped without a JDK."""
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools'))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import log_digest as L   # noqa: E402
from test_log_digest import CRASH, TID, frames, start   # noqa: E402

JAVA_DIR = REPO / 'android/app/src/main/java/dev/nxnative/android'
SRC = [JAVA_DIR / 'LogDigest.java', JAVA_DIR / 'ExitNotice.java', REPO / 'android/app/src/jvmtest/java/dev/nxnative/android/LogDigestJvmTest.java']
RULES, NOISE = REPO / 'dm/tools/log_rules.tsv', REPO / 'dm/tools/log_noise.tsv'
GRADLE = REPO / 'android/app/build.gradle'
DOCS = 'https://git.eden-emu.dev/moceure/BananaNative/src/branch/android/'

NONE, SIGNALED, LOW_MEMORY, CRASH_NATIVE = -1, 2, 3, 5
RUN = f'v=1\ntitle={TID}\nname=Test Game\npid=1234\nstart=1700000000000\nstate=running\nlog={TID}-20261109-101010.log\n'
KEPT = ('== log digest', '-- verdict', '-- crash', '-- [RED] / error lines', '-- rules')


def sections(text):
    """{first line of a section: its lines} for the sections the launcher keeps; lines in parentheses (notes) left out"""
    out, cur = {}, None
    for line in text.split('\n'):
        if line.startswith(('-- ', '== ')):
            cur = next((k for k in KEPT if line.startswith(k)), None)
            if cur: out[cur] = [line]
            continue
        if cur and line and not line.startswith('('): out[cur].append(line)
    return out


@unittest.skipUnless(shutil.which('javac') and shutil.which('java'), 'no JDK')
class LauncherDigest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix='logdigest_'))
        subprocess.run(['javac', '-Xlint:all', '-Werror', '-encoding', 'UTF-8', '-d', str(cls.tmp / 'classes'), *map(str, SRC)], check=True, capture_output=True, text=True)
        cls.n = 0

    @classmethod
    def tearDownClass(cls): shutil.rmtree(cls.tmp, ignore_errors=True)

    def java(self, *a):
        r = subprocess.run(['java', '-Dstdout.encoding=UTF-8', '-cp', str(self.tmp / 'classes'), 'dev.nxnative.android.LogDigestJvmTest', *map(str, a)],
                           capture_output=True, text=True, encoding='utf-8')
        self.assertEqual(r.returncode, 0, r.stderr)
        return r.stdout

    def file(self, text, name=None):
        if text is None: return '-'
        type(self).n += 1
        p = self.tmp / (name or f'f{self.n}')
        p.write_text(text, encoding='utf-8')
        return p

    def both(self, lines, record=None, reason=NONE, status=0, desc=None, bg=False, tail=1 << 21):
        """(the tool's sections, the launcher's sections, the launcher's verdict line fields) for one log"""
        d = self.tmp / f'run{self.n}'; d.mkdir(); type(self).n += 1
        log = d / f'{TID}-20261109-101010.log'
        log.write_text('\n'.join(lines) + '\n', encoding='utf-8')
        rec = L.parse_record(record or '')
        rec.update({'android_reason': str(reason), 'android_status': str(status), 'android_description': desc or '', 'android_importance': '400' if bg else '100'})
        dg = L.Digest(L.load_rules(), L.load_symptoms())
        with L.resolve(log).open_log() as f:
            for line in f: dg.feed(line)
        py = L.render(dg.result(log.name, log.stat().st_size, rec))
        out = self.java('digest', RULES, NOISE, log, self.file(record), reason, status, desc or '-', int(bg), tail)
        text, _, verdict = out.rpartition('== verdict ')
        return sections(py), sections(text), verdict.rstrip('\n').split('\t')

    def same(self, *a, **kw):
        py, jv, v = self.both(*a, **kw)
        self.assertEqual(jv, py)
        return py, v

    def test_crash(self):
        py, v = self.same(start() + frames(2) + CRASH, RUN + 'state=native\nsig=11\n', CRASH_NATIVE, 11)
        self.assertEqual(v[:2], ['crashed', 'crash-segv'])
        self.assertEqual(v[2], DOCS + 'docs/agents/05-debugging.md#reading-a-crash-report')
        self.assertEqual(v[3], 'The game crashed reading or writing memory it should not (SIGSEGV).')   # the notice's line under its own sentence
        self.assertIn('  first game frame: main+0x8109d8', py['-- crash'])

    def test_host_fatal(self):
        _, v = self.same(start()[:20] + ['[dm] FATAL the game files /storage/emulated/0/Switch2/Game/romfs (rom:) are missing: neither a folder nor a RomFS image',
                                         '[dm] CRASH sig=6 addr=0x1 pc=abort+0xa0 lr=abort+0x7c sp=0x1'], RUN + 'state=native\nsig=6\n', CRASH_NATIVE, 6)
        self.assertEqual(v[:2], ['host_fatal', 'romfs-missing'])

    def test_gpu(self):
        _, v = self.same(start() + frames(2) + ['[dm] [RED] vk: upload batch 12: the wait failed after 0.01 s (-4: the device is lost)',
                                                '[dm] [RED] vk: GPU hang: queue; fence 9 not reached in 6.0 s: nothing more is submitted',
                                                '[dm] received GPU hang: graceful exit'], RUN, 1, 0)
        self.assertEqual(v[:2], ['driver_lost', 'gpu-hang'])
        self.assertTrue(v[2].startswith(DOCS + 'docs/reference/rendering.md#'))

    def test_memory_and_android_kills(self):
        _, v = self.same(start() + frames(1) + ['[dm] [RED] mmap: out of memory'], RUN, LOW_MEMORY, 0, bg=True)
        self.assertEqual(v[:2], ['out_of_memory', 'oom-line'])
        _, v = self.same(start() + frames(1), RUN, SIGNALED, 9, 'killed by lmkd')
        self.assertEqual(v, ['out_of_memory', '-', '-', '-'])          # no line says why: no likely cause, no link
        _, v = self.same(start() + frames(1), RUN, SIGNALED, 9)
        self.assertEqual(v[0], 'killed')

    def test_java_crash(self):
        _, v = self.same(start() + ['[RED] crash: uncaught exception on thread main, the game process ends',
                                    '[RED] crash: java.lang.IllegalStateException: DM_TEST_CRASH=java: a test crash on purpose',
                                    '[RED] crash: \tat dev.nxnative.android.X.run(X.java:1)'], RUN + 'state=java\n', 4)
        self.assertEqual(v[:2], ['java_crash', 'java-crash'])
        self.assertEqual(v[3], '-')   # its cause is the notice's sentence: not said twice

    def test_watchdog_and_cut_short(self):
        _, v = self.same(start() + frames(1) + ['[dm] [RED] watchdog: no progress since second 40, stuck at tick 12 / stage present', '[dm] CRASH sig=14 pc=0x1'],
                         RUN + 'state=native\nsig=14\n', CRASH_NATIVE, 14)
        self.assertEqual(v[:2], ['hang', 'watchdog'])
        py, v = self.same(start()[:22], RUN)
        self.assertEqual(v[0], 'cut_short')
        self.assertIn('It stopped before stage 6.', py['-- verdict'][1])

    def test_many_groups_keys_and_paths(self):
        lines = start() + [f'[dm] [RED] svc: unknown thing kind{chr(97 + i)} at /storage/emulated/0/Android/data/dev.nxnative.android/files/x{i}' for i in range(20)]
        lines += ['title_key = 0123456789abcdef0123456789abcdef', '[dm] [RED] ' + 'long line ' * 40, '[dm] [RED] naïve ünïcode ' + 'é' * 300]
        py, _ = self.same(lines, RUN, 1, 0)
        self.assertIn('  +7 more groups', py['-- [RED] / error lines'])
        self.assertNotIn('0123456789abcdef', ''.join(sum(py.values(), [])))

    def test_long_log_tail(self):
        """a log longer than the part read: the launcher reads its end and numbers its lines from the file's start"""
        end = start() + frames(2) + CRASH
        filler = [f'[dm] audio #0 device callbacks {i}: 96–96 frames each, in these 2000, 0 underruns' for i in range(3000)]
        size = len(('\n'.join(end) + '\n').encode())
        py, v = self.same(filler + end, RUN + 'state=native\n', CRASH_NATIVE, 11, tail=size + 40)   # starts inside the filler, in the middle of a line
        self.assertTrue(py['== log digest'][0].startswith(f'== log digest: {TID}-20261109-101010.log ({3000 + len(end)} lines'))
        self.assertEqual(v[1], 'crash-segv')

    def test_crlf(self):
        self.same([x + '\r' for x in start() + CRASH], RUN + 'state=native\n', CRASH_NATIVE, 11)

    def test_tables_compile_in_java(self):
        """every regex of both tables compiles under java.util.regex, with the same rows the tool reads"""
        for kind, path, rows in (('rules', RULES, L.load_rules()), ('noise', NOISE, L.load_noise())):
            out = self.java('table', kind, path).split('\n')
            self.assertEqual([x for x in out if x], [f'ok {r["id"]}' for r in rows], kind)
        bad = self.file('id\tkind\tregex\tcause\tdoc\nx\tcrash\t(?P<n>a)\tc\t\n')
        self.assertTrue(self.java('table', 'rules', bad).startswith('bad '))

    def test_key_patterns_are_bug_reports(self):
        k1, k2 = self.java('keys').split('\n')[:2]
        self.assertEqual([k1, k2], [L.KEY_LINE.pattern.replace("\\'", "'"), L.KEY_LINE2.pattern])

    def test_redact_is_the_tools(self):
        lines = ['[dm] [RED] at 0x7a8d5de3b0 in /system/lib64/libc.so', '/storage/emulated/0/Android/data/dev.nxnative.android/files/logs/a.log', '/data/user/0/dev.x/cache/y',
                 '/data/app/~~a==/dev.nxnative.android-b==/base.apk', '/sdcard/Switch2/x', '/storage/1234-ABCD/g', '/home/someone/x', 'abc' * 80,
                 'f' * 40, 'é' * 250]
        out = self.java('redact', self.file('\n'.join(lines))).split('\n')
        self.assertEqual(out[:len(lines)], [L.redact(x) for x in lines])


class Packaging(unittest.TestCase):
    def test_tables_come_from_dm_tools(self):
        g = GRADLE.read_text()
        self.assertIn("tasks.register('nxDigestAssets', Sync)", g)
        self.assertIn("from(new File(repo, 'dm/tools')) { include 'log_rules.tsv', 'log_noise.tsv' }", g)
        self.assertIn("into layout.buildDirectory.dir('generated/nxdigest/assets/digest')", g)
        self.assertIn("dependsOn nxDigest", g)
        src = (JAVA_DIR / 'LogDigest.java').read_text()
        self.assertIn('RULES_ASSET = "digest/log_rules.tsv", NOISE_ASSET = "digest/log_noise.tsv"', src)
        copies = [p for p in (REPO / 'android').rglob('log_*.tsv') if 'build' not in p.relative_to(REPO / 'android').parts]
        self.assertEqual(copies, [], 'a hand copy of a digest table in android/')

    def test_docs_base_is_the_updaters_forgejo(self):
        u = (JAVA_DIR / 'UpdateCheck.java').read_text()
        self.assertIn('FORGEJO_HOST = "git.eden-emu.dev"', u)
        self.assertIn('REPO = "moceure/BananaNative"', u)
        self.assertIn(f'DOCS_BASE = "{DOCS}"', (JAVA_DIR / 'LogDigest.java').read_text())


if __name__ == '__main__':
    unittest.main()
