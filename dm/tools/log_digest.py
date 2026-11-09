#!/usr/bin/env python3
"""A short, fixed-format digest of one Android run log, so an agent or a player sees what went wrong without reading the raw log (which can be many MB).

    python3 dm/tools/log_digest.py <run.log | folder | .zip> [--logcat FILE] [--exit FILE] [--json] [--tail N]
    python3 dm/tools/log_digest.py --filter <run.log | folder | .zip> [--keep REGEX] [--context N] [--max-lines N]

The input is a game log (files/logs/<title ID>-<time>.log), a folder (its newest *.log; a last-exit.txt there is used when it names that log) or a zip (the bug report's or a
folder of logs: text members are read in place, nothing is extracted). --logcat adds an `adb logcat` capture (only the game process's lines are used), --exit the launcher's
last-exit.txt (ExitWatch; `dumpsys activity exit-info` output works too: its newest ":game" entry gives Android's exit reason).

The digest: header (app, device, GPU and driver, game, build, switches), the furthest start-up stage (android/TESTING.md "What the log says at each stage"), one verdict (the
categories and sentences of the launcher's ExitNotice, so the tool and the "The game stopped unexpectedly" dialog agree), the crash block, the [RED] / FATAL lines grouped, the
entries of docs/agents/symptoms.md and dm/tools/log_rules.tsv the lines match, the performance lines summarised and the last lines. --json prints the same as one object.
--filter prints the log itself without the routine lines of dm/tools/log_noise.tsv, each kept line with its line number (L<n>: ...), runs of the same line collapsed.
Every printed line is redacted (app folders and the home folder shortened, key-like lines dropped, runs of 32 or more hex digits masked). The file is read line by line.
Exit code: 0 (also for a log with problems), 2 the input cannot be read."""
import argparse
import collections
import contextlib
import importlib.util
import io
import json
import os
import re
import statistics
import sys
import zipfile
from pathlib import Path
from typing import Any, Optional

REPO = Path(__file__).resolve().parents[2]
RULES_TSV = REPO / 'dm/tools/log_rules.tsv'
NOISE_TSV = REPO / 'dm/tools/log_noise.tsv'
SYMPTOMS_MD = REPO / 'docs/agents/symptoms.md'
CUT = 200                 # printed line length
MAX_GROUPS = 15           # [RED] / error groups shown
HEADER_LINES = 20000      # the header lines come in the first lines of a run
TRACK_GROUPS = 4000       # distinct groups kept in memory; later new ones are only counted
KINDS = ('crash', 'fatal', 'red', 'driver', 'oom', 'hang', 'perf', 'info')

# The launcher's sentences (android/app/src/main/java/dev/nxnative/android/ExitNotice.java; test_log_digest.py checks they are the same)
NOTICE = {
    'JAVA': 'The app ran into an error and had to close the game.',
    'NATIVE': 'The game ran into an error and crashed.',
    'FATAL': 'The game reached something the app cannot handle yet, so it was stopped.',
    'GPU': 'The GPU driver stopped responding, so the game had to close.',
    'MEMORY': 'Android closed it: the phone ran out of memory. Closing other apps before playing can help.',
    'MEMORY_BG': 'Android closed it while it was in the background: the phone ran out of memory. Unsaved progress is lost.',
    'KILLED': 'Android closed it without saying why. This is often for memory: closing other apps before playing can help.',
    'ANR_TEXT': 'Android closed it because it stopped responding.',
    'RESOURCES': "Android closed it for using too much of the phone's resources.",
    'HOST_ERROR': 'The game ended with an error.',
    'UNKNOWN_TEXT': 'It closed without saying why.',
}
# ApplicationExitInfo.REASON_* (as ExitNotice mirrors them)
R_EXIT_SELF, R_SIGNALED, R_LOW_MEMORY, R_CRASH, R_CRASH_NATIVE, R_ANR, R_RESOURCES = 1, 2, 3, 4, 5, 6, 9
R_USER = (10, 11, 15, 16)   # USER_REQUESTED, USER_STOPPED, PACKAGE_STATE_CHANGE, PACKAGE_UPDATED


# ---------------------------------------------------------------- tables

def _java_safe(rx):
    """A regex of the tables as Python compiles it: a leading (?i) is scoped to the pattern so patterns can be joined (Java accepts both forms)."""
    return f'(?i:{rx[4:]})' if rx.startswith('(?i)') else rx


def read_tsv(path, cols):
    """The rows of a rule table as dicts. The regexes must be valid in both Python re and Java java.util.regex, because the launcher ships the same tables: no named groups,
    no (?P...), no \\Z, no inline flags except a leading (?i) or a scoped (?i:...). Keep them simple: literal text and character classes."""
    rows, header = [], None
    for n, line in enumerate(Path(path).read_text(encoding='utf-8').split('\n'), 1):
        if not line or line.startswith('#'): continue
        f = line.split('\t')
        if header is None:
            header = f
            if header[:len(cols)] != list(cols): raise ValueError(f'{path}:{n}: header {header} is not {list(cols)}')
            continue
        f += [''] * (len(cols) - len(f))
        r: dict = dict(zip(cols, f)); r['line'] = n
        r['rx'] = re.compile(_java_safe(r['regex']))
        rows.append(r)
    return rows


def load_rules(path=RULES_TSV): return read_tsv(path, ('id', 'kind', 'regex', 'cause', 'doc'))
def load_noise(path=NOISE_TSV): return read_tsv(path, ('id', 'regex', 'note'))


def _skip_class(rx, k):
    """the index after the character class that starts at rx[k] == '['"""
    k += 1
    if k < len(rx) and rx[k] == '^': k += 1
    if k < len(rx) and rx[k] == ']': k += 1
    while k < len(rx) and rx[k] != ']': k += 2 if rx[k] == '\\' else 1
    return k + 1


def _top_alternatives(rx):
    alts, cur, depth, k = [], '', 0, 0
    while k < len(rx):
        ch = rx[k]
        if ch == '\\': cur += rx[k:k + 2]; k += 2; continue
        if ch == '[': e = _skip_class(rx, k); cur += rx[k:e]; k = e; continue
        if ch == '(': depth += 1
        elif ch == ')': depth -= 1
        if ch == '|' and depth == 0: alts.append(cur); cur = ''
        else: cur += ch
        k += 1
    alts.append(cur)
    return alts


def _literal_runs(alt):
    """The literal runs of one alternative: an escaped punctuation character is a literal; classes, groups, wildcards, anchors, \\d-style escapes and a character
    made optional by ? * {} break a run"""
    runs, cur, k = [], '', 0
    while k < len(alt):
        ch = alt[k]
        if ch == '\\' and k + 1 < len(alt):
            nx = alt[k + 1]; k += 2
            if nx.isalnum():
                if cur: runs.append(cur)
                cur = ''
            else: cur += nx
            continue
        brk = True
        if ch == '[': k = _skip_class(alt, k)
        elif ch == '(':
            d = 1; k += 1
            while k < len(alt) and d:
                if alt[k] == '\\': k += 2; continue
                if alt[k] == '[': k = _skip_class(alt, k); continue
                d += {'(': 1, ')': -1}.get(alt[k], 0); k += 1
        elif ch in '?*{':
            cur = cur[:-1]
            if ch == '{':
                while k < len(alt) and alt[k] != '}': k += 1
            k += 1
        elif ch in '+.^$': k += 1
        else: cur += ch; k += 1; brk = False
        if brk:
            if cur: runs.append(cur)
            cur = ''
    if cur: runs.append(cur)
    return runs


def gate(rx):
    """(case-insensitive, literals): a line must hold one of the literals for rx to match (each top-level alternative's longest literal run); None: no gate"""
    ci = rx.startswith('(?i)')
    out = []
    for a in _top_alternatives(rx[4:] if ci else rx):
        best = max(_literal_runs(a), key=len, default='')
        if len(best) < 3: return None
        out.append(best.lower() if ci else best)
    return (ci, tuple(out))


class Matcher:
    """The rows of a table searched one by one, each behind its literal gate (much faster in Python than one joined alternation)"""
    def __init__(self, rows):
        self.rows = [(i, r['rx'], gate(r['regex'])) for i, r in enumerate(rows)]

    def search(self, line):
        """the index of the first row that matches, or -1"""
        low = None
        for i, rx, g in self.rows:
            if g is not None:
                if g[0]:
                    if low is None: low = line.lower()
                    if not any(x in low for x in g[1]): continue
                elif not any(x in line for x in g[1]): continue
            if rx.search(line): return i
        return -1


# ---------------------------------------------------------------- redaction and normalisation

def _bug_report():
    spec = importlib.util.spec_from_file_location('bug_report', REPO / 'scripts/bug_report.py')
    assert spec and spec.loader
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
    return m


_BR = _bug_report()
KEY_LINE, KEY_LINE2 = _BR.KEY_LINE, _BR.KEY_LINE2     # scripts/bug_report.py's key-material patterns
HEX_RUN = re.compile(r'[0-9a-fA-F]{32,}')
HEX_LONG = re.compile(r'[0-9a-fA-F]{32}')
PATHS = [
    (re.compile(r'/(?:storage/emulated/\d+|sdcard)/Android/data/[\w.]+/files'), '<app files>'),
    (re.compile(r'/data/(?:user/\d+|data)/[\w.]+/files'), '<app internal>'),
    (re.compile(r'/data/(?:user/\d+|data)/[\w.]+/cache'), '<app cache>'),
    (re.compile(r'/data/app/[^\s/]*/[\w.]+-[^\s/]+/'), '<apk>/'),
    (re.compile(r'/(?:storage/emulated/\d+|sdcard)/'), '<sdcard>/'),
    (re.compile(r'/storage/[0-9A-F]{4}-[0-9A-F]{4}/'), '<sd card>/'),
    (re.compile(r'/(?:Users|home)/[^\s/]+'), '~'),
]
HOME = str(Path.home())


def key_like(line): return bool(HEX_LONG.search(line) and (KEY_LINE.search(line) or KEY_LINE2.search(line)))


def redact(s, cut=CUT):
    """A line fit to print: folders shortened, long hex masked, cut to `cut` characters (key-like lines are dropped before this)"""
    if HOME and HOME != '/' and HOME in s: s = s.replace(HOME, '~')
    if '/' in s:
        for rx, rep in PATHS: s = rx.sub(rep, s)
    s = HEX_RUN.sub('<hex>', s.rstrip('\r\n'))
    return s if len(s) <= cut else s[:cut - 1] + '…'


NORM = [
    (re.compile(r'(?:/[\w.~@+%=,-]+)+/?'), '<path>'),
    (re.compile(r'\b0x[0-9a-fA-F]+\b'), '#'),
    (re.compile(r'\b(?=[0-9a-fA-F]*\d)[0-9a-fA-F]{6,}\b'), '#'),
    (re.compile(r'\d+(?:\.\d+)?'), '#'),
    (re.compile(r'\s+'), ' '),
]


def normalise(s):
    """A line's shape: paths, hex, numbers and spacing made equal (for grouping, collapsing and matching the symptom index)"""
    for rx, rep in NORM: s = rx.sub(rep, s)
    return s.strip()


# ---------------------------------------------------------------- the symptom index

SYM_LINE = re.compile(r'^- (.*) → \[([^\]]+)\]\(\.\./reference/([^)#]+)#([^)]+)\)\s*$')
GAP = re.compile(r'…|\.\.\.|<[^>]*>')


def load_symptoms(path=SYMPTOMS_MD):
    """[(pieces, heading, doc)]: the backticked strings of each bullet, normalised and split at their placeholders (…, <...>); short or generic strings are left out (precision)"""
    out = []
    try: text = Path(path).read_text(encoding='utf-8')
    except OSError: return out
    for line in text.split('\n'):
        m = SYM_LINE.match(line)
        if not m: continue
        for frag in re.findall(r'`([^`]+)`', m.group(1)):
            pieces = [p for p in (normalise(x).strip(' :;,.') for x in GAP.split(frag)) if len(p) >= 6]
            if not pieces or sum(map(len, pieces)) < 24 or max(map(len, pieces)) < 16: continue
            if sum(c.isalpha() for c in ''.join(pieces)) < 18: continue
            out.append((pieces, m.group(2), f'docs/reference/{m.group(3)}#{m.group(4)}'))
    # a string that several entries quote, alone or inside a longer one (an abort line, a common [RED] line), says nothing about which one applies: left out
    texts = [(' '.join(p), doc) for p, _h, doc in out]
    def entries(t): return {doc for u, doc in texts if t in u}
    return [x for x in out if len(entries(' '.join(x[0]))) == 1]


def piece_match(pieces, norm):
    at = 0
    for p in pieces:
        i = norm.find(p, at)
        if i < 0: return False
        at = i + len(p)
    return True


# ---------------------------------------------------------------- inputs

LOG_NAME = re.compile(r'(?:^|/)([0-9A-Fa-f]{16})-(\d{8}-\d{6})\.log$')


def _text_stream(binary):
    return io.TextIOWrapper(binary, encoding='utf-8', errors='replace', newline='')


@contextlib.contextmanager
def _zip_member(path, name):
    """a zip member's text, read in place (the archive is opened for the read and closed after it)"""
    with zipfile.ZipFile(path) as z, z.open(name) as b:
        yield _text_stream(b)


class Inputs:
    """The run log (a function that opens it for reading), the logcat and the exit record's text; `name` for the header"""
    def __init__(self):
        self.open_log: Any = None
        self.open_logcat: Any = None
        self.exit_text: Optional[str] = None
        self.name, self.size = '', 0


def _pick_log(names):
    named = sorted((m.group(2), n) for n in names if (m := LOG_NAME.search(n)))
    return named[-1][1] if named else (sorted(names)[-1] if names else None)


def resolve(path, logcat=None, exit_file=None):
    """Inputs for a log file, a folder or a zip; raises OSError / zipfile.BadZipFile when it cannot be read"""
    p = Path(path)
    inp = Inputs()
    if p.is_dir():
        logs = [q.name for q in p.iterdir() if q.suffix == '.log' and q.is_file()]
        pick = _pick_log(logs)
        if not pick: raise OSError(f'no *.log in {p}')
        lp = p / pick
        inp.open_log, inp.name, inp.size = (lambda: open(lp, encoding='utf-8', errors='replace', newline='')), pick, lp.stat().st_size
        rec = p / 'last-exit.txt'
        if exit_file is None and rec.is_file():
            t = rec.read_text(errors='replace')
            if parse_record(t).get('log') == pick: inp.exit_text = t
    elif zipfile.is_zipfile(p):
        with zipfile.ZipFile(p) as z: members = [i for i in z.infolist() if not i.is_dir()]; size = {i.filename: i.file_size for i in members}
        texts = [i.filename for i in members if i.filename.lower().endswith(('.log', '.txt'))]
        logs = [n for n in texts if n.endswith('.log') and 'logcat' not in n.lower()]
        pick = _pick_log(logs)
        if not pick: raise OSError(f'no *.log in {p}')
        inp.open_log, inp.name, inp.size = (lambda: _zip_member(p, pick)), f'{p.name}:{pick}', size[pick]
        cats = sorted(n for n in texts if 'logcat' in n.lower())
        if logcat is None and cats: inp.open_logcat = lambda: _zip_member(p, cats[-1])
        recs = sorted(n for n in texts if n.rsplit('/', 1)[-1] == 'last-exit.txt')
        if exit_file is None and recs:
            with zipfile.ZipFile(p) as z: inp.exit_text = z.read(recs[-1]).decode('utf-8', 'replace')
    else:
        with open(p, 'rb'): pass
        inp.open_log, inp.name, inp.size = (lambda: open(p, encoding='utf-8', errors='replace', newline='')), p.name, p.stat().st_size
    if logcat is not None:
        lc = Path(logcat)
        with open(lc, 'rb'): pass
        inp.open_logcat = lambda: open(lc, encoding='utf-8', errors='replace', newline='')
    if exit_file is not None: inp.exit_text = Path(exit_file).read_text(errors='replace')
    return inp


def parse_record(text):
    """last-exit.txt: key=value lines, the last of a key wins (ExitNotice.parse). A `dumpsys activity exit-info` text gives its newest ':game' entry's reason, status, description"""
    m = {}
    if not text: return m
    if 'ApplicationExitInfo #' in text:
        for block in re.split(r'ApplicationExitInfo #\d+:', text)[1:]:
            if not re.search(r'process=\S+:game\b', block): continue
            for k, rx in (('reason', r'\breason=(\d+)'), ('status', r'\bstatus=(-?\d+)'), ('description', r'\bdescription=(.*)'), ('importance', r'\bimportance=(\d+)')):
                mm = re.search(rx, block)
                if mm: m['android_' + k] = mm.group(1).strip()
            break
        return m
    for line in text.split('\n'):
        k, eq, v = line.partition('=')
        if eq and k.strip(): m[k.strip()] = v.strip()
    return m


# ---------------------------------------------------------------- the stages (android/TESTING.md)

STAGES = [
    (1, 'launcher', re.compile(r'^\[nxlaunch\] (env |arg |hooks: )')),
    (2, 'host started', re.compile(r'^\[dm\] build: commit ')),
    (3, 'thread slots', re.compile(r'^\[dm\] thread slots: ')),
    (4, 'loader done', re.compile(r'^\[dm\] (stubs=|sdk: NintendoSDK )')),
    (5, 'first guest code', re.compile(r'^\[dm\] (>>> nnosInitialize|guest AS )')),
    (6, 'SDK start-up (nnMain)', re.compile(r'^\[dm\] >>> nnMain')),
    (7, 'first NVN call', re.compile(r'^\[dm\] \[nvn\] device API ')),
    (8, 'frame loop', re.compile(r'^\[dm\] (\[nvn\] (frame \d+:|frame interval up to|timing up to)|\[vk\] fps: |\[longframe\] frame )')),
    (9, 'marked frame (DM_MARK_DRAWS)', re.compile(r'^\[dm\] \[mark\] frame ')),
]
STAGE_ANY = re.compile('|'.join(f'(?:{s[2].pattern})' for s in STAGES))
TIME_RX = re.compile(r'^\[dm\] (?:\[longframe\] frame \d+ \(([\d.]+) s\)|\[mark\] .* ([\d.]+) s from process start)')

ERROR_RX = re.compile(r'\[RED\]|\bFATAL |CRASH sig=|terminating due to|Abort message|Fatal signal')
EXPECTED_RX = re.compile(r'not yet on (?:Android|Vulkan)')
PROTECT_RX = re.compile(r'\[RED\]|CRASH|FATAL|(?i:abort|\bsignal\b|\bwarn\w*|\berror\w*|\bfail\w*|exception)')
PROTECT_WORDS = ('[red]', 'crash', 'fatal', 'abort', 'signal', 'warn', 'error', 'fail', 'exception')


ZERO_RX = re.compile(r'\b0 (?:failed|failures?|errors?|warnings?)\b')


def protected(line):
    """a [RED], crash, FATAL, abort, signal, warning or error line (\"0 failed\" and the like do not count)"""
    low = line.lower()
    if not any(w in low for w in PROTECT_WORDS): return False
    if '0 ' in line: line = ZERO_RX.sub('', line)
    return bool(PROTECT_RX.search(line))


def error_line(line):
    return ('[RED]' in line or 'FATAL' in line or 'CRASH' in line or 'terminating' in line or 'Abort' in line or 'Fatal' in line) and bool(ERROR_RX.search(line))


# ---------------------------------------------------------------- performance lines

FPS_RX = re.compile(r'^\[dm\] \[vk\] fps: ([\d.]+) game frames a second \(([\d.]+) ms\), ([\d.]+) presented')
TIMING_RX = re.compile(r'^\[dm\] \[nvn\] timing up to frame \d+: frame time p50 ([\d.]+) / p95 ([\d.]+) / max ([\d.]+)')
ONSCREEN_RX = re.compile(r'^\[dm\] \[nvn\] on-screen up to frame \d+: (\d+) frames, dropped (\d+).*?footprint (\d+) MiB.*?thermal (-?\d+)')
LONG_RX = re.compile(r'^\[dm\] \[longframe\] frame (\d+) \(([\d.]+) s\) whole frame ([\d.]+) ms')
VKMEM_RX = re.compile(r'^\[dm\] \[vk\] memory: placed pools .*: (\d+) MiB$')
THROTTLE_RX = re.compile(r'\[overlay\] throttle (\w+): thermal status (-?\d+)')


class Perf:
    def __init__(self):
        self.fps, self.presented, self.p50, self.p95, self.fmax = [], [], [], [], []
        self.frames = self.dropped = 0
        self.footprint, self.thermal, self.vkmem = [], [], []
        self.long_n, self.long_worst = 0, None
        self.throttle = collections.Counter()
        self.life_n, self.life = 0, []

    def feed(self, n, line):
        """True when the line is a performance line"""
        if line.startswith('[dm] [longframe]'):
            m = LONG_RX.match(line)
            if m:
                self.long_n += 1; ms = float(m.group(3))
                if self.long_worst is None or ms > self.long_worst[0]: self.long_worst = (ms, int(m.group(1)), float(m.group(2)), n)
            return True
        if line.startswith('[dm] [vk] fps:'):
            m = FPS_RX.match(line)
            if m: self.fps.append(float(m.group(1))); self.presented.append(float(m.group(3)))
            return True
        if line.startswith('[dm] [nvn] timing up to'):
            m = TIMING_RX.match(line)
            if m: self.p50.append(float(m.group(1))); self.p95.append(float(m.group(2))); self.fmax.append(float(m.group(3)))
            return True
        if line.startswith('[dm] [nvn] on-screen up to'):
            m = ONSCREEN_RX.match(line)
            if m:
                self.frames += int(m.group(1)); self.dropped += int(m.group(2)); self.footprint.append(int(m.group(3)))
                if int(m.group(4)) >= 0: self.thermal.append(int(m.group(4)))
            return True
        if line.startswith('[dm] [vk] memory: placed pools'):
            m = VKMEM_RX.match(line)
            if m: self.vkmem.append(int(m.group(1)))
            return True
        if '[overlay] throttle' in line:
            m = THROTTLE_RX.search(line)
            if m: self.throttle[m.group(1)] += 1; self.thermal.append(int(m.group(2)))
            return True
        if '[lifecycle]' in line:
            self.life_n += 1
            if len(self.life) < 3: self.life.append((n, line))
            else: self.life[2] = (n, line)
            return True
        return False

    def summary(self):
        def mmm(v, f='%.1f'):
            return None if not v else {'min': float(f % min(v)), 'median': float(f % statistics.median(v)), 'max': float(f % max(v)), 'n': len(v)}
        d = {'fps_game': mmm(self.fps), 'fps_presented': mmm(self.presented), 'frame_p50_ms': mmm(self.p50, '%.2f'), 'frame_p95_ms': mmm(self.p95, '%.2f'),
             'frame_max_ms': mmm(self.fmax, '%.2f'),
             'on_screen': {'frames': self.frames, 'dropped': self.dropped} if self.frames else None,
             'footprint_mib': mmm(self.footprint, '%d'), 'vk_memory_mib': mmm(self.vkmem, '%d'), 'thermal_status': mmm(self.thermal, '%d'),
             'longframes': {'count': self.long_n, 'worst_ms': self.long_worst[0], 'frame': self.long_worst[1], 'at_s': self.long_worst[2], 'line': self.long_worst[3]}
             if self.long_worst else None,
             'throttle': dict(sorted(self.throttle.items())) or None,
             'lifecycle': {'count': self.life_n, 'lines': [{'line': n, 'text': redact(t, 160)} for n, t in self.life]} if self.life_n else None}
        return {k: v for k, v in d.items() if v is not None}


def perf_lines(p):
    out = []
    def r(d): return f"{d['min']:g} / {d['median']:g} / {d['max']:g}"
    if 'fps_game' in p: out.append(f"fps (game, min / median / max of {p['fps_game']['n']} readings): {r(p['fps_game'])}; presented {r(p['fps_presented'])}")
    if 'frame_p50_ms' in p: out.append(f"frame time p50 {r(p['frame_p50_ms'])} ms, p95 {r(p['frame_p95_ms'])} ms, worst {p['frame_max_ms']['max']:g} ms ({p['frame_p50_ms']['n']} windows of 300 frames)")
    if 'on_screen' in p: out.append(f"on screen: {p['on_screen']['frames']} frames counted, {p['on_screen']['dropped']} dropped")
    if 'footprint_mib' in p: out.append(f"memory footprint {r(p['footprint_mib'])} MiB" + (f"; GPU memory {r(p['vk_memory_mib'])} MiB" if 'vk_memory_mib' in p else ''))
    elif 'vk_memory_mib' in p: out.append(f"GPU memory {r(p['vk_memory_mib'])} MiB")
    if 'thermal_status' in p: out.append(f"thermal status {r(p['thermal_status'])} (0 none … 4 critical)")
    if 'throttle' in p: out.append('overlay throttle: ' + ', '.join(f'{k} ×{v}' for k, v in p['throttle'].items()))
    if 'longframes' in p:
        lf = p['longframes']; out.append(f"long frames: {lf['count']}, worst {lf['worst_ms']:g} ms (frame {lf['frame']}, {lf['at_s']:g} s, L{lf['line']})")
    if 'lifecycle' in p:
        lc = p['lifecycle']; out.append(f"[lifecycle] lines: {lc['count']}")
        out += [f"  L{x['line']}: {x['text']}" for x in lc['lines']]
    return out


# ---------------------------------------------------------------- the digest

ENV_RX = re.compile(r'^\[nxlaunch\] env ([A-Z_][A-Z0-9_]*)=(.*)$')
PATH_KEYS = {'NX_EXIT_RECORD', 'DM_ROMFS', 'DM_SAVE', 'DM_TEMP', 'DM_ROOT', 'DM_AOC_DIR', 'HOME', 'TMPDIR', 'DM_CTL', 'NX_BUILD_TAG', 'DM_BUILD_COMMIT', 'DM_LSFG_DLL', 'DM_WINDOW_SIZE'}


def switches(env):
    """The [nxlaunch] env lines that are switches (not the launcher's paths), each value cut to 40 characters; a custom driver by its folder name"""
    out = []
    for k, v in env.items():
        if k in PATH_KEYS: continue
        if k == 'DM_VK_DRIVER': v = os.path.basename(v.rstrip('/'))
        out.append(f'{k}={v if len(v) <= 40 else v[:39] + "…"}')
    return out


HDR = [
    ('app_build', re.compile(r'^\[nxlaunch\] build (.+)$')),
    ('app_version', re.compile(r'^\[nxlaunch\] nxnative (\S+ \([0-9a-f]+\))')),
    ('game', re.compile(r'^\[nxlaunch\] nxnative .*?; (.+ [0-9A-F]{16} version \d+)')),
    ('device', re.compile(r'^\[nxlaunch\] device ([^;]+)')),
    ('host_build', re.compile(r'^\[dm\] build: (commit .+)$')),
    ('readiness', re.compile(r'^\[nxlaunch\] (?:\[nxlaunch\] )?\[readiness\] [0-9A-F]{16}: (.+)$')),
    ('gpu_driver_picked', re.compile(r'^\[nxlaunch\] GPU driver: (.+?) \(picked in the launcher')),
    ('vk_driver', re.compile(r'^\[dm\] \[vk\] driver: (.+)$')),
    ('vk_custom_driver', re.compile(r'^\[dm\] \[vk\] custom GPU driver: (.+)$')),
    ('shader_library', re.compile(r'^\[dm\] \[vk\] shader library \S+: (\d+ shaders, .+)$')),
    ('sdk', re.compile(r'^\[dm\] sdk: (NintendoSDK \S+)')),
]
BUILD_ID_RX = re.compile(r'^\[dm\] (?:\[RED\] )?cheats: .*(?:no file for build|not main\'s) ([0-9A-F]{16})')
DRIVER_ID_RX = re.compile(r'driver ID (\d+)')
APP_ID_RX = re.compile(r'/Android/data/([\w.]+)/files')
CRASH_RX = re.compile(r'^\[dm\] CRASH sig=(\d+)(.*)$')
GAME_MOD = re.compile(r'^(main|sdk|subsdk\d*|rtld)\+0x[0-9a-f]+$')
JAVA_CRASH_RX = re.compile(r'\[RED\] crash: uncaught exception on thread (.+?), the game process ends')

# logcat
LC_RX = re.compile(r'^(?:\d\d-\d\d \d\d:\d\d:\d\d\.\d+\s+)?(?:(\d+)\s+(\d+)\s+([VDIWEFA])\s+([^:]*?)\s*: |([VDIWEFA])/([^(]*?)\(\s*(\d+)\): )(.*)$')
OURS = re.compile(r':game\b|dev\.nxnative\.android|dev\.bananative\.android|\[nxlaunch\]|\[dm\]')


class Digest:
    def __init__(self, rules, symptoms, tail=15, noise=None):
        self.rules, self.symptoms = rules, symptoms
        self.noise_rx = Matcher(noise if noise is not None else load_noise())
        self.rules_rx = Matcher(rules)
        self.n = 0
        self.tail = collections.deque(maxlen=max(tail, 0))
        self.before = collections.deque(maxlen=12)
        self.hdr, self.env, self.build_id, self.app_id = {}, {}, None, None
        self.stage = {}   # stage -> first line
        self.last_time = None
        self.groups, self.group_order, self.untracked = {}, [], 0
        self.expected = collections.Counter(); self.expected_n = 0
        self.rule_hits = {}   # rule index -> [count, first line, last line, example, source]
        self.crash, self.crash_n, self.crash_wait = None, 0, 0
        self.java = None
        self.dropped_keys = 0
        self.flags = collections.Counter()
        self.perf = Perf()
        self.tomb = None

    # -- the run log
    def feed(self, line):
        self.n += 1; n = self.n
        line = line.rstrip('\r\n')
        if key_like(line): self.dropped_keys += 1; return
        self.tail.append((n, line))
        if self.crash_wait: self._crash_more(n, line)
        if line.startswith('[nxlaunch] env '):
            m = ENV_RX.match(line)
            if m:
                self.env.pop(m.group(1), None); self.env[m.group(1)] = m.group(2)
        if n <= HEADER_LINES and len(self.hdr) < len(HDR) and line.startswith(('[nxlaunch] ', '[dm] ')):
            for k, rx in HDR:
                if k not in self.hdr:
                    m = rx.match(line)
                    if m: self.hdr[k] = m.group(1)
        if self.build_id is None and 'cheats: ' in line:
            m = BUILD_ID_RX.match(line)
            if m: self.build_id = m.group(1)
        if self.app_id is None and '/Android/data/' in line:
            m = APP_ID_RX.search(line)
            if m: self.app_id = m.group(1)
        if STAGE_ANY.match(line):
            for i, _name, rx in STAGES:
                if i not in self.stage and rx.match(line): self.stage[i] = (n, self.last_time)
        if line.startswith(('[dm] [longframe]', '[dm] [mark]')):
            m = TIME_RX.match(line)
            if m: self.last_time = float(m.group(1) or m.group(2))
        self.perf.feed(n, line)
        if 'graceful exit' in line: self.flags['graceful'] += 1
        i = self.rules_rx.search(line)
        if i >= 0: self._hit(i, n, line, 'log')
        if error_line(line): self._error(n, line)
        if line.startswith('[dm] CRASH sig='): self._crash(n, line)
        elif 'uncaught exception on thread' in line and self.java is None:
            m = JAVA_CRASH_RX.search(line)
            if m: self.java = {'line': n, 'thread': m.group(1), 'trace': []}
        elif self.java is not None and len(self.java['trace']) < 8 and '[RED] crash: ' in line and n > self.java['line']:
            self.java['trace'].append(redact(line.split('[RED] crash: ', 1)[1]))
        self.before.append((n, line))

    def _hit(self, i, n, line, src):
        h = self.rule_hits.get(i)
        if h is None: self.rule_hits[i] = [1, n, n, line, src]
        else: h[0] += 1; h[2] = n

    def _error(self, n, line):
        if EXPECTED_RX.search(line):
            self.expected_n += 1
            if len(self.expected) < TRACK_GROUPS: self.expected[normalise(line)[:160]] += 1
            return
        key = normalise(line)[:400]
        g = self.groups.get(key)
        if g is None:
            if len(self.groups) >= TRACK_GROUPS: self.untracked += 1; return
            self.groups[key] = [1, n, n, line]; self.group_order.append(key)
        else: g[0] += 1; g[2] = n

    def _crash(self, n, line):
        self.crash_n += 1
        if self.crash is not None: return
        m = CRASH_RX.match(line)
        rest = m.group(2) if m else ''
        f = dict(re.findall(r'\b(addr|pc|lr|sp)=(\S+)', rest))
        self.crash = {'line': n, 'text': line, 'sig': int(m.group(1)) if m else None, 'pc': f.get('pc'), 'lr': f.get('lr'), 'addr': f.get('addr'),
                      'before': [(bn, bl) for bn, bl in self.before if self.noise_rx.search(bl) < 0], 'bt': [], 'stack_scan': []}
        self.crash_wait = 80

    def _crash_more(self, n, line):
        self.crash_wait -= 1
        c = self.crash
        if c is None: self.crash_wait = 0; return
        if line.startswith('[dm] bt:') and not c['bt']: c['bt'] = line[8:].split()[:8]
        elif line.startswith('[dm] stack scan') and not c['stack_scan']: c['stack_scan'] = re.findall(r'\[[+-]\d+\](\S+)', line)[:6]
        if c['bt'] and c['stack_scan']: self.crash_wait = 0

    # -- the logcat: the game process's lines only
    def feed_logcat(self, lines):
        tomb, keep = None, None
        for raw in lines:
            line = raw.rstrip('\r\n')
            if key_like(line): continue
            m = LC_RX.match(line)
            if not m: continue
            lvl, tag, msg = (m.group(3), m.group(4), m.group(8)) if m.group(3) else (m.group(5), m.group(6), m.group(8))
            tag = tag.strip()
            if tag == 'DEBUG' and lvl == 'F':
                if msg.startswith('*** *** ***'):
                    if tomb is not None and keep: self.tomb = tomb
                    tomb, keep = {'signal': None, 'abort': None, 'thread': None, 'frames': [], 'cmdline': None}, False
                    continue
                if tomb is None: continue
                if msg.startswith('Cmdline: '): tomb['cmdline'] = msg[9:]; keep = keep or ':game' in msg
                elif msg.startswith('pid: '):
                    mm = re.search(r'name: (.*?)\s+>>> (.*?) <<<', msg)
                    if mm: tomb['thread'] = mm.group(1); keep = keep or ':game' in mm.group(2)
                elif msg.startswith('signal ') and tomb['signal'] is None: tomb['signal'] = msg
                elif msg.startswith('Abort message: '): tomb['abort'] = msg[15:]
                elif re.match(r'\s*#\d+ pc ', msg) and len(tomb['frames']) < 8: tomb['frames'].append(re.sub(r'\s*\(BuildId: [0-9a-f]+\)', '', msg.strip()))
                if keep:
                    r = self.rules_rx.search(msg)
                    if r >= 0: self._hit(r, 0, msg, 'logcat')
                continue
            if not OURS.search(line): continue
            r = self.rules_rx.search(line)
            if r >= 0: self._hit(r, 0, msg, 'logcat')
            if lvl == 'F' and tag == 'libc' and ':game' in msg: self.flags['logcat_signal'] += 1
            if 'lowmemorykiller' in tag.lower() or 'am_kill' in tag or 'lowmemorykiller' in msg:
                if ':game' in msg: self.flags['logcat_lmk'] += 1
        if tomb is not None and keep: self.tomb = tomb

    # -- results
    def _rule(self, i):
        r = self.rules[i]; h = self.rule_hits[i]
        return {'id': r['id'], 'kind': r['kind'], 'cause': r['cause'], 'doc': r['doc'], 'count': h[0], 'first_line': h[1], 'last_line': h[2],
                'source': h[4], 'example': redact(h[3])}

    def first_rule(self, kinds):
        for i, r in enumerate(self.rules):
            if r['kind'] in kinds and i in self.rule_hits: return self._rule(i)
        return None

    def verdict(self, rec):
        """ExitNotice.cause's order, with what the log, the logcat and the record give"""
        st = rec.get('state')
        try: reason = int(rec.get('android_reason', -1))
        except ValueError: reason = -1
        try: status = int(rec.get('android_status', 0))
        except ValueError: status = 0
        desc = rec.get('android_description', '').lower()
        bg = int(rec.get('android_importance', '100') or 100) > 200 if rec.get('android_importance', '').isdigit() else False
        gpu = self.first_rule(('driver',))
        oom = self.first_rule(('oom',))
        lmk = self.flags['logcat_lmk'] > 0 or reason == R_LOW_MEMORY or any(w in desc for w in ('lmk', 'low memory', 'lowmemory', 'oom'))
        crash = self.crash is not None or st == 'native' or reason == R_CRASH_NATIVE or self.flags['logcat_signal'] > 0 or self.tomb is not None
        fatal = self.first_rule(('fatal',))
        hang = self.first_rule(('hang',))
        v = None
        if st == 'stopped' or reason in R_USER: v = ('stopped', 'stopped by the player', None, None)
        elif gpu: v = ('driver_lost', 'driver lost', NOTICE['GPU'], gpu)
        elif st == 'java' or reason == R_CRASH or self.java: v = ('java_crash', 'crashed (the app)', NOTICE['JAVA'], self.first_rule(('crash',)))
        elif crash and hang and self.crash and self.crash['sig'] == 14: v = ('hang', 'hang (watchdog)', hang['cause'], hang)
        elif crash:
            if oom: v = ('out_of_memory', 'out of memory', NOTICE['MEMORY'], oom)
            elif fatal: v = ('host_fatal', 'host FATAL', NOTICE['FATAL'], fatal)
            else: v = ('crashed', 'crashed', NOTICE['NATIVE'], self.first_rule(('crash',)))
        elif hang and hang['id'] == 'watchdog': v = ('hang', 'hang (watchdog)', hang['cause'], hang)
        elif st == 'host_exit':
            v = ('normal_exit', 'ran and exited normally', None, None) if rec.get('rc', '0') == '0' else ('host_error', 'host error', NOTICE['HOST_ERROR'], fatal)
        elif lmk: v = ('out_of_memory', 'out of memory', NOTICE['MEMORY_BG'] if bg else NOTICE['MEMORY'], oom)
        elif reason == R_ANR: v = ('anr', 'not responding', NOTICE['ANR_TEXT'], None)
        elif reason == R_RESOURCES: v = ('killed', 'killed by Android', NOTICE['RESOURCES'], None)
        elif reason == R_SIGNALED: v = ('killed', 'killed by Android', NOTICE['MEMORY_BG'] if bg and status == 9 else NOTICE['KILLED'] if status == 9 else NOTICE['NATIVE'], None)
        elif fatal: v = ('host_fatal', 'host FATAL', NOTICE['FATAL'], fatal)
        elif self.flags['graceful']: v = ('normal_exit', 'ran and exited normally', None, None)
        elif reason == R_EXIT_SELF and status != 0: v = ('host_error', 'host error', NOTICE['HOST_ERROR'], None)
        else: v = ('cut_short', 'log cut short', NOTICE['UNKNOWN_TEXT'] + ' The log ends without an exit line (stopped from outside, or the log was cut).', None)
        kind, label, sentence, rule = v
        last = max(self.stage) if self.stage else None
        out = {'kind': kind, 'label': label, 'sentence': sentence, 'rule': rule['id'] if rule else None, 'cause': rule['cause'] if rule else None,
               'doc': rule['doc'] if rule else None, 'line': rule['first_line'] if rule and rule['first_line'] else None}
        if kind == 'cut_short' and last is not None and last < 8: out['sentence'] += f' It stopped before stage {last + 1}.'
        return out

    def symptom_matches(self):
        texts = [(g[1], k) for k, g in ((k, self.groups[k]) for k in self.group_order)]
        if self.crash: texts += [(n, normalise(t)) for n, t in self.crash['before'] + [(self.crash['line'], self.crash['text'])] if self.noise_rx.search(t) < 0]
        if self.tomb and self.tomb['abort']: texts.append((0, normalise(self.tomb['abort'])))
        seen, out = set(), []
        for pieces, head, doc in self.symptoms:
            if doc in seen: continue
            for n, t in texts:
                if piece_match(pieces, t):
                    seen.add(doc); out.append({'heading': head, 'doc': doc, 'line': n or None}); break
        out.sort(key=lambda d: (d['line'] or 0, d['doc']))
        return out[:8]

    def result(self, name='', size=0, rec=None):
        rec = rec or {}
        h = self.hdr
        dv = os.path.basename(self.env.get('DM_VK_DRIVER', '').rstrip('/'))
        picked = h.get('gpu_driver_picked')
        drv = f'custom (DM_VK_DRIVER {dv})' if dv and dv != 'system' else 'system' if dv == 'system' or picked == 'system' else f'custom ({picked})' if picked else None
        rd = h.get('readiness', '')
        did = DRIVER_ID_RX.search(h.get('vk_driver') or rd)
        header = {'app_build': h.get('app_build'), 'app_version': h.get('app_version'), 'app_id': self.app_id, 'device': h.get('device'),
                  'gpu': rd.split('; driver ')[0].split('; ', 1)[-1] if rd else None, 'driver': (h.get('vk_driver') or (rd.split('; driver ', 1)[1].split(', driver ID')[0] if '; driver ' in rd else None)),
                  'driver_kind': drv, 'driver_id': int(did.group(1)) if did else None, 'readiness': rd.split(';')[0] if rd else None,
                  'game': h.get('game'), 'game_build_id': self.build_id, 'host_build': h.get('host_build'), 'sdk': h.get('sdk'), 'shader_library': h.get('shader_library'),
                  'switches': switches(self.env)}
        header = {k: (redact(v) if isinstance(v, str) else [redact(x) for x in v] if isinstance(v, list) else v) for k, v in header.items()}
        header = {k: v for k, v in header.items() if v not in (None, [], '')}
        last = max(self.stage) if self.stage else None
        stage = None
        if last is not None:
            ln, t = self.stage[last]
            stage = {'number': last, 'name': dict((i, nm) for i, nm, _ in STAGES)[last], 'line': ln, 'seconds': t,
                     'missing': [nm for i, nm, _ in STAGES if i < last and i not in self.stage and i != 9]}
        crash = None
        if self.crash:
            c = self.crash
            bt = c['bt']
            game = next((f for f in [c['pc'], c['lr']] + bt if f and GAME_MOD.match(f)), None)
            crash = {'line': c['line'], 'signal': c['sig'], 'pc': c['pc'], 'lr': c['lr'], 'addr': c['addr'], 'first_game_frame': game, 'backtrace': bt,
                     'stack_scan': c['stack_scan'], 'text': redact(c['text']), 'before': [{'line': n, 'text': redact(t)} for n, t in c['before'][-3:]],
                     'count': self.crash_n, 'thread': None}
        if self.tomb:
            t = self.tomb
            crash = crash or {}
            crash['logcat'] = {'signal': redact(t['signal'] or ''), 'abort': redact(t['abort']) if t['abort'] else None, 'thread': t['thread'],
                               'frames': [redact(f, 160) for f in t['frames']]}
            if 'thread' in crash and crash.get('thread') is None: crash['thread'] = t['thread']
        if self.java:
            crash = crash or {}
            crash['java'] = {'line': self.java['line'], 'thread': self.java['thread'], 'trace': self.java['trace']}
        groups = [{'count': self.groups[k][0], 'first_line': self.groups[k][1], 'last_line': self.groups[k][2], 'example': redact(self.groups[k][3])}
                  for k in self.group_order]
        errors = {'groups': groups[:MAX_GROUPS], 'more_groups': max(0, len(groups) - MAX_GROUPS) + (1 if self.untracked else 0),
                  'lines': sum(g['count'] for g in groups) + self.untracked}
        if self.expected_n: errors['expected_stubs'] = {'lines': self.expected_n, 'groups': len(self.expected)}
        rules = sorted((self._rule(i) for i in self.rule_hits), key=lambda r: (r['first_line'] or 10 ** 12, r['id']))
        return {'file': name, 'bytes': size, 'lines': self.n, 'key_lines_dropped': self.dropped_keys, 'header': header, 'stage': stage,
                'verdict': self.verdict(rec), 'crash': crash, 'errors': errors, 'symptoms': self.symptom_matches(), 'rules': rules,
                'perf': self.perf.summary(), 'tail': [{'line': n, 'text': redact(t)} for n, t in self.tail]}


def render(d):
    o = [f"== log digest: {d['file']} ({d['lines']} lines, {d['bytes'] / 1e6:.1f} MB)"]
    h = d['header']
    if h:
        o.append('-- header')
        for k, label in (('app_build', 'app'), ('app_version', 'version'), ('app_id', 'app id'), ('device', 'device'), ('gpu', 'GPU'), ('driver', 'driver'),
                         ('driver_kind', 'driver kind'), ('driver_id', 'driver ID'), ('readiness', 'readiness'), ('game', 'game'), ('game_build_id', 'game build ID'),
                         ('host_build', 'host'), ('sdk', 'SDK'), ('shader_library', 'shader library')):
            if k in h: o.append(f'  {label}: {h[k]}')
        if 'switches' in h: o.append('  switches: ' + ' '.join(h['switches']))
    s = d['stage']
    if s:
        o.append('-- stage')
        o.append(f"  reached {s['number']} {s['name']} (L{s['line']}" + (f", about {s['seconds']:g} s" if s['seconds'] is not None else '') + ')'
                 + (f"; lines of earlier stages not seen: {', '.join(s['missing'])}" if s['missing'] else ''))
    v = d['verdict']
    o.append('-- verdict')
    o.append(f"  {v['label']}" + (f": {v['sentence']}" if v['sentence'] else ''))
    if v['rule']: o.append(f"  likely cause [{v['rule']}" + (f", L{v['line']}" if v['line'] else '') + f"]: {v['cause']}" + (f" ({v['doc']})" if v['doc'] else ''))
    c = d['crash']
    if c:
        o.append('-- crash')
        if 'line' in c:
            for b in c['before']: o.append(f"  L{b['line']}: {b['text']}")
            o.append(f"  L{c['line']}: {c['text']}" + (f'  (+{c["count"] - 1} more CRASH lines)' if c['count'] > 1 else ''))
            if c['first_game_frame']: o.append(f"  first game frame: {c['first_game_frame']}")
            if c['backtrace']: o.append('  bt: ' + ' '.join(c['backtrace']))
            if c['stack_scan']: o.append('  stack scan: ' + ' '.join(c['stack_scan']))
            if c.get('thread'): o.append(f"  thread: {c['thread']} (logcat)")
        if 'java' in c:
            o.append(f"  L{c['java']['line']}: uncaught exception on thread {c['java']['thread']}")
            o += [f'    {t}' for t in c['java']['trace']]
        if 'logcat' in c:
            lc = c['logcat']
            o.append(f"  logcat: {lc['signal']}" + (f" on thread {lc['thread']}" if lc['thread'] else ''))
            if lc['abort']: o.append(f"  logcat abort message: {lc['abort']}")
            o += [f'    {f}' for f in lc['frames']]
    e = d['errors']
    if e['groups'] or e.get('expected_stubs'):
        o.append(f"-- [RED] / error lines ({e['lines']} in {len(e['groups']) + e['more_groups']} groups)")
        for g in e['groups']:
            o.append(f"  ×{g['count']} L{g['first_line']}" + (f"–L{g['last_line']}" if g['last_line'] != g['first_line'] else '') + f": {g['example']}")
        if e['more_groups']: o.append(f"  +{e['more_groups']} more groups")
        if e.get('expected_stubs'): o.append(f"  expected stubs (\"not yet on Android/Vulkan\"): {e['expected_stubs']['lines']} lines in {e['expected_stubs']['groups']} groups")
    if d['symptoms']:
        o.append('-- symptom index matches')
        o += [f"  {('L' + str(x['line'])) if x['line'] else 'logcat'}: {x['heading']} → {x['doc']}" for x in d['symptoms']]
    rs = [r for r in d['rules'] if r['kind'] != 'info' or r['id'] != 'graceful-exit']
    if rs:
        o.append('-- rules')
        for r in rs[:12]:
            where = f"L{r['first_line']}" if r['first_line'] else 'logcat'
            o.append(f"  [{r['id']}] {r['kind']} ×{r['count']} {where}: {r['cause']}" + (f" ({r['doc']})" if r['doc'] else ''))
        if len(rs) > 12: o.append(f'  +{len(rs) - 12} more rules')
    p = perf_lines(d['perf'])
    if p: o.append('-- performance'); o += [f'  {x}' for x in p]
    if d['tail']:
        o.append(f"-- tail (last {len(d['tail'])} lines)")
        o += [f"  L{t['line']}: {t['text']}" for t in d['tail']]
    if d['key_lines_dropped']: o.append(f"({d['key_lines_dropped']} key-like line(s) left out)")
    return '\n'.join(o) + '\n'


def digest(inp, tail=15, rules=None, symptoms=None):
    dg = Digest(rules if rules is not None else load_rules(), symptoms if symptoms is not None else load_symptoms(), tail)
    with inp.open_log() as f:
        for line in f: dg.feed(line)
    if inp.open_logcat:
        with inp.open_logcat() as f: dg.feed_logcat(f)
    return dg.result(inp.name, inp.size, parse_record(inp.exit_text))


# ---------------------------------------------------------------- the filter

class Filter:
    def __init__(self, rules, noise, keep=None, context=0, max_lines=400):
        self.rules_rx = Matcher(rules)
        self.noise = noise; self.noise_rx = Matcher(noise)
        self.keep = re.compile(keep) if keep else None
        self.context, self.max_lines = max(context, 0), max(max_lines, 10)
        self.n = self.kept_n = self.out_n = 0
        self.dropped = collections.Counter()
        self.behind = collections.deque(maxlen=self.context)
        self.after = 0
        self.run = None          # [norm, first n, last n, count, text]
        self.head, self.tail_buf, self.cut = [], collections.deque(maxlen=self.max_lines // 2), 0
        self.perf = Perf()
        self.env = {}
        self.stages = set()

    def _first_stage(self, line):
        """the line is the first of its start-up stage (later lines of a stage, a second env line, a later frame-loop line, are not milestones)"""
        if not STAGE_ANY.match(line): return False
        for i, _name, rx in STAGES:
            if i not in self.stages and rx.match(line): self.stages.add(i); return True
        return False

    def _emit(self, s):
        self.out_n += 1
        if len(self.head) < self.max_lines - self.max_lines // 2: self.head.append(s); return
        if len(self.tail_buf) == self.tail_buf.maxlen: self.cut += 1
        self.tail_buf.append(s)

    def _flush(self):
        r = self.run
        if r is None: return
        self._emit(f'L{r[1]}: {redact(r[4])}' + (f'  (×{r[3]}, L{r[1]}–{r[2]})' if r[3] > 1 else ''))
        self.run = None

    def _keep(self, n, line):
        self.kept_n += 1
        k = normalise(line)
        if self.run is not None and self.run[0] == k: self.run[2] = n; self.run[3] += 1; return
        self._flush(); self.run = [k, n, n, 1, line]

    def feed(self, line):
        self.n += 1; n = self.n
        line = line.rstrip('\r\n')
        if key_like(line): self.dropped['(key-like)'] += 1; return
        self.perf.feed(n, line)
        if line.startswith('[nxlaunch] env '):
            m = ENV_RX.match(line)
            if m: self.env.pop(m.group(1), None); self.env[m.group(1)] = m.group(2)
        important = error_line(line) or line.startswith('[dm] CRASH')
        protect = important or protected(line) or self.rules_rx.search(line) >= 0 or self._first_stage(line) or (self.keep and self.keep.search(line))
        m = -1 if protect else self.noise_rx.search(line)
        if important and self.context:
            while self.behind:
                bn, bl, bid = self.behind.popleft(); self.dropped[bid] -= 1; self._keep(bn, bl)
            self.after = self.context
        if m >= 0 and self.after > 0: self.after -= 1; self._keep(n, line); return
        if m >= 0:
            rid = self.noise[m]['id']; self.dropped[rid] += 1
            if self.context: self.behind.append((n, line, rid))
            return
        if self.after > 0 and not important: self.after -= 1
        self._keep(n, line)

    def finish(self):
        self._flush()
        o = list(self.head)
        if self.cut: o.append(f'... {self.cut} kept lines cut here (--max-lines {self.max_lines}) ...')
        o += list(self.tail_buf)
        sw = switches(self.env)
        o.append('-- filtered out')
        if sw: o.append('  switches: ' + redact(' '.join(sw), 400))
        o += [f'  {x}' for x in perf_lines(self.perf.summary())]
        o.append(f'  input {self.n} lines, kept {self.kept_n} ({self.out_n} printed after collapsing), dropped {sum(self.dropped.values())}')
        top = sorted(((k, v) for k, v in self.dropped.items() if v > 0), key=lambda kv: (-kv[1], kv[0]))[:10]
        if top: o.append('  dropped by rule: ' + ', '.join(f'{k} {v}' for k, v in top))
        return '\n'.join(o) + '\n'


def filter_log(inp, keep=None, context=0, max_lines=400, rules=None, noise=None):
    fl = Filter(rules if rules is not None else load_rules(), noise if noise is not None else load_noise(), keep, context, max_lines)
    with inp.open_log() as f:
        for line in f: fl.feed(line)
    return fl.finish()


# ---------------------------------------------------------------- the command line

def main(argv=None):
    ap = argparse.ArgumentParser(description='A short digest of an Android run log (or, with --filter, the log without its routine lines).')
    ap.add_argument('log', help='the run log, a folder of logs (the newest *.log) or a zip')
    ap.add_argument('--logcat', metavar='FILE', help='an adb logcat capture of the run')
    ap.add_argument('--exit', metavar='FILE', help="the launcher's last-exit.txt (or dumpsys activity exit-info output)")
    ap.add_argument('--json', action='store_true', help='one JSON object instead of text')
    ap.add_argument('--tail', type=int, default=15, metavar='N', help='last lines to show (default 15)')
    ap.add_argument('--filter', action='store_true', help='print the log without routine lines (dm/tools/log_noise.tsv), with line numbers')
    ap.add_argument('--keep', metavar='REGEX', help='--filter: also keep lines matching this')
    ap.add_argument('--context', type=int, default=0, metavar='N', help='--filter: keep N lines around each [RED] / crash line')
    ap.add_argument('--max-lines', type=int, default=400, metavar='N', help='--filter: at most N output lines (first and last kept)')
    a = ap.parse_args(argv)
    try:
        inp = resolve(a.log, a.logcat, a.exit)
        if a.filter: out = filter_log(inp, a.keep, a.context, a.max_lines)
        else:
            d = digest(inp, a.tail)
            out = json.dumps(d, indent=1, sort_keys=True, ensure_ascii=False) + '\n' if a.json else render(d)
    except (OSError, zipfile.BadZipFile, KeyError, UnicodeError) as e:
        print(f'log_digest: cannot read {a.log}: {e}', file=sys.stderr); return 2
    except re.error as e:
        print(f'log_digest: bad --keep regex: {e}', file=sys.stderr); return 2
    sys.stdout.write(out)
    return 0


if __name__ == '__main__':
    sys.exit(main())
