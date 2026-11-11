"""The translator the repository ships (dm/gfx/tools/dmsh_xlate_bin) must be the one dmsh_chain.lock names.

The Mac and Windows packers run these dlls (Windows: `dotnet dmsh_xlate.dll`); a translator commit that moves the lock without
committing the rebuilt dlls makes every packer run stop at "translator does not match the lock file" (Forgejo issue #115, 2026-11-10:
the dlls were left at their 2026-11-07 build while the lock moved with the 2026-11-08 translator commits). The fingerprints are plain
file hashes, so this needs no .NET runtime; tool_lock.py --check-tools also runs the translator for its embedded source digest.
"""
import hashlib, os, unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))
BIN = os.path.join(ROOT, 'dm', 'gfx', 'tools', 'dmsh_xlate_bin')
LOCK = os.path.join(ROOT, 'dm', 'gfx', 'tools', 'dmsh_chain.lock')


def sha16(p):
    with open(p, 'rb') as f: return hashlib.sha256(f.read()).hexdigest()[:16]


class XlateBinLock(unittest.TestCase):
    def test_shipped_dlls_are_the_locked_ones(self):
        want = dict(l.rstrip('\n').split('\t', 1) for l in open(LOCK) if '\t' in l and not l.startswith('#'))
        for dll in ('dmsh_xlate.dll', 'Ryujinx.Graphics.Shader.dll', 'Ryujinx.Common.dll'):
            with self.subTest(dll=dll):
                self.assertEqual(sha16(os.path.join(BIN, dll)), want.get(dll),
                                 f'{dll} in dmsh_xlate_bin is not the one dmsh_chain.lock names: commit the rebuilt translator with the lock')


if __name__ == '__main__':
    unittest.main()
