#!/usr/bin/env python3
"""Quake 4 sound data check against the grammar DUDE's sound code accepts.

Reads the retail paks directly (later paks override earlier ones, like the engine) and checks:
  - every sound shader (sound/*.sndshd) parses with the keywords neo/sound/snd_shader.cpp
    accepts for the Quake 4 title (unknown keywords default the whole shader);
  - every sample a shader names resolves to a file in the paks the way the engine resolves it:
    a bare path plays <path>.ogg (or <path>.wav), "sound/vo/x" plays "sound/vo_<lang>/x";
  - every maps/**/<map>.reverb area table parses and names presets that efxs/default.efx
    defines (neo/sound/snd_efxfile.cpp idMapReverb).

Samples missing from the retail paks are listed by folder: the engine plays its default
sound for those, as it does for any missing sample. Never starts the game.

Usage: q4_sound_census.py [q4base dir] [language]
"""
import collections
import os
import re
import sys
import zipfile

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
from check_guides import strip_comments   # idLexer comment rules

base = sys.argv[1] if len(sys.argv) > 1 else "/mnt/linux-data/SteamLibrary/steamapps/common/Quake 4/q4base"
lang = sys.argv[2] if len(sys.argv) > 2 else "english"

# later paks win; the engine skips the binary game*.pk4 paks
files, texts = set(), {}
for pak in sorted(p for p in os.listdir(base) if p.lower().endswith(".pk4") and not p.lower().startswith("game")):
    z = zipfile.ZipFile(os.path.join(base, pak))
    for n in z.namelist():
        low = n.lower()
        files.add(low)
        if low.endswith((".sndshd", ".reverb", ".efx")):
            texts[low] = z.read(n).decode("latin-1")

TOK = re.compile(r'"[^"\n]*"|,|\{|\}|[^\s,{}"]+')
NUM = re.compile(r"-?\d*\.?\d+$")
# keyword -> number of argument tokens (a "," counts as one), as snd_shader.cpp parses them
ARGS = {"mindistance": 1, "maxdistance": 1, "volume": 1, "volumedb": 1, "leadinvolume": 1, "soundclass": 1,
        "minsamples": 1, "description": 1, "altsound": 1, "shakedata": 2, "leadin": 1, "frequencyshift": 3,
        "reverb": 3}
FLAGS = {"mask_center", "mask_left", "mask_right", "mask_backright", "mask_backleft", "mask_lfe", "ordered",
         "no_dups", "no_flicker", "plain", "looping", "no_occlusion", "private", "antiprivate", "playonce", "global",
         "unclamped", "omnidirectional", "ondemand", "frequentlyused", "voforplayer", "no_shakes", "norandomstart"}


def resolve(name):
    n = name.strip('"').replace("\\", "/").lower()
    if n.startswith("sound/vo/"):
        n = "sound/vo_%s/%s" % (lang, n[len("sound/vo/"):])
    stem, ext = os.path.splitext(n)
    return stem + ".ogg" in files or n in files or (not ext and n + ".wav" in files)


shaders = samples = bare = 0
failed, missing = [], collections.Counter()
for path in sorted(p for p in texts if p.startswith("sound/") and p.endswith(".sndshd")):
    toks = TOK.findall(strip_comments(texts[path]))
    i = 0
    while i < len(toks):
        name = toks[i]
        i += 1
        if name.lower() == "sound" and i < len(toks) and toks[i] != "{":
            name = toks[i]
            i += 1
        if i >= len(toks) or toks[i] != "{":
            continue
        i += 1
        shaders += 1
        while i < len(toks) and toks[i] != "}":
            tok = toks[i]
            low = tok.strip('"').lower()
            i += 1
            if low in ARGS:
                args = toks[i:i + ARGS[low]]
                i += ARGS[low]
                if low in ("volume", "volumedb", "mindistance", "maxdistance", "soundclass") and not NUM.match(args[0]):
                    failed.append((path, name, f"{tok} {args[0]}"))
                    while i < len(toks) and toks[i] != "}":
                        i += 1
                elif low == "leadin":
                    samples += 1
                    if not resolve(args[0]):
                        missing[os.path.dirname(args[0].lower())] += 1
            elif low == "shakes":
                if i < len(toks) and NUM.match(toks[i]):
                    i += 1
            elif low in FLAGS:
                pass
            elif ".wav" in low or ".ogg" in low or "/" in low or "\\" in low:
                samples += 1
                if not os.path.splitext(low)[1]:
                    bare += 1
                if not resolve(tok):
                    missing[os.path.dirname(low)] += 1
            else:
                failed.append((path, name, tok))
                while i < len(toks) and toks[i] != "}":
                    i += 1
        i += 1

print(f"sound shaders: {shaders}, samples: {samples} ({bare} without an extension), "
      f"missing from the paks: {sum(missing.values())}")
for path, name, tok in failed:
    print(f"  FAIL {path}: '{name}' at '{tok}'")
if missing:
    print("  missing samples by folder (top 10):")
    for folder, n in missing.most_common(10):
        print(f"    {n:4d} {folder}")

presets = set(re.findall(r'^\s*reverb\s+"([^"]+)"', texts.get("efxs/default.efx", ""), re.M))
tables = bad = 0
for path in sorted(p for p in texts if p.endswith(".reverb")):
    tables += 1
    body = strip_comments(texts[path])
    m = re.fullmatch(r"\s*reverb\s*\{((?:\s*\{\s*\d+\s+\S+\s*\})*)\s*\}\s*", body)
    if not m:
        bad += 1
        print(f"  REVERB {path}: does not parse")
        continue
    for area, preset in re.findall(r"\{\s*(\d+)\s+(\S+)\s*\}", m.group(1)):
        if preset not in presets:
            bad += 1
            print(f"  REVERB {path}: area {area} names unknown preset '{preset}'")
print(f"reverb tables: {tables}, presets in efxs/default.efx: {len(presets)}, problems: {bad}")
sys.exit(1 if bad else 0)
