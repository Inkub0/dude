# Skin / flesh subsurface scattering (wrapped diffuse) — design

Design reference for **cheap, asset-free subsurface scattering (SSS)** on skin and Hell flesh,
in the per-light interaction pass on both RHI backends (opengl3 + Vulkan). Same enhancement
framing as the rest of the suite (PBR in [pbr-materials.md](pbr-materials.md), parallax in
[parallax.md](parallax.md)): built on the shared interaction shader, driven by cvars and
preset-gated. Defaults **on** at `r_skinShading 0.6` (the Low+ preset value, so a no-preset boot
matches the enhanced tiers); the **Potato** preset alone forces it to `0` — the faithful floor.

## The problem: skin reads as plastic under GGX

The PBR path ([pbr-materials.md](pbr-materials.md)) lights faces and monsters with a tight
Cook-Torrance GGX lobe over an energy-conserving Lambert diffuse. Lambert is the issue for
living tissue: it cuts the diffuse off **hard** at the terminator (N·L = 0), with a straight
`max(N·L, 0)` falloff. Real skin doesn't do that. Light entering the lit side scatters a short
way through the dermis and re-emerges on the shadow side, so the terminator is **soft** and
**reddened** — red light penetrates living tissue furthest, green less, blue least. Without
that, a lit face under GGX reads like painted plastic: a hard, slightly shiny shell.

The canonical fix is Eric Penner's **pre-integrated skin shading** (GDC / SIGGRAPH 2011):
pre-integrate the diffusion profile over the hemisphere against surface curvature and N·L,
stored in a lookup texture. That needs a curvature signal and a baked LUT — **assets**. We want
the look with **no assets**, cheap, on every tier.

## The approach: per-channel wrapped diffuse

We approximate the pre-integrated diffuse's signature analytically with a **per-channel wrapped
diffuse**. For a channel with wrap width `w`:

```
wrapped = clamp( (N·L + w) / (1 + w), 0, 1 )
```

- At `N·L = 1` (fully lit interior) this is exactly `1` — the lit side is **unchanged**.
- It stays positive down to `N·L = -w`, filling the near-terminator shadow with a soft falloff.
- Give red the widest `w`, green less, blue least, and the terminator **warms to red** exactly
  as a diffusion profile does. **The RGB ratio of the wrap widths _is_ the scatter colour.**

`skinScatterExcess()` ([`interaction.frag`](../neo/shaders/interaction.frag)) returns the
per-channel **excess over plain Lambert**, `wrapped - max(N·L, 0) ≥ 0` (≈ 0 except near the
terminator). That is provably non-negative (`wrapped ≥ max(N·L,0)` for all N·L), which is what
lets the shader **add** it instead of restructuring the combine — see below.

This is wrap lighting done per channel. It is not physically pre-integrated (no curvature term),
but it reproduces the two things the eye reads as "skin": a **soft** terminator and a **warm**
one. Cost is a few ALU per lit skin pixel; nothing per-frame, no extra pass, no texture.

## Why it's additive (and therefore safe)

The interaction combine folds N·L into a single `lightScale` that scales **both** the diffuse
and the specular (the specular's N·L cancels the `1/N·L` in the GGX visibility term, so the
shared cosine is correct for both). We must **not** change the specular's cosine — skin
specular should stay tight; only the diffuse scatters.

So instead of rewriting the combine, the shader leaves it byte-for-byte identical and **adds** a
term on top:

```glsl
fragColor.rgb += diffuse.rgb * sssExcess * lightAtten.rgb
               * ( shadowVis * parallaxSelfShadow ) * var_Color.rgb;
```

`sssExcess` rides **exactly** the modulation the Lambert diffuse already got — the light's
`projection × falloff` (`lightAtten`), this light's shadow visibility, the parallax self-shadow
and the vertex colour — but carries the scatter cosine instead of the N·L that `light` folds in.
Summing it with the existing diffuse term relights the albedo by the warm `wrapped` cosine while
the specular lobe keeps the true N·L. It uses the final **AO-weighted** diffuse albedo (added
after the SSAO / occlusion-map blocks), and is non-negative, so the `[0, ∞)` HDR floor holds.

`sssExcess` is `0` on every non-skin draw (`u_skinParms.x == 0`), so the whole thing is a
**strict no-op** off the skin/flesh materials and when `r_skinShading` is 0 — a hard early-out
in `skinScatterExcess()` and a guard on the add. The `shadowVis` scalar is split back out of
`lightScale` for this (a behaviour-preserving refactor); `shadowVisibility()` is still called
exactly once per fragment (it can trace a ray — never call it twice).

Applies in **both** shading branches: the PBR (GGX) path and the legacy Blinn path, so it works
on the non-PBR tiers too — hence "all tiers".

## Material targeting (no new assets)

Keyed off the **PBR material classifier** that already ships
([pbr-materials.md](pbr-materials.md), `base/pbr/pbr_materials.cfg`): faces, heads and monster
hide are tagged `skin`; gibs, raw meat and imp hide are tagged `flesh`. The 56 skin and 300+
flesh rows already exist and are calibrated. `RB_RHI_DrawInteraction`
([`RhiWorld.cpp`](../neo/renderer/rhi/RhiWorld.cpp)) reads `GetPbrCategory()` — a trivial
accessor set at material-parse time, **independent of `r_pbr`** — and fills `u_skinParms` only
for `PBR_CAT_SKIN` / `PBR_CAT_FLESH`, on direct-light non-film draws. Eyes (`PBR_CAT_EYES`) are
deliberately excluded: cornea and enamel are wet, hard and glossy, not scattering.

Flesh scatters ~30 % deeper and redder than face skin (per-category wrap-width multipliers on
the CPU). No `.mtr` edits, no textures, no per-map setup.

## Uniform plumbing

`u_skinParms` (`vec4`, [`renderparms.glsl`](../neo/shaders/renderparms.glsl) /
[`RenderParams.h`](../neo/renderer/rhi/RenderParams.h), appended **last** so no existing std140
offset shifts; struct now 1040 bytes):

| Component | Meaning |
|-----------|---------|
| `.x` | blend strength (`r_skinShading`); **0 = off**, the no-op gate |
| `.y` | red-channel wrap width (`r_skinScatter × 1.0` skin / `× 1.30` flesh) |
| `.z` | green-channel wrap width (`× 0.35` skin / `× 0.30` flesh) |
| `.w` | blue-channel wrap width (`× 0.20` skin / `× 0.15` flesh) |

Uploaded as part of the shared per-draw UBO (`sizeof(RenderParams)`, so the tail is always
written); `0` (memset) on every non-skin draw.

## Cvars

| Cvar | Default | Range | Meaning |
|------|---------|-------|---------|
| `r_skinShading` | `0.6` | 0..1 | SSS strength on skin/flesh materials (0 = vanilla, 1 = full). Defaults on; preset-owned. |
| `r_skinScatter` | `0.4` | 0..1 | Red-channel scatter reach (how far the wrap bleeds past the terminator; g/b derive shorter). Free tuning knob. |

Both are `CVAR_ARCHIVE`, live (no reload), and exposed in **Graphics → Lighting** (a strength
slider; the reach slider greys out while strength is 0). The whole risk here is **taste**, not
code — both knobs are meant to be dialled in-game.

## Preset gating

`r_skinShading` is preset-owned, **on from Low up** at `0.6`; **Potato keeps it `0`** — the
faithful id-render floor (it changes the diffuse falloff, a deliberate deviation from stock
Lambert). The **cvar default is also `0.6`**, so a fresh boot with no preset applied gets the
enhanced look rather than an unlit-looking face. `r_skinScatter` stays the cvar default (`0.4`)
across tiers (pure taste, no cost tier). `0.6 / 0.4` are the tuned starting values; both remain
live knobs in **Graphics → Lighting**.

## Files

- [`neo/shaders/interaction.frag`](../neo/shaders/interaction.frag) — `skinScatterExcess()` +
  the additive term (shared by GL3, Vulkan and the RT-sun variant `interaction_rt.frag`).
- [`neo/shaders/renderparms.glsl`](../neo/shaders/renderparms.glsl),
  [`neo/renderer/rhi/RenderParams.h`](../neo/renderer/rhi/RenderParams.h) — `u_skinParms`.
- [`neo/renderer/rhi/RhiWorld.cpp`](../neo/renderer/rhi/RhiWorld.cpp) — per-draw category gate.
- [`neo/renderer/RenderSystem_init.cpp`](../neo/renderer/RenderSystem_init.cpp) — cvars.
- [`neo/framework/Dhewm3SettingsMenu.cpp`](../neo/framework/Dhewm3SettingsMenu.cpp) — menu
  sliders + preset column.

## Possible follow-ups (not built)

- **Curvature term.** Real pre-integrated skin modulates the scatter width by local curvature
  (flat cheeks scatter more than a sharp nose ridge). We have per-object normals; a screen-space
  `length(fwidth(N))` curvature estimate could scale the wrap width for a more accurate look —
  at the cost of derivatives and some noise. Deferred: the flat wrap already kills the plastic
  read, and curvature is the taste-sensitive part.
- **Transmission (thin-part glow).** Ears/nostrils lit from behind (Barré-Brisebois translucency)
  — a separate back-light term, needs a thickness signal. Out of scope for the terminator fix.
