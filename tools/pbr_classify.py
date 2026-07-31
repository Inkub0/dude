#!/usr/bin/env python3
"""DUDE PBR material classifier (docs/pbr-materials.md Phase B).

Scans every .mtr in a game directory's pk4s (plus loose materials/*.mtr),
classifies each material into a coarse physical category, and emits the
per-material scalar table the renderer loads at material-parse time:

    <game>/pbr/pbr_materials.cfg      one "<name> <metalness> <roughness>" per line

The table is *generated* — never hand-edit it. Per-material corrections go in
<game>/pbr/pbr_overrides.cfg (same format), which the engine loads second so it
wins on collision; `reloadPbrTable` in the console re-applies edits live.

Classification signals, in priority order (docs/pbr-materials.md sec. 2-3):
  1. declared surface type in the material body (sparse but authoritative)
  2. keyword tokens in the material name + its stage texture paths
  3. directory priors (e.g. base_wall/ -> worn station metal, caves/ -> stone)
Materials with no signal get no entry and fall back to the r_pbr* globals.

Usage:
    python3 tools/pbr_classify.py            # base game -> base/pbr/pbr_materials.cfg
    python3 tools/pbr_classify.py --game d3xp  # base+d3xp pk4s -> d3xp/pbr/...
"""

import argparse
import os
import re
import sys
import zipfile

# category -> (metalness, roughness); bands from docs/pbr-materials.md sec. 3,
# calibrated around r_pbrRoughness 0.58 = the Blinn-exp-16-equivalent look.
#
# Metalness note: PBR metalness means *bare* metal at the surface. Painted or
# coated metal is a dielectric — light interacts with the paint, not the steel
# beneath — so the station's painted panelling gets a low metalness with just a
# hint of metallic response from scuffs, and rust (a dielectric oxide) pulls
# rusted metal down to a patchy mix. Full metalness 1.0 is reserved for
# genuinely bare-metal surfaces (grates, pipes, machined steel, chrome).
CATEGORIES = {
    "metal":         (0.8, 0.32),   # bare machined metal / grates / pipes (0.8 =
                                    # r_pbrMetalMetalness: keeps a diffuse sliver so
                                    # metals don't go black between lights; the old
                                    # global r_pbrMetalnessMax cap was removed)
    "metal_painted": (0.2, 0.55),   # painted station panelling, coated fixtures
    "ceramic_sheen": (0.2, 0.45),   # glossy hard surfaces: painted floors and
                                    # ceramic tile (floors AND walls, e.g. the
                                    # washroom). Tighter roughness than the wall
                                    # panelling stretches lights into streaks —
                                    # the wet-floor / glazed-tile look
    "metal_rust":    (0.4, 0.78),   # rusted metal (oxide is dielectric -> mixed)
    "stone":         (0.0, 0.90),   # rock, concrete, brick, plaster
    "wood":          (0.0, 0.75),
    "skin":          (0.0, 0.40),   # human heads/faces: tight oily sheen so facial
                                    # detail pops (user request; the firefly clamp
                                    # bounds the core so it can't burn out)
    "eyes":          (0.0, 0.15),   # eyes + teeth: cornea/enamel, the wettest,
                                    # hardest surfaces on a face — flashlight
                                    # catchlights in the dark
    "flesh":         (0.0, 0.55),   # body flesh, meat, hell-growth
    "plastic":       (0.0, 0.45),   # plastic, rubber, vinyl
    "cloth":         (0.0, 0.85),
    "cardboard":     (0.0, 0.85),
    "glass":         (0.0, 0.05),
    "liquid":        (0.0, 0.15),
}

# declared surface type keyword -> category (ricochet behaves like metal)
SURFTYPE_TO_CATEGORY = {
    "metal": "metal", "ricochet": "metal", "stone": "stone", "flesh": "flesh",
    "wood": "wood", "cardboard": "cardboard", "glass": "glass",
    "plastic": "plastic", "liquid": "liquid",
}

# keyword tokens matched against words split from the material name and its
# stage texture paths (split on / _ . -). Each category has exact-match tokens
# (short/ambiguous words where substring matching would misfire: "rock" in
# "rocket", "tin" in "testing") and substring tokens (long, distinctive words
# that appear glued into compounds like "sopanel"/"tecpipebraid"/"burntflesh").
# Highest total hit count wins; dict order below is the tie-break priority
# (most specific first, metal last because its list is the broadest).
TOKEN_SETS = {
    # ordered before flesh so a head material with gore words still reads as a
    # face; "head"/"face" match as *prefixes* below (head04, faces) rather than
    # substrings so bulkhead/surface/interface can't misfire
    "skin":      ({"skin"},
                  set()),
    "flesh":     ({"meat", "vein", "veins", "gut", "guts", "gore",
                   "brain", "heart", "teeth", "belly"},
                  {"flesh", "organ", "tissue", "wound", "tongue"}),
    "glass":     ({"lens"},
                  {"glass", "crystal"}),
    "liquid":    ({"slime"},
                  {"liquid", "water"}),
    "cardboard": (set(),
                  {"cardboard", "carton"}),
    "cloth":     ({"cloth"},
                  {"fabric", "canvas", "curtain", "carpet", "leather",
                   "mattress"}),
    "wood":      ({"wood"},
                  {"wooden", "plank"}),
    "plastic":   ({"rubber", "vinyl"},
                  {"plastic"}),
    "stone":     ({"rock", "sand", "dirt", "mud", "cliff", "cave", "lava",
                   "stone", "brick", "cement"},
                  {"concrete", "marble", "plaster", "asphalt", "gravel",
                   "boulder", "granite"}),
    # bare metal: words that mean exposed metal at the surface. "sflpanel" =
    # the base_floor "steel floor panel" family (sflpanel1..8 + variants) —
    # dark riveted steel plates that the generic "panel" token was routing to
    # painted->ceramic via the floor remap; user-identified as bare metal.
    # The tie with metal_painted's "panel" hit resolves to metal by dict order.
    "metal":     ({"iron", "alum", "copper", "brass", "tin", "pipes", "grates",
                   "mesh", "vent", "vents", "duct", "rail", "railing",
                   "bolt", "rivet", "tread", "wire"},
                  {"metal", "steel", "chrome", "aluminum", "pipe", "grate",
                   "grating", "girder", "sflpanel"}),
    # painted/coated metal objects: metal things whose visible surface is paint
    "metal_painted": ({"hull", "tank", "plate", "plates", "mech", "server",
                       "hatch"},
                      {"panel", "machine", "locker", "elevator"}),
}

# substrings that refine any metal result to rusted (substring so compounds
# like "rustpanel" / "gotrustcol" match). dirty/stain: a dirty or stained metal
# reads as weathered -> rust category. Note the asset vocabulary is sparse
# (~25 "rust" + ~30 dirty/stain in base): most of Doom 3's rusty *look* is
# painted into the diffuse art of neutrally-named panels, which is why the
# metal_painted category is the mass lever for the grungy-panel appearance.
RUST_TOKENS = ("rust", "oxid", "corro", "dirty", "stain")

# second-level directory (textures/<dir>/...) -> category prior, used when no
# surftype and no token hits. The base_* / station dirs are Mars-station
# panelling almost throughout — *painted* metal, i.e. mostly-dielectric
# (see the metalness note above); the override file exists for the exceptions.
DIR_PRIORS = {
    "base_wall": "metal_painted",
    "base_trim": "metal_painted", "base_door": "metal_painted",
    "base_light": "metal_painted", "mcity": "metal_painted", "enpro": "metal_painted",
    "lab": "metal_painted", "alphalabs": "metal_painted", "cpu": "metal_painted",
    "recyc_wall": "metal_painted",
    "morgue": "metal_painted",
    # bathroom ceramic tile is glossy, not matte stone — ride the floor class
    # so the Floor Roughness slider drives the tiles too (user request; the
    # metal panels/pipes in there still win their own tokens)
    "washroom": "ceramic_sheen",
    # floors get their own class so ceiling lights can streak on them (tighter
    # roughness than the walls); grate floors still win the bare-metal tokens
    "base_floor": "ceramic_sheen", "recyc_floor": "ceramic_sheen",
    "caves": "stone", "rock": "stone", "outside": "stone", "hell": "stone",
    "glass": "glass",
}

# textures/<dir> namespaces that never draw lit interactions (or aren't
# surfaces at all) — emit nothing for them
SKIP_DIRS = {"particles", "sfx", "decals", "skies", "common", "gui"}

# models/<dir> -> category prior. Weapons deliberately absent: viewmodels fill
# the screen and are mostly painted/polymer, and the early metal prior made
# them read badly (metalness kills diffuse with no Phase C env reflections at
# point-blank view) — they're excluded below and render on the global fallback.
MODEL_PRIORS = {
    "characters": "flesh", "monsters": "flesh", "md5": "flesh",
}

# models/mapobjects/<sub> -> category prior for the recurring fixture families
# (doors, machinery, light housings); the rest of mapobjects is too mixed for
# a prior and falls through to the global fallback
MAPOBJECT_PRIORS = {
    "doors": "metal_painted", "cpu": "metal_painted", "lab": "metal_painted",
    "recycle": "metal_painted", "skmachines": "metal_painted",
    "elevators": "metal_painted", "teleporter": "metal_painted",
    "lights": "metal", "swinglights": "metal", "pipes": "metal",
}

WORD_SPLIT = re.compile(r"[/_.\-]+")
PATHISH = re.compile(r"[A-Za-z0-9_\-]+(?:/[A-Za-z0-9_.\-]+)+")
MD5_SHADER = re.compile(r'shader\s+"([^"]+)"')

# surftypes that must survive the head-mesh skin signal: a helmet on a head
# mesh is still metal, a visor still glass
HEAD_KEEP_SURFTYPES = ("metal", "ricochet", "glass", "plastic")


def load_head_materials(game_dirs):
    """Ground-truth head materials: every shader referenced by an md5 head
    mesh (models/md5/**head**.md5mesh). This is what actually draws on heads —
    it catches the named cast (Betruger, Campbell, Swann, Sarge), whose face
    textures live on full character sheets the name heuristics can't see, plus
    eyes, teeth and every zombie head."""
    mats = set()
    for game in game_dirs:
        if not os.path.isdir(game):
            continue
        for pk4 in sorted(f for f in os.listdir(game) if f.lower().endswith(".pk4")):
            with zipfile.ZipFile(os.path.join(game, pk4)) as z:
                for entry in z.namelist():
                    low = entry.lower()
                    if low.startswith("models/md5/") and "head" in low and low.endswith(".md5mesh"):
                        text = z.read(entry).decode("utf-8", "replace")
                        for shader in MD5_SHADER.findall(text):
                            mats.add(shader.lower().replace("//", "/"))
    return mats


def strip_comments(text):
    # single-pass alternation, processed left to right like a real lexer: a //
    # comment consumes to end of line (including any stray "/*" inside it), a
    # /* comment consumes to its own "*/". Stripping the two kinds in separate
    # passes broke on a "/*" buried inside a // line — the block pass ran first
    # and ate everything up to the next "*/" in a *later* concatenated file,
    # silently swallowing whole .mtr files (washroom.mtr was a casualty).
    return re.sub(r"//[^\n]*|/\*.*?\*/", " ", text, flags=re.S)


def iter_materials(text):
    """Yield (name, body) for each top-level material decl; skips table decls."""
    toks = text.replace("{", " { ").replace("}", " } ").split()
    i, n = 0, len(toks)
    while i < n:
        tok = toks[i]
        if tok == "table":
            # table <name> { ... }  — skip the balanced block
            i += 2
            depth = 0
            while i < n:
                if toks[i] == "{":
                    depth += 1
                elif toks[i] == "}":
                    depth -= 1
                    if depth == 0:
                        break
                i += 1
            i += 1
            continue
        if i + 1 < n and toks[i + 1] == "{":
            name = tok
            i += 2
            depth = 1
            body = []
            while i < n and depth > 0:
                if toks[i] == "{":
                    depth += 1
                elif toks[i] == "}":
                    depth -= 1
                    if depth == 0:
                        break
                body.append(toks[i])
                i += 1
            i += 1
            yield name.lower(), body
        else:
            i += 1


def classify(name, body, head_mats=frozenset()):
    """Return (category, source) or (None, reason)."""
    parts = name.split("/")

    # blood check first, before the decals/particles skip: blood stains are
    # decals, and ~40% of them carry full lit bump/spec stages (bloodpool01,
    # bloodrun01, hell's blood blends) — route them to flesh so the gore-film
    # wetness makes them glisten (user request). Name-based only: a bloodied
    # pipe or cabinet reading as wet flesh is the intended horror effect.
    if parts[0] in ("textures", "models") and any(
            t in name for t in ("blood", "gore", "gib")):
        return "flesh", "blood"

    if parts[0] == "textures" and len(parts) > 1 and parts[1] in SKIP_DIRS:
        return None, "skip-dir"
    if parts[0] not in ("textures", "models"):
        return None, "out-of-scope"

    # weapons get no entry at all (not even token matches): the viewmodel is
    # the closest, most-stared-at surface in the game and reads best on the
    # global dielectric fallback until Phase C gives metals real reflections
    if parts[0] == "models" and len(parts) > 1 and parts[1] == "weapons":
        return None, "skip-dir"

    # ground truth beats every heuristic: this material draws on an md5 head
    # mesh -> skin, unless its declared surftype says it's gear (helmet ->
    # metal, visor -> glass), in which case fall through to the normal rules.
    # Within a head, the eye/teeth materials (named left*/right*/teeth* in
    # characters/common) split into the harder, wetter `eyes` class.
    if name in head_mats:
        body_low = set(t.lower() for t in body)
        if not any(st in body_low for st in HEAD_KEEP_SURFTYPES):
            base = parts[-1]
            if base.startswith(("left", "right", "teeth", "eye")):
                return "eyes", "headmesh"
            return "skin", "headmesh"

    # words from the material name + every path-looking token in the body
    # (bumpmap/diffusemap/specularmap/addnormals(...) arguments etc.)
    haystack = set(WORD_SPLIT.split(name))
    body_str = " ".join(body).replace("(", " ").replace(")", " ").replace(",", " ")
    for path in PATHISH.findall(body_str):
        haystack.update(WORD_SPLIT.split(path.lower()))

    rusty = any(rt in w for w in haystack for rt in RUST_TOKENS)

    # eyes outside the head meshes (monster eyeballs etc.): eye-prefixed words
    # under models/ only. Checked before the head/face rule so "head02_eyes"
    # reads as eyes, not skin.
    if parts[0] == "models" and any( w.startswith( "eye" ) for w in haystack ):
        return "eyes", "token"

    # human heads/faces -> skin, checked as word *prefixes* (head04, face2 hit;
    # bulkhead, surface, interface don't) and only under models/ (characters,
    # monsters), so hell's face-covered walls and weapon faceplates can't
    # misfire. Beats every other signal — including a declared flesh surftype —
    # because a head material full of gore words is still a face.
    if parts[0] == "models" and any(
            w.startswith("head") or w.startswith("face") for w in haystack):
        return "skin", "token"

    def rustify(cat):
        return "metal_rust" if rusty and cat in ("metal", "metal_painted") else cat

    # 1. declared surface type (standalone token in the body)
    body_set = set(t.lower() for t in body)
    for st, cat in SURFTYPE_TO_CATEGORY.items():
        if st in body_set:
            return rustify(cat), "surftype"

    # 2. keyword tokens, highest hit count wins (dict order breaks ties)
    best, best_hits = None, 0
    for cat, (exact, subs) in TOKEN_SETS.items():
        hits = len(haystack & exact)
        hits += sum(1 for w in haystack - exact if any(s in w for s in subs))
        if hits > best_hits:
            best, best_hits = cat, hits
    if best is not None:
        # a painted "panel"/"plate" that lives in an actual floor dir is a
        # floor panel: keep it in the floor class so the floor slider covers it
        # (bare-metal tokens like grates still win outright above). Deliberately
        # NOT keyed on DIR_PRIORS, which also routes the washroom's tile *walls*
        # through the floor class — its metal wall panels should stay walls.
        if best == "metal_painted" and parts[0] == "textures" and len(parts) > 1 \
                and parts[1] in ("base_floor", "recyc_floor"):
            best = "ceramic_sheen"
        return rustify(best), "token"

    # 3. directory priors
    if parts[0] == "textures" and len(parts) > 1 and parts[1] in DIR_PRIORS:
        return rustify(DIR_PRIORS[parts[1]]), "dir"
    if parts[0] == "models" and len(parts) > 1:
        if parts[1] == "mapobjects" and len(parts) > 2 and parts[2] in MAPOBJECT_PRIORS:
            return rustify(MAPOBJECT_PRIORS[parts[2]]), "dir"
        if parts[1] in MODEL_PRIORS:
            return rustify(MODEL_PRIORS[parts[1]]), "dir"

    return None, "unclassified"


def load_mtr_sources(game_dirs):
    """Return concatenated .mtr text honoring load order (later overrides)."""
    chunks = []
    for game in game_dirs:
        pk4s = sorted(
            f for f in os.listdir(game) if f.lower().endswith(".pk4")
        ) if os.path.isdir(game) else []
        for pk4 in pk4s:
            with zipfile.ZipFile(os.path.join(game, pk4)) as z:
                for entry in sorted(z.namelist()):
                    if entry.lower().startswith("materials/") and entry.lower().endswith(".mtr"):
                        chunks.append(z.read(entry).decode("utf-8", "replace"))
        loose = os.path.join(game, "materials")
        if os.path.isdir(loose):
            for fn in sorted(os.listdir(loose)):
                if fn.lower().endswith(".mtr"):
                    with open(os.path.join(loose, fn), encoding="utf-8", errors="replace") as f:
                        chunks.append(f.read())
    return "\n".join(chunks)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--game", default="base",
                    help="mod dir to generate for (d3xp also scans base first)")
    ap.add_argument("--root", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                    help="game root containing base/, d3xp/ (default: repo root)")
    args = ap.parse_args()

    game_dirs = [os.path.join(args.root, "base")]
    if args.game != "base":
        game_dirs.append(os.path.join(args.root, args.game))

    text = strip_comments(load_mtr_sources(game_dirs))
    head_mats = load_head_materials(game_dirs)

    entries = {}          # name -> (category, source); later decls override
    stats = {"skip-dir": 0, "out-of-scope": 0, "unclassified": 0}
    for name, body in iter_materials(text):
        cat, source = classify(name, body, head_mats)
        if cat is None:
            entries.pop(name, None)
            stats[source] = stats.get(source, 0) + 1
        else:
            entries[name] = (cat, source)

    out_dir = os.path.join(args.root, args.game, "pbr")
    os.makedirs(out_dir, exist_ok=True)
    out_path = os.path.join(out_dir, "pbr_materials.cfg")

    cat_counts = {}
    with open(out_path, "w", encoding="utf-8") as f:
        f.write("# BOOTSTRAP baseline from tools/pbr_classify.py — a fresh classifier pass.\n")
        f.write("# WARNING: this OVERWRITES the authored pbr_materials.cfg and DISCARDS any\n")
        f.write("# hand-tuning baked into it. Bake accumulated pbr_overrides.cfg edits back in\n")
        f.write("# afterwards: tools/pbr_make_overrides.py --bake (docs/pbr-materials.md sec. 6).\n")
        f.write("# format: <material> <metalness> <roughness> [category]   (docs/pbr-materials.md Phase B)\n")
        f.write("# the category column lets the engine's live per-category cvars (Developer tab)\n")
        f.write("# override the baked numbers for the main material classes\n")
        for name in sorted(entries):
            cat, source = entries[name]
            metal, rough = CATEGORIES[cat]
            cat_counts[cat] = cat_counts.get(cat, 0) + 1
            f.write("%s %.2f %.2f %s  # (%s)\n" % (name, metal, rough, cat, source))

    total = len(entries)
    print("wrote %s: %d entries" % (out_path, total))
    for cat in CATEGORIES:
        if cat in cat_counts:
            print("  %-11s %5d" % (cat, cat_counts[cat]))
    print("  no entry (global fallback): %d unclassified, %d skip-dir, %d out-of-scope"
          % (stats.get("unclassified", 0), stats.get("skip-dir", 0), stats.get("out-of-scope", 0)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
