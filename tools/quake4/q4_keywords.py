#!/usr/bin/env python3
"""Keyword-level drift (set Q4BASE=/path/to/q4base to override the default): harvest the leading keywords used in Quake 4 .mtr,
.gui, .fx (BSE), .sndshd, .def(spawnclass), .skin files and compare against the
keyword sets DUDE's parsers accept (greps of Material.cpp, Window.cpp, snd_shader.cpp)."""
import collections
import os
import re
import subprocess
import sys
import zipfile

Q4 = os.environ.get("Q4BASE", "/mnt/linux-data/SteamLibrary/steamapps/common/Quake 4/q4base")
DUDE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "neo")
paks = sorted(f for f in os.listdir(Q4) if f.endswith(".pk4"))

def iter_files(exts):
    for p in paks:
        z = zipfile.ZipFile(os.path.join(Q4, p))
        for info in z.infolist():
            if info.is_dir():
                continue
            ext = os.path.splitext(info.filename)[1].lower()
            if ext in exts:
                yield p, info.filename, z.read(info.filename).decode("latin-1")

def strip_comments(txt):
    txt = re.sub(r"/\*.*?\*/", " ", txt, flags=re.S)
    txt = re.sub(r"//[^\n]*", " ", txt)
    return txt

tok_re = re.compile(r'"[^"]*"|\{|\}|\(|\)|,|[^\s"{}(),]+')

def leading_keywords(txt, depth_filter=None):
    """Return Counter of the first token on each statement line at each brace depth."""
    c = collections.Counter()
    depth = 0
    for line in strip_comments(txt).splitlines():
        s = line.strip()
        if not s:
            continue
        toks = tok_re.findall(s)
        if not toks:
            continue
        first = toks[0]
        if first not in ("{", "}") and not first.startswith('"'):
            if depth_filter is None or depth in depth_filter:
                c[(depth, first.lower())] += 1
        for t in toks:
            if t == "{":
                depth += 1
            elif t == "}":
                depth -= 1
    return c

def dude_keywords(path, pattern=r'!token\.Icmp\(\s*"([A-Za-z_0-9]+)"'):
    try:
        src = open(path, encoding="latin-1").read()
    except FileNotFoundError:
        return set()
    return {m.lower() for m in re.findall(pattern, src)}

# ---------------- materials ----------------
mtr = collections.Counter()
mtr_files = 0
mtr_decl_types = collections.Counter()
for p, name, txt in iter_files({".mtr"}):
    mtr_files += 1
    mtr.update(leading_keywords(txt))
    for m in re.finditer(r"^\s*([A-Za-z_]+)\s+[A-Za-z0-9_/\.]+\s*(?:\n\s*)?\{", strip_comments(txt), flags=re.M):
        mtr_decl_types[m.group(1).lower()] += 1

dude_mtr = set()
for f in ("renderer/Material.cpp",):
    dude_mtr |= dude_keywords(os.path.join(DUDE, f))
    dude_mtr |= dude_keywords(os.path.join(DUDE, f), r'token\.Icmp\(\s*"([A-Za-z_0-9]+)"')
    dude_mtr |= dude_keywords(os.path.join(DUDE, f), r'CheckTokenString\(\s*"([A-Za-z_0-9]+)"')

print(f"== .mtr files: {mtr_files}; decl types seen at depth 0 (count of decls):")
for k, n in mtr_decl_types.most_common(40):
    print(f"   {k:20s} {n}")
print("== .mtr leading keywords at material depth (1) and stage depth (2); NOT in DUDE Material.cpp marked with *")
for (d, k), n in sorted(mtr.items(), key=lambda kv: (-kv[1])):
    if d in (1, 2) and n >= 2:
        flag = " " if k in dude_mtr else "*"
        print(f"   {flag} d{d} {k:28s} {n}")

# ---------------- gui ----------------
gui = collections.Counter()
gui_files = 0
gui_wintypes = collections.Counter()
for p, name, txt in iter_files({".gui", ".guide"}):
    gui_files += 1
    gui.update(leading_keywords(txt))
    for m in re.finditer(r"^\s*([A-Za-z_]+Def)\s+[A-Za-z0-9_]+", strip_comments(txt), flags=re.M):
        gui_wintypes[m.group(1)] += 1
dude_gui = set()
for f in ("ui/Window.cpp", "ui/SimpleWindow.cpp", "ui/UserInterface.cpp", "ui/GuiScript.cpp", "ui/ListWindow.cpp", "ui/Winvar.cpp"):
    for pat in (r'!token\.Icmp\(\s*"([A-Za-z_0-9:]+)"', r'token\.Icmp\(\s*"([A-Za-z_0-9:]+)"', r'\{\s*"([A-Za-z_0-9]+)"\s*,\s*Script_', r'if\s*\(\s*idStr::Icmp\(\s*_name\s*,\s*"([A-Za-z_0-9]+)"', r'Icmp\(\s*"([A-Za-z_0-9]+)"'):
        dude_gui |= dude_keywords(os.path.join(DUDE, f), pat)
print(f"\n== .gui/.guide files: {gui_files}; window types:")
for k, n in gui_wintypes.most_common():
    print(f"   {k:20s} {n}")
print("== .gui leading keywords (depth>=1), NOT found as a literal in DUDE ui/*.cpp marked *")
for (d, k), n in sorted(gui.items(), key=lambda kv: (-kv[1])):
    if d >= 1 and n >= 3:
        base = k.split("::")[0]
        flag = " " if (k in dude_gui or base in dude_gui) else "*"
        print(f"   {flag} d{d} {k:28s} {n}")

# ---------------- BSE effects ----------------
fx = collections.Counter()
fx_files = 0
seg_types = collections.Counter()
seg_props = collections.Counter()
domain_types = collections.Counter()
envelopes = collections.Counter()
for p, name, txt in iter_files({".fx"}):
    if not name.startswith("effects/"):
        continue
    fx_files += 1
    kw = leading_keywords(txt)
    fx.update(kw)
    for (d, k), n in kw.items():
        if d == 2:
            seg_types[k] += n
        elif d == 3:
            seg_props[k] += n
    for m in re.finditer(r"\{\s*(point|line|box|sphere|cylinder|spiral|model|cone|ring|triangle|segment)\b", txt):
        domain_types[m.group(1)] += 1
    for m in re.finditer(r'envelope\s+"([A-Za-z_0-9]+)"', txt):
        envelopes[m.group(1)] += 1
print(f"\n== BSE .fx files: {fx_files}")
print("   depth-1 (effect body) keywords:", dict((k, n) for (d, k), n in fx.items() if d == 1))
print("   depth-2 (emitter body = segment types / emitter props):")
for k, n in seg_types.most_common():
    print(f"      {k:16s} {n}")
print("   depth-3 (segment body) keywords:")
for k, n in seg_props.most_common():
    print(f"      {k:16s} {n}")
print("   depth-4 (start/motion/end params):", [(k, n) for (d, k), n in fx.most_common() if d == 4][:40])
print("   spawn domain types:", domain_types.most_common())
print("   envelope names:", envelopes.most_common())

# ---------------- sound shaders ----------------
snd = collections.Counter()
snd_files = 0
for p, name, txt in iter_files({".sndshd"}):
    snd_files += 1
    snd.update(leading_keywords(txt, {1}))
dude_snd = dude_keywords(os.path.join(DUDE, "sound/snd_shader.cpp"))
print(f"\n== .sndshd files: {snd_files}; keywords at depth 1, NOT in DUDE snd_shader.cpp marked *")
for (d, k), n in snd.most_common(40):
    flag = " " if k in dude_snd else "*"
    print(f"   {flag} {k:28s} {n}")

# ---------------- decl types in .def / other decl files ----------------
decl_types = collections.Counter()
spawnclasses = collections.Counter()
for p, name, txt in iter_files({".def", ".playback", ".lipsync", ".skin", ".af", ".mtt", ".efx", ".viseme", ".reverb"}):
    st = strip_comments(txt)
    for m in re.finditer(r"^\s*([A-Za-z_][A-Za-z_0-9]*)\s+[^\s{]+\s*(?:\n\s*)?\{", st, flags=re.M):
        decl_types[(os.path.splitext(name)[1], m.group(1))] += 1
    for m in re.finditer(r'"spawnclass"\s+"([A-Za-z_0-9]+)"', st):
        spawnclasses[m.group(1)] += 1
print("\n== decl types by file extension:")
for (ext, k), n in decl_types.most_common(60):
    print(f"   {ext:10s} {k:20s} {n}")
print("\n== spawnclass frequency (game-side classes referenced by .def):")
for k, n in spawnclasses.most_common(80):
    print(f"   {k:28s} {n}")
