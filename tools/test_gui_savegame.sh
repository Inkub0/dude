#!/bin/sh
# Offline test of the savegame GUI state restore (neo/ui, idUserInterfaceLocal::ReadFromSaveGame) on
# real savegames: links the engine's own GUI objects from build/ into tools/tests/gui_savegame_test.cpp
# with stand-in engine interfaces, and restores every GUI record of each savegame against the GUI
# files as a game folder installs them (see the .cpp for what is checked). Never starts the game.
#
# Usage: tools/test_gui_savegame.sh [-q] [-g GAME] [-b GAME_BASE] [-s SAVED_PAK ...] FILE.save ...
#   -q      Quake 4 savegames (with D3BASE the Quake 4 install, -g q4base)
#   -g      the game folder the savegames belong to (fs_game, default base)
#   -b      its fs_game_base (d3xp for The Lost Mission, d3le); base comes last in any case
#   -s      a pak kept in a savegames folder (read for old savegames only), may repeat
# Needs a built build/ (the engine target). D3BASE overrides the game data folder (default: the repo,
# which holds the paks); the repo's own loose files and paks are layered like the search path.
set -e
DIR="$(cd "$(dirname "$0")/.." && pwd)"
D3BASE="${D3BASE:-$DIR}"
WORK="${TMPDIR:-/tmp}/dude-gui-savegame-test"
GAME=base
GAMEBASE=""
SAVED=""
TITLE=""
while getopts "qg:b:s:" opt; do
	case $opt in
		q) TITLE="--quake4" ;;
		g) GAME="$OPTARG" ;;
		b) GAMEBASE="$OPTARG" ;;
		s) SAVED="$SAVED $OPTARG" ;;
		*) exit 2 ;;
	esac
done
shift $((OPTIND - 1))

rm -rf "$WORK/layers" "$WORK/saved" && mkdir -p "$WORK/layers" "$WORK/saved" "$WORK/gen"

# a pak's guis/ folder, its names as the engine reads them (some paks use backslashes)
extract_guis() {
	python3 - "$1" "$2" <<'PY'
import os, sys, zipfile
try:
    z = zipfile.ZipFile(sys.argv[1])
except zipfile.BadZipFile:
    sys.exit(0)
for info in z.infolist():
    name = info.filename.replace("\\", "/")
    if name.lower().startswith("guis/") and not name.endswith("/"):
        dst = os.path.join(sys.argv[2], name)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        open(dst, "wb").write(z.read(info))
PY
}

# the search path of a game folder, highest priority first: the folder's loose files, its paks in
# reverse name order (pak000 last); then base's for an expansion. One layer per pak.
LAYERS=""
add_game() {
	g="$1"
	if [ -d "$D3BASE/$g/guis" ]; then
		ln -s "$D3BASE/$g" "$WORK/layers/$g-loose"		# (a path without spaces)
		LAYERS="$LAYERS --layer $WORK/layers/$g-loose"
	fi
	for name in $(cd "$D3BASE/$g" && ls *.pk4 2>/dev/null | sort -r); do
		out="$WORK/layers/$g-${name%.pk4}"
		extract_guis "$D3BASE/$g/$name" "$out"
		if [ -d "$out/guis" ]; then
			LAYERS="$LAYERS --layer $out"
		fi
	done
}
add_game "$GAME"
if [ -n "$GAMEBASE" ] && [ "$GAMEBASE" != base ]; then
	add_game "$GAMEBASE"
fi
if [ "$GAME" != base ] && [ -z "$TITLE" ]; then
	add_game base
fi

# the table decls a GUI expression may index (idWindow::ParseTerm)
python3 - "$WORK/tables.txt" "$D3BASE" <<'PY'
import glob, re, sys, zipfile
names = set()
for pak in glob.glob(sys.argv[2] + "/*/*.pk4"):
    try:
        z = zipfile.ZipFile(pak)
    except zipfile.BadZipFile:
        continue
    for n in z.namelist():
        if n.lower().startswith("materials/") and n.lower().endswith(".mtr"):
            t = z.read(n).decode("latin1")
            t = re.sub(r"/\*.*?\*/", "", t, flags=re.S)
            t = re.sub(r"//[^\n]*", "", t)
            names.update(m.group(1).lower() for m in re.finditer(r"(?:^|\s)table\s+([A-Za-z_][\w/]*)\s*\{", t))
open(sys.argv[1], "w").write("\n".join(sorted(names)) + "\n")
PY

SAVEDARGS=""
for pak in $SAVED; do
	out="$WORK/saved/$(basename "$pak")"
	extract_guis "$pak" "$out"
	SAVEDARGS="$SAVEDARGS --saved $out"
done

# the interface methods the test doesn't define, from the compiler's list of pure virtuals
CXXFLAGS="-std=gnu++17 -g -O0 -D_DEBUG -msse4.1 -fno-strict-aliasing -fno-math-errno -fno-trapping-math -ffinite-math-only -w \
	-DD3_ARCH=\"x86_64\" -DD3_HAS_SIMD_SSE41=1 -DD3_IS_BIG_ENDIAN=0 -DD3_OSTYPE=\"linux\" -DD3_SIZEOFPTR=8 -D__DOOM_DLL__ -DDHEWM3_VULKAN=1 \
	-I$WORK/gen -I$DIR/build -I$DIR/neo -I/usr/include/SDL2"
for c in idFileSystem idSession idCVarSystem idDeclManager idRenderSystem idSys idCmdSystem idDeclBase; do
	: > "$WORK/gen/gui_savegame_stubs_$c.h"
done
c++ $CXXFLAGS -fdiagnostics-plain-output -fsyntax-only "$DIR/tools/tests/gui_savegame_test.cpp" 2> "$WORK/gen/pure.txt" || true
python3 - "$WORK/gen" <<'PY'
import re, sys
gen = sys.argv[1]
methods = {}
for line in open(gen + "/pure.txt"):
    m = re.search(r"[\u2018']virtual (.+?) (id\w+)::(\w+)\((.*)\)( const)?[\u2019']", line)
    if not m:
        continue
    ret, cls, name, params, const = m.groups()
    params = params.strip().replace("__va_list_tag*", "va_list")
    if ret.endswith("&"):
        body = "{ static std::remove_const<std::remove_reference<%s>::type>::type r; return r; }" % ret
    elif ret == "void":
        body = "{}"
    else:
        body = "{ typedef %s R; return R(); }" % ret
    methods.setdefault(cls, {})["%s %s(%s)%s" % (ret, name, params, const or "")] = body
for cls, ms in methods.items():
    with open("%s/gui_savegame_stubs_%s.h" % (gen, cls), "w") as f:
        for sig, body in ms.items():
            f.write("\tvirtual %s %s\n" % (sig, body))
PY

OBJ="$DIR/build/CMakeFiles/dude.dir"
c++ $CXXFLAGS "$DIR/tools/tests/gui_savegame_test.cpp" "$OBJ"/ui/*.o "$OBJ/framework/File.cpp.o" \
	"$DIR/build/libidlib.a" -lpthread -o "$WORK/gui_savegame_test"

$RUNNER "$WORK/gui_savegame_test" --tables "$WORK/tables.txt" $TITLE $LAYERS $SAVEDARGS "$@"
