"""DUDE PBR material classes - the one vocabulary every PBR tool and title shares.

Mirrors pbrCategoryInfos / pbrCategoryAliases in neo/renderer/Material.cpp (keep them in
step). Tokens are Quake 4's own materialType names (q4base materials/types/types.mtt)
where Raven defined one, so a Quake 4 material's declared type and its PBR class read
the same; DUDE's additions refine one of them (skin/eyes refine flesh, rust refines the
metals, ceramic refines concrete) or cover what Quake 4 has no type for. Every class
tracks its live preset row ("@cat <name> metal rough wet env"); "none" pins a row's
explicit numbers.
"""

import re

# display / output order (the engine's Categories panel groups them the same way)
CLASSES = (
    # metal
    "solidmetal", "hollowmetal", "monstermetal", "rust",
    # mineral
    "concrete", "ceramic", "rock", "dirt", "sand", "gravel",
    # organic
    "skin", "eyes", "flesh", "wood", "cloth", "cardboard",
    # synthetic
    "armor", "electronics", "plastic", "rubber",
    # glass / liquid
    "glass", "bpglass", "water", "oil", "gelatinousfluid",
)

# the pre-unification Doom 3 names, still accepted on load
ALIASES = {
    "metal": "solidmetal",
    "metal_painted": "hollowmetal",
    "ceramic_sheen": "ceramic",
    "metal_rust": "rust",
    "stone": "concrete",        # an "@cat stone" row seeds all of STONE_SPLIT
    "liquid": "water",
}

# Doom 3's "stone" was split into these; an old "@cat stone" row sets all of them
STONE_SPLIT = ("concrete", "rock", "dirt", "sand", "gravel")

CLASS_ORDER = {c: i for i, c in enumerate(CLASSES)}

# Doom 3 "stone" -> which split class, by material name (the 2026-10-28 migration rule;
# pbr_classify.py applies it to its "stone" output). Construction words win, then natural
# rock, then the loose grounds; folders only decide what the name leaves open.
_STONE_CONCRETE = ("brick", "cement", "concrete", "plaster", "tile", "stair", "pillar", "trim",
                   "tablet", "column", "block", "marble", "slab")
_STONE_ROCK = ("rock", "cliff", "cave", "lava", "frozen", "mountain", "boulder", "canyon",
               "crater", "hillside", "glacier", "rubble", "cracked")


def _stone_leaf(leaf):
    toks = [t for t in re.split(r"[^a-z]+", leaf) if t]
    joined = " ".join(toks)
    if any(k in joined for k in _STONE_CONCRETE):
        return "concrete"
    if any(k in joined for k in _STONE_ROCK) or any(t.startswith("ice") for t in toks):
        return "rock"
    if any(t.startswith(("gravel", "pebble")) for t in toks):
        return "gravel"
    if any("sand" in t and "sandbag" not in t for t in toks):
        return "sand"
    if toks and toks[0].startswith(("dirt", "mud", "soil")):    # "skpanel11_dirt" = a dirty panel
        return "dirt"
    return None


def split_stone(name):
    """a Doom 3 "stone" material -> concrete / rock / dirt / sand / gravel."""
    parts = name.lower().split("/")
    leaf, dirs = parts[-1], parts[:-1]
    cls = _stone_leaf(leaf)
    if cls:
        return cls
    if any(d in ("rock", "rockd3xp", "hell", "helld3xp") for d in dirs):
        return "rock"
    if "caves" in dirs and leaf.startswith("cav"):
        return "rock"
    return "concrete"


def canonical(token):
    """table category token -> class name, or "none" (none / absent / unknown)."""
    t = (token or "").lower()
    t = ALIASES.get(t, t)
    return t if t in CLASS_ORDER else "none"
