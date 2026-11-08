#!/usr/bin/env python3
"""A reader of NUP files (github.com/a55uka/nup SPEC.md at commit 92089de3, format 1.0-draft): one file that holds a game's base, its updates and its
add-ons, each an ExeFS and a RomFS tree (android/NUP.md). The file is only ever read.

A package is named as android/NUP.md says: `<file>.nup:<selector>[/<path in the package>]`, the selector being `base`, `update/<decimal version>` or
`addon/<16 hex digits>`, and a package standing where a package folder of a folder dump would (`game.nup:update/458752/romfs`,
`game.nup:update/458752/exefs/main`). `<file>.nup:` alone is the container's root, whose folders are `base`, `update` and `addon`.

Open() checks the file per SPEC §6.1 before anything is read from it: the superblock, the TOC's SHA-256 (before any TOC value is used), the bounds of every
record, extent and name, the regions not overlapping, the tree (sorted, unique, acyclic), and it refuses an unknown codec, package kind (>= 5) or flags bit.
A file's content is read through its record: codec 0 is a slice of the file (pread), codec 1 one zstd frame of the whole file. zstd is compression.zstd
(Python 3.14, as on the Mac's environment and the phone's embedded CPython 3.14), else backports.zstd (the same API); it is imported on the first zstd file,
so a raw-only file needs no zstd module. A small zstd file is decompressed into memory when it is first read, a large one is streamed (a seek back starts the
frame again) and goes to an unlinked temporary file only when a caller asks for its descriptor (mmap: romfs.py).
romfs.py's Path / open / os / mmap take these paths as they take paths inside a RomFS image, so tools that read a RomFS folder through romfs.py read a package.

A .nup may also be read through a descriptor in place of its path (android/NUP.md "A .nup used where it is"): DM_NUP_FD=<fd>:<path> names a path that is read
through descriptor fd (duplicated for each reader, read with pread only), whether or not anything is at that path. The phone's packer sets it for a .nup the user
picked with the system file picker, which the app may not open by any path; the tools name the file by its stand-in path as usual.

Usage: nup.py <file>.nup [packages | ls [<name>] | cat <name> | verify [<name>]]   (exit 1 on a bad file; <name> is a selector with a path)
"""
import errno, functools, hashlib, io, os, re, stat as _stat, struct, sys, tempfile

MAGIC, TOC_MAGIC = b'NUP\x01', b'TOC\x01'
SB = struct.Struct('<4sHHQQ32sQQII16s32s')     # superblock, 0x80 bytes (SPEC §2)
TOC = struct.Struct('<4sIQQQQQ')               # TOC header, 0x30 bytes (§3)
PKG = struct.Struct('<QIIIIBBHIQI20s')        # package record, 0x40 (§3.1)
DIR = struct.Struct('<IIIIQ')                  # directory record, 0x18 (§3.2)
FILE = struct.Struct('<IIQQQ32sBBH12s')       # file record, 0x50 (§3.4)
NONE = 0xFFFFFFFF
BASE, UPDATE, ADDON = 0, 1, 2
KINDS = {BASE: 'base', UPDATE: 'update', ADDON: 'addon', 3: 'addon_update', 4: 'system'}   # 3 and 4 are reserved: accepted in a file, never selected
RAW, ZSTD = 0, 1
SMALL = 64 << 20   # a zstd file up to this size is decompressed into memory at its first read; a larger one is streamed
SELECTOR = re.compile(r'(base|update/[0-9]+|addon/[0-9a-fA-F]{16})(?:/(.*))?', re.S)


class Error(ValueError): pass


# Windows has no os.pread, and os.open there opens in text mode (CR LF read as LF) unless O_BINARY is given: every descriptor of a .nup (and romfs.py's of an
# image) is opened with O_RDONLY | O_BINARY (0 elsewhere) and read with pread, os.pread where there is one, else a seek and reads under a lock (each stream has a
# descriptor of its own; the lock keeps two threads reading one stream from moving its position under each other)
O_RDONLY = os.O_RDONLY | getattr(os, 'O_BINARY', 0)
if hasattr(os, 'pread'): pread = os.pread
else:
    import threading
    _seek_lock = threading.Lock()
    def pread(fd, n, offset):
        out = []
        with _seek_lock:
            os.lseek(fd, offset, os.SEEK_SET)
            while n > 0:
                b = os.read(fd, min(n, 1 << 30))
                if not b: break
                out.append(b); n -= len(b)
        return b''.join(out)


def fd_of(path):
    """The descriptor DM_NUP_FD gives for path (`<fd>:<path>`, the paths compared after realpath), else None. Read at each call: the phone's packer runs the tools
    in one Python, with the environment of each run"""
    v = os.environ.get('DM_NUP_FD', '')
    fd, sep, named = v.partition(':')
    if not sep or not fd.isdigit() or not named: return None
    try: same = os.path.realpath(os.fsdecode(path)) == os.path.realpath(named)
    except (TypeError, ValueError): return None
    return int(fd) if same else None


def open_fd(path):
    """A read descriptor of a .nup: a duplicate of DM_NUP_FD's for its path, else the file opened (O_RDONLY | O_BINARY). The caller closes it. Read it with pread
    only: a duplicate shares its file position with the descriptor it came from"""
    fd = fd_of(path)
    return os.dup(fd) if fd is not None else os.open(path, O_RDONLY)


def stat(path):
    """os.stat of a .nup, or fstat of DM_NUP_FD's descriptor for its path"""
    fd = fd_of(path)
    return os.fstat(fd) if fd is not None else os.stat(path)


def zstd():
    """The zstd module: compression.zstd (Python 3.14), else backports.zstd (the same API)"""
    try: from compression import zstd as z
    except ImportError:
        try: from backports import zstd as z
        except ImportError: raise Error(f'Python {sys.version_info[0]}.{sys.version_info[1]} has no compression.zstd (3.14+): a .nup with zstd files needs it, '
                                        'or pip install backports.zstd') from None
    return z


class Package:
    def __init__(self, i, title_id, root, exefs, romfs, version, kind, flags):
        self.index, self.title_id, self.root, self.exefs, self.romfs, self.version, self.kind, self.flags = i, title_id, root, exefs, romfs, version, kind, flags

    @property
    def selector(self):
        return 'base' if self.kind == BASE else f'update/{self.version}' if self.kind == UPDATE else f'addon/{self.title_id:016x}' if self.kind == ADDON else None

    def __repr__(self): return f'<{KINDS.get(self.kind, self.kind)} {self.title_id:016X} v{self.version}>'


# Nodes: a directory or file record's node id (SPEC §3.3: < dir_count a directory, else a file), or a str for the container's virtual folders:
# '' the root, 'update' and 'addon' (their children are the packages), and ('pkg', i) a package's root (its exefs and romfs from the package record)
class Nup:
    def __init__(self, path):
        self.path = os.path.abspath(path)
        fd = open_fd(self.path)
        try:
            st = os.fstat(fd); self.stat = st
            if not _stat.S_ISREG(st.st_mode): raise Error(f'{path}: not a file')
            head = pread(fd, SB.size, 0)
            if len(head) < SB.size: raise Error(f'{path}: not a NUP file (shorter than its superblock)')
            magic, hsize, hver, toc_off, toc_size, toc_sha, blob, fsize, npk, flags, self.uuid, _ = SB.unpack(head)
            if magic[:3] != MAGIC[:3]: raise Error(f'{path}: not a NUP file')
            if magic != MAGIC: raise Error(f'{path}: NUP format {magic[3]}, this reader knows format 1')
            if hsize < SB.size: raise Error(f'{path}: header size {hsize:#x} is below 0x80')
            if flags: raise Error(f'{path}: unknown superblock flags {flags:#x}')
            if fsize != st.st_size: raise Error(f'{path}: {st.st_size} bytes, the superblock says {fsize} (a copy cut off, or not a whole NUP file)')
            if toc_off != hsize or blob % 16 or blob < toc_off + toc_size or blob > fsize or toc_size < TOC.size: raise Error(f'{path}: bad region bounds in the superblock')
            if not npk: raise Error(f'{path}: no packages')
            toc = pread(fd, toc_size, toc_off)
        finally: os.close(fd)
        if len(toc) != toc_size: raise Error(f'{path}: TOC cut off')
        if hashlib.sha256(toc).digest() != toc_sha: raise Error(f'{path}: the TOC does not match its SHA-256 (damaged)')
        self.toc, self.blob, self.size = toc, blob, fsize
        tm, tpk, nd, nf, pool, pool_size, _ = TOC.unpack_from(toc, 0)
        if tm != TOC_MAGIC: raise Error(f'{path}: bad TOC magic')
        if tpk != npk: raise Error(f'{path}: {npk} packages in the superblock, {tpk} in the TOC')
        self.nd, self.nf = nd, nf
        self.o_pkg = TOC.size; self.o_dir = self.o_pkg + PKG.size * npk; self.o_file = self.o_dir + DIR.size * nd; self.o_kids = self.o_file + FILE.size * nf
        if nd >= NONE or self.o_kids > toc_size or pool < self.o_kids or pool + pool_size > toc_size: raise Error(f'{path}: TOC records out of bounds')
        self.pool = pool; self.pool_end = pool + pool_size
        nkids = sum(DIR.unpack_from(toc, self.o_dir + DIR.size * d)[3] for d in range(nd))
        if self.o_kids + 4 * nkids > pool: raise Error(f'{path}: the children array runs into the string pool')
        self.kids = struct.unpack_from(f'<{nkids}I', toc, self.o_kids)
        self.packages = []
        for i in range(npk):
            tid, root, exe, rom, ver, kind, pflags, r1, r2, moff, msize, r3 = PKG.unpack_from(toc, self.o_pkg + PKG.size * i)
            if kind >= 5: raise Error(f'{path}: package {i} has unknown kind {kind}')
            if pflags & ~1: raise Error(f'{path}: package {i} has unknown flags {pflags:#x}')
            if root >= nd or any(d != NONE and d >= nd for d in (exe, rom)): raise Error(f'{path}: package {i} names a directory out of range')
            if moff and (moff < toc_off + toc_size or moff + msize > fsize): raise Error(f'{path}: package {i} meta block out of bounds')
            self.packages.append(Package(i, tid, root, exe, rom, ver, kind, pflags))
        if sum(p.kind == BASE for p in self.packages) > 1: raise Error(f'{path}: more than one base package')
        self._check_dirs(); self._check_files(); self._check_tree()

    # ---- checks (SPEC §6.1) ----
    def _name_ok(self, off, ln):
        return off + ln <= self.pool_end - self.pool

    def _check_dirs(self):
        nk, total = len(self.kids), self.nd + self.nf
        for d in range(self.nd):
            no, nl, first, count, _ = DIR.unpack_from(self.toc, self.o_dir + DIR.size * d)
            if not self._name_ok(no, nl) or first + count > nk: raise Error(f'{self.path}: directory {d} out of bounds')
            prev = None
            for s in range(first, first + count):
                k = self.kids[s]
                if k >= total: raise Error(f'{self.path}: directory {d} has a child out of range')
                n = self.name(k)
                if not n or b'/' in n or n in (b'.', b'..') or b'\0' in n: raise Error(f'{self.path}: bad name {n!r} in directory {d}')
                if prev is not None and prev >= n: raise Error(f'{self.path}: directory {d}: children unsorted or not unique ({prev!r}, {n!r})')
                prev = n

    def _check_files(self):
        ext = set()
        for i in range(self.nf):
            no, nl, size, off, stored, _, codec, cflags, r1, _ = FILE.unpack_from(self.toc, self.o_file + FILE.size * i)
            if not self._name_ok(no, nl): raise Error(f'{self.path}: file {i} name out of bounds')
            if codec not in (RAW, ZSTD): raise Error(f'{self.path}: file {self.name(self.nd + i)!r} has unknown codec {codec}')
            if cflags: raise Error(f'{self.path}: file {self.name(self.nd + i)!r} has unknown codec flags {cflags:#x}')
            if not size:
                if stored or codec: raise Error(f'{self.path}: empty file {i} with an extent')
                continue
            if codec == RAW and stored != size: raise Error(f'{self.path}: raw file {i}: extent {stored} bytes, size {size}')
            if not stored or off % 16 or off < self.blob or off + stored > self.size: raise Error(f'{self.path}: file {i} extent out of the blob region')
            ext.add((off, stored))
        end = 0   # extents may be shared (dedup) but never overlap otherwise
        for off, stored in sorted(ext):
            if off < end: raise Error(f'{self.path}: overlapping extents at {off:#x}')
            end = off + stored

    def _check_tree(self):   # no directory is its own descendant (a walk ends)
        state = bytearray(self.nd)   # 0 unseen, 1 on the path, 2 done
        for p in self.packages:
            for r in (p.root, p.exefs, p.romfs):
                if r == NONE or state[r]: continue
                stack = [(r, iter(self._dir_kids(r)))]; state[r] = 1
                while stack:
                    d, it = stack[-1]
                    k = next((k for k in it if k < self.nd), None)
                    if k is None: state[d] = 2; stack.pop(); continue
                    if state[k] == 1: raise Error(f'{self.path}: directory {k} contains itself')
                    if not state[k]: state[k] = 1; stack.append((k, iter(self._dir_kids(k))))

    # ---- records ----
    def _dir_kids(self, d):
        _, _, first, count, _ = DIR.unpack_from(self.toc, self.o_dir + DIR.size * d)
        return self.kids[first:first + count]

    def name(self, k):
        o = self.o_dir + DIR.size * k if k < self.nd else self.o_file + FILE.size * (k - self.nd)
        no, nl = struct.unpack_from('<II', self.toc, o)
        return self.toc[self.pool + no:self.pool + no + nl]

    def record(self, k):
        """(size, extent offset, extent size, sha256, codec) of a file node"""
        no, nl, size, off, stored, sha, codec, _, _, _ = FILE.unpack_from(self.toc, self.o_file + FILE.size * (k - self.nd))
        return size, off, stored, sha, codec

    def is_dir(self, k): return not isinstance(k, int) or k < self.nd

    def file_size(self, k): return 0 if self.is_dir(k) else self.record(k)[0]

    # ---- packages ----
    def default(self):
        """The package a bare `<file>.nup` names: the highest-version update, else the base"""
        ups = [p for p in self.packages if p.kind == UPDATE]
        if ups: return max(ups, key=lambda p: p.version)
        base = [p for p in self.packages if p.kind == BASE]
        if not base: raise Error(f'{self.path}: no update and no base package')
        return base[0]

    def select(self, selector):
        """The package of a selector (`base`, `update/<version>`, `addon/<title id>`; None or '' = default())"""
        if not selector: return self.default()
        m = re.fullmatch(r'base|update/([0-9]+)|addon/([0-9a-fA-F]{16})', selector)
        if not m: raise Error(f'{selector!r} is not a package selector (base, update/<version>, addon/<16 hex digits>)')
        if selector == 'base': hits = [p for p in self.packages if p.kind == BASE]
        elif m.group(1) is not None: hits = [p for p in self.packages if p.kind == UPDATE and p.version == int(m.group(1))]
        else: hits = [p for p in self.packages if p.kind == ADDON and p.title_id == int(m.group(2), 16)]
        if not hits:
            raise Error(f'{self.path}: no package {selector} (has: {", ".join(p.selector for p in self.packages if p.selector) or "none"})')
        if len(hits) > 1: raise Error(f'{self.path}: {len(hits)} packages {selector}')
        return hits[0]

    # ---- the tree ----
    def children(self, k):
        """[(name str, node)] of a directory node, sorted by name bytes"""
        if k == '':
            base = [p for p in self.packages if p.kind == BASE]
            return ([('addon', 'addon')] if any(p.kind == ADDON for p in self.packages) else []) + ([('base', ('pkg', base[0].index))] if base else []) + \
                   ([('update', 'update')] if any(p.kind == UPDATE for p in self.packages) else [])
        if k in ('update', 'addon'):
            ps = [p for p in self.packages if p.kind == (UPDATE if k == 'update' else ADDON)]
            return sorted(((p.selector.split('/')[1], ('pkg', p.index)) for p in ps), key=lambda c: c[0].encode())
        if isinstance(k, tuple):
            p = self.packages[k[1]]
            own = {'exefs': p.exefs, 'romfs': p.romfs}   # resolved through the record fields (SPEC §3.1.3), the root's other folders as they are
            kids = {os.fsdecode(self.name(c)): c for c in self._dir_kids(p.root)}
            for n, d in own.items():
                if d == NONE: kids.pop(n, None)
                else: kids[n] = d
            return sorted(kids.items(), key=lambda c: c[0].encode())
        if k >= self.nd: raise NotADirectoryError(errno.ENOTDIR, 'not a directory', self.path)
        return [(os.fsdecode(self.name(c)), c) for c in self._dir_kids(k)]

    def lookup(self, k, name):
        """The child of directory node k named name, or None"""
        if not isinstance(k, int): return dict(self.children(k)).get(name)
        if k >= self.nd: raise NotADirectoryError(errno.ENOTDIR, 'not a directory', self.path)
        want = os.fsencode(name); kids = self._dir_kids(k)
        lo, hi = 0, len(kids)
        while lo < hi:
            mid = (lo + hi) // 2
            if self.name(kids[mid]) < want: lo = mid + 1
            else: hi = mid
        return kids[lo] if lo < len(kids) and self.name(kids[lo]) == want else None

    def find(self, inner, start=''):
        """The node of a path inside the container ('' its root; 'update/458752/romfs/a/b', 'base/exefs/main'); FileNotFoundError when there is none"""
        k = start
        for part in inner.split('/'):
            if not part or part == '.': continue
            c = self.lookup(k, part)
            if c is None: raise FileNotFoundError(errno.ENOENT, 'no such file in the NUP', f'{self.path}:{inner}')
            k = c
        return k

    def node(self, rest):
        """The node `<file>.nup:<rest>` names: rest is '' (the root), a selector, or a selector and a path"""
        if not rest: return ''
        m = SELECTOR.fullmatch(rest)
        if not m:
            k = self.find(rest)   # a virtual folder ('update', 'addon') or a mistyped selector
            if isinstance(k, str): return k
            raise FileNotFoundError(errno.ENOENT, 'not a package selector', f'{self.path}:{rest}')
        try: p = self.select(m.group(1))
        except Error as e: raise FileNotFoundError(errno.ENOENT, str(e), f'{self.path}:{rest}') from None   # a path naming no package is missing, as a missing folder is
        return self.find(m.group(2) or '', ('pkg', p.index))

    def walk(self, k=''):
        """(relative path, dirs, files) of every directory under node k, top down"""
        stack = [('', k)]
        while stack:
            rel, d = stack.pop()
            kids = self.children(d)
            dirs = [n for n, c in kids if self.is_dir(c)]
            yield rel, dirs, [n for n, c in kids if not self.is_dir(c)]
            stack.extend((f'{rel}/{n}' if rel else n, c) for n, c in reversed(kids) if self.is_dir(c))

    # ---- content ----
    def open(self, k, regions=None):
        """A read-only, seekable raw stream of file node k. regions: a dict that gets {fd: (start, length)} for the stream's descriptor while it is open (romfs.py's mmap)"""
        if self.is_dir(k): raise IsADirectoryError(errno.EISDIR, 'is a directory', self.path)
        size, off, stored, _, codec = self.record(k)
        if codec == RAW or not size: return _Raw(self.path, off, size, regions)
        return _Zstd(self.path, off, stored, size, regions)

    def read(self, k):
        with self.open(k) as f: return f.read()

    def verify(self, k):
        """True when file node k's content matches its SHA-256"""
        h = hashlib.sha256()
        with self.open(k) as f:
            while b := f.read(8 << 20): h.update(b)
        return h.digest() == self.record(k)[3]


class _Raw(io.RawIOBase):
    """A file's bytes as a slice of the .nup (codec 0); fileno() is the .nup's descriptor and the slice is in regions"""
    def __init__(self, path, start, length, regions=None):
        self.fd = open_fd(path)
        self.start, self.length, self.pos, self.regions = start, length, 0, regions
        if regions is not None: regions[self.fd] = (start, length)
    def readable(self): return True
    def seekable(self): return True
    def fileno(self): return self.fd
    def tell(self): return self.pos
    def seek(self, offset, whence=0):
        pos = offset + (self.pos if whence == 1 else self.length if whence == 2 else 0)
        if whence not in (0, 1, 2) or pos < 0: raise ValueError('invalid seek')
        self.pos = pos; return pos
    def readinto(self, buffer):
        size = min(len(buffer), max(0, self.length - self.pos))
        data = pread(self.fd, size, self.start + self.pos) if size else b''
        buffer[:len(data)] = data; self.pos += len(data)
        return len(data)
    def readall(self):
        data = pread(self.fd, max(0, self.length - self.pos), self.start + self.pos) if self.pos < self.length else b''
        self.pos += len(data); return data
    def close(self):
        if not self.closed:
            # A pack the user pauses mid-read finalizes this object through __init__ that never ran (the interpreter's "Exception ignored while
            # finalizing"): with the attributes missing, that used to raise AttributeError and hide the real reason in the log
            regions = getattr(self, 'regions', None); fd = getattr(self, 'fd', -1)
            if regions is not None: regions.pop(fd, None)
            if fd >= 0: os.close(fd)
        super().close()


class _Zstd(io.RawIOBase):
    """A zstd file (codec 1: one frame of the whole file). Up to SMALL: decompressed into memory at the first read. Larger: streamed through the module's ZstdFile
    over the extent (a seek back starts the frame again). fileno() decompresses it into an unlinked temporary file ($DM_TEMP, else the system's) and reads from
    there from then on"""
    def __init__(self, path, start, stored, size, regions=None):
        self.path, self.start, self.stored, self.length, self.regions = path, start, stored, size, regions
        self.pos, self.data, self.stream, self.tmp = 0, None, None, None
        zstd()   # fails here, at open, when there is no zstd module
    def readable(self): return True
    def seekable(self): return True
    def tell(self): return self.pos
    def seek(self, offset, whence=0):
        pos = offset + (self.pos if whence == 1 else self.length if whence == 2 else 0)
        if whence not in (0, 1, 2) or pos < 0: raise ValueError('invalid seek')
        self.pos = pos; return pos
    def _bad(self, got): return Error(f'{self.path}: zstd extent at {self.start:#x} gives {got} bytes, its record says {self.length}')
    def _load(self):
        with _Raw(self.path, self.start, self.stored) as r: comp = r.readall()
        try: self.data = zstd().decompress(comp)
        except Exception as e: raise Error(f'{self.path}: zstd extent at {self.start:#x}: {e}') from None
        if len(self.data) != self.length: raise self._bad(len(self.data))
    def readinto(self, buffer):
        want = min(len(buffer), max(0, self.length - self.pos))
        if not want: return 0
        if self.tmp is not None: data = pread(self.tmp.fileno(), want, self.pos)
        elif self.length <= SMALL:
            if self.data is None: self._load()
            data = self.data[self.pos:self.pos + want]
        else:
            if self.stream is None or self.stream.tell() > self.pos:
                if self.stream is not None: self.stream.close()
                self.stream = zstd().ZstdFile(_Raw(self.path, self.start, self.stored), 'rb')
            if self.stream.tell() < self.pos: self.stream.seek(self.pos)
            data = self.stream.read(want)
            if len(data) < want: raise self._bad(self.pos + len(data))
        buffer[:len(data)] = data; self.pos += len(data)
        return len(data)
    def readall(self):
        if self.tmp is None and self.length <= SMALL:
            if self.data is None: self._load()
            data = self.data[self.pos:]; self.pos = max(self.pos, self.length); return bytes(data)
        return super().readall()
    def fileno(self):
        if self.tmp is None:
            tmp = tempfile.TemporaryFile(dir=os.environ.get('DM_TEMP') or None)
            src = _Zstd(self.path, self.start, self.stored, self.length)
            n = 0
            while b := src.read(8 << 20): tmp.write(b); n += len(b)
            src.close(); tmp.flush()
            if n != self.length: tmp.close(); raise self._bad(n)
            self.tmp, self.data = tmp, None
            if self.stream is not None: self.stream.close(); self.stream = None
            if self.regions is not None: self.regions[tmp.fileno()] = (0, self.length)
        return self.tmp.fileno()
    def close(self):
        if not self.closed:
            if self.stream is not None: self.stream.close()
            if self.tmp is not None:
                if self.regions is not None: self.regions.pop(self.tmp.fileno(), None)
                self.tmp.close()
            self.data = None
        super().close()


@functools.lru_cache(maxsize=8)
def _open(path, identity): return Nup(path)


def open_nup(path):
    """The Nup of a .nup file, kept while the file is unchanged"""
    path = os.path.abspath(os.fsdecode(path))
    st = stat(path)
    return _open(path, (st.st_dev, st.st_ino, st.st_size, st.st_mtime_ns, st.st_ctime_ns))


def split(path):
    """(the .nup file's path, the rest) of `<file>.nup:<rest>`, split at the last '.nup:', or None for any other path. The rest is always written with '/':
    on Windows a path joined to a package (pathlib, os.path.join) has '\\' after the colon (`C:\\games\\game.nup:update\\458752\\romfs`), read the same as '/'
    (a drive letter's colon is not '.nup:')"""
    p = os.fsdecode(path)
    i = p.rfind('.nup:')
    if i < 0: return None
    rest = p[i + 5:]
    if os.sep != '/': rest = rest.replace(os.sep, '/')
    return p[:i + 4], rest.strip('/')


def is_nup(path):
    """Whether path is a NUP file itself (no selector): its magic, whatever its name"""
    try:
        fd = open_fd(path)
        try: return _stat.S_ISREG(os.fstat(fd).st_mode) and pread(fd, 3, 0) == MAGIC[:3]
        finally: os.close(fd)
    except (OSError, TypeError, ValueError): return False


def resolve(path):
    """(Nup, node) of `<file>.nup:<rest>`, or None when path names no NUP (FileNotFoundError when it names one but no node in it)"""
    s = split(path)
    if not s: return None
    f, rest = s
    try: st = stat(f)
    except OSError: return None
    if not _stat.S_ISREG(st.st_mode): return None
    nup = open_nup(f)   # Error when the file is not a NUP file
    return nup, nup.node(rest)


def default_romfs(path):
    """A RomFS argument as android/NUP.md names a package's RomFS: a bare `<file>.nup` or `<file>.nup:<selector>` → `<file>.nup:<selector>/romfs`
    (the default package for a bare one); anything else as it is"""
    p = os.fsdecode(path)
    s = split(p)
    if s is None:
        if p.endswith('.nup') and is_nup(p): return f'{p}:{open_nup(p).default().selector}/romfs'
        return p
    f, rest = s
    m = SELECTOR.fullmatch(rest)
    if m and not m.group(2) and is_nup(f): return f'{f}:{m.group(1)}/romfs'
    if not rest and is_nup(f): return f'{f}:{open_nup(f).default().selector}/romfs'
    return p


def main():
    args = sys.argv[1:]
    if not args or args[0] in ('-h', '--help'): print(__doc__); return
    try:
        nup = open_nup(args[0]); cmd = args[1] if len(args) > 1 else 'packages'
        if cmd == 'packages':
            for p in nup.packages:
                print(f'{p.selector or KINDS[p.kind]}\t{p.title_id:016X}\tv{p.version}\texefs={"yes" if p.exefs != NONE else "no"}\tromfs={"yes" if p.romfs != NONE else "no"}'
                      + ('\tstandalone' if p.flags & 1 else ''))
        elif cmd == 'ls':
            k = nup.node(args[2] if len(args) > 2 else '')
            for n, c in nup.children(k): print(f'{n}/' if nup.is_dir(c) else f'{n}\t{nup.file_size(c)}')
        elif cmd == 'cat': sys.stdout.buffer.write(nup.read(nup.node(args[2])))
        elif cmd == 'verify':
            top = nup.node(args[2] if len(args) > 2 else ''); bad = n = 0
            nodes = [top] if not nup.is_dir(top) else []
            if nup.is_dir(top):
                stack = [top]
                while stack:
                    for _, c in nup.children(stack.pop()): (stack if nup.is_dir(c) else nodes).append(c)
            for k in nodes:
                n += 1
                if not nup.verify(k): bad += 1; print(f'[RED] {nup.name(k).decode(errors="replace")}: content does not match its SHA-256')
            print(f'{n} files, {bad} bad')
            if bad: sys.exit(1)
        else: sys.exit(f'unknown command {cmd}')
    except (Error, OSError) as e: sys.exit(f'[RED] {e}')


if __name__ == '__main__':
    main()
