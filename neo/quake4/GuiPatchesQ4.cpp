/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.h for license header)
===========================================================================
*/

// DUDE: DUDE's own additions to Quake 4's GUI files (quake4/GuiPatchesQ4.h).
//
// A patch is a list of operations on one retail file, in a small text form read here:
//
//   insert <windowName> { <text> }
//       adds the text at the end of the retail window of that name: windowDefs become its last
//       children (they draw over its own and take the mouse first), properties override its own
//       (a later rect, matscalex, noclip... replaces the retail one)
//   material <name> { <stages> }
//       a material made in memory before the file is parsed, for the windows the patch adds
//
// Every windowDef and material here is DUDE's own, written for this patch; the retail windows
// they change are only named, the retail textures only referred to by path.

#include "sys/platform.h"
#include "framework/Common.h"
#include "framework/DeclManager.h"
#include "framework/GameTitle.h"
#include "idlib/containers/StrList.h"
#include "renderer/RenderSystem.h"
#include "ui/UserInterface.h"

#include "quake4/GuiPatchesQ4.h"

#include <ctype.h>

namespace {

/*
The main menu.

DUDE Settings button, under the bottom row (Mods, Updates, Credits, Exit). It looks like the main
entries (New Game, Load Game...): their font size, colour and hover, without the corner mark,
right-aligned clear of the frame's corner. A label in p_main, which slides away with the main
screen, and the mouse area in p_btns over it, which is where the main screen's buttons take the
mouse (p_btns covers the whole screen, above p_main). The mouse area is shown whenever the row's
Exit button is, which is on the main screen in every state of the menu (from the desktop, in a
game, in multiplayer).

Wide screens. The menu stays scaled to 4:3 at the centre (r_scaleMenusTo43); its frame and
backdrop carry on into the sides instead of stopping at the 4:3 edges. The desktop and p_mainmenu,
which holds them, no longer clip their children to 0..640 (the screens' panels inside it still
clip their own, as before), and each layer goes on 1000 units past both edges (to about 32:9):
  - screen, screen2: a gradient exactly the same across its width - widened
  - grad: a gradient whose columns differ a little (lighter edges) - kept as it is, carried on
    by its edge columns (materials dude/q4menu/darkgrad_...), as the bars below
  - static: the scrolling noise, a repeating texture - widened with its texture repeated at the
    same scale (matscalex -3 over three widths, as -1 over one)
  - grid1, grid2: the grid of crosses - more tiles of it, never stretched
  - topbar, btmbar: the black masks that cut the frame's top and bottom edges; both ends of
    their texture are one column repeated, so the mask carries on as that column (materials
    dude/q4menu/...: the texture with s pinned to 0 or 1). Children, so they slide with the bars
    between the main screen's place and the other screens'.
  - grad4..grad7: masks that darken the upper left during screen changes, drawn mirrored; they
    reach the left edge, so they are widened leftwards with the texture's scale grown to match
    (the old 640 units keep the old mapping, the new ones sample past the edge, clamped: its
    edge column). grad2 and grad3 never reach an edge: unchanged.
  - grad8, fade: plain colour - widened
  - videoBlack: the black under the intro videos (id, Raven, Bethesda logos), shown for the
    whole intro and faded out after it - widened (p_video unclipped), so the menu does not show
    beside the 4:3 videos
  - video_bethsoft: the Bethesda video is 16:9 (512x288, square pixels: its logo's square mark
    is square), squeezed into the 4:3 video rect by the retail menu. With room beside the menu
    (gui::dude_sideBars, quake4/SettingsQ4.cpp) it is drawn 16:9 instead, centred - the whole
    of a 16:9 screen; on a wider one the sides beyond it are its white (its edges are 253 of 255
    for nearly all of it), shown and hidden with it, Esc too. Its first column is a grey codec
    artifact: covered (two texels, 2.5 units: the filtering between it and the next) when drawn
    16:9. A 4:3 or stretched screen shows it as vanilla.
The scene pictures (bg_temp..bg_temp4) and the intro videos are kept 4:3, never stretched.
On a 4:3 screen (or with r_scaleMenusTo43 0) the menu looks as vanilla: everything added lies
outside 0..640, and every retail layer changed keeps its texture mapping inside 0..640.
*/
const char *MAINMENU_PATCH = R"DUDE(
insert p_main {
	windowDef dude_settings_text
	{
		rect	200,427,165,23
		visible	1
		forecolor	1,1,1,0.8
		text	"DUDE SETTINGS"
		textscale	0.33
		textalign	2
		font	"fonts/marine"
	}
}

insert p_btns {
	windowDef dude_settings_btn
	{
		rect	200,427,165,26
		visible	("main_b_exit::visible" == 1)

		onMouseEnter {
			if ("desktop::active" == 0) {
				stoptransitions "dude_settings_btn" ;
				transition "dude_settings_text::forecolor" "$desktop::white_8" "$desktop::orange" "0" ;
				set "dude_settings_text::textscale" "0.34" ;
				set "cmd" "play main_menu_mouseover" ;
			}
		}

		onMouseExit {
			if ("desktop::active" == 0) {
				transition "dude_settings_text::forecolor" "$desktop::orange" "$desktop::white_8" "300" ;
				transition "dude_settings_text::textscale" "0.34" "0.33" "300" ;
			}
		}

		onAction {
			if ("desktop::active" == 0) {
				set "cmd" "play main_menu_selection ; exec dudeSettings" ;
			}
		}
	}
}

insert Desktop {
	noclip	1
}
insert p_mainmenu {
	noclip	1
}
insert static {
	rect	-640,0,1920,480
	matscalex	-3
}
insert screen {
	rect	-1000,24,2640,480
}
insert screen2 {
	rect	-1000,294,2640,349
}
insert grad4 {
	rect	-1000,0,1640,480
	matscalex	-2.5625
}
insert grad5 {
	rect	-1000,0,1640,750
	matscalex	-2.5625
}
insert grad6 {
	rect	-1000,0,1640,548
	matscalex	-2.5625
}
insert grad7 {
	rect	-1000,0,1640,574
	matscalex	-2.5625
}
insert grad8 {
	rect	-1000,0,2640,480
}
insert grid1 {
	noclip	1
	windowDef dude_grid_l1
	{
		rect	-480,0,480,480
		background	"gfx/guis/mainmenu/bg_grid"
		matcolor	1,1,1,0.04
	}
	windowDef dude_grid_l2
	{
		rect	-960,0,480,480
		background	"gfx/guis/mainmenu/bg_grid"
		matcolor	1,1,1,0.04
	}
}
insert grid2 {
	noclip	1
	windowDef dude_grid_r1
	{
		rect	480,0,480,480
		background	"gfx/guis/mainmenu/bg_grid"
		matcolor	1,1,1,0.04
	}
}

material dude/q4menu/topbar_left {
	{
		blend	blend
		colored
		map		"gfx/guis/mainmenu/topbar"
		clamp
		scale	0, 1
	}
}
material dude/q4menu/topbar_right {
	{
		blend	blend
		colored
		map		"gfx/guis/mainmenu/topbar"
		clamp
		translate	1, 0
		scale	0, 1
	}
}
material dude/q4menu/btmbar_left {
	{
		blend	blend
		colored
		map		"gfx/guis/mainmenu/btmbar"
		clamp
		scale	0, 1
	}
}
material dude/q4menu/btmbar_right {
	{
		blend	blend
		colored
		map		"gfx/guis/mainmenu/btmbar"
		clamp
		translate	1, 0
		scale	0, 1
	}
}
material dude/q4menu/darkgrad_left {
	{
		blend	blend
		colored
		map		"gfx/guis/mainmenu/bg_darkgrad"
		clamp
		scale	0, 1
	}
}
material dude/q4menu/darkgrad_right {
	{
		blend	blend
		colored
		map		"gfx/guis/mainmenu/bg_darkgrad"
		clamp
		translate	1, 0
		scale	0, 1
	}
}
insert grad {
	noclip	1
	windowDef dude_grad_l
	{
		rect	-1000,0,1000,480
		background	"dude/q4menu/darkgrad_left"
		matcolor	0,0,0,1
	}
	windowDef dude_grad_r
	{
		rect	640,0,1000,480
		background	"dude/q4menu/darkgrad_right"
		matcolor	0,0,0,1
	}
}
insert topbar {
	noclip	1
	windowDef dude_topbar_l
	{
		rect	-1000,0,1000,129
		background	"dude/q4menu/topbar_left"
		matcolor	1,1,1,1
	}
	windowDef dude_topbar_r
	{
		rect	1045,0,1000,129
		background	"dude/q4menu/topbar_right"
		matcolor	1,1,1,1
	}
}
insert btmbar {
	noclip	1
	windowDef dude_btmbar_l
	{
		rect	-1000,0,1000,129
		background	"dude/q4menu/btmbar_left"
		matcolor	1,1,1,1
	}
	windowDef dude_btmbar_r
	{
		rect	1045,0,1000,129
		background	"dude/q4menu/btmbar_right"
		matcolor	1,1,1,1
	}
}
insert fade {
	rect	-1000,0,2640,480
}
insert p_video {
	noclip	1
}
insert videoBlack {
	rect	-1000,0,2640,480
}
insert video_bethsoft {
	rect	(0 - "gui::dude_sideBars" * 106.667), 0, (640 + "gui::dude_sideBars" * 213.333), 479
}
insert p_video {
	windowDef dude_bethsoft_l
	{
		rect	-1000,0,893.333,480
		visible	("video_bethsoft::visible" == 1)
		backcolor	0.992,0.992,0.992,1
	}
	windowDef dude_bethsoft_r
	{
		rect	746.667,0,1000,480
		visible	("video_bethsoft::visible" == 1)
		backcolor	0.992,0.992,0.992,1
	}
	windowDef dude_bethsoft_edge
	{
		rect	-106.667,0,2.5,479
		visible	("video_bethsoft::visible" == 1 && "gui::dude_sideBars" == 1)
		backcolor	0.992,0.992,0.992,1
	}
}
)DUDE";

/*
The map loading screens (guis/loading/generic.gui - every campaign map, its picture from the
mapDef's loadimage - and intro.gui). On a wide screen with the screen fitted to 4:3
(gui::dude_horPad > 0, the room beside the 4:3 area in GUI units, Q4Gui_SetWidescreenVars) the
level's picture fills the width instead: scaled up evenly, centred, its top and bottom cropped,
never stretched. The frame carries on into the sides as on the main menu: the desktop stops
clipping to 0..640, the dark masks along the top and bottom (load_*_edge, and their green tint
load_*_edgeadd) carry on their end columns (materials dude/q4load/...), and the grid is tiled. The
row of dots moves to the screen's left edge, the level name and the loading bar (with its
"Loading" / "click to continue" text, right-aligned over it) to the right edge; the corner
marks stay where they are. With dude_horPad 0 (a 4:3 screen, or stretched) every rect is the retail one:
the loading screen as vanilla. guis/loading/mplevel.gui (multiplayer, not implemented) is left
as it is.
*/
#define LOADING_EDGE_MATERIAL( name, image, blendMode, translate ) \
	"material dude/q4load/" name " {\n\t{\n\t\tblend " blendMode "\n\t\tcolored\n\t\tmap \"gfx/guis/mainmenu/" image "\"\n\t\tclamp\n" translate "\t\tscale 0, 1\n\t}\n}\n"
#define LOADING_EDGE_MATERIALS( image, blendMode ) \
	LOADING_EDGE_MATERIAL( image "_l", image, blendMode, "" ) \
	LOADING_EDGE_MATERIAL( image "_r", image, blendMode, "\t\ttranslate 1, 0\n" )
#define LOADING_EDGE_EXTEND( window, image, height, color ) \
	"insert " window " {\n\tnoclip 1\n" \
	"\twindowDef dude_" window "_l {\n\t\trect -1000,0,1000," height "\n\t\tbackground \"dude/q4load/" image "_l\"\n\t\tmatcolor " color "\n\t}\n" \
	"\twindowDef dude_" window "_r {\n\t\trect 640,0,1000," height "\n\t\tbackground \"dude/q4load/" image "_r\"\n\t\tmatcolor " color "\n\t}\n}\n"

#define LOADING_FRAME_PATCH \
	LOADING_EDGE_MATERIALS( "load_top_edge", "blend" ) \
	LOADING_EDGE_MATERIALS( "load_btm_edge", "blend" ) \
	LOADING_EDGE_MATERIALS( "load_top_edgeadd", "add" ) \
	LOADING_EDGE_MATERIALS( "load_btm_edgeadd", "add" ) \
	LOADING_EDGE_EXTEND( "top_edge", "load_top_edge", "119", "0,0,0,0.8" ) \
	LOADING_EDGE_EXTEND( "btm_edge", "load_btm_edge", "253", "0,0,0,0.8" ) \
	LOADING_EDGE_EXTEND( "top_edgeadd", "load_top_edgeadd", "119", "0.0941,0.113,0.0392,1" ) \
	LOADING_EDGE_EXTEND( "btm_edgeadd", "load_btm_edgeadd", "253", "0.0941,0.113,0.0392,1" ) \
	R"DUDE(
insert Desktop {
	noclip	1
}
insert loadimg {
	rect	(0 - "gui::dude_horPad"), (240 - (320 + "gui::dude_horPad") * 0.75), (640 + 2 * "gui::dude_horPad"), ((640 + 2 * "gui::dude_horPad") * 0.75)
}
insert load_dots {
	rect	(3 - "gui::dude_horPad"), 2, 105, 25
}
insert button {
	rect	-1000,0,2640,480
}
insert bar {
	rect	(235 + "gui::dude_horPad"), 431, 405, 26
}
insert p_load_bar {
	rect	(235 + "gui::dude_horPad"), 431, ("gui::map_loading" * 405), 26
}
insert bar_full {
	rect	(235 + "gui::dude_horPad"), 431, 405, 26
}
insert load_text {
	rect	(4 + "gui::dude_horPad"), 428, 624, 36
}
)DUDE"

#define LOADING_NAME_PATCH R"DUDE(
insert t_levelname_grad {
	rect	(291 + "gui::dude_horPad"), 14, 349, 22
}
insert t_levelname {
	rect	(290 + "gui::dude_horPad"), 11, 336, 20
}
)DUDE"

#define LOADING_GRID_PATCH R"DUDE(
insert grid1 {
	noclip	1
	windowDef dude_grid_l1
	{
		rect	-480,0,480,480
		background	"gfx/guis/mainmenu/bg_grid"
		matcolor	1,1,1,0.04
	}
	windowDef dude_grid_l2
	{
		rect	-960,0,480,480
		background	"gfx/guis/mainmenu/bg_grid"
		matcolor	1,1,1,0.04
	}
}
insert grid2 {
	noclip	1
	windowDef dude_grid_r1
	{
		rect	480,0,480,480
		background	"gfx/guis/mainmenu/bg_grid"
		matcolor	1,1,1,0.04
	}
}
)DUDE"

struct guiPatch_t {
	const char *	gui;
	const char *	patch;
};

const guiPatch_t q4GuiPatches[] = {
	{ "guis/mainmenu.gui", MAINMENU_PATCH },
	{ "guis/loading/generic.gui", LOADING_FRAME_PATCH LOADING_GRID_PATCH LOADING_NAME_PATCH },
	{ "guis/loading/intro.gui", LOADING_FRAME_PATCH },
};

/*
A token scan of GUI text, enough to find windows and their braces: names, quoted strings (with
their escapes) and single characters, skipping white space and comments.
*/
class guiScan_t {
public:
	guiScan_t( const char *text, int length ) : s( text ), len( length ), pos( 0 ), start( 0 ) {}

	// the next token; false at the end of the text
	bool Next( void ) {
		SkipBlank();
		if ( pos >= len ) {
			return false;
		}
		start = pos;
		if ( s[pos] == '"' ) {
			pos++;
			while ( pos < len && s[pos] != '"' ) {
				if ( s[pos] == '\\' && pos + 1 < len ) {
					pos++;
				}
				pos++;
			}
			pos = Min( pos + 1, len );
		} else if ( IsNameChar( s[pos] ) ) {
			while ( pos < len && IsNameChar( s[pos] ) ) {
				pos++;
			}
		} else {
			pos++;
		}
		return true;
	}

	bool Is( const char *str ) const {
		const int n = idStr::Length( str );
		return pos - start == n && idStr::Icmpn( s + start, str, n ) == 0;
	}
	bool IsName( void ) const { return IsNameChar( s[start] ); }
	// a window type's keyword: windowDef, choiceDef, listDef...
	bool IsWindowKeyword( void ) const {
		return IsName() && pos - start > 3 && idStr::Icmpn( s + pos - 3, "Def", 3 ) == 0;
	}

	// past the '}' that closes the block whose '{' was the last token read; Start() is then that '}'
	bool SkipBlock( void ) {
		int depth = 1;
		while ( Next() ) {
			if ( Is( "{" ) ) {
				depth++;
			} else if ( Is( "}" ) && --depth == 0 ) {
				return true;
			}
		}
		return false;
	}

	int		Start( void ) const { return start; }
	int		End( void ) const { return pos; }

private:
	static bool IsNameChar( char c ) { return isalnum( (unsigned char)c ) || c == '_'; }

	void SkipBlank( void ) {
		while ( pos < len ) {
			if ( (unsigned char)s[pos] <= ' ' ) {
				pos++;
			} else if ( s[pos] == '/' && pos + 1 < len && s[pos + 1] == '/' ) {
				while ( pos < len && s[pos] != '\n' ) {
					pos++;
				}
			} else if ( s[pos] == '/' && pos + 1 < len && s[pos + 1] == '*' ) {
				pos += 2;
				while ( pos + 1 < len && !( s[pos] == '*' && s[pos + 1] == '/' ) ) {
					pos++;
				}
				pos = Min( pos + 2, len );
			} else {
				break;
			}
		}
	}

	const char *	s;
	int				len;
	int				pos;
	int				start;
};

/*
Where the window windowName's closing '}' is in text, -1 when no window has that name.
*/
int FindWindowClose( const idStr &text, const char *windowName ) {
	guiScan_t scan( text.c_str(), text.Length() );
	while ( scan.Next() ) {
		if ( !scan.IsWindowKeyword() ) {
			continue;
		}
		if ( !scan.Next() || !scan.Is( windowName ) ) {
			continue;
		}
		if ( !scan.Next() || !scan.Is( "{" ) ) {
			continue;
		}
		return scan.SkipBlock() ? scan.Start() : -1;
	}
	return -1;
}

/*
Applies the patch to text; false (text then as far as it got) when the patch could not be read or
named a window the file does not have. materials: the patch's materials, name then text.
*/
bool ApplyPatch( const char *qpath, const char *patch, idStr &text, idStrList &materials ) {
	guiScan_t scan( patch, idStr::Length( patch ) );
	while ( scan.Next() ) {
		const bool isMaterial = scan.Is( "material" );
		if ( !isMaterial && !scan.Is( "insert" ) ) {
			common->Warning( "DUDE GUI patch for %s: unknown operation at offset %d", qpath, scan.Start() );
			return false;
		}
		if ( !scan.Next() || !scan.IsName() ) {
			common->Warning( "DUDE GUI patch for %s: an operation without a name", qpath );
			return false;
		}
		// a name: a window's, or a material's path (names and '/' between them)
		const int nameStart = scan.Start();
		int nameEnd = scan.End();
		while ( scan.Next() && !scan.Is( "{" ) ) {
			nameEnd = scan.End();
		}
		const idStr name( patch + nameStart, 0, nameEnd - nameStart );
		if ( !scan.Is( "{" ) ) {
			common->Warning( "DUDE GUI patch for %s: %s without a '{'", qpath, name.c_str() );
			return false;
		}
		const int bodyStart = scan.End();
		if ( !scan.SkipBlock() ) {
			common->Warning( "DUDE GUI patch for %s: %s has no closing '}'", qpath, name.c_str() );
			return false;
		}
		const idStr body( patch + bodyStart, 0, scan.Start() - bodyStart );

		if ( isMaterial ) {
			materials.Append( name );
			materials.Append( body );
			continue;
		}
		const idStr &windowName = name;

		const int at = FindWindowClose( text, windowName );
		if ( at < 0 ) {
			common->Warning( "DUDE GUI patch for %s: the file has no window '%s' (a GUI mod?), left out",
				qpath, windowName.c_str() );
			return false;
		}
		const idStr insertion = idStr( "\n// DUDE (quake4/GuiPatchesQ4.cpp)\n" ) + body + "\n";
		text.Insert( insertion.c_str(), at );
	}
	return true;
}

/*
A material of the patch, made in memory the first time (neither title ships it; the same way as
R_ForcedAmbientMaterial). Before the GUI is parsed: its windows look their backgrounds up as they
are read.
*/
void DefineMaterial( const idStr &name, const idStr &stages ) {
	if ( declManager->FindType( DECL_MATERIAL, name, false ) != NULL ) {
		return;
	}
	auto *decl = declManager->CreateNewDecl( DECL_MATERIAL, name, "materials/_dude_engine.mtr" );
	const idStr text = idStr( "material " ) + name + "\n{\n" + stages + "\n}\n";
	decl->SetText( text.c_str() );
	// CreateNewDecl parsed its default text already: parse this one instead
	decl->Invalidate();
}

// the patch for the GUI file qpath, NULL when there is none (or the title is not Quake 4)
const char *FindPatch( const char *qpath ) {
	if ( qpath == NULL || GameTitle::Get().title != GAME_TITLE_QUAKE4 ) {
		return NULL;
	}
	for ( size_t i = 0; i < sizeof( q4GuiPatches ) / sizeof( q4GuiPatches[0] ); i++ ) {
		if ( idStr::Icmp( qpath, q4GuiPatches[i].gui ) == 0 ) {
			return q4GuiPatches[i].patch;
		}
	}
	return NULL;
}

} // namespace

/*
================
Q4Gui_HasPatch
================
*/
bool Q4Gui_HasPatch( const char *qpath ) {
	return FindPatch( qpath ) != NULL;
}

/*
================
Q4Gui_PatchSource
================
*/
bool Q4Gui_PatchSource( const char *qpath, idStr &text ) {
	const char *patch = FindPatch( qpath );
	if ( patch == NULL ) {
		return false;
	}
	// all or nothing: a patch half applied could leave a mouse area without its label
	idStr patched = text;
	idStrList materials;
	if ( !ApplyPatch( qpath, patch, patched, materials ) ) {
		return false;
	}
	for ( int i = 0; i + 1 < materials.Num(); i += 2 ) {
		DefineMaterial( materials[i], materials[i + 1] );
	}
	text = patched;
	return true;
}

/*
================
Q4Gui_SetWidescreenVars
================
*/
void Q4Gui_SetWidescreenVars( idUserInterface *gui, bool scaledTo43 ) {
	if ( gui == NULL ) {
		return;
	}
	// idDeviceContext::SetMenuScaleFix fits to 4:3 only above this aspect (below it, stretched)
	const int screenWidth = renderSystem->GetScreenWidth(), screenHeight = renderSystem->GetScreenHeight();
	const float aspect = screenHeight > 0 ? (float)screenWidth / (float)screenHeight : 0.0f;
	const bool sideBars = scaledTo43 && aspect > 1.4f;
	gui->SetStateInt( "dude_sideBars", sideBars ? 1 : 0 );
	// the screen is 480 GUI units high, so 480 * aspect wide: the room on each side of 0..640
	gui->SetStateFloat( "dude_horPad", sideBars ? 240.0f * aspect - 320.0f : 0.0f );
}
