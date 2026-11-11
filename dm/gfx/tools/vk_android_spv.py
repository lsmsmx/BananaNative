"""SPIR-V reading and the few rewrites the Android Vulkan library needs (vk_android_build.py): the descriptor set / binding renumbering into the Vulkan ABI
(dm/gfx/nvn/dmsh_abi_vk.h), vertex outputs that start as (0, 0, 0, 1), the GL depth and alpha test constants, the integer-output and dual-source variants, the
viewport mask of an emulated passthrough geometry shader, and the facts the reflection reads (resources, vertex inputs, colour outputs, capabilities). The translator's modules are taken as they come; every rewrite keeps the module valid for spirv-val --target-env vulkan1.3."""
import collections, struct

MAGIC = 0x07230203
# opcodes
OP_NAME, OP_EXTENSION, OP_EXT_INST_IMPORT, OP_EXT_INST, OP_ENTRY_POINT, OP_EXECUTION_MODE, OP_CAPABILITY = 5, 10, 11, 12, 15, 16, 17
OP_TYPE_VOID, OP_TYPE_BOOL, OP_TYPE_INT, OP_TYPE_FLOAT, OP_TYPE_VECTOR, OP_TYPE_IMAGE, OP_TYPE_SAMPLER, OP_TYPE_SAMPLED_IMAGE = 19, 20, 21, 22, 23, 25, 26, 27
OP_TYPE_ARRAY, OP_TYPE_RUNTIME_ARRAY, OP_TYPE_STRUCT, OP_TYPE_POINTER = 28, 29, 30, 32
OP_CONSTANT, OP_CONSTANT_COMPOSITE, OP_FUNCTION, OP_FUNCTION_END, OP_VARIABLE, OP_LOAD, OP_STORE = 43, 44, 54, 56, 59, 61, 62
OP_ACCESS_CHAIN, OP_IN_BOUNDS_ACCESS_CHAIN, OP_DECORATE, OP_MEMBER_DECORATE = 65, 66, 71, 72
OP_COMPOSITE_EXTRACT, OP_COMPOSITE_INSERT, OP_COPY_OBJECT, OP_BITCAST, OP_FADD, OP_FMUL = 81, 82, 83, 124, 129, 133
OP_EMIT_VERTEX, OP_EMIT_STREAM_VERTEX, OP_LABEL, OP_RETURN, OP_RETURN_VALUE, OP_IMAGE_TEXEL_POINTER = 218, 220, 248, 253, 254, 60
OP_SPEC_CONSTANT_FALSE, OP_SELECT, OP_IMAGE_READ, OP_IMAGE_WRITE = 49, 169, 98, 99
# decorations, storage classes, builtins
DEC_SPEC_ID, DEC_BLOCK, DEC_BUFFER_BLOCK, DEC_BUILTIN, DEC_LOCATION, DEC_BINDING, DEC_SET = 1, 2, 3, 11, 30, 33, 34
SC_UNIFORM_CONSTANT, SC_INPUT, SC_UNIFORM, SC_OUTPUT, SC_FUNCTION, SC_PUSH_CONSTANT, SC_STORAGE_BUFFER = 0, 1, 2, 3, 7, 9, 12
BUILTIN_POSITION = 0
# VkDescriptorType
VK_SAMPLER, VK_COMBINED, VK_SAMPLED_IMAGE, VK_STORAGE_IMAGE, VK_UNIFORM_TEXEL, VK_STORAGE_TEXEL, VK_UNIFORM_BUFFER, VK_STORAGE_BUFFER = 0, 1, 2, 3, 4, 5, 6, 7
DIM_BUFFER = 5
EXEC_MODELS = {0: 'vs', 1: 'hs', 2: 'ds', 3: 'gs', 4: 'ps', 5: 'cs', 5364: 'ts', 5365: 'ms'}
CAPS = {1: 'Shader', 2: 'Geometry', 3: 'Tessellation', 25: 'ImageGatherExtended', 27: 'StorageImageMultisample', 34: 'ImageCubeArray', 35: 'SampleRateShading',
        43: 'Sampled1D', 44: 'Image1D', 45: 'SampledCubeArray', 46: 'SampledBuffer', 47: 'ImageBuffer', 48: 'ImageMSArray', 49: 'StorageImageExtendedFormats', 50: 'ImageQuery',
        51: 'DerivativeControl', 52: 'InterpolationFunction', 54: 'GeometryStreams', 4427: 'DrawParameters', 9: 'Float16', 10: 'Float64', 11: 'Int64', 12: 'Int64Atomics', 22: 'Int16', 39: 'Int8', 32: 'ClipDistance', 33: 'CullDistance', 53: 'TransformFeedback',
        55: 'StorageImageReadWithoutFormat', 56: 'StorageImageWriteWithoutFormat', 57: 'MultiViewport', 61: 'GroupNonUniform', 62: 'GroupNonUniformVote',
        63: 'GroupNonUniformArithmetic', 64: 'GroupNonUniformBallot', 65: 'GroupNonUniformShuffle', 66: 'GroupNonUniformShuffleRelative', 67: 'GroupNonUniformClustered',
        68: 'GroupNonUniformQuad', 4423: 'SubgroupBallotKHR', 4431: 'SubgroupVoteKHR', 5251: 'GeometryShaderPassthroughNV', 5254: 'ShaderViewportIndexLayerEXT',
        5255: 'ShaderViewportMaskNV', 5347: 'PhysicalStorageBufferAddresses', 5363: 'FragmentShaderSampleInterlockEXT', 5372: 'FragmentShaderShadingRateInterlockEXT',
        5378: 'FragmentShaderPixelInterlockEXT', 5379: 'DemoteToHelperInvocation', 6022: 'CooperativeMatrixKHR', 6033: 'AtomicFloat32AddEXT',
        5013: 'ImageGatherBiasLodAMD', 5568: 'IntegerFunctions2INTEL', 5283: 'MeshShadingEXT'}


def string_at(words, i):
    """A nul-terminated literal string starting at words[i] → (text, index of the word after it)"""
    out = bytearray()
    while i < len(words):
        b = struct.pack('<I', words[i]); i += 1
        if 0 in b: out += b[:b.index(0)]; break
        out += b
    return out.decode('utf-8', 'replace'), i


def lit_string(text):
    b = text.encode() + b'\0'
    b += b'\0' * (-len(b) % 4)
    return list(struct.unpack('<%dI' % (len(b) // 4), b))


class Module:
    def __init__(self, data):
        if len(data) % 4 or len(data) < 20: raise ValueError('not a SPIR-V module (length)')
        w = list(struct.unpack('<%dI' % (len(data) // 4), data))
        if w[0] != MAGIC: raise ValueError('not a SPIR-V module (magic)')
        self.header, self.ins, i = w[:5], [], 5
        while i < len(w):
            n = w[i] >> 16
            if not n or i + n > len(w): raise ValueError('malformed SPIR-V instruction at word %d' % i)
            self.ins.append(w[i:i + n]); i += n
        self.index()

    def index(self):
        self.names, self.decos, self.types, self.consts, self.vars, self.caps, self.exts = {}, {}, {}, {}, {}, [], []
        self.entry = None; self.modes = []
        for x in self.ins:
            op = x[0] & 0xffff
            if op == OP_NAME: self.names[x[1]] = string_at(x, 2)[0]
            elif op == OP_DECORATE: self.decos.setdefault(x[1], {})[x[2]] = x[3:]
            elif op == OP_CAPABILITY: self.caps.append(x[1])
            elif op == OP_EXTENSION: self.exts.append(string_at(x, 1)[0])
            elif op == OP_ENTRY_POINT: self.entry = x
            elif op == OP_EXECUTION_MODE: self.modes.append(x[2:])
            elif 19 <= op <= 39: self.types[x[1]] = x
            elif op == OP_CONSTANT: self.consts[x[2]] = (x[1], x[3:])
            elif op == OP_VARIABLE and len(x) >= 4: self.vars[x[2]] = (x[1], x[3])

    def data(self):
        return struct.pack('<%dI' % (5 + sum(len(x) for x in self.ins)), *self.header, *(v for x in self.ins for v in x))

    def new_id(self):
        i = self.header[3]; self.header[3] += 1; return i

    @property
    def model(self): return EXEC_MODELS.get(self.entry[1]) if self.entry else None

    def interface(self):
        _, k = string_at(self.entry, 3)
        return self.entry[k:]

    def op(self, ident): return self.types[ident][0] & 0xffff if ident in self.types else None

    def pointee(self, ptr):   # OpTypePointer → (storage class, type)
        t = self.types[ptr]; return t[2], t[3]

    def strip_arrays(self, t):
        """(element type, descriptor count): count 0 for a runtime array"""
        count = 1
        while self.op(t) in (OP_TYPE_ARRAY, OP_TYPE_RUNTIME_ARRAY):
            x = self.types[t]
            if self.op(t) == OP_TYPE_RUNTIME_ARRAY: count = 0; t = x[2]; continue
            n = self.consts.get(x[3], (0, [0]))[1][0]
            count *= n; t = x[2]
        return t, count

    def int_type(self, t):   # (width, signed) of an OpTypeInt, or None
        x = self.types.get(t)
        return (x[2], x[3]) if x and x[0] & 0xffff == OP_TYPE_INT else None

    def scalar_of(self, t):   # the component type of a scalar or vector
        return self.types[t][2] if self.op(t) == OP_TYPE_VECTOR else t

    def find_type(self, op, *operands):
        for i, x in self.types.items():
            if x[0] & 0xffff == op and x[2:] == list(operands): return i
        return None

    def add_type(self, op, *operands):
        """An existing type with these operands, or a new one placed after the last type / constant declaration"""
        i = self.find_type(op, *operands)
        if i is not None: return i
        i = self.new_id(); x = [(2 + len(operands)) << 16 | op, i, *operands]
        self._insert_decl(x); self.types[i] = x
        return i

    def add_const(self, ty, value):
        for i, (t, v) in self.consts.items():
            if t == ty and v == [value]: return i
        i = self.new_id(); x = [4 << 16 | OP_CONSTANT, ty, i, value]
        self._insert_decl(x); self.consts[i] = (ty, [value])
        return i

    def _insert_decl(self, x):   # after the last type, constant or global variable: before the first function
        at = next(k for k, y in enumerate(self.ins) if y[0] & 0xffff == OP_FUNCTION)
        self.ins.insert(at, x)

    def resources(self):
        """Descriptor variables → [{id, name, set, binding, desc, count, dim, rw}] in declaration order"""
        out = []
        for vid, (ptr, sc) in self.vars.items():
            d = self.decos.get(vid, {})
            if DEC_BINDING not in d: continue
            _, t = self.pointee(ptr)
            elem, count = self.strip_arrays(t)
            e = {'id': vid, 'name': self.names.get(vid, ''), 'set': d.get(DEC_SET, [0])[0], 'binding': d[DEC_BINDING][0], 'count': count, 'dim': None}
            eop = self.op(elem)
            if sc == SC_UNIFORM and self.op(elem) == OP_TYPE_STRUCT:
                e['desc'] = VK_STORAGE_BUFFER if DEC_BUFFER_BLOCK in self.decos.get(elem, {}) else VK_UNIFORM_BUFFER
            elif sc == SC_STORAGE_BUFFER: e['desc'] = VK_STORAGE_BUFFER
            elif sc == SC_UNIFORM_CONSTANT:
                if eop == OP_TYPE_SAMPLER: e['desc'] = VK_SAMPLER
                else:
                    img = self.types[elem][2] if eop == OP_TYPE_SAMPLED_IMAGE else elem
                    if self.op(img) != OP_TYPE_IMAGE: raise ValueError(f'resource {e["name"]}: type {eop} is not an image, sampler or block')
                    _, _, sampled_t, dim, depth, arrayed, ms, sampled = self.types[img][:8]
                    e['dim'] = {'dim': dim, 'depth': depth == 1, 'array': bool(arrayed), 'ms': bool(ms), 'type': self.int_type(sampled_t)}
                    if eop == OP_TYPE_SAMPLED_IMAGE: e['desc'] = VK_COMBINED
                    elif sampled == 2: e['desc'] = VK_STORAGE_TEXEL if dim == DIM_BUFFER else VK_STORAGE_IMAGE
                    else: e['desc'] = VK_UNIFORM_TEXEL if dim == DIM_BUFFER else VK_SAMPLED_IMAGE
            else: raise ValueError(f'resource {e["name"]}: storage class {sc}')
            out.append(e)
        return out

    def set_binding(self, vid, s, b):
        for x in self.ins:
            if x[0] & 0xffff == OP_DECORATE and x[1] == vid:
                if x[2] == DEC_SET: x[3] = s
                elif x[2] == DEC_BINDING: x[3] = b
        d = self.decos.setdefault(vid, {})
        if DEC_SET not in d:   # a resource without a set decoration is in set 0: give it its set explicitly
            at = next(k for k, y in enumerate(self.ins) if y[0] & 0xffff == OP_DECORATE and y[1] == vid)
            self.ins.insert(at, [4 << 16 | OP_DECORATE, vid, DEC_SET, s])
        d[DEC_SET] = [s]; d[DEC_BINDING] = [b]

    def io(self, sc):
        """Location-decorated Input / Output variables of the entry point → {location: (variable, value type)}"""
        out = {}
        for vid in self.interface():
            ptr, c = self.vars.get(vid, (None, None))
            if c != sc: continue
            d = self.decos.get(vid, {})
            if DEC_LOCATION in d: out[d[DEC_LOCATION][0]] = (vid, self.pointee(ptr)[1])
        return out

    def builtin_var(self, sc, builtin):
        """(variable, member index or None) of a builtin in the interface: a variable decorated with it, or a block member"""
        for vid in self.interface():
            ptr, c = self.vars.get(vid, (None, None))
            if c != sc: continue
            if self.decos.get(vid, {}).get(DEC_BUILTIN) == [builtin]: return vid, None
            t = self.pointee(ptr)[1]
            for x in self.ins:
                if x[0] & 0xffff == OP_MEMBER_DECORATE and x[1] == t and x[3] == DEC_BUILTIN and x[4] == builtin: return vid, x[2]
        return None, None

    def entry_function(self): return self.entry[2]

    def function_range(self, fid):   # (index of OpFunction, index of OpFunctionEnd)
        a = next(k for k, x in enumerate(self.ins) if x[0] & 0xffff == OP_FUNCTION and x[2] == fid)
        return a, next(k for k in range(a, len(self.ins)) if self.ins[k][0] & 0xffff == OP_FUNCTION_END)

    def uses(self):
        """id → [instruction indices that take it as an operand] (operand positions are not decoded: a literal equal to an id counts too, which only widens the search)"""
        u = {}
        for k, x in enumerate(self.ins):
            for v in x[1:]: u.setdefault(v, []).append(k)
        return u


def defined_id(x):
    """The result id of an instruction with a result type (or a type / label), else None: [opcode, type, result, ...] for most"""
    op = x[0] & 0xffff
    if 19 <= op <= 39 or op in (OP_LABEL, OP_EXT_INST_IMPORT): return x[1]
    if op in (OP_LOAD, OP_ACCESS_CHAIN, OP_IN_BOUNDS_ACCESS_CHAIN, OP_COMPOSITE_EXTRACT, OP_COPY_OBJECT, OP_BITCAST, OP_VARIABLE, OP_CONSTANT, OP_CONSTANT_COMPOSITE): return x[2]
    return None


def first_block_insert_point(m, fid):
    """The index in m.ins right after the entry function's first label and its OpVariable declarations"""
    a, _ = m.function_range(fid)
    k = next(k for k in range(a, len(m.ins)) if m.ins[k][0] & 0xffff == OP_LABEL) + 1
    while m.ins[k][0] & 0xffff == OP_VARIABLE: k += 1
    return k


def vec4_const(m, f32, vals):
    vt = m.add_type(OP_TYPE_VECTOR, f32, 4)
    ids = [m.add_const(f32, struct.unpack('<I', struct.pack('<f', v))[0]) for v in vals]
    for i, x in enumerate(m.ins):
        if x[0] & 0xffff == OP_CONSTANT_COMPOSITE and x[1] == vt and x[3:] == ids: return x[2]
    c = m.new_id(); m._insert_decl([7 << 16 | OP_CONSTANT_COMPOSITE, vt, c, *ids])
    return c


def init_outputs(m, extra_locations=()):
    """Vertex outputs start as (0, 0, 0, 1) before the body runs, as an unwritten component reads on the hardware (dmsh_build.py vs_init); extra_locations get new
    vec4 outputs holding that value, for paired pixel shaders that read what this shader does not write (vs_pad). → the locations added"""
    outs = m.io(SC_OUTPUT)
    f32 = m.find_type(OP_TYPE_FLOAT, 32)
    if f32 is None: return []
    one = vec4_const(m, f32, (0.0, 0.0, 0.0, 1.0))
    vt = m.find_type(OP_TYPE_VECTOR, f32, 4)
    added = []
    for loc in sorted(set(extra_locations) - set(outs)):
        ptr = m.add_type(OP_TYPE_POINTER, SC_OUTPUT, vt)
        v = m.new_id(); m._insert_decl([4 << 16 | OP_VARIABLE, ptr, v, SC_OUTPUT]); m.vars[v] = (ptr, SC_OUTPUT)
        at = next(k for k, y in enumerate(m.ins) if y[0] & 0xffff == OP_DECORATE)
        m.ins.insert(at, [4 << 16 | OP_DECORATE, v, DEC_LOCATION, loc]); m.decos[v] = {DEC_LOCATION: [loc]}
        name = lit_string(f'dm_pad{loc}'); m.ins.insert(at, [(2 + len(name)) << 16 | OP_NAME, v, *name])
        ep = m.entry; ep.append(v); ep[0] = len(ep) << 16 | OP_ENTRY_POINT
        outs[loc] = (v, vt); added.append(loc)
    k = first_block_insert_point(m, m.entry_function())
    stores = [[3 << 16 | OP_STORE, v, one] for loc, (v, t) in sorted(outs.items()) if t == vt]
    m.ins[k:k] = stores
    return added


def gl_depth(m, spec_id):
    """The GL depth convention (near = -w, NVN's default) under a specialization constant: z' = fma(z, 0.5, w * 0.5) of the position output, selected when the bool
    constant spec_id is true (default false: z as translated, NVN's near = 0), before every return of the entry function or before every vertex a geometry shader emits
    (dmsh_build.py vs_depth, the same arithmetic; there it is a second entry point). → True when the module writes a position"""
    pos, member = m.builtin_var(SC_OUTPUT, BUILTIN_POSITION)
    if pos is None: return False
    f32 = m.find_type(OP_TYPE_FLOAT, 32); vt = m.find_type(OP_TYPE_VECTOR, f32, 4)
    pvec = m.add_type(OP_TYPE_POINTER, SC_OUTPUT, vt)
    half = m.add_const(f32, 0x3f000000)
    i32 = m.find_type(OP_TYPE_INT, 32, 1) or m.add_type(OP_TYPE_INT, 32, 1)
    midx = m.add_const(i32, member) if member is not None else None
    glsl = ensure_glsl(m)
    boolean = m.add_type(OP_TYPE_BOOL)
    spec = m.new_id(); m._insert_decl([3 << 16 | OP_SPEC_CONSTANT_FALSE, boolean, spec])
    at = next(k for k, y in enumerate(m.ins) if y[0] & 0xffff == OP_DECORATE)
    m.ins.insert(at, [4 << 16 | OP_DECORATE, spec, DEC_SPEC_ID, spec_id]); m.decos[spec] = {DEC_SPEC_ID: [spec_id]}
    name = lit_string('dm_gl_depth'); m.ins.insert(next(k for k, y in enumerate(m.ins) if y[0] & 0xffff in (OP_NAME, OP_DECORATE)), [(2 + len(name)) << 16 | OP_NAME, spec, *name])
    a, e = m.function_range(m.entry_function())
    stop = (OP_EMIT_VERTEX, OP_EMIT_STREAM_VERTEX) if m.model == 'gs' else (OP_RETURN, OP_RETURN_VALUE)
    out, n = [], 0
    for k, x in enumerate(m.ins):
        if a < k < e and x[0] & 0xffff in stop:
            p = pos
            if midx is not None: p = m.new_id(); out.append([5 << 16 | OP_ACCESS_CHAIN, pvec, p, pos, midx])
            v, z, w, wh, z2, z3, v2 = (m.new_id() for _ in range(7))
            out += [[4 << 16 | OP_LOAD, vt, v, p], [5 << 16 | OP_COMPOSITE_EXTRACT, f32, z, v, 2], [5 << 16 | OP_COMPOSITE_EXTRACT, f32, w, v, 3],
                    [5 << 16 | OP_FMUL, f32, wh, w, half], [8 << 16 | OP_EXT_INST, f32, z2, glsl, 50, z, half, wh],   # GLSL.std.450 50: Fma
                    [6 << 16 | OP_SELECT, f32, z3, spec, z2, z], [6 << 16 | OP_COMPOSITE_INSERT, vt, v2, z3, v, 2], [3 << 16 | OP_STORE, p, v2]]
            n += 1
        out.append(x)
    m.ins = out
    return n > 0


def value_uses(m):
    """id → [(instruction, operand position)] for the instructions of function bodies whose operands are ids at known positions"""
    u = {}
    for x in m.ins:
        op = x[0] & 0xffff
        if op in (OP_LOAD,): ids = [(3, x[3])]
        elif op == OP_STORE: ids = [(1, x[1]), (2, x[2])]
        elif op in (OP_ACCESS_CHAIN, OP_IN_BOUNDS_ACCESS_CHAIN): ids = [(p, x[p]) for p in range(3, len(x))]
        elif op == OP_COMPOSITE_EXTRACT: ids = [(3, x[3])]
        elif op in (OP_COPY_OBJECT, OP_BITCAST): ids = [(3, x[3])]
        elif op == OP_ENTRY_POINT or op == OP_NAME or op == OP_DECORATE or op == OP_MEMBER_DECORATE: continue
        else: ids = [(p, x[p]) for p in range(1, len(x))]   # conservative: every word may be an id
        for p, v in ids: u.setdefault(v, []).append((x, p))
    return u


def integer_only_inputs(m):
    """Vertex inputs declared float that the shader only uses as integer bits (dmsh_build.py int_inputs, here on the SPIR-V): every value loaded from the input, whole or
    by component, is only bit-cast to an integer type, copied, or stored to a function variable whose loads are such values in turn. → {location: 'uint'}"""
    u = value_uses(m)
    def int_bits(val, seen):
        if val in seen: return True
        seen.add(val)
        users = u.get(val, [])
        if not users: return False
        for x, p in users:
            op = x[0] & 0xffff
            if op == OP_BITCAST and m.int_type(m.scalar_of(x[1])): continue
            if op in (OP_COPY_OBJECT, OP_COMPOSITE_EXTRACT) and p == 3:
                if not int_bits(x[2], seen): return False
                continue
            if op == OP_STORE and p == 2:   # into a local: its loads must be integer uses too
                var = x[1]
                if m.vars.get(var, (0, None))[1] != SC_FUNCTION: return False
                loads = [y[2] for y, q in u.get(var, []) if y[0] & 0xffff == OP_LOAD and q == 3]
                if any(y[0] & 0xffff not in (OP_LOAD, OP_STORE) for y, _ in u.get(var, [])): return False
                if not all(int_bits(l, seen) for l in loads): return False
                continue
            return False
        return True
    out = {}
    for loc, (var, t) in m.io(SC_INPUT).items():
        if m.int_type(m.scalar_of(t)): continue
        vals, ok = [], True
        for x, p in u.get(var, []):
            op = x[0] & 0xffff
            if op == OP_LOAD and p == 3: vals.append(x[2])
            elif op in (OP_ACCESS_CHAIN, OP_IN_BOUNDS_ACCESS_CHAIN) and p == 3:
                for y, q in u.get(x[2], []):
                    if y[0] & 0xffff == OP_LOAD and q == 3: vals.append(y[2])
                    else: ok = False
            else: ok = False
        if ok and vals and all(int_bits(v, set()) for v in vals): out[str(loc)] = 'uint'
    return out


def int_outputs(m):
    """Colour outputs (fragment Output locations) some store writes from integer bits (a bit cast of an integer value, whole or through an access chain)"""
    u = value_uses(m); defs = {}
    for x in m.ins:
        d = defined_id(x)
        if d is not None: defs[d] = x
    found = set()
    for loc, (var, _) in m.io(SC_OUTPUT).items():
        ptrs = [var] + [x[2] for x, p in u.get(var, []) if x[0] & 0xffff in (OP_ACCESS_CHAIN, OP_IN_BOUNDS_ACCESS_CHAIN) and p == 3]
        for ptr in ptrs:
            for x, p in u.get(ptr, []):
                if x[0] & 0xffff == OP_STORE and p == 1:
                    d = defs.get(x[2])
                    if d and d[0] & 0xffff == OP_BITCAST: found.add(loc)   # the translator bit-casts only between integers and floats
    return found


def functions_named(m):
    return {m.names.get(x[2], ''): x[2] for x in m.ins if x[0] & 0xffff == OP_FUNCTION}


def ensure_glsl(m):
    """The GLSL.std.450 import's id, added before OpMemoryModel when the module has none"""
    for x in m.ins:
        if x[0] & 0xffff == OP_EXT_INST_IMPORT and string_at(x, 2)[0] == 'GLSL.std.450': return x[1]
    i = m.new_id(); s = lit_string('GLSL.std.450')
    at = next(k for k, x in enumerate(m.ins) if x[0] & 0xffff == 14)   # OpMemoryModel
    m.ins.insert(at, [(2 + len(s)) << 16 | OP_EXT_INST_IMPORT, i, *s])
    return i


SUBGROUP_OPS = set(range(333, 367)) | {4421, 4422, 4428, 4429, 4430, 4432}   # OpGroupNonUniform*, the KHR ballot / vote instructions
SUBGROUP_BUILTINS = {36, 37, 40, 41, 4416, 4417, 4418, 4419, 4420}   # SubgroupSize, NumSubgroups, SubgroupId, SubgroupLocalInvocationId, the lane masks
CAP_SUBGROUP = {61, 62, 63, 64, 65, 66, 67, 68, 4423, 4431}
CAP_FLOAT64, CAP_INTERLOCK = 10, {5363, 5372, 5378}
OP_TYPE_FLOAT64_WIDTH = 64
INTERLOCK_OPS, INTERLOCK_MODES = {5364, 5365}, {5366, 5367, 5368, 5369, 5370, 5371}   # Begin / EndInvocationInterlockEXT; the interlock execution modes


def subgroup_used(m):
    return any(x[0] & 0xffff in SUBGROUP_OPS for x in m.ins) or any(d.get(DEC_BUILTIN, [None])[0] in SUBGROUP_BUILTINS for d in m.decos.values()) or \
        any(x[0] & 0xffff == OP_MEMBER_DECORATE and x[3] == DEC_BUILTIN and x[4] in SUBGROUP_BUILTINS for x in m.ins)


def strip_unused_caps(m):
    """Capabilities the translator declares in every module whatever it uses (its host answers say the device has them), dropped where nothing needs them, so a module asks the device
    only for what it uses: Float64 without a 64-bit float type (the phone has no shaderFloat64; a shader that used doubles was already lowered, Program.cs softfloat64), fragment shader
    interlock without an interlock instruction (with its extension and modes), the subgroup capabilities without a subgroup instruction or builtin. → names of those dropped"""
    drop = set()
    if not any(x[0] & 0xffff == OP_TYPE_FLOAT and x[2] == 64 for x in m.ins): drop.add(CAP_FLOAT64)
    if not any(x[0] & 0xffff in INTERLOCK_OPS for x in m.ins): drop |= CAP_INTERLOCK
    if not subgroup_used(m): drop |= CAP_SUBGROUP
    drop &= set(m.caps)
    keep = []
    for x in m.ins:
        op = x[0] & 0xffff
        if op == OP_CAPABILITY and x[1] in drop: continue
        if op == OP_EXTENSION and drop & CAP_INTERLOCK and string_at(x, 1)[0] == 'SPV_EXT_fragment_shader_interlock': continue
        if op == OP_EXECUTION_MODE and drop & CAP_INTERLOCK and x[2] in INTERLOCK_MODES: continue
        keep.append(x)
    m.ins = keep; m.index()
    return sorted(CAPS.get(c, str(c)) for c in drop)


GLSL_PRECISE = {37: 79, 40: 80, 43: 81}   # GLSL.std.450 FMin / FMax / FClamp → NMin / NMax / NClamp


def precise_minmax(m):
    """min, max and clamp with Maxwell's NaN results: FMNMX returns the other operand of a NaN and saturation gives 0, which GLSL.std.450's NMin / NMax / NClamp
    define and FMin / FMax / FClamp leave to the device (dmsh_build.py precise, the same rule for Metal's fast:: functions). → instructions rewritten"""
    glsl = {x[1] for x in m.ins if x[0] & 0xffff == OP_EXT_INST_IMPORT and string_at(x, 2)[0] == 'GLSL.std.450'}
    n = 0
    for x in m.ins:
        if x[0] & 0xffff == OP_EXT_INST and x[3] in glsl and x[4] in GLSL_PRECISE and m.op(x[1]) != OP_TYPE_INT and m.op(m.scalar_of(x[1])) == OP_TYPE_FLOAT:
            x[4] = GLSL_PRECISE[x[4]]; n += 1
    return n


def const_int(m, ident):
    c = m.consts.get(ident)
    return c[1][0] if c and m.int_type(c[0]) else None


def block_rows(m, vid):
    """The 16-byte rows of a uniform block declared as vec4[n] (the translator's constant buffers) that the shader reads: (sorted byte offsets of rows read at
    constant indices, whether any index is computed), from the access chains on the variable"""
    rows, dyn = set(), False
    for x in m.ins:
        if x[0] & 0xffff in (OP_ACCESS_CHAIN, OP_IN_BOUNDS_ACCESS_CHAIN) and x[3] == vid:
            if len(x) < 6: dyn = True; continue
            r = const_int(m, x[5])
            if r is None: dyn = True
            else: rows.add(r * 16 + (4 * const_int(m, x[6]) if len(x) > 6 and const_int(m, x[6]) is not None else 0))
        elif x[0] & 0xffff == OP_LOAD and x[3] == vid: dyn = True   # the whole block
    return sorted(rows), dyn


CAP_READ_WITHOUT_FORMAT, CAP_WRITE_WITHOUT_FORMAT = 55, 56


def formatless_storage_caps(m):
    """The capabilities a read or write of a storage image declared with format Unknown needs, which the translator leaves out (its images take the format of the answer
    table, and without an answer it declares none). → names of those added (the device features shaderStorageImageRead / WriteWithoutFormat)"""
    imgs = {i for i, x in m.types.items() if x[0] & 0xffff == OP_TYPE_IMAGE and x[7] == 2 and x[8] == 0}   # sampled 2 = storage, format 0 = Unknown
    if not imgs: return []
    typed = {}
    for x in m.ins:
        if x[0] & 0xffff == OP_LOAD and x[1] in imgs: typed[x[2]] = True
    need = set()
    for x in m.ins:
        op = x[0] & 0xffff
        if op == OP_IMAGE_READ and x[3] in typed: need.add(CAP_READ_WITHOUT_FORMAT)
        elif op == OP_IMAGE_WRITE and x[1] in typed: need.add(CAP_WRITE_WITHOUT_FORMAT)
    need -= set(m.caps)
    if not need: return []
    at = max(k for k, y in enumerate(m.ins) if y[0] & 0xffff == OP_CAPABILITY) + 1
    for c in sorted(need): m.ins.insert(at, [2 << 16 | OP_CAPABILITY, c])
    m.index()
    return sorted(CAPS[c] for c in need)


def maybe_ids(x):
    """The words of a function-body instruction that may be ids (the literal operands of the instructions whose literals could pass for an id left out; others whole)"""
    op = x[0] & 0xffff
    if op == OP_EXT_INST: return [x[1], x[3]] + x[5:]
    if op == OP_COMPOSITE_EXTRACT: return x[1:4]
    if op in (OP_COMPOSITE_INSERT, 79): return x[1:5]   # 79: OpVectorShuffle
    if op == 251: return [x[1]] + x[2:3] + x[4::2]   # OpSwitch: selector, default, the targets after each literal
    mask_at = {87: 5, 88: 5, 91: 5, 92: 5, 95: 5, 98: 5, 89: 6, 90: 6, 93: 6, 94: 6, 96: 6, 97: 6, 99: 4}.get(op)   # image sample / fetch / gather / read / write: the image operands mask
    if mask_at is not None and len(x) > mask_at: return x[1:mask_at] + x[mask_at + 1:]
    return x[1:]


SC_PRIVATE = 6
BUILTIN_VIEWPORT_MASK_NV = 5253
CAP_VIEWPORT_MASK_NV, CAP_VIEWPORT_INDEX_LAYER = 5255, 5254


def private_viewport_mask(m):
    """A geometry shader whose passthrough Ryujinx emulates (Program.cs gs_passthrough false: the device has no SPV_NV_geometry_shader_passthrough) keeps NVN's viewport mask
    output (0x3a0) for one use: it emits the primitive only when the mask is not 0 (EmitterContext.PrepareForReturn; a cull, as the hardware sends a primitive with an empty mask
    to no viewport). Without SPV_NV_viewport_array2 (Adreno) the ViewportMaskNV output becomes a Private variable of the same type: the cull stays, the mask leaves the
    shader. What is lost is the routing of a primitive to the viewports the mask names (a single-viewport draw sees no difference). → True when rewritten"""
    var = next((v for v, d in m.decos.items() if d.get(DEC_BUILTIN) == [BUILTIN_VIEWPORT_MASK_NV] and m.vars.get(v, (0, None))[1] == SC_OUTPUT), None)
    if var is None: return False
    ptr, _ = m.vars[var]; t = m.pointee(ptr)[1]
    pptr = m.add_type(OP_TYPE_POINTER, SC_PRIVATE, t)
    chains, in_fn = [], False   # access chains on the variable; any other use in a function body but a load or store is not the shape this rewrite knows
    for x in m.ins:
        op = x[0] & 0xffff
        in_fn = (in_fn or op == OP_FUNCTION) and op != OP_FUNCTION_END
        if op in (OP_ACCESS_CHAIN, OP_IN_BOUNDS_ACCESS_CHAIN) and x[3] == var:
            if m.pointee(x[1])[0] != SC_OUTPUT: raise ValueError('viewport mask access chain not in Output')
            chains.append(x)
        elif in_fn and var in maybe_ids(x) and op not in (OP_LOAD, OP_STORE): raise ValueError(f'viewport mask used by opcode {op}')
    twins = {}   # Output pointer type of a chain → its Private twin (added after the scan: add_type inserts into m.ins)
    for x in chains:
        if x[1] not in twins: twins[x[1]] = m.add_type(OP_TYPE_POINTER, SC_PRIVATE, m.pointee(x[1])[1])
        x[1] = twins[x[1]]
    keep = []
    for x in m.ins:
        op = x[0] & 0xffff
        if op == OP_DECORATE and x[1] == var: continue
        if op == OP_VARIABLE and x[2] == var: continue   # declared again below, after its new pointer type
        if op == OP_ENTRY_POINT and m.header[1] < 0x10400:   # before SPIR-V 1.4 the interface lists only Input / Output variables (the translator writes 1.3)
            _, k = string_at(x, 3)
            x = x[:k] + [v for v in x[k:] if v != var]; x[0] = len(x) << 16 | OP_ENTRY_POINT
        keep.append(x)
    m.ins = keep; m._insert_decl([4 << 16 | OP_VARIABLE, pptr, var, SC_PRIVATE]); m.index()
    if not any(d.get(DEC_BUILTIN) == [BUILTIN_VIEWPORT_MASK_NV] for d in m.decos.values()) and \
            not any(x[0] & 0xffff == OP_MEMBER_DECORATE and x[3] == DEC_BUILTIN and x[4] == BUILTIN_VIEWPORT_MASK_NV for x in m.ins):
        layer = CAP_VIEWPORT_INDEX_LAYER in m.caps
        m.ins = [x for x in m.ins if not (x[0] & 0xffff == OP_CAPABILITY and x[1] == CAP_VIEWPORT_MASK_NV) and
                 not (x[0] & 0xffff == OP_EXTENSION and not layer and string_at(x, 1)[0] == 'SPV_NV_viewport_array2')]
        m.index()
    return True


OP_CONSTANT_TRUE, OP_CONSTANT_FALSE, OP_SPEC_CONSTANT, OP_CONSTANT_NULL = 41, 42, 50, 46
OP_IEQUAL, OP_FORD_EQUAL, OP_FUNORD_NOT_EQUAL, OP_FORD_LESS, OP_FORD_GREATER, OP_FORD_LESS_EQUAL, OP_FORD_GREATER_EQUAL = 170, 180, 183, 184, 186, 188, 190
OP_SELECTION_MERGE, OP_BRANCH_CONDITIONAL, OP_KILL = 247, 250, 252
DEC_INDEX = 32


def _float_vec(m, t):
    """(component count, float32 type) of a float scalar or vector type, else None"""
    s = m.scalar_of(t)
    if m.op(s) != OP_TYPE_FLOAT or m.types[s][2] != 32: return None
    return (m.types[t][3] if m.op(t) == OP_TYPE_VECTOR else 1), s


def const_written_outputs(m):
    """Colour outputs (locations) every store to which, whole or by component, writes a constant: a clear drawn into an integer target writes 0 that way
    (dmsh_build.py fs_uint's second rule). → the locations, empty unless every output written is such"""
    consts = {x[2] for x in m.ins if x[0] & 0xffff in (OP_CONSTANT, OP_CONSTANT_COMPOSITE, OP_CONSTANT_NULL)}
    outs = m.io(SC_OUTPUT); ptrs = {}
    for loc, (var, _) in outs.items(): ptrs[var] = loc
    for x in m.ins:
        if x[0] & 0xffff in (OP_ACCESS_CHAIN, OP_IN_BOUNDS_ACCESS_CHAIN) and x[3] in ptrs: ptrs[x[2]] = ptrs[x[3]]
    stored = {}
    for x in m.ins:
        if x[0] & 0xffff == OP_STORE and x[1] in ptrs: stored.setdefault(ptrs[x[1]], []).append(x[2] in consts)
    return set(stored) if stored and all(all(v) for v in stored.values()) else set()


def retype_outputs(m, locs):
    """The fragment outputs at locs, declared float, retyped to the unsigned integer vector of the same width: every store bit-casts its value, every load bit-casts back
    (dmsh_build.py fs_uint: the same bits, nothing converted). An integer attachment takes only an integer output (Vulkan: the numeric class must match). → locations retyped"""
    outs = m.io(SC_OUTPUT); u32 = m.add_type(OP_TYPE_INT, 32, 0)
    want = {}
    for loc in sorted(locs):
        if loc not in outs: continue
        var, t = outs[loc]; fv = _float_vec(m, t)
        if fv: want[var] = (loc, t, fv[0])
    if not want: return []
    def utype(n): return u32 if n == 1 else m.add_type(OP_TYPE_VECTOR, u32, n)
    newptr = {var: m.add_type(OP_TYPE_POINTER, SC_OUTPUT, utype(n)) for var, (_, _, n) in want.items()}
    ptr_of = {var: (t, utype(n)) for var, (_, t, n) in want.items()}   # pointer id → (float value type, uint value type)
    chains = []
    for x in m.ins:
        if x[0] & 0xffff in (OP_ACCESS_CHAIN, OP_IN_BOUNDS_ACCESS_CHAIN) and x[3] in ptr_of:
            et = m.pointee(x[1])[1]; fv = _float_vec(m, et)
            if not fv: raise ValueError('colour output access chain to a non-float')
            chains.append((x, et, fv[0]))
    for x, et, n in chains:
        ptr_of[x[2]] = (et, utype(n)); x[1] = m.add_type(OP_TYPE_POINTER, SC_OUTPUT, utype(n))
    out, in_fn = [], False
    for x in m.ins:
        op = x[0] & 0xffff
        in_fn = (in_fn or op == OP_FUNCTION) and op != OP_FUNCTION_END
        if op == OP_VARIABLE and x[2] in want: continue   # declared again below, after its new pointer type
        if op == OP_STORE and x[1] in ptr_of:
            v = m.new_id(); out.append([4 << 16 | OP_BITCAST, ptr_of[x[1]][1], v, x[2]]); out.append([3 << 16 | OP_STORE, x[1], v, *x[3:]]); continue
        if op == OP_LOAD and x[3] in ptr_of:
            v = m.new_id(); out.append([x[0], ptr_of[x[3]][1], v, *x[3:]]); out.append([4 << 16 | OP_BITCAST, x[1], x[2], v]); continue
        if op in (OP_STORE, OP_LOAD) or op in (OP_ACCESS_CHAIN, OP_IN_BOUNDS_ACCESS_CHAIN): out.append(x); continue
        if in_fn and any(v in ptr_of for v in maybe_ids(x)):
            raise ValueError(f'colour output used by opcode {op}')
        out.append(x)
    m.ins = out
    for var in want: m._insert_decl([4 << 16 | OP_VARIABLE, newptr[var], var, SC_OUTPUT])
    m.index()
    return sorted(loc for loc, _, _ in want.values())


DEC_PER_PRIMITIVE, CAP_MESH_SHADING = 5271, 5283   # PerPrimitiveEXT, MeshShadingEXT (SPV_EXT_mesh_shader)


def per_primitive_inputs(m, locs):
    """The inputs of a pixel shader that a mesh program writes per primitive (shader_split.py mesh_facts: the flat inputs of the mesh's pixel shader): Vulkan matches a
    PerPrimitiveEXT mesh output only with an input decorated the same, so each such input gets PerPrimitiveEXT, and the module the capability and extension. Done only for a
    shader drawn with mesh programs alone (vk_android_build.py MESH_PRIM): it is not valid in a vertex pipeline any more. → the locations decorated"""
    ins = m.io(SC_INPUT)
    done = sorted(l for l in locs if l in ins)
    if not done: return []
    at = next(k for k, y in enumerate(m.ins) if y[0] & 0xffff == OP_DECORATE)
    for l in done: m.ins.insert(at, [3 << 16 | OP_DECORATE, ins[l][0], DEC_PER_PRIMITIVE])
    if CAP_MESH_SHADING not in m.caps:
        k = max(k for k, y in enumerate(m.ins) if y[0] & 0xffff == OP_CAPABILITY)
        m.ins.insert(k + 1, [2 << 16 | OP_CAPABILITY, CAP_MESH_SHADING])
    if 'SPV_EXT_mesh_shader' not in m.exts:
        k = max(k for k, y in enumerate(m.ins) if y[0] & 0xffff in (OP_CAPABILITY, OP_EXTENSION))
        e = lit_string('SPV_EXT_mesh_shader'); m.ins.insert(k + 1, [(1 + len(e)) << 16 | OP_EXTENSION, *e])
    if m.header[1] < 0x10400:   # the extension needs SPIR-V 1.4, whose entry point lists every global variable it uses, not only Input / Output: all of them are listed
        m.header[1] = 0x10400
        have = set(m.interface())
        glob = [x[2] for x in m.ins if x[0] & 0xffff == OP_VARIABLE and x[3] != SC_FUNCTION and x[2] not in have]
        k = m.ins.index(m.entry)
        m.ins[k] = m.entry + glob; m.ins[k][0] = len(m.ins[k]) << 16 | OP_ENTRY_POINT
        m.index(); buffer_block_to_storage(m)
    m.index()
    return done


OP_PTR_ACCESS_CHAIN, DEC_BLOCK_ = 67, 2


def buffer_block_to_storage(m):
    """SPIR-V 1.4 has no BufferBlock: a Uniform-class block decorated BufferBlock (this translator's storage buffers) becomes a StorageBuffer-class Block, and every pointer
    into it (its variables, the access chains from them) gets a StorageBuffer pointer type of the same pointee. → the number of blocks rewritten"""
    blocks = {x[1] for x in m.ins if x[0] & 0xffff == OP_DECORATE and x[2] == DEC_BUFFER_BLOCK}
    if not blocks: return 0
    def sb_ptr(uptr):   # the StorageBuffer pointer of a Uniform pointer type's pointee, declared right after it (a global variable may use it)
        t = m.types[uptr][3]
        i = m.find_type(OP_TYPE_POINTER, SC_STORAGE_BUFFER, t)
        if i is not None: return i
        i = m.new_id(); x = [4 << 16 | OP_TYPE_POINTER, i, SC_STORAGE_BUFFER, t]
        m.ins.insert(m.ins.index(m.types[uptr]) + 1, x); m.types[i] = x
        return i
    def elem(t):   # an array of blocks (a descriptor array): its element
        while m.op(t) in (OP_TYPE_ARRAY, OP_TYPE_RUNTIME_ARRAY): t = m.types[t][2]
        return t
    ptrs = {i: sb_ptr(i) for i, x in list(m.types.items()) if x[0] & 0xffff == OP_TYPE_POINTER and x[2] == SC_UNIFORM and elem(x[3]) in blocks}
    sb = set()
    for x in list(m.ins):   # a copy: sb_ptr may insert a pointer type
        op = x[0] & 0xffff
        if op == OP_DECORATE and x[1] in blocks and x[2] == DEC_BUFFER_BLOCK: x[2] = DEC_BLOCK_
        elif op == OP_VARIABLE and x[1] in ptrs and x[3] == SC_UNIFORM: x[1] = ptrs[x[1]]; x[3] = SC_STORAGE_BUFFER; sb.add(x[2])
        elif op in (OP_ACCESS_CHAIN, OP_IN_BOUNDS_ACCESS_CHAIN, OP_PTR_ACCESS_CHAIN, OP_COPY_OBJECT) and x[3] in sb:
            x[1] = sb_ptr(x[1]); sb.add(x[2])
    m.index()
    return len(blocks)


def dual_source(m):
    """Dual-source blending (dmsh_build.py fs_dual): NVN's SRC1 factors read output 1 as target 0's second source; Vulkan reads it only from Location 0 Index 1. For a shader
    writing exactly colours 0 and 1: output 1 moved to Location 0, Index 1. → True when rewritten"""
    outs = m.io(SC_OUTPUT)
    if sorted(outs) != [0, 1]: return False
    var = outs[1][0]
    for k, x in enumerate(m.ins):
        if x[0] & 0xffff == OP_DECORATE and x[1] == var and x[2] == DEC_LOCATION:
            x[3] = 0; m.ins.insert(k + 1, [4 << 16 | OP_DECORATE, var, DEC_INDEX, 1]); break
    m.index()
    return True


ALPHA_CMP = {2: OP_FORD_LESS, 3: OP_FORD_EQUAL, 4: OP_FORD_LESS_EQUAL, 5: OP_FORD_GREATER, 6: OP_FUNORD_NOT_EQUAL, 7: OP_FORD_GREATER_EQUAL}   # NVN_ALPHA_FUNC_* → comparison of alpha with the reference


def alpha_test(m, spec_id):
    """NVN's alpha test (ColorStateSetAlphaTest / SetAlphaRef; dmsh_alpha.py on Metal): colour 0's alpha compared with a reference before blending, the fragment dropped when the
    comparison fails. The function is a uint specialization constant spec_id (NVN_ALPHA_FUNC_*: 1 never, 2 less … 7 gequal, 8 always; default 0 = no test, which the driver
    folds away), part of the pipeline as the colour state is; the reference is per draw, the float bits of the support buffer's word 0 (Ryujinx's unused alpha_test field), so a
    reference that changes every draw (a dissolve) keeps the pipeline. The test runs before every return of the entry function. → True when added (a float vec4 colour 0 and
    the support buffer present)"""
    outs = m.io(SC_OUTPUT)
    if 0 not in outs: return False
    c0, t = outs[0]; fv = _float_vec(m, t)
    if not fv or fv[0] != 4: return False
    f32 = fv[1]
    sb = next((v for v, (p, sc) in m.vars.items() if sc == SC_UNIFORM and m.names.get(v) == 'support_buffer'), None)
    if sb is None: return False
    st = m.pointee(m.vars[sb][0])[1]
    if m.op(st) != OP_TYPE_STRUCT or m.int_type(m.types[st][2]) != (32, 0): return False
    u32 = m.types[st][2]; i32 = m.find_type(OP_TYPE_INT, 32, 1) or m.add_type(OP_TYPE_INT, 32, 1)
    boolean = m.add_type(OP_TYPE_BOOL); pu = m.add_type(OP_TYPE_POINTER, SC_UNIFORM, u32)
    zero = m.add_const(i32, 0)
    fconst = {n: m.add_const(u32, n) for n in [1] + list(ALPHA_CMP)}
    spec = m.new_id(); m._insert_decl([4 << 16 | OP_SPEC_CONSTANT, u32, spec, 0])
    t_ = next((x[2] for x in m.ins if x[0] & 0xffff == OP_CONSTANT_TRUE and x[1] == boolean), None)
    if t_ is None: t_ = m.new_id(); m._insert_decl([3 << 16 | OP_CONSTANT_TRUE, boolean, t_])
    f_ = next((x[2] for x in m.ins if x[0] & 0xffff == OP_CONSTANT_FALSE and x[1] == boolean), None)
    if f_ is None: f_ = m.new_id(); m._insert_decl([3 << 16 | OP_CONSTANT_FALSE, boolean, f_])
    at = next(k for k, y in enumerate(m.ins) if y[0] & 0xffff == OP_DECORATE)
    m.ins.insert(at, [4 << 16 | OP_DECORATE, spec, DEC_SPEC_ID, spec_id])
    name = lit_string('dm_alpha_func'); m.ins.insert(next(k for k, y in enumerate(m.ins) if y[0] & 0xffff in (OP_NAME, OP_DECORATE)), [(2 + len(name)) << 16 | OP_NAME, spec, *name])
    vt = t; a, e = m.function_range(m.entry_function()); out, n = [], 0
    for k, x in enumerate(m.ins):
        if a < k < e and x[0] & 0xffff == OP_RETURN:
            col, al, pw, rw, ref = (m.new_id() for _ in range(5))
            out += [[4 << 16 | OP_LOAD, vt, col, c0], [5 << 16 | OP_COMPOSITE_EXTRACT, f32, al, col, 3],
                    [5 << 16 | OP_ACCESS_CHAIN, pu, pw, sb, zero], [4 << 16 | OP_LOAD, u32, rw, pw], [4 << 16 | OP_BITCAST, f32, ref, rw]]
            res = t_   # an unknown function (0, 8 always) passes
            for fn in [1] + list(ALPHA_CMP):
                is_fn, cmp_, r2 = m.new_id(), m.new_id(), m.new_id()
                out.append([5 << 16 | OP_IEQUAL, boolean, is_fn, spec, fconst[fn]])
                if fn == 1: cmp_ = f_
                else: out.append([5 << 16 | ALPHA_CMP[fn], boolean, cmp_, al, ref])
                out.append([6 << 16 | OP_SELECT, boolean, r2, is_fn, cmp_, res]); res = r2
            ok, kill = m.new_id(), m.new_id()
            out += [[3 << 16 | OP_SELECTION_MERGE, ok, 0], [4 << 16 | OP_BRANCH_CONDITIONAL, res, ok, kill],
                    [2 << 16 | OP_LABEL, kill], [1 << 16 | OP_KILL], [2 << 16 | OP_LABEL, ok]]
            n += 1
        out.append(x)
    m.ins = out; m.index()
    return n > 0


OP_FUNCTION_PARAMETER, OP_FUNCTION_CALL = 55, 57
OP_IADD, OP_ISUB, OP_IMUL, OP_SHR_LOGICAL, OP_SHR_ARITH, OP_SHL_LOGICAL, OP_BITWISE_OR, OP_BITWISE_XOR, OP_BITWISE_AND = 128, 130, 132, 194, 195, 196, 197, 198, 199
OP_CONTROL_BARRIER, OP_GROUP_NON_UNIFORM_SHUFFLE = 224, 345
SC_WORKGROUP, SCOPE_SUBGROUP, SEM_ACQ_REL_WORKGROUP = 4, 3, 0x8 | 0x100
BUILTIN_SUBGROUP_SIZE, BUILTIN_SUBGROUP_ID, BUILTIN_SUBGROUP_LOCAL_INVOCATION_ID = 36, 40, 41
CAP_GROUP_NON_UNIFORM, EXEC_MODE_LOCAL_SIZE = 61, 17
VN_OPS = {OP_IADD, OP_ISUB, OP_IMUL, OP_SHR_LOGICAL, OP_SHR_ARITH, OP_SHL_LOGICAL, OP_BITWISE_OR, OP_BITWISE_XOR, OP_BITWISE_AND}


def input_builtin(m, builtin, ty):
    """The Input variable of a builtin (a plain variable), added with its decoration and in the entry point's interface when the module has none → its id"""
    v, member = m.builtin_var(SC_INPUT, builtin)
    if v is not None and member is None: return v
    p = m.add_type(OP_TYPE_POINTER, SC_INPUT, ty); v = m.new_id()
    m._insert_decl([4 << 16 | OP_VARIABLE, p, v, SC_INPUT])
    m.ins.insert(next(k for k, x in enumerate(m.ins) if x[0] & 0xffff == OP_DECORATE), [4 << 16 | OP_DECORATE, v, DEC_BUILTIN, builtin])
    k = next(k for k, x in enumerate(m.ins) if x[0] & 0xffff == OP_ENTRY_POINT)
    m.ins[k] = m.ins[k] + [v]; m.ins[k][0] = len(m.ins[k]) << 16 | OP_ENTRY_POINT
    m.index()
    return v


def workgroup_bytes(m):
    """Bytes of the module's Workgroup variables (arrays of 32-bit scalars or vectors, as the translator declares them); None for another type"""
    n = 0
    for vid, (ptr, sc) in m.vars.items():
        if sc != SC_WORKGROUP: continue
        t, count = m.strip_arrays(m.pointee(ptr)[1])
        comps = m.types[t][3] if m.op(t) == OP_TYPE_VECTOR else 1
        s = m.scalar_of(t)
        if m.op(s) not in (OP_TYPE_INT, OP_TYPE_FLOAT) or m.types[s][2] != 32 or not count: return None
        n += 4 * comps * count
    return n


class BlockValues:
    """Value numbers within a block of the translator's code, which keeps its registers in Function variables: a load of a variable numbers as the value last stored to it in
    the block, or as (variable, version) with a version that every store and every call taking it as an argument moves; bit casts and copies keep their operand's number;
    integer arithmetic numbers by its operation and operands' numbers, a load of an Input variable (or of a constant index into one) by the variable. Two values with one
    number hold the same bits in every lane. see(x) before x is passed on; block()
    at each label; function() at each OpFunction"""
    def __init__(self, m): self.m = m; self.inptr = {}; self.function()
    def function(self): self.fvars, self.fbase, self.ver, self.stored, self.vn = set(), {}, {}, {}, {}
    def block(self): self.stored = {}
    def __call__(self, i): return self.vn.get(i, i)
    def key_of_var(self, p):   # the number of what a load of Function variable p would give here
        b = self.fbase.get(p, p)
        return self(self.stored[p]) if p in self.stored else ('ld', b, self.ver.get(b, 0))
    def bump(self, p):
        b = self.fbase.get(p, p); self.ver[b] = self.ver.get(b, 0) + 1; self.stored.pop(b, None)
    def see(self, x):
        m, op = self.m, x[0] & 0xffff
        if op == OP_FUNCTION: self.function()
        elif op == OP_LABEL: self.block()
        elif (op == OP_VARIABLE and len(x) > 3 and x[3] == SC_FUNCTION) or (op == OP_FUNCTION_PARAMETER and m.op(x[1]) == OP_TYPE_POINTER and m.types[x[1]][2] == SC_FUNCTION):
            self.fvars.add(x[2])   # a local variable, or the translator's pointer argument (written only through a store here or by a call)
        elif op in (OP_ACCESS_CHAIN, OP_IN_BOUNDS_ACCESS_CHAIN) and (x[3] in self.fvars or x[3] in self.fbase): self.fbase[x[2]] = self.fbase.get(x[3], x[3])
        elif op == OP_STORE and (x[1] in self.fvars or x[1] in self.fbase):
            self.bump(x[1])
            if x[1] in self.fvars: self.stored[x[1]] = x[2]
        elif op == OP_LOAD and x[3] in self.fvars: self.vn[x[2]] = self.key_of_var(x[3])
        elif op == OP_LOAD and (x[3] in self.inptr or m.vars.get(x[3], (0, 0))[1] == SC_INPUT): self.vn[x[2]] = ('in', self.inptr.get(x[3], x[3]))
        elif op in (OP_ACCESS_CHAIN, OP_IN_BOUNDS_ACCESS_CHAIN) and m.vars.get(x[3], (0, 0))[1] == SC_INPUT and all(i in m.consts for i in x[4:]): self.inptr[x[2]] = (x[3], *x[4:])
        elif op in (OP_BITCAST, OP_COPY_OBJECT): self.vn[x[2]] = self(x[3])
        elif op in VN_OPS: self.vn[x[2]] = (op, self(x[3]), self(x[4]))
        elif op == OP_FUNCTION_CALL:
            for p in x[4:]:
                if p in self.fvars or p in self.fbase: self.bump(p)

def shuffle_lds(m, shared_max):
    """The 32-bit subgroup shuffles of a compute module through workgroup memory (the library's DMSH_VK_MOD_SHUFFLE_LDS module, for drivers without an arbitrary-index
    shuffle instruction): each lane stores the value at its slot of its subgroup's row (SubgroupId * SubgroupSize + lane), a subgroup barrier, the slot of the lane asked
    for is read. Turnip has no such instruction: ir3 makes each OpGroupNonUniformShuffle a loop of rotates, one pass per distinct (index - lane) in the subgroup, up to 24
    for the translator's LDSM / MOVM gathers (agl's upscaler network on Donkey Kong Bananza: 389 loops, 10.7 ms a dispatch on the Adreno 840 against 2.0 ms this way);
    Qualcomm's driver shuffles natively and is faster with its base module (1.7 ms against 1.9). Two rows alternate, so a store comes after the other row's reads only
    with the barrier of the exchange between them; an exchange of the value a row still holds in the same block reads it without a store or barrier (LDSM's four reads of
    one address, MOVM's two); the last exchange before the block ends or another function is called gets a trailing barrier. The module is rewritten in place.
    → bytes of workgroup memory added (0: not a compute module with such shuffles, or over shared_max with them; the module is then unchanged)"""
    if m.model != 'cs': return 0
    shuf = [x for x in m.ins if x[0] & 0xffff == OP_GROUP_NON_UNIFORM_SHUFFLE and m.op(x[1]) in (OP_TYPE_INT, OP_TYPE_FLOAT) and m.types[x[1]][2] == 32
            and m.consts.get(x[3], (0, [0]))[1] == [SCOPE_SUBGROUP]]
    ls = next((x[1:4] for x in m.modes if x[0] == EXEC_MODE_LOCAL_SIZE), None)   # (m.modes: the entry point's, without its id)
    used = workgroup_bytes(m)
    if not shuf or ls is None or used is None: return 0
    words = -(-ls[0] * ls[1] * ls[2] // 128) * 128   # a row: SubgroupId * SubgroupSize + lane, subgroups of up to 128 lanes
    if used + 8 * words > shared_max: return 0
    u32 = m.add_type(OP_TYPE_INT, 32, 0)
    arr = m.add_type(OP_TYPE_ARRAY, u32, m.add_const(u32, 2 * words))
    p_arr = m.add_type(OP_TYPE_POINTER, SC_WORKGROUP, arr); p_u = m.add_type(OP_TYPE_POINTER, SC_WORKGROUP, u32)
    xs = m.new_id(); m._insert_decl([4 << 16 | OP_VARIABLE, p_arr, xs, SC_WORKGROUP])
    row_off = [None, m.add_const(u32, words)]; C = {v: m.add_const(u32, v) for v in (SCOPE_SUBGROUP, SEM_ACQ_REL_WORKGROUP)}
    sgid, sgsz, sgl = (input_builtin(m, b, u32) for b in (BUILTIN_SUBGROUP_ID, BUILTIN_SUBGROUP_SIZE, BUILTIN_SUBGROUP_LOCAL_INVOCATION_ID))
    if CAP_GROUP_NON_UNIFORM not in m.caps:
        at = max(k for k, x in enumerate(m.ins) if x[0] & 0xffff == OP_CAPABILITY); m.ins.insert(at + 1, [2 << 16 | OP_CAPABILITY, CAP_GROUP_NON_UNIFORM])
    nm = lit_string('dm_shuffle_rows'); m.ins.insert(next(k for k, y in enumerate(m.ins) if y[0] & 0xffff in (OP_NAME, OP_DECORATE)), [(2 + len(nm)) << 16 | OP_NAME, xs, *nm])
    barrier = [4 << 16 | OP_CONTROL_BARRIER, C[SCOPE_SUBGROUP], C[SCOPE_SUBGROUP], C[SEM_ACQ_REL_WORKGROUP]]
    m.index()
    want = {id(x) for x in shuf}
    calls, cur = {}, None   # functions that exchange, themselves or through a call: a call of one ends the caller's run of exchanges
    for x in m.ins:
        op = x[0] & 0xffff
        if op == OP_FUNCTION: cur = x[2]; calls[cur] = [False, set()]
        elif op == OP_GROUP_NON_UNIFORM_SHUFFLE and id(x) in want: calls[cur][0] = True
        elif op == OP_FUNCTION_CALL: calls[cur][1].add(x[3])
    exch = {f for f, (h, _) in calls.items() if h}
    while True:
        more = {f for f, (_, c) in calls.items() if f not in exch and c & exch}
        if not more: break
        exch |= more
    out, trail = [], []   # trail: positions in out after an exchange's read that take a barrier
    V = BlockValues(m)
    rows, dirty, last = [None, None], [False, False], None   # the value each row holds in this block, rows read since the last barrier, the last exchange's read
    def close():   # the block ends or a function that exchanges runs: the last exchange gets a trailing barrier, and the rows' contents count as unknown after it
        nonlocal rows, dirty, last
        if last is not None: trail.append(last)
        rows, dirty, last = [None, None], [False, False], None
    def em(op, ty, *ops):
        r = m.new_id(); out.append([(3 + len(ops)) << 16 | op, ty, r, *ops]); return r
    for x in m.ins:
        op = x[0] & 0xffff
        if op in (OP_LABEL, OP_FUNCTION_END) or (op == OP_FUNCTION_CALL and x[3] in exch): close()
        V.see(x)
        if op != OP_GROUP_NON_UNIFORM_SHUFFLE or id(x) not in want: out.append(x); continue
        key = V(x[4])   # (a value number: the same in two shuffles of this block only when every lane's value is the same)
        row = next((r for r in (0, 1) if rows[r] == key), None)
        sg = em(OP_IMUL, u32, em(OP_LOAD, u32, sgid), em(OP_LOAD, u32, sgsz))
        if row is None:   # store into a row nothing has read since the last barrier
            clean = [r for r in (0, 1) if not dirty[r]]
            if not clean: trail.append(last); clean = [0, 1]
            row = next((r for r in clean if rows[r] is None), clean[0])
            base = sg if row == 0 else em(OP_IADD, u32, sg, row_off[1])
            v = x[4] if x[1] == u32 else em(OP_BITCAST, u32, x[4])
            out.append([3 << 16 | OP_STORE, em(OP_ACCESS_CHAIN, p_u, xs, em(OP_IADD, u32, base, em(OP_LOAD, u32, sgl))), v])
            out.append(list(barrier))
            rows[row] = key; dirty = [False, False]   # the barrier orders every earlier read
        else: base = sg if row == 0 else em(OP_IADD, u32, sg, row_off[1])
        dirty[row] = True
        r = em(OP_LOAD, u32, em(OP_ACCESS_CHAIN, p_u, xs, em(OP_IADD, u32, base, x[5])))
        out.append([4 << 16 | (OP_COPY_OBJECT if x[1] == u32 else OP_BITCAST), x[1], x[2], r])
        last = len(out)
    for k in sorted(set(trail), reverse=True): out.insert(k, list(barrier))
    m.ins = out; m.index()
    return 8 * words


HMMA_NAME = 'dmsh_hmma16816_f16'
HMMA_KEEP = 2   # converted A/B tiles hmma_lds keeps between the calls of a block (see hmma_lds)
OP_MEMORY_BARRIER, OP_COMPOSITE_CONSTRUCT, OP_TYPE_FUNCTION_ = 225, 80, 33
OP_VECTOR_SHUFFLE, OP_ATOMIC_COMPARE_EXCHANGE, OP_LOOP_MERGE, OP_SELECTION_MERGE, OP_BRANCH, OP_BRANCH_CONDITIONAL, OP_SWITCH = 79, 230, 246, 247, 249, 250, 251
GLSL_FMA, GLSL_PACK_HALF_2X16, GLSL_UNPACK_HALF_2X16 = 50, 58, 62
NO_RESULT_OPS = {OP_STORE, OP_CONTROL_BARRIER, OP_MEMORY_BARRIER, OP_RETURN, OP_RETURN_VALUE, 246, 247, 249, 250, 251, 252, OP_FUNCTION_END}


def _remap_operands(x, remap):
    op = x[0] & 0xffff
    if op == OP_EXT_INST:
        for i in range(5, len(x)):
            if x[i] in remap: x[i] = remap[x[i]]
    elif op == OP_COMPOSITE_EXTRACT:
        if x[3] in remap: x[3] = remap[x[3]]
    elif op in (OP_COMPOSITE_INSERT, OP_VECTOR_SHUFFLE):
        if x[3] in remap: x[3] = remap[x[3]]
        if x[4] in remap: x[4] = remap[x[4]]
    elif op in (OP_LOOP_MERGE, OP_SELECTION_MERGE): pass
    elif op == OP_SWITCH:
        if x[1] in remap: x[1] = remap[x[1]]
    elif op in NO_RESULT_OPS:
        for i in range(1, len(x)):
            if x[i] in remap: x[i] = remap[x[i]]
    else:
        for i in range(3, len(x)):
            if x[i] in remap: x[i] = remap[x[i]]


OP_CONVERT_U_TO_PTR, OP_ATOMIC_OR = 120, 241
GMEM_MAP_SINK_WORDS = (6, 7)   # dmsh_abi.h DMSH_GMEM_MAP_*: entry 0's last two words, the device address of a scratch word the host keeps for accesses out of every pool


def gmem_sink(m, body, cond):
    """A GlobalToStorage raw helper (TryGenerateRawStorageOp) about to lose its inPool branch: its physical address taken from the table's sink (entry 0, words 6 and 7)
    when the address is in no pool (cond false), and its write log made to write nothing then: the old log's compare-exchanges (a hash set of pages) get the comparator ~0,
    which no page is, and the bitmap log's OR (one bit a table entry, a319e773) ORs 0. Only the log's atomics, on storage buffer words: an atomic through the physical
    pointer (a small integer's store loop) is the access itself and goes to the sink. True when done; False leaves the helper as it is (the branch is then kept)"""
    cu = [x for x in body if x[0] & 0xffff == OP_CONVERT_U_TO_PTR]
    ac = next((x for x in body if x[0] & 0xffff == OP_ACCESS_CHAIN and len(x) == 6), None)   # the search's first table word: table, uint pointer type, constant 0
    if len(cu) != 1 or ac is None or ac[4] not in m.consts: return False
    defs = {x[2]: x for x in body if len(x) > 2 and (x[0] & 0xffff) not in NO_RESULT_OPS}
    bc = defs.get(cu[0][3])
    cc = defs.get(bc[3]) if bc is not None and bc[0] & 0xffff == OP_BITCAST else None
    if cc is None or cc[0] & 0xffff != OP_COMPOSITE_CONSTRUCT or len(cc) != 5: return False
    u32 = m.types.get(cc[1], [0, 0, 0])[2]
    if m.pointee(ac[1]) != (SC_UNIFORM, u32) and m.pointee(ac[1]) != (SC_STORAGE_BUFFER, u32): return False
    ity = m.consts[ac[4]][0]
    ins = []
    for k, w in zip((3, 4), GMEM_MAP_SINK_WORDS):
        p_, v_, s_ = m.new_id(), m.new_id(), m.new_id()
        ins += [[6 << 16 | OP_ACCESS_CHAIN, ac[1], p_, ac[3], ac[4], m.add_const(ity, w)], [4 << 16 | OP_LOAD, u32, v_, p_], [6 << 16 | OP_SELECT, u32, s_, cond, cc[k], v_]]
        cc[k] = s_
    at = body.index(cc); body[at:at] = ins
    never, nothing = m.add_const(u32, 0xffffffff), m.add_const(u32, 0)
    def log_word(x):   # an atomic on a storage buffer word (the log), not on the physical pointer
        ch = defs.get(x[3])
        return ch is not None and ch[0] & 0xffff == OP_ACCESS_CHAIN and m.pointee(ch[1])[0] == SC_STORAGE_BUFFER
    for x in list(body):
        if x[0] & 0xffff == OP_ATOMIC_COMPARE_EXCHANGE and log_word(x):
            s_ = m.new_id(); body.insert(body.index(x), [6 << 16 | OP_SELECT, u32, s_, cond, x[8], never]); x[8] = s_
        elif x[0] & 0xffff == OP_ATOMIC_OR and log_word(x):
            s_ = m.new_id(); body.insert(body.index(x), [6 << 16 | OP_SELECT, u32, s_, cond, x[6], nothing]); x[6] = s_
    return True


def simplify_helpers(m):
    """Read-only Function pointer parameters of non-entry functions passed by value; in the translator's GlobalToStorage raw helpers (the functions that make a
    physical pointer: GMEM_MAP search, GMEM_LOG probes, the access) the 8-probe log CAS chain collapsed to one probe and the inPool branch removed, the access going to
    the table's sink word when the address is in no pool (gmem_sink), so inlining hundreds of helper calls does not multiply basic blocks, SSA defs, and Function
    variables in the driver (Turnip nir_opt_algebraic). The binary search keeps all its steps (13, GlobalToStorage.GmemMapLog2): the table holds up to
    DMSH_GMEM_MAP_ENTRIES pools, and a shorter search finds only the first 2^steps (cut to 6, it found the first 64: Donkey Kong Bananza's terrain records sat past them,
    docs/reference/shaders.md, Global memory at an address loaded from memory; the cut came back once with a merge, A search cut that came back). The helpers' own fast
    paths (the entry chosen last, the 2 MiB chunk's hint) are what make most lookups short now. The inPool branch goes only when it is the branch around the access
    (the physical pointer inside its true block): the helpers since a319e773 open with the last-entry and hint branches, whose blocks hold no access, so they keep
    every branch"""
    ent = m.entry_function()
    ro_fns = {}
    for f in [x[2] for x in m.ins if x[0] & 0xffff == OP_FUNCTION and x[2] != ent]:
        a, e = m.function_range(f)
        body = m.ins[a:e + 1]
        raw = any(x[0] & 0xffff == OP_CONVERT_U_TO_PTR for x in body)
        cas = [i for i, x in enumerate(body) if (x[0] & 0xffff) == OP_ATOMIC_COMPARE_EXCHANGE]
        if raw and len(cas) == 8:
            k0 = cas[0]
            ksel = next((i for i in range(k0 + 1, min(k0 + 12, len(body))) if (body[i][0] & 0xffff) == OP_SELECTION_MERGE), None)
            if ksel is not None:
                mlbl = body[ksel][1]
                km = next((i for i in range(ksel + 2, len(body)) if (body[i][0] & 0xffff) == OP_LABEL and body[i][1] == mlbl), None)
                if km is not None:
                    gone = {y[2] for y in body[k0 + 1:km + 1] if len(y) > 2 and (y[0] & 0xffff) not in NO_RESULT_OPS} | {y[1] for y in body[k0 + 1:km + 1] if y[0] & 0xffff == OP_LABEL}
                    del body[k0 + 1:km + 1]
                    m.ins[a:e + 1] = body
                    m.ins = [y for y in m.ins if not (y[0] & 0xffff in (OP_NAME, OP_DECORATE) and y[1] in gone)]
                    a, e = m.function_range(f); body = m.ins[a:e + 1]
        ksel = next((i for i, x in enumerate(body) if (x[0] & 0xffff) == OP_SELECTION_MERGE), None) if raw else None
        if ksel is not None and ksel + 2 < len(body) and (body[ksel + 1][0] & 0xffff) == OP_BRANCH_CONDITIONAL:
            merge_lbl = body[ksel][1]
            kbr = ksel + 1
            cond = body[kbr][1]
            true_lbl = body[kbr][2]
            km_lbl = next((i for i in range(ksel + 2, len(body)) if (body[i][0] & 0xffff) == OP_LABEL and body[i][1] == merge_lbl), None)
            around = km_lbl is not None and any(body[i][0] & 0xffff == OP_CONVERT_U_TO_PTR for i in range(kbr, km_lbl))   # the branch guards the access
            if around and km_lbl + 1 < len(body) and (body[km_lbl + 1][0] & 0xffff) in (OP_RETURN, OP_RETURN_VALUE) and gmem_sink(m, body, cond):
                km_lbl = next(i for i in range(ksel + 2, len(body)) if (body[i][0] & 0xffff) == OP_LABEL and body[i][1] == merge_lbl)
                a, e = m.function_range(f)   # (constants were declared before the functions)
                ret_true = body[km_lbl - 1]
                ret_false = body[km_lbl + 1]
                op_rt = ret_true[0] & 0xffff
                op_rf = ret_false[0] & 0xffff
                if op_rt == OP_RETURN_VALUE and op_rf == OP_RETURN_VALUE:
                    t_ins = next((x for x in body if len(x) > 2 and x[2] == ret_true[1]), None)
                    if t_ins is not None:
                        ret_ty = t_ins[1]
                        res_id = m.new_id()
                        sel_ins = [6 << 16 | OP_SELECT, ret_ty, res_id, cond, ret_true[1], ret_false[1]]
                        ret_true[1] = res_id
                        del body[km_lbl:km_lbl + 2]
                        del body[ksel:ksel + 3]
                        ret_idx = body.index(ret_true)
                        body.insert(ret_idx, sel_ins)
                        m.ins[a:e + 1] = body
                        gone_lbls = {merge_lbl, true_lbl}
                        m.ins = [y for y in m.ins if not ((y[0] & 0xffff) in (OP_NAME, OP_DECORATE) and y[1] in gone_lbls)]
                        a, e = m.function_range(f); body = m.ins[a:e + 1]
                elif op_rt == OP_RETURN and op_rf == OP_RETURN:
                    del body[km_lbl:km_lbl + 2]
                    del body[ksel:ksel + 3]
                    m.ins[a:e + 1] = body
                    gone_lbls = {merge_lbl, true_lbl}
                    m.ins = [y for y in m.ins if not ((y[0] & 0xffff) in (OP_NAME, OP_DECORATE) and y[1] in gone_lbls)]
                    a, e = m.function_range(f); body = m.ins[a:e + 1]
        params = []
        k = 1
        while k < len(body) and (body[k][0] & 0xffff) == OP_FUNCTION_PARAMETER:
            params.append((k, body[k][1], body[k][2])); k += 1
        if not params: continue
        p_ids = {pid for _, _, pid in params}
        bad = set()
        for x in body[k:]:
            op = x[0] & 0xffff
            if op == OP_LOAD and len(x) == 4 and x[3] in p_ids: continue
            for w in x[1:]:
                if w in p_ids: bad.add(w)
        ro, new_ptys = {}, []
        for idx, (_, pty, pid) in enumerate(params):
            if pid not in bad and m.op(pty) == OP_TYPE_POINTER and m.types[pty][2] == SC_FUNCTION:
                vty = m.types[pty][3]
                ro[idx] = (pid, vty); body[idx + 1][1] = vty; new_ptys.append(vty)
            else: new_ptys.append(pty)
        if not ro: continue
        ro_pid = {pid: vty for pid, vty in ro.values()}
        remap, out = {}, []
        for x in body:
            op = x[0] & 0xffff
            if op == OP_LOAD and len(x) == 4 and x[3] in ro_pid:
                remap[x[2]] = x[3]; continue
            if remap: _remap_operands(x, remap)
            out.append(x)
        body = out
        body[0][4] = m.add_type(OP_TYPE_FUNCTION_, body[0][1], *new_ptys)
        a, e = m.function_range(f)
        m.ins[a:e + 1] = body
        ro_fns[f] = ro
    if ro_fns:
        out = []
        for x in m.ins:
            if (x[0] & 0xffff) == OP_FUNCTION_CALL and x[3] in ro_fns:
                x = list(x)
                for idx, (_, vty) in ro_fns[x[3]].items():
                    r = m.new_id()
                    out.append([4 << 16 | OP_LOAD, vty, r, x[4 + idx]])
                    x[4 + idx] = r
            out.append(x)
        m.ins = out
    m.index()


OP_INOT_EQUAL, OP_PHI, LOOP_DONT_UNROLL = 171, 245, 0x2


def loop_pool_search(m):
    """The pool table search in each GlobalToStorage raw helper (after promote_locals: a chain of n OpSelects, step k taking entry idx + 2^(n-k) when its start is at or
    below the address, each select's false operand the select before it) made one loop of n iterations with LoopControl DontUnroll: the step's own instructions once, the
    index and the step in two Function variables. The search stays whole (every entry of the table reachable) and its code is one step, not n: the driver inlines the
    helper at every call site (Ocarina of Time's mesh shader 73dc0b90c4082e43: 637 calls), and 13 unrolled steps there ran Qualcomm's compiler out of memory
    (std::bad_alloc: docs/reference/shaders.md, The pool table search cut back to 6 steps by a merge). → the number of searches made loops"""
    ent = m.entry_function()
    i32b = m.find_type(OP_TYPE_BOOL)
    done = 0
    for f in [x[2] for x in m.ins if x[0] & 0xffff == OP_FUNCTION and x[2] != ent]:
        a, e = m.function_range(f)
        body = m.ins[a:e + 1]
        if not any(x[0] & 0xffff == OP_CONVERT_U_TO_PTR for x in body): continue
        at = {x[2]: k for k, x in enumerate(body) if len(x) > 2 and (x[0] & 0xffff) not in NO_RESULT_OPS and x[0] & 0xffff != OP_LABEL}
        def const_of(i): return m.consts[i][1][0] if i in m.consts and len(m.consts[i][1]) == 1 else None
        def step_of(s, prev):   # (index of its IAdd, step) when s = Select(c, prev + step, prev)
            if s[0] & 0xffff != OP_SELECT or len(s) != 6 or s[5] != prev or s[4] not in at: return None
            t = body[at[s[4]]]
            if t[0] & 0xffff != OP_IADD or t[3] != prev or const_of(t[4]) is None: return None
            return at[s[4]], const_of(t[4])
        chain = None
        for k, s in enumerate(body):
            if s[0] & 0xffff != OP_SELECT or len(s) != 6 or const_of(s[5]) != 0: continue
            st = step_of(s, s[5])
            if st is None: continue
            ch = [(st[0], k, st[1])]
            while True:
                nxt = next((j for j in range(ch[-1][1] + 1, len(body)) if body[j][0] & 0xffff == OP_SELECT and body[j][5] == body[ch[-1][1]][2]), None)
                st = step_of(body[nxt], body[ch[-1][1]][2]) if nxt is not None else None
                if st is None: break
                ch.append((st[0], nxt, st[1]))
            if len(ch) >= 2: chain = ch; break
        if chain is None: continue
        n = len(chain); ity = body[chain[0][1]][1]
        if [c[2] for c in chain] != [chain[0][2] >> k for k in range(n)] or chain[-1][2] != 1: continue
        if any(chain[k + 1][0] != chain[k][1] + 1 for k in range(n - 1)): continue   # the steps follow each other
        lo_k, hi_k = chain[0][0], chain[-1][1]
        if any(body[j][0] & 0xffff in (OP_LABEL, OP_STORE, OP_FUNCTION_CALL) or (body[j][0] & 0xffff) in NO_RESULT_OPS for j in range(lo_k, hi_k + 1)): continue
        defs = {body[j][2] for j in range(lo_k, hi_k + 1)}
        last = body[hi_k][2]
        if any(w in defs - {last} for x in body[:lo_k] + body[hi_k + 1:] for w in x[1:]): continue   # nothing after the search reads a step's values
        s1 = body[chain[0][1]][2]
        tpl = [list(x) for x in body[chain[1][0]:chain[1][1] + 1]]
        tdefs = {x[2] for x in tpl}
        if any(w in defs and w not in tdefs and w != s1 for x in tpl for w in x[3:]): continue   # step 2 reads only its own values and step 1's index
        # declarations first (they move the function)
        pty = m.add_type(OP_TYPE_POINTER, SC_FUNCTION, ity)
        bty = i32b if i32b is not None else m.add_type(OP_TYPE_BOOL)
        c0, c1, cstart = m.add_const(ity, 0), m.add_const(ity, 1), m.add_const(ity, chain[0][2])
        vlo, vst, lbl_h, lbl_b, lbl_c, lbl_m = (m.new_id() for _ in range(6))
        lo, st, snew, stn, more = (m.new_id() for _ in range(5))
        for x in tpl:
            for i in range(3, len(x)):
                if x[i] == s1: x[i] = lo
        tpl[0][4] = st   # step 2's IAdd: idx + step
        tpl[-1][2] = snew
        a, e = m.function_range(f)
        body = m.ins[a:e + 1]
        loop = [[3 << 16 | OP_STORE, vlo, c0], [3 << 16 | OP_STORE, vst, cstart], [2 << 16 | OP_BRANCH, lbl_h],
                [2 << 16 | OP_LABEL, lbl_h], [4 << 16 | OP_LOOP_MERGE, lbl_m, lbl_c, LOOP_DONT_UNROLL], [2 << 16 | OP_BRANCH, lbl_b],
                [2 << 16 | OP_LABEL, lbl_b], [4 << 16 | OP_LOAD, ity, lo, vlo], [4 << 16 | OP_LOAD, ity, st, vst], *tpl,
                [3 << 16 | OP_STORE, vlo, snew], [5 << 16 | OP_SHR_LOGICAL, ity, stn, st, c1], [3 << 16 | OP_STORE, vst, stn],
                [5 << 16 | OP_INOT_EQUAL, bty, more, stn, c0], [2 << 16 | OP_BRANCH, lbl_c],
                [2 << 16 | OP_LABEL, lbl_c], [4 << 16 | OP_BRANCH_CONDITIONAL, more, lbl_h, lbl_m],
                [2 << 16 | OP_LABEL, lbl_m], [4 << 16 | OP_LOAD, ity, last, vlo]]
        pre = next(body[j][1] for j in range(lo_k, -1, -1) if body[j][0] & 0xffff == OP_LABEL)
        for x in body:   # the block's end moves to the loop's merge block
            if x[0] & 0xffff == OP_PHI: x[4::2] = [lbl_m if p == pre else p for p in x[4::2]]
        body[lo_k:hi_k + 1] = loop
        k0 = next(k for k, x in enumerate(body) if x[0] & 0xffff == OP_LABEL)
        body[k0 + 1:k0 + 1] = [[4 << 16 | OP_VARIABLE, pty, vlo, SC_FUNCTION], [4 << 16 | OP_VARIABLE, pty, vst, SC_FUNCTION]]
        m.ins[a:e + 1] = body
        gone = defs - tdefs - {last}
        m.ins = [y for y in m.ins if not ((y[0] & 0xffff) in (OP_NAME, OP_DECORATE) and y[1] in gone)]
        done += 1
    if done: m.index()
    return done


OP_VECTOR_INSERT_DYNAMIC, OP_IEQUAL_, OP_ULESS_THAN, OP_ULESS_THAN_EQUAL, OP_LOGICAL_OR, OP_LOGICAL_AND, OP_LOGICAL_NOT = 78, 170, 176, 178, 166, 167, 168
SC_PHYSICAL_STORAGE_BUFFER = 5349
GMEM_RUN_WORDS = 4   # the most words of one run cache_raw_loads reads at once (the translator splits a 128-bit global load into four 32-bit raw loads)
GMEM_CACHE_MIN_CALLS = 4   # a module with fewer raw load calls is left as it is: its few inlined helpers cost little, and its library entry stays the same
                           # (agl's compute shaders with one or two raw loads, which several games ship: their libraries need no update for this pass)
# instructions that may not lie between the calls of one run: they write memory, order memory, end the invocation or call something else
_RUN_BREAKERS = {63, 64, OP_IMAGE_WRITE, 218, 219, 220, 221, OP_CONTROL_BARRIER, OP_MEMORY_BARRIER, OP_KILL, 4416, 4448, 5294, 5295, 5364, 5365, 5380} | set(range(227, 243))


def _store_class(m, defs, p):   # the storage class of the variable a pointer is into (through access chains), or None
    while p in defs and defs[p][0] & 0xffff in (OP_ACCESS_CHAIN, OP_IN_BOUNDS_ACCESS_CHAIN, OP_COPY_OBJECT): p = defs[p][3]
    return m.vars[p][1] if p in m.vars else None


def _raw_load_helper(m, f, tables):
    """f's facts when it is a GlobalToStorage raw load helper of one of the pool tables (the variables in tables): two 32-bit integer parameters (the address's low
    and high words), one physical pointer read once, its value (bits unchanged) or 0 returned, nothing written but Function or Private variables, nothing called;
    the table read as [table, 0, word] (a uint pointer of the table's storage class). Else None"""
    a, e = m.function_range(f)
    body = m.ins[a:e + 1]
    rty = body[0][1]
    params = [x for x in body[1:3] if x[0] & 0xffff == OP_FUNCTION_PARAMETER]
    if len(params) != 2 or (body[3][0] & 0xffff) == OP_FUNCTION_PARAMETER: return None
    if any((m.int_type(t) or (0,))[0] != 32 for t in (rty, params[0][1], params[1][1])): return None
    cu = [x for x in body if x[0] & 0xffff == OP_CONVERT_U_TO_PTR]
    if len(cu) != 1: return None
    sc, pt = m.pointee(cu[0][1])
    if sc != SC_PHYSICAL_STORAGE_BUFFER: return None
    ptr, member = cu[0][2], None
    if m.op(pt) == OP_TYPE_STRUCT:   # a block of one word (GLSL's buffer_reference): its member 0
        uses = [x for x in body if ptr in x[1:] and x is not cu[0]]
        if len(uses) != 1 or uses[0][0] & 0xffff != OP_ACCESS_CHAIN or len(uses[0]) != 5 or uses[0][3] != ptr or m.consts.get(uses[0][4], (0, [1]))[1] != [0]: return None
        member = (uses[0][1], uses[0][4]); ptr = uses[0][2]; pt = m.pointee(uses[0][1])[1]
    if (m.int_type(pt) or (0,))[0] != 32: return None
    uses = [x for x in body if ptr in x[1:] and x[2:3] != [ptr]]
    if len(uses) != 1 or uses[0][0] & 0xffff != OP_LOAD or uses[0][3] != ptr: return None
    load = uses[0]
    defs = {x[2]: x for x in body if len(x) > 2 and (x[0] & 0xffff) not in NO_RESULT_OPS and x[0] & 0xffff != OP_LABEL}
    for x in body:
        op = x[0] & 0xffff
        if op in _RUN_BREAKERS or op == OP_FUNCTION_CALL or (op == OP_STORE and _store_class(m, defs, x[1]) not in (SC_FUNCTION, SC_PRIVATE)): return None
    chains = [x for x in body if x[0] & 0xffff == OP_ACCESS_CHAIN and x[3] in tables]
    if not chains or len({x[3] for x in chains}) != 1 or any(len(x) != 6 or m.consts.get(x[4], (0, [1]))[1] != [0] for x in chains): return None
    tptr = chains[0][1]
    if m.pointee(tptr)[0] not in (SC_UNIFORM, SC_STORAGE_BUFFER) or (m.int_type(m.pointee(tptr)[1]) or (0,))[0] != 32: return None
    def bits_of(v):   # the value through bit-preserving casts
        while v in defs and defs[v][0] & 0xffff in (OP_BITCAST, OP_COPY_OBJECT): v = defs[v][3]
        return v
    zero = lambda v: v in m.consts and m.consts[v][1] == [0] or any(y[0] & 0xffff == OP_CONSTANT_NULL and y[2] == v for y in m.ins[:a])
    rets = [x[1] for x in body if x[0] & 0xffff == OP_RETURN_VALUE]
    if not rets or any(bits_of(v) != load[2] and not zero(v) for v in rets) or not any(bits_of(v) == load[2] for v in rets): return None
    return {'ret': rty, 'lo': params[0][1], 'hi': params[1][1], 'table': chains[0][3], 'tptr': tptr, 'ity': m.consts[chains[0][4]][0], 'word': pt,
            'pptr': cu[0][1], 'member': member, 'mem': load[4:]}


def cache_raw_loads(m, tables):
    """The GlobalToStorage raw load helpers' calls (a guest global load whose address the shader read from memory: docs/reference/shaders.md, Global memory at an
    address loaded from memory) replaced by calls of one generated function per helper and run length: the pool table entry of the last access kept in six Private
    words (start, end, delta), and a run of up to four words at one base address (the translator splits a 128-bit load into four calls at +0, +4, +8, +12 of the
    same 64-bit address) read at once when the entry holds all of them: no table read, no branch taken. Otherwise (the first access of an invocation, another pool,
    a run that leaves its entry) a cold loop looks each word up as the helper does (the 2 MiB chunk's hint, else the 13-step search), keeps its entry and reads it; an
    address in no entry reads the table's sink (entry 0, words 6 and 7) and gives 0, as the helper gives 0. The value of every word is the helper's.

    Why: the driver inlines every call, each with the helper's branches and search loop, so a vertex shader of 253 calls became ~1,000 small blocks; Turnip's ir3
    then spilled heavily (Ocarina of Time b1f06157308e3b22: 48 registers, 1,859 + 3,832 spill instructions, 2 us a vertex against 0.03 on Qualcomm's driver). With one
    likely-taken branch a run: 77 lookups instead of 253, 2.8x faster on Turnip, 2x on Qualcomm's driver (docs/reference/shaders.md, Raw global memory loads one block
    each on Turnip). tables: the pool table variables (reflection kind GMEM_MAP). → the number of calls replaced"""
    if not tables: return 0
    ent = m.entry_function()
    fns = [x[2] for x in m.ins if x[0] & 0xffff == OP_FUNCTION]
    helpers = {f: h for f in fns if f != ent for h in [_raw_load_helper(m, f, tables)] if h}
    if not helpers: return 0
    # runs of calls: one block, consecutive calls of one helper, nothing between them that writes or orders memory, addresses (H:B) + c, c + 4, ... (no carry out of c)
    runs = []
    for f in fns:
        if f in helpers: continue
        a, e = m.function_range(f)
        body = m.ins[a:e + 1]
        defs = {x[2]: x for x in body if len(x) > 2 and (x[0] & 0xffff) not in NO_RESULT_OPS and x[0] & 0xffff != OP_LABEL}
        def root(v):
            while v in defs and defs[v][0] & 0xffff in (OP_BITCAST, OP_COPY_OBJECT): v = defs[v][3]
            return v
        def cval(v):
            v = root(v)
            return m.consts[v][1][0] if v in m.consts and len(m.consts[v][1]) == 1 else None
        def addr(lo, hi):   # (B, H, c): the address is (H:B) + c, c a 32-bit constant, the high word with the translator's carry (B + c < B)
            x = defs.get(root(lo))
            if x is not None and x[0] & 0xffff == OP_IADD and (cval(x[3]) is not None) != (cval(x[4]) is not None):
                b, c = (root(x[4]), cval(x[3])) if cval(x[3]) is not None else (root(x[3]), cval(x[4]))
                y = defs.get(root(hi))
                if y is None or y[0] & 0xffff != OP_IADD: return (b, root(hi), c) if c == 0 else None
                for h, s in ((y[3], y[4]), (y[4], y[3])):
                    s = defs.get(root(s))
                    if s is None or s[0] & 0xffff != OP_SELECT or cval(s[4]) != 1 or cval(s[5]) != 0: continue
                    lt = defs.get(root(s[3]))
                    if lt is not None and lt[0] & 0xffff == OP_ULESS_THAN and root(lt[3]) == root(lo) and root(lt[4]) == b: return (b, root(h), c)
                return None
            return (root(lo), root(hi), 0)
        cur = []
        for k, x in enumerate(body):
            op = x[0] & 0xffff
            if op == OP_FUNCTION_CALL and x[3] in helpers and len(x) == 6:
                ad = addr(x[4], x[5])
                if (cur and ad is not None and cur[-1][2] is not None and x[3] == cur[-1][1][3] and ad[:2] == cur[-1][2][:2] and ad[2] == cur[-1][2][2] + 4
                        and len(cur) < GMEM_RUN_WORDS):
                    cur.append((k, x, ad)); continue
                if cur: runs.append((f, cur))
                cur = [(k, x, ad)]; continue
            if op == OP_LABEL or op in _RUN_BREAKERS or op == OP_FUNCTION_CALL or (op == OP_STORE and _store_class(m, defs, x[1]) not in (SC_FUNCTION, SC_PRIVATE)):
                if cur: runs.append((f, cur))
                cur = []
        if cur: runs.append((f, cur))
    if sum(len(c) for _, c in runs) < GMEM_CACHE_MIN_CALLS: return 0
    # declarations
    bty = m.add_type(OP_TYPE_BOOL)
    u32 = m.add_type(OP_TYPE_INT, 32, 0)
    v2 = m.add_type(OP_TYPE_VECTOR, u32, 2)
    v4 = m.add_type(OP_TYPE_VECTOR, u32, 4)
    u64 = m.add_type(OP_TYPE_INT, 64, 0)
    ppriv = m.add_type(OP_TYPE_POINTER, SC_PRIVATE, u32)
    U = {v: m.add_const(u32, v) for v in (0, 1, 4, 11, 13, 21, 256, 8191, 0xffffffff)}
    U.update({4 * k: m.add_const(u32, 4 * k) for k in range(1, GMEM_RUN_WORDS)})
    z4 = m.new_id(); m._insert_decl([3 << 16 | OP_CONSTANT_NULL, v4, z4])
    caches, gfns, new_globals = {}, {}, []
    for h in helpers.values():
        if h['table'] in caches: continue
        cv = []
        for k in range(6):   # start (low, high), end (low, high), delta (low, high): an empty entry at first (start ~0, end 0)
            i = m.new_id(); m._insert_decl([5 << 16 | OP_VARIABLE, ppriv, i, SC_PRIVATE, U[0xffffffff] if k < 2 else U[0]]); cv.append(i); new_globals.append(i)
        caches[h['table']] = cv
    if m.header[1] >= 0x10400:   # SPIR-V 1.4: the entry point lists every global variable it uses
        m.entry = m.entry + new_globals; m.entry[0] = len(m.entry) << 16 | OP_ENTRY_POINT
        m.ins[next(k for k, x in enumerate(m.ins) if x[0] & 0xffff == OP_ENTRY_POINT)] = m.entry
    for f, h in helpers.items():
        for n in sorted({len(c) for g, c in runs if c[0][1][3] == f}):
            gfns[f, n] = _cached_load_fn(m, h, n, caches[h['table']], f, dict(bool=bty, u32=u32, v2=v2, v4=v4, u64=u64, z4=z4, U=U))
    # the call sites
    done = 0
    for f in fns:
        if f in helpers: continue
        mine = [c for g, c in runs if g == f]
        if not mine: continue
        a, e = m.function_range(f)
        body = m.ins[a:e + 1]
        rep = {}
        for c in mine:
            k0, x0, _ = c[0]; n = len(c); r = m.new_id()
            new = [[6 << 16 | OP_FUNCTION_CALL, v4, r, gfns[x0[3], n], x0[4], x0[5]]]
            for j, (_, x, _) in enumerate(c):
                w = m.new_id()
                new += [[5 << 16 | OP_COMPOSITE_EXTRACT, u32, w, r, j], [4 << 16 | (OP_COPY_OBJECT if x[1] == u32 else OP_BITCAST), x[1], x[2], w]]
            rep[k0] = new
            for k, _, _ in c[1:]: rep[k] = []
            done += n
        out = []
        for k, x in enumerate(body): out += rep.get(k, [x])
        m.ins[a:e + 1] = out
    # helpers no call is left to
    called = {x[3] for x in m.ins if x[0] & 0xffff == OP_FUNCTION_CALL}
    for f in helpers:
        if f in called: continue
        a, e = m.function_range(f)
        gone = {x[1] if x[0] & 0xffff == OP_LABEL else x[2] for x in m.ins[a:e + 1] if (x[0] & 0xffff) not in NO_RESULT_OPS and len(x) > 2 or x[0] & 0xffff == OP_LABEL}
        del m.ins[a:e + 1]
        m.ins = [y for y in m.ins if not ((y[0] & 0xffff) in (OP_NAME, OP_DECORATE) and y[1] in gone)]
    m.index()
    return done


def _cached_load_fn(m, h, n, cache, helper, T):
    """cache_raw_loads' function for runs of n words of helper h: (low, high) of the first word → uvec4 of the n words' bits (the rest 0)"""
    ins = []
    bty, u32, v2, v4, u64, z4, U = T['bool'], T['u32'], T['v2'], T['v4'], T['u64'], T['z4'], T['U']
    ity, tptr, table = h['ity'], h['tptr'], h['table']
    def op(o, *w): ins.append([(len(w) + 1) << 16 | o, *w])
    def val(o, ty, *w):
        r = m.new_id(); ins.append([(len(w) + 3) << 16 | o, ty, r, *w]); return r
    def cast(v, frm, to): return v if frm == to else val(OP_BITCAST, to, v)
    I = {v: m.add_const(ity, v) for v in (0, 1, 2, 3, 4, 5, 6, 7, 8, 4096)}
    def word(idx_i, w):   # table word w of entry idx (an int of the index type) → u32
        p = val(OP_ACCESS_CHAIN, tptr, table, I[0], val(OP_IADD, ity, val(OP_IMUL, ity, idx_i, I[8]), I[w]))
        return cast(val(OP_LOAD, m.pointee(tptr)[1], p), m.pointee(tptr)[1], u32)
    def lt(ahi, alo, bhi, blo, eq=False):   # (ahi:alo) < (bhi:blo), or <=
        return val(OP_LOGICAL_OR, bty, val(OP_ULESS_THAN, bty, ahi, bhi),
                   val(OP_LOGICAL_AND, bty, val(OP_IEQUAL_, bty, ahi, bhi), val(OP_ULESS_THAN_EQUAL if eq else OP_ULESS_THAN, bty, alo, blo)))
    def add64(alo, ahi, blo, bhi):
        lo = val(OP_IADD, u32, alo, blo)
        return lo, val(OP_IADD, u32, val(OP_IADD, u32, ahi, bhi), val(OP_SELECT, u32, val(OP_ULESS_THAN, bty, lo, alo), U[1], U[0]))
    def access(alo, ahi):   # the word at a physical address
        p = val(OP_CONVERT_U_TO_PTR, h['pptr'], val(OP_BITCAST, u64, val(OP_COMPOSITE_CONSTRUCT, v2, alo, ahi)))
        if h['member']: p = val(OP_ACCESS_CHAIN, h['member'][0], p, h['member'][1])
        return cast(val(OP_LOAD, h['word'], p, *h['mem']), h['word'], u32)
    fid, plo, phi = m.new_id(), m.new_id(), m.new_id()
    fty = m.add_type(OP_TYPE_FUNCTION_, v4, h['lo'], h['hi'])
    E, HOT, COLD, M = m.new_id(), m.new_id(), m.new_id(), m.new_id()
    ins += [[5 << 16 | OP_FUNCTION, v4, fid, 0, fty], [3 << 16 | OP_FUNCTION_PARAMETER, h['lo'], plo], [3 << 16 | OP_FUNCTION_PARAMETER, h['hi'], phi]]
    op(OP_LABEL, E)
    lu, hu = cast(plo, h['lo'], u32), cast(phi, h['hi'], u32)
    al, ah = [lu], [hu]
    for k in range(1, n):
        x, y = add64(lu, hu, U[4 * k], U[0]); al.append(x); ah.append(y)
    c = [val(OP_LOAD, u32, v) for v in cache]
    hit = val(OP_LOGICAL_AND, bty, lt(c[1], c[0], hu, lu, eq=True), lt(ah[-1], al[-1], c[3], c[2]))
    op(OP_SELECTION_MERGE, M, 0); op(OP_BRANCH_CONDITIONAL, hit, HOT, COLD)
    # every word in the kept entry
    op(OP_LABEL, HOT)
    hv = [access(*add64(al[k], ah[k], c[4], c[5])) for k in range(n)]
    hr = val(OP_COMPOSITE_CONSTRUCT, v4, *hv, *[U[0]] * (4 - n))
    op(OP_BRANCH, M)
    # each word looked up: hint, else search; its entry kept when it holds the word
    LH, LB, LC, LM, SB, SH, SBB, SC, SM, SMM = (m.new_id() for _ in range(10))
    k0, acc, k1, acc2, x0, s0, x1, s1 = (m.new_id() for _ in range(8))
    op(OP_LABEL, COLD); op(OP_BRANCH, LH)
    op(OP_LABEL, LH)
    ins.append([7 << 16 | OP_PHI, u32, k0, U[0], COLD, k1, LC]); ins.append([7 << 16 | OP_PHI, v4, acc, z4, COLD, acc2, LC])
    op(OP_LOOP_MERGE, LM, LC, LOOP_DONT_UNROLL); op(OP_BRANCH, LB)
    op(OP_LABEL, LB)
    x, y = add64(lu, hu, val(OP_IMUL, u32, k0, U[4]), U[0])
    chunk = val(OP_BITWISE_OR, u32, val(OP_SHL_LOGICAL, u32, y, U[11]), val(OP_SHR_LOGICAL, u32, x, U[21]))
    hw = word(cast(val(OP_BITWISE_AND, u32, chunk, U[8191]), u32, ity), 6)
    hidx = val(OP_BITWISE_AND, u32, hw, U[8191])
    hok = val(OP_LOGICAL_AND, bty, val(OP_LOGICAL_AND, bty, val(OP_INOT_EQUAL, bty, hidx, U[0]), val(OP_IEQUAL_, bty, val(OP_SHR_LOGICAL, u32, hw, U[13]), chunk)),
              val(OP_ULESS_THAN, bty, y, U[256]))
    hidx_i = cast(hidx, u32, ity)
    hok = val(OP_LOGICAL_AND, bty, hok, lt(y, x, word(hidx_i, 3), word(hidx_i, 2)))
    op(OP_SELECTION_MERGE, SMM, 0); op(OP_BRANCH_CONDITIONAL, hok, SMM, SB)
    op(OP_LABEL, SB); op(OP_BRANCH, SH)
    op(OP_LABEL, SH)
    ins.append([7 << 16 | OP_PHI, ity, x0, I[0], SB, x1, SC]); ins.append([7 << 16 | OP_PHI, ity, s0, I[4096], SB, s1, SC])
    op(OP_LOOP_MERGE, SM, SC, LOOP_DONT_UNROLL); op(OP_BRANCH, SBB)
    op(OP_LABEL, SBB)
    nx = val(OP_IADD, ity, x0, s0)
    ins.append([6 << 16 | OP_SELECT, ity, x1, lt(word(nx, 1), word(nx, 0), y, x, eq=True), nx, x0])
    ins.append([5 << 16 | OP_SHR_LOGICAL, ity, s1, s0, I[1]])
    more = val(OP_INOT_EQUAL, bty, s1, I[0])
    op(OP_BRANCH, SC); op(OP_LABEL, SC); op(OP_BRANCH_CONDITIONAL, more, SH, SM)
    op(OP_LABEL, SM); op(OP_BRANCH, SMM)
    op(OP_LABEL, SMM)
    idx = val(OP_PHI, ity, hidx_i, LB, x1, SM)
    ws = [word(idx, w) for w in range(6)]
    found = val(OP_LOGICAL_AND, bty, lt(ws[1], ws[0], y, x, eq=True), lt(y, x, ws[3], ws[2]))
    for v, wv in zip(cache, ws):
        op(OP_STORE, v, val(OP_SELECT, u32, found, wv, val(OP_LOAD, u32, v)))
    pl, ph = add64(x, y, ws[4], ws[5])
    sink = [word(I[0], 6), word(I[0], 7)]
    got = access(val(OP_SELECT, u32, found, pl, sink[0]), val(OP_SELECT, u32, found, ph, sink[1]))
    ins.append([6 << 16 | OP_VECTOR_INSERT_DYNAMIC, v4, acc2, acc, val(OP_SELECT, u32, found, got, U[0]), k0])
    ins.append([5 << 16 | OP_IADD, u32, k1, k0, U[1]])
    more2 = val(OP_ULESS_THAN, bty, k1, m.add_const(u32, n))
    op(OP_BRANCH, LC); op(OP_LABEL, LC); op(OP_BRANCH_CONDITIONAL, more2, LH, LM)
    op(OP_LABEL, LM); op(OP_BRANCH, M)
    op(OP_LABEL, M)
    op(OP_RETURN_VALUE, val(OP_PHI, v4, hr, HOT, acc2, LM))
    op(OP_FUNCTION_END)
    m.ins += ins
    return fid


def lower_image_offsets(m):
    """Lower dynamic Offset operands on OpImageFetch instructions to explicit coordinate addition (OpIAdd),
    satisfying Vulkan 1.3 SPIR-V VUID-RuntimeSpirv-Offset-10213."""
    type_of = {}
    for x in m.ins:
        if len(x) >= 3 and x[1] in m.types:
            type_of[x[2]] = x[1]

    out = []
    changed = False

    for x in m.ins:
        op = x[0] & 0xffff
        if op == 95 and len(x) >= 6:   # 95: OpImageFetch
            mask = x[5]
            if mask & 0x10:   # Offset operand
                off_idx = 6
                if mask & 0x01: off_idx += 1
                if mask & 0x02: off_idx += 1
                if mask & 0x04: off_idx += 2
                if mask & 0x08: off_idx += 1

                offset_id = x[off_idx]
                coord_id = x[4]
                coord_type = type_of.get(coord_id) or type_of.get(offset_id)
                new_coord = m.new_id()
                out.append([5 << 16 | OP_IADD, coord_type, new_coord, coord_id, offset_id])

                new_mask = mask & ~0x10
                new_x = x[:4] + [new_coord]
                if new_mask:
                    new_x.append(new_mask)
                    cur = 6
                    for bit, cnt in [(0x01, 1), (0x02, 1), (0x04, 2), (0x08, 1), (0x10, 1), (0x20, 1), (0x40, 1), (0x80, 1)]:
                        if mask & bit:
                            if bit != 0x10: new_x.extend(x[cur:cur + cnt])
                            cur += cnt
                new_x[0] = (len(new_x) << 16) | op
                out.append(new_x)
                changed = True
                continue
        out.append(x)
    if changed:
        m.ins = out
        m.index()
    return lower_sample_offsets(m) or changed


def txq_extra_components(m):
    """A size query's component past the image's dimensions (TXQ's z of a 2D texture: the translator's GenerateTextureQuerySize extracts texOp.Index from a vector of
    `dimensions` components, which spirv-val refuses: "Vector access is out of bounds") taken as 1, the depth the hardware reports for an image without one (NVN's texture
    header holds depth - 1 = 0 for 2D). Splatoon Raiders: 175 effect vertex shaders (TXQ of their 2D textures with mask 0x7). The translator is where this belongs (open: a
    DMSH_CHAIN change); here so the Vulkan library has these shaders now. → the number of components replaced"""
    sizes = {x[2]: x[1] for x in m.ins if x[0] & 0xffff in (103, 104) and len(x) >= 4}   # OpImageQuerySizeLod, OpImageQuerySize: result → its type
    if not sizes: return 0
    i32 = m.find_type(OP_TYPE_INT, 32, 1)
    def past(x):
        if x[0] & 0xffff != OP_COMPOSITE_EXTRACT or len(x) != 5 or x[3] not in sizes or x[1] != i32: return False
        t = sizes[x[3]]
        return x[4] >= (m.types[t][3] if m.op(t) == OP_TYPE_VECTOR else 1)
    bad = [x for x in m.ins if past(x)]
    if not bad: return 0
    one = m.add_const(i32, 1)
    for x in bad: x[:] = [4 << 16 | OP_COPY_OBJECT, x[1], x[2], one]
    n = len(bad)
    return n


IMAGE_OPERAND_WORDS = [(0x01, 1), (0x02, 1), (0x04, 2), (0x08, 1), (0x10, 1), (0x20, 1), (0x40, 1), (0x80, 1)]   # Bias, Lod, Grad, ConstOffset, Offset, ConstOffsets, Sample, MinLod


def lower_sample_offsets(m):
    """A non-constant Offset image operand on OpImageSample{,Dref}{Implicit,Explicit}Lod (Maxwell's TEX .AOFFI with the offsets in a register) moved into the coordinates:
    coord.xy += vec(offset) / the size of the level sampled (Vulkan allows Offset only on gathers without maintenance8, VUID-RuntimeSpirv-Offset-10213; Qualcomm's driver of the
    Adreno 840 has no maintenance8). The level: Lod's value; with Grad, level 0; else OpImageQueryLod's level plus Bias; rounded and clamped to the image's levels. Exact for a
    sample of one level (nearest or bilinear, the level an integer); between two levels the hardware shifts each level by the offset in its own texels, this by the rounded
    level's. Splatoon Raiders' Hoian_UBER pixel shaders (2063291119471f6a, 5e83c6d05463b8b5, ba504f7a86f86d0c): an offset the translator keeps in a Function variable that holds
    a constant on the only path that samples. → whether a sample was changed"""
    OPS = {87: 5, 88: 5, 89: 6, 90: 6}   # opcode → the index of its image operands mask
    def todo_list(): return [k for k, x in enumerate(m.ins) if (x[0] & 0xffff) in OPS and len(x) > OPS[x[0] & 0xffff] and x[OPS[x[0] & 0xffff]] & 0x10]
    if not todo_list(): return False
    glsl = ensure_glsl(m)   # every declaration first: they move the instructions below them
    if 50 not in m.caps:   # ImageQuery
        at = max(k for k, x in enumerate(m.ins) if x[0] & 0xffff == OP_CAPABILITY)
        m.ins.insert(at + 1, [2 << 16 | OP_CAPABILITY, 50]); m.caps.append(50)
    f32 = m.add_type(OP_TYPE_FLOAT, 32); i32 = m.add_type(OP_TYPE_INT, 32, 1); v2f = m.add_type(OP_TYPE_VECTOR, f32, 2)
    ivec = {1: i32, **{q: m.add_type(OP_TYPE_VECTOR, i32, q) for q in (2, 3, 4)}}
    c0, c1 = m.add_const(i32, 0), m.add_const(i32, 1)
    type_of = {}
    for x in m.ins:
        if len(x) >= 3 and x[1] in m.types: type_of[x[2]] = x[1]
    todo = todo_list()
    lowered = {}
    for k in todo:
        x = m.ins[k]; op = x[0] & 0xffff; mi = OPS[op]; mask = x[mi]
        si, coord = x[3], x[4]
        img_t = m.types[type_of[si]][2]; it = m.types[img_t]
        dim, arrayed = it[3], it[5]
        n = {0: 1, 1: 2, 2: 3}.get(dim)
        if n is None: continue   # Offset is not allowed on cube images, nor on other dimensions
        pos, cur = {}, mi + 1
        for bit, cnt in IMAGE_OPERAND_WORDS:
            if mask & bit: pos[bit] = cur; cur += cnt
        pre = []
        def em(o, ty, *ops):
            r = m.new_id(); pre.append([(3 + len(ops)) << 16 | o, ty, r, *ops]); return r
        img = em(100, img_t, si)   # OpImage
        if 0x02 in pos: lv = x[pos[0x02]]
        elif 0x04 in pos: lv = None
        else:
            lv = em(OP_COMPOSITE_EXTRACT, f32, em(105, v2f, si, coord), 0)   # OpImageQueryLod: the level accessed
            if 0x01 in pos: lv = em(OP_FADD, f32, lv, x[pos[0x01]])
        if lv is None: level = c0
        else:
            top = em(130, i32, em(106, i32, img), c1)   # OpISub(OpImageQueryLevels, 1)
            level = em(OP_EXT_INST, i32, glsl, 45, em(110, i32, em(OP_EXT_INST, f32, glsl, 1, lv)), c0, top)   # SClamp(ConvertFToS(Round(lv)), 0, levels - 1)
        nq = n + arrayed
        size = em(103, ivec[nq], img, level)   # OpImageQuerySizeLod
        off = x[pos[0x10]]
        ct = type_of[coord]; nc = m.types[ct][3] if m.op(ct) == OP_TYPE_VECTOR else 1
        comps = [em(OP_COMPOSITE_EXTRACT, f32, coord, c) for c in range(nc)] if nc > 1 else [coord]
        for c in range(n):
            sc = size if nq == 1 else em(OP_COMPOSITE_EXTRACT, i32, size, c)
            oc = off if n == 1 else em(OP_COMPOSITE_EXTRACT, i32, off, c)
            comps[c] = em(OP_FADD, f32, comps[c], em(136, f32, em(111, f32, oc), em(111, f32, sc)))   # + float(offset) / float(size)
        nco = comps[0] if nc == 1 else em(OP_COMPOSITE_CONSTRUCT, ct, *comps)
        nm = mask & ~0x10
        y = x[:4] + [nco] + x[5:mi]
        if nm:
            y.append(nm)
            for bit, cnt in IMAGE_OPERAND_WORDS:
                if mask & bit and bit != 0x10: y.extend(x[pos[bit]:pos[bit] + cnt])
        y[0] = len(y) << 16 | op
        lowered[k] = pre + [y]
    if not lowered: return False
    out = []
    for k, x in enumerate(m.ins): out.extend(lowered.get(k, [x]))
    m.ins = out; m.index()
    return True


def _dominance(blks, preds):
    """A test dominates(a, b) for a function's blocks (blks in order, the first the entry; preds {block: its predecessors}) that answers as the maximal fixed point of
    dom(b) = {b} | the intersection of dom(p) over b's predecessors, with dom(entry) = {entry} and dom(b) = {b} for a block without predecessors: the dominator tree
    (Cooper, Harvey and Kennedy's iteration) of the blocks reachable from those roots, and every block dominating one that no root reaches (a cycle entered only from
    itself, whose set the fixed point leaves full). Memory O(blocks); the sets of the fixed point were O(blocks^2): 16.5 GB on a 10 MB compute module"""
    roots = [blks[0]] + [b for b in blks[1:] if not preds[b]]
    succs = {b: [] for b in blks}
    for b in blks[1:]:
        for p in preds[b]: succs[p].append(b)
    post, seen = [], set(roots)
    for r in roots:   # postorder, iteratively (a module has tens of thousands of blocks)
        stack = [(r, iter(succs[r]))]
        while stack:
            b, it = stack[-1]
            for s in it:
                if s not in seen: seen.add(s); stack.append((s, iter(succs[s]))); break
            else: post.append(b); stack.pop()
    num = {b: i for i, b in enumerate(post)}   # postorder number; the virtual root above every root is len(post)
    top = len(post); idom = {r: top for r in roots}
    order = [b for b in reversed(post) if b not in idom]
    def meet(a, b):
        while a != b:
            while a < b: a = num_idom[a]
            while b < a: b = num_idom[b]
        return a
    num_idom = {top: top}
    for r in roots: num_idom[num[r]] = top
    chg = True
    while chg:
        chg = False
        for b in order:
            nb = num[b]; new = None
            for p in preds[b]:
                np_ = num.get(p)
                if np_ is None or np_ not in num_idom: continue   # a predecessor no root reaches (its set is full), or not reached yet
                new = np_ if new is None else meet(np_, new)
            if new is not None and num_idom.get(nb) != new: num_idom[nb] = new; chg = True
    kids = collections.defaultdict(list)
    for n, d in num_idom.items():
        if n != top: kids[d].append(n)
    pre, out_, t = {}, {}, 0
    stack = [(top, False)]
    while stack:   # entry and exit times in the dominator tree
        n, done = stack.pop()
        if done: out_[n] = t; t += 1; continue
        pre[n] = t; t += 1; stack.append((n, True)); stack.extend((k, False) for k in kids[n])
    def dominates(a, b):
        nb = num.get(b)
        if nb is None: return True   # no root reaches b: every block is in its set
        na = num.get(a)
        if na is None: return False
        return pre[na] <= pre[nb] and out_[nb] <= out_[na]
    return dominates


def promote_locals(m):
    """Single-store (or unused) Function variables whose store dominates every load of them replaced by the stored value, so the translator's per-expression temporaries
    and lowered helper arguments do not leave tens of thousands of Function variables for the driver's pre-SSA passes (nir_opt_copy_prop_vars)"""
    lower_image_offsets(m)
    simplify_helpers(m)
    def _promote():
        for f in [x[2] for x in m.ins if x[0] & 0xffff == OP_FUNCTION]:
            while True:
                a, e = m.function_range(f)
                body = m.ins[a:e + 1]
                blks = [x[1] for x in body if x[0] & 0xffff == OP_LABEL]
                fvars = {x[2] for x in body if x[0] & 0xffff == OP_VARIABLE and len(x) == 4 and x[3] == SC_FUNCTION and x[2] not in m.decos}
                if not fvars or not blks: break
                preds = {b: set() for b in blks}
                bad, stores, loads = set(), {}, {}
                blk = None
                for idx, x in enumerate(body):
                    op = x[0] & 0xffff
                    if op == OP_LABEL: blk = x[1]
                    elif op == OP_BRANCH: preds[x[1]].add(blk)
                    elif op == OP_BRANCH_CONDITIONAL: preds[x[2]].add(blk); preds[x[3]].add(blk)
                    elif op == OP_SWITCH:
                        preds[x[2]].add(blk)
                        for t in x[4::2]: preds[t].add(blk)
                    if op == OP_VARIABLE and x[2] in fvars: continue
                    if op == OP_STORE and len(x) == 3 and x[1] in fvars and x[2] not in fvars:
                        if x[1] in stores: bad.add(x[1])
                        else: stores[x[1]] = (blk, idx, x[2])
                        continue
                    if op == OP_LOAD and len(x) == 4 and x[3] in fvars:
                        loads.setdefault(x[3], []).append((blk, idx, x[2]))
                        continue
                    for w in x[1:]:
                        if w in fvars: bad.add(w)
                dominates = _dominance(blks, preds)
                good, remap = set(), {}
                for v in fvars - bad:
                    ls = loads.get(v, [])
                    if v not in stores:
                        if not ls: good.add(v)
                        continue
                    sblk, sidx, val = stores[v]
                    if all(dominates(sblk, lblk) and (lblk != sblk or lidx > sidx) for lblk, lidx, _ in ls):
                        good.add(v)
                        for _, _, lres in ls: remap[lres] = val
                if not good: break
                for k in list(remap):
                    v = remap[k]
                    while v in remap: v = remap[v]
                    remap[k] = v
                out = []
                for x in body:
                    op = x[0] & 0xffff
                    if op == OP_VARIABLE and x[2] in good: continue
                    if op == OP_STORE and len(x) == 3 and x[1] in good: continue
                    if op == OP_LOAD and len(x) == 4 and x[3] in good: continue
                    if remap: _remap_operands(x, remap)
                    out.append(x)
                m.ins[a:e + 1] = out
                m.ins = [y for y in m.ins if not (y[0] & 0xffff in (OP_NAME, OP_DECORATE) and y[1] in good)]
        m.index()
    _promote()
    simplify_helpers(m)
    _promote()


def hmma_helper(m):
    """The translator's HMMA.16816 helper (HelperFunctionManager.GenerateHmma16816F16) when it has the translator's shape: 11 pointer arguments, one block, 8 + 64 accesses
    of one Workgroup array of u32 used by nothing else, two subgroup barriers → {fid, i32, u32, scratch, barrier}, else None"""
    fid = functions_named(m).get(HMMA_NAME)
    if fid is None: return None
    a, e = m.function_range(fid)
    i32 = m.ins[a][1]
    if m.int_type(i32) != (32, 1): return None
    params = [x for x in m.ins[a + 1:e] if x[0] & 0xffff == OP_FUNCTION_PARAMETER]
    if len(params) != 11 or any(m.types.get(p[1], [0, 0, 0, 0])[2:4] != [SC_FUNCTION, i32] for p in params): return None
    body = m.ins[a + 1 + len(params):e]
    if sum(1 for x in body if x[0] & 0xffff == OP_LABEL) != 1: return None
    chains = [x for x in body if x[0] & 0xffff in (OP_ACCESS_CHAIN, OP_IN_BOUNDS_ACCESS_CHAIN)]
    if len(chains) != 8 + 64 or len({x[3] for x in chains}) != 1: return None
    scratch = chains[0][3]; pwu = chains[0][1]
    if m.vars.get(scratch, (0, 0))[1] != SC_WORKGROUP or m.types.get(pwu, [0, 0, 0])[2] != SC_WORKGROUP: return None
    u32 = m.pointee(pwu)[1]; arr = m.pointee(m.vars[scratch][0])[1]
    if m.int_type(u32) != (32, 0) or m.op(arr) != OP_TYPE_ARRAY or m.types[arr][2] != u32 or m.consts[m.types[arr][3]][1][0] % 4: return None
    if any(scratch in x[1:] for k, x in enumerate(m.ins) if not (a <= k <= e) and not (x[0] & 0xffff in (OP_NAME, OP_DECORATE, OP_VARIABLE) or 19 <= x[0] & 0xffff <= 52)):
        return None   # the scratch array is the helper's alone
    groups, cur = [], None   # runs of barrier instructions: two equal ones (the translator's subgroupBarrier, or a memory barrier and an execution barrier)
    for x in body:
        if x[0] & 0xffff in (OP_CONTROL_BARRIER, OP_MEMORY_BARRIER):
            if cur is None: cur = []; groups.append(cur)
            cur.append(list(x))
        else: cur = None
    if len(groups) != 2 or groups[0] != groups[1]: return None
    return {'fid': fid, 'i32': i32, 'u32': u32, 'scratch': scratch, 'barrier': groups[0]}


def hmma_lds(m, keep=None):
    """Every call of the translator's HMMA.16816 helper (f16 sources and accumulator) replaced at its call site, with the gathers of A and B shared by the calls of a block
    that take the same fragments. The translator's body stores the lane's 8 fragment words into its warp's 256-word block of the scratch array, then reads 64 words one by one
    (both rows of A, both columns of B, for each output) and converts each half: 240 calls in agl's upscaler network on Donkey Kong Bananza, 35 ms a dispatch on the Adreno
    840 with Qualcomm's driver, 29 ms on Turnip (with its shuffles). Here the block is uvec4 rows: A row-major (16 rows of 8 words) and B transposed (8 rows), each row's k
    words in the order (k-word t, t + 4) for t = 0..3 so a lane's two words of a row are adjacent; C stays in the lane, which is the only one to need it. A gather is the
    stores, a subgroup barrier, 8 uvec4 reads (the lane's two rows of A and two columns of B, converted to f32 once), a barrier; a call whose A (or B) fragments are those of
    an earlier call in the block (the same value numbers, BlockValues) reuses its rows: agl's network multiplies two A tiles by two B tiles, so 8 calls gather 4 A and 6 B.
    The arithmetic is the translator's (f32 FMAs from C over k in order, one rounding to f16), so D is bit for bit the same. 8.0 ms instead of 35 on Qualcomm's driver,
    10.7 ms instead of 29 on Turnip (with shuffle_lds; 13 ms with the translator's body). The helper function is removed.
    The rows kept for later calls are bounded: after each call only the `keep` tiles (HMMA_KEEP) needed soonest stay (Belady's order over the block's calls, known here),
    and a tile needed again after it was dropped is gathered again. Kept tiles are live f32 registers (32 a tile): agl's network on Ocarina of Time multiplies 132 tiles in
    192 calls of one block, and keeping all of them (up to 11 live at once) made Qualcomm's driver spill 36 KB an invocation (57 ms a dispatch; 5.8 ms keeping 3, which
    gathers 192 tiles instead of 132; 7.8 ms keeping 4, 36 ms keeping 6; 9.0 ms keeping none). Caching the packed words instead and converting at each use was slower
    (the driver merges the conversions back). D is the same bits whatever is kept. HMMA_KEEP is 2, not 3: on Turnip, OoT's network takes 7.2 ms keeping 2 and 23.7
    keeping 3 (6.1 and 5.8 on Qualcomm's), and Splatoon Raiders' (372 calls on 222 tiles in one block, more values live around them) spills keeping 3 on both drivers:
    78 / 120 ms a dispatch (Qualcomm / Turnip) against 48 / 58 keeping 2 and 640 / over a second keeping all; DKB's blocks need no more than 2 (7.8 / 9.1 ms either way).
    → number of calls lowered (0: no helper of the translator's shape; the module is then unchanged)"""
    h = hmma_helper(m)
    if h is None: return 0
    if keep is None: keep = HMMA_KEEP
    fid, i32, u32, scratch = h['fid'], h['i32'], h['u32'], h['scratch']
    glsl = ensure_glsl(m)
    f32 = m.add_type(OP_TYPE_FLOAT, 32); v2f = m.add_type(OP_TYPE_VECTOR, f32, 2); uv4 = m.add_type(OP_TYPE_VECTOR, u32, 4)
    n = m.consts[m.types[m.pointee(m.vars[scratch][0])[1]][3]][1][0]
    arr4 = m.add_type(OP_TYPE_ARRAY, uv4, m.add_const(u32, n // 4))
    p_arr4 = m.add_type(OP_TYPE_POINTER, SC_WORKGROUP, arr4); p_uv4 = m.add_type(OP_TYPE_POINTER, SC_WORKGROUP, uv4); p_u = m.add_type(OP_TYPE_POINTER, SC_WORKGROUP, u32)
    k = next(k for k, x in enumerate(m.ins) if x[0] & 0xffff == OP_VARIABLE and x[2] == scratch)
    x = m.ins.pop(k); x[1] = p_arr4; m._insert_decl(x)   # the scratch array as uvec4s, declared after its new type
    CI = {v: m.add_const(i32, v) for v in (1, 2, 3, 8, 32)}
    CU = {v: m.add_const(u32, v) for v in (0, 1)}
    m.index()
    calls = 0
    def tile_keys(x, V):   # [OpFunctionCall, i32, result, fid, lane, base, a0..a3, b0, b1, c0, c1, d1 (out)] → its A and B tiles' keys
        key = [V.key_of_var(p) for p in x[4:14]]
        return ('A', *key[:6]), ('B', *key[:2], *key[6:8])
    for f in [x[2] for x in m.ins if x[0] & 0xffff == OP_FUNCTION and x[2] != fid]:
        a, e = m.function_range(f)
        seq = []; V = BlockValues(m)   # the tiles of each call in order (the value numbers as the lowering below sees them: its D1 store is a new value)
        for x in m.ins[a:e + 1]:
            if x[0] & 0xffff == OP_FUNCTION_CALL and x[3] == fid:
                seq.append(tile_keys(x, V)); V.see([3 << 16 | OP_STORE, x[14], ('d1', len(seq))])
            else: V.see(x)
        uses = {}   # tile → the call numbers (in this function's order) that take it; a block's tiles never match another block's (BlockValues)
        for i, ks in enumerate(seq):
            for q in ks: uses.setdefault(q, []).append(i)
        out = []; V = BlockValues(m); cache = {}; ci = 0
        def em(op, ty, *ops):
            r = m.new_id(); out.append([(3 + len(ops)) << 16 | op, ty, r, *ops]); return r
        def I(op, p, q): return em(op, i32, p, q)
        def cvt(w):   # a row's 8 words in k order → (lo, hi) f32 pairs
            u = [em(OP_EXT_INST, v2f, glsl, GLSL_UNPACK_HALF_2X16, x) for x in w]
            return [(em(OP_COMPOSITE_EXTRACT, f32, v, 0), em(OP_COMPOSITE_EXTRACT, f32, v, 1)) for v in u]
        def next_use(q, i): return next((j for j in uses[q] if j > i), None)
        for x in m.ins[a:e + 1]:
            op = x[0] & 0xffff
            if op == OP_LABEL: cache = {}
            if not (op == OP_FUNCTION_CALL and x[3] == fid): V.see(x); out.append(x); continue
            args = x[4:15]
            kA, kB = seq[ci]
            need = [q for q in (kA, kB) if q not in cache]
            if need:
                lane, bse = em(OP_LOAD, i32, args[0]), em(OP_LOAD, i32, args[1])
                g = I(OP_SHR_LOGICAL, lane, CI[2]); t = I(OP_BITWISE_AND, lane, CI[3])
                b4 = I(OP_SHR_LOGICAL, bse, CI[2])   # the warp's block in uvec4s: A rows at 2 r, B rows at 32 + 2 n
                vi = I(OP_SHR_LOGICAL, t, CI[1]); c0 = em(OP_BITCAST, u32, I(OP_SHL_LOGICAL, I(OP_BITWISE_AND, t, CI[1]), CI[1])); c1 = em(OP_IADD, u32, c0, CU[1])
                rowA = lambda r: I(OP_IADD, b4, I(OP_SHL_LOGICAL, r, CI[1]))
                rowB = lambda c: I(OP_IADD, b4, I(OP_IADD, CI[32], I(OP_SHL_LOGICAL, c, CI[1])))
                def st(row, w0, w1):   # the lane's k-words t and t + 4 of a row: components 2 (t & 1) and + 1 of its uvec4 t / 2
                    v4 = I(OP_IADD, row, vi)
                    for c, w in ((c0, w0), (c1, w1)): out.append([3 << 16 | OP_STORE, em(OP_ACCESS_CHAIN, p_u, scratch, v4, c), em(OP_BITCAST, u32, w)])
                g8 = I(OP_IADD, g, CI[8])
                if kA in need:
                    st(rowA(g), em(OP_LOAD, i32, args[2]), em(OP_LOAD, i32, args[4]))
                    st(rowA(g8), em(OP_LOAD, i32, args[3]), em(OP_LOAD, i32, args[5]))
                if kB in need: st(rowB(g), em(OP_LOAD, i32, args[6]), em(OP_LOAD, i32, args[7]))
                out.extend(list(b) for b in h['barrier'])
                def ld_row(r0):   # a row's 8 words, read as two uvec4, converted to (lo, hi) f32 pairs in k order
                    w = []
                    for p in range(2):
                        v4 = em(OP_LOAD, uv4, em(OP_ACCESS_CHAIN, p_uv4, scratch, I(OP_IADD, r0, CI[1]) if p else r0))
                        w += [em(OP_COMPOSITE_EXTRACT, u32, v4, c) for c in range(4)]
                    return cvt([w[2 * (kw % 4) + kw // 4] for kw in range(8)])
                if kA in need: cache[kA] = [ld_row(rowA(g)), ld_row(rowA(g8))]
                if kB in need:
                    t2 = I(OP_SHL_LOGICAL, t, CI[1]); cache[kB] = [ld_row(rowB(t2)), ld_row(rowB(I(OP_IADD, t2, CI[1])))]
                out.extend(list(b) for b in h['barrier'])
            A, B = cache[kA], cache[kB]
            d = []
            for q in range(2):   # D rows g, g + 8: the translator's order, C then k = 0..15 per output
                cv = em(OP_EXT_INST, v2f, glsl, GLSL_UNPACK_HALF_2X16, em(OP_BITCAST, u32, em(OP_LOAD, i32, args[8 + q])))
                acc = [em(OP_COMPOSITE_EXTRACT, f32, cv, 0), em(OP_COMPOSITE_EXTRACT, f32, cv, 1)]
                for j in range(2):
                    for kw in range(8):
                        for hh in range(2):
                            acc[j] = em(OP_EXT_INST, f32, glsl, GLSL_FMA, A[q][kw][hh], B[j][kw][hh], acc[j])
                d.append(em(OP_BITCAST, i32, em(OP_EXT_INST, u32, glsl, GLSL_PACK_HALF_2X16, em(OP_COMPOSITE_CONSTRUCT, v2f, *acc))))
            st_out = [3 << 16 | OP_STORE, args[10], d[1]]
            out.append(st_out); V.see(st_out)
            out.append([4 << 16 | OP_COPY_OBJECT, i32, x[2], d[0]])
            nu = {q: next_use(q, ci) for q in cache}   # tiles kept for later calls: at most `keep`, those needed soonest (the others are gathered again)
            cache = {q: v for q, v in cache.items() if nu[q] is not None and q in sorted((q for q in nu if nu[q] is not None), key=nu.get)[:keep]}
            calls += 1; ci += 1
        m.ins[a:e + 1] = out
    a, e = m.function_range(fid)   # the helper, no longer called
    gone = {y[2] for y in m.ins[a:e + 1] if len(y) > 2 and (y[0] & 0xffff) not in NO_RESULT_OPS} | {y[1] for y in m.ins[a:e + 1] if y[0] & 0xffff == OP_LABEL}
    del m.ins[a:e + 1]
    m.ins = [y for y in m.ins if not (y[0] & 0xffff in (OP_NAME, OP_DECORATE) and y[1] in gone)]
    m.index()
    promote_locals(m)
    return calls
