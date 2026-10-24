#!/usr/bin/env python3
"""What stands between Raven's Quake 4 game code and DUDE: a grouped compile-error inventory.

Compiles every source of the game build (its compile_commands.json, written by
tools/quake4/build-q4game.sh) syntax-only, with every error shown, and groups what fails:
by what is missing (an undeclared name, a type, a member of a class, a call with no matching
signature, a header) and by where (Raven's game code, Raven's idlib, a DUDE header). An error
seen in many sources - one in a header - is counted once, with the number of sources it reached.

Usage: q4game_inventory.py <compile_commands.json> <game source root> [--top N]
Writes inventory.txt next to compile_commands.json and prints its summary.
"""
import collections
import concurrent.futures
import json
import os
import re
import shlex
import subprocess
import sys

ERROR = re.compile(r'^(.*?):(\d+):(\d+): (fatal error|error): (.*)$')
HIDDEN = re.compile(r"^(.*?):\d+:\d+: warning: '(.*?)' was hidden")
HIDDEN_BY = re.compile(r"^(.*?):(\d+):\d+: note: +by '(.*?)'")
RULES = [
    (r"'([\w:~]+)' was not declared in this scope", 'undeclared name'),
    (r"'([\w:~]+)' does not name a type", 'unknown type'),
    (r"'([\w:~<>, ]+)' has not been declared", 'undeclared name'),
    (r"'(?:const )?(?:class|struct|union) ([\w:<>, ]+)'(?: \{aka '[^']*'\})? has no member named '(\w+)'", 'missing member'),
    # a typedef'd type: 'renderEntity_t' {aka 'struct renderEntity_s'}
    (r"'(?:const )?([\w:<>, ]+)' \{aka '[^']*'\} has no member named '(\w+)'", 'missing member'),
    (r"request for member '(\w+)' in .*non-class type '([^']+)'", 'member of a non-class'),
    (r"cast from '([^']+)' to '([^']+)' loses precision", '64-bit cast'),
    (r"wrong number of template arguments \((\d+), should be (\d+)\)", 'template arguments (given, expected)'),
    (r"'(\w+)' is not a member of '([\w:<>, ]+)'", 'missing member (scope)'),
    (r"no matching function for call to '([\w:~]+)\(", 'no matching call'),
    (r"no matching member function for call to '(\w+)'", 'no matching call'),
    (r"no declaration matches '.*?([\w:~]+)\(", 'definition without declaration'),
    (r"redefinition of '(?:class |struct |)([\w:<>~ ]+?)'", 'redefinition'),
    (r"conflicting declaration '.*?(\w+)'", 'conflicting declaration'),
    (r"(\S+\.h): No such file or directory", 'missing header'),
    (r"invalid use of incomplete type '(?:class|struct) ([\w:]+)'", 'incomplete type'),
    (r"cannot convert '.*?' to '(.*?)'", 'conversion'),
    (r"invalid conversion from '(.*?)' to '(.*?)'", 'conversion'),
]


def classify(message):
    message = message.replace('\u2018', "'").replace('\u2019', "'")
    for pattern, kind in RULES:
        m = re.search(pattern, message)
        if m:
            return kind, '::'.join(g for g in m.groups() if g)
    return 'other', re.sub(r"'[^']*'", "'…'", message)[:120]


def compile_one(entry, source_root):
    args = shlex.split(entry['command']) if 'command' in entry else list(entry['arguments'])
    # syntax only, every error, plain text; drop the object output
    out, skip = [], False
    for a in args:
        if skip:
            skip = False
            continue
        if a in ('-o', '-MF', '-MT', '-MQ'):
            skip = True
            continue
        if a in ('-c', '-MD', '-MMD') or a.startswith('-fdiagnostics-color'):
            continue
        out.append(a)
    # -Woverloaded-virtual: a game class whose method has Raven's signature hides DUDE's virtual
    # instead of overriding it - it compiles, and the engine never calls it
    out += ['-fsyntax-only', '-fmax-errors=0', '-fdiagnostics-color=never', '-Woverloaded-virtual']
    env = dict(os.environ, LC_ALL='C')        # plain ASCII quotes in the messages
    r = subprocess.run(out, cwd=entry['directory'], capture_output=True, text=True, errors='replace', env=env)
    return entry['file'], r.stderr


def area(path, source_root, dude_neo):
    path = os.path.normpath(path)
    if path.startswith(source_root + '/'):
        rel = path[len(source_root) + 1:]
        return 'SDK ' + rel.split('/')[0] + '/', rel
    if dude_neo and path.startswith(dude_neo + '/'):
        rel = path[len(dude_neo) + 1:]
        return 'DUDE ' + rel.split('/')[0] + '/', 'DUDE:' + rel
    return 'system', path


def main():
    db_path, source_root = sys.argv[1], os.path.normpath(sys.argv[2])
    top = int(sys.argv[sys.argv.index('--top') + 1]) if '--top' in sys.argv else 40
    entries = json.load(open(db_path))
    dude_neo = None
    m = re.search(r'-I(\S+/neo)\b', entries[0].get('command', ''))
    if m:
        dude_neo = os.path.normpath(m.group(1))

    errors = {}             # (file, line, message) -> set of sources that hit it
    fatal = collections.Counter()
    hidden = {}             # hiding method -> (hidden base method, where)
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 4) as pool:
        for source, stderr in pool.map(lambda e: compile_one(e, source_root), entries):
            pending = None
            for line in stderr.splitlines():
                h = HIDDEN.match(line)
                if h:
                    # only a base declared by DUDE matters: Raven's game classes hiding Raven's own
                    # virtuals are its design
                    base_path = os.path.normpath(os.path.join(os.path.dirname(source), h.group(1)))
                    pending = h.group(2) if dude_neo and base_path.startswith(dude_neo + '/') else None
                    continue
                b = HIDDEN_BY.match(line)
                if b and pending:
                    where = area(os.path.normpath(os.path.join(os.path.dirname(source), b.group(1))), source_root, dude_neo)[1]
                    hidden.setdefault(b.group(3), (pending, f'{where}:{b.group(2)}'))
                    pending = None
                    continue
                m = ERROR.match(line)
                if not m:
                    continue
                path, ln, _col, sev, msg = m.groups()
                key = (os.path.normpath(os.path.join(os.path.dirname(source), path)) if not os.path.isabs(path) else path, int(ln), msg)
                errors.setdefault(key, set()).add(source)
                if sev == 'fatal error':
                    fatal[(path, msg)] += 1

    by_kind = collections.Counter()
    by_symbol = collections.defaultdict(lambda: [0, set(), set()])     # (kind, symbol) -> [count, areas, examples]
    by_area = collections.Counter()
    for (path, ln, msg), sources in errors.items():
        kind, symbol = classify(msg)
        where, rel = area(path, source_root, dude_neo)
        by_kind[kind] += 1
        by_area[where] += 1
        entry = by_symbol[(kind, symbol)]
        entry[0] += 1
        entry[1].add(where)
        if len(entry[2]) < 2:
            entry[2].add(f'{rel}:{ln}')

    failing = {s for v in errors.values() for s in v}
    lines = [f'{len(entries)} sources, {len(failing)} with errors, {len(errors)} distinct errors',
             '', 'by kind:'] + [f'  {n:6}  {k}' for k, n in by_kind.most_common()]
    lines += ['', 'by where the error is:'] + [f'  {n:6}  {k}' for k, n in by_area.most_common()]
    if fatal:
        lines += ['', 'fatal (the source stopped there):'] + [f'  {n:4} sources  {p}: {m}' for (p, m), n in fatal.most_common()]
    if hidden:
        lines += ['', f'hidden DUDE virtuals ({len(hidden)}: a game method that hides an engine-interface virtual instead of overriding it - compiles, never called):']
        lines += [f'  {by}  hides  {base}  ({where})' for by, (base, where) in sorted(hidden.items())]
    lines += ['', f'top {top} missing / mismatched things:']
    for (kind, symbol), (n, where, ex) in sorted(by_symbol.items(), key=lambda kv: -kv[1][0])[:top]:
        lines.append(f'  {n:5}  {kind:30} {symbol[:70]:70}  [{", ".join(sorted(where))}]  e.g. {sorted(ex)[0]}')
    report = '\n'.join(lines)
    out = os.path.join(os.path.dirname(db_path), 'inventory.txt')
    with open(out, 'w') as f:
        f.write(report + '\n\nall:\n')
        for (kind, symbol), (n, where, ex) in sorted(by_symbol.items(), key=lambda kv: -kv[1][0]):
            f.write(f'{n:5}  {kind:30} {symbol}  [{", ".join(sorted(where))}]  {", ".join(sorted(ex))}\n')
    print(report)
    print(f'\nfull list: {out}')


if __name__ == '__main__':
    main()
