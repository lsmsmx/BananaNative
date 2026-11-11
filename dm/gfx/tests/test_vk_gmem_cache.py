"""The Vulkan library's cache_raw_loads (vk_android_spv.py): the GlobalToStorage raw load helpers' calls read through the pool table entry kept last.

The helper is shaped as the translator's since a319e773 (the entry chosen last, the 2 MiB chunk's hint, the 13-step search, the inPool check, the access through a
physical pointer: test_vk_gmem_helpers.py), after promote_locals and loop_pool_search as vk_android_build.py runs them. main reads the base address from io and
loads words at base + 0, 4, 8, 12 (the high word with the translator's carry), then one word after a store to io (a run ends there), then a single word and a run
of two at another address. cache_raw_loads makes the 4-word run one call, the 2-word run one call, keeps the store between the runs and leaves the store helper alone.

A small SPIR-V interpreter runs main before and after over a table of 300 pools (test_vk_gmem_helpers.pool_table: entries past 64, hints for the 2 MiB chunks a pool
covers whole) for addresses at a pool's start, middle and end, runs that leave their pool after 1, 2 or 3 words (the rest in no pool, or in the next pool when two
pools touch), addresses in no pool (0 and the sink), a chunk a pool covers only partly (no hint: the search), and the same pool again (the kept entry): every word
read and written is the same."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[0] / 'tools'))
sys.path.insert(0, str(HERE))
import vk_android_spv as spv  # noqa: E402
from test_vk_gmem_helpers import ADDR, FIND_NEW, LOG, pool_table, word_at  # noqa: E402

SHADER = '''#version 450
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
layout(local_size_x = 1) in;
layout(set = 0, binding = 0, std430) readonly buffer Map { uint w[]; } map;
layout(set = 0, binding = 1, std430) buffer Log { uint w[]; } glog;
layout(set = 0, binding = 2, std430) buffer Io { uint w[]; } io;
layout(buffer_reference, std430, buffer_reference_align = 4) buffer Word { uint v; };
uint g_last = 0u;

void gstore(uint lo, uint hi, uint v)
{
@STORE@
@LOG@
@ADDR@
        Word(packUint2x32(uvec2(hl, hh))).v = v;
}

uint gload(uint lo, uint hi)
{
@LOAD@
@ADDR@
        return Word(packUint2x32(uvec2(hl, hh))).v;
}

uint plus1(uint lo, uint hi)   // not a raw load helper (its value is not the word's): left alone
{
    return gload(lo, hi) + 1u;
}

void main()
{
    uint lo = io.w[0], hi = io.w[1];
    uint lo1 = lo + 4u, hi1 = hi + (lo1 < lo ? 1u : 0u);
    uint lo2 = lo + 8u, hi2 = hi + (lo2 < lo ? 1u : 0u);
    uint lo3 = lo + 12u, hi3 = hi + (lo3 < lo ? 1u : 0u);
    uint a0 = gload(lo, hi), a1 = gload(lo1, hi1), a2 = gload(lo2, hi2), a3 = gload(lo3, hi3);
    io.w[8] = a0; io.w[9] = a1; io.w[10] = a2; io.w[11] = a3;
    uint lo4 = lo + 16u, hi4 = hi + (lo4 < lo ? 1u : 0u);
    io.w[12] = gload(lo4, hi4);
    uint blo = io.w[2], bhi = io.w[3];
    uint blo1 = blo + 4u, bhi1 = bhi + (blo1 < blo ? 1u : 0u);
    uint b0 = gload(blo, bhi), b1 = gload(blo1, bhi1);
    io.w[13] = b0; io.w[14] = b1;
    io.w[15] = gload(lo, hi) ^ plus1(blo, bhi);
    gstore(io.w[4], io.w[5], 7u);
}
'''.replace('@STORE@', FIND_NEW.replace('@RET@', '')).replace('@LOAD@', FIND_NEW.replace('@RET@', ' 0u')).replace('@LOG@', LOG).replace('@ADDR@', ADDR)

M = 0xffffffff


def sg(v): return v - (1 << 32) if v & 0x80000000 else v


BIN = {128: lambda a, b: (a + b) & M, 130: lambda a, b: (a - b) & M, 132: lambda a, b: (a * b) & M, 194: lambda a, b: a >> b, 196: lambda a, b: (a << b) & M,
       197: lambda a, b: a | b, 198: lambda a, b: a ^ b, 199: lambda a, b: a & b, 170: lambda a, b: a == b, 171: lambda a, b: a != b, 176: lambda a, b: a < b,
       178: lambda a, b: a <= b, 177: lambda a, b: sg(a) < sg(b), 179: lambda a, b: sg(a) <= sg(b), 172: lambda a, b: a > b, 173: lambda a, b: sg(a) > sg(b),
       174: lambda a, b: a >= b, 175: lambda a, b: sg(a) >= sg(b), 166: lambda a, b: a or b, 167: lambda a, b: a and b, 164: lambda a, b: a == b, 165: lambda a, b: a != b}


def run(m, table, io):
    """main of the module run once by a small SPIR-V interpreter over the pool table (binding 0) and io (binding 2, a dict of words): (io after, physical stores)"""
    res = {v['binding']: v['id'] for v in m.resources()}
    mapvar, logvar, iovar = res[0], res[1], res[2]
    io, phys_stores = dict(io), {}
    glob = {i: (v[0] if len(v) == 1 else v) for i, (_, v) in m.consts.items()}
    for x in m.ins:
        op = x[0] & 0xffff
        if op in (41, 42): glob[x[2]] = op == 41   # OpConstantTrue / False
        elif op == spv.OP_CONSTANT_NULL: glob[x[2]] = (0, 0, 0, 0) if m.op(x[1]) == spv.OP_TYPE_VECTOR else 0
        elif op == spv.OP_CONSTANT_COMPOSITE: glob[x[2]] = tuple(glob[c] for c in x[3:])
    mem = {}
    for x in m.ins:
        if x[0] & 0xffff == spv.OP_VARIABLE and x[3] == spv.SC_PRIVATE:
            glob[x[2]] = ('v', x[2]); mem[('v', x[2])] = glob[x[4]] if len(x) > 4 else 0
        if x[0] & 0xffff == spv.OP_FUNCTION: break
    steps = [0]

    def call(f, args):
        a, e = m.function_range(f)
        body = m.ins[a:e + 1]
        val = dict(glob)
        labels = {x[1]: k for k, x in enumerate(body) if x[0] & 0xffff == spv.OP_LABEL}
        for x, v in zip([x for x in body if x[0] & 0xffff == spv.OP_FUNCTION_PARAMETER], args):
            if m.op(x[1]) == spv.OP_TYPE_POINTER: val[x[2]] = v
            else: val[x[2]] = v
        k = next(i for i, x in enumerate(body) if x[0] & 0xffff == spv.OP_LABEL)
        blk = prev = None
        while True:
            steps[0] += 1
            assert steps[0] < 2_000_000, 'interpreter: too many steps'
            x = body[k]; op = x[0] & 0xffff; k += 1
            if op == spv.OP_LABEL: prev, blk = blk, x[1]; continue
            if op in (spv.OP_SELECTION_MERGE, spv.OP_LOOP_MERGE): continue
            if op == spv.OP_PHI: val[x[2]] = next(val[v] for v, p in zip(x[3::2], x[4::2]) if p == prev); continue
            if op == spv.OP_VARIABLE: val[x[2]] = ('v', (x[2], steps[0])); mem[val[x[2]]] = val.get(x[4]) if len(x) > 4 else 0; continue
            if op == spv.OP_BRANCH: k = labels[x[1]]; continue
            if op == spv.OP_BRANCH_CONDITIONAL: k = labels[x[2] if val[x[1]] else x[3]]; continue
            if op == spv.OP_RETURN_VALUE: return val[x[1]]
            if op == spv.OP_RETURN: return None
            if op == spv.OP_STORE:
                p = val[x[1]]
                if p[0] == 'io': io[p[1]] = val[x[2]]
                elif p[0] == 'phys': phys_stores[p[1]] = val[x[2]]
                elif p[0] == 'log': pass
                else: mem[p] = val[x[2]]
                continue
            if op in (8, 317): continue   # OpLine, OpNoLine
            r = x[2]
            if op == spv.OP_LOAD:
                p = val[x[3]]
                val[r] = (table[p[1]] if p[0] == 'map' else io.get(p[1], 0) if p[0] == 'io' else word_at(p[1]) if p[0] == 'phys' else 0 if p[0] == 'log' else mem[p])
            elif op == spv.OP_ACCESS_CHAIN:
                b = val.get(x[3])
                if isinstance(b, tuple) and b[0] == 'phys': val[r] = b   # Word(p).v
                elif x[3] == mapvar: val[r] = ('map', val[x[5]])
                elif x[3] == iovar: val[r] = ('io', val[x[5]])
                elif x[3] == logvar: val[r] = ('log', val[x[5]])
                else: raise AssertionError(f'interpreter: access chain into {x[3]}')
            elif op == spv.OP_FUNCTION_CALL: val[r] = call(x[3], [val[i] for i in x[4:]])
            elif op in (spv.OP_BITCAST, spv.OP_COPY_OBJECT):
                v = val[x[3]]
                val[r] = v[0] | v[1] << 32 if isinstance(v, tuple) and m.op(x[1]) == spv.OP_TYPE_INT else v
            elif op == spv.OP_CONVERT_U_TO_PTR: val[r] = ('phys', val[x[3]])
            elif op == spv.OP_COMPOSITE_CONSTRUCT: val[r] = tuple(val[i] for i in x[3:])
            elif op == spv.OP_COMPOSITE_EXTRACT:
                v = val[x[3]]
                for i in x[4:]: v = v[i]
                val[r] = v
            elif op == 78:   # OpVectorInsertDynamic
                v = list(val[x[3]]); v[val[x[5]]] = val[x[4]]; val[r] = tuple(v)
            elif op == 154: val[r] = any(val[x[3]])   # OpAny
            elif op == 155: val[r] = all(val[x[3]])   # OpAll
            elif op == 168: val[r] = not val[x[3]]   # OpLogicalNot
            elif op == spv.OP_SELECT: val[r] = val[x[4]] if val[x[3]] else val[x[5]]
            elif op in (227, 241):   # the log's atomic load / or (no effect on what is read)
                val[r] = 0
            elif op in BIN: val[r] = BIN[op](val[x[3]], val[x[4]])
            else: raise AssertionError(f'interpreter: opcode {op}')
    call(m.entry_function(), [])
    return io, phys_stores


def calls(m):
    """{callee: number of calls} over the module"""
    out = {}
    for x in m.ins:
        if x[0] & 0xffff == spv.OP_FUNCTION_CALL: out[x[3]] = out.get(x[3], 0) + 1
    return out


@unittest.skipUnless(shutil.which('glslangValidator') and shutil.which('spirv-val'), 'needs glslang and SPIRV-Tools')
class GmemCache(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with tempfile.TemporaryDirectory() as t:
            d = Path(t)
            (d / 's.comp').write_text(SHADER)
            r = subprocess.run(['glslangValidator', '-V', '--target-env', 'vulkan1.2', str(d / 's.comp'), '-o', str(d / 's.spv')], capture_output=True, text=True)
            assert r.returncode == 0, r.stdout + r.stderr
            m = spv.Module((d / 's.spv').read_bytes())
            spv.promote_locals(m)
            spv.loop_pool_search(m)
            cls.before = spv.Module(m.data())
            cls.table_id = next(v['id'] for v in m.resources() if v['binding'] == 0)
            cls.n = spv.cache_raw_loads(m, {cls.table_id})
            (d / 'r.spv').write_bytes(m.data())
            cls.val = subprocess.run(['spirv-val', '--target-env', 'vulkan1.3', str(d / 'r.spv')], capture_output=True, text=True)
            cls.after = spv.Module(m.data())

    def test_valid(self):
        self.assertEqual(self.val.returncode, 0, self.val.stdout + self.val.stderr)

    def test_runs(self):
        """the 4-word run is one call, the single word after the store one, the 2-word run one, the word read again one; plus1's call stays (its value is not the
        word's, so plus1 is no raw load helper, and its own call is a single); the store helper and its call stay"""
        names = {v: k for k, v in self.before.names.items()}
        gload, gstore = names['gload(u1;u1;'], names['gstore(u1;u1;u1;']
        self.assertEqual(calls(self.before)[gload], 9)
        self.assertEqual(self.n, 9, 'every load call replaced')
        c = calls(self.after)
        self.assertNotIn(gload, c, 'gload has no caller left')
        self.assertNotIn(gload, [x[2] for x in self.after.ins if x[0] & 0xffff == spv.OP_FUNCTION], 'and is gone')
        self.assertEqual(c.get(gstore), 1, 'the store helper is called as before')
        new = {f: k for f, k in c.items() if f not in self.before.names}
        self.assertEqual(sorted(new.values()), [1, 1, 3], 'one call of the 4- and of the 2-word function, three of the 1-word one (one in plus1)')
        self.assertIn(spv.OP_CONVERT_U_TO_PTR, [x[0] & 0xffff for x in self.after.ins], 'the store helper keeps its access')

    def test_store_order(self):
        """the run's word stores (io 8..11) come after its one call, and the next load (io 12) after them: no load moved over a store"""
        m = self.after
        a, e = m.function_range(m.entry_function())
        body = m.ins[a:e + 1]
        iovar = next(v['id'] for v in m.resources() if v['binding'] == 2)
        chains = {x[2]: m.consts[x[5]][1][0] for x in body if x[0] & 0xffff == spv.OP_ACCESS_CHAIN and x[3] == iovar and len(x) == 6 and x[5] in m.consts}
        at = {chains[x[1]]: k for k, x in enumerate(body) if x[0] & 0xffff == spv.OP_STORE and x[1] in chains}
        k_call = [k for k, x in enumerate(body) if x[0] & 0xffff == spv.OP_FUNCTION_CALL]
        self.assertLess(k_call[0], at[8])
        self.assertTrue(any(at[11] < k < at[12] for k in k_call), 'the load of io 12 after the stores of the run')

    def test_same_words(self):
        table, pools, sink = pool_table(300)
        hinted = lambda a: table[((a >> 21) & 8191) * 8 + 6] >> 13 == (a >> 21) & 0x7ffff and table[((a >> 21) & 8191) * 8 + 6] & 8191
        # two pools that touch: pool 151 ends where pool 152 starts (a run that leaves one pool goes on in the next)
        s151, e151, d151 = pools[150]; s152, e152, d152 = pools[151]
        e2 = s152; table[151 * 8 + 2:151 * 8 + 4] = [e2 & M, e2 >> 32]; pools[150] = (s151, e2, d151)
        addrs = [(0, 0), (sink & M, sink >> 32), (0x0f_ffff_fff0 & M, 0x0f)]
        for p in (1, 2, 63, 64, 65, 100, 151, 200, 299, 300):
            s_, e_, _ = pools[p - 1]
            for a in (s_, s_ + 4, (s_ + e_) // 2 & ~3, e_ - 16, e_ - 12, e_ - 8, e_ - 4, e_, s_ - 4):
                addrs.append((a & M, a >> 32))
        part = next(a for a in range(pools[199][0], pools[199][1], 0x10000) if not hinted(a))
        addrs.append((part & M, part >> 32))
        self.assertTrue(any(hinted(lo | hi << 32) for lo, hi in addrs) and not hinted(part), 'some addresses by the hint, one by the search')
        n, kinds = 0, set()
        for i, (lo, hi) in enumerate(addrs):
            blo, bhi = addrs[(i * 7 + 3) % len(addrs)]
            for second in ((blo, bhi), (lo, hi)):   # another pool, the same one (the kept entry)
                io = {0: lo, 1: hi, 2: second[0], 3: second[1], 4: lo, 5: hi}
                want = run(self.before, table, io)
                got = run(self.after, table, io)
                self.assertEqual(got, want, f'base {hi:#x}:{lo:#x}, second {second[1]:#x}:{second[0]:#x}')
                n += 1
                a = lo | hi << 32
                where = [next((j for j, (s_, e_, dl) in enumerate(pools) if s_ <= a + 4 * k < e_), None) for k in range(4)]
                kinds.add('in' if len(set(where)) == 1 and where[0] is not None else 'none' if set(where) == {None} else 'two pools' if None not in where else 'leaves')
                for k in range(4):
                    w = next((word_at(a + 4 * k + dl) for s_, e_, dl in pools if s_ <= a + 4 * k < e_), 0)
                    self.assertEqual(got[0][8 + k], w, f'word {k} of {a:#x}: the pool\'s, or 0 in no pool')
        self.assertGreater(n, 150)
        self.assertEqual(kinds, {'in', 'none', 'two pools', 'leaves'}, 'runs inside a pool, in none, across two touching pools, leaving a pool')


if __name__ == '__main__':
    unittest.main()
