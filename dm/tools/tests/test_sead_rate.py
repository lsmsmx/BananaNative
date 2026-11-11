"""sead's frame-rate controller found without a build ID (dm/tools/nso_code.py sead_find) on synthetic images, no game files: the controller's sdiv of 60 by its interval in the forms
the Switch 2 mains have (the quotient in the dividend's register, Donkey Kong Bananza; in another, The Legend of Zelda: Ocarina of Time and Splatoon Raiders), inside a function
the unwind table bounds that also has the clamp's ldp of the controller, 60.0f and an fcvtzs; anything less, or two of them, gates nothing."""
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import nso_code

RET, NOP = 0xd65f03c0, 0xd503201f
FN, FN2, TEXT = 0x1000, 0x2000, 0x4000


def unwind(text, functions, mod0=0xe00):
    """MOD0 and an .eh_frame_hdr in text describing the functions (as test_il2cpp_sym.unwind: a zR CIE, pcrel/sdata4 PCs)"""
    ehs, cie = mod0 + 0x40, mod0 + 0x100
    struct.pack_into('<I', text, 4, mod0)
    text[mod0:mod0 + 4] = b'MOD0'
    struct.pack_into('<ii', text, mod0 + 0x10, ehs - mod0, ehs + 12 + len(functions) * 8 - mod0)
    text[ehs:ehs + 4] = bytes((1, 0x1b, 3, 0x3b))
    struct.pack_into('<iI', text, ehs + 4, cie - (ehs + 4), len(functions))
    struct.pack_into('<II', text, cie, 16, 0)
    text[cie + 8:cie + 17] = b'\x01zR\0\x04\x78\x1e\x01\x1b'
    for i, (start, end) in enumerate(functions):
        fde = cie + 0x40 + i * 24
        struct.pack_into('<ii', text, ehs + 12 + i * 8, start - ehs, fde - ehs)
        struct.pack_into('<IIiI', text, fde, 16, fde + 4 - cie, start - (fde + 8), end - start)


def ldr_w(t, n, off): return 0xB9400000 | (off // 4) << 10 | n << 5 | t
def str_w(t, n, off): return 0xB9000000 | (off // 4) << 10 | n << 5 | t
def sdiv(d, n, m): return 0x1AC00C00 | m << 16 | n << 5 | d
def mov60(d): return 0x52800780 | d
def mov_w(d, s): return 0x2A0003E0 | s << 16 | d   # orr wd, wzr, ws
def b_lt(frm, to): return 0x54000000 | (((to - frm) // 4) & 0x7ffff) << 5 | 0xb


HEAD = [0x29402e88,   # ldp w8, w11, [x20]: the clamp's minimum and maximum
        0x52a84e08,   # mov w8, #0x42700000 (60.0f)
        0x1e380009]   # fcvtzs w9, s0
DKB = [mov60(8), ldr_w(9, 20, 0x24), str_w(21, 20, 0x40), sdiv(8, 8, 21), mov_w(1, 8), str_w(8, 20, 0x50), RET]   # the sdiv at word 3
OOT = [mov60(8), str_w(21, 20, 0x48), sdiv(1, 8, 21), ldr_w(8, 20, 0x2c), str_w(1, 20, 0x58), RET]   # the sdiv at word 2
SPLAT = [mov60(8), ldr_w(9, 0, 0x24), str_w(20, 0, 0x40), sdiv(2, 8, 20), str_w(2, 0, 0x50), mov_w(8, 2), RET]


def image(body, at=3, head=HEAD, second=None):
    img = bytearray(struct.pack('<I', NOP) * (TEXT // 4))
    words = head + body
    struct.pack_into(f'<{len(words)}I', img, FN, *words)
    fns = [(FN, FN + 4 * len(words))]
    if second:
        struct.pack_into(f'<{len(head) + len(second)}I', img, FN2, *(head + second)); fns.append((FN2, FN2 + 4 * (len(head) + len(second))))
    unwind(img, fns)
    return bytes(img), FN + 4 * (len(head) + at)


class SeadRate(unittest.TestCase):
    def test_forms(self):
        for body, at, regs in ((DKB, 3, (8, 21, 20)), (OOT, 2, (8, 21, 20)), (SPLAT, 3, (8, 20, 0))):
            img, site = image(body, at, head=[0x29402c08] + HEAD[1:] if regs[2] == 0 else HEAD)   # ldp w8, w11, [x0] where the controller is in x0
            off, r, why = nso_code.sead_find(img, TEXT)
            self.assertEqual((off, r), (site, regs), why)
            self.assertEqual(nso_code.sead_check(img, TEXT, site), (regs, None))
        self.assertEqual(nso_code.sead_param((8, 21, 20)), 8 | 21 << 5 | 20 << 10)

    def test_not_the_controller(self):
        cases = {
            'no clamp of the controller': (DKB, [HEAD[1], HEAD[2]]),
            'no 60.0f': (DKB, [HEAD[0], HEAD[2]]),
            'the dividend read after the sdiv': ([mov60(8), str_w(21, 20, 0x48), sdiv(1, 8, 21), str_w(8, 20, 0x2c), str_w(1, 20, 0x58), RET], HEAD),
            'a branch before the dividend is dead': ([mov60(8), str_w(21, 20, 0x48), sdiv(1, 8, 21), str_w(1, 20, 0x58), b_lt(0, 0x40), RET], HEAD),
            'no interval store': ([mov60(8), ldr_w(9, 20, 0x24), sdiv(8, 8, 21), str_w(8, 20, 0x50), RET], HEAD),
            'the rate not stored': ([mov60(8), str_w(21, 20, 0x40), sdiv(8, 8, 21), mov_w(1, 8), RET], HEAD),
            'another base between': ([mov60(8), ldr_w(9, 19, 0x24), str_w(21, 20, 0x40), sdiv(8, 8, 21), str_w(8, 20, 0x50), RET], HEAD),
            'not 60': ([0x52800700 | 8, str_w(21, 20, 0x40), sdiv(8, 8, 21), str_w(8, 20, 0x50), RET], HEAD),
        }
        for what, (body, head) in cases.items():
            img, _ = image(body, head=head)
            self.assertEqual(nso_code.sead_find(img, TEXT), (None, None, ''), what)

    def test_two_controllers(self):
        img, _ = image(DKB, second=DKB)
        off, r, why = nso_code.sead_find(img, TEXT)
        self.assertIsNone(off); self.assertTrue(why.startswith('[RED] '), why)
        img, _ = image(DKB + DKB)
        off, r, why = nso_code.sead_find(img, TEXT)
        self.assertIsNone(off); self.assertTrue(why.startswith('[RED] '), why)

    def test_check_refuses_other_words(self):
        img, site = image(DKB)
        self.assertIsNone(nso_code.sead_check(img, TEXT, site + 4)[0])
        self.assertIsNone(nso_code.sead_check(img, TEXT, site + 2)[0])


class OptIn(unittest.TestCase):
    def test_rows_only_for_profiles_that_opt_in(self):   # an OBS row of the gate in a profile without fps_decouple sead would gate a game nobody checked
        games = Path(__file__).resolve().parents[2] / 'games'
        for p in sorted(games.glob('*/patches.py')):
            src = p.read_text(encoding='utf-8')
            ns = {}; exec(compile(src, str(p), 'exec'), ns)
            rows = [oid for gates in ns.get('OBS', {}).values() for _, oid, _, _ in gates if oid & 0xff == nso_code.OBS_SEAD_RATE]
            tsv = (p.parent / 'game.tsv').read_text(encoding='utf-8').splitlines()
            if rows: self.assertIn('fps_decouple\tsead', [l.strip() for l in tsv], p)


if __name__ == '__main__':
    unittest.main()
