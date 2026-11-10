"""A game with several registered builds (Mario Kart World 1.0.0 and 1.8.0: the same game, another module set and another RomFS) packs from a dump of either:
scripts/pack_android.py dump_modules takes the modules of the build row a dump matches, and optional_keys reads a profile's shader_optional.tsv, the shader keys only some builds have."""
import importlib.util
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('pack_android', REPO / 'scripts/pack_android.py')
pa = importlib.util.module_from_spec(spec); spec.loader.exec_module(pa)

A, B, C, D = ('%032x' % n for n in (1, 2, 3, 4))   # build IDs


class Profile:   # what dump_modules asks of a game profile
    dir = Path('.')
    def __init__(self, modules, builds): self._m, self._b = modules, builds
    def words(self, key): return self._m.split()
    def builds(self): return self._b


def nso(path, build_id):
    path.write_bytes(b'NSO0' + bytes(0x3c) + bytes.fromhex(build_id) + bytes(0x10))


class DumpModules(unittest.TestCase):
    G = Profile('main subsdk0 subsdk1 sdk', {'v0': {'display': '1.0.0', 'main': A, 'subsdk0': B, 'sdk': C},
                                             'v524288': {'display': '1.8.0', 'main': D, 'subsdk0': C, 'subsdk1': B, 'sdk': A}})

    def exefs(self, files):
        d = Path(self.enterContext(tempfile.TemporaryDirectory()))
        for n, i in files.items(): nso(d / n, i)
        return d

    def test_every_module_there(self):
        self.assertEqual(pa.dump_modules(self.G, self.exefs({'main': D, 'subsdk0': C, 'subsdk1': B, 'sdk': A})), ['main', 'subsdk0', 'subsdk1', 'sdk'])

    def test_build_with_one_module_less(self):
        self.assertEqual(pa.dump_modules(self.G, self.exefs({'main': A, 'subsdk0': B, 'sdk': C})), ['main', 'subsdk0', 'sdk'])

    def test_unregistered_set_is_left_to_the_caller(self):
        self.assertEqual(pa.dump_modules(self.G, self.exefs({'main': A, 'subsdk0': D, 'sdk': C})), ['main', 'subsdk0', 'subsdk1', 'sdk'])   # ids of no row: the caller says what is missing
        self.assertEqual(pa.dump_modules(self.G, self.exefs({'main': A, 'sdk': C})), ['main', 'subsdk0', 'subsdk1', 'sdk'])


class OptionalKeys(unittest.TestCase):
    def test_file(self):
        d = Path(self.enterContext(tempfile.TemporaryDirectory()))
        (d / 'shader_optional.tsv').write_text('# comment\n00aa\t1.0.0\n11bb\t1.8.0\n')
        g = Profile('main', {}); g.dir = d
        self.assertEqual(pa.optional_keys(g), {'00aa', '11bb'})
        g.dir = Path(self.enterContext(tempfile.TemporaryDirectory()))
        self.assertEqual(pa.optional_keys(g), set())


class SplitAbsent(unittest.TestCase):
    """split_shaders: the keys of the profile's map that the dump lacks and shader_optional.tsv names go to <package>/_absent.tsv; any other missing key stops the pack"""

    def run_split(self, dump_keys, optional):
        root = Path(self.enterContext(tempfile.TemporaryDirectory()))
        prof = root / 'profile'; prof.mkdir()
        row = lambda k: f'cs_x.bin\t0\tcompute\t{k}\tsrc\n'
        (prof / 'shader_map.tsv').write_text('# map\npackage\tindex\tstage\tdm_ctl_sha16\tsource\n' + ''.join(row(k) for k in ('k1', 'k2', 'k3')))
        if optional: (prof / 'shader_optional.tsv').write_text(''.join(f'{k}\t1.0.0\n' for k in optional))
        g = Profile('main', {}); g.dir = prof; g.id = '0000000000000000'
        def fake_run(cmd, env=None):   # shader_split.py: <romfs> <main> <pkg> <map> <facts> <pairs> <pso_pairs>
            Path(cmd[4]).mkdir(parents=True)
            Path(cmd[5]).write_text('# map\npackage\tindex\tstage\tdm_ctl_sha16\tsource\n' + ''.join(row(k) for k in dump_keys))
            for f in cmd[6:]: Path(f).write_text('')
            return type('R', (), {'returncode': 0})()
        args = type('A', (), {'shader_python': None})()
        pkg = root / 'pkg'
        old = pa.subprocess.run, pa.shader_python, pa.child_env
        pa.subprocess.run, pa.shader_python, pa.child_env = fake_run, lambda *a, **k: 'python', lambda **k: {}
        try: pa.split_shaders(args, g, root / 'v', pkg, (root / 'romfs', root / 'exefs'))
        finally: pa.subprocess.run, pa.shader_python, pa.child_env = old
        return pkg

    def test_optional_keys_are_listed(self):
        pkg = self.run_split(['k1', 'k2'], ['k3'])
        self.assertEqual((pkg / '_absent.tsv').read_text(), 'k3\n')

    def test_complete_dump_has_no_list(self):
        pkg = self.run_split(['k1', 'k2', 'k3'], ['k3'])
        self.assertFalse((pkg / '_absent.tsv').exists())

    def test_other_missing_key_stops(self):
        with self.assertRaises(pa.Error) as e: self.run_split(['k1'], ['k3'])
        self.assertIn('lack 1 of the 3', str(e.exception))

    def test_absent_optional(self):
        """pack_shaders builds the library without the keys of _absent.tsv only when the profile's shader_optional.tsv names every one"""
        pkg = self.run_split(['k1', 'k2'], ['k3'])
        g = Profile('main', {}); g.dir = pkg.parent / 'profile'
        self.assertEqual(pa.absent_optional(g, pkg), {'k3'})
        (pkg / '_absent.tsv').write_text('k2\nk3\n')
        self.assertEqual(pa.absent_optional(g, pkg), set(), 'k2 is not optional')
        (pkg / '_absent.tsv').unlink()
        self.assertEqual(pa.absent_optional(g, pkg), set())


class MarioKartWorldOptional(unittest.TestCase):
    """Mario Kart World 1.8.0 as update 917504 (Forgejo #50): its split lacks only the profile's optional key, which the profile's map has"""

    def test_profile(self):
        d = REPO / 'dm/games/0400C3F00006E000'
        g = Profile('main', {}); g.dir = d
        self.assertEqual(pa.optional_keys(g), {'797e77c2ec4341c5'})
        self.assertLessEqual(pa.optional_keys(g), pa.map_keys(d / 'shader_map.tsv'))


if __name__ == '__main__':
    unittest.main()
