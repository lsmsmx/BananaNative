"""Shaders the host saved (DM_NVN_SHDUMP) enter the split through nvn_dumps, and runtime_shaders.py reports an NVN2 control section no reader pairs. Synthetic bytes only."""
import contextlib
import io
import struct
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools/fuzz'))
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
import fuzzlib
ss = fuzzlib.import_shader_split()
import runtime_shaders as rs


def nvn2(stage=5, code=bytes(0x100)):
    """An NVN2 control section (0xd8 bytes: magic, version 0xc9, +0x40 c1 start, +0x44 code length, +0x56 stage, +0x58 thread group) and its code area"""
    ctl = bytearray(0xd8); ctl[:8] = ss.CTL_MAGIC + b'\xc9\0\0\0'
    struct.pack_into('<2I', ctl, 0x40, len(code), len(code)); ctl[0x56] = stage; struct.pack_into('<3I', ctl, 0x58, 32, 1, 1)
    return bytes(ctl), code


class NvnDumps(unittest.TestCase):
    def dumps(self, files):
        with tempfile.TemporaryDirectory() as t, contextlib.redirect_stdout(io.StringIO()) as log:
            for name, data in files.items(): (Path(t) / name).write_bytes(data)
            return ss.nvn_dumps(t), log.getvalue()

    def test_saved_shader_is_taken_under_its_key(self):
        ctl, code = nvn2(); k = ss.skey(ctl, code)
        out, log = self.dumps({k + '.ctl': ctl + bytes(0x880 - 0xd8), k + '.code': code})   # the host's copy is longer than the section
        self.assertEqual([(w, s, c, k2) for w, s, c, k2 in out], [(f'/nvn_shdump/{k}', 'cs', ctl, code)])
        self.assertNotIn('[RED]', log)

    def test_short_section_and_wrong_name_are_left_out(self):
        ctl, code = nvn2(); k = ss.skey(ctl, code)
        out, log = self.dumps({k + '.ctl': ctl[:0x48], k + '.code': code, '0123456789abcdef.ctl': ctl, '0123456789abcdef.code': code})
        self.assertEqual(out, [])
        self.assertIn('less than 0xd8 bytes', log); self.assertIn('do not hash to the file name', log)

    def test_no_folder(self):
        self.assertEqual(self.dumps({})[0], [])
        self.assertEqual(ss.nvn_dumps('/nonexistent/nvn_shdump'), [])


class TaskMesh(unittest.TestCase):
    """NVN2 task / mesh programs: their stage from control +0x56 (their SPH says vertex), their translator layout, the per-primitive values from the second header (checked against the paired pixel shader)"""
    def mesh(self):
        code = bytearray(0x100); code[0:4] = struct.pack('<I', 0x461)   # an SPH (at the code area's start in NVN2) that says vertex
        code[0x35] = 0xf0; code[0x36] = 0x03                                    # position x y z w, generic 0 components x y
        code[0x80:0x84] = struct.pack('<I', 0x1061); code[0x80 + 0x36] = 0x10    # the per-primitive header at +0x80: generic 1 x
        ctl, code = nvn2(7, bytes(code))
        ctl = bytearray(ctl); struct.pack_into('<HH', ctl, 0x58, 32, 256); struct.pack_into('<HH', ctl, 0x60, 128, 64); struct.pack_into('<I', ctl, 0x64, 0x80)
        ctl[0x5f] = 3   # triangles
        return bytes(ctl), code

    def test_points(self):   # a points program (control +0x5f = 1) with the point size (byte 0x35 bit 3) before position, as Ocarina of Time's 28f82c129deea140
        ctl, code = self.mesh(); ctl, code = bytearray(ctl), bytearray(code)
        ctl[0x5f] = 1; code[0x35] = 0xf8; code[0x36] = 0; struct.pack_into('<I', ctl, 0x64, 0)
        f = ss.mesh_facts('m', 'ms', bytes(ctl), bytes(code), {}, set())
        self.assertEqual((f['mesh'], f['mesh_topology']), ([128, 64, 256, 132, 0, 1, 2, 3], 1))
        ctl[0x5f] = 3; self.assertNotIn('mesh_topology', ss.mesh_facts('m', 'ms', bytes(ctl), bytes(code), {}, set()))   # triangles, the default, are not written
        with contextlib.redirect_stdout(io.StringIO()) as log:
            ctl[0x5f] = 2; self.assertNotIn('mesh', ss.mesh_facts('m', 'ms', bytes(ctl), bytes(code), {}, set()))
        self.assertIn('neither points (1) nor triangles (3)', log.getvalue())

    def test_stage_byte(self):
        ctl, code = self.mesh()
        self.assertEqual(ss.nvn2_stage(ctl, code), 'ms')
        self.assertEqual(ss.nvn2_stage(nvn2(6)[0], code), 'ts')
        self.assertEqual(ss.nvn2_stage(nvn2(0)[0], code), 'vs')

    def test_layout_and_per_primitive_values(self):
        ctl, code = self.mesh()
        ps = bytearray(0x100); ps[0x18] = 0x02 | 0x04   # generic 0: x perspective (2), y flat (1)
        ps[0x19] = 0x01                                   # generic 1: x flat
        ps[0x1a] = 0x02                                   # generic 2: x perspective, written by neither
        keys = {'m': ('/a', 'ms', ctl, code), 'p': ('/a', 'ps', b'', bytes(ps))}
        f = ss.mesh_facts('m', 'ms', ctl, code, keys, {('m', 'p', 'mesh')})
        # planes 0-3 position, 4 / 5 generic 0 x y; generic 0 y is written per vertex, so only generic 1 x is per primitive; generic 2 is padded
        self.assertEqual(f, {'local': [32, 1, 1], 'shared': 0, 'mesh': [128, 64, 256, 0, 1, 2, 3, 4, 5, -1, 8, -2, 2], 'pair': ['p']})
        with contextlib.redirect_stdout(io.StringIO()) as log:   # a pixel shader whose flat inputs are not the second header's values: a red line, the header's layout kept
            ps[0x19] = 0x02; g = ss.mesh_facts('m', 'ms', ctl, code, {**keys, 'p': ('/a', 'ps', b'', bytes(ps))}, {('m', 'p', 'mesh')})
        self.assertIn('[RED] mesh program m', log.getvalue()); self.assertEqual(g['mesh'][:11], f['mesh'][:11])
        t = ss.mesh_facts('t', 'ts', nvn2(6)[0], code, keys, {('t', 'm', 'task')})
        self.assertEqual(t['mesh'], [0, 0, 0]); self.assertEqual(t['pair'], ['m'])


class RuntimeShaders(unittest.TestCase):
    def test_unpaired_control_section_is_reported(self):
        ctl, code = nvn2(stage=7)
        with tempfile.TemporaryDirectory() as t:
            p = Path(t) / 'loose.bin'; p.write_bytes(bytes(0x40) + ctl + code)
            shaders, loose, why = rs.scan((str(p), 'loose.bin'))
        self.assertEqual((shaders, why), ({}, None))
        self.assertEqual(loose, [('loose.bin', 0x40, 7, len(code))])

    def test_switch2mac_key(self):
        ctl, code = nvn2()
        self.assertEqual(rs.s2m_key(ctl + b'tail', code), __import__('hashlib').sha256(ctl + code).hexdigest()[:16])


if __name__ == '__main__':
    unittest.main()
