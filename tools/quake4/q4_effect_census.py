#!/usr/bin/env python3
"""Quake 4 BSE effects (.fx): an independent census of the text, for test_effect_parse.sh.

  census <dir>                 every effects/**/*.fx under <dir>: how many of each segment,
                               particle, keyword, parameter, domain, modifier and envelope list the
                               files hold, and how many numbers (and their sum) - as COUNT / SUM
                               lines in the form tests/effect_parse_test.cpp prints them from the
                               engine parser's stored structures
  compare <parsed> <census>    the two must agree exactly (sums to 1e-6 relative): proof the parser
                               stored everything the files say, and nothing else
  refs <q4base> <parsed>       resolves the REF / TRAIL lines the parser test printed against the
                               retail paks: tables, materials, sound shaders, effects, entity defs
                               and models the effects name that the game data does not have

Written from the files, not from the engine's parser: its own tokenizer and its own walk of the
grammar (docs/quake4-bse.md). Never starts the game, only reads files.
"""

import glob
import os
import re
import sys
import zipfile
from collections import Counter, defaultdict

TOKEN = re.compile(r'''
   (?P<ws>[ \t\r\n]+)
 | (?P<lc>//[^\n]*)
 | (?P<bc>/\*.*?\*/)
 | (?P<str>"[^"\n]*")
 | (?P<num>-?(?:\d+\.?\d*|\.\d+))
 | (?P<punct>[{}(),;=\[\]])
 | (?P<name>[^\s{}(),;"=\[\]]+)
''', re.X | re.S)


def tokens(text):
    out = []
    for m in TOKEN.finditer(text):
        kind = m.lastgroup
        if kind in ('ws', 'lc', 'bc'):
            continue
        value = m.group(kind)
        if kind == 'str':
            value = value[1:-1]
        out.append((kind, value))
    return out


SEGMENTS = {'emitter', 'spawner', 'trail', 'decal', 'light', 'sound', 'effect', 'shake', 'doublevision', 'tunnel', 'delay'}
PARTICLES = {'sprite', 'line', 'oriented', 'electricity', 'debris', 'model', 'linked', 'decal', 'light'}
PARMS = {'position', 'velocity', 'acceleration', 'friction', 'size', 'length', 'tint', 'fade', 'rotate', 'angle', 'offset'}
DOMAINS = {'point', 'line', 'box', 'sphere', 'cylinder', 'spiral'}
MODIFIERS = {'surface', 'relative', 'cone', 'useendorigin', 'linearspacing', 'attenuate', 'inverseattenuate'}
SEG_NUMBERS = {'start', 'duration', 'count', 'density', 'attenuation', 'volume', 'freqshift', 'detail', 'scale', 'particlecap'}
SEG_NAMES = {'soundshader', 'effect'}
SEG_FLAGS = {'locked', 'constant', 'attenuateemitter', 'inverseattenuateemitter'}
PART_NUMBERS = {'duration', 'gravity', 'trailtime', 'trailcount', 'fork', 'jitterrate', 'jittersize', 'tiling'}
PART_NAMES = {'material', 'entitydef', 'model', 'jittertable', 'trailtype', 'trailmaterial'}
PART_FLAGS = {'persist', 'generatednormal', 'generatedoriginnormal', 'flipnormal', 'generatedline', 'shadows', 'specular'}


class Census:
    def __init__(self):
        self.count = Counter()
        self.sum = defaultdict(float)
        self.toks = []
        self.i = 0

    def peek(self):
        return self.toks[self.i] if self.i < len(self.toks) else ('eof', '')

    def take(self):
        tok = self.peek()
        self.i += 1
        return tok

    def expect(self, value):
        kind, got = self.take()
        if got != value:
            raise ValueError("expected '%s', found '%s'" % (value, got))

    def numbers(self, group):
        values = []
        while True:
            kind, value = self.take()
            if kind != 'num':
                raise ValueError("expected a number, found '%s'" % value)
            values.append(float(value))
            if self.peek()[1] != ',':
                break
            self.take()
        self.count[group + '.numbers'] += len(values)
        self.sum[group + '.sum'] += sum(values)
        return values

    def effect(self):
        self.count['decls'] += 1
        self.expect('{')
        while self.peek()[1] != '}':
            kind, value = self.take()
            key = value.lower()
            if key in ('size', 'cutoffdistance'):
                self.count['effect.' + key] += 1
                self.numbers('values')
            elif key in SEGMENTS:
                self.count['segment.' + key] += 1
                if self.peek()[1] != '{':
                    self.take()                                  # the segment's name
                self.segment()
            else:
                raise ValueError("unknown effect keyword '%s'" % value)
        self.take()

    def segment(self):
        self.expect('{')
        while self.peek()[1] != '}':
            kind, value = self.take()
            key = value.lower()
            if key in SEG_NUMBERS:
                self.count['segment.key.' + key] += 1
                self.numbers('values')
            elif key in SEG_NAMES:
                self.count['segment.key.' + key] += 1
                self.take()
            elif key in SEG_FLAGS:
                self.count['segment.flag.' + key] += 1
            elif key in PARTICLES and self.peek()[1] == '{':
                self.count['particle.' + key] += 1
                self.particle()
            else:
                raise ValueError("unknown segment keyword '%s'" % value)
        self.take()

    def particle(self):
        self.expect('{')
        while self.peek()[1] != '}':
            kind, value = self.take()
            key = value.lower()
            if key in PART_NUMBERS:
                self.count['particle.key.' + key] += 1
                self.numbers('values')
            elif key in PART_NAMES:
                self.count['particle.key.' + key] += 1
                name = self.take()[1].lower()
                if key == 'trailtype':
                    self.count['particle.trail.' + (name if name in ('motion', 'burn') else 'segment')] += 1
            elif key == 'blend':
                self.count['particle.flag.blend'] += 1
                self.take()
            elif key in PART_FLAGS:
                self.count['particle.flag.' + key] += 1
            elif key in ('start', 'motion', 'end'):
                self.stage(key)
            elif key in ('impact', 'timeout'):
                self.count['particle.key.' + key] += 1
                self.event(key)
            else:
                raise ValueError("unknown particle keyword '%s'" % value)
        self.take()

    def event(self, which):
        self.expect('{')
        while self.peek()[1] != '}':
            kind, value = self.take()
            key = value.lower()
            if key == 'effect':
                self.count['particle.%s.effect' % which] += 1
                self.take()
            elif which == 'impact' and key in ('bounce', 'remove'):
                self.count['particle.key.' + key] += 1
                self.numbers('values')
            else:
                raise ValueError("unknown %s keyword '%s'" % (which, value))
        self.take()

    def stage(self, stage):
        self.expect('{')
        while self.peek()[1] != '}':
            kind, parm = self.take()
            parm = parm.lower()
            if parm not in PARMS:
                raise ValueError("unknown parameter '%s'" % parm)
            self.count['stage.%s.%s' % (stage, parm)] += 1
            self.expect('{')
            kind, value = self.take()
            key = value.lower()
            if stage == 'motion':
                if key != 'envelope':
                    raise ValueError("expected an envelope, found '%s'" % value)
                self.take()                                      # the table
                while self.peek()[1] != '}':
                    kind, value = self.take()
                    if value.lower() not in ('count', 'offset', 'rate'):
                        raise ValueError("unknown envelope keyword '%s'" % value)
                    self.count['envelope.' + value.lower()] += 1
                    self.numbers('envelope')
            else:
                if key not in DOMAINS:
                    raise ValueError("unknown domain '%s'" % value)
                self.count['domain.' + key] += 1
                self.numbers('domain')
                while self.peek()[1] != '}':
                    kind, value = self.take()
                    if value.lower() not in MODIFIERS:
                        raise ValueError("unknown domain modifier '%s'" % value)
                    self.count['domain.flag.' + value.lower()] += 1
            self.take()
        self.take()

    def file(self, name, text):
        """The decl manager's split: [effect] <name> { ... } - a missing type word is the folder's
        default type; anything else (two retail files end in stray words) is skipped."""
        self.toks = tokens(text)
        self.i = 0
        while self.i < len(self.toks):
            start = self.i
            if self.peek()[1].lower() == 'effect':
                self.take()
            if self.i + 1 < len(self.toks) and self.peek()[1] != '{' and self.toks[self.i + 1][1] == '{':
                self.take()
                try:
                    self.effect()
                except ValueError as e:
                    raise ValueError('%s: %s' % (name, e))
            else:
                self.count['junk'] += 1
                self.i = start + 1


def read_effects(root):
    files = []
    for path in sorted(glob.glob(os.path.join(root, 'effects', '**', '*.fx'), recursive=True)):
        files.append((os.path.relpath(path, root), open(path, 'rb').read().decode('latin-1')))
    return files


def census(root):
    c = Census()
    for name, text in read_effects(root):
        c.file(name, text)
    junk = c.count.pop('junk', 0)
    for key in sorted(c.count):
        print('COUNT %s %d' % (key, c.count[key]))
    for key in sorted(c.sum):
        print('SUM %s %.6f' % (key, c.sum[key]))
    print('== census: %d decls, %d stray tokens outside decls' % (c.count['decls'], junk))


def load(path):
    counts, sums = {}, {}
    for line in open(path, encoding='latin-1'):
        parts = line.split()
        if len(parts) == 3 and parts[0] == 'COUNT':
            counts[parts[1]] = int(parts[2])
        elif len(parts) == 3 and parts[0] == 'SUM':
            sums[parts[1]] = float(parts[2])
    return counts, sums


def compare(parsed_path, census_path):
    pc, ps = load(parsed_path)
    cc, cs = load(census_path)
    bad = 0
    for key in sorted(set(pc) | set(cc)):
        if pc.get(key, 0) != cc.get(key, 0):
            print('  MISMATCH count %-40s parser %d, text %d' % (key, pc.get(key, 0), cc.get(key, 0)))
            bad += 1
    for key in sorted(set(ps) | set(cs)):
        a, b = ps.get(key, 0.0), cs.get(key, 0.0)
        if abs(a - b) > 1e-6 * max(1.0, abs(a), abs(b)):
            print('  MISMATCH sum %-42s parser %.6f, text %.6f' % (key, a, b))
            bad += 1
    print('== compare: %d counts, %d sums, %d mismatches' % (len(set(pc) | set(cc)), len(set(ps) | set(cs)), bad))
    return 1 if bad else 0


def top_level_decls(text, types=None):
    """Names of the decls in a decl file: '[type] name {' at brace depth 0 (comments stripped)."""
    names = []
    toks = tokens(text)
    depth = 0
    for i, (kind, value) in enumerate(toks):
        if value == '{':
            if depth == 0 and i > 0:
                if types is None or (i > 1 and toks[i - 2][1].lower() in types):
                    names.append(toks[i - 1][1].lower())
            depth += 1
        elif value == '}':
            depth = max(0, depth - 1)
    return names


def refs(q4base, parsed_path):
    paks = sorted(glob.glob(os.path.join(q4base, '*.pk4')))
    files = set()
    materials, tables, sounds, effects, entitydefs = set(), set(), set(), set(), set()
    for pak in paks:
        try:
            z = zipfile.ZipFile(pak)
        except zipfile.BadZipFile:
            continue
        for n in z.namelist():
            low = n.lower().replace('\\', '/')
            files.add(low)
            if low.endswith('.mtr') or low.endswith('.guide'):
                text = z.read(n).decode('latin-1')
                materials.update(top_level_decls(text))
                materials.update(m.lower() for m in re.findall(r'^\s*guide\s+(\S+)', text, re.M))
                tables.update(top_level_decls(text, {'table'}))
            elif low.endswith('.sndshd'):
                sounds.update(top_level_decls(z.read(n).decode('latin-1')))
            elif low.endswith('.def'):
                entitydefs.update(top_level_decls(z.read(n).decode('latin-1'), {'entitydef'}))
            elif low.endswith('.fx'):
                effects.update(top_level_decls(z.read(n).decode('latin-1')))

    def image(name):
        stem = os.path.splitext(name)[0]
        return any(stem + ext in files for ext in ('.tga', '.dds', '.jpg', '.png')) or 'dds/' + stem + '.dds' in files

    def sample(name):
        stem = os.path.splitext(name)[0]
        return any(stem + ext in files for ext in ('.wav', '.ogg'))

    found = {
        'table': lambda n: n in tables,
        'material': lambda n: n in materials or image(n),
        'sound': lambda n: n in sounds or sample(n),
        'effect': lambda n: n in effects,
        'entitydef': lambda n: n in entitydefs,
        'model': lambda n: n in files,
    }
    named = defaultdict(list)
    trails = []
    for line in open(parsed_path, encoding='latin-1'):
        if line.startswith('REF '):
            kind, name = line[4:].rstrip('\n').split(' ', 1)
            named[kind].append(name)
        elif line.startswith('TRAIL '):
            trails.append(line[6:].strip())
    print('== references (retail data; informational, not part of the gate)')
    for kind in sorted(named):
        check = found.get(kind)
        missing = [n for n in named[kind] if check is not None and not check(n)]
        print('  %-10s %5d named, %d not in the paks%s' % (kind, len(named[kind]), len(missing), (': ' + ', '.join(missing)) if missing else ''))
    print('  %-10s %5d trailType names with no trail segment of that name%s' % ('trail', len(trails), (': ' + '; '.join(trails)) if trails else ''))
    return 0


def main():
    if len(sys.argv) == 3 and sys.argv[1] == 'census':
        census(sys.argv[2])
        return 0
    if len(sys.argv) == 4 and sys.argv[1] == 'compare':
        return compare(sys.argv[2], sys.argv[3])
    if len(sys.argv) == 4 and sys.argv[1] == 'refs':
        return refs(sys.argv[2], sys.argv[3])
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main())
