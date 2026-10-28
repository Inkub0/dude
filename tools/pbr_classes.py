"""DUDE PBR material classes - the one vocabulary every PBR tool and title shares.

Mirrors pbrCategoryInfos / pbrCategoryAliases in neo/renderer/Material.cpp (keep them in
step). Tokens are Quake 4's own materialType names (q4base materials/types/types.mtt)
where Raven defined one, so a Quake 4 material's declared type and its PBR class read
the same; DUDE's additions refine one of them (skin/eyes refine flesh, rust refines the
metals, ceramic refines concrete) or cover what Quake 4 has no type for. Every class
tracks its live preset row ("@cat <name> metal rough wet env"); "none" pins a row's
explicit numbers.
"""

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


def canonical(token):
    """table category token -> class name, or "none" (none / absent / unknown)."""
    t = (token or "").lower()
    t = ALIASES.get(t, t)
    return t if t in CLASS_ORDER else "none"
