#!/usr/bin/env python3
"""Do the engine's own "#str_" ids resolve on every title?

Collects every "#str_<n>" literal of the engine source (not the game libraries; comments
stripped), then resolves each the way the engine does: the loaded tables by exact id, for
Quake 4 also its renumbered form ("#str_04343" -> "#str_104343", idLangDict::FindString), and
the English fallbacks of idCommonLocal::AddEngineStringFallbacks (read from Common.cpp, with
their replace-the-renumbered-form flag). Reports ids that stay unresolved per title, fallback
entries no longer used by the engine, and - for review - the ids whose Quake 4 text reads
differently from Doom 3's.

Usage: check_engine_strings.py [q4base]        exit 1 if any id is unresolved
"""
import glob
import os
import re
import sys
import zipfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
ENGINE_DIRS = ('framework', 'ui', 'sys', 'renderer', 'sound', 'cm', 'tools', 'idlib', 'nullgame')


def strip_comments(text):
    return re.sub(r'//[^\n]*|/\*.*?\*/', '', text, flags=re.S)


def engine_ids():
    ids = {}
    for d in ENGINE_DIRS:
        for path in glob.glob(os.path.join(ROOT, 'neo', d, '**', '*.[ch]*'), recursive=True):
            text = strip_comments(open(path, encoding='latin-1').read())
            for m in re.finditer(r'"(#str_\d+)"', text):
                ids.setdefault(m.group(1), set()).add(os.path.relpath(path, os.path.join(ROOT, 'neo')))
    return ids


def fallbacks():
    text = open(os.path.join(ROOT, 'neo', 'framework', 'Common.cpp'), encoding='latin-1').read()
    body = text[text.index('void idCommonLocal::AddEngineStringFallbacks'):]
    body = body[:body.index('};')]
    return {m.group(1): m.group(2) == 'true' for m in re.finditer(r'\{\s*"(#str_\d+)",\s*"(?:[^"\\]|\\.)*",\s*(true|false)\s*\}', body)}


def tables(patterns):
    d = {}
    for pattern in patterns:
        for pak in sorted(glob.glob(pattern)):
            try:
                z = zipfile.ZipFile(pak)
            except zipfile.BadZipFile:
                continue
            for n in z.namelist():
                low = n.lower()
                if low.startswith('strings/english') and low.endswith('.lang'):
                    for m in re.finditer(r'"(#str_\d+)"\s+"((?:[^"\\]|\\.)*)"', z.read(n).decode('latin-1')):
                        d[m.group(1).lower()] = m.group(2)
    return d


def renumbered(sid):
    digits = sid[5:]
    return '#str_1' + digits if len(digits) == 5 and digits[0] == '0' else None


def main():
    q4base = sys.argv[1] if len(sys.argv) > 1 else '/mnt/linux-data/SteamLibrary/steamapps/common/Quake 4/q4base'
    ids, fb = engine_ids(), fallbacks()
    d3 = tables([os.path.join(ROOT, 'base', 'pak00*.pk4')])
    q4 = tables([os.path.join(q4base, '*.pk4')])
    failed = 0
    for title, table, raven in (('Doom 3', d3, False), ('Quake 4', q4, True)):
        missing = []
        for sid in sorted(ids):
            exact = sid.lower() in table
            other = raven and renumbered(sid) is not None and renumbered(sid).lower() in table
            if not ( exact or other or sid in fb ):
                missing.append(sid)
        failed += len(missing)
        print(f'{title}: {len(ids)} engine ids, unresolved {len(missing)}' + (': ' + ', '.join(missing) if missing else ''))
    stale = sorted(set(fb) - set(ids))
    if stale:
        print('fallbacks the engine no longer uses:', ', '.join(stale))
    print('Quake 4 renumbered texts that read differently from Doom 3 (review; replaced ones marked *):')
    for sid in sorted(ids):
        r = renumbered(sid)
        if r and r.lower() in q4 and sid.lower() in d3 and q4[r.lower()].strip().lower() != d3[sid.lower()].strip().lower():
            mark = '*' if fb.get(sid) else ' '
            print(f'  {mark} {sid}: d3 {d3[sid.lower()][:48]!r}  q4 {q4[r.lower()][:48]!r}')
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
