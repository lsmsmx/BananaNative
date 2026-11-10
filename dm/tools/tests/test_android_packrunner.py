"""The on-device packer's tool runner (android/ondevice/dm_pack.c RUNNER, android/ONDEVICE.md): the Python it runs each tool with is taken from the C source and run
here, in a Python of its own. A run that ends with an uncaught exception or a sys.exit(<text>) leaves a [RED] line in the step's log, the reason Prepare's card
shows (PackService's Tail reads only [RED] lines; before, an exception left only its traceback, and the card said "exit code 1" with no reason); an exit code
or red() is passed on as it was."""
import codecs
import json
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
DM_PACK = REPO / 'android/ondevice/dm_pack.c'

TOOLS = {
    'raises.py': 'import os\ndef f(): raise FileNotFoundError(2, "No such file", "/x/romfs/a.bin")\nprint("progress: 1/2", flush=True)\nf()\n',
    'red_then_raises.py': 'print("[RED] /a.bin: an earlier red line (its shaders are left out)", flush=True)\nraise KeyError("vs_lib.bin")\n',
    'memory.py': 'raise MemoryError()\n',
    'exit_text.py': 'import sys\nsys.exit("usage: exit_text.py <in> <out>")\n',
    'exit_red.py': 'import sys\nsys.exit("[RED] the dump has no RomFS")\n',
    'exit_code.py': 'import sys\nsys.exit(3)\n',
    'ok.py': 'print("done")\n',
}

DRIVER = '''import json, os, sys
ns = {}
exec(compile(sys.argv[1], 'dm_pack.c RUNNER', 'exec'), ns)
d = sys.argv[2]
out = {}
for t in sorted(os.listdir(d)):
    if not t.endswith('.py'): continue
    log = os.path.join(d, t + '.log')
    code = ns['run'](os.path.join(d, t), [], [], log)
    with open(log, encoding='utf-8') as f: out[t] = [code, f.read().splitlines()]
print(json.dumps(out))
'''


def runner_source():
    """RUNNER's Python: the C string literals between `RUNNER[] =` and its `;`, joined and unescaped"""
    src = DM_PACK.read_text(encoding='utf-8')
    body = src[src.index('static const char RUNNER[] ='):]
    body = body[:body.index('";\n') + 2]
    return ''.join(codecs.decode(s, 'unicode_escape') for s in re.findall(r'"((?:[^"\\\n]|\\.)*)"', body))


class Runner(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with tempfile.TemporaryDirectory(prefix='packrunner_') as d:
            for name, text in TOOLS.items(): Path(d, name).write_text(text)
            r = subprocess.run([sys.executable, '-c', DRIVER, runner_source(), d], capture_output=True, text=True)
            assert r.returncode == 0, r.stderr
            cls.runs = json.loads(r.stdout)

    def last_red(self, tool):   # what PackService's Tail takes for the card: the log's last line starting with "[RED] "
        reds = [l for l in self.runs[tool][1] if l.startswith('[RED] ')]
        return reds[-1] if reds else None

    def test_exception_gives_a_reason(self):
        code, lines = self.runs['raises.py']
        self.assertEqual(code, 1)
        self.assertIn('Traceback (most recent call last):', lines, 'the traceback stays in the log')
        self.assertEqual(self.last_red('raises.py'), "[RED] FileNotFoundError: [Errno 2] No such file: '/x/romfs/a.bin' (raises.py:2)")
        self.assertEqual(lines[-1], self.last_red('raises.py'), 'after the traceback')

    def test_exception_after_another_red_line(self):   # the card names the exception, not an earlier file's red line
        self.assertEqual(self.runs['red_then_raises.py'][0], 1)
        self.assertEqual(self.last_red('red_then_raises.py'), "[RED] KeyError: 'vs_lib.bin' (red_then_raises.py:2)")

    def test_memory_error(self):   # PrepareText.toolStopped: "MemoryError" in the line gives the out-of-memory text
        self.assertEqual(self.runs['memory.py'][0], 1)
        self.assertEqual(self.last_red('memory.py'), '[RED] MemoryError (memory.py:1)')

    def test_exit_text_is_a_red_line(self):
        self.assertEqual(self.runs['exit_text.py'], [1, ['[RED] usage: exit_text.py <in> <out>']])
        self.assertEqual(self.runs['exit_red.py'], [1, ['[RED] the dump has no RomFS']], 'red() is not prefixed twice')

    def test_codes_pass_through(self):
        self.assertEqual(self.runs['exit_code.py'], [3, []])
        self.assertEqual(self.runs['ok.py'], [0, ['done']])


if __name__ == '__main__':
    unittest.main()
