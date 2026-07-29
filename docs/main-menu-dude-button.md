# Main-menu "DUDE Settings" button (GUI patch snippet)

This documents a small, reproducible edit to the main-menu button that opens the
F10 settings menu, so the customization is version-controlled **without** committing
any third-party/derivative game GUI into this repo.

## What & where

The button lives in `guis/mainmenu.gui` inside a **widescreen GUI pak** (a CstDoom3 /
"WideGuis"-style community mod, e.g. `zWideGuis_D3.pk4`). It is **not** part of the
DUDE engine and is intentionally git-ignored (`*.pk4`). Only *our* change to it — the
`dhewm3set1` windowDef below — is tracked here.

The pak is read from the game's writable/base data dir, which is OS-specific:

- **Linux:** `$XDG_DATA_HOME/dude/base/` (typically `~/.local/share/dude/base/`) — this
  is `fs_savepath` and **overrides** whatever you pass as `fs_basepath`.
- **Windows:** `Documents/My Games/dhewm3/base/`
- **macOS:** `~/Library/Application Support/dhewm3/base/`
- Plus whatever `fs_basepath` you launch with (e.g. a repo `base/`). If a pak of the
  same name exists in both, the `fs_savepath` copy wins (see
  `neo/framework/FileSystem.cpp` `Startup()` order). Edit the copy that actually loads.

## The edit

Stock (image button — the low-res "dhewm³ Settings" logo):

```
////dhewm3 settings
	windowDef dhewm3set1 {
		background    "guis/assets/dhewm3/logo1"
		rect        208, 443, 156.75, 19
		matcolor 	0.9, 1, 1, 0.6
		onMouseEnter {
				transition "matcolor" "0.9, 1, 1, 0.5" "1 1 1 1" "200" ;
				resetTime "ToolTip1" "0" ;
				set "ToolTip2::text" "dhewm3 Settings Menu" ;
			}
		onMouseExit {
					transition "matcolor" "1 1 1 1" "0.9, 1, 1, 0.5" "200" ;
					resetTime "ToolTip2" "0" ;
		}
		onAction {
				set	"cmd" "play guisounds_menuclickdown" ;
				set "cmd" "exec" "dhewm3Settings";
		}
}
```

Ours (text button — smooth `fonts/bank`, renders "DUDE Settings"):

```
////DUDE settings
	windowDef dhewm3set1 {
		rect        208, 443, 156.75, 19
		text        "DUDE Settings"
		font        "fonts/bank"
		textscale   0.34
		textalign   0
		textaligny  -6
		forecolor   0.9, 1, 1, 0.6
		onMouseEnter {
				transition "forecolor" "0.9 1 1 0.6" "1 1 1 1" "200" ;
				resetTime "ToolTip1" "0" ;
				set "ToolTip2::text" "DUDE Settings Menu" ;
			}
		onMouseExit {
				transition "forecolor" "1 1 1 1" "0.9 1 1 0.6" "200" ;
				resetTime "ToolTip2" "0" ;
		}
		onAction {
				set	"cmd" "play guisounds_menuclickdown" ;
				set "cmd" "exec" "dhewm3Settings";
		}
}
```

Notes:
- Keep the `dhewm3set1` windowDef **name** (a rect wiggle animation elsewhere in
  `mainmenu.gui` references `dhewm3set1::rect`) and the `exec "dhewm3Settings"`
  command (that's the registered engine command that opens the F10 menu).
- `textscale` must stay above `gui_smallFontLimit` (0.30) to use the crisp 24px font
  sheet; `textaligny` nudges vertical position; `fonts/bank` is Doom 3's smooth
  "terminal" font. See `gui_hiResFonts` (Video tab) for the resolution-aware upgrade.

## Reapply

Unpack `guis/mainmenu.gui` from the pak, replace the `windowDef dhewm3set1 { ... }`
block with the "Ours" version above, then update that single entry back into the pak
(`zip -X <pak> guis/mainmenu.gui`).

## Button-label vertical nudge (all buttons +2)

Beyond the settings button, every **button/tab label** in `mainmenu.gui` was moved
**down 2 points** so the text sits better on its button graphic. This is a rule, not
a hand-edit of ~80 blocks:

1. **+2 to `textaligny`** on every text `windowDef` whose name contains `Btn`,
   `Button`, or `Tab` (73 buttons + 5 LAN tabs). Where a block had no `textaligny`,
   insert `textaligny 2` (absent == 0).
2. **Exclusions:** the `dhewm3set1` DUDE-Settings button (positioned separately,
   above), and pure titles (`*TitleText`), tooltips (`ToolTip*`/`*Tip`), credits
   (`Credits*`), column headers, and dialog body text.
3. The four main nav buttons (`NewGameBtnText`, `LoadBtnText`, `MultiplayerBtnText`,
   `OptionsBtnText`) got the same +2 first, so they're already at the target.
4. **Settings-list rows** (the `*Title` entries inside option lists — control binds,
   video/audio/system settings, ~100 of them) were **left as-is** — they're list
   content, not buttons.
5. **Difficulty selectors** then nudged back **−1** (net +1 vs. stock): `NGBtnText1`
   (Recruit), `NGBtnText2` (Marine), `NGBtnText3` (Veteran), `NGBtnText4` +
   `NGBtnText4Alt` (Nightmare). The `NGBtnText0` "LEVEL OF DIFFICULTY" header keeps
   the +2.

Apply it with a brace-aware pass that only touches each block's **own-level**
`textaligny` (never `textaligny` inside nested `onAction`/`set` handlers), e.g. the
`shift.py`/`ng.py` transformers used during this work.

## Loose-file override (alternative to editing the pak)

None of the above requires touching `zWideGuis_D3.pk4`. Doom 3 inserts each search
**directory ahead of its pak files** (`neo/framework/FileSystem.cpp` `AddGameDirectory`),
so a **loose `base/guis/mainmenu.gui` overrides the pak's copy**. Caveats:

- It's **whole-file** — GUIs have no partial/patch merge, so the loose file is a full
  copy of `mainmenu.gui` with the edits, and it shadows the pak's version entirely.
- It must sit in the **winning** game dir. With `run.sh`
  (`fs_savepath == fs_basepath == repo`), `repo/base/guis/mainmenu.gui` wins; under the
  old launch (`fs_savepath = ~/.local/share/dude`) it must go there instead. Editing the
  pak(s) directly (as done here) is launch-method-agnostic.
- Either way this file is derived from a third-party/community GUI, so it stays
  git-ignored (see the `*.pk4` / `/base/…` rules in `.gitignore`).
