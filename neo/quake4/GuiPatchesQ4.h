/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.h for license header)
===========================================================================
*/

// DUDE: DUDE's own additions to Quake 4's GUI files (the main menu's DUDE Settings button, and the
// widescreen fill of the main menu and the loading screens).
// The retail files are Raven's and DUDE ships none of them, so instead of a modified copy the
// engine splices DUDE-authored windowDefs into the retail text as it loads it. Engine-only: the
// game library never includes this.

#ifndef __GUIPATCHESQ4_H__
#define __GUIPATCHESQ4_H__

class idStr;
class idUserInterface;

// whether DUDE has additions to the GUI file qpath (only ever when the title is Quake 4)
bool	Q4Gui_HasPatch( const char *qpath );
// text: the GUI file qpath as read. Applies DUDE's patches for that file (only when the title is
// Quake 4); false when there are none, text then untouched.
bool	Q4Gui_PatchSource( const char *qpath, idStr &text );

// the GUI state DUDE's widescreen additions read: gui::dude_sideBars (1 when the GUI is fitted to
// 4:3 with room beside it, 0 when it fills the screen as vanilla: a 4:3 screen, or stretched) and
// gui::dude_horPad (that room on each side, in GUI units; 0 without it). scaledTo43: the GUI is
// drawn fitted to 4:3 (r_scaleMenusTo43, scaleto43).
void	Q4Gui_SetWidescreenVars( idUserInterface *gui, bool scaledTo43 );

#endif /* !__GUIPATCHESQ4_H__ */
