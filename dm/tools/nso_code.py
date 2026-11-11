#!/usr/bin/env python3
"""Where an NSO's text segment holds code: function ranges from .eh_frame, and data blocks in the text that no function covers.
patch_nso.py and insn_census.py import it. Also sead's frame-rate controller, found by its code (sead_find, at the end). Usage on its own: nso_code.py <module.nso | decompressed image .bin> (lists the data blocks)

A data block is a stretch of at least DATA_MIN bytes between FDE ranges in which more than a quarter of the words do not decode as instructions. Absolum's main
(.NET NativeAOT on SDK 18) starts its text with 1.7 MB of data that no code references (no adrp, no relocation): 62% of its words do not decode, and 6,858 of the
rest read as instructions that use x18, which a linear sweep would rewrite. Code without unwind information (zlib, libunwind's register restore) stays code: it decodes."""
import struct

DATA_MIN = 0x1000


def eh_frame_hdr(img):
    """(eh_frame_hdr offset, its end) from the MOD0 header"""
    m0 = struct.unpack_from('<I', img, 4)[0]
    assert img[m0:m0 + 4] == b'MOD0', 'no MOD0'
    return m0 + struct.unpack_from('<i', img, m0 + 16)[0], m0 + struct.unpack_from('<i', img, m0 + 20)[0]


def _read(img, o, enc, ehs):
    fmt, app = enc & 0xF, enc & 0x70
    if fmt == 0x3: v, sz = struct.unpack_from('<I', img, o)[0], 4
    elif fmt == 0xB: v, sz = struct.unpack_from('<i', img, o)[0], 4
    elif fmt in (0x0, 0x4): v, sz = struct.unpack_from('<Q', img, o)[0], 8
    elif fmt == 0xC: v, sz = struct.unpack_from('<q', img, o)[0], 8
    else: raise ValueError(f'pointer encoding {enc:#x} at {o:#x}')
    if app == 0x10: v += o
    elif app == 0x30: v += ehs
    return v, sz


def _uleb(img, o):
    r = sh = 0
    while True:
        b = img[o]; o += 1; r |= (b & 0x7F) << sh; sh += 7
        if not b & 0x80: return r, o


def fde_table(img):
    """[(function start, FDE offset)] from the eh_frame_hdr binary search table; empty when the module has no valid eh_frame_hdr (New Super Mario Bros. U Deluxe: a 32-bit module)"""
    ehs, ehe = eh_frame_hdr(img)
    if ehe - ehs < 12 or img[ehs] != 1 or img[ehs + 3] in (0xFF, 0): return []
    _, penc, cenc, tenc = img[ehs:ehs + 4]
    o = ehs + 4
    _, s = _read(img, o, penc, ehs); o += s
    n, s = _read(img, o, cenc, ehs); o += s
    out = []
    for _ in range(n):
        a, s = _read(img, o, tenc, ehs); o += s
        f, s = _read(img, o, tenc, ehs); o += s
        out.append((a, f))
    return out


def fde_ranges(img):
    """Sorted [(start, end)] of every function the FDEs describe (pc begin and pc range, in the encoding the FDE's CIE gives)"""
    ehs, _ = eh_frame_hdr(img)
    cies = {}

    def cie_encoding(cie):
        if cie not in cies:
            p = cie + 8; ver = img[p]; p += 1
            aug = img[p:img.index(b'\0', p)]; p += len(aug) + 1
            _, p = _uleb(img, p)                                  # code alignment
            while img[p] & 0x80: p += 1                           # data alignment (sleb)
            p += 1
            p = p + 1 if ver == 1 else _uleb(img, p)[1]           # return address register
            enc = 0
            if aug.startswith(b'z'):
                _, p = _uleb(img, p)
                for ch in aug[1:]:
                    if ch == ord('R'): enc = img[p]; p += 1
                    elif ch == ord('P'): pe = img[p]; p += 1; p += _read(img, p, pe, ehs)[1]
                    elif ch == ord('L'): p += 1
            cies[cie] = enc
        return cies[cie]

    out = []
    for _, f in fde_table(img):
        cptr = f + 4
        enc = cie_encoding(cptr - struct.unpack_from('<I', img, cptr)[0])
        st, s = _read(img, f + 8, enc, ehs)
        ln, _ = _read(img, f + 8 + s, enc & 0xF, ehs)
        out.append((st, st + ln))
    return sorted(out)


def data_blocks(img, text_end, md):
    """Sorted [(start, end)] of the data blocks in text [0, text_end) (see the module docstring). md: a capstone AArch64 disassembler"""
    # An NRO's text includes its header: the build ID at +0x40 is arbitrary bytes, not instructions. FFX/X-2's magic_0898_2015.nro has a literal ldr x18 there.
    # Keep the entry branch and MOD0 pointer (+0..+8), but never scan the reserved word or NRO0 header (+8..+0x80), even when this gap is shorter than DATA_MIN.
    nro = img[0x10:0x14] == b'NRO0'
    gaps, last = [], 0x80 if nro else 0
    for s, e in fde_ranges(img):
        if s >= text_end: break
        if s > last: gaps.append((last, s))
        last = max(last, e)
    if last < text_end: gaps.append((last, text_end))
    out = [(8, 0x80)] if nro else []
    for s, e in gaps:
        s, e = (s + 3) & ~3, e & ~3
        if e - s < DATA_MIN: continue
        undecodable = sum(1 for a in range(s, e, 4) if next(md.disasm_lite(bytes(img[a:a + 4]), a), None) is None)
        if undecodable * 4 > (e - s) // 4: out.append((s, e))
    return out


if __name__ == '__main__':
    import sys
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
    p = sys.argv[1]; raw = open(p, 'rb').read()
    if raw[:4] == b'NSO0':
        import nso
        try: m = nso.load(raw, p)
        except nso.Error as e: sys.exit(f'[RED] {e}')
        img, text_end = m.image, m.text_end
    else:
        img = raw; text_end = int(sys.argv[2], 0)
    blocks = data_blocks(img, text_end, Cs(CS_ARCH_ARM64, CS_MODE_ARM))
    print(f'{p}: {len(blocks)} data blocks in text, {sum(e - s for s, e in blocks)} bytes')
    for s, e in blocks: print(f'  {s:#x}..{e:#x} ({e - s} bytes)')


def out_store(img, start, size, md):
    """(offset, register) of the one store through a function's second argument, an out pointer (x1, copied once in the prologue: mov xB, x1 ... str xA, [xB]):
    where the SDK's VammManager::AllocateAddressRegion hands the new region's address back (patch_nso.py "Address regions"). ValueError when that shape is not there"""
    import re
    ins = list(md.disasm(bytes(img[start:start + size]), start))
    out = [i for i in ins[:16] if i.mnemonic == 'mov' and re.fullmatch(r'x\d+, x1', i.op_str)]
    if len(out) != 1: raise ValueError('no single copy of the out pointer in the prologue')
    xb = out[0].op_str.split(',')[0]
    st = [i for i in ins if i.mnemonic == 'str' and re.fullmatch(rf'x\d+, \[{xb}\]', i.op_str)]
    if len(st) != 1: raise ValueError(f'{len(st)} stores through the out pointer')
    return st[0].address, int(st[0].op_str.split(',')[0][1:])

# ---- sead's frame-rate controller (patch_nso.py "sead's frame-rate controller") ----
# sead's dynamic present interval controller in main, found by its own code, without a build ID: for a game whose profile opts in (fps_decouple sead) patch_nso.py puts
# observation gate OBS_SEAD_RATE on the one instruction where
# the controller turns the interval it picked into the frame rate it hands the framework (60 / interval, an sdiv), and the host makes that the measured frame rate there
# (dm/host/hle_fpslock.c "sead_rate"): the game steps its world by that rate, so with it the game keeps its speed at any frame rate (docs/reference/frame-rate.md, "A game whose
# speed follows its present interval runs slow when frames come late").
#
# The controller (Donkey Kong Bananza v458752 main+0x204a18, called once a frame from the framework's main+0x203674 with the controller in x0 and the framework in x1): each frame it takes the last frame's time in vsyncs (ticks / frequency * 60.0f,
# plus a bias), picks the interval from it (fcvtzs, + 1), clamps it to the two int32 at the controller's start (ldp wMin, wMax, [xC]), stores it in the controller, then
#     mov  wN, #60            (straight-line, at most four instructions before the sdiv; what lies between only loads or stores through xC)
#     ...  str wM, [xC, #i]   (the interval into the controller)
#     sdiv wD, wN, wM         (the frame rate: the gate goes here)
#     ...  str wD, [xC, #r]   (the rate into the controller, within four straight-line instructions, wD not written before it; when D is not N, wN is written again
#                              before anything reads it)
# and hands the rate to the framework. A site has all of that (sead_site()); the function around it, as the FDEs give it, also has the clamp's ldp of [xC] at offset 0, the
# 60.0f constant (movz wX, #0x4270, lsl #16) and an fcvtzs of a float into a w register (sead_controller()). Exactly one function of main may have exactly one such site, else
# nothing is gated. Found (each checked by disassembly, 2026-11-10): Donkey Kong Bananza v0 (main+0x1eadf0) and v458752 (main+0x204b20), The Legend of Zelda: Ocarina of
# Time v65536 (main+0x3af19ec) and v131072 (main+0x3b4f05c), Splatoon Raiders (main+0x3df0e28). Not in Mario Kart World 1.8.0, whose framework has no dynamic interval
# controller (a fixed interval, framework+0x88, and its own wait), nor in Mario Tennis Fever, the Welcome Tour or Pokemon Pokopia (KTGL). Only a profile that opts in gets
# the gate: a game's speed follows the rate only if its framework steps by it, which is checked per game (Ocarina of Time's turns it back into a present interval and sets
# its own rate from that: docs/reference/frame-rate.md). For the others the rewriter logs the site and leaves the code as it is.
#
# A build's own OBS row for the gate (the profile's patches.py: OBS = {build ID: [(offset, OBS_SEAD_RATE | sead_param() << 8, the sdiv's word, why)]}) names the site for that build: the
# place the gate goes, after sead_site()'s checks (sead_check()), whether the search finds it, another place or none (an override, and a fallback for a build whose function the search
# does not take; Donkey Kong Bananza v458752 has one; the search covers its other builds). Each OBS row is in the pack's rewrite key, as patches.py is.
#
# The gate passes the registers in its number's upper bits (N, M, C, five bits each: sead_param()); the callback writes N = rate * M, so the sdiv yields the rate (wN is the
# dividend, and dead after the sdiv unless it is also its destination, so the write changes nothing else). Two forms: sdiv w8, w8, w21 (Donkey Kong Bananza), and the quotient in
# another register, sdiv w1, w8, w21 then ldr w8 (The Legend of Zelda: Ocarina of Time) or sdiv w2, w8, w20 then mov w8, w2 (Splatoon Raiders).
OBS_SEAD_RATE = 27   # dm/host/dm.h

SDIV_W, SDIV_M = 0x1AC00C00, 0xFFE0FC00          # sdiv wd, wn, wm
MOV60 = 0x52800780                               # movz wd, #60
STRW, LDRW, UIMM_M = 0xB9000000, 0xB9400000, 0xFFC00000   # str / ldr wt, [xn, #imm12 * 4]
LDPW0_M, LDPW0 = 0xFFFF8000, 0x29400000         # ldp wt, wt2, [xn] (signed offset 0)
F60, F60_M = 0x52A84E00, 0xFFFFFFE0              # movz wd, #0x4270, lsl #16 (60.0f)
FCVTZS_WS, FCVTZS_M = 0x1E380000, 0xFFFFFC00     # fcvtzs wd, sn


def _sead_rn(w): return (w >> 5) & 31


def _sead_md():
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM); md.detail = True
    return md


def _sead_regs(ins, which):
    """The x-register numbers (w and x alike) an instruction reads (0) or writes (1)"""
    out = set()
    for r in ins.regs_access()[which]:
        nm = ins.reg_name(r)
        if nm[0] in 'wx' and nm[1:].isdigit(): out.add(int(nm[1:]))
    return out


def sead_site(w, i, md=None):
    """(N, M, C) when word index i is the controller's sdiv with its surroundings (the comment above), else None. w: the text's words"""
    v = w[i]
    if v & SDIV_M != SDIV_W: return None
    d, n, m = v & 31, _sead_rn(v), (v >> 16) & 31
    if m == n or n > 28 or m > 28 or d > 28: return None
    base = interval = None
    for k in range(i - 1, max(i - 5, -1), -1):   # back to mov wN, #60 through loads and stores of the controller only
        u = w[k]
        if u == MOV60 | n: break
        if u & UIMM_M not in (STRW, LDRW): return None
        if base is None: base = _sead_rn(u)
        if _sead_rn(u) != base or (u & UIMM_M == LDRW and u & 31 in (n, m, base)): return None
        if u & UIMM_M == STRW and u & 31 == m: interval = k
    else: return None
    if base is None or interval is None or base in (n, m, d) or base > 28: return None
    # After it, straight-line: the rate (wD) into the controller before anything writes wD, and wN, when it is not wD, written again before anything reads it (the callback's
    # write to it then changes nothing but the quotient)
    md = md or _sead_md()
    stored, dead = False, d == n
    for k in range(i + 1, min(i + 5, len(w))):
        ins = next(md.disasm(struct.pack('<I', w[k]), k * 4), None)
        if ins is None: return None
        u = w[k]
        if u & UIMM_M == STRW and u & 31 == d and _sead_rn(u) == base: stored = True
        rd, wr = _sead_regs(ins, 0), _sead_regs(ins, 1)
        if not dead and n in rd: return None
        if not dead and n in wr: dead = True
        if d in wr and not stored: return None
        if stored and dead: return n, m, base
        if any(ins.group(g) for g in (1, 2, 3, 7)): return None   # a jump, a call or a return: the window ends
    return None


def sead_controller(w, lo, hi, base):
    """Whether the function [lo, hi) (word indices) has the clamp's ldp of [xC], the 60.0f constant and an fcvtzs into a w register"""
    seg = w[lo:hi]
    return (any(u & LDPW0_M == LDPW0 and _sead_rn(u) == base for u in seg) and any(u & F60_M == F60 for u in seg)
            and any(u & FCVTZS_M == FCVTZS_WS for u in seg))


def sead_param(regs):
    n, m, c = regs
    return n | m << 5 | c << 10


def sead_check(img, text_end, off):
    """sead_site()'s registers for a given offset (a profile row), or a reason it is not the controller's sdiv"""
    if off % 4 or not 0 <= off < text_end: return None, f'{off:#x} is not in the text'
    w = struct.unpack_from(f'<{text_end // 4}I', img, 0)
    r = sead_site(w, off // 4)
    return (r, None) if r else (None, f'{off:#x} ({w[off // 4]:08x}) is not the controller\'s sdiv of 60 by its interval')


def sead_find(img, text_end):
    """(offset, (N, M, C), why): the controller's sdiv, or None and why not (a why starting [RED] when the search found a controller it could not take; '' for a main without one)"""
    import bisect
    w = struct.unpack_from(f'<{text_end // 4}I', img, 0)
    try: fns = fde_ranges(img)
    except (AssertionError, ValueError, struct.error, IndexError): return None, None, ''   # no unwind table: no function bounds to check a controller by
    starts = [s for s, _ in fns]
    hits, md = {}, _sead_md()
    for i, v in enumerate(w):
        if v & SDIV_M != SDIV_W: continue
        r = sead_site(w, i, md)
        if not r: continue
        j = bisect.bisect_right(starts, i * 4) - 1
        if j < 0 or i * 4 >= fns[j][1]: continue
        lo, hi = fns[j]
        if sead_controller(w, lo // 4, hi // 4, r[2]): hits.setdefault(lo, []).append((i * 4, r))
    if not hits: return None, None, ''
    if len(hits) > 1: return None, None, '[RED] sead\'s frame-rate controller: ' + ', '.join(f'{f:#x}' for f in hits) + ' all look like it'
    (fn, sites), = hits.items()
    if len(sites) > 1: return None, None, f'[RED] sead\'s frame-rate controller at {fn:#x}: ' + ', '.join(f'{o:#x}' for o, _ in sites) + ' all look like its sdiv'
    off, r = sites[0]
    return off, r, f'sead\'s frame-rate controller at {fn:#x}: 60 / interval at {off:#x} (w{r[0]} / w{r[1]}, the controller in x{r[2]})'
