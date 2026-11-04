# DRAFT: GPLv3 section 7 permission for the Quake 4 game library

**Status: draft for the author's review, not in effect.** It takes effect only when the
final wording is published with DUDE's licence files (the plan is in "Where it goes"
below). Until then everything in DUDE is plain GPLv3.

## Why

The Quake 4 game library (`q4base.so` / `q4base.dll`, built from the dude-q4 repository)
statically links DUDE's idlib. That idlib is GPLv3. Raven's game code is under the Quake 4
SDK EULA, and the two licences don't allow that combination. GPLv3 section 7 lets a
copyright holder add permissions to their own code. This one allows exactly that
combination for the code the DUDE author holds the copyright in.

It can't cover anyone else's code. What the library still carries from others
(`tools/quake4/q4base_licence_audit.py`, numbers in
[quake4-release-licensing.md](quake4-release-licensing.md)) stays plain GPLv3:

- id Software's GPL-only idlib code (mostly the SSE/SSE2 SIMD paths)
- the remaining dhewm3 one-liners (casts, renames fixed by the ABI)

## The text (proposed)

```
Additional permission under GNU GPL version 3 section 7

The following applies only to the parts of DUDE whose copyright is held by
Inkub0 [optional: legal name] - the code and text added or changed in this
repository's history by commits authored as Inkub0. It does not apply to
code by id Software, the dhewm3 contributors or any other author.

If you modify DUDE, or any covered work, by linking or combining it with the
game code of the Quake 4 Software Development Kit by Raven Software and
id Software (or a modified version of that code), containing parts covered
by the terms of the Quake 4 SDK End User License Agreement, the licensor of
those parts of DUDE grants you additional permission to convey the resulting
work. The parts of such a combination that come from the Quake 4 SDK remain
under the terms of its End User License Agreement; the GNU GPL does not
require you to provide them as Corresponding Source.

You may remove this additional permission from your copy, or from works based
on it, as section 7 of the GNU GPL version 3 allows.
```

## Choices for the author

1. **Scope: any Quake 4 SDK game code, or only dude-q4.** The draft covers any
   SDK-based game code, so other people's Quake 4 mods built against DUDE get the same
   permission. The narrower version would replace "the game code of the Quake 4 Software
   Development Kit ... (or a modified version of that code)" with "the Quake 4 game library
   built from the dude-q4 repository (github.com/Inkub0/dude-q4)".
2. **Name.** The draft names the holder as Inkub0, which the git history ties to the
   commits. You can add your legal name in the bracketed slot or leave it out.
3. **The Corresponding Source sentence.** The FSF template optionally requires the
   library's source to ship with the binary. For the SDK code we can't promise that for
   everyone downstream (the EULA governs it), so the draft says the opposite: the SDK parts
   are not GPL Corresponding Source. dude-q4 publishes its source anyway.

## Where it goes (once the wording is final)

- A new file `COPYING-QUAKE4-EXCEPTION.txt` next to `COPYING.txt`, holding the text.
- One sentence in README.md's licence note pointing to it.
- The README of the `dude-q4` archive (`package-q4.sh`) quotes it where it explains which
  parts of the library are GPL.
- Not in every source file's header: the permission names the code by author, and the git
  history is the record of what that covers.

Background: the GPL FAQ on linking exceptions,
https://www.gnu.org/licenses/gpl-faq.html#GPLIncompatibleLibs - the text above follows its
template. Not legal advice.
