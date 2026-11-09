"""The launcher's notice when the game's process ends unexpectedly (android/README.md, When the game stops unexpectedly; ExitNotice.java with android/app/src/jvmtest's
ExitNoticeJvmTest): the exit record's keys, the cause sentence from the record, Android's exit reason and the log's end, and the [RED] crash: block of an uncaught
exception. The JVM part is skipped without a JDK."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
SRC = REPO / 'android/app/src/main/java/dev/nxnative/android/ExitNotice.java'
TEST = REPO / 'android/app/src/jvmtest/java/dev/nxnative/android/ExitNoticeJvmTest.java'
NXLAUNCH = REPO / 'android/app/src/main/cpp/nxlaunch.c'

# ApplicationExitInfo.REASON_*
NONE, EXIT_SELF, SIGNALED, LOW_MEMORY, CRASH, CRASH_NATIVE, ANR, USER_REQUESTED, PACKAGE_UPDATED = -1, 1, 2, 3, 4, 5, 6, 10, 16
RUN = 'v=1\ntitle=040074A01BF12000\nname=Test Game\npid=1234\nstart=1700000000000\nstate=running\nlog=040074A01BF12000-20261107-120000.log\n'


@unittest.skipUnless(shutil.which('javac') and shutil.which('java'), 'no JDK')
class ExitNotice(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix='exitnotice_'))
        subprocess.run(['javac', '-Xlint:all', '-Werror', '-encoding', 'UTF-8', '-d', str(cls.tmp / 'classes'), str(SRC), str(TEST)], check=True, capture_output=True, text=True)
        cls.n = 0

    @classmethod
    def tearDownClass(cls): shutil.rmtree(cls.tmp, ignore_errors=True)

    def java(self, *a):
        r = subprocess.run(['java', '-Dstdout.encoding=UTF-8', '-cp', str(self.tmp / 'classes'), 'dev.nxnative.android.ExitNoticeJvmTest', *map(str, a)],
                           capture_output=True, text=True, encoding='utf-8')
        self.assertEqual(r.returncode, 0, r.stderr)
        return r.stdout.rstrip('\n').split(' ', 1)[1]

    def file(self, text):
        if text is None: return '-'
        type(self).n += 1
        p = self.tmp / f'f{self.n}'
        p.write_text(text, encoding='utf-8')
        return p

    def cause(self, record, reason=NONE, status=0, desc=None, bg=False, log=None):
        r = self.java('cause', self.file(record), reason, status, desc or '-', int(bg), self.file(log))
        return None if r == 'null' else r

    def test_nothing_to_tell(self):
        self.assertIsNone(self.cause(None))                                              # no record yet
        self.assertIsNone(self.cause(RUN + 'state=stopped\n', SIGNALED, 9))              # Stop (killProcess is a SIGKILL)
        self.assertIsNone(self.cause(RUN + 'state=native\nsig=11\nshown=1\n'))          # told once
        self.assertIsNone(self.cause(RUN, USER_REQUESTED))                               # swiped away
        self.assertIsNone(self.cause(RUN, PACKAGE_UPDATED))                              # the in-app updater
        self.assertIsNone(self.cause(RUN + 'state=host_exit\nrc=0\n', EXIT_SELF, 0))    # the game returned by itself
        self.assertIsNone(self.cause(RUN, EXIT_SELF, 0, log='received SIGTERM: graceful exit\ngraceful exit done (0.1 s)\n'))

    def test_pending(self):
        self.assertEqual(self.java('pending', self.file(RUN)), 'true')
        self.assertEqual(self.java('pending', self.file(RUN + 'state=stopped\n')), 'false')
        self.assertEqual(self.java('pending', self.file(RUN + 'shown=1\n')), 'false')
        self.assertEqual(self.java('pending', self.file('garbage without keys\n')), 'false')

    def test_crashes(self):
        self.assertEqual(self.cause(RUN + 'state=java\nexception=java.lang.NullPointerException\n', CRASH), 'The app ran into an error and had to close the game.')
        self.assertEqual(self.cause(RUN, CRASH), 'The app ran into an error and had to close the game.')          # Android saw it, the record did not
        native = 'The game ran into an error and crashed.'
        self.assertEqual(self.cause(RUN + 'state=native\nsig=11\naddr=0x0\n', CRASH_NATIVE, 11), native)
        self.assertEqual(self.cause(RUN + 'state=native\nsig=11\n'), native)                                         # before Android has its exit info
        self.assertEqual(self.cause(RUN, EXIT_SELF, 128 + 11, log='[dm] CRASH sig=11 (stderr held for 1 s, no dump)\n'), native)   # crash.c's _exit(128 + sig)
        self.assertEqual(self.cause(RUN, EXIT_SELF, 128 + 6), native)
        self.assertEqual(self.cause(RUN, SIGNALED, 11), native)
        self.assertEqual(self.cause(RUN + 'state=native\nsig=6\n', CRASH_NATIVE, 6, log='[dm] FATAL too many page fault hooks\n[dm] CRASH sig=6\n'),
                         'The game reached something the app cannot handle yet, so it was stopped.')
        self.assertEqual(self.cause(RUN + 'state=host_exit\nrc=3\n', EXIT_SELF, 3), 'The game ended with an error.')

    def test_gpu(self):
        gpu = 'The GPU driver stopped responding, so the game had to close.'
        self.assertEqual(self.cause(RUN, EXIT_SELF, 0, log='[RED] GPU hang: fence 12 not reached in 6.0 s\n[RED] GPU hang: the host exits\nreceived GPU hang: graceful exit\n'), gpu)
        self.assertEqual(self.cause(RUN + 'state=native\nsig=11\n', CRASH_NATIVE, 11, log='vkQueueSubmit: VK_ERROR_DEVICE_LOST\n[dm] CRASH sig=11\n'), gpu)

    def test_android_closed_it(self):
        mem = 'Android closed it: the phone ran out of memory. Closing other apps before playing can help.'
        self.assertEqual(self.cause(RUN, LOW_MEMORY), mem)
        self.assertEqual(self.cause(RUN, SIGNALED, 9, 'lmk'), mem)                               # phones that report the low-memory killer as a signal
        self.assertTrue(self.cause(RUN, LOW_MEMORY, bg=True).startswith('Android closed it while it was in the background'))
        self.assertTrue(self.cause(RUN, SIGNALED, 9).startswith('Android closed it without saying why'))
        self.assertEqual(self.cause(RUN, ANR), 'Android closed it because it stopped responding.')
        self.assertEqual(self.cause(RUN), 'It closed without saying why.')                       # no exit info at all
        self.assertEqual(self.cause(RUN + 'state=native\nsig=6\n', log='[RED] mmap: out of memory\n[dm] CRASH sig=6\n'), mem)

    def test_last_key_wins(self):
        self.assertIsNone(self.cause(RUN + 'state=native\nsig=11\nstate=stopped\n'))
        self.assertEqual(self.cause(RUN.replace('state=running', 'state=stopped') + 'state=java\n'), 'The app ran into an error and had to close the game.')

    def test_texts(self):
        self.assertEqual(self.java('message', 'Test Game', 'It closed without saying why.'),
                         'Test Game: It closed without saying why.\\n\\nShare log sends the log of that run, which says more.')
        self.assertTrue(self.java('message', 'null', 'x').startswith('The game: x'))
        self.assertEqual(self.java('message', 'Test Game', 'The game ran into an error and crashed.', 'The game crashed on a bad memory access (SIGBUS).'),
                         'Test Game: The game ran into an error and crashed.\\n\\nLikely cause: The game crashed on a bad memory access (SIGBUS).'
                         '\\n\\nShare log sends the log of that run, which says more.')
        trace = 'java.lang.NullPointerException: boom\n\tat a.B.c(B.java:1)\n\tat a.B.d(B.java:2)\n\tat a.B.e(B.java:3)\n'
        self.assertEqual(self.java('block', 'main', self.file(trace), 2),
                         '[RED] crash: uncaught exception on thread main, the game process ends\\n[RED] crash: java.lang.NullPointerException: boom\\n'
                         '[RED] crash: \tat a.B.c(B.java:1)\\n[RED] crash: ... 2 more lines\\n')
        self.assertEqual(self.java('line', 'java.lang.IllegalStateException', 'two\nlines'), 'java.lang.IllegalStateException: two lines')
        self.assertEqual(self.java('line', 'java.lang.NullPointerException', '-'), 'java.lang.NullPointerException')


class NativeRecord(unittest.TestCase):
    """nxlaunch.c's signal handler stays async-signal-safe: no stdio, no malloc, no locks between its start and its end"""
    def test_handler_calls(self):
        src = NXLAUNCH.read_text()
        body = src[src.index('static void on_exit_signal('):]
        body = body[:body.index('\n}\n')]
        for bad in ('printf', 'malloc', 'free(', 'snprintf', 'mutex', 'LOG('):
            self.assertNotIn(bad, body, bad)
        app = src[src.index('static void exit_append('):]
        app = app[:app.index('\n}\n')]
        for bad in ('printf', 'malloc', 'fopen'):
            self.assertNotIn(bad, app, bad)


if __name__ == '__main__':
    unittest.main()
