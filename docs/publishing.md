# Phase: Publishing

Status: **PUBLISHED (2026-08-19)** — live at https://github.com/Inkub0/dude as a public
GitHub **fork of dhewm/dhewm3** (renamed to `dude`; the user opted for the fork route
over a standalone repo). License hygiene shipped; `origin` = the fork, `upstream` =
dhewm/dhewm3; merges continue within our fork, never into dhewm3.

**Goal:** publish DUDE (dhewm-rt) as its own public **GPLv3** project — engine source
only — so the work can be shared, without ever redistributing id/Bethesda game assets.

**Why standalone, not upstream:** `master` is ~171 commits ahead of
`upstream/master` (github.com/dhewm/dhewm3) — a whole Vulkan/RHI backend plus the
enhancement suite. dhewm3 upstream is a deliberately conservative faithful port and
won't take a divergence this size, so DUDE is effectively its own project. "Merging"
happens **within our own fork** (feature branches → our `master`), never into dhewm3.

A GitHub "fork" vs a fresh standalone repo is **immaterial to the GPL** — the fork
button is a social/UI feature, not a legal one. For something this divergent a
standalone repo is the nicer home; GPL only requires preserving the existing copyright
notices (already present), not a GitHub fork link.

## GPL compliance checklist (we ship id's + dhewm3's + our code as one work)

The combined derivative work must stay GPLv3 — this is exactly what the GPL is for;
every Doom 3 engine fork (dhewm3 included) does this. Requirements:

- [x] Ship the license text — `COPYING.txt` present and tracked.
- [x] Preserve the original copyright/license headers (id Software Doom 3 GPL header,
  1999–2011) — intact; never strip them.
- [x] Complete corresponding source — the public repo satisfies this (required if we
  ever distribute binaries).
- [x] **Note our modifications** (GPLv3 §5a): README "License note" added 2026-08-19
  ("modified version of dhewm3, based on id Software's Doom 3 GPL source", with links).
- [x] **GPL headers on new DUDE source files** — new .cpp/.h all carry the
  "see ArbProgram.h for license header" pointer convention (ModCvarTranslation.{cpp,h}
  were the only stragglers, fixed 2026-08-19). The 140 new shader files are covered by
  the README statement that all DUDE-authored source incl. `neo/shaders/` is GPLv3
  (the sanctioned alternative to per-file headers). Third-party (SMAA/ImGui/VMA/FSR2)
  keep their own licenses.
- The whole combined work stays GPLv3; our additions are copyleft-bound — no relicensing
  more restrictively, no added restrictions on the GPL rights.

## Assets — the one line not to cross

id's Doom 3 GPL release was **engine-only**. The `base/*.pk4` game data (~1.67 GB) is
proprietary id/Bethesda content and was **never** GPL — it cannot be redistributed.
Already `.gitignore`d (verified). The public repo stays **code-only**; users bring their
own Doom 3 install. Generated assets (baked-AO pk4s, `pbr/`, `envprobes/`) also stay out
of the code repo — LFS / release attachments / a separate private repo (see the "assets
in a separate repo" plan and docs/filesystem-path-strategy / packaging).

**Decision 2026-08-19 — tracked menu GUIs stay in:** `base/guis/mainmenu.gui` (+
`guis/dude/`) remain tracked at the user's call: heavily modified, derived via a
community widescreen pack rather than raw id files, and slated for a complete
from-scratch re-authoring (docs/main-menu-dude-button.md). Risk acknowledged: they
still share lineage with id's GUI scripts; the full rewrite retires the concern.

**Known exception — `base/guis/assets/mainmenu/dude_logo.tga`:** the high-res DOOM 3
logo used by the DUDE main menu. It is **technically copyright-protected id/Bethesda
artwork** (the same logo art is distributed through other channels, e.g. the Steam
store assets) — tracked in the repo and shipped in `zz_dude_menu.pk4` as a knowing,
deliberate exception to the assets rule, on the theory that a game logo used to launch
that same game is the lowest-risk asset class. If it ever draws a complaint, drop it
from the repo/pk4 and let the menu fall back to text or a re-drawn original logo.

## Steps (when we do it)

1. Create the empty public repo (standalone recommended; GitHub/GitLab/Codeberg).
2. Add it as `origin` locally (there is **no** `origin` yet — only `upstream=dhewm3`).
3. License hygiene: add GPL headers to new DUDE files; add the README attribution +
   "modified from dhewm3" note; confirm `COPYING.txt` is intact.
4. Verify no assets are staged (`.gitignore` airtight; `git ls-files | grep pk4` empty).
5. Push `master` + the feature branches; do the branch merges within our fork.

**Publishing is the user's action** — never `git push` / open PRs unprompted; leave the
actual publish + repo creation to the user. This doc is the prep checklist for that day.
