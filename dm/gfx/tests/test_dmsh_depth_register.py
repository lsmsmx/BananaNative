#!/usr/bin/env python3
"""The register a Switch 2 pixel shader's depth output is read from (dmsh_xlate, arch sm86, stage ps). The colour outputs take four registers for each render target with any
component enabled in the SPH's OmapTargets (word 18), a component in its own slot, and the depth (OmapDepth, word 19 bit 1) is two registers after the last of them:
4 x (enabled targets) + 1. Ryujinx counted components (PopCount(OmapTargets) + 1), right only when every enabled target writes all four; Ocarina of Time's agl depth
passes (OmapTargets 0x100, 0x30, 0x10f, 0x3f) write their depth to R5 or R9 and the count read R2, R3, R6 or R7
(docs/reference/shaders.md#a-pixel-shader-with-partial-colour-masks-wrote-the-wrong-depth). Each program moves i + 0.5 into Ri, so the constant stored to the depth names
the register (the method of Swap2-Native's test of the same name, MIT). Needs DMSH_TEST_XLATE (the translator) like the other translator tests."""
import json, os, re, struct, subprocess, tempfile, unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1] / 'tools'


def inst(hi, lo): return struct.pack('<QQ', lo, hi)


EXIT = inst(0x000fea0003800000, 0x000000000000794d)
def mov(rd, value): return inst(0x000fc60000000f00, value << 32 | rd << 16 | 0x7802)   # MOV Rd, imm32
def fbits(f): return struct.unpack('<I', struct.pack('<f', f))[0]


def sph(omap_targets, depth=True):
    """a pixel shader's 0x80-byte SPH: type 2, version 3, shader type 5 (pixel); word 18 the colour components written, word 19 bit 1 the depth"""
    w = [0] * 32
    w[0] = 2 | 3 << 5 | 5 << 10
    w[18] = omap_targets
    w[19] = 2 if depth else 0
    return struct.pack('<32I', *w)


REGS = 34   # R0..R33: eight full targets and the depth
def program(omap_targets): return sph(omap_targets) + b''.join(mov(r, fbits(r + 0.5)) for r in range(REGS)) + EXIT


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'set DMSH_TEST_XLATE to the offline translator')
class DepthRegister(unittest.TestCase):
    def depth_of(self, omap_targets):
        """→ the value the translated shader stores to its depth output (spirv-cross's MSL)"""
        with tempfile.TemporaryDirectory(prefix='depth_register_') as d:
            work = Path(d); (work / 'k.bin').write_bytes(program(omap_targets))
            job = [dict(key='k', stage='ps', arch='sm86', code=str(work / 'k.bin'), tex={}, img={}, attr={}, fout={str(t): 'float' for t in range(8)}, debug=False, reduced=True)]
            (work / 'jobs.json').write_text(json.dumps(job))
            r = subprocess.run([os.environ['DMSH_TEST_XLATE'], str(work / 'jobs.json'), d], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertTrue((work / 'k.spv').exists(), r.stdout + r.stderr)
            r = subprocess.run([str(TOOLS / 'spirv_cross_bin/spirv-cross'), '--msl', '--msl-version', '20300', str(work / 'k.spv')], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            m = re.findall(r'gl_FragDepth = ([-0-9.e]+)', r.stdout)
            self.assertEqual(len(m), 1, r.stdout)
            return float(m[0])

    def test_full_targets(self):   # every enabled target writes RGBA: both rules agree
        self.assertEqual(self.depth_of(0xf), 5.5)
        self.assertEqual(self.depth_of(0xff), 9.5)

    def test_one_component_of_a_later_target(self):   # 0x100: RT2 R only, R0 its colour (OoT's agl depth pass c668ea1520abf635 writes its depth to R5)
        self.assertEqual(self.depth_of(0x100), 5.5)

    def test_partial_masks(self):
        self.assertEqual(self.depth_of(0x30), 5.5)    # RT1 RG
        self.assertEqual(self.depth_of(0x10f), 9.5)   # RT0 RGBA, RT2 R
        self.assertEqual(self.depth_of(0x3f), 9.5)    # RT0 RGBA, RT1 RG

    def test_eight_targets_one_component_each(self):
        self.assertEqual(self.depth_of(0x11111111), 33.5)


if __name__ == '__main__':
    unittest.main()
