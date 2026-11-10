#!/usr/bin/env python3
"""Switch 2 task / mesh programs through the offline translator (dmsh_xlate, arch sm86, stage ts / ms): the stage memory instructions 0x923 / 0x927
(Ryujinx Instructions/InstEmitAmpereMesh.cs) in hand-encoded programs, emitted as SPV_EXT_mesh_shader TaskEXT / MeshEXT entry points (CodeGen/Spirv/SpirvMeshEntry.cs)
that must pass spirv-val --target-env vulkan1.3 (SPIRV_VAL, default spirv-val on PATH), and forms the translator refuses. Needs DMSH_TEST_XLATE (the translator)
like the other translator tests."""
import json, os, shutil, struct, subprocess, tempfile, unittest
from pathlib import Path

EXIT = (0x000fea0003800000, 0x000000000000794d)   # hi, lo: EXIT
RZ = 255
CTL = (1 << 41) | (7 << 46) | (7 << 49)   # control bits: stall 1, no write or read barrier, no wait
STAGE_LOAD, STAGE_STORE = 0x923, 0x927
HEADER, TASK_READ, TASK_WRITE, PLANES, INDICES, COUNTS = 0x7800, 0x3800, 0xb800, 0xf800, 0x8000, 0x8800   # the mode words (bits 64..95)
VAL = os.environ.get('SPIRV_VAL') or shutil.which('spirv-val')


def inst(hi, lo): return struct.pack('<QQ', lo, hi)


def stage(op, mode, rd=0, ra=RZ, rb=0, imm=0):
    """0x923 Rd, [Ra + imm] / 0x927 [Ra + imm], Rb, unpredicated"""
    return inst(mode | CTL, op | 0x7000 | rd << 16 | ra << 24 | rb << 32 | imm << 40)


def program(*insts): return bytes(0x80) + b''.join(insts) + inst(*EXIT)   # a zero SPH (no local memory), as the code area of a graphics stage


# SPIR-V numbers the checks read
OP_ENTRY_POINT, OP_EXECUTION_MODE, OP_DECORATE, OP_VARIABLE, OP_EMIT_MESH_TASKS, OP_SET_MESH_OUTPUTS = 15, 16, 71, 59, 5294, 5295
MODEL_TASK, MODEL_MESH = 5364, 5365
MODE_LOCAL_SIZE, MODE_OUTPUT_VERTICES, MODE_OUTPUT_PRIMITIVES, MODE_OUTPUT_TRIANGLES = 17, 26, 5270, 5298
DEC_SPEC_ID, DEC_BUILTIN, DEC_LOCATION, DEC_PER_PRIMITIVE = 1, 11, 30, 5271
BUILTIN_PRIMITIVE_TRIANGLE_INDICES = 5296
SC_TASK_PAYLOAD = 5402


def parse(spv):
    """→ {opcode: [operand word lists]} of a SPIR-V module"""
    w = struct.unpack(f'<{len(spv) // 4}I', spv)
    ops, i = {}, 5
    while i < len(w):
        n = w[i] >> 16
        ops.setdefault(w[i] & 0xffff, []).append(list(w[i + 1:i + n]))
        i += max(1, n)
    return w[1], ops


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'set DMSH_TEST_XLATE to the offline translator')
class Mesh(unittest.TestCase):
    def run_jobs(self, code, **job):
        d = tempfile.mkdtemp(prefix='ampere_mesh_'); self.addCleanup(shutil.rmtree, d, True)
        work = Path(d); (work / 'k.bin').write_bytes(code)
        j = dict(key='k', code=str(work / 'k.bin'), arch='sm86', tex={}, img={}, attr={}, fout={}, debug=True, subgroup_size=64, ssbo_align=64, gs_passthrough=False)
        j.update(job)
        (work / 'jobs.json').write_text(json.dumps([j]))
        r = subprocess.run([os.environ['DMSH_TEST_XLATE'], str(work / 'jobs.json'), d], capture_output=True, text=True)
        return r, work

    def translate(self, code, **job):
        r, work = self.run_jobs(code, **job)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        spv = (work / 'k.spv').read_bytes()
        if VAL:
            v = subprocess.run([VAL, '--target-env', 'vulkan1.3', str(work / 'k.spv')], capture_output=True, text=True)
            self.assertEqual(v.returncode, 0, v.stdout + v.stderr)
        return parse(spv), json.loads((work / 'k.res.json').read_text())

    def refused(self, code, text, **job):
        r, work = self.run_jobs(code, **job)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('[RED] k: NotSupportedException', r.stderr)
        self.assertIn(text, r.stderr)
        self.assertFalse((work / 'k.spv').exists())

    # A task: its workgroup index (header word 0) into the payload at 0x10, and as the mesh workgroups to launch (+4)
    TASK = program(stage(STAGE_LOAD, HEADER, rd=0), stage(STAGE_STORE, TASK_WRITE, rb=0, imm=0x10), stage(STAGE_STORE, TASK_WRITE, rb=0, imm=4))

    def test_task(self):
        (version, ops), res = self.translate(self.TASK, stage='ts', local=[32, 1, 1], mesh=[0, 0, 64])
        self.assertEqual(version, 0x10400)   # SPV_EXT_mesh_shader takes SPIR-V 1.4
        self.assertEqual([e[0] for e in ops[OP_ENTRY_POINT]], [MODEL_TASK])
        self.assertIn([ops[OP_ENTRY_POINT][0][1], MODE_LOCAL_SIZE, 32, 1, 1], ops[OP_EXECUTION_MODE])
        self.assertEqual(len(ops[OP_EMIT_MESH_TASKS]), 1)
        payload = [v for v in ops[OP_VARIABLE] if v[2] == SC_TASK_PAYLOAD]
        self.assertEqual(len(payload), 1)
        self.assertEqual(ops[OP_EMIT_MESH_TASKS][0][3], payload[0][1])   # the payload goes with the launch
        self.assertEqual(res['stage'], 'ts')

    # A mesh program: its index (task memory word 0), a payload word by it, the four position planes and the generic planes of vertex 0, one per-primitive value of
    # primitive 0, triangle 0's indices (bytes 4..6) and the primitive count (byte 3)
    MESH = program(stage(STAGE_LOAD, TASK_READ, rd=1), stage(STAGE_LOAD, TASK_READ, rd=2, ra=1, imm=0x10),
                   *[stage(STAGE_STORE, PLANES, rb=2, imm=0x80 * p) for p in range(6)],
                   stage(STAGE_STORE, PLANES, rb=2, imm=2 * 6 * 0x80),   # 64 vertices: 2 chunks of 6 planes, then the per-primitive chunk
                   stage(STAGE_STORE, INDICES, rb=RZ, imm=4), stage(STAGE_STORE, INDICES, rb=1, imm=5), stage(STAGE_STORE, INDICES, rb=2, imm=6),
                   stage(STAGE_STORE, COUNTS, rb=1, imm=3))
    MESH_LAYOUT = [64, 32, 64, 0, 1, 2, 3, 4, 5, -1, 8, -3, 9, -2, 3]   # g0.xy per vertex, g1.xy per primitive (values 0 and 2; 1 has no output), g3 padded

    def test_mesh(self):
        (version, ops), res = self.translate(self.MESH, stage='ms', local=[32, 1, 1], mesh=self.MESH_LAYOUT, gl_depth_spec=0)
        self.assertEqual(version, 0x10400)
        ep = ops[OP_ENTRY_POINT][0]
        self.assertEqual(ep[0], MODEL_MESH)
        modes = {m[1]: m[2:] for m in ops[OP_EXECUTION_MODE] if m[0] == ep[1]}
        self.assertEqual(modes[MODE_OUTPUT_VERTICES], [64])
        self.assertEqual(modes[MODE_OUTPUT_PRIMITIVES], [32])
        self.assertIn(MODE_OUTPUT_TRIANGLES, modes)
        self.assertEqual(len(ops[OP_SET_MESH_OUTPUTS]), 1)
        decos = {}
        for dc in ops[OP_DECORATE]: decos.setdefault(dc[0], {})[dc[1]] = dc[2:]
        locations = {d[DEC_LOCATION][0]: DEC_PER_PRIMITIVE in d for d in decos.values() if DEC_LOCATION in d}
        self.assertEqual(locations, {0: False, 1: True, 3: False})   # per vertex g0 and the padded g3, per primitive g1
        self.assertTrue(any(d.get(DEC_BUILTIN) == [BUILTIN_PRIMITIVE_TRIANGLE_INDICES] for d in decos.values()))
        self.assertTrue(any(d.get(DEC_SPEC_ID) == [0] for d in decos.values()))   # the GL depth constant
        self.assertEqual(len([v for v in ops[OP_VARIABLE] if v[2] == SC_TASK_PAYLOAD]), 1)
        self.assertEqual(res['mesh'], {'max_vertices': 64, 'max_primitives': 32, 'vertex_locations': [0, 3], 'primitive_locations': [1]})
        # SPIR-V 1.4: the entry point lists every global variable
        self.assertEqual(set(ep[2 + (len(b'main') + 4) // 4:]), {v[1] for v in ops[OP_VARIABLE] if v[2] != 7})

    def test_mesh_without_payload(self):   # a mesh program run without a task reads only its index: no task payload declared
        (_, ops), _ = self.translate(program(stage(STAGE_LOAD, HEADER, rd=1), stage(STAGE_STORE, PLANES, rb=1), stage(STAGE_STORE, COUNTS, rb=RZ, imm=3)),
                                     stage='ms', local=[32, 1, 1], mesh=[32, 16, 128, 0, 1, 2, 3])
        self.assertFalse([v for v in ops[OP_VARIABLE] if v[2] == SC_TASK_PAYLOAD])

    def test_refused_mode(self):
        self.refused(program(stage(STAGE_STORE, 0x9800, rb=0)), 'stage memory mode 0x9800', stage='ms', local=[32, 1, 1], mesh=[32, 16, 128, 0, 1, 2, 3])

    def test_refused_window_of_the_other_stage(self):
        self.refused(program(stage(STAGE_STORE, TASK_WRITE, rb=0, imm=4)), 'in a mesh program', stage='ms', local=[32, 1, 1], mesh=[32, 16, 128, 0, 1, 2, 3])
        self.refused(program(stage(STAGE_STORE, PLANES, rb=0)), 'in a task program', stage='ts', local=[32, 1, 1], mesh=[0, 0, 64])

    def test_refused_outside_task_and_mesh(self):
        self.refused(program(stage(STAGE_STORE, PLANES, rb=0)), 'outside a task / mesh program', stage='vs')

    def test_refused_header_word(self):   # only the header word at 0 is known
        self.refused(program(stage(STAGE_LOAD, HEADER, rd=1, imm=4)), 'other than [RZ + 0]', stage='ts', local=[32, 1, 1], mesh=[0, 0, 64])


if __name__ == '__main__':
    unittest.main()
