#!/usr/bin/env python3
"""Inventory the Quake 4 pk4s (set Q4BASE=/path/to/q4base to override the default): extension counts per pak, top-level dirs, and
header samples of the format-bearing files so we can size the Doom 3 -> Quake 4
format drift precisely."""
import collections
import os
import sys
import zipfile

Q4 = os.environ.get("Q4BASE", "/mnt/linux-data/SteamLibrary/steamapps/common/Quake 4/q4base")
paks = sorted(f for f in os.listdir(Q4) if f.endswith(".pk4"))

ext_counts = collections.Counter()
dir_counts = collections.Counter()
per_pak = {}
samples = collections.defaultdict(list)
WANT = {
    ".proc": 2, ".cm": 2, ".map": 2, ".aas32": 1, ".aas48": 1, ".aas96": 1,
    ".aas250": 1, ".aas_mcc": 1, ".aas_guardian": 1, ".aas_marine": 1,
    ".md5mesh": 2, ".md5anim": 2, ".mtr": 2, ".gui": 2, ".fx": 2, ".def": 2,
    ".sndshd": 2, ".lipsync": 2, ".playback": 2, ".script": 1, ".dat": 1,
    ".lang": 1, ".skin": 1, ".table": 1, ".pb": 1, ".ai": 1, ".mattype": 1,
    ".entityDef": 1, ".env": 1, ".lwo": 0, ".ase": 0, ".dds": 0, ".tga": 0,
    ".wav": 0, ".ogg": 0, ".roq": 0, ".bik": 0, ".cfg": 1, ".txt": 1,
    ".mat": 1, ".anim": 1, ".ik": 1, ".md3": 0, ".mdr": 0, ".obj": 0,
}
extra_ext = collections.Counter()
for p in paks:
    z = zipfile.ZipFile(os.path.join(Q4, p))
    c = collections.Counter()
    for info in z.infolist():
        if info.is_dir():
            continue
        name = info.filename
        base, ext = os.path.splitext(name)
        ext = ext.lower()
        # aas variants: ".aas32" etc keep their full ext
        c[ext] += 1
        ext_counts[ext] += 1
        dir_counts[name.split("/")[0]] += 1
        if ext in WANT and len(samples[ext]) < WANT[ext]:
            try:
                data = z.read(name)[:1400]
                samples[ext].append((p, name, info.file_size, data))
            except Exception as e:  # noqa
                samples[ext].append((p, name, info.file_size, repr(e).encode()))
    per_pak[p] = c

print("== per-pak summary (top ext)")
for p in paks:
    c = per_pak[p]
    tot = sum(c.values())
    print(f"{p:22s} files={tot:6d}  " + ", ".join(f"{e}:{n}" for e, n in c.most_common(8)))
print()
print("== global extension counts")
for e, n in ext_counts.most_common():
    print(f"{e:14s} {n}")
print()
print("== top-level dirs")
for d, n in dir_counts.most_common():
    print(f"{d:20s} {n}")
print()
print("== header samples")
for ext in sorted(samples):
    for p, name, size, data in samples[ext]:
        print(f"----- [{ext}] {p}:{name} ({size} bytes)")
        try:
            txt = data.decode("utf-8", "replace")
        except Exception:
            txt = repr(data)
        # collapse to the first ~25 lines
        lines = txt.splitlines()[:28]
        for ln in lines:
            print("   " + ln[:160])
