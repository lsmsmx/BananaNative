#!/usr/bin/env python3
# The translator's source tree digest: every C# source and project file the translator library is compiled from (dmsh_xlate/Program.cs and its csproj, the
# Android library's own files in dmsh_xlate_android/, and the Ryujinx projects it references: Ryujinx.Graphics.Shader, Ryujinx.Common, Spv.Generator), by path from
# the repository root, in sorted order. dmsh_xlate_android/build.sh embeds it in libdmsh_xlate.so (resource "tree", printed by --tree-sha); vk_android_build.py
# refuses a translator library whose digest is not this tree's. The program form's check is the lock's dll fingerprints (tool_lock); the library form has no dlls,
# and its embedded Program.cs digest (--source-sha) alone let a library built before a Ryujinx change pass (found 2026-11-03: a Mac library from before the
# Ampere tensor fix made 28 compute shaders differ). Not a DMSH_CHAIN file: it changes no translation.
# The APK build checks the same before it packs a stage's library (android/app/build.gradle nxPackXlateCheck, --check-lib): the library is arm64 Android code that
# cannot run on the build machine, but NativeAOT keeps its managed resources verbatim, so the tree digest ("tree": 16 hex digits and a newline) and Program.cs
# ("src0") are in its bytes. An APK built from a stage made before a translator change failed every phone's shaders step at that check (2026-11-10).
# Usage: xlate_tree.py   print the digest (16 hex digits)
#        xlate_tree.py --check-lib <libdmsh_xlate.so>   exit 1 with [RED] lines when the library was not built from this tree (or the lock is not this tree's)
import hashlib, os, re, sys
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '../../..'))
DIRS = ['dm/gfx/tools/dmsh_xlate_android', 'dm/third_party/ryujinx/src/Ryujinx.Graphics.Shader', 'dm/third_party/ryujinx/src/Ryujinx.Common',
        'dm/third_party/ryujinx/src/Spv.Generator']
FILES = ['dm/gfx/tools/dmsh_xlate/Program.cs', 'dm/gfx/tools/dmsh_xlate/dmsh_xlate.csproj']


def files():
    out = list(FILES)
    for d in DIRS:
        for root, dirs, names in os.walk(os.path.join(REPO, d)):
            dirs[:] = sorted(x for x in dirs if x not in ('bin', 'obj'))
            out += [os.path.relpath(os.path.join(root, n), REPO).replace(os.sep, '/') for n in names if n.endswith(('.cs', '.csproj'))]
    return sorted(out)


def sha():
    h = hashlib.sha256()
    for f in files(): h.update(f.encode() + b'\0' + open(os.path.join(REPO, f), 'rb').read() + b'\0')
    return h.hexdigest()[:16]


def embedded(data):
    """the tree digests a translator library's bytes hold (build.sh embeds this script's output, 16 hex digits and a newline, as the resource "tree"); [] for a
    library from before --tree-sha. The digest is delimited, so a longer hex run (a public key token, a hash) is not taken for one"""
    return sorted({m.decode() for m in re.findall(rb'(?<![0-9A-Za-z])([0-9a-f]{16})\n', data)})


def open_bytes(path):
    with open(path, 'rb') as f: return f.read()


FIX = ('run android/ondevice/build.sh <work dir> tools (it runs dm/gfx/tools/dmsh_xlate_android/build.sh lib and copies libdmsh_xlate.so into <work dir>/stage/lib), '
       'then build the APK again')


def check_lib(path):
    """→ what is wrong with the translator library at path for this tree ([] when nothing): its embedded tree digest, its embedded Program.cs, and the lock's
    xlate_source, which vk_android_build.py check_translator compares on the phone (the same three, read here without running the library)"""
    sys.path.insert(0, HERE)
    import dmsh_chain
    with open(path, 'rb') as f: data = f.read()
    want, got, bad = sha(), embedded(data), []
    if want not in got:
        bad.append(f'{path}: built from source tree {", ".join(got) or "unknown (no embedded tree digest: a library from before --tree-sha)"}, this tree is {want}')
    elif open_bytes(os.path.join(HERE, 'dmsh_xlate/Program.cs')) not in data:
        bad.append(f'{path}: its embedded dmsh_xlate/Program.cs is not this tree\'s')
    with open(dmsh_chain.LOCK) as f: lock = dict(l.rstrip('\n').split('\t', 1) for l in f if '\t' in l and not l.startswith('#'))
    if lock.get('xlate_source') != dmsh_chain.xlate_source_sha():
        bad.append(f'{dmsh_chain.LOCK}: xlate_source {lock.get("xlate_source")}, this tree\'s Program.cs is {dmsh_chain.xlate_source_sha()}')
    return bad


if __name__ == '__main__':
    a = sys.argv[1:]
    if len(a) == 2 and a[0] == '--check-lib':
        if not os.path.isfile(a[1]): sys.exit(f'[RED] {a[1]}: no such file')
        bad = check_lib(a[1])
        if bad: sys.exit('\n'.join(f'[RED] stale translator library: {b}' for b in bad) + f'\n[RED] {FIX}')
        print(f'{a[1]}: tree {sha()}')
    elif a: sys.exit('usage: xlate_tree.py [--check-lib <libdmsh_xlate.so>]')
    else: print(sha())
