#!/usr/bin/env python3
"""Classify the [longframe] lines of a game log by what took the frame's time.

    python3 dm/tools/longframes.py <game log> [--min-ms 50] [--top 20]

Each line (hle_nvn.c, with the attribution of 2026-11-03: the GPU's busy time and span, the frame's waits, CPU, page faults, file reads and, on Android, the replay's
waits) goes to the first cause that took at least 40 % of the frame: pipelines made on the frame path, texture uploads, the GPU waiting for the recording (span minus
busy: a stall of the replay thread inside a submission), GPU work, the game's waits on GPU fences, the game waiting for the replay worker, the game thread's own CPU,
file reads, page faults read from storage (major), else "unexplained". Lines of 2026-11-03 (perf3) add: lookups that waited for a memory pool placement's lock (first), the
game's fence waits that the replay's recording time explains (not the GPU), and the frame thread's guest waits (another game thread), service waits and sleeps; the longest
frames show what ended the longest guest wait and the busiest threads. Older lines (no attribution) still classify by pipelines, uploads and GPU time. Prints the counts
and summed time per cause for frames of at least --min-ms, and the longest frames with their numbers.
"""
import argparse
import re
import sys

NUM = r'(-?[\d.]+)'
FIELDS = {
    'frame': r'frame (\d+) \(',
    't': r'\(' + NUM + r' s\) whole frame',
    'fr': r'whole frame ' + NUM + ' ms',
    'gpu': r'GPU ' + NUM + ' ms',
    'span': r'span ' + NUM + ' ms',
    'pso': r'new pipelines (\d+)',
    'make': r'shader libraries \d+, ' + NUM + ' ms in all',
    'up': r'texture uploads (\d+) first',
    'up_ms': r'MB / ' + NUM + ' ms',
    'hy': r'hybrid pipeline (\d+)',
    'fence': r'GPU fences ' + NUM,
    'acq': r'acquire ' + NUM,
    'cpu': r"this thread's CPU " + NUM,
    'pcpu': r'process CPU ' + NUM,
    'majflt': r'page faults (\d+) major',
    'file_ms': r'files: read [\d.]+ MB ' + NUM + ' ms',
    'room': r'waited for the replay worker ' + NUM,
    'rec': r'replay recording ' + NUM,
    'rec_cpu': r'replay recording [\d.]+ \(its CPU ' + NUM,
    'enc': r'waited for the encoder ' + NUM,
    'pw': r'waited for prewarm ' + NUM,
    'gw': r'guest waits ' + NUM,
    'gsl': r'sleeps ' + NUM,
    'gnat': r'service waits ' + NUM,
    'place': r'pool placement ' + NUM + ' ms',
    'give': r'give-back ' + NUM + ' ms',
    'lkr': r"lock waited for: replay " + NUM,
    'lko': r"lock waited for: replay [\d.]+, others " + NUM,
    'new_n': r'new images (\d+), ',
    'new_ms': r'new images \d+, ' + NUM + ' ms',
}
TEXT = {
    'gw_what': re.compile(r'guest waits [\d.]+ \(longest [\d.]+: ([^)]*)\)'),
    'busy': re.compile(r'busiest threads over ([^;]*)'),
}
RX = {k: re.compile(v) for k, v in FIELDS.items()}


def parse(line):
    if '[longframe]' not in line:
        return None
    r = {}
    for k, rx in RX.items():
        m = rx.search(line)
        if m:
            r[k] = float(m.group(1))
    for k, rx in TEXT.items():
        m = rx.search(line)
        if m:
            r[k] = m.group(1).strip()
    return r if 'fr' in r else None


def cause(r):
    fr = max(r['fr'], 1e-6)
    share = lambda v: v / fr >= 0.4
    span, gpu = r.get('span'), r.get('gpu', 0.0)
    if share(r.get('lkr', 0) + r.get('lko', 0)):
        return 'waiting for a pool placement (lock)'
    if r.get('pso', 0) and share(r.get('make', 0) + r.get('pw', 0)):
        return 'pipelines'
    if r.get('up_ms', 0) and share(r['up_ms']):
        return 'texture uploads'
    if r.get('new_n', 0) and share(r.get('new_ms', 0)):
        return 'new images (first texture uses)'
    if span is not None and share(span - gpu):
        return 'GPU waiting for the recording'
    if share(gpu) and span is None and r.get('pso', 0):
        return 'pipelines (old line: GPU = recording stall)'
    if share(gpu):
        return 'GPU'
    if share(r.get('fence', 0)) and share(r.get('rec', 0) - gpu):
        if 'rec_cpu' in r:
            return 'GPU fences: replay blocked in a submission' if share(r['rec'] - r['rec_cpu']) else 'GPU fences: replay recording busy (CPU)'
        return 'GPU fences: replay recording slow'
    for k, name in (('fence', 'game waiting on GPU fences'), ('room', 'game waiting for the replay worker'), ('cpu', "game thread's CPU"), ('file_ms', 'file reads'),
                    ('gw', 'game thread waiting on a game thread'), ('gnat', 'game thread in a service wait'), ('gsl', 'game thread sleeping')):
        if share(r.get(k, 0)):
            return name
    if r.get('majflt', 0) >= 50:
        return 'page faults from storage'
    if share(r.get('acq', 0)):
        return 'acquire (pacing)'
    return 'unexplained'


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('log')
    ap.add_argument('--min-ms', type=float, default=50)
    ap.add_argument('--top', type=int, default=20)
    a = ap.parse_args(argv)
    rows = []
    with open(a.log, errors='replace') as f:
        for line in f:
            r = parse(line)
            if r:
                rows.append(r)
    long_ = [r for r in rows if r['fr'] >= a.min_ms]
    print(f'{len(rows)} [longframe] lines, {len(long_)} of at least {a.min_ms:g} ms; time over 16.7 ms: {sum(max(r["fr"] - 16.7, 0) for r in rows) / 1000:.1f} s')
    by = {}
    for r in long_:
        c = cause(r)
        n, t = by.get(c, (0, 0.0))
        by[c] = (n + 1, t + r['fr'])
    for c, (n, t) in sorted(by.items(), key=lambda kv: -kv[1][1]):
        print(f'  {c:44s} {n:5d} frames {t / 1000:8.1f} s')
    print(f'pipelines made on the frame path: {sum(r.get("pso", 0) for r in rows):.0f}, {sum(r.get("make", 0) for r in rows) / 1000:.1f} s; draws with a linked (GPL) pipeline: {sum(r.get("hy", 0) for r in rows):.0f}')
    print(f'longest {a.top}:')
    for r in sorted(long_, key=lambda r: -r['fr'])[:a.top]:
        keys = ('gpu', 'span', 'pso', 'make', 'up', 'up_ms', 'fence', 'cpu', 'pcpu', 'majflt', 'file_ms', 'room', 'rec', 'enc', 'pw', 'rec_cpu', 'gw', 'gsl', 'gnat', 'place', 'give', 'lkr', 'lko')
        print(f'  {r.get("t", 0):8.2f} s {r["fr"]:8.1f} ms  {cause(r):30s} ' + ' '.join(f'{k}={r[k]:g}' for k in keys if k in r)
              + (f' [{r["gw_what"]}]' if r.get('gw_what') else '') + (f' [{r["busy"]}]' if r.get('busy') else ''))
    return 0


if __name__ == '__main__':
    sys.exit(main())
