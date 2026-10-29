/*
===========================================================================

Doom 3 GPL Source Code
Copyright (C) 1999-2011 id Software LLC, a ZeniMax Media company.

This file is part of the Doom 3 GPL Source Code ("Doom 3 Source Code").

Doom 3 Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

#include "sys/platform.h"
#include "framework/FileSystem.h"
#include "framework/DemoFile.h"
#include "framework/KeyInput.h"
#include "ui/ListGUILocal.h"
#include "ui/DeviceContext.h"
#include "ui/Window.h"

#include "ui/UserInterfaceLocal.h"
#include "framework/GameTitle.h"

#include "renderer/tr_local.h" // glConfig for winWidth/winHeight

extern idCVar r_skipGuiShaders;		// 1 = don't render any gui elements on surfaces
extern idCVar r_scaleMenusTo43; // DG: for the "scale menus to 4:3" hack

idUserInterfaceManagerLocal	uiManagerLocal;
idUserInterfaceManager *	uiManager = &uiManagerLocal;

/*
===============================================================================

	idUserInterfaceManagerLocal

===============================================================================
*/

void idUserInterfaceManagerLocal::Init() {
	screenRect = idRectangle(0, 0, 640, 480);
	dc.Init();
}

void idUserInterfaceManagerLocal::Shutdown() {
	guis.DeleteContents( true );
	demoGuis.DeleteContents( true );
	icons.Clear();
	dc.Shutdown();
}

void idUserInterfaceManagerLocal::Touch( const char *name ) {
	idUserInterface *gui = Alloc();
	gui->InitFromFile( name );
//	delete gui;
}

void idUserInterfaceManagerLocal::WritePrecacheCommands( idFile *f ) {

	int c = guis.Num();
	for( int i = 0; i < c; i++ ) {
		char	str[1024];
		sprintf( str, "touchGui %s\n", guis[i]->Name() );
		common->Printf( "%s", str );
		f->Printf( "%s", str );
	}
}

void idUserInterfaceManagerLocal::SetSize( float width, float height ) {
	dc.SetSize( width, height );
}

void idUserInterfaceManagerLocal::BeginLevelLoad() {
	int c = guis.Num();
	for ( int i = 0; i < c; i++ ) {
		if ( (guis[ i ]->GetDesktop()->GetFlags() & WIN_MENUGUI) == 0 ) {
			guis[ i ]->ClearRefs();
			/*
			delete guis[ i ];
			guis.RemoveIndex( i );
			i--; c--;
			*/
		}
	}
}

void idUserInterfaceManagerLocal::EndLevelLoad() {
	int c = guis.Num();
	for ( int i = 0; i < c; i++ ) {
		if ( guis[i]->GetRefs() == 0 ) {
			//common->Printf( "purging %s.\n", guis[i]->GetSourceFile() );

			// use this to make sure no materials still reference this gui
			bool remove = true;
			for ( int j = 0; j < declManager->GetNumDecls( DECL_MATERIAL ); j++ ) {
				const idMaterial *material = static_cast<const idMaterial *>(declManager->DeclByIndex( DECL_MATERIAL, j, false ));
				if ( material->GlobalGui() == guis[i] ) {
					remove = false;
					break;
				}
			}
			if ( remove ) {
				delete guis[ i ];
				guis.RemoveIndex( i );
				i--; c--;
			}
		}
	}

	// DG: this should probably be reset at this point
	Sys_SetInteractiveIngameGuiActive( false, NULL );
}

void idUserInterfaceManagerLocal::Reload( bool all ) {
	ID_TIME_T ts;

	int c = guis.Num();
	for ( int i = 0; i < c; i++ ) {
		if ( !all ) {
			fileSystem->ReadFile( guis[i]->GetSourceFile(), NULL, &ts );
			if ( ts <= guis[i]->GetTimeStamp() ) {
				continue;
			}
		}

		guis[i]->InitFromFile( guis[i]->GetSourceFile() );
		common->Printf( "reloading %s.\n", guis[i]->GetSourceFile() );
	}
}

void idUserInterfaceManagerLocal::ListGuis() const {
	int c = guis.Num();
	common->Printf( "\n   size   refs   name\n" );
	size_t total = 0;
	int copies = 0;
	int unique = 0;
	for ( int i = 0; i < c; i++ ) {
		idUserInterfaceLocal *gui = guis[i];
		size_t sz = gui->Size();
		bool isUnique = guis[i]->interactive;
		if ( isUnique ) {
			unique++;
		} else {
			copies++;
		}
		common->Printf( "%6.1fk %4i (%s) %s ( %i transitions )\n", sz / 1024.0f, guis[i]->GetRefs(), isUnique ? "unique" : "copy", guis[i]->GetSourceFile(), guis[i]->desktop->NumTransitions() );
		total += sz;
	}
	common->Printf( "===========\n  %i total Guis ( %i copies, %i unique ), %.2f total Mbytes", c, copies, unique, total / ( 1024.0f * 1024.0f ) );
}

bool idUserInterfaceManagerLocal::CheckGui( const char *qpath ) const {
	idFile *file = fileSystem->OpenFileRead( qpath );
	if ( file ) {
		fileSystem->CloseFile( file );
		return true;
	}
	return false;
}

idUserInterface *idUserInterfaceManagerLocal::Alloc( void ) const {
	return new idUserInterfaceLocal();
}

void idUserInterfaceManagerLocal::DeAlloc( idUserInterface *gui ) {
	if ( gui ) {
		int c = guis.Num();
		for ( int i = 0; i < c; i++ ) {
			if ( guis[i] == gui ) {
				delete guis[i];
				guis.RemoveIndex( i );
				return;
			}
		}
	}
}

idUserInterface *idUserInterfaceManagerLocal::FindGui( const char *qpath, bool autoLoad, bool needUnique, bool forceNOTUnique ) {
	int c = guis.Num();

	for ( int i = 0; i < c; i++ ) {
		if ( !idStr::Icmp( guis[i]->GetSourceFile(), qpath ) ) {
			if ( !forceNOTUnique && ( needUnique || guis[i]->IsInteractive() ) ) {
				break;
			}
			guis[i]->AddRef();
			return guis[i];
		}
	}

	if ( autoLoad ) {
		idUserInterface *gui = Alloc();
		if ( gui->InitFromFile( qpath ) ) {
			gui->SetUniqued( forceNOTUnique ? false : needUnique );
			return gui;
		} else {
			delete gui;
		}
	}
	return NULL;
}

idUserInterface *idUserInterfaceManagerLocal::FindDemoGui( const char *qpath ) {
	int c = demoGuis.Num();
	for ( int i = 0; i < c; i++ ) {
		if ( !idStr::Icmp( demoGuis[i]->GetSourceFile(), qpath ) ) {
			return demoGuis[i];
		}
	}
	return NULL;
}

idListGUI *	idUserInterfaceManagerLocal::AllocListGUI( void ) const {
	return new idListGUILocal();
}

void idUserInterfaceManagerLocal::FreeListGUI( idListGUI *listgui ) {
	delete listgui;
}

/*
================
idUserInterfaceManagerLocal::RegisterIcon, FindIcon

Quake 4's text icons: "^i<code>" in a string draws this material (or the part x, y, w, h of it)
instead of the characters. Registered here; drawing them in text comes with the in-game GUI
work (docs/quake4-port.md 5.6).
================
*/
void idUserInterfaceManagerLocal::RegisterIcon( const char *code, const char *shader, int x, int y, int w, int h ) {
	guiIcon_t *icon = NULL;
	for ( int i = 0; i < icons.Num(); i++ ) {
		if ( icons[i].code.Icmp( code ) == 0 ) {
			icon = &icons[i];
			break;
		}
	}
	if ( icon == NULL ) {
		icon = &icons.Alloc();
		icon->code = code;
	}
	icon->material = declManager->FindMaterial( shader );
	icon->x = x;
	icon->y = y;
	icon->w = w;
	icon->h = h;
}

const idUserInterfaceManagerLocal::guiIcon_t *idUserInterfaceManagerLocal::FindIcon( const char *code ) const {
	for ( int i = 0; i < icons.Num(); i++ ) {
		if ( icons[i].code.Icmp( code ) == 0 ) {
			return &icons[i];
		}
	}
	return NULL;
}

/*
===============================================================================

	idUserInterfaceLocal

===============================================================================
*/

idUserInterfaceLocal::idUserInterfaceLocal() {
	cursorX = cursorY = 0.0;
	desktop = NULL;
	hudAnchors = 0;
	loading = false;
	active = false;
	interactive = false;
	uniqued = false;
	bindHandler = NULL;
	//so the reg eval in gui parsing doesn't get bogus values
	time = 0;
	refs = 1;
	timeStamp = 0;
	lastGlWidth = lastGlHeight = 0;
	initScriptsRun = false;
	lightColor.Set( 1.0f, 1.0f, 1.0f, 1.0f );
}

idUserInterfaceLocal::~idUserInterfaceLocal() {
	delete desktop;
	desktop = NULL;
}

const char *idUserInterfaceLocal::Name() const {
	return source;
}

const char *idUserInterfaceLocal::Comment() const {
	if ( desktop ) {
		return desktop->GetComment();
	}
	return "";
}

bool idUserInterfaceLocal::IsInteractive() const {
	return interactive;
}

/*
================
HudAnchorKind

DUDE: which GUIs get CST anchors so they keep their shape on a wider screen
(idWindow::AutoAnchorHud): those a game draws over the whole screen as its HUD. 1: a HUD,
every part anchored; 2: an overlay composed to cover the screen (a vehicle's scope or noise),
kept whole but for its lines of text; 3: a full-screen menu over a backdrop, the backdrop
filling the screen and the menu kept 4:3 at the centre (idWindow::AutoAnchorMenuFill); 0: none.

Quake 4: the player's HUDs ("hud", "mphud", "cursor" in the retail defs - its crosshairs sit
at the centre, the game only swaps their image) and the vehicles' ("gui_hud"). The cinematic
bars and the weapons' zoom views span the screen.

Doom 3, RoE, Lost Mission: the HUD and the crosshairs, and the death screen (restart.gui, a
menu over a backdrop) - the stock games only (a mod's are its own), and only when the file has
no anchors already (the community "WideGuis" packs anchor theirs by hand: kept as authored).
The PDA and the intro are menus, scaled to 4:3 (r_scaleMenusTo43).
================
*/
static int HudAnchorKind( idWindow *desktop, const char *qpath ) {
	static const struct {
		const char *	gui;
		int				kind;
	} quake4[] = {
		{ "guis/hud.gui",						1 },
		{ "guis/hud_strogg.gui",				1 },
		{ "guis/mphud.gui",						1 },
		{ "guis/cursor.gui",					1 },
		{ "guis/vehicles/hud.gui",				1 },
		{ "guis/vehicles/airdef_cannon.gui",	2 },
		{ "guis/vehicles/medchange.gui",		2 },
		{ "guis/vehicles/strogg_noise.gui",		2 },
		{ "guis/vehicles/blank.gui",			2 },
	}, doom3[] = {
		{ "guis/hud.gui",						1 },
		{ "guis/mphud.gui",						1 },
		{ "guis/cursor.gui",					1 },
		{ "guis/restart.gui",					3 },
	};

	if ( desktop == NULL ) {
		return 0;
	}
	if ( GameTitle::Get().title == GAME_TITLE_QUAKE4 ) {
		for ( int i = 0; i < (int)( sizeof( quake4 ) / sizeof( quake4[0] ) ); i++ ) {
			if ( idStr::Icmp( qpath, quake4[i].gui ) == 0 ) {
				return quake4[i].kind;
			}
		}
		return 0;
	}
	const char *game = cvarSystem->GetCVarString( "fs_game" );
	if ( game[0] != '\0' && idStr::Icmp( game, "base" ) != 0 && idStr::Icmp( game, "d3xp" ) != 0 && idStr::Icmp( game, "d3le" ) != 0 ) {
		return 0;
	}
	for ( int i = 0; i < (int)( sizeof( doom3 ) / sizeof( doom3[0] ) ); i++ ) {
		if ( idStr::Icmp( qpath, doom3[i].gui ) == 0 ) {
			return desktop->HasCstAnchors() ? 0 : doom3[i].kind;
		}
	}
	return 0;
}

/*
================
AnchorHud

The anchors of HudAnchorKind's kind, and where the position rule would part what belongs
together, by name
================
*/
static void AnchorHud( idWindow *desktop, const char *qpath, int kind ) {
	static const struct {
		const char *	gui;
		const char *	window;
		int				anchor;
	} fixes[] = {
		// Quake 4: overlaps the armor bar beside it, its centre a little past the left third
		{ "guis/vehicles/hud.gui",	"p_shields",	idDeviceContext::CST_ANCHOR_BOTTOM_LEFT },
	};

	if ( kind == 0 ) {
		return;
	}
	if ( kind == 3 ) {
		desktop->AutoAnchorMenuFill();
		return;
	}
	desktop->AutoAnchorHud( kind == 2 );
	if ( GameTitle::Get().title == GAME_TITLE_QUAKE4 ) {
		for ( int j = 0; j < (int)( sizeof( fixes ) / sizeof( fixes[0] ) ); j++ ) {
			if ( idStr::Icmp( qpath, fixes[j].gui ) == 0 ) {
				desktop->SetChildCstAnchor( fixes[j].window, fixes[j].anchor );
			}
		}
	}
}

// how GUI files are parsed (InitFromFile, and a shadowed copy for a savegame's state)
static const int GUI_PARSER_FLAGS = LEXFL_NOFATALERRORS | LEXFL_NOSTRINGCONCAT | LEXFL_ALLOWMULTICHARLITERALS | LEXFL_ALLOWBACKSLASHSTRINGCONCAT;

bool idUserInterfaceLocal::InitFromFile( const char *qpath, bool rebuild, bool cache ) {

	if ( !( qpath && *qpath ) ) {
		// FIXME: Memory leak!!
		return false;
	}

	loading = true;

	idParser src( GUI_PARSER_FLAGS );

	//Load the timestamp so reload guis will work correctly
	fileSystem->ReadFile(qpath, NULL, &timeStamp);

	src.LoadFile( qpath );

	if ( !ParseDesktop( src, qpath, rebuild ) ) {
		loading = false;
		return false;
	}

	if ( uiManagerLocal.guis.Find( this ) == NULL ) {
		uiManagerLocal.guis.Append( this );
	}

	loading = false;

	return true;
}

/*
================
idUserInterfaceLocal::ParseDesktop

InitFromFile's parse of the loaded source (DUDE: also a shadowed copy of the file, for a savegame's state)
================
*/
bool idUserInterfaceLocal::ParseDesktop( idParser &src, const char *qpath, bool rebuild ) {

	if ( rebuild ) {
		delete desktop;
		desktop = new idWindow( this );
		bindHandler = NULL;		// DUDE: one of the windows just deleted
	} else if ( desktop == NULL ) {
		desktop = new idWindow( this );
	}

	source = qpath;
	state.Set( "text", "Test Text!" );

	if ( src.IsLoaded() ) {
		idToken token;
		while( src.ReadToken( &token ) ) {
			if ( idStr::Icmp( token, "windowDef" ) == 0 ) {
				desktop->SetDC( &uiManagerLocal.dc );
				if ( desktop->Parse( &src, rebuild ) ) {
					desktop->SetFlag( WIN_DESKTOP );
					desktop->FixupParms();
					// DUDE: a HUD keeps its shape on a wider screen
					hudAnchors = HudAnchorKind( desktop, qpath );
					AnchorHud( desktop, qpath, hudAnchors );
				}
				continue;
			}
		}

		state.Set( "name", qpath );
	} else {
		desktop->SetDC( &uiManagerLocal.dc );
		desktop->SetFlag( WIN_DESKTOP );
		desktop->name = "Desktop";
		desktop->text = va( "Invalid GUI: %s", qpath );
		desktop->rect = idRectangle( 0.0f, 0.0f, 640.0f, 480.0f );
		desktop->drawRect = desktop->rect;
		desktop->foreColor = idVec4( 1.0f, 1.0f, 1.0f, 1.0f );
		desktop->backColor = idVec4( 0.0f, 0.0f, 0.0f, 1.0f );
		desktop->SetupFromState();
		common->Warning( "Couldn't load gui: '%s'", qpath );
		return false;
	}

	interactive = desktop->Interactive();

	return true;
}

// static
bool idUserInterface::IsUserInterfaceScaledTo43( const idUserInterface* ui_ )
{
	const idUserInterfaceLocal* ui = (const idUserInterfaceLocal*)ui_;
	if ( ui == NULL ) {
		assert( 0 && "why do you call this without a ui?!" );
		return false;
	}
	idWindow* win = ui->GetDesktop();
	if ( win == NULL ) {
		return false;
	}
	int winFlags = win->GetFlags();
	if ( (winFlags & WIN_MENUGUI) == 0 || !r_scaleMenusTo43.GetBool() ) {
		// if the window is no fullscreen menu (but an ingame menu or noninteractive like the HUD)
		// or scaling menus to 4:3 by default (r_scaleMenusTo43) is disabled,
		// they only get scaled if they explicitly requested it with "scaleto43 1"
		return (winFlags & WIN_SCALETO43) != 0;
	} else {
		// if it's a fullscreen menu and r_scaleMenusTo43 is enabled,
		// they get scaled to 4:3 unless they explicitly disable it with "scaleto43 0"
		return (winFlags & WIN_NO_SCALETO43) == 0;
	}
}

const char *idUserInterfaceLocal::HandleEvent( const sysEvent_t *event, int _time, bool *updateVisuals ) {

	time = _time;

	if ( bindHandler && event->evType == SE_KEY && event->evValue2 == 1 ) {
		const char *ret = bindHandler->HandleEvent( event, updateVisuals );
		bindHandler = NULL;
		return ret;
	}

	// DG: used to translate gamepad input into events the UI system is familiar with
	sysEvent_t fakedEvent = {};

	if ( event->evType == SE_MOUSE || event->evType == SE_MOUSE_ABS ) {
		if ( !desktop || (desktop->GetFlags() & WIN_MENUGUI) ) {
			// DG: this is a fullscreen GUI, scale the mousedelta added to cursorX/Y
			//     by 640/w, because the GUI pretends that everything is 640x480
			//     even if the actual resolution is higher => mouse moved too fast
			float w = glConfig.winWidth;
			float h = glConfig.winHeight;
			if( w <= 0.0f || h <= 0.0f ) {
				w = VIRTUAL_WIDTH;
				h = VIRTUAL_HEIGHT;
			}
			const float realW = w;
			const float realH = h;

			if ( IsUserInterfaceScaledTo43(this) ) {
				// in case we're scaling menus to 4:3, we need to take that into account
				// when scaling the mouse events.
				// no, we can't just call uiManagerLocal.dc.GetFixScaleForMenu() or sth like that,
				// because when we're here dc.SetMenuScaleFix(true) is not active and it'd just return (1, 1)!
				float aspectRatio = w/h;
				static const float virtualAspectRatio = float(VIRTUAL_WIDTH)/float(VIRTUAL_HEIGHT); // 4:3
				if(aspectRatio > 1.4f) {
					// widescreen (4:3 is 1.333 3:2 is 1.5, 16:10 is 1.6, 16:9 is 1.7778)
					// => we need to modify cursorX scaling, by modifying w
					w *= virtualAspectRatio/aspectRatio;
				} else if(aspectRatio < 1.24f) {
					// portrait-mode, "thinner" than 5:4 (which is 1.25)
					// => we need to scale cursorY via h
					h *= aspectRatio/virtualAspectRatio;
				}
			}

			if( event->evType == SE_MOUSE ) {
				cursorX += event->evValue * (float(VIRTUAL_WIDTH)/w);
				cursorY += event->evValue2 * (float(VIRTUAL_HEIGHT)/h);
			} else { // SE_MOUSE_ABS
				// Note: In case of scaling to 4:3, w and h are already scaled down
				//       to the 4:3 size that fits into the real resolution.
				//       Otherwise xOffset/yOffset will just be 0
				float xOffset = (realW  - w) * 0.5f;
				float yOffset = (realH - h) * 0.5f;
				// offset the mouse coordinates into 4:3 area and scale down to 640x480
				// yes, result could be negative, doesn't matter, code below checks that anyway
				cursorX = (event->evValue  - xOffset) * (float(VIRTUAL_WIDTH)/w);
				cursorY = (event->evValue2 - yOffset) * (float(VIRTUAL_HEIGHT)/h);
			}
		} else {
			// not a fullscreen GUI but some ingame thing - no scaling needed
			cursorX += event->evValue;
			cursorY += event->evValue2;
		}

		if (cursorX < 0) {
			cursorX = 0;
		}
		if (cursorY < 0) {
			cursorY = 0;
		}
	}
	else if ( event->evType == SE_JOYSTICK && event->evValue2 != 0 && event->evValue < 4 )
	{
		// evValue:  axis = jEvent - J_AXIS_MIN;
		// evValue2: percent (-100 to 100)

		// currently uses both sticks for cursor movement
		// TODO could use one stick for scrolling (maybe by generating K_UPARROW/DOWNARROW events?)
		float addVal = expf( fabsf(event->evValue2 * 0.03f) ) * 0.5f;
		if(event->evValue2 < 0)
			addVal = -addVal;

		if( event->evValue == 0 || event->evValue == 2 ) {
			cursorX += addVal;
		} else if( event->evValue == 1 || event->evValue == 3 ) {
			cursorY += addVal;
		}

		if (cursorX < 0) {
			cursorX = 0;
		}
		if (cursorY < 0) {
			cursorY = 0;
		}

		// some things like highlighting hovered UI elements need a mouse event,
		// so create a fake mouse event
		fakedEvent.evType = SE_MOUSE;
		// the coordinates (evValue/evValue2) aren't used, but keeping them at 0
		// (as default-initialized above) shouldn't hurt either way
		event = &fakedEvent;
	}
	else if ( event->evType == SE_KEY && event->evValue >= K_FIRST_JOY && event->evValue <= K_LAST_JOY )
	{
		// map some gamepad buttons to SE_KEY events that the UI already knows how to use
		int key = 0;
		if( idKeyInput::GetUsercmdAction( event->evValue ) ==  20 /* UB_ATTACK*/ ) {
			// if this button is bound to _attack (fire), treat it as left mouse button
			key = K_MOUSE1;
		} else {
			switch(event->evValue) {
				// emulate mouse buttons
				case K_JOY_BTN_SOUTH: // A on xbox controller
					key = K_MOUSE1;
					break;
				case K_JOY_BTN_EAST: // B on xbox controller
					key = K_MOUSE2;
					break;
				// emulate cursor keys (sometimes used for scrolling or selecting in a list)
				case K_JOY_DPAD_UP:
					key = K_UPARROW;
					break;
				case K_JOY_DPAD_DOWN:
					key = K_DOWNARROW;
					break;
				case K_JOY_DPAD_LEFT:
					key = K_LEFTARROW;
					break;
				case K_JOY_DPAD_RIGHT:
					key = K_RIGHTARROW;
					break;
				// enter is useful after selecting something with cursor keys (or dpad)
				// in a list, like selecting a savegame - I guess left trigger is suitable for that?
				// (right trigger is often used for shooting, which we use as K_MOUSE1 here)
				case K_JOY_TRIGGER1:
					key = K_ENTER;
					break;
			}
		}
		if (key != 0) {
			fakedEvent = *event;
			fakedEvent.evValue = key;
			event = &fakedEvent;
		}
	}

	if ( desktop ) {
		return desktop->HandleEvent( event, updateVisuals );
	}

	return "";
}

void idUserInterfaceLocal::HandleNamedEvent ( const char* eventName ) {
	desktop->RunNamedEvent( eventName );
}

void idUserInterfaceLocal::Redraw( int _time ) {
	if ( r_skipGuiShaders.GetInteger() > 5 ) {
		return;
	}
	if ( !loading && desktop ) {
		if ( desktop->GetFlags() & WIN_MENUGUI ) {
			// if the (SDL) window size has changed, calculate and set the
			// "gui::cst*" window register variables accordingly
			if ( MaybeSetCstWinRegs() ) {
				// tell the GUI script about it in case it wants to handle the size change in
				// some way (though usually it's enough to use sth like
				// `rect 0, 200, "gui::cstHorPad", 100"` and `cstAnchor  CST_ANCHOR_LEFT`
				// for a windowDef that should fill part of the left side)
				HandleNamedEvent( "CstScreenSizeChange" );
			}
		}

		time = _time;

		// DUDE: Quake 4 onInit runs once, on the first redraw: by then the game has set the
		// gui_parm* state that world screens branch on (strogg vs marine labels and the like).
		if ( !initScriptsRun ) {
			initScriptsRun = true;
			desktop->RunInitScripts();
		}

		uiManagerLocal.dc.PushClipRect( uiManagerLocal.screenRect );
		desktop->Redraw( 0, 0 );
		uiManagerLocal.dc.PopClipRect();
	}
}

void idUserInterfaceLocal::DrawCursor() {
	if ( !desktop || desktop->GetFlags() & WIN_MENUGUI ) {
		uiManagerLocal.dc.DrawCursor(&cursorX, &cursorY, 32.0f );
	} else {
		uiManagerLocal.dc.DrawCursor(&cursorX, &cursorY, 64.0f );
	}
}

const idDict &idUserInterfaceLocal::State() const {
	return state;
}

void idUserInterfaceLocal::DeleteStateVar( const char *varName ) {
	state.Delete( varName );
}

void idUserInterfaceLocal::SetStateString( const char *varName, const char *value ) {
	state.Set( varName, value );
}

// Quake 4: where multiplayer chat lines wrap. Multiplayer is a later phase (docs/quake4-port.md
// Q9); until then every line fits.
bool idUserInterfaceLocal::GetMaxTextIndex( const char *windowName, const char *text, wrapInfo_t &wrapInfo ) const {
	wrapInfo = wrapInfo_t();
	return false;
}

void idUserInterfaceLocal::SetStateBool( const char *varName, const bool value ) {
	state.SetBool( varName, value );
}

void idUserInterfaceLocal::SetStateInt( const char *varName, const int value ) {
	state.SetInt( varName, value );
}

void idUserInterfaceLocal::SetStateFloat( const char *varName, const float value ) {
	state.SetFloat( varName, value );
}

const char* idUserInterfaceLocal::GetStateString( const char *varName, const char* defaultString ) const {
	return state.GetString(varName, defaultString);
}

bool idUserInterfaceLocal::GetStateBool( const char *varName, const char* defaultString ) const {
	return state.GetBool(varName, defaultString);
}

int idUserInterfaceLocal::GetStateInt( const char *varName, const char* defaultString ) const {
	return state.GetInt(varName, defaultString);
}

float idUserInterfaceLocal::GetStateFloat( const char *varName, const char* defaultString ) const {
	return state.GetFloat(varName, defaultString);
}

void idUserInterfaceLocal::StateChanged( int _time, bool redraw ) {
	time = _time;
	if (desktop) {
		// DG: little hack: allow game DLLs to do
		//     ui->SetStateBool("scaleto43", true);
		//     ui->StateChanged(gameLocal.time);
		//     so we can force cursors.gui (crosshair) to be scaled, for example.
		//     Not sure if/where that's needed, but ui->SetStateBool("scaleto43", false);
		//     is now also supported to explicitly disable scaling from the code
		int scaleTo43 = 0;
		if(state.GetInt("scaleto43", "-1", scaleTo43)) {
			if(scaleTo43 > 0) {
				desktop->SetFlag(WIN_SCALETO43);
				desktop->ClearFlag(WIN_NO_SCALETO43);
				// TODO
			} else if(scaleTo43 == 0) {
				desktop->ClearFlag(WIN_SCALETO43);
				desktop->SetFlag(WIN_NO_SCALETO43);
			} // do nothing for -1, it means that it wasn't set at all
		}
		// DG end

		desktop->StateChanged( redraw );
	}
	if ( state.GetBool( "noninteractive" ) ) {
		interactive = false;
	}
	else {
		if (desktop) {
			interactive = desktop->Interactive();
		} else {
			interactive = false;
		}
	}
}

const char *idUserInterfaceLocal::Activate(bool activate, int _time) {
	time = _time;
	active = activate;
	if ( desktop ) {
		// DG: added this hack for gamepad input - Note that it can happen that
		//     a UI has been made non-interactive before Activate(false, ..) is called
		//     and it still needs to be unregistered with Sys_SetInteractiveIngameGuiActive()!
		if ( interactive || !activate ) {
			Sys_SetInteractiveIngameGuiActive( activate, this );
		} // DG end
		activateStr = "";

		if ( desktop->GetFlags() & WIN_MENUGUI ) {
			// DG: calculate and set the "gui::cst*" window register variables
			//     so the GUI can use them
			MaybeSetCstWinRegs(true);
		}
		desktop->Activate( activate, activateStr );
		return activateStr;
	}
	return "";
}

void idUserInterfaceLocal::Trigger(int _time) {
	time = _time;
	if ( desktop ) {
		desktop->Trigger();
	}
}

void idUserInterfaceLocal::ReadFromDemoFile( class idDemoFile *f ) {
	idStr work;
	f->ReadDict( state );
	source = state.GetString("name");

	if (desktop == NULL) {
		f->Log("creating new gui\n");
		desktop = new idWindow(this);
		desktop->SetFlag( WIN_DESKTOP );
		desktop->SetDC( &uiManagerLocal.dc );
		desktop->ReadFromDemoFile(f);
	} else {
		f->Log("re-using gui\n");
		desktop->ReadFromDemoFile(f, false);
	}

	f->ReadFloat( cursorX );
	f->ReadFloat( cursorY );

	bool add = true;
	int c = uiManagerLocal.demoGuis.Num();
	for ( int i = 0; i < c; i++ ) {
		if ( uiManagerLocal.demoGuis[i] == this ) {
			add = false;
			break;
		}
	}

	if (add) {
		uiManagerLocal.demoGuis.Append(this);
	}
}

void idUserInterfaceLocal::WriteToDemoFile( class idDemoFile *f ) {
	idStr work;
	f->WriteDict( state );
	if (desktop) {
		desktop->WriteToDemoFile(f);
	}

	f->WriteFloat( cursorX );
	f->WriteFloat( cursorY );
}

/*
===============================================================================

	DUDE: savegame GUI state

	A GUI's saved state is its state dict and a few values, then every window's variables, scripts,
	registers and transitions in the order its GUI file declares them; nothing in it says how long
	it is or which version of the file wrote it. Read against another version of the file - a
	widescreen or HUD mod installed or removed since the game was saved, a retail or dhewm3 save
	under such a mod - it falls out of step, which used to crash the load or stop it. The readers
	check what the stream lets them (window and register names, counts, lengths) and stop at the
	first mismatch (GuiRestore_Failed); ReadFromSaveGame then rewinds, finds the copy of the file
	the state was written with among the copies in the search path (the stock one a mod shadows),
	and keeps the saved state dict over the GUI laid out by the current file.

	One window type wrote a struct holding a pointer as it lay in memory (idGameSSDWindow, RoE's
	arcade on phobos2), so its state also depends on whether a 32-bit engine (retail Doom 3, 32-bit
	dhewm3) or a 64-bit one wrote it; a GUI that holds one is tried both ways.

	DUDE writes the state length-prefixed (DUDE_SAVEGAME_REVISION 1), so its own savegames skip a
	GUI that changed since without needing the old file.

===============================================================================
*/

static const int GUI_SAVE_MAGIC		= (int)0xD0DE6015;	// negative: an older save starts with the state dict's count here
static const int GUI_SAVE_VERSION	= 1;

static struct {
	bool		active;
	bool		failed;
	int			end;			// where this GUI's state ends in the file (or the file does)
	int			pointerSize;	// sizeof( void * ) of the engine that wrote it, as assumed
	bool		pointerSized;	// something read depended on that
	idStr		reason;
} guiRestore;

bool GuiRestore_Failed( void ) {
	return guiRestore.active && guiRestore.failed;
}

int GuiRestore_PointerSize( void ) {
	if ( !guiRestore.active ) {
		return sizeof( void * );
	}
	guiRestore.pointerSized = true;
	return guiRestore.pointerSize;
}

bool GuiRestore_Fail( const char *fmt, ... ) {
	if ( guiRestore.active && !guiRestore.failed ) {
		char text[MAX_STRING_CHARS];
		va_list argptr;
		va_start( argptr, fmt );
		idStr::vsnPrintf( text, sizeof( text ), fmt, argptr );
		va_end( argptr );
		guiRestore.failed = true;
		guiRestore.reason = text;
	}
	return false;
}

bool GuiRestore_Count( idFile *savefile, int num, int minBytesEach ) {
	if ( num < 0 ) {
		return GuiRestore_Fail( "a negative count (%d)", num );
	}
	if ( guiRestore.active && (long long)num * minBytesEach > (long long)( guiRestore.end - savefile->Tell() ) ) {
		return GuiRestore_Fail( "a count (%d) running past the end of the state", num );
	}
	return true;
}

bool GuiRestore_ReadString( idFile *savefile, idStr &string ) {
	int len = 0;
	savefile->Read( &len, sizeof( len ) );
	if ( !GuiRestore_Count( savefile, len, 1 ) ) {
		return false;
	}
	string.Fill( ' ', len );
	if ( len > 0 ) {
		savefile->Read( &string[0], len );
	}
	return true;
}

/*
================
idUserInterfaceLocal::WriteToSaveGame
================
*/
bool idUserInterfaceLocal::WriteToSaveGame( idFile *savefile ) const {
	// DUDE: the state as before, prefixed with its length (see above)
	idFile_Memory stateFile( "guiSaveState" );
	WriteStateToSaveGame( &stateFile );

	const int magic = GUI_SAVE_MAGIC;
	const int version = GUI_SAVE_VERSION;
	const int length = stateFile.Length();
	savefile->Write( &magic, sizeof( magic ) );
	savefile->Write( &version, sizeof( version ) );
	savefile->Write( &length, sizeof( length ) );
	savefile->Write( stateFile.GetDataPtr(), length );

	return true;
}

void idUserInterfaceLocal::WriteStateToSaveGame( idFile *savefile ) const {
	int len;
	const idKeyValue *kv;
	const char *string;

	int num = state.GetNumKeyVals();
	savefile->Write( &num, sizeof( num ) );

	for( int i = 0; i < num; i++ ) {
		kv = state.GetKeyVal( i );
		len = kv->GetKey().Length();
		string = kv->GetKey().c_str();
		savefile->Write( &len, sizeof( len ) );
		savefile->Write( string, len );

		len = kv->GetValue().Length();
		string = kv->GetValue().c_str();
		savefile->Write( &len, sizeof( len ) );
		savefile->Write( string, len );
	}

	savefile->Write( &active, sizeof( active ) );
	savefile->Write( &interactive, sizeof( interactive ) );
	savefile->Write( &uniqued, sizeof( uniqued ) );
	savefile->Write( &time, sizeof( time ) );
	len = activateStr.Length();
	savefile->Write( &len, sizeof( len ) );
	savefile->Write( activateStr.c_str(), len );
	len = pendingCmd.Length();
	savefile->Write( &len, sizeof( len ) );
	savefile->Write( pendingCmd.c_str(), len );
	len = returnCmd.Length();
	savefile->Write( &len, sizeof( len ) );
	savefile->Write( returnCmd.c_str(), len );

	savefile->Write( &cursorX, sizeof( cursorX ) );
	savefile->Write( &cursorY, sizeof( cursorY ) );

	desktop->WriteToSaveGame( savefile );
}

/*
================
idUserInterfaceLocal::ReadValuesFromSaveGame

The part of the state that doesn't depend on the GUI file: the state dict and the GUI's own values
================
*/
void idUserInterfaceLocal::ReadValuesFromSaveGame( idFile *savefile ) {
	int num = 0;
	idStr key;
	idStr value;

	savefile->Read( &num, sizeof( num ) );
	if ( !GuiRestore_Count( savefile, num, 2 * sizeof( int ) ) ) {
		return;
	}

	state.Clear();
	for( int i = 0; i < num; i++ ) {
		if ( !GuiRestore_ReadString( savefile, key ) || !GuiRestore_ReadString( savefile, value ) ) {
			return;
		}
		state.Set( key, value );
	}

	savefile->Read( &active, sizeof( active ) );
	savefile->Read( &interactive, sizeof( interactive ) );
	savefile->Read( &uniqued, sizeof( uniqued ) );
	savefile->Read( &time, sizeof( time ) );

	if ( !GuiRestore_ReadString( savefile, activateStr ) || !GuiRestore_ReadString( savefile, pendingCmd ) || !GuiRestore_ReadString( savefile, returnCmd ) ) {
		return;
	}

	savefile->Read( &cursorX, sizeof( cursorX ) );
	savefile->Read( &cursorY, sizeof( cursorY ) );
}

/*
================
idUserInterfaceLocal::RestoreState

The state as written by an engine of the given pointer size, read against this GUI's file up to
end; false at the first mismatch. pointerSized: whether the pointer size mattered.
================
*/
bool idUserInterfaceLocal::RestoreState( idFile *savefile, int end, int pointerSize, idStr *reason, bool *pointerSized ) {
	guiRestore.active = true;
	guiRestore.failed = false;
	guiRestore.end = end;
	guiRestore.pointerSize = pointerSize;
	guiRestore.pointerSized = false;
	guiRestore.reason.Clear();

	ReadValuesFromSaveGame( savefile );
	if ( !guiRestore.failed ) {
		desktop->ReadFromSaveGame( savefile );
	}
	if ( !guiRestore.failed && savefile->Tell() > end ) {
		GuiRestore_Fail( "the state ran %d bytes past its end", savefile->Tell() - end );
	}

	const bool ok = !guiRestore.failed;
	if ( reason != NULL ) {
		*reason = guiRestore.reason;
	}
	if ( pointerSized != NULL ) {
		*pointerSized = guiRestore.pointerSized;
	}
	guiRestore.active = false;
	guiRestore.failed = false;

	if ( ok ) {
		// DUDE: the anchors come from the layout, not the game's state - a save made before
		// the HUD had them holds none
		AnchorHud( desktop, source, hudAnchors );
	}
	return ok;
}

/*
================
idUserInterfaceLocal::RestoreValuesOnly

The state was written against another version of this GUI's file: the GUI is laid out anew from the
current file, with the saved state dict and values over it. Reads the values at start, leaves the
file at end.
================
*/
void idUserInterfaceLocal::RestoreValuesOnly( idFile *savefile, int start, int end ) {
	InitFromFile( source, true, false );

	savefile->Seek( start, FS_SEEK_SET );
	guiRestore.active = true;
	guiRestore.failed = false;
	guiRestore.end = end;
	ReadValuesFromSaveGame( savefile );
	guiRestore.active = false;
	guiRestore.failed = false;

	savefile->Seek( end, FS_SEEK_SET );
}

/*
================
idUserInterfaceLocal::ReadFromSaveGame
================
*/
bool idUserInterfaceLocal::ReadFromSaveGame( idFile *savefile ) {
	initScriptsRun = true;	// DUDE: the saved state already includes what onInit set up

	const int start = savefile->Tell();
	const int fileEnd = savefile->Length();
	idStr reason;

	// DUDE: a length-prefixed state (DUDE_SAVEGAME_REVISION 1 on)
	int magic = 0;
	savefile->Read( &magic, sizeof( magic ) );
	if ( magic == GUI_SAVE_MAGIC ) {
		int version = 0, length = -1;
		savefile->Read( &version, sizeof( version ) );
		savefile->Read( &length, sizeof( length ) );
		const int stateStart = savefile->Tell();
		if ( length < 0 || length > fileEnd - stateStart ) {
			common->Warning( "Savegame: the saved state of GUI %s is damaged (%d bytes); the savegame can't be read past it", source.c_str(), length );
			return false;
		}
		const int stateEnd = stateStart + length;
		if ( version != GUI_SAVE_VERSION ) {
			// from a later DUDE: skipped whole, the GUI as its file sets it up
			common->Printf( "Savegame: GUI %s was saved in a newer format (%d), reset to its start\n", source.c_str(), version );
			InitFromFile( source, true, false );
			savefile->Seek( stateEnd, FS_SEEK_SET );
			return true;
		}
		if ( !RestoreState( savefile, stateEnd, sizeof( void * ), &reason, NULL ) || savefile->Tell() != stateEnd ) {
			if ( reason.IsEmpty() ) {
				reason = va( "%d bytes of it left unread", stateEnd - savefile->Tell() );
			}
			common->Printf( "Savegame: GUI %s changed since the game was saved (%s): its state values kept, its layout from the current file\n", source.c_str(), reason.c_str() );
			RestoreValuesOnly( savefile, stateStart, stateEnd );
		}
		savefile->Seek( stateEnd, FS_SEEK_SET );
		return true;
	}

	// a savegame from retail Doom 3, dhewm3 or DUDE before revision 1: nothing says where the state
	// ends but the GUI file it was written with
	const int nativePointerSize = sizeof( void * );
	savefile->Seek( start, FS_SEEK_SET );
	if ( RestoreState( savefile, fileEnd, nativePointerSize, &reason, NULL ) ) {
		return true;
	}

	// written with another copy of the file (the stock one a mod now shadows, the mod's one that now
	// shadows the stock file, one kept in savegames/), or by an engine of the other pointer size
	int stateEnd = -1, matchPointerSize = 0;
	idStr origin;
	bool installedCopy = false;
	if ( RestoreWithCopies( savefile, source, start, fileEnd, stateEnd, matchPointerSize, origin, installedCopy ) ) {
		if ( installedCopy && matchPointerSize != nativePointerSize ) {
			// the file as installed, written by a 32-bit (64-bit) engine: restored whole
			InitFromFile( source, true, false );
			savefile->Seek( start, FS_SEEK_SET );
			if ( RestoreState( savefile, fileEnd, matchPointerSize, NULL, NULL ) && savefile->Tell() == stateEnd ) {
				common->Printf( "Savegame: GUI %s was saved by a %d-bit engine\n", source.c_str(), matchPointerSize * 8 );
				return true;
			}
		}
		common->Printf( "Savegame: GUI %s was saved with the copy in %s (%s): its state values kept, its layout from the current file\n",
			source.c_str(), origin.c_str(), reason.c_str() );
		RestoreValuesOnly( savefile, start, stateEnd );
		return true;
	}

	// no copy of the file reads it, but where it ends can still be told for the player's HUD:
	// Doom 3's player saves its PDA right after it (idPlayer::Save)
	if ( GameTitle::Get().title != GAME_TITLE_QUAKE4 && idStr::Icmp( source, "guis/hud.gui" ) == 0 ) {
		stateEnd = FindFollowingGuiState( savefile, start, fileEnd, "guis/pda.gui" );
		if ( stateEnd >= 0 ) {
			common->Printf( "Savegame: GUI %s was saved with a version of the file no longer installed (%s): its state values kept, its layout from the current file\n",
				source.c_str(), reason.c_str() );
			RestoreValuesOnly( savefile, start, stateEnd );
			return true;
		}
	}

	common->Error( "The savegame's state of GUI %s matches no version of that file now installed (%s): "
		"was a GUI mod (a widescreen pack, a HUD) removed since the game was saved? Putting that mod's .pk4 "
		"in the savegames folder lets this savegame load (it is read there for old savegames only).",
		source.c_str(), reason.c_str() );
	return false;
}

/*
================
idUserInterfaceLocal::RestoreWithCopies

Reads the state at start with each copy of the GUI file qpath (FS_ReadFileVersions) as written by a
64- and a 32-bit engine, in trial GUIs, until one reads it whole. stateEnd: where it ends;
installedCopy: it was the copy the game loads.
================
*/
bool idUserInterfaceLocal::RestoreWithCopies( idFile *savefile, const char *qpath, int start, int fileEnd,
		int &stateEnd, int &pointerSize, idStr &origin, bool &installedCopy ) {
	const int nativePointerSize = sizeof( void * );
	idStrList texts, origins;

	FS_ReadFileVersions( qpath, texts, origins );
	for ( int i = 0; i < texts.Num(); i++ ) {
		for ( int j = 0; j < 2; j++ ) {
			const int trialPointerSize = ( j == 0 ) ? nativePointerSize : 12 - nativePointerSize;	// 4 <-> 8
			idUserInterfaceLocal *trial = new idUserInterfaceLocal();
			idParser src( GUI_PARSER_FLAGS );
			src.LoadMemory( texts[i].c_str(), texts[i].Length(), qpath );
			trial->loading = true;
			const bool parsed = trial->ParseDesktop( src, qpath, true );
			trial->loading = false;
			bool pointerSized = false;
			savefile->Seek( start, FS_SEEK_SET );
			const bool ok = parsed && trial->RestoreState( savefile, fileEnd, trialPointerSize, NULL, &pointerSized );
			delete trial;
			if ( ok ) {
				stateEnd = savefile->Tell();
				pointerSize = trialPointerSize;
				origin = origins[i];
				installedCopy = ( i == 0 );
				return true;
			}
			if ( !pointerSized ) {
				break;		// the other pointer size reads it no differently
			}
		}
	}
	return false;
}

/*
================
idUserInterfaceLocal::FindFollowingGuiState

Where a GUI's state at start ends when no copy of its file can read it, the game having saved the GUI
nextGui right after it: at that GUI's record (its name, its unique flag), taken only when nextGui's
state reads whole from there. -1 when not found.
================
*/
int idUserInterfaceLocal::FindFollowingGuiState( idFile *savefile, int start, int fileEnd, const char *nextGui ) {
	static const int SEARCH_LIMIT = 1 << 20;	// a HUD's state is about 150 KB

	// the values ahead of the windows don't depend on the file: past them at least
	savefile->Seek( start, FS_SEEK_SET );
	guiRestore.active = true;
	guiRestore.failed = false;
	guiRestore.end = fileEnd;
	ReadValuesFromSaveGame( savefile );
	const bool valuesRead = !guiRestore.failed;
	guiRestore.active = false;
	guiRestore.failed = false;
	if ( !valuesRead ) {
		return -1;
	}

	const int from = savefile->Tell();
	const int length = Min( fileEnd - from, SEARCH_LIMIT );
	const int nameLength = idStr::Length( nextGui );
	if ( length < (int)sizeof( int ) + nameLength + 1 ) {
		return -1;
	}
	idList<byte> data;
	data.SetNum( length );
	if ( savefile->Read( data.Ptr(), length ) != length ) {
		return -1;
	}

	for ( int p = 0; p + (int)sizeof( int ) + nameLength + 1 <= length; p++ ) {
		int len;
		memcpy( &len, &data[p], sizeof( len ) );
		len = LittleInt( len );
		if ( len != nameLength || idStr::Icmpn( (const char *)&data[p + sizeof( int )], nextGui, nameLength ) != 0 ) {
			continue;
		}
		const byte unique = data[p + sizeof( int ) + nameLength];
		if ( unique > 1 ) {
			continue;
		}
		int nextEnd, pointerSize;
		idStr origin;
		bool installedCopy;
		const int nextStart = from + p + (int)sizeof( int ) + nameLength + 1;
		if ( RestoreWithCopies( savefile, nextGui, nextStart, fileEnd, nextEnd, pointerSize, origin, installedCopy ) ) {
			return from + p;
		}
	}
	return -1;
}

size_t idUserInterfaceLocal::Size() {
	size_t sz = sizeof(*this) + state.Size() + source.Allocated();
	if ( desktop ) {
		sz += desktop->Size();
	}
	return sz;
}

void idUserInterfaceLocal::RecurseSetKeyBindingNames( idWindow *window ) {
	int i;
	idWinVar *v = window->GetWinVarByName( "bind" );
	if ( v ) {
		SetStateString( v->GetName(), idKeyInput::KeysFromBinding( v->GetName() ) );
	}
	i = 0;
	while ( i < window->GetChildCount() ) {
		idWindow *next = window->GetChild( i );
		if ( next ) {
			RecurseSetKeyBindingNames( next );
		}
		i++;
	}
}

/*
==============
idUserInterfaceLocal::SetKeyBindingNames
==============
*/
void idUserInterfaceLocal::SetKeyBindingNames( void ) {
	if ( !desktop ) {
		return;
	}
	// walk the windows
	RecurseSetKeyBindingNames( desktop );
}

/*
==============
idUserInterfaceLocal::SetCursor
==============
*/
void idUserInterfaceLocal::SetCursor( float x, float y ) {
	cursorX = x;
	cursorY = y;
}


bool idUserInterfaceLocal::MaybeSetCstWinRegs(bool force) {
	if ( desktop == NULL ) {
		return false;
	}
	int glWidth, glHeight;
	renderSystem->GetGLSettings(glWidth, glHeight);
	if (glWidth <= 0 || glHeight <= 0 || (!force && glWidth == lastGlWidth && glHeight == lastGlHeight) ) {
		return false;
	}
	lastGlWidth = glWidth;
	lastGlHeight = glHeight;

	float glAspectRatio = (float)glWidth / (float)glHeight;
	const float vidAspectRatio = (float)VIRTUAL_WIDTH / (float)VIRTUAL_HEIGHT;

	const float desktopWidth  = desktop->forceAspectWidth;
	const float desktopHeight = desktop->forceAspectHeight;

	float horizPadding = 0;
	float vertPadding = 0;
	float modWidth = desktopWidth;
	float modHeight = desktopHeight;

	if (glAspectRatio >= vidAspectRatio) {
		modWidth = desktopHeight * glAspectRatio;
		horizPadding = 0.5f * (modWidth - desktopWidth);
	} else {
		modHeight = desktopWidth / glAspectRatio;
		vertPadding = 0.5f * (modHeight - desktopHeight);
	}

	SetStateFloat( "cstAspectRatio", glAspectRatio );
	SetStateFloat( "cstWidth", modWidth );
	SetStateFloat( "cstHeight", modHeight );
	SetStateFloat( "cstHorPad", horizPadding );
	SetStateFloat( "cstVertPad", vertPadding );

	return true;
}
