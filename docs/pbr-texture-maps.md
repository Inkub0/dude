# PBR texture maps for modders (ORM / RMAO sidecars)

DUDE's PBR mode (`r_pbr`, on from the High preset up) gives every material a metalness and a
roughness. For stock assets those are **one value per material**, taken from a table
(`pbr/pbr_materials.cfg`) and tuned in the in-game editor. If you author real PBR textures, you
can replace those two values **per texel** by dropping a packed texture next to the material's
diffuse map. No material (`.mtr`) edit is needed.

Design notes for engine work are in [pbr-materials.md](pbr-materials.md) section 7.

## File names and channel order

Name the texture after the material's **diffuse map** and add a suffix. The suffix tells DUDE
the channel order:

| suffix | Red | Green | Blue | used by |
|---|---|---|---|---|
| `_orm` | ambient occlusion | roughness | metalness | glTF, Unreal, Substance "ORM" exports |
| `_rmao` | roughness | metalness | ambient occlusion | RBDOOM-3-BFG |

Example, the plasma gun:

```
models/weapons/plasmagun/plasmagun.tga        diffuse (from the material)
models/weapons/plasmagun/plasmagun_orm.tga    your packed texture
```

If the diffuse is called `foo_d.tga`, both `foo_d_orm.tga` and `foo_orm.tga` work.

DUDE looks for `.tga`, then `.jpg`, then `.dds`. Use **uncompressed TGA** where you can: the
engine never block-compresses these, and a lossy JPG or a DXT `.dds` smears one channel into
the others.

**Careful with "ORM" tools.** Not every script that says ORM writes ORM. The `batch_orm.py`
going around the community packs Red = roughness, Green = metallic, Blue = AO, which is the
`_rmao` order. Name its output `_rmao`, or use the packer below.

## What each channel does

- **Roughness** (0 = mirror, 1 = fully matte) replaces the material's roughness.
- **Metalness** (0 = paint, plastic, wood, skin; 1 = bare metal) replaces the material's
  metalness. Keep it close to 0 or 1; values in between are for blends such as dirt on metal.
- **Ambient occlusion** (white = open, black = buried) darkens creases. It is used when
  Material AO (`r_occlusionMaps`) is on, and then it **replaces** any other AO map for that
  surface, including DUDE's baked character AO. If your texture has no real AO, fill the
  channel with white.

- **Alpha: height (optional)**, white = top, black = deepest. See
  [Height in the alpha](#height-in-the-alpha). A texture without an alpha channel (24-bit), or
  with a solid white or solid black alpha, has no height and parallax behaves as before.

Everything else about the material stays as it is: diffuse, normal map, specular map
(still a per-texel mask on the highlight), and the material's Wetness and Env glow values
from the table or the editor.

The texture is sampled with the diffuse map's texture coordinates (including the diffuse
stage's `scale`, `translate`... transforms), so it must line up with the diffuse. Keep it at the
diffuse's resolution or lower. If the material scales its bump or specular stage differently
from the diffuse (`sofloor1` repeats its normal map twice), paint the sidecar in the diffuse's
layout; the extractor below resamples those stages for you.

## Height in the alpha

The alpha channel can carry the surface's **height**, which DUDE's parallax occlusion mapping
(`r_parallax`, Graphics > Parallax) marches instead of the height field it otherwise rebuilds
from the normal map at load. That runtime field is shallow by construction (it relaxes the
normal map for only a few texels and then high-passes the result), so broad panels and deep
grooves lose their depth. A baked or painted height keeps them.

- **White = top, black = deepest**, stretched over the whole 0..255 range. Use the full range:
  the depth is set separately (below), not by how bright the map is.
- It is read in the **diffuse's** texture space like the other channels. DUDE carries the
  parallax offset over to the normal and specular maps itself, also when their stages are
  scaled differently.
- Parallax applies to **world surfaces only** (walls, floors, ceilings), and not to alpha-tested
  ones (grates). A height in a model's sidecar is ignored.
- **Depth.** `pbr/height/<name>.cfg` lists, per sidecar, how deep the 0..255 range really is:
  `<sidecar image name> <depth>`, the depth as a distance in the texture's own coordinates
  (texels of relief divided by the texture width). The extractor below writes it for every
  height it bakes, and `tools/bake_pbr_sidecars.sh` puts it in the pack as
  `pbr/height/<pack name>.cfg`. DUDE reads every file in `pbr/height/`, so several packs add up.
  A height with no entry (a hand-painted one) uses the material's usual parallax depth.
  The listed depths set how deep textures are **relative to each other**; the overall depth stays
  the **Parallax depth** slider's (`r_parallaxScale`). A texture of typical depth (0.062) marches
  as deep as a material without a sidecar at the same slider value, since the true depth of
  Doom 3's normal maps is far too deep for parallax mapping, which has no texels for the walls of
  a recess.
- The in-game material editor says "A = height" under **Material set by texture** when it
  found one, and whether it has a baked depth.

```
// pbr/height/my_pack.cfg
textures/mymod/panel01_orm 0.031
```

## Packing your maps

`tools/pbr_pack_orm.py` (Python 3 with Pillow and NumPy) packs separate maps:

```
python3 tools/pbr_pack_orm.py -i my_maps -o packed
```

It reads every `<name>_roughness`, `<name>_metallic` and `<name>_ao` image in `my_maps` (PNG,
TGA, JPG and more) and writes `packed/<name>_orm.tga`. Add `--layout rmao` for the RBDOOM
order. A missing map is filled with a constant (`--default-rough 0.6`, `--default-metal 0`,
white AO) instead of skipping the set, so a texture with only a metal mask still packs.

A `<name>_height` image (also `_heightmap`, `_displacement`, `_disp`) goes into the alpha,
which makes the TGA 32-bit; without one the TGA stays 24-bit, with no height.

One texture at a time:

```
python3 tools/pbr_pack_orm.py --base plasmagun --metal plasmagun_metal.png \
    --rough-from-spec plasmagun_s.tga -o packed
```

`--rough-from-spec` turns a Doom 3 specular map into a **starting** roughness (bright
specular = smoother). It is a first pass to paint over, not a conversion: the specular map
does not hold enough information to rebuild roughness on its own. Metalness always needs a
hand-made mask (the wires on the plasma gun are not metal; most of its casing is).

## Starting from the game's own textures

`tools/pbr_extract_maps.py` makes a first-pass sidecar for stock materials, straight from the
game's `.pk4` files. Doom 3 and Quake 4 only ship a diffuse, a normal map and a specular map,
so every channel is an estimate:

| channel | made from | how far to trust it |
|---|---|---|
| Roughness | the specular map, inverted (brighter specular = smoother) | a fair start |
| Metalness | the specular map's grey (`--metal-source spec`), scaled so its average is the material's PBR table metalness (`--metal-anchor table`: hollowmetal 0.38, solidmetal 0.50...), so the class level tuned in game stays and the specular map supplies the variation: bare steel above, paint and grime below. Doom 3 is mostly metal and painted metal, and id painted bare steel bright, paint darker. `--metal-anchor none --metal-cap 0.2` gives the earlier look (raw grey, capped). `--metal-source mask` makes a near-binary mask instead (bright specular on a colourless diffuse) | fine for Doom 3's metal and painted-metal surfaces; glossy non-metals still need the class or name checks below |
| AO | horizon-based AO of the surface (normal map turned into heights, plus the material's height map), wrapping around as if the tile sat among its 3x3 neighbours | good on tiling world textures; model atlases and non-tiling textures (doors) can shade their borders |
| Height (alpha) | the same surface heights, whole: the normal map integrated exactly over the tile (Frankot-Chellappa, periodic) plus the material's height map, stretched to 0..255; its depth goes to `pbr/height/<--height-name>.cfg`. `--no-height` leaves it out, `--height-highpass F` drops variation broader than F of the width | as true as the normal map's slopes; a painted normal map that does not describe a real surface integrates to slow tilts |

Roughness and metalness both keep the material's current table value as their average
(`--rough-anchor table`, `--metal-anchor table`), so the look tuned in game stays and the
specular map only adds variation around it. Ceramic-class
materials are then made 2.4 times smoother (`--class-shine ceramic=2.4`, picked in game:
glazed tile reads too dull otherwise); repeat the flag for other classes, `ceramic=1` turns it
off. Materials that share a diffuse share a sidecar, so the class of the first one baked wins.

A whole class can also get a channel adjust of its own, applied last, after the global adjust:
`--class-adjust CLASS:CHANNEL=B,C,G` (brightness, contrast, gamma; channel `ao`, `rough` or
`metal`), the same step as the PBR editor's **Edit texture channels** on one texture, but for
every texture of the class. The default is `ceramic:rough=-0.15,1,1` (2026-11-17, picked in
game on `stectile1quad2` and `washroom/btile_d`: glazed tile still read too dull);
`ceramic:rough=0,1,1` turns it off. Repeat the flag for more.

AO is baked from the normal and height maps the game itself loads, so it lines up with the
relief you see. `--hd FOLDER` bakes from a higher-resolution texture pack instead, but only use
it for a pack your players install too: an HD pack's normal maps are often redrawn, and AO
from relief the game does not draw lands beside the real one. Even then, the tool compares
each HD map with the stock one, re-orients it if it was saved mirrored (D3HDP has several,
the kitchen chair among them) and falls back to the stock map when they do not match; the
run ends with a list of both. Roughness and metalness always come from the stock specular maps.

A texture without a specular map keeps the material's current values from the PBR table,
flat, so the sidecar is never worse than what it replaces. A material whose PBR class is a
non-metal one (concrete, wood, skin...) or whose name contains words like tile, ceramic or
marble never gets metal.

```
python3 tools/pbr_extract_maps.py --search base -m textures/mcity/mcityc9 -o pbr_out
python3 tools/pbr_extract_maps.py --search base --match 'textures/mcity/*' -o pbr_out
python3 tools/pbr_extract_maps.py --search /path/to/quake4/q4base --search q4base \
    --match 'textures/medlabs/*' -o pbr_out
```

For a whole level, `--map` collects every material the map draws: world surfaces, prop
models, entity models (characters and their heads, through the entity definitions) and skins:

```
python3 tools/pbr_extract_maps.py --search base --map game/mars_city1 \
    --no-ao 'models/characters/*' -o pbr_out
```

`--md5 GLOB` bakes every material the matching `.md5mesh` files use, skins included, for
example `--md5 'models/md5/*'` for all characters, monsters and weapons.

A sidecar's AO replaces every other AO source in game (except on models that have their own
baked map while "Accurate Model Baked AO", `r_pbrModelBakedAO`, is on: there the baked map wins,
docs/pbr-materials.md section 7), including the AO DUDE's own baker made
for models (`bakeAO`, the `generated/aomaps/` maps in the baked AO pack). So for models (md5
characters and monsters, and LWO / ASE props) the extractor copies that baked AO into the
sidecar instead of guessing AO from the normal map: it holds the real geometry occlusion plus
the normal map's cavities, in the diffuse's UV space. When several meshes share one texture (a
character and its zombie variants), each texel averages the meshes that cover it.

Baked AO follows one model's UV layout, so it is only used where that layout is the texture's
only use. It is left out (and AO comes from the normal map as usual) for a texture that world
brushes use in any level, and for one laid out differently on different models (a tiling rock
texture wrapped over many props; `--baked-ao-conflict` sets how strict that test is). For props
the tool also works out the engine's surface order and checks it against the baked maps' sizes;
a model that does not check out is listed and skipped. `--no-baked-ao` turns all of this off.

`--no-ao GLOB` writes white AO for matching materials without baked AO. Use it where AO guessed
from the normal map would look wrong, for example character atlases, whose island borders
shade badly.

`--search` takes game folders in load order (later wins). Quake 4's guide templates are
expanded the way the engine does, and its specular maps, authored darker than Doom 3's, get a
brightness gain (`--spec-gain`, automatic).

What you get in `pbr_out`:

- `textures/.../<diffuse>_orm.tga` at the game path, ready to zip into a `.pk4`
  (`--layout rmao` for the other order).
- `_maps/textures/.../<diffuse>_roughness.png`, `_metallic.png`, `_ao.png`: greyscale maps
  to fix by hand, plus `_preview.png` (diffuse, roughness, metalness, AO side by side) to
  review a batch quickly.

After fixing a map, repack it with `tools/pbr_pack_orm.py` (single mode, `--rough`, `--metal`,
`--ao`). Final per-channel corrections for every texture: brightness, contrast and gamma for AO,
roughness and metalness. Tune them in game in the PBR editor's **All sidecars** tab, which shows
every sidecar live with the sliders applied *relative to what the textures were baked with*:
sliders equal to the last bake show the textures as they are. **Save for next bake** writes the
sliders to `pbr/pbr_sidecar_adjust.cfg`; the extractor uses that file as its defaults, and the
bake records what it used in the pack as `pbr/pbr_sidecar_baked.cfg`. After the re-bake the
sliders start where you left them, now matching the textures. `--ao-adjust`, `--rough-adjust`
and `--metal-adjust` (BRIGHTNESS CONTRAST GAMMA) override the file. Re-bake every pack with the
same settings: the game reads the record from the last-loaded pack.

Materials whose diffuse image is missing from the game data (leftovers in id's own
material files) or shared with another material are skipped and counted at the end; `-v`
lists them. The derivation thresholds are all flags; `--help` lists them.

### A whole level in one go

`tools/bake_pbr_sidecars.sh` runs the extractor with the defaults above and packs the result,
so a pack can be rebuilt the same way any time:

```
tools/bake_pbr_sidecars.sh -m game/mars_city1 -i base
tools/bake_pbr_sidecars.sh -m game/mars_city1 -m game/mc_underground -n z_pbr_marscity -i base
```

It writes `z_pbr_<level>.pk4` (or `-n NAME`) and copies it to each `-i` folder. Hand-made
sidecars win: a loose `_orm` or `_rmao` file already in a game folder's `textures/` or
`models/` is left out of the pack. The pack carries a `bake-info.txt` with the command, the tool
and PBR table versions, so you can tell what made it. The work folder
(`build/pbr-sidecars/<name>`, or `-o`) keeps the editable `_maps/` and the log. Options after
`--` go to the extractor; `-h` lists the rest.

### The ready-made packs

DUDE's own sidecar packs, the optional downloads, are made with `tools/pack_pbr_sidecars_d3.sh vN`.
It bakes every level and md5 model of each game with the settings in `pbr/pbr_sidecar_adjust.cfg`
and writes one pack per game to `dist/d3-pbr-sidecars/`: `z_pbr_sidecars.pk4` for `base/`,
`z_pbr_sidecars_roe.pk4` for `d3xp/` and `z_pbr_sidecars_lm.pk4` for `d3le/`. Each game gets its
own pack in its own folder because Resurrection of Evil and The Lost Mission redefine some of
Doom 3's materials. Give it the games to re-bake (`base`, `roe`, `lm`); it packs all three either way.
`OUT_DIR=<dir>` writes them elsewhere, so a new version does not overwrite the files of one already
uploaded; `PACK_ONLY=1` bakes nothing and only re-collects and re-stamps the packs already in
`build/pbr-sidecars/`. The packs carry the parallax height in the alpha and their
`pbr/height/<pack>.cfg`; the alpha compresses worse than the other channels, so they are about
65% larger than without it (Doom 3: 156 -> 258 MB).

## Installing and checking

1. Put the textures at the same path as the diffuse, inside a `.pk4` or loose in your
   mod folder or `base/`.
2. In game, turn on PBR (Graphics, or the High preset and above). Turn on Material AO too if
   your texture has AO.
3. Aim at the surface and run `editPbrMaterial` (bind it to a key, for example
   `bind p editPbrMaterial`). The editor shows **"Material set by texture:"** with the file
   it found and the channel order. The Metalness and Roughness sliders are greyed out, because
   the texture now sets them. Wetness and Env glow still work.
4. To adjust a sidecar in game, use **Edit texture channels** in the same editor: brightness,
   contrast and gamma for the AO, roughness and metalness channels. The change previews live on
   every surface that uses the texture (also in the debug views below). **Save to texture**
   writes it into `<name>.tga` in your save folder (`fs_savepath`), where it overrides the
   packed copy; `tools/bake_pbr_sidecars.sh` leaves such loose sidecars out of its packs.
   `.dds` sidecars cannot be edited this way.
5. Added a texture while the game was running? Press **Reload PBR Table** in the editor (or
   run `reloadPbrTable`) so DUDE looks for it again; the next level load looks again as well.
   Changed the pixels of a texture it already loaded? Run `reloadImages`.

Sidecars load with the level (`r_pbrTextureMapPrecache`, on by default; Debugging > Loading &
Caches > "Preload PBR Textures at Level Load"), so there is no pause the first time a material
comes into view and each level frees the previous one's.

`r_pbrTextureMaps 0` switches all sidecar textures off for comparison, live: every material
falls back to its PBR table values, and the editor says the texture is ignored and the sliders
apply again. In the menus it is **Graphics > Lighting > Per-Pixel PBR Textures (ORM)**, and the
**Per-pixel textures (ORM sidecars)** checkbox at the top of the PBR Material Editor.

To see the channels on their own, use **PBR Material View** in the settings menu (Debug Views),
or `r_pbrDebugView`: 1 = ambient occlusion, 2 = roughness, 3 = metalness, 4 = the ORM texture
(R = AO, G = roughness, B = metalness, whichever order your file uses), 0 = the normal view.
`bind <key> pbrDebugViewNext` cycles through them. Surfaces without a sidecar show their PBR
table values (flat), so a sidecar stands out by its detail. AO shows white where a surface has
none. While a view is up, exposure, eye adaptation, bloom, the tonemap and gamma are switched
off, so the grey on screen is the stored value: black 0, mid grey 0.5, white 1.

## Limits

- Ray-traced bounce light and the surfaces seen inside ray-traced reflections still use the
  material's single table value. Everything you see directly (lighting, screen-space and ray
  traced reflections on the surface itself, ReSTIR lighting) uses the texture.
- Image programs as diffuse (`addnormals(...)`, `heightmap(...)` and the like) have no file
  name to hang a sidecar on, so those materials cannot take one.
- The sidecar follows the **diffuse map**, so materials that share a diffuse share a sidecar.
- A `.dds` sidecar needs an alpha format (BC3 / DXT5 or BC7) to carry a height; BC1 / DXT1
  drops it.
