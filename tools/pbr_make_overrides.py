#!/usr/bin/env python3
"""
Emit a fully-populated, hand-editable PBR override file from the generated table.

The engine (neo/renderer/Material.cpp) loads pbr/pbr_materials.cfg (generated, do
not hand-edit) and then pbr/pbr_overrides.cfg on top (wins on collision). This
tool snapshots the *merged* result — every material with its current
classification — into the override file, so an artist can open one file and tune
any surface. See docs/pbr-materials.md sec. 6/8.

Column layout (shared by both files; override is a superset):

    <material>  <metalness>  <roughness>  <category>  <wetness>  <env>

Any numeric column may be '*' meaning inherit (metalness/roughness -> the
per-category slider or r_pbr* global; wetness -> the per-category wetness cvar,
1.0 off-category; env -> 1.0). A real <category> makes the line track the live
Developer-tab sliders; 'none' pins the explicit numbers. wetness/env default to
'*' here so the wetness sliders stay live on skin/flesh out of the box.

It also appends, as COMMENTED lines, every *lit* material the classifier left
unclassified (no table entry -> global fallback). Those are invisible in the
generated table, so they get surfaced here for discovery: uncomment + set values
to bring one into PBR. Many carry a 'no bump' flag — Doom 3 tends to leave the
normal map off flat surfaces, which are often smooth/polished (granite counters,
glass) but sometimes just flat-matte (paper, signage) — so check in-game before
glossing. Surfacing needs the pk4s: pass --root/--game (imports tools/pbr_classify).

Usage:
    pbr_make_overrides.py --table base/pbr/pbr_materials.cfg \\
        --out base/pbr/pbr_overrides.cfg [--merge FILE] [--root . --game base]

--merge defaults to --out (so a regen preserves your hand edits: the current
override file is read back and merged on top of itself). --root/--game enable the
unclassified-lit scan; omit them to just snapshot the table.
"""

import argparse
import os
import sys


def parse_num(tok):
    """A table numeric column: '*'/'-'/'auto' -> None (inherit), else float."""
    if not tok or tok[0] in "*-" or tok.lower() == "auto":
        return None
    try:
        return float(tok)
    except ValueError:
        return None


def parse_file(path):
    """name(lower) -> dict(metal, rough, cat, wet, env, comment). Later lines win."""
    out = {}
    if not path or not os.path.exists(path):
        return out
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for raw in fh:
            comment = ""
            h = raw.find("#")
            if h >= 0:
                comment = raw[h + 1:].strip()
                raw = raw[:h]
            toks = raw.split()
            if len(toks) < 3:
                continue
            name = toks[0]
            out[name.lower()] = {
                "name": name,
                "metal": parse_num(toks[1]),
                "rough": parse_num(toks[2]),
                "cat": toks[3] if len(toks) >= 4 else "none",
                "wet": parse_num(toks[4]) if len(toks) >= 5 else None,
                "env": parse_num(toks[5]) if len(toks) >= 6 else None,
                "comment": comment,
            }
    return out


def scan_unclassified_lit(root, game, have_keys):
    """
    Return [(name, flags_str)] for every LIT, NO-BUMP material the classifier
    leaves genuinely unclassified (reason == 'unclassified'; skip-dirs, weapons
    and out-of-scope are deliberately excluded) and that isn't already in
    have_keys. These are the real gap: Doom 3 omits the normal map on flat
    surfaces, which then miss both a table entry AND any Toksvig roughness cue, so
    they sit on the matte global fallback even when they're smooth/polished.
    Bump-mapped unclassified materials render acceptably on the generic fallback
    and are left out to keep the surfaced list focused. 'flags_str' notes 'spec'.
    Requires tools/pbr_classify importable and the pk4s under root.
    """
    try:
        import pbr_classify as P
    except Exception as e:                       # pragma: no cover
        sys.stderr.write("scan skipped (can't import pbr_classify: %s)\n" % e)
        return []
    game_dirs = [os.path.join(root, "base")]
    if game != "base":
        game_dirs.append(os.path.join(root, game))
    try:
        text = P.strip_comments(P.load_mtr_sources(game_dirs))
        head = P.load_head_materials(game_dirs)
    except Exception as e:                       # pragma: no cover
        sys.stderr.write("scan skipped (mtr load failed: %s)\n" % e)
        return []
    found = []
    for name, body in P.iter_materials(text):
        if name.lower() in have_keys:
            continue
        b = set(t.lower() for t in body)
        hd, hs, hb = "diffusemap" in b, "specularmap" in b, "bumpmap" in b
        if hb or not (hd or hs):                 # only lit surfaces with NO bump
            continue
        cat, reason = P.classify(name, body, head)
        if cat is not None or reason != "unclassified":
            continue                             # classified, weapon, skip-dir...
        flags = "no bump, spec" if hs else "no bump"
        found.append((name, flags))
    found.sort(key=lambda t: t[0].lower())
    return found


def entries_equiv(a, b):
    """Semantic equality for the --parent diff: same category, wetness and env;
    metalness/roughness compared only for non-slider categories (slider-driven ones
    ignore those columns at draw time, so '*' vs a baked number is the same look)."""
    if a["cat"].lower() != b["cat"].lower():
        return False
    if a["wet"] != b["wet"] or a["env"] != b["env"]:
        return False
    if a["cat"].lower() in CAT_ORDER:      # slider-driven -> metal/rough ignored
        return True
    return a["metal"] == b["metal"] and a["rough"] == b["rough"]


def fmt_num(v, width):
    return ("*" if v is None else f"{v:.2f}").rjust(width)


HEADER = """\
# DUDE PBR per-material overrides (docs/pbr-materials.md Phase B) — FULL SNAPSHOT
#
# Generated by tools/pbr_make_overrides.py from pbr/pbr_materials.cfg (+ any prior
# overrides merged on top). This is the hand-edit file: it is loaded AFTER the
# generated table and wins on every collision, so editing a line here retunes that
# material live (run `reloadPbrTable` in the console; r_pbr 1; r_showSurfaceInfo 1
# reads the material name under the crosshair).
#
# Columns:
#   <material>  <metalness>  <roughness>  <category>  <wetness>  <env>
#
#   metalness  0 = dielectric (stone/wood/skin/plastic), 1 = bare metal.
#              Effective value clamped by r_pbrMetalnessMax.
#   roughness  0.03 = mirror .. 1.0 = fully matte.
#   category   metal | metal_painted | ceramic_sheen | metal_rust | stone |
#              skin | eyes | flesh | none  (long-tail names like glass/wood/cloth
#              behave as 'none'). A real category tracks the live per-category
#              Developer-tab sliders and IGNORES the metalness/roughness numbers
#              on this line; set it to 'none' (or leave it) to PIN the numbers.
#   wetness    per-material specular-energy multiplier (the wet/sweat/slime film).
#              1.0 = neutral. '*' = inherit (skin/eyes -> r_pbrSkinWetness,
#              flesh -> r_pbrFleshWetness, everything else 1.0).
#   env        per-material metal env-glow multiplier over r_pbrEnvScale (only
#              affects metals). 1.0 = neutral. '*' = inherit (1.0).
#
# Any numeric column may be '*' to inherit. To hard-pin one surface: give it real
# metalness/roughness numbers and set its category to 'none'. To make a wall look
# wet: raise its wetness. To reclassify a texture: change its category word.
#
# This file is a complete snapshot, so it is large; you may safely delete any line
# you don't intend to customize — the generated table still supplies its default.
"""

UNCLASSIFIED_HEADER = """\

# =============================================================================
# UNCLASSIFIED lit materials — no table entry, so they fall back to the r_pbr*
# globals (metalness 0, roughness r_pbrRoughness). They were invisible in the
# generated table; surfaced here so you can opt them in. Each is COMMENTED at the
# neutral fallback (uncomment to change nothing, then tune). 'no bump' = Doom 3
# left the normal map off: often a smooth/polished or flat surface (granite
# counters, glass) but sometimes flat-matte (paper, signage) — check in-game
# before lowering roughness. Uncomment (drop the leading '# ') and edit to enable.
# =============================================================================
"""

MATERIALS_HEADER = """\
# DUDE PBR material config (docs/pbr-materials.md) — the main, authored table.
#
# Bootstrapped by tools/pbr_classify.py, then hand-tuned in-game with the material
# editor (editPbrMaterial) and baked in here. Loaded first; the per-game
# pbr/pbr_overrides.cfg loads on top and wins on collision. This is the shippable
# base config — running pbr_classify.py again regenerates a fresh classifier
# baseline and DISCARDS the hand-tuning below, so fold overrides in instead:
#   tools/pbr_make_overrides.py --bake --table <this file> --out <overrides> ...
#
# Columns: <material> <metalness> <roughness> <category> <wetness> <env>
# '*' = inherit (slider-driven categories track their live cvar); explicit numbers
# pin. Category 'none' / long-tail (glass/wood/cloth...) carry real numbers.
"""

OVERRIDES_EMPTY_HEADER = """\
# DUDE PBR per-game overrides (docs/pbr-materials.md).
#
# Loaded after pbr/pbr_materials.cfg and wins on collision — the place for
# game-specific or experimental per-material tweaks. The in-game editor
# (editPbrMaterial) writes here. Empty by default: the base tuning lives in
# pbr_materials.cfg. Fold accumulated edits back down with:
#   tools/pbr_make_overrides.py --bake --table <materials> --out <this file> ...
#
# Columns: <material> <metalness> <roughness> <category> <wetness> <env>; '*' = inherit.
"""

SLIM_PARENT_HEADER = """\
# DUDE PBR per-game override (docs/pbr-materials.md) — DELTA over the shared base.
#
# This game has NO pbr_materials.cfg of its own: it inherits
# base/pbr/pbr_materials.cfg through the VFS fallback (mod dir searched first, then
# base). This file carries only what differs — game-only materials plus deliberate
# per-game edits — so the base tuning is not duplicated (DRY). The in-game editor
# writes here; regenerate the delta with:
#   tools/pbr_make_overrides.py --parent base/pbr/pbr_materials.cfg --table <game baseline> ...
#
# Columns: <material> <metalness> <roughness> <category> <wetness> <env>; '*' = inherit.
"""

# order categories for grouped, skimmable output
CAT_ORDER = {
    "metal": 0, "metal_painted": 1, "ceramic_sheen": 2, "metal_rust": 3,
    "stone": 4, "skin": 5, "eyes": 6, "flesh": 7,
}


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    ap.add_argument("--table", required=True, help="generated pbr_materials.cfg")
    ap.add_argument("--out", required=True, help="override file to write")
    ap.add_argument("--merge", default=None,
                    help="prior overrides to merge on top (default: --out itself)")
    ap.add_argument("--root", default=None,
                    help="game root with base/, d3xp/ — enables the unclassified scan")
    ap.add_argument("--game", default="base", help="mod dir for the scan (base/d3xp)")
    ap.add_argument("--bake", action="store_true",
                    help="fold the merged result into --table (the main authored "
                         "config) and reset --out to an empty per-game override")
    ap.add_argument("--parent", default=None,
                    help="a parent config (e.g. base/pbr/pbr_materials.cfg) the game "
                         "inherits via VFS fallback: emit only entries missing from it "
                         "or that differ (DRY per-game override; drops the unclassified "
                         "section). Incompatible with --bake.")
    args = ap.parse_args()

    merge_path = args.merge if args.merge is not None else args.out

    merged = parse_file(args.table)
    for key, e in parse_file(merge_path).items():
        e = dict(e)
        # a prior override with no category is an explicit pin: force 'none' so the
        # generated category (which may be wrong — that's why it was overridden)
        # can't drag the pinned numbers back to a slider.
        if e["cat"].lower() in ("none", "*", "-"):
            e["cat"] = "none"
        if not e["comment"]:
            e["comment"] = "hand-pinned (kept from prior pbr_overrides.cfg)"
        merged[key] = e

    def sort_key(e):
        return (CAT_ORDER.get(e["cat"].lower(), 99), e["name"].lower())

    entries = sorted(merged.values(), key=sort_key)

    # DRY: the game inherits --parent (e.g. base) via VFS fallback, so keep only
    # (a) materials the parent lacks (game-only) and (b) genuine hand-edits — entries
    # that differ from THIS game's classifier baseline (--table). A shared material
    # the parent tuned but this game never edited matches its own baseline, so it's
    # dropped and the parent's tuned value shows through (not the classifier default).
    if args.parent:
        parent = parse_file(args.parent)
        baseline = parse_file(args.table)
        def is_delta(e):
            k = e["name"].lower()
            if k not in parent:
                return True                                  # game-only material
            # inheritable from the parent: keep only a genuine hand-edit (differs
            # from this game's own classifier baseline) whose value also differs
            # from what the parent already supplies (else the parent covers it).
            b = baseline.get(k)
            edited = b is None or not entries_equiv(e, b)
            return edited and not entries_equiv(e, parent[k])
        entries = [e for e in entries if is_delta(e)]

    lines = [MATERIALS_HEADER if args.bake else
             (SLIM_PARENT_HEADER if args.parent else HEADER)]
    cur_cat = None
    for e in entries:
        cat = e["cat"]
        if cat != cur_cat:
            lines.append(f"\n# ---- {cat} ----")
            cur_cat = cat
        name = e["name"]
        pad = name if len(name) < 52 else name + "  "
        # slider-driven categories (the ones the engine maps to live cvars) ignore
        # the metalness/roughness columns at draw time, so emit '*' (inherit) rather
        # than a baked number that would go stale when the category default changes —
        # matching what the in-game editor writes. Pinned 'none' and long-tail
        # categories (glass/wood/cloth...) keep explicit numbers (they ARE used).
        driven = cat.lower() in CAT_ORDER
        mtok = fmt_num(None if driven else e['metal'], 4)
        rtok = fmt_num(None if driven else e['rough'], 4)
        row = (f"{pad:<52} {mtok} {rtok}"
               f"  {cat:<14} {'*' if e['wet'] is None else format(e['wet'], '.2f')}"
               f"  {'*' if e['env'] is None else format(e['env'], '.2f')}")
        if e["comment"]:
            row += f"   # {e['comment']}"
        lines.append(row)

    unclassified = []
    if args.root and not args.parent:
        unclassified = scan_unclassified_lit(args.root, args.game, set(merged.keys()))
        if unclassified:
            lines.append(UNCLASSIFIED_HEADER.rstrip("\n"))
            for name, flags in unclassified:
                pad = name if len(name) < 50 else name + "  "
                lines.append(f"# {pad:<50} 0.00 0.58 none  *  *   # UNCLASSIFIED: {flags}")

    if args.bake:
        with open(args.table, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines) + "\n")
        with open(args.out, "w", encoding="utf-8") as fh:
            fh.write(OVERRIDES_EMPTY_HEADER)
        sys.stderr.write("baked %d entries + %d unclassified -> %s ; reset %s to empty\n"
                         % (len(entries), len(unclassified), args.table, args.out))
    else:
        with open(args.out, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines) + "\n")
        sys.stderr.write("wrote %d active + %d commented-unclassified -> %s\n"
                         % (len(entries), len(unclassified), args.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
