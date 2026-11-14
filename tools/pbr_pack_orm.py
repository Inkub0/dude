#!/usr/bin/env python3
"""Pack separate roughness / metalness / AO maps into a DUDE PBR sidecar texture.

DUDE (docs/pbr-materials.md sec. 7) picks up a packed texture that sits next to a material's
diffuse map and overrides that material's PBR table values per texel:

    <diffuse>_orm.tga    R = ambient occlusion, G = roughness, B = metalness   (glTF / Unreal order)
    <diffuse>_rmao.tga   R = roughness, G = metalness, B = ambient occlusion   (RBDOOM-3-BFG order)

Example: the diffuse is models/weapons/plasmagun/plasmagun.tga, so the sidecar is
models/weapons/plasmagun/plasmagun_orm.tga (a diffuse named foo_d.tga also finds foo_orm.tga).

Batch mode (default): every <base>_roughness / _metallic / _ao image in --input becomes
<base>_orm.tga (or _rmao with --layout rmao) in --output. A missing channel gets a constant
instead of skipping the set: --default-metal (0), --default-rough (0.6), AO always white.
Accepted spellings per channel: _roughness/_rough, _metallic/_metalness/_metal,
_ao/_occlusion. Inputs may be any format Pillow reads; the first channel (or luminance) is used.

Single mode: --base NAME --rough FILE [--metal FILE] [--ao FILE] -o OUT_DIR.

--rough-from-spec FILE derives a STARTING roughness from a Doom 3 specular map (1 - luminance,
remapped to --spec-range). It is a first pass for hand editing, not a conversion: the specular
map has no unique PBR meaning.

Writes uncompressed 24-bit TGA. The engine loads sidecars uncompressed (no DXT), so keep them
at the diffuse's resolution or below.
"""

import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image

CHANNEL_SUFFIXES = {
    "rough": ("_roughness", "_rough"),
    "metal": ("_metallic", "_metalness", "_metal"),
    "ao": ("_ao", "_occlusion"),
}
IMAGE_EXTS = {".tga", ".png", ".jpg", ".jpeg", ".bmp", ".tif", ".tiff", ".dds", ".webp"}


def load_channel(path, size=None):
    img = Image.open(path)
    if img.mode in ("I;16", "I;16B", "I", "F"):
        arr = np.asarray(img, dtype=np.float64)
        arr = arr / max(arr.max(), 1.0) * 255.0
        img = Image.fromarray(arr.astype(np.uint8), "L")
    elif img.mode != "L":
        # a greyscale map saved as RGB: take the first channel, as artists' tools expect
        img = img.convert("RGB").getchannel(0)
    if size is not None and img.size != size:
        img = img.resize(size, Image.BICUBIC)
    return np.asarray(img, dtype=np.uint8)


def rough_from_spec(path, lo, hi, size=None):
    img = Image.open(path).convert("L")
    if size is not None and img.size != size:
        img = img.resize(size, Image.BICUBIC)
    spec = np.asarray(img, dtype=np.float64) / 255.0
    rough = lo + (hi - lo) * (1.0 - spec)
    return np.clip(rough * 255.0 + 0.5, 0, 255).astype(np.uint8)


def constant(size, value):
    return np.full((size[1], size[0]), int(round(np.clip(value, 0.0, 1.0) * 255.0)), dtype=np.uint8)


def pack(rough, metal, ao, layout):
    if layout == "orm":
        chans = (ao, rough, metal)
    else:
        chans = (rough, metal, ao)
    return Image.fromarray(np.stack(chans, axis=-1), "RGB")


def build_one(name, files, args, out_dir):
    sources = [f for f in files.values() if f is not None]
    if not sources and not args.rough_from_spec:
        print(f"skip {name}: no input maps", file=sys.stderr)
        return False
    ref = sources[0] if sources else Path(args.rough_from_spec)
    size = Image.open(ref).size
    if files.get("rough"):
        rough = load_channel(files["rough"], size)
    elif args.rough_from_spec:
        rough = rough_from_spec(args.rough_from_spec, args.spec_range[0], args.spec_range[1], size)
    else:
        rough = constant(size, args.default_rough)
    metal = load_channel(files["metal"], size) if files.get("metal") else constant(size, args.default_metal)
    ao = load_channel(files["ao"], size) if files.get("ao") else constant(size, 1.0)
    out = out_dir / f"{name}_{args.layout}.tga"
    out.parent.mkdir(parents=True, exist_ok=True)
    pack(rough, metal, ao, args.layout).save(out, compression=None)
    have = ", ".join(k for k in ("rough", "metal", "ao") if files.get(k)) or "none"
    print(f"{out}  ({size[0]}x{size[1]}, from: {have})")
    return True


def split_suffix(stem):
    low = stem.lower()
    for chan, suffixes in CHANNEL_SUFFIXES.items():
        for suf in suffixes:
            if low.endswith(suf) and len(stem) > len(suf):
                return stem[: -len(suf)], chan
    return None, None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--layout", choices=("orm", "rmao"), default="orm",
                    help="channel order and file suffix (default orm: R=AO G=rough B=metal)")
    ap.add_argument("-i", "--input", type=Path, default=Path("input"), help="batch input folder")
    ap.add_argument("-o", "--output", type=Path, default=Path("output"), help="output folder")
    ap.add_argument("--base", help="single mode: output base name (the diffuse name without extension)")
    ap.add_argument("--rough", type=Path, help="single mode: roughness map")
    ap.add_argument("--metal", type=Path, help="single mode: metalness map")
    ap.add_argument("--ao", type=Path, help="single mode: ambient occlusion map")
    ap.add_argument("--rough-from-spec", help="single mode: derive a starting roughness from a specular map")
    ap.add_argument("--spec-range", type=float, nargs=2, default=(0.25, 0.9), metavar=("LO", "HI"),
                    help="roughness for white / black specular with --rough-from-spec (default 0.25 0.9)")
    ap.add_argument("--default-rough", type=float, default=0.6, help="roughness when no map is given (0..1)")
    ap.add_argument("--default-metal", type=float, default=0.0, help="metalness when no map is given (0..1)")
    args = ap.parse_args()

    if args.base:
        files = {"rough": args.rough, "metal": args.metal, "ao": args.ao}
        return 0 if build_one(args.base, files, args, args.output) else 1

    if not args.input.is_dir():
        ap.error(f"input folder {args.input} not found (or use --base for a single texture)")
    sets = {}
    for f in sorted(args.input.iterdir()):
        if not f.is_file() or f.suffix.lower() not in IMAGE_EXTS:
            continue
        base, chan = split_suffix(f.stem)
        if base:
            sets.setdefault(base, {})[chan] = f
    if not sets:
        print(f"no *_roughness / *_metallic / *_ao images in {args.input}", file=sys.stderr)
        return 1
    ok = sum(build_one(name, files, args, args.output) for name, files in sorted(sets.items()))
    print(f"packed {ok}/{len(sets)}")
    return 0 if ok == len(sets) else 1


if __name__ == "__main__":
    sys.exit(main())
