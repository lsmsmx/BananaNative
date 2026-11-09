"""dm/tools/log_digest.py on synthetic Android run logs: the verdict of a crash, a host FATAL, a lost device, an out-of-memory end (from last-exit.txt and logcat), a clean
run with its frame rate, a log cut short; zip and folder inputs; redaction of app folders and key-like lines; --filter (routine lines dropped, unknown and [RED] lines kept,
runs collapsed, statistics summarised); byte-identical output for the same input; and the two tables (log_rules.tsv, log_noise.tsv): every regex compiles, keeps to what
Java's regex accepts, its literal gate never hides a match, and every doc link names an existing file and heading. No game files or real logs needed."""
import io
import json
import re
import subprocess
import sys
import tempfile
import unittest
import unittest.mock
import zipfile
from contextlib import redirect_stdout
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools'))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import log_digest as L   # noqa: E402
import test_docs   # noqa: E402

TID = '0100AAAA0000B000'
APP = '/storage/emulated/0/Android/data/dev.nxnative.android/files'
KEY = '0123456789abcdef0123456789abcdef'


def start(extra_env=()):
    """the launcher's and the host's start-up lines, as the sources print them (GameActivity.start, nxlaunch.c, main.c, plat_android.c, hle_svc.c, hle_nvn.c)"""
    return [
        '[nxlaunch] build Banana Native test · int@abc1234 · signer 0011223344556677',
        '[nxlaunch] env DM_ROMFS=/storage/emulated/0/Switch2/Game/updates/65536/romfs',
        f'[nxlaunch] env DM_SAVE={APP}/saves/{TID}',
        f'[nxlaunch] env HOME={APP}',
        '[nxlaunch] env DM_VK_DRIVER=/data/user/0/dev.nxnative.android/files/gpu_drivers/Mainline_Turnip_v32',
        '[nxlaunch] env DM_WATCH_STALL=25',
        *[f'[nxlaunch] env {e}' for e in extra_env],
        '[nxlaunch] arg 0: nxhost',
        '[nxlaunch] hooks: window yes, input yes, lifecycle yes',
        '[nxlaunch] nxhost_main starts on its own thread (8 MiB stack)',
        f'[dm] build: commit abc1234def56, shader chain 111122223333, profile {TID}',
        f'[nxlaunch] [readiness] {TID}: PASS; Adreno (TM) 840, Vulkan 1.4.300; driver turnip Mesa driver (Mesa 26.0), driver ID 18, version 0x1, custom driver Mainline_Turnip_v32; RAM 12 G',
        f'[nxlaunch] nxnative 0.2.4 (abc1234def56), host libnxhost_{TID}.so; Test Game {TID} version 65536',
        f'[nxlaunch] device ACME Phone-1, Android 16 (SDK 36), arm64-v8a; game /storage/emulated/0/Switch2/Game; pack {APP}/packs/{TID}',
        '[nxlaunch] GPU driver: Mainline Turnip v32 (Vulkan 1.4.335) (/data/user/0/dev.nxnative.android/files/gpu_drivers/Mainline_Turnip_v32) (picked in the launcher; a DM_VK_DRIVER line in env.txt wins, the host\'s [vk] driver line says which ran)',
        '[dm] thread slots: tpidr_el0 + 0x10 → K18 +0 KTLS +8 KTLR +0x10 KGATE +0x18',
        '[dm] main: gates 54 patch_off 0x2820000 keys 0/1/2/3',
        f'[dm] cheats: {APP}/root/cheats: no file for build 0011AABBCCDDEEFF',
        '[dm] sdk: NintendoSDK nnSdk-Generic_20_5_19-Rele (major 20)',
        '[dm] stubs=0',
        '[dm] >>> nnosInitialize',
        '[dm] guest AS 0x4000000000 +0x270000000  heap 0x4030000000 alias 0x4010000000 stack 0x4020000000',
        '[dm] >>> nninitInitializeSdkModule',
        '[dm] >>> nninitStartup',
        '[dm] >>> nnMain',
        '[dm] [RED] gfx: mtl_bw not yet on Android (NVN → Vulkan replay)',
        '[dm] [nvn] device API 231.30 (SDK 20, Switch 2)',
        '[dm] [vk] shader library /sdcard/Switch2/pack/shaders/v-aaaabbbbcccc/dmsh_vk.lib: 100 shaders, 120 modules, 1.0 MB (chain 111122223333, Vulkan chain aaaabbbbcccc, inputs 000011112222; 64-lane subgroups, storage buffers at 64)',
    ]


def frames(n=3, fps=(58.0, 60.0, 30.0)):
    out = []
    for i in range(n):
        f = 300 * (i + 1)
        out += [f'[dm] [longframe] frame {f - 7} ({f / 60:.2f} s) whole frame {40 + i}.0 ms, submit+present 0.4 ms, GPU 20.0 ms (busy; span 21.0 ms, 1.0 waiting for the recording)',
                f'[dm] [vk] fps: {fps[i % len(fps)]:.1f} game frames a second ({1000 / fps[i % len(fps)]:.1f} ms), {fps[i % len(fps)]:.1f} presented',
                f'[dm] [nvn] timing up to frame {f}: frame time p50 16.6{i} / p95 2{i}.00 / max 5{i}.00 ms (300 frames); frame work p50 16.0 / p95 17.0 / max 18.0 ms (300 frames)',
                f'[dm] [nvn] on-screen up to frame {f}: 300 frames, dropped {i} (discarded 0), p99 16.59 ms; footprint {2000 + 100 * i} MiB; brightness -1.00; thermal {i}',
                f'[dm] audio #0 device callbacks {2000 * (i + 1)}: 96–96 frames each, 4.0 blocks queued on average at callback; in these 2000, 0 underruns, 0.0 ms zero-padded']
    return out


CRASH = ['[dm] [RED] cannot open the standard shared font (point DM_SHARED_FONT or DM_FONT_DIR/standard.ttf at a TTF)',
         '[dm] CRASH sig=11 addr=0x100 pc=main+0x8109d8 lr=main+0x8109d0 sp=0x7a8d5de3b0',
         'x0=0 x1=0x3756 x2=0x6 x3=0x40211e9d70 x4=0x1 x5=0x2',
         '[dm] *x3: 0 0xffffffff 0x28d300003593 0 0 0',
         '[dm] bt: main+0x8109d0 main+0x2ea2ac 0x1234 sdk+0x10 main+0x5 main+0x6 main+0x7 main+0x8 main+0x9 main+0xa',
         '[dm] fp=0x7a8d5de530  0x100 bytes below sp:',
         '[dm]   0 0 0 0',
         '[dm] stack scan sp=0x7a8d5de3b0: [+8]main+0x36c8020 [+464]main+0x36c8020',
         "[dm] thread 0x1026 'Worker' priority 55 core 0 mask 0x1 CPU 0.00s svc 2  created at tick 0 (0.0 s ago) waits:address addr=0x1 last svc 0x34 lr=sdk+0x10"]


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def write(self, lines, name=f'{TID}-20261109-101010.log'):
        p = self.root / name; p.write_text('\n'.join(lines) + '\n'); return p

    def run_main(self, *args):
        out = io.StringIO()
        with redirect_stdout(out): rc = L.main([str(a) for a in args])
        return rc, out.getvalue()

    def digest(self, lines, **kw):
        p = self.write(lines)
        return L.digest(L.resolve(p, kw.get('logcat'), kw.get('exit_file')), kw.get('tail', 15))


class Verdicts(Base):
    def test_crash(self):
        d = self.digest(start() + frames(1) + CRASH)
        self.assertEqual(d['verdict']['kind'], 'crashed')
        self.assertEqual(d['verdict']['sentence'], L.NOTICE['NATIVE'])
        self.assertEqual(d['verdict']['rule'], 'crash-segv')
        c = d['crash']
        self.assertEqual((c['signal'], c['pc'], c['lr'], c['first_game_frame']), (11, 'main+0x8109d8', 'main+0x8109d0', 'main+0x8109d8'))
        self.assertEqual(len(c['backtrace']), 8)
        self.assertEqual(c['stack_scan'], ['main+0x36c8020', 'main+0x36c8020'])
        self.assertEqual(d['stage']['number'], 8)
        text = L.render(d)
        block = text.split('-- crash')[1].split('\n--')[0]
        self.assertNotIn("'Worker' priority", block); self.assertNotIn('device callbacks', block)   # not the thread dump, not routine lines
        self.assertLessEqual(text.count('\n'), 80)
        self.assertTrue(any('No stand-in shared font' in s['heading'] for s in d['symptoms']), d['symptoms'])

    def test_host_fatal(self):
        d = self.digest(start()[:20] + ['[dm] FATAL the game files /storage/emulated/0/Switch2/Game/romfs (rom:) are missing: neither a folder nor a RomFS image',
                                       '[dm] CRASH sig=6 addr=0x1 pc=abort+0xa0 lr=abort+0x7c sp=0x1'])
        self.assertEqual((d['verdict']['kind'], d['verdict']['rule']), ('host_fatal', 'romfs-missing'))
        self.assertEqual(d['verdict']['sentence'], L.NOTICE['FATAL'])
        self.assertTrue(d['verdict']['doc'].startswith('docs/reference/packing.md#'))
        self.assertEqual(d['stage']['number'], 5)

    def test_device_lost(self):
        d = self.digest(start() + frames(2) + ['[dm] [RED] vk: upload batch 12: the wait failed after 0.01 s (-4: the device is lost)',
                                               '[dm] [RED] vk: GPU hang: queue; fence 9 not reached in 6.0 s (the timeline at 8, 9 handed to the queue, 9 reserved): nothing more is submitted, every fence reads as done',
                                               '[dm] [RED] vk: GPU hang: the host exits (DM_VK_HANG_EXIT=0 runs on without the GPU)', '[dm] received GPU hang: graceful exit'])
        self.assertEqual(d['verdict']['kind'], 'driver_lost')
        self.assertEqual(d['verdict']['sentence'], L.NOTICE['GPU'])
        self.assertEqual(d['verdict']['rule'], 'gpu-hang')

    def test_oom_from_last_exit_and_logcat(self):
        rec = self.root / 'last-exit.txt'
        rec.write_text(f'v=1\ntitle={TID}\nname=Test Game\npid=4242\nstart=1\nstate=running\nlog={TID}-20261109-101010.log\n')
        lc = self.root / 'logcat.txt'
        lc.write_text('11-09 10:12:00.000  1172  1172 I lowmemorykiller: Kill \'dev.nxnative.android:game\' (4242), uid 10451, oom_score_adj 0 to free 6000000kB rss\n'
                      '11-09 10:12:00.100   700   720 I ActivityManager: Process dev.nxnative.android:game (pid 4242) has died: fg  TOP\n'
                      '11-09 10:12:00.200   999   999 I lowmemorykiller: Kill \'com.example.other\' (1), uid 1\n')
        d = self.digest(start() + frames(2), logcat=lc, exit_file=rec)
        self.assertEqual((d['verdict']['kind'], d['verdict']['rule']), ('out_of_memory', 'lmk-kill'))
        self.assertEqual(d['verdict']['sentence'], L.NOTICE['MEMORY'])

    def test_oom_from_exit_info_and_native_record(self):
        info = self.root / 'exitinfo.txt'
        info.write_text('  package: dev.nxnative.android\n        ApplicationExitInfo #0:\n          timestamp=x pid=4242\n'
                        '          process=dev.nxnative.android:game reason=3 (LOW MEMORY) subreason=0 status=0\n          importance=400 pss=0\n          description=lmk\n'
                        '        ApplicationExitInfo #1:\n          process=dev.nxnative.android:game reason=10 (USER REQUESTED) status=0\n')
        d = self.digest(start() + frames(1), exit_file=info)
        self.assertEqual(d['verdict']['kind'], 'out_of_memory')
        self.assertEqual(d['verdict']['sentence'], L.NOTICE['MEMORY_BG'])   # importance 400: in the background
        rec = self.root / 'last-exit.txt'; rec.write_text('v=1\nstate=running\nstate=native\nsig=6\n')
        d = self.digest(start() + ['libc++abi: terminating due to uncaught exception of type St9bad_alloc: std::bad_alloc'], exit_file=rec)
        self.assertEqual((d['verdict']['kind'], d['verdict']['rule']), ('out_of_memory', 'bad-alloc'))

    def test_clean_run_with_fps(self):
        d = self.digest(start() + frames(3) + ['[dm] [lifecycle] background: threads\' CPU since the last pause or resume 1.00 s in 2.0 s (40 threads)',
                                               '[dm] received SIGTERM: graceful exit', '[dm] graceful exit done (0.120 s)'])
        self.assertEqual(d['verdict']['kind'], 'normal_exit')
        p = d['perf']
        self.assertEqual(p['fps_game'], {'min': 30.0, 'median': 58.0, 'max': 60.0, 'n': 3})
        self.assertEqual(p['frame_p95_ms']['max'], 22.0)
        self.assertEqual(p['on_screen'], {'frames': 900, 'dropped': 3})
        self.assertEqual(p['footprint_mib']['max'], 2200)
        self.assertEqual(p['thermal_status'], {'min': 0, 'median': 1, 'max': 2, 'n': 3})
        self.assertEqual(p['longframes']['count'], 3)
        self.assertEqual(p['lifecycle']['count'], 1)
        h = d['header']
        self.assertEqual(h['device'], 'ACME Phone-1, Android 16 (SDK 36), arm64-v8a')
        self.assertEqual(h['driver_kind'], 'custom (DM_VK_DRIVER Mainline_Turnip_v32)')
        self.assertEqual(h['driver_id'], 18)
        self.assertEqual(h['game_build_id'], '0011AABBCCDDEEFF')
        self.assertEqual(h['app_id'], 'dev.nxnative.android')
        self.assertEqual(h['switches'], ['DM_VK_DRIVER=Mainline_Turnip_v32', 'DM_WATCH_STALL=25'])
        self.assertEqual(d['errors']['groups'], []); self.assertEqual(d['errors']['expected_stubs'], {'lines': 1, 'groups': 1})

    def test_cut_short(self):
        d = self.digest(start()[:22])
        self.assertEqual(d['verdict']['kind'], 'cut_short')
        self.assertIn('stopped before stage 6', d['verdict']['sentence'])
        self.assertEqual(d['stage']['number'], 5)

    def test_stopped_and_watchdog(self):
        rec = self.root / 'last-exit.txt'; rec.write_text('state=running\nstate=stopped\n')
        self.assertEqual(self.digest(start() + frames(1), exit_file=rec)['verdict']['kind'], 'stopped')
        d = self.digest(start() + frames(1) + ['[dm] [RED] watchdog: no progress since second 40, stuck at tick 12 / stage present'])
        self.assertEqual((d['verdict']['kind'], d['verdict']['rule']), ('hang', 'watchdog'))

    def test_java_crash_and_tombstone(self):
        d = self.digest(start() + ['[RED] crash: uncaught exception on thread main, the game process ends',
                                   '[RED] crash: java.lang.IllegalStateException: DM_TEST_CRASH=java: a test crash on purpose', '[RED] crash: \tat dev.nxnative.android.X.run(X.java:1)'])
        self.assertEqual(d['verdict']['kind'], 'java_crash')
        self.assertEqual(d['crash']['java']['thread'], 'main')
        lc = self.root / 'lc.txt'
        lc.write_text('\n'.join([
            '11-08 04:13:30.198  2666  2753 F libc    : Fatal signal 6 (SIGABRT), code -1 (SI_QUEUE) in tid 2753 (vk-replay), pid 2666 (ve.android:game)',
            '11-08 04:13:31.654  4435  4435 F DEBUG   : *** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***',
            '11-08 04:13:31.655  4435  4435 F DEBUG   : Cmdline: dev.nxnative.android:game',
            '11-08 04:13:31.655  4435  4435 F DEBUG   : pid: 2666, ppid: 3117, tid: 2753, name: vk-replay  >>> dev.nxnative.android:game <<<',
            '11-08 04:13:31.655  4435  4435 F DEBUG   : signal 6 (SIGABRT), code -1 (SI_QUEUE), fault addr --------',
            "11-08 04:13:31.655  4435  4435 F DEBUG   : Abort message: 'Scudo ERROR: internal map failure (error desc=Out of memory)'",
            '11-08 04:13:31.655  4435  4435 F DEBUG   :       #00 pc 0000000000093f04  /apex/com.android.runtime/lib64/bionic/libc.so (raise+136) (BuildId: 7880f6fb2b18023401eaaf250b1957cb)',
            '11-08 04:13:31.655  4435  4435 F DEBUG   :       #01 pc 000000000000a5bc  /data/app/~~abc==/dev.nxnative.android-xyz==/base.apk!libnxhost.so (offset 0x578000)',
        ]) + '\n')
        d = self.digest(start() + frames(1), logcat=lc)
        self.assertEqual(d['verdict']['kind'], 'out_of_memory')
        t = d['crash']['logcat']
        self.assertEqual(t['thread'], 'vk-replay')
        self.assertIn('Scudo ERROR', t['abort'])
        self.assertEqual(len(t['frames']), 2)
        self.assertIn('<apk>/base.apk', t['frames'][1]); self.assertNotIn('BuildId', t['frames'][0])


class Inputs(Base):
    def test_zip(self):
        log = '\n'.join(start() + frames(1) + CRASH) + '\n'
        z = self.root / 'report.zip'
        with zipfile.ZipFile(z, 'w') as f:
            f.writestr(f'logs/{TID}-20261101-000000.log', '\n'.join(start()[:5]) + '\n')
            f.writestr(f'logs/{TID}-20261109-101010.log', log)
            f.writestr('report.md', '# report\n')
            f.writestr('screens/shot.png', b'\x89PNG\0\0')
        before = sorted(p.name for p in self.root.iterdir())
        rc, out = self.run_main(z)
        self.assertEqual(rc, 0)
        self.assertIn(f'report.zip:logs/{TID}-20261109-101010.log', out)
        self.assertIn('crashed:', out)
        self.assertEqual(sorted(p.name for p in self.root.iterdir()), before)   # nothing extracted

    def test_folder_picks_newest_and_its_record(self):
        self.write(start()[:5], f'{TID}-20261101-000000.log')
        self.write(start() + frames(1), f'{TID}-20261109-101010.log')
        (self.root / 'last-exit.txt').write_text(f'state=running\nlog={TID}-20261109-101010.log\nstate=stopped\n')
        rc, out = self.run_main(self.root, '--json')
        d = json.loads(out)
        self.assertEqual((rc, d['file'], d['verdict']['kind']), (0, f'{TID}-20261109-101010.log', 'stopped'))

    def test_unreadable_is_exit_2(self):
        out = io.StringIO()
        with redirect_stdout(out), unittest.mock.patch('sys.stderr', io.StringIO()):
            self.assertEqual(L.main([str(self.root / 'missing.log')]), 2)
            self.assertEqual(L.main([str(self.root)]), 2)   # no *.log in it

    def test_user_script(self):
        p = self.write(start() + frames(1) + CRASH)
        r = subprocess.run([sys.executable, str(REPO / 'scripts/log_digest.py'), str(p)], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr); self.assertIn('-- verdict', r.stdout)


class Redaction(Base):
    def test_paths_and_keys(self):
        lines = start() + [f'title_key = {KEY}', f'master_key_00 : {KEY}{KEY}', f'[dm] [RED] something odd with {KEY}{KEY} in it',
                           '[dm] [RED] cannot read /Us' 'ers/somebody/Games/x and /home/other/y']
        d = self.digest(lines, tail=40)
        text = L.render(d) + json.dumps(d)
        self.assertNotIn(KEY, text)
        self.assertIsNone(re.search(r'[0-9a-fA-F]{32}', text))
        self.assertNotIn('/storage/emulated/0/Android/data', text)
        self.assertNotIn('somebody', text); self.assertNotIn('/home/other', text)
        self.assertEqual(d['key_lines_dropped'], 2)
        self.assertIn('<app files>', L.redact(f'{APP}/logs/x.log'))
        out = L.filter_log(L.resolve(self.write(lines)))
        self.assertNotIn(KEY, out); self.assertIn('(key-like) 2', out)


class Filter(Base):
    def healthy(self):
        return start() + frames(6) + ['[dm] something nobody has seen before'] + ['[dm] [RED] vk: odd thing 1'] * 3 + frames(4) + ['[dm] graceful exit done (0.1 s)']

    def test_filter(self):
        lines = self.healthy()
        rc, out = self.run_main(self.write(lines), '--filter')
        self.assertEqual(rc, 0)
        self.assertIn('L', out.split(':')[0])
        self.assertIn('[dm] something nobody has seen before', out)
        self.assertRegex(out, r'L\d+: \[dm\] \[RED\] vk: odd thing 1  \(×3, L\d+–\d+\)')
        self.assertIn('[dm] build: commit', out)                         # a stage milestone
        self.assertNotIn('[dm] [vk] fps:', out.split('-- filtered out')[0])
        self.assertNotIn('env DM_WATCH_STALL', out.split('-- filtered out')[0])
        foot = out.split('-- filtered out')[1]
        self.assertIn('fps (game, min / median / max of 10 readings): 30 / 58 / 60', foot)
        self.assertIn(f'input {len(lines)} lines', foot)
        self.assertRegex(foot, r'dropped by rule: .*longframe 9')       # the first long frame is the frame-loop milestone
        km = re.search(r'kept (\d+)', foot); assert km
        kept = int(km.group(1))
        self.assertLess(kept, len(lines) / 2)

    def test_context_and_cap(self):
        lines = start() + frames(4) + ['[dm] [RED] vk: the one'] + frames(4)
        p = self.write(lines)
        plain = L.filter_log(L.resolve(p))
        ctx = L.filter_log(L.resolve(p), context=2)
        self.assertGreater(ctx.count('\n'), plain.count('\n'))
        n = next(i for i, l in enumerate(lines, 1) if 'the one' in l)
        self.assertIn(f'L{n - 1}: ', ctx); self.assertIn(f'L{n + 2}: ', ctx); self.assertNotIn(f'L{n - 1}: ', plain)
        many = start() + [f'[dm] unknown line {chr(97 + i % 26)}{"x" * (i % 7)} {i % 3}' for i in range(500)]
        out = L.filter_log(L.resolve(self.write(many)), max_lines=40)
        body = out.split('-- filtered out')[0]
        self.assertIn('kept lines cut here (--max-lines 40)', body)
        self.assertLessEqual(body.count('\n'), 41)
        self.assertIn('L1: ', body); self.assertIn(f'L{len(many)}: ', body)


class Determinism(Base):
    def test_same_output(self):
        p = self.write(start() + frames(3) + CRASH)
        a = [self.run_main(p)[1], self.run_main(p, '--json')[1], self.run_main(p, '--filter')[1]]
        b = [self.run_main(p)[1], self.run_main(p, '--json')[1], self.run_main(p, '--filter')[1]]
        self.assertEqual(a, b)
        json.loads(a[1])


class Tables(unittest.TestCase):
    JAVA_BAD = re.compile(r'\(\?P|\(\?<[a-zA-Z]|\\Z|\\A(?!\w)|\(\?[a-z]*[msux][a-z]*[:)]|(?<!^)\(\?i\)')

    def check_table(self, rows, cols):
        ids = [r['id'] for r in rows]
        self.assertEqual(len(ids), len(set(ids)), 'ids are unique')
        for r in rows:
            self.assertTrue(re.fullmatch(r'[a-z0-9-]+', r['id']), r['id'])
            re.compile(L._java_safe(r['regex']))
            self.assertIsNone(self.JAVA_BAD.search(r['regex']), f"{r['id']}: keep the regex to what java.util.regex accepts the same way")
            self.assertNotIn('\t', r['regex'], r['id'])

    def test_rules(self):
        rows = L.load_rules()
        self.check_table(rows, ('id', 'kind', 'regex', 'cause', 'doc'))
        self.assertGreaterEqual(len(rows), 20)
        for r in rows:
            self.assertIn(r['kind'], L.KINDS, r['id'])
            self.assertTrue(r['cause'].endswith('.') and len(r['cause']) < 160, r['id'])
            if r['doc']:
                f, _, a = r['doc'].partition('#')
                self.assertTrue((REPO / f).is_file(), r['doc'])
                if a: self.assertIn(a, test_docs.anchors(REPO / f), r['doc'])

    def test_noise(self):
        rows = L.load_noise()
        self.check_table(rows, ('id', 'regex', 'note'))
        must_keep = ['[dm] [RED] vk: GPU hang: queue; fence 9 not reached', '[dm] CRASH sig=11 addr=0 pc=main+0x10 lr=main+0x14 sp=0x1', '[dm] FATAL open /x',
                     '[dm] [RED] watchdog: no progress since second 40', '[dm] [vk] [RED] fps: none', '[nxlaunch] [RED] env.txt: x exists but cannot be read']
        for line in must_keep:
            for r in rows: self.assertIsNone(r['rx'].search(line), f"{r['id']} matches {line!r}")

    def test_gates_never_hide_a_match(self):
        lines = start() + frames(2) + CRASH + ['[dm] [RED] vk: upload batch 3 not done in 6.0 s', "11-09 I lowmemorykiller: Kill 'dev.nxnative.android:game' (1)",
                                               '[dm] [vk] memory pool 0x1000+0x2000 placed when the GPU was given 0x1000+0x10', 'OUT OF MEMORY here', '[RED] save swap journal x names y',
                                               "[dm] [vk] shader library /p: this host's version abc is missing (the app's shader update makes it); playing version d",
                                               '[dm] main: undefined weak glslcInitialize bound to 0', 'x12=0x1 x13=0', '[dm]   0 1a 2b 3c', '[dm] [vk] compute pipeline 1a2b took 134 ms']
        for rows in (L.load_rules(), L.load_noise()):
            m = L.Matcher(rows)
            for line in lines:
                want = next((i for i, r in enumerate(rows) if r['rx'].search(line)), -1)
                self.assertEqual(m.search(line), want, line)

    def test_notice_sentences_are_the_launchers(self):
        java = (REPO / 'android/app/src/main/java/dev/nxnative/android/ExitNotice.java').read_text()
        for k, v in L.NOTICE.items():
            m = re.search(rf'\b{k} = "([^"]*)"', java)
            assert m, k
            self.assertEqual(m.group(1), v, k)


if __name__ == '__main__':
    unittest.main()
