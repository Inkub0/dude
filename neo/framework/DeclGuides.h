/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.h for license header)
===========================================================================
*/

// DUDE: material guides - Quake 4's material templates (docs/quake4-port.md section 5.2).
//
// Raven writes most Quake 4 materials as one-line instances of templates kept in
// guides/*.guide:
//
//     guide generic_shader(TextureParm) { diffusemap textures/TextureParm_d ... }   // template
//     guide textures/canyon/can_plate_01 generic_shader("canyon/can_plate_01")      // .mtr use
//     inlineGuide corpseBurnEffect(models/monsters/burnout.tga)                     // inside a material
//
// A parameter name is replaced wherever it appears in the template body, including inside
// longer tokens (textures/TextureParm_d), case-sensitively: two retail templates have a
// parameter "Scale" next to the material keyword "scale". The expansion is purely textual
// and happens before the decl loader splits a .mtr file into declarations, so a guide
// becomes an ordinary "name { body }" material. Doom 3 has no guides folder, so nothing
// here runs for it.

#ifndef __DECLGUIDES_H__
#define __DECLGUIDES_H__

#include "idlib/Str.h"
#include "idlib/containers/List.h"

namespace DeclGuides {

// (Re)load every guides/*.guide template. Call before the materials are registered.
void		Load( void );
void		Clear( void );

// Number of templates loaded (0 for Doom 3).
int			Num( void );

// Expand the guide and inlineGuide references in a decl file's text into out. Returns false
// (and leaves out untouched) when the text has none. An expansion keeps the template's own
// lines (the material parser is line-sensitive), so out has more lines than text: lineMap[k]
// is the line of text that line k + 1 of out comes from.
bool		Expand( const char *text, int length, const char *fileName, idStr &out, idList<int> &lineMap );

}

#endif /* !__DECLGUIDES_H__ */
