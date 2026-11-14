#!/usr/bin/env python3
"""Derive starting PBR sidecar textures for stock materials from the game's own data.

Doom 3 / Quake 4 materials carry a diffuse, a normal map and a specular map, but no roughness,
metalness or AO. This tool reads the materials straight from the game's .pk4 files (and loose
files), derives the three channels from what is there, and writes a sidecar DUDE loads
(docs/pbr-texture-maps.md) next to each diffuse:

  roughness  the specular map inverted: brighter specular = smoother (--rough-range), then
             re-centred on the material's current table roughness (--rough-anchor), so the
             in-game tuned look stays and the map only adds per-texel variation around it
  metalness  the specular map's grey copied as is (--metal-source spec, the default: Doom 3 is
             mostly metal and painted metal, and id painted bare steel bright, paint darker), or
             with --metal-source mask a near-binary mask: bright specular on an unsaturated
             diffuse (--metal-spec, --metal-sat), median filtered so grime speckle stays out. Forced to 0 when the material's PBR class is
             a non-metal one in pbr_materials.cfg (concrete, wood, skin, ...): a bright tile is
             not steel. Same for a material whose name holds a --nonmetal-words token (tile,
             ceramic, marble...): white glossy tile passes every other test
  (no specular map: both stay flat at the material's current table values, so the sidecar
   is never worse than the single value it replaces)
  AO         horizon-based ambient occlusion of the surface's height field, in texel units: the
             normal map integrated to heights (Frankot-Chellappa) plus the material's heightmap()
             image when it has one (as addnormals combines them), at the sources' full resolution
             (--bake-size). Every texel looks out in 16 directions for the highest horizon: a
             narrow crevice sees its walls steep and close and goes dark, a wide recess sees them
             low and far and stays light. Sampling wraps around, which is exactly a bake on the
             tile surrounded by its 3x3 neighbours, so seams match when it tiles

These are STARTING POINTS. The specular map does not hold enough information to rebuild PBR:
check every metal mask (worn paint and bright decals read as metal), then repaint and repack.
For that, each material also gets editable greyscale maps in <out>/_maps/ named for
tools/pbr_pack_orm.py, plus a preview sheet (diffuse | roughness | metalness | AO).

Examples:
  python3 tools/pbr_extract_maps.py --search base -m textures/mcity/mcityc9 -o pbr_out
  python3 tools/pbr_extract_maps.py --search base --match 'textures/mcity/*' -o pbr_out
  # after hand-editing pbr_out/_maps/textures/mcity/mcityc9_metallic.png:
  python3 tools/pbr_pack_orm.py --base textures/mcity/mcityc9 \\
      --rough pbr_out/_maps/textures/mcity/mcityc9_roughness.png \\
      --metal pbr_out/_maps/textures/mcity/mcityc9_metallic.png \\
      --ao pbr_out/_maps/textures/mcity/mcityc9_ao.png -o pbr_out

The output tree mirrors game paths (pbr_out/textures/mcity/mcityc9_orm.tga), so it can be
zipped into a .pk4 as is. --search takes game folders (base/, q4base/, a mod folder) in the
engine's order: later folders and, inside one, later .pk4 names win, and loose files win over
.pk4s in the same folder.
"""

import argparse
import fnmatch
import io
import re
import sys
import zipfile
from pathlib import Path

import numpy as np
from PIL import Image, ImageFilter

sys.path.insert(0, str(Path(__file__).resolve().parent))
from pbr_pack_orm import pack  # noqa: E402  (same channel order rules as the packer)

IMAGE_EXTS = (".tga", ".jpg", ".dds", ".png")


# ---------------------------------------------------------------------------------------------
# virtual file system: game folders, their .pk4s and loose files

class GameFS:
    def __init__(self, folders):
        self.files = {}		# lower-case path -> (zip or None, real name or Path)
        self.prio = {}		# lower-case path -> load order of the winning copy (higher = later = wins)
        self.zips = []
        order = 0
        for folder in folders:
            folder = Path(folder)
            if not folder.is_dir():
                sys.exit(f"--search {folder}: not a folder")
            for pk4 in sorted(folder.glob("*.pk4"), key=lambda p: p.name.lower()):
                z = zipfile.ZipFile(pk4)
                self.zips.append(z)
                order += 1
                for name in z.namelist():
                    if not name.endswith("/"):
                        self.files[name.lower()] = (z, name)
                        self.prio[name.lower()] = order
            order += 1
            for sub in ("materials", "textures", "models", "pbr", "gfx", "guis", "lights", "particles",
                        "skins", "generated"):
                root = folder / sub
                if root.is_dir():
                    for f in root.rglob("*"):
                        if f.is_file():
                            key = f.relative_to(folder).as_posix().lower()
                            self.files[key] = (None, f)
                            self.prio[key] = order

    def read(self, path):
        hit = self.files.get(path.lower())
        if hit is None:
            return None
        z, name = hit
        return z.read(name) if z else Path(name).read_bytes()

    def list(self, prefix, ext):
        prefix = prefix.lower()
        return sorted(p for p in self.files if p.startswith(prefix) and p.endswith(ext))


# ---------------------------------------------------------------------------------------------
# material parsing: just enough of the .mtr grammar to find the diffuse / bump / specular images

def tokenize(text):
    toks, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c.isspace():
            i += 1
        elif text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
        elif c == '"':
            j = text.find('"', i + 1)
            j = n if j < 0 else j
            toks.append(text[i + 1:j])
            i = j + 1
        elif c in "{}(),":
            toks.append(c)
            i += 1
        else:
            j = i
            while j < n and not text[j].isspace() and text[j] not in '{}(),"' and not text.startswith("//", j):
                j += 1
            toks.append(text[i:j])
            i = j
    return toks


def read_image_expr(toks, i):
    """An image expression: a path or a program like addnormals( a, heightmap( b, 4 ) ).
    Returns (tree, next index); tree = str path or (func, [args])."""
    head = toks[i]
    i += 1
    if i < len(toks) and toks[i] == "(":
        args, i = [], i + 1
        while i < len(toks) and toks[i] != ")":
            if toks[i] == ",":
                i += 1
                continue
            arg, i = read_image_expr(toks, i)
            args.append(arg)
        return (head.lower(), args), i + 1
    return head, i


STAGE_TM_OPS = ("scale", "centerscale", "translate", "scroll", "shear", "rotate")


def _read_arg(toks, j, end):
    """One stage-transform argument: a number, or an expression (parenthesised parts and binary
    operators chained). Returns (tokens, next index)."""
    out, depth = [], 0
    while j < end:
        t = toks[j]
        depth += (t == "(") - (t == ")")
        out.append(t)
        j += 1
        if depth == 0:
            if j < end and toks[j] in ("+", "-", "*", "/", "%"):
                out.append(toks[j])
                j += 1
                continue
            break
    return out, j


def _const(arg):
    return float(arg[0]) if len(arg) == 1 and _isnum(arg[0]) else None


def stage_op_matrix(op, args):
    """3x3 texture matrix of one constant stage transform, as idMaterial::ParseStage builds it;
    None when an argument is an expression (scrolling, time, parms): not bakeable."""
    vals = [_const(a) for a in args]
    if any(v is None for v in vals):
        return None
    if op == "rotate":
        c, s = np.cos(2 * np.pi * vals[0]), np.sin(2 * np.pi * vals[0])
        return np.array([[c, -s, -0.5 * c + 0.5 * s + 0.5], [s, c, -0.5 * s - 0.5 * c + 0.5], [0, 0, 1.0]])
    a, b = vals
    if op in ("translate", "scroll"):
        return np.array([[1, 0, a], [0, 1, b], [0, 0, 1.0]])
    if op == "scale":
        return np.array([[a, 0, 0], [0, b, 0], [0, 0, 1.0]])
    if op == "centerscale":
        return np.array([[a, 0, 0.5 - 0.5 * a], [0, b, 0.5 - 0.5 * b], [0, 0, 1.0]])
    return np.array([[1, a, -0.5 * a], [b, 1, -0.5 * b], [0, 0, 1.0]])	# shear


def skip_block(toks, i):
    depth = 0
    while i < len(toks):
        if toks[i] == "{":
            depth += 1
        elif toks[i] == "}":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return i


def parse_materials(text):
    """-> {name: {'diffuse': expr, 'bump': expr, 'specular': expr}} (first stage of each kind)."""
    toks, out, i = tokenize(text), {}, 0
    while i < len(toks):
        t = toks[i]
        if t.lower() in ("table", "skin", "particle", "sound", "entitydef"):
            # not materials: skip the name and its block
            j = i + 1
            while j < len(toks) and toks[j] != "{":
                j += 1
            i = skip_block(toks, j)
            continue
        if t.lower() == "material":
            i += 1
            continue
        if t in "{}(),":
            i = skip_block(toks, i) if t == "{" else i + 1
            continue
        name = t
        if i + 1 >= len(toks) or toks[i + 1] != "{":
            i += 1
            continue
        end = skip_block(toks, i + 1)
        mat, depth, stage_kind, stage_map, stage_tm, j = {}, 0, None, None, None, i + 1
        while j < end:
            tk, low = toks[j], toks[j].lower()
            if tk == "{":
                depth += 1
                if depth == 2:
                    stage_kind = stage_map = None
                    stage_tm = np.eye(3)
                j += 1
                continue
            if tk == "}":
                if depth == 2 and stage_kind and stage_map is not None and stage_kind not in mat:
                    mat[stage_kind] = stage_map
                    # the stage's texture matrix (identity = none; "animated" = not constant)
                    if stage_tm is None or not np.allclose(stage_tm, np.eye(3)):
                        mat[stage_kind + "_tm"] = stage_tm if stage_tm is not None else "animated"
                depth -= 1
                j += 1
                continue
            if (depth == 1 and low in ("deform", "guisurf", "discrete", "mirror")) \
                    or (depth == 1 and low == "sort" and j + 1 < end and toks[j + 1].lower() == "subview") \
                    or low in ("remoterendermap", "xrayrendermap"):
                mat["discrete"] = True		# idMaterial::IsDiscrete: never merged with other surfaces
            if depth == 1 and low in ("diffusemap", "bumpmap", "specularmap") and j + 1 < end:
                expr, j = read_image_expr(toks, j + 1)
                mat.setdefault({"diffusemap": "diffuse", "bumpmap": "bump", "specularmap": "specular"}[low], expr)
                continue
            if depth == 2 and low == "blend" and j + 1 < end:
                kind = toks[j + 1].lower()
                stage_kind = {"diffusemap": "diffuse", "bumpmap": "bump", "specularmap": "specular"}.get(kind)
                j += 2
                continue
            if depth == 2 and low in STAGE_TM_OPS:
                nargs = 1 if low == "rotate" else 2
                args, k = [], j + 1
                for n in range(nargs):
                    arg, k = _read_arg(toks, k, end)
                    args.append(arg)
                    if n + 1 < nargs and k < end and toks[k] == ",":
                        k += 1
                m = stage_op_matrix(low, args)
                if stage_tm is not None:
                    stage_tm = None if m is None else stage_tm @ m	# MultiplyTextureMatrix: old * new
                j = k
                continue
            if depth == 2 and low == "map" and j + 1 < end:
                stage_map, j = read_image_expr(toks, j + 1)
                continue
            j += 1
        out.setdefault(name.lower(), dict(mat, name=name))
        i = end
    return out


GUIDE_DEF = re.compile(r"(?:^|\n)\s*(?:guide|inlineGuide)\s+(\w+)\s*\(([^)]*)\)\s*\{", re.IGNORECASE)
GUIDE_USE = re.compile(r"^[ \t]*guide[ \t]+(\S+)[ \t]+(\w+)[ \t]*\(([^)\n]*)\)", re.IGNORECASE | re.MULTILINE)


def _clean_arg(a):
    return a.strip().strip('"').strip()


def parse_guides(text):
    """Quake 4 .guide templates -> {name: (parms, body)} (neo/framework/DeclGuides.cpp)."""
    out = {}
    for m in GUIDE_DEF.finditer(text):
        depth, i = 1, m.end()
        while i < len(text) and depth:
            depth += {"{": 1, "}": -1}.get(text[i], 0)
            i += 1
        parms = [_clean_arg(a) for a in m.group(2).split(",")]
        out[m.group(1).lower()] = ([a for a in parms if a], text[m.end():i - 1])
    return out


def expand_guides(text, guides):
    """Replace each `guide <material> <template>( args )` line with the material it defines: the
    template body with every parameter name substituted as a substring, longest name first, as
    the engine does."""
    def sub(m):
        tpl = guides.get(m.group(2).lower())
        if tpl is None:
            return m.group(0)
        parms, body = tpl
        args = [_clean_arg(a) for a in m.group(3).split(",")]
        if len(args) != len(parms):
            return m.group(0)
        order = sorted(range(len(parms)), key=lambda k: -len(parms[k]))
        out, i = [], 0
        while i < len(body):
            for k in order:
                if body.startswith(parms[k], i):
                    out.append(args[k])
                    i += len(parms[k])
                    break
            else:
                out.append(body[i])
                i += 1
        return f"{m.group(1)}\n{{{''.join(out)}}}\n"
    return GUIDE_USE.sub(sub, text)


# ---------------------------------------------------------------------------------------------
# --map: every material a map draws (world surfaces, props, entity models, skins)

PROC_SURFACE = re.compile(r'/\* surface \d+ \*/\s*\{\s*"([^"]+)"')
MAP_KV = re.compile(r'^\s*"([^"]+)"\s+"([^"]*)"', re.MULTILINE)


def parse_decl_blocks(text, kind):
    """`kind NAME { ... }` blocks (entityDef / model / skin) -> {name: token list of the body}."""
    toks, out, i = tokenize(text), {}, 0
    while i < len(toks) - 2:
        if toks[i].lower() == kind and toks[i + 2] == "{":
            end = skip_block(toks, i + 2)
            out[toks[i + 1].lower()] = toks[i + 3:end - 1]
            i = end
        else:
            i += 1
    return out


def model_file_materials(fs, path):
    data = fs.read(path)
    if data is None:
        return set()
    low = path.lower()
    names = set()
    if low.endswith(".md5mesh"):
        names.update(re.findall(r'shader\s+"([^"]+)"', data.decode("latin-1")))
    elif low.endswith(".ase"):
        text = data.decode("latin-1")
        for bmp in re.findall(r'\*BITMAP\s+"([^"]+)"', text):	# the engine names ASE surfaces by bitmap
            b = bmp.replace("\\", "/")
            k = b.lower().find("base/")
            names.add(strip_ext(b[k + 5:] if k >= 0 else b))
        names.update(re.findall(r'\*MATERIAL_NAME\s+"([^"]+)"', text))
    elif low.endswith(".lwo") and data[:4] == b"FORM":
        i = 12		# LWO2: the TAGS chunk lists the surface (= material) names
        while i + 8 <= len(data):
            cid, size = data[i:i + 4], int.from_bytes(data[i + 4:i + 8], "big")
            if cid == b"TAGS":
                names.update(t.decode("latin-1") for t in data[i + 8:i + 8 + size].split(b"\0") if t)
            i += 8 + size + (size & 1)
    return names


# ---------------------------------------------------------------------------------------------
# md5 models: their materials, and the AO the engine's own baker made for them (bakeAO)

MD5_SHADER = re.compile(r'shader\s+"([^"]+)"')


def load_skins(fs):
    """skin decls -> {name: (model paths it is limited to, [(from, to), ...])}."""
    skins = {}
    for f in fs.list("skins/", ".skin"):
        for name, body in parse_decl_blocks(fs.read(f).decode("latin-1"), "skin").items():
            models, pairs = [], []
            for k in range(0, len(body) - 1, 2):
                if body[k].lower() == "model":
                    models.append(body[k + 1].lower())
                else:
                    pairs.append((body[k].lower(), body[k + 1].lower()))
            skins[name] = (models, pairs)
    return skins


def canonical_decl_name(name):
    """idDecl names as declManager->FindMaterial sees them: lower case, forward slashes, extension
    dropped (so an LWO's "cbrick3" and "cbrick3.tga" are one material, one surface)."""
    n = name.replace("\\", "/").lower()
    dot, slash = n.rfind("."), n.rfind("/")
    return n[:dot] if dot > slash else n


def image_size(fs, path):
    """Longer side of the image a texture name resolves to, from its header (no decode)."""
    stem = strip_ext(path)
    for ext in IMAGE_EXTS:
        data = fs.read(stem + ext)
        if data is None:
            continue
        if ext == ".tga" and len(data) >= 18:
            return max(int.from_bytes(data[12:14], "little"), int.from_bytes(data[14:16], "little"))
        try:
            return max(Image.open(io.BytesIO(data)).size)
        except Exception:  # noqa: BLE001
            return None
    return None


def baker_size_fits(fs, mat, ao_size):
    """Does a baked map of `ao_size` fit this material? The baker (auto size) makes each map as
    large as the surface's diffuse (else bump) image, power-of-two rounded by the engine, or 256
    without one. None = cannot tell."""
    if mat is None:
        return None
    path = plain_path(mat.get("diffuse"))
    if not path:
        n, h, _ = bump_sources(mat.get("bump"))
        path = n or h
    size = image_size(fs, path) if path else None
    if not size:
        return None
    lo = 1 << (size.bit_length() - 1)
    fits = {size, lo, lo * 2 if lo != size else size}
    return min(max(ao_size, 16), 4096) in {min(max(f, 16), 4096) for f in fits}


def _ase_bitmap_material(bmp):
    b = bmp.replace("\\", "/")
    k = b.lower().find("base/")		# fileSystem->OSPathToRelativePath, as far as stock files need it
    return strip_ext(b[k + 5:] if k >= 0 else b).lower()


def _ase_surface_names(text):
    """Material name per *GEOMOBJECT, in file order (ASE_Parse / ConvertASEToModelSurfaces): the
    *BITMAP of the top-level *MATERIAL its *MATERIAL_REF points at (submaterials are ignored)."""
    toks = re.findall(r'"[^"]*"|[{}]|\S+', text)
    mats, objs, i, depth, stack = [], [], 0, 0, []
    while i < len(toks):
        t = toks[i]
        if t == "{":
            depth += 1
        elif t == "}":
            depth -= 1
            while stack and stack[-1][1] > depth:
                stack.pop()
        elif t == "*MATERIAL" and stack and stack[-1][0] == "list" and depth == stack[-1][1]:
            mats.append(None)
            stack.append(("mat", depth + 1))
        elif t == "*MATERIAL_LIST":
            stack.append(("list", depth + 1))
        elif t == "*BITMAP" and stack and stack[-1][0] == "mat" and depth == stack[-1][1] + 1 and mats[-1] is None:
            mats[-1] = _ase_bitmap_material(toks[i + 1].strip('"'))	# inside *MAP_DIFFUSE
        elif t == "*GEOMOBJECT":
            objs.append(0)
            stack.append(("obj", depth + 1))
        elif t == "*MATERIAL_REF" and stack and stack[-1][0] == "obj" and depth == stack[-1][1]:
            objs[-1] = int(toks[i + 1])
        i += 1
    return [mats[r] if 0 <= r < len(mats) and mats[r] else "_default" for r in objs] if mats else ["_default"] * bool(objs)


def _lwo_surface_names(data):
    """LWO2 surface names in lwo->surf order: the SURF chunks as stored, then a default surface per
    polygon tag that has none."""
    tags, surfs, i = [], [], 12
    while i + 8 <= len(data):
        cid, size = data[i:i + 4], int.from_bytes(data[i + 4:i + 8], "big")
        body = data[i + 8:i + 8 + size]
        if cid == b"TAGS":
            tags = [t.decode("latin-1") for t in body.split(b"\0") if t]
        elif cid == b"SURF":
            surfs.append(body.split(b"\0", 1)[0].decode("latin-1"))
        i += 8 + size + (size & 1)
    low = {n.lower() for n in surfs}
    return surfs + [t for t in tags if t.lower() not in low]


def static_surface_materials(names, materials):
    """The surfaces idRenderModelStatic ends up with for these per-object (ASE) or per-surface (LWO)
    material names, in order: with r_mergeModelSurfaces (default 1) a name already seen merges into
    that surface unless its material is discrete. Index = the AO baker's _s<N>. Mirrored back sides
    (twoSided bump) are appended after these, with the same UVs, so they are not needed."""
    out = []
    for n in names:
        n = canonical_decl_name(n)
        if n in out and not materials.get(n, {}).get("discrete"):
            continue
        out.append(n)
    return out


def model_surface_uses(fs, skins, materials, pattern=None, kinds=(".md5mesh", ".lwo", ".ase")):
    """{material: [(model, surface index), ...]} for every model file of these kinds (or those
    matching `pattern`): each surface's own material, plus what any skin turns it into (a skin
    limited to other models by its `model` lines is left out). md5: the mesh index; LWO / ASE: the
    position among the loaded, merged surfaces. These indices are what the AO baker names maps by."""
    out = {}
    for kind in kinds:
        for path in fs.list("models/", kind):
            if pattern and not fnmatch.fnmatch(path.lower(), pattern.lower()):
                continue
            data = fs.read(path)
            if kind == ".md5mesh":
                names = [n.lower() for n in MD5_SHADER.findall(data.decode("latin-1"))]
            elif kind == ".ase":
                names = static_surface_materials(_ase_surface_names(data.decode("latin-1")), materials)
            elif data[:4] == b"FORM" and data[8:12] == b"LWO2":
                names = static_surface_materials(_lwo_surface_names(data), materials)
            else:
                continue
            for i, shader in enumerate(names):
                out.setdefault(canonical_decl_name(shader), []).append((path, i))
    base = {k: list(v) for k, v in out.items()}		# skins remap the meshes' own shaders, not each other
    for models, pairs in skins.values():
        for src, dst in pairs:
            for use in base.get(src, []):
                if (not models or use[0].lower() in models) and use not in out.setdefault(dst, []):
                    out[dst].append(use)
    return out


def baked_ao_maps(fs, uses, rejected=frozenset()):
    """The AO baker's maps (generated/aomaps/<model sans ext>_s<surface>.tga) that exist for these
    (model, surface index) uses, minus models whose surface order did not check out."""
    found = []
    for path, i in uses:
        if path in rejected:
            continue
        p = f"generated/aomaps/{path.rsplit('.', 1)[0]}_s{i}.tga"
        if p.lower() in fs.prio and p not in found:
            found.append(p)
    return found


def coverage_conflict(fs, maps):
    """0 = these maps (one texture, several meshes) cover the same UV islands (variants of one mesh)
    or separate ones (parts of one atlas): safe to combine. Towards 1 = their islands overlap only
    in part, i.e. the texture is laid out differently on each mesh (a tiling texture wrapped over
    many props), and no combination of their AO fits all of them."""
    cov = [np.asarray(Image.open(io.BytesIO(fs.read(p))).convert("L").resize((128, 128))) < 253 for p in maps]
    worst = 0.0
    for i in range(len(cov)):
        for j in range(i + 1, len(cov)):
            inter = (cov[i] & cov[j]).sum() / max(1, min(cov[i].sum(), cov[j].sum()))
            worst = max(worst, 2.0 * min(inter, 1.0 - inter))
    return worst


def check_static_order(fs, uses_by_mat, materials):
    """LWO / ASE surface order is reproduced from the loader's rules, so check it against the baked
    maps before trusting it: every map whose predicted surface has a known texture must have that
    texture's size. Models that fail are returned (their baked AO is not used)."""
    by_model = {}
    for mname, uses in uses_by_mat.items():
        for path, i in uses:
            if not path.lower().endswith(".md5mesh"):
                by_model.setdefault(path, {})[i] = mname
    rejected = set()
    for path, surfs in by_model.items():
        for i, mname in surfs.items():
            p = f"generated/aomaps/{path.rsplit('.', 1)[0]}_s{i}.tga"
            data = fs.read(p) if p.lower() in fs.prio else None
            if data is None or len(data) < 18:
                continue
            ao_size = max(int.from_bytes(data[12:14], "little"), int.from_bytes(data[14:16], "little"))
            if baker_size_fits(fs, materials.get(mname), ao_size) is False:
                rejected.add(path)
                break
    return rejected


def collect_map_materials(fs, mapname):
    """-> set of lower-case material names maps/<mapname> draws, as far as files can tell."""
    stem = mapname[:-4] if mapname.lower().endswith((".map", ".proc")) else mapname
    if not stem.lower().startswith("maps/"):
        stem = "maps/" + stem
    names = set()
    proc = fs.read(stem + ".proc")
    if proc is not None:
        names.update(PROC_SURFACE.findall(proc.decode("latin-1")))
    mp = fs.read(stem + ".map")
    if mp is None and proc is None:
        sys.exit(f"--map {mapname}: neither {stem}.map nor {stem}.proc found")
    defs, modeldefs, skins = {}, {}, {}
    for f in fs.list("def/", ".def"):
        text = fs.read(f).decode("latin-1")
        for name, body in parse_decl_blocks(text, "entitydef").items():
            defs[name] = {body[k].lower(): body[k + 1] for k in range(0, len(body) - 1, 2)}
        for name, body in parse_decl_blocks(text, "model").items():
            mesh = next((body[k + 1] for k in range(len(body) - 1) if body[k].lower() == "mesh"), None)
            parent = next((body[k + 1] for k in range(len(body) - 1) if body[k].lower() == "inherit"), None)
            modeldefs[name] = (mesh, parent)
    for f in fs.list("skins/", ".skin"):
        for name, body in parse_decl_blocks(fs.read(f).decode("latin-1"), "skin").items():
            skins[name] = [body[k + 1] for k in range(0, len(body) - 1, 2) if body[k].lower() != "model"]

    def def_key(name, key, depth=0):
        d = defs.get(name.lower())
        if d is None or depth > 16:
            return None
        return d.get(key) or (def_key(d["inherit"], key, depth + 1) if "inherit" in d else None)

    def add_model(model):
        if not model:
            return
        low = model.lower()
        if low.endswith((".lwo", ".ase", ".md5mesh")):
            names.update(model_file_materials(fs, model))
        else:
            for _ in range(16):		# model decls inherit their mesh
                mesh, parent = modeldefs.get(low, (None, None))
                if mesh:
                    names.update(model_file_materials(fs, mesh))
                    break
                if not parent:
                    break
                low = parent.lower()

    def add_skin(skin):
        names.update(skins.get((skin or "").lower(), []))

    seen_defs = set()

    def add_def(classname):
        if not classname or classname.lower() in seen_defs:
            return
        seen_defs.add(classname.lower())
        if classname.lower() not in defs:
            add_model(classname)	# def_head and friends may name a model decl directly
            return
        for key in ("model", "model_view", "model_world", "model_ragdoll"):
            add_model(def_key(classname, key))
        add_skin(def_key(classname, "skin"))
        for key in ("def_head", "def_attach", "def_weapon", "def_dropDeathItem"):
            add_def(def_key(classname, key.lower()))

    if mp is not None:
        for block in mp.decode("latin-1").split("// entity ")[1:]:
            kv = {k.lower(): v for k, v in MAP_KV.findall(block)}
            add_model(kv.get("model"))
            add_skin(kv.get("skin"))
            add_def(kv.get("classname"))
    return {n.lower() for n in names}


def plain_path(expr):
    return expr if isinstance(expr, str) else None


def bump_sources(expr):
    """(normal map path or None, height map path or None, height scale) from a bump expression."""
    if expr is None:
        return None, None, 0.0
    if isinstance(expr, str):
        return expr, None, 0.0
    func, args = expr
    if func == "heightmap" and args:
        scale = float(args[1]) if len(args) > 1 and isinstance(args[1], str) and _isnum(args[1]) else 1.0
        return None, plain_path(args[0]), scale
    if func == "addnormals":
        normal = height = None
        scale = 0.0
        for a in args:
            n, h, s = bump_sources(a)
            normal = normal or n
            if h and not height:
                height, scale = h, s
        return normal, height, scale
    for a in args:	# other programs (scale, invertcolor...): take what is inside
        n, h, s = bump_sources(a)
        if n or h:
            return n, h, s
    return None, None, 0.0


def _isnum(s):
    try:
        float(s)
        return True
    except ValueError:
        return False


# ---------------------------------------------------------------------------------------------
# images

def strip_ext(path):
    p = path.replace("\\", "/")
    for ext in IMAGE_EXTS:
        if p.lower().endswith(ext):
            return p[: -len(ext)]
    return p


def load_image(fs, path, mode):
    """The image a texture name resolves to, among .tga / .jpg / .dds / .png: the copy from the
    highest-priority source (a later --search folder or .pk4), so an HD pack's .jpg replaces a stock
    .tga. (The engine itself tries .tga before .jpg; this deliberately takes the most detailed.)"""
    stem = strip_ext(path)
    found = [stem + ext for ext in IMAGE_EXTS if (stem + ext).lower() in fs.prio]
    found.sort(key=lambda f: -fs.prio[f.lower()])
    for cand in found[:1]:
        data = fs.read(cand)
        if data is not None:
            ext = cand[len(stem):]
            try:
                return Image.open(io.BytesIO(data)).convert(mode), stem + ext
            except Exception as e:  # noqa: BLE001  (corrupt / unsupported: report and move on)
                print(f"  cannot decode {stem + ext}: {e}", file=sys.stderr)
                return None, None
    return None, None


HD_ORIENTATIONS = (
    ("as is", lambda im: im),
    ("flipped vertically", lambda im: im.transpose(Image.FLIP_TOP_BOTTOM)),
    ("flipped horizontally", lambda im: im.transpose(Image.FLIP_LEFT_RIGHT)),
    ("rotated 180", lambda im: im.transpose(Image.ROTATE_180)),
)


def _detail(img, size):
    """Zero-mean detail of a bump image at `size`: a normal map's xy tilt, a height map's
    high-passed grey. Used only to compare two versions of the same map."""
    a = as_float(img, size)
    if a.ndim == 3:
        a = (a[..., :2] - 0.5).reshape(-1)
    else:
        a = (a - as_float(img.filter(ImageFilter.BoxBlur(4)), size)).reshape(-1)
    return a - a.mean()


def load_bump(fs, fs_hd, path, mode, report):
    """A normal or height map for the AO bake: the --hd copy when it lines up with the stock one.
    HD packs are not always faithful (D3HDP's kitchenchair_local.tga is stored upside down: rows
    top-down without the TGA top-origin flag), and AO from a misaligned map lands beside the
    relief the game draws. So the HD map is compared with the stock one, used as is or
    re-oriented if that is what matches, and dropped otherwise."""
    stock, stock_name = load_image(fs, path, mode)
    if fs_hd is fs:
        return stock
    hd, hd_name = load_image(fs_hd, path, mode)
    if hd is None or stock is None or hd_name.lower() == stock_name.lower() and hd.size == stock.size:
        return hd or stock
    size = stock.size
    ref = _detail(stock, size)
    ref_n = np.linalg.norm(ref)
    if ref_n < 1e-3 * np.sqrt(ref.size):	# flat stock map: nothing to line up with
        return hd
    scores = []
    for label, fn in HD_ORIENTATIONS:
        d = _detail(fn(hd), size)
        scores.append((float(d @ ref / (np.linalg.norm(d) * ref_n + 1e-12)), label, fn))
    best = max(scores, key=lambda t: t[0])
    if best[0] < 0.5:
        report.setdefault("HD map does not match the stock one: stock used", []).append(f"{path} (best {best[1]}, {best[0]:.2f})")
        return stock
    if best[1] != "as is":
        report.setdefault(f"HD map {best[1]} to match the stock one", []).append(f"{path} ({best[0]:.2f})")
    return best[2](hd)


def as_float(img, size=None):
    if size is not None and img.size != size:
        img = img.resize(size, Image.BICUBIC)
    return np.asarray(img).astype(np.float64) / 255.0


# ---------------------------------------------------------------------------------------------
# channel derivation

def smoothstep(a, b, x):
    t = np.clip((x - a) / (b - a), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def fft_freqs(shape):
    h, w = shape
    return np.fft.fftfreq(w)[None, :], np.fft.fftfreq(h)[:, None]


def periodic_blur(a, sigma):
    u, v = fft_freqs(a.shape)
    return np.real(np.fft.ifft2(np.fft.fft2(a) * np.exp(-2.0 * (np.pi * sigma) ** 2 * (u * u + v * v))))


def height_from_normals(nrm, green_sign):
    """Frankot-Chellappa: the periodic height field whose gradient best matches the normal map."""
    nz = np.clip(nrm[..., 2], 0.2, 1.0)
    p = -nrm[..., 0] / nz
    q = -green_sign * nrm[..., 1] / nz
    u, v = fft_freqs(p.shape)
    den = (2.0 * np.pi) ** 2 * (u * u + v * v)
    den[0, 0] = 1.0
    z = (-1j * 2.0 * np.pi * u * np.fft.fft2(p) - 1j * 2.0 * np.pi * v * np.fft.fft2(q)) / den
    z[0, 0] = 0.0
    h = np.real(np.fft.ifft2(z))
    return h


def stage_warp(a, src_tm, diff_tm, size):
    """Resample `a`, an image of a stage with texture matrix src_tm, into the texture space of the
    diffuse stage (diff_tm) at `size`, wrapping: the sidecar is read with the diffuse's texture
    coordinates, so a bump map scaled 2, 2 (sofloor1) must repeat twice across it."""
    w, h = size
    m = np.asarray(src_tm, float) @ np.linalg.inv(np.asarray(diff_tm, float))
    v, u = np.meshgrid((np.arange(h) + 0.5) / h, (np.arange(w) + 0.5) / w, indexing="ij")
    su = m[0, 0] * u + m[0, 1] * v + m[0, 2]
    sv = m[1, 0] * u + m[1, 1] * v + m[1, 2]
    sh, sw = a.shape[:2]
    x, y = su * sw - 0.5, sv * sh - 0.5
    x0, y0 = np.floor(x).astype(int), np.floor(y).astype(int)
    fx, fy = x - x0, y - y0
    if a.ndim == 3:
        fx, fy = fx[..., None], fy[..., None]
    g = lambda yy, xx: a[yy % sh, xx % sw]  # noqa: E731
    top = g(y0, x0) * (1 - fx) + g(y0, x0 + 1) * fx
    bot = g(y0 + 1, x0) * (1 - fx) + g(y0 + 1, x0 + 1) * fx
    return top * (1 - fy) + bot * fy


def resample(a, size):
    """float array (H, W) or (H, W, C) -> size (w, h), bilinear."""
    if (a.shape[1], a.shape[0]) == tuple(size):
        return a
    if a.ndim == 2:
        return np.asarray(Image.fromarray(a.astype(np.float32), "F").resize(size, Image.BILINEAR), dtype=np.float64)
    return np.stack([resample(a[..., c], size) for c in range(a.shape[2])], axis=-1)


def surface_height(normal, height, height_scale, size, green_sign):
    """The surface as heights in texel units on a grid of `size`: the normal map's slopes integrated,
    plus a heightmap() image at the engine's scale (R_HeightmapToNormalMap: slope = byte step * scale
    / 256, so height = byte * scale / 256 source texels). None without either source."""
    h = None
    if normal is not None:
        n = resample(normal, size) * 2.0 - 1.0
        h = height_from_normals(n, green_sign)
    if height is not None:
        src_w = height.shape[1]
        hh = resample(height, size) * 255.0 * height_scale / 256.0 * (size[0] / src_w)
        h = hh if h is None else h + hh
    return h


def horizon_ao(h, radius, bias=0.1, directions=16):
    """Horizon-based AO of a periodic height field (texel units): per direction, the steepest
    elevation to any sample within `radius`, attenuated with distance so far walls count less;
    visibility = 1 - sin(horizon), averaged. np.roll wraps = the tile among its 3x3 neighbours.
    `bias` (tan of an angle) is subtracted first, as HBAO does, so texel-scale noise in the
    integrated heights reads as flat instead of a dotted grid."""
    steps = sorted({max(1, int(round(r))) for r in np.geomspace(1.0, max(radius, 1.0), 12)})
    vis = np.zeros_like(h)
    for d in range(directions):
        ang = 2.0 * np.pi * (d + 0.5) / directions
        cx, cy = np.cos(ang), np.sin(ang)
        best = np.zeros_like(h)		# tan of the horizon elevation
        for r in steps:
            dx, dy = int(round(cx * r)), int(round(cy * r))
            if dx == 0 and dy == 0:
                continue
            dist = float(np.hypot(dx, dy))
            if dist > radius * 1.0001:
                continue
            rise = np.roll(h, (-dy, -dx), axis=(0, 1)) - h
            falloff = 1.0 - (dist / radius) ** 2
            np.maximum(best, rise / dist * falloff, out=best)
        best = np.maximum(best - bias, 0.0)
        vis += 1.0 - best / np.sqrt(1.0 + best * best)		# 1 - sin(atan(t))
    return vis / directions


def derive(diffuse, spec, args, table, name_says_nonmetal=False):
    """table = the material's current (metalness, roughness) from the PBR table, None = unknown."""
    metal_allowed = ( table is None or table[0] > 0.0 ) and not name_says_nonmetal
    size = diffuse.shape[1], diffuse.shape[0]
    rough_lo, rough_hi = args.rough_range
    spec_raw = spec
    if spec is not None:
        spec = np.clip(spec * args.spec_gain, 0.0, 1.0)
    if spec is not None:
        spec_peak = max(np.percentile(spec, 99.5), 0.05) if args.spec_normalize else args.spec_white
        s = np.clip(spec / spec_peak, 0.0, 1.0)
        rough = rough_hi - (rough_hi - rough_lo) * smoothstep(0.0, 1.0, s)
        if table is not None and args.rough_anchor == "table":
            # keep the tuned average: the specular map supplies variation, the table the level
            rough = table[1] + (rough - rough.mean()) * args.rough_contrast
    else:
        rough = np.full(diffuse.shape[:2], table[1] if table else args.default_rough)

    metal = np.full(diffuse.shape[:2], table[0] if (table and spec is None) else 0.0)
    if spec is not None and metal_allowed and args.metal_source == "spec":
        # the specular map's grey copied as is: id painted bare steel bright and paint darker, and in a
        # game that is mostly metal and painted metal that grey works as a continuous metalness
        metal = spec_raw.copy()
    elif spec is not None and metal_allowed:
        spec_u8 = Image.fromarray(np.clip(spec * 255.0 + 0.5, 0, 255).astype(np.uint8))
        spec_med = as_float(spec_u8.filter(ImageFilter.MedianFilter(args.metal_median)))
        sat = diffuse.max(axis=2) - diffuse.min(axis=2)
        metal = smoothstep(args.metal_spec[0], args.metal_spec[1], spec_med) \
            * (1.0 - smoothstep(args.metal_sat[0], args.metal_sat[1], sat))

    return np.clip(rough, 0.03, 1), np.clip(metal, 0, args.metal_cap)


# ---------------------------------------------------------------------------------------------
# PBR table: which materials may be metal at all

def load_pbr_table(table_texts, default_rough):
    """-> f(material) = (metalness, roughness) as the engine resolves them from pbr_materials.cfg +
    pbr_overrides.cfg (later text wins), or None when the material has no row."""
    cat, rows = {}, {}
    for text in table_texts:
        for line in text.splitlines():
            line = line.split("#", 1)[0].split()
            if not line:
                continue
            if line[0] == "@cat" and len(line) >= 4 and _isnum(line[2]) and _isnum(line[3]):
                cat[line[1].lower()] = (float(line[2]), float(line[3]))
            elif len(line) >= 3:
                rows[line[0].lower()] = (line[1], line[2], line[3].lower() if len(line) > 3 else "none")

    def resolve(material):
        row = rows.get(material.lower())
        if row is None:
            return None
        metal, rough, category = row
        if category in cat:		# a class row tracks its class preset (the engine ignores the numbers)
            return cat[category]
        return (float(metal) if _isnum(metal) else 0.0, float(rough) if _isnum(rough) else default_rough)

    def category(material):
        row = rows.get(material.lower())
        return row[2] if row else None
    resolve.category = category
    return resolve


# ---------------------------------------------------------------------------------------------

def preview(diffuse, rough, metal, ao, path):
    h, w = rough.shape
    tiles = [Image.fromarray((np.clip(diffuse, 0, 1) * 255).astype(np.uint8), "RGB")]
    tiles += [Image.fromarray((c * 255 + 0.5).astype(np.uint8), "L").convert("RGB") for c in (rough, metal, ao)]
    sheet = Image.new("RGB", (w * 4 + 30, h), (40, 40, 40))
    for i, t in enumerate(tiles):
        sheet.paste(t, (i * (w + 10), 0))
    sheet.save(path)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--search", action="append", type=Path,
                    help="game folder with .pk4s / loose files, repeatable, later wins (default: ./base)")
    ap.add_argument("--hd", action="append", default=[], type=Path,
                    help="folder with a higher-resolution texture pack, used ONLY for the normal and height maps the AO "
                         "is baked from. Only for a pack the players install too: AO from relief the game does "
                         "not draw lands beside the real one. Each HD map is checked against the stock one "
                         "(re-oriented if mirrored, dropped if redrawn). Roughness and metalness stay stock")
    ap.add_argument("-m", "--material", action="append", default=[], help="material name, repeatable")
    ap.add_argument("--match", action="append", default=[], help="material name glob, e.g. 'textures/mcity/*'")
    ap.add_argument("--md5", action="append", default=[], metavar="GLOB",
                    help="every material the matching .md5mesh files use (skins included), e.g. "
                         "'models/md5/monsters/*' or 'models/md5/*'")
    ap.add_argument("--no-baked-ao", action="store_true",
                    help="ignore the engine AO baker's maps (generated/aomaps/, bakeAO) for models")
    ap.add_argument("--baked-ao-conflict", type=float, default=0.3,
                    help="how far the UV islands of several models sharing a texture may disagree before "
                         "their baked AO is not used (0 = identical or separate islands, 1 = half overlap; "
                         "default 0.3)")
    ap.add_argument("--map", action="append", default=[],
                    help="every material a map draws, e.g. game/mars_city1: world surfaces (.proc), prop "
                         "models, entityDef models (md5 meshes, heads) and skins")
    ap.add_argument("-o", "--output", type=Path, default=Path("pbr_out"), help="output folder (mirrors game paths)")
    ap.add_argument("--layout", choices=("orm", "rmao"), default="orm", help="sidecar channel order (default orm)")
    ap.add_argument("--pbr-table", action="append", type=Path,
                    help="pbr_materials.cfg / pbr_overrides.cfg to read metal classes from "
                         "(default: pbr/ in the --search folders; 'none' to skip)")
    ap.add_argument("--rough-range", type=float, nargs=2, default=(0.3, 0.85), metavar=("SHINY", "DULL"),
                    help="roughness at full and at zero specular (default 0.3 0.85)")
    ap.add_argument("--rough-anchor", choices=("table", "none"), default="table",
                    help="table (default): shift each roughness map so its average is the material's current "
                         "PBR table roughness (the look tuned in game); none: absolute values from --rough-range")
    ap.add_argument("--rough-contrast", type=float, default=1.0,
                    help="scale of the per-texel roughness variation around the anchor (default 1)")
    ap.add_argument("--spec-white", type=float, default=0.62,
                    help="specular value that counts as fully shiny (default 0.62, Doom 3's maps rarely exceed it)")
    ap.add_argument("--spec-gain", default="auto",
                    help="multiply specular maps before deriving, so one set of thresholds fits both games. "
                         "auto = 1.0 for Doom 3 data, 1.47 for Quake 4 data (detected by its guide templates): "
                         "Quake 4's brightest specular maps run darker (90th percentile of the per-texture 95th "
                         "percentile 0.384 vs 0.565 over every textures/ material, measured 2026-11-12)")
    ap.add_argument("--spec-normalize", action="store_true",
                    help="per texture, treat its brightest specular (99.5th percentile) as fully shiny instead")
    ap.add_argument("--default-rough", type=float, default=0.6,
                    help="roughness without a specular map, for a material the PBR table does not list")
    ap.add_argument("--metal-source", choices=("spec", "mask"), default="spec",
                    help="spec (default): metalness = the specular map's grey, unchanged; mask: a near-binary "
                         "metal mask from bright specular on a colourless diffuse")
    ap.add_argument("--class-shine", action="append", metavar="CLASS=FACTOR",
                    default=None,
                    help="divide the roughness of a PBR table class by FACTOR, after everything else "
                         "(default ceramic=2.4, picked in game on mars_city1: glazed tile reads too dull at "
                         "its class roughness once the specular map adds its variation). Repeat for more "
                         "classes; 'ceramic=1' turns the default off")
    ap.add_argument("--metal-cap", type=float, default=0.2,
                    help="highest metalness written (default 0.2, picked in game on mars_city1: raw "
                         "specular grey reads far too metallic above it; 1 = no cap)")
    ap.add_argument("--metal-spec", type=float, nargs=2, default=(0.28, 0.40), metavar=("LO", "HI"),
                    help="specular ramp that turns metal on (default 0.28 0.40)")
    ap.add_argument("--metal-sat", type=float, nargs=2, default=(0.08, 0.20), metavar=("LO", "HI"),
                    help="diffuse saturation ramp that turns metal back off: coloured paint is not metal")
    ap.add_argument("--metal-median", type=int, default=5, help="median filter size against grime speckle (odd)")
    ap.add_argument("--nonmetal-words", default="tile,ceramic,porcelain,marble,granite,concrete,plaster,"
                    "stone,brick,wood,glass,carpet,cloth,fabric,paper,rubber,flesh,skin,wire,cable,hose",
                    help="comma list: a material whose last path part contains one never gets metal")
    ap.add_argument("--no-ao", action="append", default=[], metavar="GLOB",
                    help="white AO for materials matching GLOB, repeatable. A sidecar's AO replaces every other "
                         "AO source, so use it where better AO already exists, e.g. 'models/characters/*' "
                         "(the baked character AO)")
    ap.add_argument("--ao-strength", type=float, default=1.0, help="scale of the occlusion (0 = white AO, 1 = as baked)")
    ap.add_argument("--ao-radius", type=float, default=16.0,
                    help="how far a texel looks for occluders, in texels of a 256-wide texture (default 16 = 1/16 "
                         "of the width): walls further away than this never darken it")
    ap.add_argument("--ao-bias", type=float, default=0.1,
                    help="ignore occluders below this slope (tan of the angle; 0.1 = about 6 degrees) so "
                         "texel-scale noise in the heights does not speckle flat areas")
    ap.add_argument("--ao-highpass", type=float, default=1.5,
                    help="drop height variation broader than this many AO radii before baking (0 = keep all): "
                         "removes the large-scale drift that greys out whole model atlases")
    ap.add_argument("--ao-depth", type=float, default=1.0,
                    help="multiply the surface heights the normal map implies (more = deeper crevices)")
    ap.add_argument("--bake-size", type=int, default=1024,
                    help="AO bake resolution cap: the normal / height map is used up to this width (default 1024)")
    ap.add_argument("--max-size", type=int, default=512,
                    help="sidecar size cap, longer side (default 512). Sidecars load uncompressed: "
                         "512x512 = 1 MB of video memory each")
    ap.add_argument("--green-sign", type=float, default=1.0,
                    help="normal map green convention for the height integration (+1 = Doom 3 / Quake 4: the "
                         "engine's own heightmap() convention, which integrates consistently on 115 of 120 "
                         "stock normal maps; -1 for the other convention)")
    ap.add_argument("--no-maps", action="store_true", help="skip the editable per-channel maps and previews")
    ap.add_argument("-v", "--verbose", action="store_true", help="list every skipped material")
    args = ap.parse_args()
    if args.class_shine is None:
        args.class_shine = ["ceramic=2.4"]

    folders = args.search or [Path("base")]
    nonmetal_words = [w.strip().lower() for w in args.nonmetal_words.split(",") if w.strip()]
    fs = GameFS(folders)
    class_shine = {}
    for item in args.class_shine:
        k, _, v = item.partition("=")
        if not _isnum(v) or float(v) <= 0:
            ap.error(f"--class-shine wants CLASS=FACTOR, got {item!r}")
        class_shine[k.strip().lower()] = float(v)
    fs_bump = GameFS(folders + args.hd) if args.hd else fs	# --hd: higher-detail normal / height maps

    guides = {}
    for g in fs.list("guides/", ".guide"):		# Quake 4 material templates
        guides.update(parse_guides(fs.read(g).decode("latin-1")))
    if args.spec_gain == "auto":
        args.spec_gain = 1.47 if guides else 1.0
    else:
        args.spec_gain = float(args.spec_gain)
    materials = {}
    for mtr in fs.list("materials/", ".mtr"):
        materials.update(parse_materials(expand_guides(fs.read(mtr).decode("latin-1"), guides)))
    print(f"{len(materials)} materials in {len(fs.list('materials/', '.mtr'))} .mtr files"
          f"{f', {len(guides)} guide templates' if guides else ''}, specular gain {args.spec_gain:g}")

    names = [m.lower() for m in args.material]
    for pat in args.match:
        names += [n for n in materials if fnmatch.fnmatch(n, pat.lower())]
    for mapname in args.map:
        used = collect_map_materials(fs, mapname)
        known = sorted(n for n in used if n in materials)
        print(f"{mapname}: {len(used)} material names, {len(known)} with a material decl")
        names += known
    skins = load_skins(fs)
    for pat in args.md5:
        found = model_surface_uses(fs, skins, materials, pat, (".md5mesh",))
        known = sorted(n for n in found if n in materials)
        print(f"md5 {pat}: {len(known)} materials")
        names += known
    names = list(dict.fromkeys(names))

    # AO the engine's baker already made for md5 meshes, per sidecar (= per diffuse): real geometry
    # occlusion plus the bump cavities, in the diffuse's UV space. It beats AO guessed from the
    # normal map on characters, and the sidecar's AO replaces it in game, so it is copied in.
    baked_by_stem = {}
    if not args.no_baked_ao:
        uses_by_mat = model_surface_uses(fs, skins, materials)
        # the order check uses the models' own materials only: a skin may swap in other textures
        rejected = check_static_order(fs, model_surface_uses(fs, {}, materials, kinds=(".lwo", ".ase")), materials)
        for path in sorted(rejected):
            print(f"baked AO not used for {path}: its maps do not fit the surface order worked out for it")
        for mname, uses in uses_by_mat.items():
            mat = materials.get(mname)
            d = plain_path(mat.get("diffuse")) if mat else None
            if d:
                maps = baked_ao_maps(fs, uses, rejected)
                if maps:
                    baked_by_stem.setdefault(strip_ext(d).lower(), [])
                    baked_by_stem[strip_ext(d).lower()] += [m for m in maps if m not in baked_by_stem[strip_ext(d).lower()]]
        # baked AO follows a model's UV layout, so it only fits a texture used that way everywhere:
        # not one that world brushes tile in any level, and not one laid out differently per model
        world = set()
        for proc in fs.list("maps/", ".proc"):
            world.update(n.lower() for n in PROC_SURFACE.findall(fs.read(proc).decode("latin-1")))
        world_stems = {strip_ext(plain_path(materials[n]["diffuse"])).lower() for n in world
                       if n in materials and plain_path(materials[n].get("diffuse"))}
        on_world = [st for st in baked_by_stem if st in world_stems]
        conflicting = [st for st in baked_by_stem if st not in world_stems and len(baked_by_stem[st]) > 1
                       and coverage_conflict(fs, baked_by_stem[st]) > args.baked_ao_conflict]
        for st in on_world + conflicting:
            del baked_by_stem[st]
        print(f"baked AO: {len(baked_by_stem)} textures; left out {len(on_world)} also on world brushes, "
              f"{len(conflicting)} laid out differently on different models")
    if not names:
        ap.error("no materials selected: use -m NAME or --match GLOB")

    if args.pbr_table and str(args.pbr_table[0]) == "none":
        table_of = lambda m: None  # noqa: E731
        table_of.category = lambda m: None
    elif args.pbr_table:
        table_of = load_pbr_table([t.read_text(errors="replace") for t in args.pbr_table], args.default_rough)
    else:
        texts = [fs.read(p).decode("latin-1") for p in ("pbr/pbr_materials.cfg", "pbr/pbr_overrides.cfg")
                 if fs.read(p) is not None]
        table_of = load_pbr_table(texts, args.default_rough)

    done, seen_diffuse, skipped, hd_report, tm_report = 0, set(), {}, {}, {}

    def skip(reason, what):
        skipped.setdefault(reason, []).append(what)
        if args.verbose:
            print(f"skip {what}: {reason}")

    for name in names:
        mat = materials.get(name)
        if mat is None:
            skip("no such material", name)
            continue
        diffuse_path = plain_path(mat.get("diffuse"))
        if not diffuse_path:
            skip("no plain diffuse map (the sidecar is named after it)", mat["name"])
            continue
        stem = strip_ext(diffuse_path)
        if stem.lower() in seen_diffuse:
            skip("shares its diffuse with a material already done", mat["name"])
            continue
        diffuse_img, _ = load_image(fs, diffuse_path, "RGB")
        if diffuse_img is None:
            skip("diffuse image missing from the game data (unused material)", mat["name"])
            continue
        seen_diffuse.add(stem.lower())

        spec_img = normal_img = height_img = None
        spec_path = plain_path(mat.get("specular"))
        if spec_path:
            spec_img, _ = load_image(fs, spec_path, "L")
        normal_path, height_path, height_scale = bump_sources(mat.get("bump"))
        if normal_path:
            normal_img = load_bump(fs, fs_bump, normal_path, "RGB", hd_report)
        if height_path:
            height_img = load_bump(fs, fs_bump, height_path, "L", hd_report)

        # output: the diffuse's aspect, as detailed as the sources go, capped (--max-size)
        dw, dh = diffuse_img.size
        # the most detailed source sets the width (in the diffuse's aspect)
        src_w = max([dw] + [max(i.size[0], i.size[1] * dw / dh) for i in (spec_img, normal_img, height_img) if i])
        out_w = max(dw, src_w)
        out_w = min(out_w, args.max_size, args.max_size * dw / dh)	# the longer side stays within --max-size
        out_w = max(1, int(round(out_w)))
        size = (out_w, max(1, int(round(dh * out_w / dw))))
        diffuse = as_float(diffuse_img, size)
        # stage texture matrices: the sidecar is read with the diffuse's, so a specular or bump stage
        # with its own (sofloor1's bump is scaled 2, 2) is resampled into the diffuse's texture space
        eye = np.eye(3)
        tms = {k: mat.get(k + "_tm", eye) for k in ("diffuse", "specular", "bump")}
        animated = [k for k, v in tms.items() if isinstance(v, str)]
        if animated:
            tm_report.setdefault("stage transform not constant (scrolling, time...): used as is", []).append(
                f"{mat['name']} ({', '.join(animated)})")
            tms = {k: (eye if isinstance(v, str) else v) for k, v in tms.items()}
        warp_spec = not np.allclose(tms["specular"], tms["diffuse"])
        warp_bump = not np.allclose(tms["bump"], tms["diffuse"])
        if warp_spec or warp_bump:
            tm_report.setdefault("stage transform differs from the diffuse's: resampled", []).append(
                mat["name"] + (" (specular)" if warp_spec else "") + (" (bump)" if warp_bump else ""))
        if spec_img and warp_spec:
            spec = stage_warp(np.asarray(spec_img, dtype=np.float64) / 255.0, tms["specular"], tms["diffuse"], size)
        else:
            spec = as_float(spec_img, size) if spec_img else None

        table = table_of(mat["name"])
        leaf = mat["name"].lower().rsplit("/", 1)[-1]
        word = next((w for w in nonmetal_words if w in leaf), None)
        rough, metal = derive(diffuse, spec, args, table, word is not None)
        shine = class_shine.get(table_of.category(mat["name"]) or "")
        if shine:
            rough = np.maximum(rough / shine, 0.03)

        # AO: baked on the surface at the bump sources' own resolution (--bake-size cap), then resampled
        ao = np.ones(rough.shape)
        no_ao = any(fnmatch.fnmatch(mat["name"].lower(), g.lower()) for g in args.no_ao)
        bump_src = [i for i in (normal_img, height_img) if i]
        baked = []
        for p in baked_by_stem.get(stem.lower(), []):
            try:
                baked.append(as_float(Image.open(io.BytesIO(fs.read(p))).convert("L"), size))
            except Exception as e:  # noqa: BLE001
                print(f"  cannot decode {p}: {e}", file=sys.stderr)
        if baked:
            # each map is white outside its mesh's UV islands; meshes sharing the texture (a character
            # and its zombie variants, posed differently) are averaged over the ones that shade the
            # texel. Darkest-wins would let one variant's pose blacken everyone's arm. The AO strength
            # and --no-ao do not apply: this is the game's own AO.
            stack = np.stack(baked)
            covers = stack < 0.995
            n = covers.sum(axis=0)
            ao = np.where(n > 0, np.where(covers, stack, 0.0).sum(axis=0) / np.maximum(n, 1), 1.0)
        elif bump_src and not no_ao and args.ao_strength > 0:
            bw = int(min(max(i.size[0] for i in bump_src), args.bake_size))
            bake = (bw, max(1, int(round(dh * bw / dw))))
            h = surface_height(np.asarray(normal_img) / 255.0 if normal_img else None,
                               np.asarray(height_img) / 255.0 if height_img else None,
                               height_scale, bake, args.green_sign)
            if h is not None:
                radius = args.ao_radius * bake[0] / 256.0
                # keep only relief at the scale AO looks at: integrated normal maps drift over large
                # distances (an atlas's high-poly curvature, integration error), which would otherwise
                # read as broad slopes and darken whole UV islands
                if args.ao_highpass > 0:
                    h = h - periodic_blur(h, args.ao_highpass * radius)
                h *= args.ao_depth
                vis = horizon_ao(h, radius, args.ao_bias)
                ao = np.clip(1.0 - args.ao_strength * (1.0 - vis), 0.0, 1.0)
                ao = stage_warp(ao, tms["bump"], tms["diffuse"], size) if warp_bump else resample(ao, size)
        normal, height = normal_img, height_img

        out = args.output / f"{stem}_{args.layout}.tga"
        out.parent.mkdir(parents=True, exist_ok=True)
        pack(*(np.clip(c * 255.0 + 0.5, 0, 255).astype(np.uint8) for c in (rough, metal, ao)),
             args.layout).save(out, compression=None)
        if not args.no_maps:
            maps = args.output / "_maps" / stem
            maps.parent.mkdir(parents=True, exist_ok=True)
            for suffix, chan in (("roughness", rough), ("metallic", metal), ("ao", ao)):
                Image.fromarray(np.clip(chan * 255.0 + 0.5, 0, 255).astype(np.uint8), "L").save(
                    f"{maps}_{suffix}.png")
            preview(diffuse, rough, metal, ao, f"{maps}_preview.png")
        used = ", ".join(k for k, v in (("spec", spec), ("normal", normal), ("height", height)) if v is not None)
        if baked:
            used += f", baked AO x{len(baked)}"
        note = ""
        if spec is None:
            note = ", no specular map: flat table values" if table else ", no specular map: flat defaults"
        elif table is not None and table[0] <= 0.0:
            note = ", metal off (non-metal class)"
        elif word:
            note = f", metal off (name says '{word}')"
        print(f"{out}  {size[0]}x{size[1]}  from {used or 'diffuse only'}{note}  "
              f"rough {rough.mean():.2f} metal {metal.mean():.2f} ao {ao.mean():.2f}")
        done += 1
    for reason, what in skipped.items():
        print(f"skipped {len(what)}: {reason}" + ("" if args.verbose else f" (e.g. {what[0]}; -v lists all)"))
    for reason, what in tm_report.items():
        print(f"{reason}: {len(what)}" + ("" if args.verbose else f" (e.g. {what[0]}; -v lists all)"))
        if args.verbose:
            for w in what:
                print(f"    {w}")
    for reason, what in hd_report.items():
        print(f"{reason}: {len(what)}")
        for w in what:
            print(f"    {w}")
    print(f"wrote {done} sidecar(s) to {args.output}")
    return 0 if done else 1


if __name__ == "__main__":
    sys.exit(main())
