#!/usr/bin/env python3
"""Is DUDE still binary-compatible with game libraries built for dhewm3?

A mod's game library (base.so / gamex86.dll built against dhewm3's headers) must load and run
in DUDE unchanged (docs/quake4-port.md 5.6, "the dhewm3 game ABI stays frozen"). This compares,
between dhewm3's headers (a git ref, default upstream/master = dhewm3 1.5.5) and this checkout's:

  - every struct / class layout: field offsets, sizes, alignment - identical, except fields
    appended to the end of an object the engine allocates and the game only reads through
    pointers (ALLOW_APPEND below, each with its reason);
  - every vtable: dhewm3's slots must be a prefix of DUDE's (new virtuals only at the end) for
    the engine's interfaces, and identical for the ones the game implements (GAME_IMPLEMENTED);
  - every enumerator's value.

The headers are the ones dhewm3's own game code (neo/game, neo/d3xp) includes, transitively,
outside the game folders; they are compiled with clang as a game library is (-DGAME_DLL,
x86_64). Exit status 1 on any break. Needs clang++ and the upstream remote.

Usage: tools/check_dhewm3_abi.py [git ref of dhewm3]
"""
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))

# objects the engine allocates and the game library only reaches through engine pointers, so
# fields appended at their end move nothing a dhewm3 build reads
ALLOW_APPEND = {
    'idMaterial': 'decl, allocated by the decl manager; inline getters read the old fields at their old offsets',
    'idSoundShader': 'decl, allocated by the decl manager',
    'idDecl': 'never grows here; listed with its decls',
    'srfTriangles_s': 'allocated by the engine (R_AllocStaticTriSurf); games hold pointers',
    'idAASFile': 'allocated by the AAS file manager; games read it through its accessors',
    'idAASSettings': 'embedded only in idAASFile (engine-allocated), read through GetSettings()',
    'glconfig_s': 'the engine global glConfig - a game library cannot link to it',
}
# interfaces the game library implements and the engine calls: their vtables must not change at all
GAME_IMPLEMENTED = {'idGame', 'idGameEdit'}

FLAGS = ['-std=gnu++17', '-w', '-DGAME_DLL', '-D_DEBUG', '-DD3_ARCH="x86_64"', '-DD3_HAS_SIMD_SSE41=1',
         '-DD3_IS_BIG_ENDIAN=0', '-DD3_OSTYPE="linux"', '-DD3_SIZEOFPTR=8', '-msse4.1']


def git_show(ref, path):
    r = subprocess.run(['git', '-C', ROOT, 'show', f'{ref}:{path}'], capture_output=True, text=True, errors='replace')
    return r.stdout if r.returncode == 0 else None


def game_visible_headers(ref):
    """Engine headers dhewm3's game code includes, transitively, outside neo/game and neo/d3xp."""
    files = subprocess.run(['git', '-C', ROOT, 'ls-tree', '-r', '--name-only', ref, 'neo/game', 'neo/d3xp'],
                           capture_output=True, text=True).stdout.split()
    inc = re.compile(r'#include\s+"([^"]+)"')
    seen, todo = set(), []

    def resolve(src, name):
        rel = os.path.normpath(os.path.join(os.path.dirname(src), name))
        return rel if git_show(ref, rel) is not None else os.path.normpath('neo/' + name)
    for f in files:
        if f.endswith(('.cpp', '.h')):
            todo += [resolve(f, m) for m in inc.findall(git_show(ref, f) or '')]
    while todo:
        p = todo.pop()
        if p in seen or p.startswith(('neo/game/', 'neo/d3xp/')):
            continue
        text = git_show(ref, p)
        if text is None:
            continue
        seen.add(p)
        todo += [resolve(p, m) for m in inc.findall(text)]
    return sorted(h[4:] for h in seen if h.endswith('.h'))


def clang(tree, source, extra, work, tag):
    out = os.path.join(work, f'{tag}.out')
    cmd = ['clang++'] + FLAGS + ['-I' + tree, '-I' + os.path.join(ROOT, 'build')] + extra + [source]
    with open(out, 'w') as o:
        r = subprocess.run(cmd, stdout=o, stderr=subprocess.PIPE, text=True, cwd=work)
    return r.returncode, open(out, encoding='latin-1').read(), r.stderr


def typedef_names(tree):
    """(relative path, line) of each 'typedef struct/union {' in the tree's headers -> its name:
    clang's layout dump calls such a record '(unnamed at <file>:<line>:<col>)'."""
    names = {}
    for dp, _dn, fn in os.walk(tree):
        for f in fn:
            if not f.endswith('.h'):
                continue
            path = os.path.join(dp, f)
            text = open(path, encoding='latin-1').read()
            # blank out comments, keeping line numbers
            text = re.sub(r'/\*.*?\*/', lambda m: re.sub(r'[^\n]', ' ', m.group(0)), text, flags=re.S)
            text = re.sub(r'//[^\n]*', '', text)
            for m in re.finditer(r'\btypedef\s+(struct|union)\b(\s+\w+)?\s*\{', text):
                depth, i = 0, text.index('{', m.start())
                while i < len(text):
                    depth += {'{': 1, '}': -1}.get(text[i], 0)
                    if depth == 0:
                        break
                    i += 1
                name = re.match(r'\s*(\w+)', text[i + 1:])
                if name:
                    line = text.count('\n', 0, m.start(1)) + 1
                    names[(os.path.relpath(path, tree), line)] = name.group(1)
    return names


def normaliser(tree, typedefs):
    prefix = tree.rstrip('/') + '/'

    def fix(text):
        def unnamed(m):
            path, line = m.group(1), int(m.group(2))
            rel = path[len(prefix):] if path.startswith(prefix) else os.path.basename(path)
            return typedefs.get((rel, line), f'(unnamed at {rel})')
        return re.sub(r'\(unnamed at ([^:)]+):(\d+):\d+\)', unnamed, text)
    return fix


def parse_records(dump, fix=lambda t: t):
    records = {}
    dump = fix(dump)
    for block in dump.split('*** Dumping AST Record Layout')[1:]:
        lines = [l.rstrip() for l in block.strip('\n').split('\n') if l.strip()]
        m = re.match(r'\s*\d+ \| (?:struct|class|union) (.+)$', lines[0])
        if not m:
            continue
        name = m.group(1).strip()
        body = [l for l in lines[1:] if not l.strip().startswith('|')]
        size = ' '.join(l.strip().lstrip('|').strip() for l in lines[1:] if l.strip().startswith('|'))
        records[name] = (body, size)
    return records


def parse_vtables(dump):
    tables = {}
    for m in re.finditer(r"Vtable for '([^']+)' \(\d+ entries\)\.\n(.*?)\n\n", dump, re.S):
        entries = []
        for line in m.group(2).split('\n'):
            e = re.match(r'\s*\d+ \| (.*)$', line)
            if e:
                entries.append(e.group(1).strip())
        tables[m.group(1)] = entries
    return tables


def main():
    ref = sys.argv[1] if len(sys.argv) > 1 else 'upstream/master'
    work = tempfile.mkdtemp(prefix='dude-abi-')
    upstream = os.path.join(work, 'dhewm3')
    os.makedirs(upstream)
    archive = subprocess.run(['git', '-C', ROOT, 'archive', ref, 'neo'], capture_output=True, check=True).stdout
    subprocess.run(['tar', '-x', '-C', upstream], input=archive, check=True)
    trees = {'dhewm3': os.path.join(upstream, 'neo'), 'dude': os.path.join(ROOT, 'neo')}

    headers = game_visible_headers(ref)
    includes = '#include "sys/platform.h"\n' + ''.join(f'#include "{h}"\n' for h in headers if h != 'sys/platform.h')
    probe = os.path.join(work, 'probe.cpp')
    open(probe, 'w').write(includes)

    # 1. record layouts
    records = {}
    for tag, tree in trees.items():
        rc, out, err = clang(tree, probe, ['-fsyntax-only', '-Xclang', '-fdump-record-layouts-complete'], work, 'rec_' + tag)
        if rc:
            sys.exit(f'{tag}: the headers do not compile:\n{err[:2000]}')
        records[tag] = parse_records(out, normaliser(tree, typedef_names(tree)))

    # 2. vtables: a derived class with an out-of-line key function makes clang lay each one out
    polymorphic = sorted(n for n, (body, _) in records['dhewm3'].items()
                         if any('vtable pointer' in l for l in body) and '<' not in n and '::' not in n
                         and not n.startswith('(anonymous'))
    vtables = {}
    for tag, tree in trees.items():
        candidates = list(polymorphic)
        for _attempt in range(6):
            probe2 = os.path.join(work, f'vt_{tag}.cpp')
            body = includes + ''.join(f'struct dude_vt_{i} : {c} {{ virtual void dude_vt_key(); }};\n'
                                      f'void dude_vt_{i}::dude_vt_key() {{}}\n' for i, c in enumerate(candidates))
            open(probe2, 'w').write(body)
            rc, out, err = clang(tree, probe2, ['-c', '-o', os.devnull, '-ferror-limit=0', '-Xclang', '-fdump-vtable-layouts'],
                                 work, 'vt_' + tag)
            if rc == 0:
                break
            bad = set(int(i) for i in re.findall(r'dude_vt_(\d+)', err))
            if not bad:
                sys.exit(f'{tag}: vtable probe failed:\n{err[:2000]}')
            candidates = [c for i, c in enumerate(candidates) if i not in bad]
        found = parse_vtables(out)
        vtables[tag] = {}
        for i, c in enumerate(candidates):
            entries = found.get(f'dude_vt_{i}')
            if entries:
                vtables[tag][c] = [e for e in entries if 'dude_vt_' not in e]

    # 3. enumerators: dhewm3's names, qualified from clang's AST (an enum inside a class is
    #    Class::NAME), values printed by a program built against each tree
    rc, ast, err = clang(trees['dhewm3'], probe, ['-fsyntax-only', '-Xclang', '-ast-dump'], work, 'ast')
    names = set()
    for m in re.finditer(r"EnumConstantDecl \S+ <[^>]*> \S+(?: referenced)? (\w+) '([^']+)'", re.sub(r'\x1b\[[0-9;]*m', '', ast)):
        name, enum_type = m.group(1), m.group(2)
        if enum_type.startswith(('std::', '__')) or '<' in enum_type:
            continue
        scope = enum_type.rsplit('::', 1)[0] if '::' in enum_type.split(' (')[0] else ''
        scope = re.sub(r'::\(unnamed enum at [^)]*\)$', '', scope)
        if '(' in scope:
            continue
        names.add(f'{scope}::{name}' if scope else name)
    names = sorted(names)
    enum_values = {}
    for tag, tree in trees.items():
        candidates = list(names)
        for _attempt in range(6):
            src = os.path.join(work, f'enum_{tag}.cpp')
            open(src, 'w').write(includes + '#include <cstdio>\nint main() {\n' +
                                 ''.join(f'\tprintf( "%s %lld\\n", "{n}", (long long)( {n} ) ); // dude_enum {i}\n'
                                         for i, n in enumerate(candidates)) + '\treturn 0;\n}\n')
            exe = os.path.join(work, f'enum_{tag}')
            r = subprocess.run(['clang++'] + FLAGS + ['-ferror-limit=0', '-I' + tree, '-I' + os.path.join(ROOT, 'build'), src, '-o', exe, '-Wl,--unresolved-symbols=ignore-all'],
                               capture_output=True, text=True)
            if r.returncode == 0:
                break
            lines = open(src).read().split('\n')
            bad = set()
            for ln in re.findall(r'enum_' + tag + r'\.cpp:(\d+):', r.stderr):
                tag_m = re.search(r'// dude_enum (\d+)', lines[int(ln) - 1])
                if tag_m:
                    bad.add(int(tag_m.group(1)))
            if not bad:
                sys.exit(f'{tag}: enum probe failed:\n{r.stderr[:2000]}')
            candidates = [n for i, n in enumerate(candidates) if i not in bad]
        enum_values[tag] = dict(l.split() for l in subprocess.run([exe], capture_output=True, text=True).stdout.splitlines())
    for n in names:
        if n in enum_values['dhewm3'] and n not in enum_values['dude']:
            enum_values['dude'].setdefault(n, None)

    # compare
    breaks, notes = [], []
    for name, (body, size) in sorted(records['dhewm3'].items()):
        if name not in records['dude']:
            breaks.append(f'record {name}: gone')
            continue
        dbody, dsize = records['dude'][name]
        if (body, size) == (dbody, dsize):
            continue
        appended = dbody[:len(body)] == body
        if appended and name in ALLOW_APPEND:
            notes.append(f'record {name}: fields appended ({ALLOW_APPEND[name]})')
        else:
            first = next((i for i, (a, b) in enumerate(zip(body, dbody)) if a != b), min(len(body), len(dbody)))
            what = 'fields appended to a game-visible object' if appended else 'layout changed'
            breaks.append(f'record {name}: {what}\n      dhewm3: {body[first] if first < len(body) else "(end)"} {size}\n'
                          f'      DUDE  : {dbody[first] if first < len(dbody) else "(end)"} {dsize}')
    for name, entries in sorted(vtables['dhewm3'].items()):
        dentries = vtables['dude'].get(name)
        if dentries is None:
            breaks.append(f'vtable {name}: not laid out on DUDE')
            continue
        if entries == dentries:
            continue
        if dentries[:len(entries)] == entries and name not in GAME_IMPLEMENTED:
            notes.append(f'vtable {name}: {len(dentries) - len(entries)} virtual(s) appended')
            continue
        first = next((i for i, (a, b) in enumerate(zip(entries, dentries)) if a != b), min(len(entries), len(dentries)))
        breaks.append(f'vtable {name}: slot {first} differs\n      dhewm3: {entries[first] if first < len(entries) else "(end)"}\n'
                      f'      DUDE  : {dentries[first] if first < len(dentries) else "(end)"}')
    for n, v in sorted(enum_values['dhewm3'].items()):
        if enum_values['dude'].get(n) != v:
            breaks.append(f'enum {n}: {v} on dhewm3, {enum_values["dude"].get(n)} on DUDE')

    print(f'dhewm3 ref {ref}: {len(headers)} game-visible headers, {len(records["dhewm3"])} records, '
          f'{len(vtables["dhewm3"])} vtables, {len(enum_values["dhewm3"])} enumerators compared')
    for n in notes:
        print('  ok   ' + n)
    for b in breaks:
        print('  BREAK ' + b)
    print(f'{len(breaks)} break(s)')
    sys.exit(1 if breaks else 0)


if __name__ == '__main__':
    main()
