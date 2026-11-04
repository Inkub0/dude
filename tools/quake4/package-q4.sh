#!/bin/sh
# Packages the Quake 4 game library as its own release archive, apart from DUDE's
# (docs/quake4-release-licensing.md, docs/releasing.md):
#
#   dist/release-<ver>/dude-q4-<ver>-<platform>.{tar.gz,zip}
#     q4base.so | q4base.dll          built from the dude-q4 repository against DUDE <ver>
#     EULA.Development Kit.rtf         the Quake 4 SDK licence, byte for byte as Raven shipped it
#     README-QUAKE4.txt                what this is, its terms, where the source is
#     BUILDINFO.txt                    the exact DUDE and dude-q4 commits it was built from
#
# DUDE's own archives stay GPL-only and never carry these files; this one carries no DUDE (GPL)
# file except inside q4base itself, whose GPL parts are DUDE's idlib at the commit in BUILDINFO.
#
# Usage:  tools/quake4/package-q4.sh <version> <linux-x86_64|win64> <path to q4base.so|.dll>
# Env:    Q4GAME_DIR  the game repository (default ~/dude-quake4-game)
#         Q4DIR       a retail Quake 4 install for the no-retail-data gate (required; set
#                     Q4DIR=none to skip it, which a real release must not do)
#         ALLOW_DIRTY=1  package from an uncommitted game repository (test runs only)
#         OUT_DIR     stage somewhere else than dist/release-<version> (test runs)
set -e
DUDE="$(cd "$(dirname "$0")/../.." && pwd)"
VER="$1"; PLAT="$2"; LIB="$3"
Q4GAME_DIR="${Q4GAME_DIR:-$HOME/dude-quake4-game}"
EULA="EULA.Development Kit.rtf"

die() { echo "package-q4.sh: $*" >&2; exit 1; }
[ -n "$VER" ] && [ -n "$PLAT" ] && [ -n "$LIB" ] || die "usage: package-q4.sh <version> <linux-x86_64|win64> <q4base library>"
case "$PLAT" in
	linux-x86_64) LIBNAME=q4base.so ;;
	win64)        LIBNAME=q4base.dll ;;
	*)            die "platform must be linux-x86_64 or win64" ;;
esac
[ -f "$LIB" ] || die "no game library at $LIB"
[ "$(basename "$LIB")" = "$LIBNAME" ] || die "$PLAT wants $LIBNAME, got $(basename "$LIB")"
[ -n "$Q4DIR" ] || die "set Q4DIR to a retail Quake 4 install (the no-retail-data gate needs it)"
[ -d "$Q4GAME_DIR/.git" ] || die "no game repository at $Q4GAME_DIR"

g() { git -C "$Q4GAME_DIR" "$@"; }

# --- the game library: it is a game library
if [ "$PLAT" = linux-x86_64 ]; then
	nm -D --defined-only "$LIB" | grep -q ' GetGameAPI$' || die "$LIB exports no GetGameAPI"
else
	x86_64-w64-mingw32-objdump -p "$LIB" | grep -qE '\] +[0-9a-f]+ +GetGameAPI$' || die "$LIB exports no GetGameAPI"
	# the C++ runtime is linked in, as in DUDE's own game DLLs: nothing the archive lacks
	if x86_64-w64-mingw32-objdump -p "$LIB" | grep -qE 'DLL Name: (libstdc\+\+|libgcc)'; then
		die "$LIB needs libstdc++/libgcc DLLs - build it with tools/quake4/build-q4game.sh (static C++ runtime)"
	fi
fi

# --- the game repository: committed, licence file pristine, no notice removed
ROOT="$(g rev-list --max-parents=0 HEAD)"
if [ -n "$(g status --porcelain -- source CMakeLists.txt README.md "$EULA")" ]; then
	[ "$ALLOW_DIRTY" = 1 ] || die "game repository has uncommitted changes (ALLOW_DIRTY=1 for a test run)"
	echo "package-q4.sh: WARNING game repository is dirty - not a release" >&2
fi
g diff --quiet "$ROOT" HEAD -- "$EULA" || die "'$EULA' differs from the SDK import ($ROOT)"
REMOVED="$(g diff "$ROOT" HEAD -- source | grep -E '^-[^-].*(Copyright|\(c\)|id Software|Raven Software)' || true)"
[ -z "$REMOVED" ] || { echo "$REMOVED" >&2; die "SDK copyright notices were removed since the import (EULA 3(l))"; }

# --- what the library was built from: build-q4game.sh writes it next to the library
STAMP="$(dirname "$LIB")/q4base.buildinfo"
[ -f "$STAMP" ] || die "no $STAMP - build the library with tools/quake4/build-q4game.sh"
if [ "$LIB" -nt "$STAMP" ]; then
	die "$STAMP is older than $LIB - rebuild with build-q4game.sh"
fi
if grep -q 'uncommitted' "$STAMP"; then
	[ "$ALLOW_DIRTY" = 1 ] || die "the library was built from uncommitted sources ($STAMP)"
	echo "package-q4.sh: WARNING built from uncommitted sources - not a release" >&2
fi
grep -q "^DUDE commit: *$(git -C "$DUDE" rev-parse HEAD)" "$STAMP" ||
	echo "package-q4.sh: WARNING the library was built from another DUDE commit than HEAD (see $STAMP)" >&2

# --- versions: both repositories should carry the release tag (docs/releasing.md)
TAG="v$VER"
for repo in "$DUDE" "$Q4GAME_DIR"; do
	if git -C "$repo" rev-parse -q --verify "refs/tags/$TAG" >/dev/null; then
		[ "$(git -C "$repo" rev-parse "$TAG^{commit}")" = "$(git -C "$repo" rev-parse HEAD)" ] ||
			echo "package-q4.sh: WARNING $repo: tag $TAG is not HEAD" >&2
	else
		echo "package-q4.sh: WARNING $repo has no tag $TAG yet - tag both before publishing" >&2
	fi
done

# --- stage
OUT="${OUT_DIR:-$DUDE/dist/release-$VER}"
NAME="dude-q4-$VER-$PLAT"
STAGE="$OUT/$NAME"
rm -rf "$STAGE"
mkdir -p "$STAGE"
cp "$LIB" "$STAGE/$LIBNAME"
cp "$Q4GAME_DIR/$EULA" "$STAGE/$EULA"

if [ "$PLAT" = win64 ]; then EXE=dude.exe; else EXE=./dude; fi
cat > "$STAGE/README-QUAKE4.txt" <<EOF
DUDE $VER - Quake 4 game library
================================

This archive adds Quake 4 to DUDE $VER.

You need your own copy of the full retail Quake 4 (Steam, GOG or disc). The Quake 4 demo is
not supported: DUDE refuses it, because the licence below lets this library run with the
full version of Quake 4 only. No Quake 4 game data is included, and DUDE never copies any:
it reads your installed copy where it is.

Setup: extract DUDE $VER and this archive into your Quake 4 folder (the one that contains
q4base/), so that $LIBNAME sits next to the DUDE executable, and start DUDE there. From
another folder, start it with

    $EXE +set fs_basepath "/path/to/Quake 4"

If the folder holds Doom 3's game data as well, add +set com_title quake4.

Licence - please read
---------------------
$LIBNAME is built from Raven Software's Quake 4 SDK game code. It is NOT under the GPL: it is
under the Quake 4 SDK licence, "$EULA", included here and binding on anyone
who uses or passes on this file. In short: non-commercial use only; you may give copies away
free of charge, never sell them or use them for any commercial purpose, and the licence file
must go with every copy; it works only with the full version of Quake 4.

DUDE itself (the engine, in DUDE's own archive) is GPLv3. The two are separate programs that
meet only through the game library interface (GetGameAPI); nothing of the SDK is in DUDE.
$LIBNAME also contains parts of DUDE's idlib (GPLv3), built from the DUDE commit in
BUILDINFO.txt; DUDE's source for it is at https://github.com/Inkub0/dude (tag $TAG).

Source of this library: https://github.com/Inkub0/dude-q4 (tag $TAG).

Quake and Quake 4 are trademarks of their owners (id Software / ZeniMax Media).
DUDE is an independent, non-commercial project and is not affiliated with, endorsed or
sponsored by id Software, ZeniMax Media, Microsoft, Raven Software or Activision.
EOF

{
	echo "dude-q4 $VER ($PLAT)"
	echo "SDK import:      $ROOT"
	echo "packaged:        $(date -u '+%Y-%m-%d %H:%M UTC')"
	cat "$STAMP"
} > "$STAGE/BUILDINFO.txt"

# --- archive
cd "$OUT"
if [ "$PLAT" = win64 ]; then
	rm -f "$NAME.zip"
	zip -qr "$NAME.zip" "$NAME"
	ARCHIVE="$OUT/$NAME.zip"
else
	tar -czf "$NAME.tar.gz" "$NAME"
	ARCHIVE="$OUT/$NAME.tar.gz"
fi

# --- gate: nothing from the retail game in it
if [ "$Q4DIR" = none ]; then
	echo "package-q4.sh: WARNING retail-data gate skipped (Q4DIR=none) - not a release" >&2
else
	python3 -I "$DUDE/tools/quake4/check_no_retail_data.py" --retail "$Q4DIR" "$ARCHIVE" ||
		die "retail Quake 4 data in $ARCHIVE"
fi
echo "package-q4.sh: $ARCHIVE"
