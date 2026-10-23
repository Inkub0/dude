#!/usr/bin/env python3
"""Summary of a Quake 4 null-game map sweep (tools/quake4/smoke-maps.sh): one row per map log
(load result, load time, the camera / dressing / AAS lines, .cm header mismatches, warnings and
errors), then every distinct warning and error across the sweep with the maps it came from.

Usage: smoke_summary.py <sweep log folder>
"""
import collections
import glob
import os
import re
import sys

ESC = re.compile(r'\x1b\[[0-9;]*m')
NUMBERS = re.compile(r'(?<![A-Za-z_])-?\d+(?:\.\d+)?')


def read(path):
    with open(path, encoding='latin-1') as f:
        return [ESC.sub('', l.rstrip('\n')) for l in f]


def first(lines, pattern):
    for l in lines:
        m = re.search(pattern, l)
        if m:
            return m
    return None


def main():
    folder = sys.argv[1]
    logs = sorted(glob.glob(os.path.join(folder, '*.log')), key=os.path.getmtime)
    if not logs:
        sys.exit(f'no logs in {folder}')
    warnings = collections.defaultdict(set)
    errors = collections.defaultdict(set)
    rows = []
    for path in logs:
        name = os.path.basename(path)[:-4]
        lines = read(path)
        tail = first(lines, r'=== smoke-maps: exit (\d+), ([\d.]+) s ===')
        rc = int(tail.group(1)) if tail else None
        load = first(lines, r'^\s*(\d+) msec to load (\S+)')
        dressed = first(lines, r'null game: dressed the map with (\d+) lights, (\d+) models')
        aas = [l for l in lines if re.search(r'null game: \S+\.aas\w*: ', l) or 'is a dummy' in l or 'did not load' in l]
        cmhdr = [l for l in lines if 'its header says' in l]
        warn = [l for l in lines if l.startswith('WARNING:')]
        err = [l for l in lines if re.match(r'(ERROR|FATAL|Sys_Error|Signal|.*[Dd]evice lost|.*VK_ERROR_)', l)]
        for l in warn:
            warnings[NUMBERS.sub('#', l)].add(name)
        for l in err:
            errors[NUMBERS.sub('#', l)].add(name)
        if rc is None:
            state = 'no exit line'
        elif rc == 0 and load and dressed:
            state = 'ok'
        elif rc == 0:
            state = 'quit, map not loaded'
        elif rc == 124:
            state = 'TIMEOUT'
        elif rc > 128:
            state = f'SIGNAL {rc - 128}'
        else:
            state = f'EXIT {rc}'
        aas_areas = ','.join(m.group(1) for m in (re.search(r': (\d+) areas', l) for l in aas) if m) or ('dummy' if any('dummy' in l for l in aas) else '-')
        rows.append((name, state, f'{int(load.group(1)) / 1000:.1f}' if load else '-',
                     f'{dressed.group(1)}/{dressed.group(2)}' if dressed else '-', aas_areas,
                     len(cmhdr), len(warn), len(err)))

    print(f'{"map":18} {"result":22} {"load s":>7} {"lights/models":>14} {"aas areas":>22} {"cm hdr":>6} {"warn":>5} {"err":>4}')
    for r in rows:
        print(f'{r[0]:18} {r[1]:22} {r[2]:>7} {r[3]:>14} {r[4]:>22} {r[5]:>6} {r[6]:>5} {r[7]:>4}')
    bad = [r[0] for r in rows if r[1] != 'ok']
    print(f'\n{len(rows) - len(bad)}/{len(rows)} maps loaded and quit cleanly' + (f'; not ok: {", ".join(bad)}' if bad else ''))
    for title, table in (('errors', errors), ('warnings', warnings)):
        if not table:
            continue
        print(f'\n{len(table)} distinct {title} (numbers shown as #):')
        for text, maps in sorted(table.items(), key=lambda kv: (-len(kv[1]), kv[0])):
            where = 'all maps' if len(maps) == len(rows) else ', '.join(sorted(maps)[:6]) + (' ...' if len(maps) > 6 else '')
            print(f'  [{len(maps):2}] {text[:160]}  <- {where}')


if __name__ == '__main__':
    main()
