#!/usr/bin/env python3
"""DUDE PBR material classifier for Quake 4 (docs/pbr-materials.md sec. 6.1).

Reads every material of a retail Quake 4 install straight from its pk4s (the
engine's rules: later pk4 wins per file, guides expanded, first definition of a
name wins), gives each LIT one a class of the shared PBR vocabulary
(tools/pbr_classes.py - Quake 4's own materialType names plus DUDE's skin, eyes,
ceramic, rust, wood, cloth, cardboard) and writes the table the renderer loads:

    q4base/pbr/pbr_materials.cfg      "<name> * * <class> * *   # (provenance)"

Every row is category-tagged with '*' numbers, so it tracks its class's live
"@cat" preset; per-material tuning goes through the in-game editor
(pbr_overrides.cfg) and pbr_bake_inplace.py, as for Doom 3.

Signals, strongest first (each row's comment says which one decided):
  1. materialType <t> declared in the material body (Raven's own label)
  2. the materialImage hit map: every pixel is one type's rgb from
     materials/types/types.mtt; the dominant type by pixel share wins
  3. refinements on top of any signal: flesh on a head -> skin, eye/teeth/
     tongue -> eyes, metal + rust/corrosion -> rust, concrete + tile -> ceramic
  4. texture sharing with a 1/2-classified material (diffuse first, then the
     normal map), plus its _ut/_glow/... variants, c_ copies and l/r twins
  5. name tokens and directory priors written for Quake 4's naming
  6. head meshes (models/lipsync/**.md5mesh + the skins that swap their
     materials): skin, unless the material is gear
Lit materials no signal reaches are appended commented out, ready to edit.

Usage:
    python3 tools/pbr_classify_q4.py                    # Steam path, writes q4base/pbr/
    python3 tools/pbr_classify_q4.py --q4dir "/path/to/Quake 4" --samples 15
"""

import argparse
import functools
import io
import os
import re
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
DEFAULT_Q4DIR = "/mnt/linux-data/SteamLibrary/steamapps/common/Quake 4"

# the guide expander validated byte for byte against the engine's DeclGuides.cpp
sys.path.insert(0, os.path.join(HERE, "quake4"))
import check_guides  # noqa: E402


class Pk4Fs:
    """Read-only view of q4base/*.pk4 with the engine's override rule: pk4s are
    searched in reverse sorted order, so for one internal path the
    alphabetically LATER pk4 wins. Paths are keyed lower-case."""

    def __init__(self, q4base):
        self.files = {}         # lower path -> (pk4 path, entry name)
        self.zips = {}
        pk4s = sorted((f for f in os.listdir(q4base) if f.lower().endswith(".pk4")),
                      key=str.lower)
        for pk4 in pk4s:
            path = os.path.join(q4base, pk4)
            with zipfile.ZipFile(path) as z:
                for entry in z.namelist():
                    if not entry.endswith("/"):
                        self.files[entry.lower().replace("\\", "/")] = (path, entry)

    def exists(self, path):
        return path.lower() in self.files

    def origin(self, path):
        return os.path.basename(self.files[path.lower()][0])

    def read(self, path):
        pk4, entry = self.files[path.lower()]
        z = self.zips.get(pk4)
        if z is None:
            z = self.zips[pk4] = zipfile.ZipFile(pk4)
        return z.read(entry)

    def text(self, path):
        return self.read(path).decode("latin-1")

    def list(self, prefix, ext):
        return [p for p in self.files if p.startswith(prefix) and p.endswith(ext)]


# ---------------------------------------------------------------------------
# decl text -> materials
# ---------------------------------------------------------------------------

# idLexer-ish tokens: quoted strings, the punctuation the material grammar uses,
# and everything else up to whitespace/punctuation (path names keep / . - :)
TOKEN = re.compile(r'"[^"\n]*"?|[{}(),]|[^\s{}(),"]+')

# every decl type name the engine registers for Quake 4; a .mtr file may prefix
# a decl with one ("table foo { ... }") - those are not materials
DECL_TYPE_NAMES = {
    "table", "material", "skin", "sound", "entitydef", "mapdef", "fx", "particle",
    "articulatedfigure", "pda", "email", "video", "audio", "export", "materialtype",
    "lipsync", "playback", "effect", "playermodel",
}


def canonical_decl_name(name):
    """idDeclManagerLocal::MakeNameCanonical: lower-case, '\\' -> '/', and cut at
    the LAST '.' - the key the engine (and the PBR table lookup) uses."""
    name = name.strip('"').replace("\\", "/").lower()
    dot = name.rfind(".")
    return name[:dot] if dot >= 0 else name


def icmp_path(a, b):
    """idStr::IcmpPath, which orders the engine's sorted decl-file listing:
    case-insensitive, and at the first difference a path that still has a
    folder separator ahead sorts BEFORE one that does not (folders first)."""
    a, b = a.lower(), b.lower()
    for i in range(min(len(a), len(b))):
        if a[i] != b[i]:
            fa, fb = "/" in a[i:], "/" in b[i:]
            if fa and not fb:
                return -1
            if fb and not fa:
                return 1
            return -1 if a[i] < b[i] else 1
    return (len(a) > len(b)) - (len(a) < len(b))


def load_guides(fs):
    """guides/*.guide templates, mirroring check_guides.load_templates (and the
    engine's DeclGuides::Load): files in sorted order, first definition wins."""
    tpls = {}
    for path in sorted(fs.list("guides/", ".guide"), key=functools.cmp_to_key(icmp_path)):
        t = check_guides.strip_comments(fs.text(path))
        for m in re.finditer(r"(?:(?<=^)|(?<=[\s{};]))(?:inlineGuide|guide)\s+(\w+)\s*\(([^)]*)\)\s*\{",
                             t, re.I):
            start = m.end()
            depth, k = 1, start
            while k < len(t) and depth:
                if t[k] == '"':
                    k += 1
                    while k < len(t) and t[k] not in '"\n':
                        k += 1
                elif t[k] == "{":
                    depth += 1
                elif t[k] == "}":
                    depth -= 1
                k += 1
            name = m.group(1).lower()
            parms = [p.strip() for p in m.group(2).split(",")]
            if parms == [""]:
                parms = []
            tpls.setdefault(name, (parms, t[start:k - 1]))
    return tpls


def iter_decls(tokens):
    """Yield (type, name, body tokens) like idDeclFile::LoadAndParse: an optional
    decl type name, the decl name, then a balanced { } block."""
    i, n = 0, len(tokens)
    while i < n:
        tok = tokens[i]
        dtype = "material"
        if tok.lower() in DECL_TYPE_NAMES:
            dtype = tok.lower()
            i += 1
            if i >= n:
                break
            tok = tokens[i]
        if tok == "{":
            # missing decl name: skip the braced section
            depth = 0
            while i < n:
                depth += tokens[i] == "{"
                depth -= tokens[i] == "}"
                i += 1
                if depth == 0:
                    break
            continue
        name = tok
        i += 1
        if i >= n or tokens[i] != "{":
            continue                    # "Expecting '{'" - the engine skips the name
        start = i + 1
        depth = 0
        while i < n:
            depth += tokens[i] == "{"
            depth -= tokens[i] == "}"
            i += 1
            if depth == 0:
                break
        yield dtype, name, tokens[start:i - 1]


def norm_image(path):
    """image path as a comparison key: lower-case, '/' separators, no extension"""
    path = path.strip('"').replace("\\", "/").lower()
    base, ext = os.path.splitext(path)
    return base if ext in (".tga", ".dds", ".jpg", ".png", ".bmp", ".pcx") else path


def parse_image_program(tok, i):
    """Image program starting at tok[i]: 'img' or 'func( args... )'. Returns
    (list of image paths inside, index after the program). Function names and
    numeric arguments are dropped, so addnormals( a, heightmap( b, 4 ) ) -> [a, b]."""
    if i >= len(tok):
        return [], i
    if i + 1 < len(tok) and tok[i + 1] == "(":
        paths, depth, j = [], 0, i + 1
        while j < len(tok):
            t = tok[j]
            if t == "(":
                depth += 1
            elif t == ")":
                depth -= 1
                if depth == 0:
                    j += 1
                    break
            elif t not in (",", "{", "}") and not (j + 1 < len(tok) and tok[j + 1] == "("):
                if not re.match(r"^-?[\d.]+$", t):
                    paths.append(t.strip('"'))
            j += 1
        return paths, j
    t = tok[i]
    if t in ("{", "}", "(", ")", ","):
        return [], i
    return [t.strip('"')], i + 1


LIT_STAGES = ("diffusemap", "bumpmap", "specularmap")


class Material:
    """What the classifier needs from one material decl."""

    def __init__(self, name, body, src):
        self.name = canonical_decl_name(name)
        self.src = src                      # .mtr path it came from
        self.material_type = None           # declared materialType
        self.material_image = None          # materialImage hit map path
        self.editor_image = None
        self.stages = []                    # (kind, [image paths]) for lit stages
        self.flags = set()                  # lower-case material-level keywords
        self.blends = []                    # blend modes of the non-lit stages
        self.parse(body)

    def parse(self, tok):
        depth, i, n = 0, 0, len(tok)
        stage = None
        while i < n:
            t = tok[i]
            low = t.lower()
            if t == "{":
                depth += 1
                if depth == 1:
                    stage = {"blend": None, "map": []}
                i += 1
                continue
            if t == "}":
                if depth == 1 and stage is not None:
                    if stage["blend"] in LIT_STAGES:
                        self.stages.append((stage["blend"], stage["map"]))
                    else:
                        self.blends.append(stage["blend"])
                    stage = None
                depth -= 1
                i += 1
                continue
            if depth == 0:
                if low in LIT_STAGES:
                    paths, i = parse_image_program(tok, i + 1)
                    self.stages.append((low, paths))
                    continue
                if low == "materialtype" and i + 1 < n:
                    self.material_type = tok[i + 1].strip('"').lower()
                    i += 2
                    continue
                if low == "materialimage":
                    paths, i = parse_image_program(tok, i + 1)
                    self.material_image = paths[0] if paths else None
                    continue
                if low == "qer_editorimage" and i + 1 < n:
                    self.editor_image = tok[i + 1].strip('"')
                    i += 2
                    continue
                self.flags.add(low)
            elif depth == 1 and stage is not None:
                if low in LIT_STAGES:
                    # Quake 4 stage shorthand: { diffusemap img alphatest .5 }
                    stage["blend"] = low
                    stage["map"], i = parse_image_program(tok, i + 1)
                    continue
                if low == "blend" and i + 1 < n:
                    stage["blend"] = tok[i + 1].lower().rstrip(",")
                    i += 2
                    continue
                if low in ("map", "cubemap", "cameracubemap", "videomap", "soundmap"):
                    if low == "map":
                        stage["map"], i = parse_image_program(tok, i + 1)
                    else:
                        i += 2
                    continue
            i += 1

    @property
    def lit(self):
        return bool(self.stages)

    def images(self, kinds=("diffusemap", "bumpmap")):
        return [p for k, paths in self.stages if k in kinds for p in paths]


def load_materials(fs, tpls):
    """Every material decl, keyed by canonical name, with the engine's rule for
    duplicates: decl files load in IcmpPath order (materials/ scanned
    recursively, as Quake 4 does); the FIRST file to define a name owns it and a
    later file's redefinition is skipped with a warning; a redefinition inside
    the same file replaces the earlier one. Returns (materials, duplicates)."""
    mats, dups = {}, []
    for path in sorted(fs.list("materials/", ".mtr"), key=functools.cmp_to_key(icmp_path)):
        text = check_guides.expand(fs.text(path), tpls)
        text = check_guides.strip_comments(text)
        tokens = TOKEN.findall(text)
        for dtype, name, body in iter_decls(tokens):
            if dtype != "material":
                continue
            m = Material(name, body, path)
            old = mats.get(m.name)
            if old is not None and old.src != path:
                dups.append((m.name, old.src, path))
                continue
            mats[m.name] = m
    return mats, dups


# ---------------------------------------------------------------------------
# Quake 4 material types and hit maps
# ---------------------------------------------------------------------------

def load_material_types(fs):
    """materials/types/*.mtt -> {name: (r, g, b)} (Quake 4's rvDeclMatType)."""
    types = {}
    for path in sorted(fs.list("materials/types/", ".mtt"), key=functools.cmp_to_key(icmp_path)):
        text = check_guides.strip_comments(fs.text(path))
        for m in re.finditer(r"materialType\s+(\w+)\s*\{([^}]*)\}", text, re.I):
            rgb = re.search(r"rgb\s+(\d+)\s*,\s*(\d+)\s*,\s*(\d+)", m.group(2))
            if rgb:
                types.setdefault(m.group(1).lower(), tuple(int(x) for x in rgb.groups()))
    return types


IMAGE_EXTS = (".tga", ".dds", ".png", ".jpg")

# hit-map pixels farther than this (RGB distance) from every type colour are
# not filtering noise but paint that is no type at all - counted as unknown
HITMAP_MAX_DIST = 32.0

# off-palette colours the retail hit maps use often enough to read. (80,50,30)
# brown is a type that did not survive into types.mtt: it paints the body bag
# (98%), the gurney mattress and the medical tool-table cover - fabric/vinyl,
# so it reads as cloth. Flat (128,128,128) grey is an unpainted placeholder
# (dropship interior: 100%) and stays unknown.
HITMAP_LEGACY = {(80, 50, 30): "cloth"}


def find_image(fs, path):
    """Resolve an image reference the way the engine's loader does: the name as
    written, then the same base name with each image extension (.tga first)."""
    path = path.strip('"').replace("\\", "/").lower()
    base, ext = os.path.splitext(path)
    if ext not in IMAGE_EXTS:
        base = path
    cands = ([path] if ext in IMAGE_EXTS else []) + [base + e for e in IMAGE_EXTS]
    return next((c for c in cands if fs.exists(c)), None)


class HitMaps:
    """Decodes materialImage hit maps into per-type pixel shares."""

    def __init__(self, fs, types):
        self.fs = fs
        # every reference colour: the types.mtt palette plus the legacy ones
        self.palette = sorted(list(types.items()) + [(n, c) for c, n in HITMAP_LEGACY.items()])
        self.cache = {}
        self.missing = set()

    def nearest(self, rgb):
        """the type whose colour is closest to this pixel (block compression and
        filtering smear the exact palette), or "?" when none is close"""
        best, best_d = "?", HITMAP_MAX_DIST * HITMAP_MAX_DIST + 1   # d is an int
        for name, ref in self.palette:
            d = sum((a - b) * (a - b) for a, b in zip(rgb, ref))
            if d < best_d:
                best, best_d = name, d
        return best

    def shares(self, path):
        """[(type, share)] sorted by share, over the pixels that are a real type
        ('none' = no-decal void and off-palette pixels excluded); [] when the
        image is missing or holds no type at all."""
        key = path.lower()
        if key in self.cache:
            return self.cache[key]
        found = find_image(self.fs, path)
        result = []
        if found is None:
            self.missing.add(key)
        else:
            from PIL import Image
            im = Image.open(io.BytesIO(self.fs.read(found))).convert("RGB")
            hist = {}
            for count, rgb in im.getcolors(im.size[0] * im.size[1]):
                t = self.nearest(rgb)
                if t not in ("none", "?"):
                    hist[t] = hist.get(t, 0) + count
            total = sum(hist.values())
            if total:
                result = sorted(((t, c / total) for t, c in hist.items()),
                                key=lambda tc: (-tc[1], tc[0]))
        self.cache[key] = result
        return result


# ---------------------------------------------------------------------------
# head meshes (ground truth for skin)
# ---------------------------------------------------------------------------

MD5_SHADER = re.compile(r'shader\s+"([^"]+)"')


def is_head_mesh(path):
    """Quake 4 keeps every speaking head as its own mesh under models/lipsync/
    (marine_head/anderson_mesh.md5mesh, head_kane_mesh.md5mesh ...) and bolts it
    on through the entity's def_head; the bodies carry no face."""
    return path.startswith("models/lipsync/") or "head" in os.path.basename(path)


def load_head_materials(fs):
    """{material: what puts it on a head} - every shader an md5 head mesh
    references, plus every material a skin decl swaps one of those for (the
    char_marinehead_* variants re-skin gen_white / gen_asian_2 / helmets)."""
    heads = {}
    for path in sorted(fs.list("models/", ".md5mesh")):
        if not is_head_mesh(path):
            continue
        for shader in MD5_SHADER.findall(fs.text(path)):
            heads.setdefault(canonical_decl_name(shader), os.path.basename(path))
    for path in sorted(fs.list("skins/", ".skin"), key=functools.cmp_to_key(icmp_path)):
        tokens = TOKEN.findall(check_guides.strip_comments(fs.text(path)))
        for _dtype, name, body in iter_decls(tokens):
            body = [t.strip('"') for t in body if t not in (",",)]
            i = 0
            while i + 1 < len(body):
                src, dst = body[i].lower(), body[i + 1]
                i += 2
                if src == "model":
                    continue
                if canonical_decl_name(src) in heads:
                    heads.setdefault(canonical_decl_name(dst), "skin " + canonical_decl_name(name))
    return heads


# ---------------------------------------------------------------------------
# classification rules
# ---------------------------------------------------------------------------

# materials that are not lit surfaces of the world (or are editor/dev only):
# never emitted, whatever stages they carry
SKIP_PREFIXES = (
    "gfx/guis/", "guis/", "textures/guis/", "textures/editor/", "textures/skies/",
    "shaderdemos/", "lights/", "fogs/", "particles/", "textures/sfx/",
)
SKIP_PATTERNS = (
    re.compile(r"^textures/common/(\d+_\d+(_ut)?|\d+_grid)$"),  # editor grid swatches
    re.compile(r"skybox|portal_sky"),                            # skybox dressing
)

WORD_SPLIT = re.compile(r"[/_.\-\d\s]+")


def words(text):
    return [w for w in WORD_SPLIT.split(text.lower()) if w]


# eye/teeth/tongue: the cornea/enamel class. "eye" as a word prefix or suffix
# catches eye_anderson, hurtmarineeye, strassaeye, sledgeeye, ft_eye; eyelash
# (a hair card) is not an eye.
def is_eye_word(w):
    if w.startswith("eyelash"):
        return False
    return (w.startswith("eye") or w.endswith("eye")
            or any(t in w for t in ("teeth", "tooth", "tongue", "cornea"))
            or w == "iris")


# rusted/corroded metal - substring, so rusty / 02crust ("02c" + "rust") /
# 06brust hit; thrust (thruster) does not
def is_rust_word(w):
    return ("rust" in w and "thrust" not in w) or "corro" in w or "oxid" in w


CERAMIC_WORDS = ("tile", "tiles", "tiled", "ceramic", "porcelain")

# head gear: a head-mesh material named like this is not the face (Raven even
# declares the medic/tech helmets flesh - they keep that, not skin)
GEAR_WORDS = {"helmet", "helmets", "visor", "mask", "goggles", "goggle", "hat", "cap", "armor",
              "armour", "headset", "gasmask"}

# flesh named head*/face* is a face here (head02, half_head, tm_head_base);
# not on the monsters, whose heads are flesh-and-metal Strogg sheets
SKIN_TOKEN_SCOPE = ("models/characters/", "models/tube_marine/")

# keyword tokens matched against the words of the material's own base name and
# of its diffuse/bump image base names (split on / _ . - and digits). Each class
# has exact-match words (short/ambiguous: "rock" must not hit "rocket") and
# substrings (long, distinctive words glued into compounds: "vertpipe",
# "bgbottle", "armscreen"). Highest hit count wins; list order breaks ties.
# Tokens decide the material; the metal SUBCLASS (solid vs hollow) mostly comes
# from the directory priors below, because Raven's own materialType labels for
# panels, walls and plates are split between the two (see DIR_PRIORS).
TOKEN_SETS = [
    # ahead of flesh so the hanging man's hose (cryohangingmanhose) is the hose
    ("rubber",          {"rubber", "hose", "hoses", "tire", "tires", "tyre", "tyres", "gasket", "mask"},
                        {"rubber", "hose"}),
    ("flesh",           {"meat", "gut", "guts", "gore", "gib", "gibs", "brain", "brains", "heart",
                         "organ", "organs", "vein", "veins", "artery", "arteries", "intestine",
                         "intestines", "corpse", "corpses", "pork", "blood", "bloody", "bodies",
                         "muscle", "stump", "entrails", "crucified"},
                        {"flesh", "intestin", "blood", "gore", "bodies", "hangingman", "artery"}),
    ("glass",           {"glass", "lens", "lenses", "visor", "bottle", "bottles", "beaker",
                         "beakers", "vial", "vials", "jar", "jars"},
                        {"glass", "crystal", "bottle", "beaker"}),
    ("water",           {"water", "puddle"},
                        {"water"}),
    ("gelatinousfluid", {"slime", "acid", "goo", "ooze", "fluid", "fluids", "liquid", "sludge",
                         "bile", "bubble"},
                        {"fluid", "slime", "liquid"}),
    ("oil",             {"oil"},
                        set()),
    ("concrete",        {"concrete", "brick", "bricks", "cement", "plaster", "asphalt", "stucco",
                         "mortar", "masonry"},
                        {"concrete", "brick", "cement", "plaster", "asphalt"}),
    ("ceramic",         {"ceramic", "porcelain", "mug", "sink", "toilet", "urinal"},
                        {"ceramic", "porcelain", "coffeecup"}),
    ("rock",            {"rock", "rocks", "cliff", "cliffs", "cave", "boulder", "boulders", "stone",
                         "stones", "lava", "ice", "strata", "terrain", "mountain", "mountains"},
                        {"sharprock", "smoothrock", "vistarock", "boulder", "cliff", "mountain"}),
    ("sand",            {"sand", "dune", "dunes"},
                        {"skysand"}),
    ("dirt",            {"dirt", "mud", "soil"},
                        set()),
    ("gravel",          {"gravel", "pebble", "pebbles", "rubble"},
                        set()),
    ("wood",            {"wood", "wooden", "plank", "planks", "lumber"},
                        {"wood", "plank"}),
    ("cloth",           {"cloth", "fabric", "canvas", "curtain", "curtains", "carpet", "banner",
                         "banners", "cot", "mattress", "tarp", "rag", "rags", "bag", "backpack",
                         "blanket", "pillow", "towel", "sandbag"},
                        {"cloth", "fabric", "banner", "curtain", "carpet", "mattress", "blanket",
                         "backpack"}),
    ("cardboard",       {"cardboard", "carton", "paper", "binder", "book", "books", "newspaper"},
                        {"cardboard", "carton"}),
    ("plastic",         {"plastic", "vinyl", "foam", "health", "medkit"},
                        {"plastic", "foamcup", "medkit"}),
    ("armor",           {"armor", "armour", "kevlar", "vest", "helmet", "helmets", "shard"},
                        {"armor", "armour", "helmet"}),
    # consoles, monitors, computers, (lit) screens, keypads, wiring - Raven types
    # its wire runs electronics (wire1_1_*: 8 electronics vs 2 rubber)
    ("electronics",     {"console", "consoles", "monitor", "monitors", "computer", "computers",
                         "screen", "screens", "keypad", "keyboard", "cpu", "circuit", "circuits",
                         "server", "servers", "data", "databank", "display", "gui", "wire", "wires",
                         "cable", "cables", "electronics", "electronic", "radio", "battery"},
                        {"console", "monitor", "screen", "computer", "keypad", "circuit",
                         "wirebundle", "wirejumble", "hangingwires", "wirechunk", "databank",
                         "dataport"}),
    # Strogg monsters' mechanical parts (only applied under models/monsters/ and
    # the Strogg-ified characters, see MONSTERMETAL_SCOPE). No bare "gun"
    # substring: the Gunner's body and head are its flesh sheets.
    ("monstermetal",    {"gun", "guns", "cannon", "blade", "blades", "spike", "spikes", "railgun",
                         "gundoors", "claw", "claws", "drill", "saw", "turret", "grenade", "rocket",
                         "launcher", "hook"},
                        {"gunarm", "gundoor", "cannon", "blade", "spike", "turret", "grenade"}),
    # bare / structural metal: words that mean exposed metal at the surface
    ("solidmetal",      {"metal", "steel", "iron", "chrome", "pipe", "pipes", "girder", "girders",
                         "grate", "grates", "grating", "beam", "beams", "rail", "railing",
                         "railings", "fence", "support", "supports", "strut", "struts", "bolt",
                         "bolts", "rivet", "rivets", "chain", "chains", "links", "gear", "gears",
                         "piston", "pistons", "pillar", "pillars", "column", "columns", "truss",
                         "bracket", "clamp", "antenna", "brass", "copper", "bronze", "aluminum",
                         "tin", "mesh", "cage", "ladder", "spring", "needle", "spatula", "grinder",
                         "engine", "pump", "gun", "blade"},
                        {"metal", "steel", "pipe", "girder", "grate", "grating", "railing", "chrome",
                         "piston", "pillar", "rivet", "beam", "brass", "blade"}),
    # thin sheet-metal objects (Raven: "thin metal plate - such as air ducts or
    # computer covering"; its crates are hollowmetal 9 of 9)
    # ("barrel" is deliberately absent: a gun barrel is solid; the Strogg
    # storage barrels ride their directory prior instead)
    ("hollowmetal",     {"crate", "crates", "locker", "lockers", "cabinet", "cabinets", "container",
                         "containers", "canister", "canisters", "cannister", "tank", "tanks", "duct",
                         "ducts", "hatch", "tray", "shelf", "shelves", "casing", "hull", "box",
                         "boxes", "ammo", "ammobox", "toolchest", "airtank"},
                        {"crate", "locker", "cabinet", "container", "canister", "cannister",
                         "duct", "toolchest", "ammobox", "airtank"}),
]

# monstermetal tokens only mean "monster" on the monsters and the Strogg-ified
# characters; elsewhere a gun or a blade is a world prop (solidmetal)
MONSTERMETAL_SCOPE = ("models/monsters/", "models/characters/kane_strogg/",
                      "models/characters/choppable_marine/", "models/characters/marine_kanestrogg/")

# signage / paint decals: only the classes a decal can actually BE (blood is
# flesh, a glass shard decal is glass, hanging wires are electronics). A
# "gear" warning sign is paint, not a gear.
DECAL_CLASSES = {"flesh", "glass", "water", "gelatinousfluid", "oil", "concrete", "rubber",
                 "electronics"}

# path prefix -> class prior, used when no declared type, hit map, texture
# share, variant or token decides. Longest prefix wins. The world-texture
# priors follow the majority of Raven's own materialType / hit-map labels in
# the same directory (counts from the retail data in the comments): Quake 4
# labels walls, trims, doors, lights and machines solidmetal far more often
# than hollowmetal - unlike Doom 3, where the painted station panelling was
# routed to the painted class.
DIR_PRIORS = {
    # textures/
    "textures/terminal/":         "solidmetal",   # Raven: solid 19, hollow 15
    "textures/command_ship/":     "solidmetal",   # solid 21, hollow 3
    "textures/prison/":           "solidmetal",   # solid 26, hollow 13
    "textures/common_walls/":     "solidmetal",   # solid 37, hollow 4
    "textures/common_trims/":     "solidmetal",   # solid 6, hollow 4
    "textures/common_lights/":    "solidmetal",   # solid 21 (light fixtures)
    "textures/common_misc/":      "solidmetal",   # railings, pipes, beams, supports
    "textures/common_floors/":    "hollowmetal",  # hollow 6, solid 4 (deck plating)
    "textures/medlabs/":          "hollowmetal",  # hollow 2 (its only declared ones)
    "textures/canyon/":           "solidmetal",   # solid 13, hollow 2
    "textures/tram/":             "solidmetal",   # solid 7, hollow 4
    "textures/stroyent/":         "solidmetal",   # tunnels/steel; gore + concrete by token
    "textures/core/":             "solidmetal",   # no labels; Strogg base set like hub
    "textures/hub/":              "solidmetal",   # no labels; Strogg base set like core
    "textures/mptextures/":       "solidmetal",   # no labels; MP station set
    "textures/q4x_":              "solidmetal",   # expansion-pack sets (q4x_cryo, _mp, ...)
    "textures/q4cmp":             "solidmetal",
    "textures/consoles/":         "electronics",  # electronics 39, glass 5
    "textures/glass/":            "glass",
    "textures/rock/":             "rock",         # rock 33 (incl. its sand blends)
    "textures/banners/":          "cloth",
    "textures/fluids/":           "gelatinousfluid",
    # models/
    "models/mapobjects/":                      "solidmetal",
    "models/mapobjects/strogg/":               "solidmetal",   # solid 127, hollow 16, glass 3
    # consoles/monitors -> electronics for the unlabelled ones (Raven's hit maps
    # paint the labelled console casings solidmetal, 16 of 16 - those keep it,
    # and so do their numbered siblings through the variant signal)
    "models/mapobjects/strogg/console/":       "electronics",
    "models/mapobjects/strogg/monitors/":      "electronics",
    "models/mapobjects/strogg/barrels/":       "hollowmetal",
    "models/mapobjects/strogg/crates/":        "hollowmetal",
    "models/mapobjects/strogg/cargo_container/": "hollowmetal",
    "models/mapobjects/strogg/prison/console/": "electronics",
    "models/mapobjects/marine/":               "solidmetal",   # solid 25, plastic 20, hollow 14
    "models/mapobjects/test/":                 None,
    "models/mapobjects/env/":                  None,
    "models/characters/":                      "flesh",        # flesh 96, monster 5, armor 4
    "models/monsters/":                        "flesh",        # the cyborg troops' body sheets
    # the purely mechanical Strogg units
    "models/monsters/air_defense_cannon/":     "monstermetal",
    "models/monsters/convoy_ground_creature/": "monstermetal",
    "models/monsters/harvester/":              "monstermetal",
    "models/monsters/heavyhovertank/":         "monstermetal",
    "models/monsters/light_tank/":             "monstermetal",
    "models/monsters/network_guardian/":       "monstermetal",
    "models/monsters/repair_bot/":             "monstermetal",
    "models/monsters/sentry/":                 "monstermetal",
    "models/monsters/stream_protector/":       "monstermetal",
    "models/monsters/tactical/":               "monstermetal",  # monster 3
    "models/monsters/teleport_dropper/":       "monstermetal",
    "models/monsters/turret/":                 "monstermetal",
    "models/monsters/turret_flying/":          "monstermetal",
    "models/monsters/maya_texture/":           None,           # Maya test surfaces
    "models/monsters/test/":                   None,
    "models/vehicles/":                        "solidmetal",
    "models/splashdamage/":                    "solidmetal",
    "models/gibs/":                            "flesh",
    "models/tube_marine/":                     "flesh",
    "models/items/ammo/":                      "hollowmetal",
    "models/items/armor/":                     "armor",
    "models/items/armorshard":                 "armor",
    "models/items/banner_flags/":              "cloth",
    "models/items/health/":                    "plastic",
    "models/pick_ups/sp_pickups/":             "hollowmetal",  # ammo crates; health/armor by token
}

# weapons get no rule rows at all (Doom 3 policy, pbr_classify.py: the
# viewmodel is the most-stared-at surface and is tuned by hand); a declared
# type, hit map or shared texture still classifies one
NO_RULE_PREFIXES = ("models/weapons/",)

# name suffixes that mark a variant of the same surface (unsmoothed tangents,
# collision/shadow flags, burn/slime/grime overlays, colour and team variants)
VARIANT_SUFFIXES = {
    "ut", "nonsolid", "noshadows", "noshadow", "nowshadows", "ns", "2sided", "2side", "twosided",
    "onesided", "2s", "glow", "alpha", "med", "mar", "burn", "burnaway", "smolder", "slime",
    "dirty", "dusty", "grime", "label", "stripe", "fade", "noscale", "clean", "nomip", "mp",
    "red", "blue", "green", "orange", "yellow", "black", "white", "grn", "marine", "strogg",
    "stroy", "char", "g", "ba", "new", "super", "global", "startoff", "starton", "storage",
    "rusty", "rust", "impact", "begin", "end", "chipped", "low", "lo", "shoot", "through",
    "test", "var", "redpulse", "pulse", "nocolor", "extra",
}


def variant_stem(name, loose=False):
    """dir/base with variant suffix tokens stripped: c_trim2_slime -> c_trim2.
    loose also strips numbers and single letters (t7_wall_10 -> t7_wall,
    medium1_b -> medium1): the numbered siblings of one family, which only
    count when two or more of them agree (see Classifier.variant)."""
    d, _, base = name.rpartition("/")
    parts = base.split("_")
    while len(parts) > 1 and (parts[-1] in VARIANT_SUFFIXES or
                              (loose and (parts[-1].isdigit() or len(parts[-1]) == 1))):
        parts.pop()
    return d + "/" + "_".join(parts)


def mirror_names(name):
    """the left/right twin of a part: gunner_armr_base <-> gunner_arml_base"""
    d, _, base = name.rpartition("/")
    out = []
    for a, b in (("arml", "armr"), ("_l_", "_r_"), ("left", "right")):
        if a in base:
            out.append(d + "/" + base.replace(a, b))
        if b in base:
            out.append(d + "/" + base.replace(b, a))
    return out


def dir_prior(name):
    best, best_len = None, -1
    for prefix, cls in DIR_PRIORS.items():
        if name.startswith(prefix) and len(prefix) > best_len:
            best, best_len = (prefix, cls), len(prefix)
    return best


# Doom 3 surface-type keywords some Quake 4 glass still declares ("glass" in
# the material body, 21 lit materials)
SURFTYPES = {"glass": "glass"}

# image-role words that carry no meaning for the token match (_d, _local, _h)
ROLE_WORDS = {"d", "local", "h", "s", "n", "nm", "bump", "diffuse", "spec", "hit"}

SIGNALS = ("materialType", "hitmap", "texture-share", "variant", "rule", "head-mesh")


def fmt_shares(shares, n=3):
    return ", ".join("%s %d%%" % (t, round(s * 100)) for t, s in shares[:n])


class Classifier:
    """Runs the signals over every in-scope lit material.

    results[name] = (class, signal, provenance); unclassified[name] = note."""

    def __init__(self, fs, mats, types, heads):
        self.fs = fs
        self.mats = mats
        self.heads = heads
        self.hitmaps = HitMaps(fs, types)
        self.results = {}
        self.unclassified = {}
        self.skipped = {}
        self.disagree = []          # (name, declared, hit-map shares)

    # -- scope -----------------------------------------------------------------
    def out_of_scope(self, m):
        name = m.name
        if not m.lit:
            return "unlit"
        if not name.startswith(("textures/", "models/", "gfx/decals/", "gfx/effects/")):
            return "out-of-scope"
        if name.startswith(SKIP_PREFIXES) or any(p.search(name) for p in SKIP_PATTERNS):
            return "skip-dir"
        return None

    # -- signal 1 + 2 ------------------------------------------------------------
    def declared(self, m):
        mt = m.material_type if m.material_type and m.material_type != "none" else None
        shares = self.hitmaps.shares(m.material_image) if m.material_image else []
        if mt and shares and shares[0][0] != mt:
            self.disagree.append((m.name, mt, shares))
        if mt:
            prov = "materialType " + mt
            if shares and shares[0][0] != mt:
                prov += "; hitmap says " + fmt_shares(shares, 2)
            return mt, "materialType", prov
        if shares:
            return shares[0][0], "hitmap", "hitmap " + fmt_shares(shares)
        return None

    # -- signal 4: shared diffuse/bump images ------------------------------------
    def build_image_index(self, seeds):
        """({diffuse image: {class: [materials]}}, {bump image: ...}) over the
        declared/hit-mapped materials that may lend their class (see
        seed_lends). An image whose users disagree (a burn overlay shared by
        concrete and metal, say) is dropped as ambiguous."""
        diffuse, bump = {}, {}
        for name, (cls, _sig, _prov) in seeds.items():
            m = self.mats[name]
            if not self.seed_lends(m):
                continue
            for img in set(norm_image(p) for p in m.images(("diffusemap",))):
                diffuse.setdefault(img, {}).setdefault(cls, []).append(name)
            if m.images(("diffusemap",)):
                # a glass/overlay with no diffuse of its own that borrows a
                # model's normal map (textures/glass/tramglass on the tram)
                # says nothing about that model
                for img in set(norm_image(p) for p in m.images(("bumpmap",))):
                    bump.setdefault(img, {}).setdefault(cls, []).append(name)
        keep = lambda index: {i: u for i, u in index.items() if len(u) == 1}
        return keep(diffuse), keep(bump)

    @staticmethod
    def seed_lends(m):
        """Raven's *_impact materials re-type a surface for gameplay (a metal
        ceiling declared rock so shots kick up debris), and textures/common/
        holds flat swatches every kind of material uses - neither says what
        the surface sharing their image is made of."""
        return "impact" not in m.name.rpartition("/")[2] and not m.name.startswith("textures/common/")

    def texture_share(self, m, index):
        for kinds, idx in ((("diffusemap",), index[0]), (("bumpmap",), index[1])):
            votes = {}
            for img in set(norm_image(p) for p in m.images(kinds)):
                for cls, users in idx.get(img, {}).items():
                    votes.setdefault(cls, set()).update(users)
            if votes:
                break
        if not votes:
            return None
        ranked = sorted(votes.items(), key=lambda kv: (-len(kv[1]), kv[0]))
        if len(ranked) > 1 and len(ranked[0][1]) == len(ranked[1][1]):
            return None                 # a tie: leave it to the later signals
        cls, users = ranked[0]
        prov = "texture-share " + sorted(users)[0]
        if kinds == ("bumpmap",):
            prov += " (normal map)"
        if len(ranked) > 1:
            prov += " (%d of %d sharers)" % (len(users), sum(len(u) for _, u in ranked))
        return cls, "texture-share", prov

    # -- signal 4b: variants of a declared/hit-mapped material -------------------
    def build_variant_index(self, seeds):
        by_stem, by_family, by_base = {}, {}, {}
        seeds = {n: r for n, r in seeds.items() if self.seed_lends(self.mats[n])}
        for name, (cls, _sig, _prov) in seeds.items():
            by_stem.setdefault(variant_stem(name), {}).setdefault(cls, []).append(name)
            by_family.setdefault(variant_stem(name, True), {}).setdefault(cls, []).append(name)
            if name.startswith("textures/"):
                base = variant_stem(name).rpartition("/")[2]
                by_base.setdefault(base, {}).setdefault(cls, []).append(name)
        return by_stem, by_family, by_base, seeds

    def variant(self, m, vindex):
        by_stem, by_family, by_base, seeds = vindex
        cands = {}
        for cls, names in by_stem.get(variant_stem(m.name), {}).items():
            cands.setdefault(cls, []).extend(names)
        if not cands:
            # numbered/lettered family: needs two or more agreeing members, so
            # one odd label (t7_walltrim_6 declared plastic) does not spread
            fam = by_family.get(variant_stem(m.name, True), {})
            ranked = sorted(fam.items(), key=lambda kv: (-len(kv[1]), kv[0]))
            if ranked and len(ranked[0][1]) >= 2 and \
                    (len(ranked) == 1 or len(ranked[0][1]) > len(ranked[1][1])):
                cands = {ranked[0][0]: ranked[0][1]}
        if not cands and m.name.startswith("textures/"):
            # textures/common_*/c_<x> is Raven's shared copy of textures/<set>/<x>
            d, _, base = variant_stem(m.name).rpartition("/")
            if d.startswith("textures/common_") and base.startswith("c_"):
                pool = by_base.get(base[2:], {})
                pool = {c: [n for n in ns if not n.startswith("textures/common_")] for c, ns in pool.items()}
            else:
                pool = by_base.get("c_" + base, {})
                pool = {c: [n for n in ns if n.startswith("textures/common_")] for c, ns in pool.items()}
            cands = {c: ns for c, ns in pool.items() if ns}
        if not cands:
            for twin in mirror_names(m.name):
                if twin in seeds:
                    cands.setdefault(seeds[twin][0], []).append(twin)
        if not cands:
            return None
        ranked = sorted(cands.items(), key=lambda kv: (-len(kv[1]), kv[0]))
        if len(ranked) > 1 and len(ranked[0][1]) == len(ranked[1][1]):
            return None
        cls, names = ranked[0]
        return cls, "variant", "variant of " + sorted(names)[0]

    # -- signal 5: name / directory rules ----------------------------------------
    def haystack(self, m):
        hay = set(words(m.name.rpartition("/")[2]))
        for p in m.images():
            hay.update(words(os.path.basename(norm_image(p))))
        return hay - ROLE_WORDS

    def rule(self, m):
        name = m.name
        if name.startswith(NO_RULE_PREFIXES):
            return None
        for st, cls in SURFTYPES.items():
            if st in m.flags:
                return cls, "rule", "surftype " + st
        hay = self.haystack(m)
        decal = "/decals/" in name or "decal_macro" in m.flags
        if name.startswith("models/") and not decal:
            eye = sorted(w for w in hay if is_eye_word(w))
            if eye:
                return "eyes", "rule", "token " + eye[0]
        best, best_hits, best_words = None, 0, []
        for cls, exact, subs in TOKEN_SETS:
            if cls == "monstermetal" and not name.startswith(MONSTERMETAL_SCOPE):
                continue
            if decal and cls not in DECAL_CLASSES:
                continue
            hit = sorted(w for w in hay if w in exact or any(s in w for s in subs))
            if len(hit) > best_hits:
                best, best_hits, best_words = cls, len(hit), hit
        if best:
            return best, "rule", "token " + "+".join(best_words)
        if decal:
            return None
        prior = dir_prior(name)
        if prior and prior[1]:
            return prior[1], "rule", "dir " + prior[0]
        return None

    # -- refinements -------------------------------------------------------------
    def head_of(self, m):
        head = self.heads.get(m.name)
        if head is None and "/marine_heads/" in m.name:
            head = "dir marine_heads/"
        return head

    def refine(self, m, cls, signal, prov):
        """DUDE's refinements on top of whichever signal decided: flesh on a
        head -> skin, eye/teeth/tongue -> eyes, metal + rust -> rust,
        concrete + tile -> ceramic."""
        name = m.name
        hay = self.haystack(m)
        if name.startswith("models/") and cls in ("flesh", "skin"):
            eye = sorted(w for w in hay if is_eye_word(w))
            if eye:
                return "eyes", signal, prov + " -> eyes: token " + eye[0]
        head = self.head_of(m)
        if cls == "flesh" and head and not hay & GEAR_WORDS:
            return "skin", signal, prov + " -> skin: head mesh " + head
        if cls == "flesh" and name.startswith(SKIN_TOKEN_SCOPE) and \
                any(w.startswith(("head", "face")) for w in hay):
            return "skin", signal, prov + " -> skin: head/face token"
        if cls in ("solidmetal", "hollowmetal"):
            rust = sorted(w for w in hay if is_rust_word(w))
            if rust:
                return "rust", signal, prov + " -> rust: token " + rust[0]
        if cls == "concrete":
            tile = sorted(w for w in hay if w in CERAMIC_WORDS or "tile" in w)
            if tile:
                return "ceramic", signal, prov + " -> ceramic: token " + tile[0]
        return cls, signal, prov

    def head_mesh(self, m, ruled):
        """Signal 6 for a head-mesh material nothing stronger decided: skin
        (eyes for its eye/teeth materials) unless a token says it is gear (a
        helmet stays armor) - a directory prior does not count against it."""
        head = self.head_of(m)
        if not head:
            return None
        if ruled and not ruled[2].startswith("dir ") and ruled[0] not in ("flesh", "skin", "eyes"):
            return None
        if self.haystack(m) & GEAR_WORDS:
            return None
        eye = any(is_eye_word(w) for w in self.haystack(m))
        return ("eyes" if eye else "skin"), "head-mesh", "head mesh " + head

    def test_copy(self, m):
        """textures/test/<x> is a scratch copy of a shipped textures/<set>/<x>:
        take that one's final class"""
        if not m.name.startswith("textures/test/"):
            return None
        base = variant_stem(m.name).rpartition("/")[2]
        found = sorted((n, r) for n, r in self.results.items()
                       if n.startswith("textures/") and not n.startswith("textures/test/")
                       and variant_stem(n).rpartition("/")[2] == base)
        classes = set(r[0] for _, r in found)
        if len(classes) != 1:
            return None
        return found[0][1][0], "variant", "copy of " + found[0][0]

    # -- driver --------------------------------------------------------------------
    def run(self):
        scope = {}
        for name in sorted(self.mats):
            why = self.out_of_scope(self.mats[name])
            if why:
                self.skipped[why] = self.skipped.get(why, 0) + 1
            else:
                scope[name] = self.mats[name]

        # signals 1 + 2 seed the propagation signals (texture share, variants)
        seeds = {}
        for name, m in scope.items():
            r = self.declared(m)
            if r:
                seeds[name] = r
        image_index = self.build_image_index(seeds)
        vindex = self.build_variant_index(seeds)

        later = []
        for name, m in scope.items():
            r = seeds.get(name) or self.texture_share(m, image_index) or self.variant(m, vindex)
            if not r:
                ruled = self.rule(m)
                r = self.head_mesh(m, ruled) or ruled
            if not r:
                later.append(m)
                continue
            self.results[name] = self.refine(m, *r)
        for m in later:                 # needs the final classes of the rest
            r = self.test_copy(m)
            if r:
                self.results[m.name] = self.refine(m, *r)
            else:
                self.unclassified[m.name] = m
        return self


# ---------------------------------------------------------------------------
# output
# ---------------------------------------------------------------------------

# the per-class presets every category-tagged row tracks (name metal rough wet
# env) - the Doom 3 table's tuned values carried over to the shared vocabulary
CAT_PRESETS = [
    ("solidmetal", 0.540, 0.460, 0.400, 0.900),
    ("hollowmetal", 0.240, 0.450, 1.000, 0.800),
    ("monstermetal", 0.540, 0.460, 0.400, 0.900),
    ("rust", 0.540, 0.450, 0.280, 0.000),
    ("armor", 0.100, 0.500, 1.000, 1.000),
    ("electronics", 0.200, 0.400, 1.000, 1.000),
    ("plastic", 0.000, 0.450, 1.000, 1.000),
    ("rubber", 0.000, 0.700, 1.000, 1.000),
    ("glass", 0.000, 0.050, 1.000, 1.000),
    ("bpglass", 0.000, 0.050, 1.000, 1.000),
    ("concrete", 0.000, 0.900, 1.000, 1.000),
    ("ceramic", 0.030, 0.320, 2.400, 0.000),
    ("rock", 0.000, 0.900, 1.000, 1.000),
    ("dirt", 0.000, 0.900, 1.000, 1.000),
    ("sand", 0.000, 0.900, 1.000, 1.000),
    ("gravel", 0.000, 0.900, 1.000, 1.000),
    ("water", 0.000, 0.150, 1.000, 1.000),
    ("oil", 0.000, 0.100, 1.000, 1.000),
    ("gelatinousfluid", 0.000, 0.200, 1.000, 1.000),
    ("skin", 0.000, 0.400, 2.000, 0.000),
    ("eyes", 0.000, 0.150, 1.000, 1.000),
    ("flesh", 0.000, 0.420, 1.420, 0.000),
    ("wood", 0.000, 0.750, 1.000, 1.000),
    ("cloth", 0.000, 0.850, 1.000, 1.000),
    ("cardboard", 0.000, 0.850, 1.000, 1.000),
]

HEADER = """\
# DUDE PBR material config for Quake 4 (docs/pbr-materials.md) - the main table.
#
# GENERATED by tools/pbr_classify_q4.py from the retail Quake 4 pk4s (q4base/*.pk4:
# material guides expanded, a later pk4 wins per file, the first definition of a
# material name wins like the engine's decl manager). Regenerate with
#   python3 tools/pbr_classify_q4.py [--q4dir "/path/to/Quake 4"]
# which rewrites this whole file (deterministic). Tune in-game with the material
# editor (editPbrMaterial): edits land in the dude folder's pbr/pbr_overrides.cfg,
# which loads on top of this table; fold them in with tools/pbr_bake_inplace.py.
#
# Columns: <material> <metalness> <roughness> <category> <wetness> <env>
# '*' = inherit: a category-tagged row tracks its live @cat preset row below
# ("@cat <class> <metal> <rough> <wet> <env>"); explicit numbers pin.
# Category = one class of the shared vocabulary (tools/pbr_classes.py):
#   Quake 4's own materialType names (materials/types/types.mtt):
#     solidmetal hollowmetal monstermetal flesh armor plastic rubber electronics
#     glass bpglass concrete rock dirt sand gravel water oil gelatinousfluid
#   DUDE refinements: skin + eyes (of flesh), ceramic (of concrete), rust (of
#     solid/hollow metal); wood, cloth, cardboard (no Quake 4 type).
#   Quake 4's "none" (no decals/effects) is no PBR class and gives no row.
#
# Every row says why (provenance), strongest signal first:
#   materialType <t>       declared in the material body (authoritative)
#   hitmap <t> N%, ...     dominant type of the materialImage hit map, by pixel
#                          share ("none" and off-palette pixels ignored)
#   texture-share <m>      shares a diffuse/bump image with a declared/hit-mapped one
#   variant of <m>         a _ut/_glow/_slime/... variant, Raven's textures/common_*
#                          c_ copy, or the left/right twin of a declared/hit-mapped one
#   token <words> / dir <prefix> / surftype <t>   name and directory rules
#   head mesh <mesh>       drawn on a models/lipsync head mesh (or a skin swapping one)
#   ... -> <class>: <why>  refinement on top (skin/eyes on heads, rust, ceramic)
# Lit materials no signal reaches are listed commented out at the end, ready to edit.
"""


def row(name, cls, prov):
    return "%-55s *    *  %-14s *  *   # (%s)" % (name, cls, prov)


def unclassified_row(m):
    """a commented-out row, columns aligned with the live ones, saying what the
    material draws so the class can be filled in by eye"""
    kinds = [k for k in LIT_STAGES if any(k == s for s, _ in m.stages)]
    diffuse = next((p for k, ps in m.stages if k == "diffusemap" for p in ps), None)
    note = "diffuse=%s" % norm_image(diffuse) if diffuse else "no diffuse"
    others = [k.replace("map", "") for k in kinds if k != "diffusemap"]
    if others:
        note += ", has " + "/".join(others)
    return "# %-53s *    *  %-14s *  *   # UNCLASSIFIED (%s)" % (m.name, "???", note)


def write_table(path, clf, classes, stats_line):
    lines = [HEADER.rstrip("\n"), "#", "# " + stats_line, "",
             "# ---- category defaults ----"]
    lines += ["@cat %s %.3f %.3f %.3f %.3f" % p for p in CAT_PRESETS]
    by_class = {}
    for name, (cls, _sig, prov) in clf.results.items():
        by_class.setdefault(cls, []).append((name, prov))
    for cls in classes:
        rows = sorted(by_class.get(cls, []))
        if rows:
            lines += ["", "# ---- %s ----" % cls]
            lines += [row(name, cls, prov) for name, prov in rows]
    if clf.unclassified:
        lines += ["", "# ---- UNCLASSIFIED: lit, no signal - fill in the class and uncomment ----"]
        groups = {}
        for name, m in clf.unclassified.items():
            groups.setdefault(name.rpartition("/")[0], []).append(m)
        for d in sorted(groups):
            lines.append("# -- %s/ (%d) --" % (d, len(groups[d])))
            for m in sorted(groups[d], key=lambda m: m.name):
                lines.append(unclassified_row(m))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--q4dir", default=DEFAULT_Q4DIR,
                    help="Quake 4 install (containing q4base/), or q4base itself")
    ap.add_argument("--out", default=os.path.join(REPO, "q4base", "pbr", "pbr_materials.cfg"),
                    help="table to write (default: <repo>/q4base/pbr/pbr_materials.cfg)")
    ap.add_argument("--samples", type=int, default=0,
                    help="also print N sample rows per signal (for sanity checks)")
    args = ap.parse_args()

    q4base = os.path.join(args.q4dir, "q4base")
    if not os.path.isdir(q4base):
        q4base = args.q4dir
    if not any(f.lower().endswith(".pk4") for f in os.listdir(q4base)):
        sys.exit("no .pk4 files in %s - pass --q4dir" % q4base)

    import pbr_classes
    fs = Pk4Fs(q4base)
    types = load_material_types(fs)
    unknown = [t for t in types if t != "none" and t not in pbr_classes.CLASSES]
    if unknown:
        sys.exit("types.mtt has types the PBR vocabulary lacks: %s" % ", ".join(unknown))
    mats, dups = load_materials(fs, load_guides(fs))
    heads = load_head_materials(fs)
    clf = Classifier(fs, mats, types, heads).run()

    lit = sum(1 for m in mats.values() if m.lit)
    in_scope = len(clf.results) + len(clf.unclassified)
    stats = "%d materials, %d lit, %d in scope: %d classified, %d unclassified" % (
        len(mats), lit, in_scope, len(clf.results), len(clf.unclassified))
    write_table(args.out, clf, pbr_classes.CLASSES, stats)

    print("wrote %s" % args.out)
    print("  " + stats)
    print("  skipped: " + ", ".join("%s %d" % kv for kv in sorted(clf.skipped.items())))
    print("  duplicate material names (first definition kept): %d" % len(dups))
    if clf.hitmaps.missing:
        print("  hit maps missing from the pk4s: %d" % len(clf.hitmaps.missing))
    sig = {}
    for cls, s, _ in clf.results.values():
        sig[s] = sig.get(s, 0) + 1
    print("  by signal: " + ", ".join("%s %d" % (s, sig[s]) for s in SIGNALS if s in sig))
    per = {}
    for cls, _, _ in clf.results.values():
        per[cls] = per.get(cls, 0) + 1
    print("  by class:  " + ", ".join("%s %d" % (c, per[c]) for c in pbr_classes.CLASSES if c in per))
    if clf.disagree:
        print("  declared materialType vs hit map disagreements (declared wins):")
        for name, mt, shares in clf.disagree:
            print("    %s: materialType %s, hitmap %s" % (name, mt, fmt_shares(shares)))
    if args.samples:
        for s in SIGNALS:
            rows = sorted((n, r) for n, r in clf.results.items() if r[1] == s)
            step = max(1, len(rows) // args.samples)
            print("\n  -- %s (%d) --" % (s, len(rows)))
            for n, (cls, _s, prov) in rows[::step][:args.samples]:
                print("    " + row(n, cls, prov))
    return 0


if __name__ == "__main__":
    sys.exit(main())
