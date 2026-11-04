# GPLv3 section 7 permission for the Quake 4 game library

**Status: in effect.** The text is in
[COPYING-QUAKE4-EXCEPTION.txt](../COPYING-QUAKE4-EXCEPTION.txt), next to `COPYING.txt`. This
page explains why it exists and what it covers.

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

## What was decided

1. **Scope: any Quake 4 SDK game code**, not only dude-q4, so other people's Quake 4 mods
   built against DUDE get the same permission.
2. **Name: Inkub0**, the name the git history ties to the commits; no legal name.
3. **Corresponding Source:** the SDK parts of a combination are not GPL Corresponding Source
   (the EULA governs them). dude-q4 publishes its source anyway.

## Where it is

- `COPYING-QUAKE4-EXCEPTION.txt` in the repository root.
- README.md's licence note and its Quake 4 section point to it.
- The `dude-q4` archive (`tools/quake4/package-q4.sh`) ships it with `COPYING.txt`, and its
  README-QUAKE4.txt says which parts of the library it covers.
- Not in every source file's header: the permission names the code by author, and the git
  history is the record of what that covers.

Background: the GPL FAQ on linking exceptions,
https://www.gnu.org/licenses/gpl-faq.html#GPLIncompatibleLibs - the text follows its
template. Not legal advice.
