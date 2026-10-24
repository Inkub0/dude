/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.h for license header)
===========================================================================
*/

#ifndef __FONTINTERNAL_H__
#define __FONTINTERNAL_H__

#include "renderer/RenderSystem.h"

/*
	DUDE: the engine's own fonts. The public glyphInfo_t / fontInfo_t / fontInfoEx_t of
	RenderSystem.h keep dhewm3's layout (whole-pixel int metrics), because a game library
	built for dhewm3 can call idRenderSystem::RegisterFont with them (tools/check_dhewm3_abi.py).
	The engine's GUI draws from these instead: float metrics, since Raven's Quake 4 .fontdat
	stores fractional pixels (an 8.562 px advance is typical, and rounding per glyph would
	accumulate visible spacing error along a string), plus the left bearing id's fonts lack.
	id's fontImage_*.dat whole pixels read into the floats exactly, so Doom 3 text is unchanged.
	Never included by a game-visible header.
*/
typedef struct {
	float				height;			// number of scan lines
	float				top;			// top of glyph in buffer
	float				bottom;			// bottom of glyph in buffer
	float				pitch;			// width for copying
	float				xSkip;			// x adjustment
	float				imageWidth;		// width of actual image
	float				imageHeight;	// height of actual image
	float				s;				// x offset in image where glyph starts
	float				t;				// y offset in image where glyph starts
	float				s2;
	float				t2;
	float				left;			// horizontal bearing (pen to glyph left edge); Raven fonts only, 0 for id fonts
	const idMaterial *	glyph;			// shader with the glyph
	char				shaderName[32];
} dudeGlyphInfo_t;

typedef struct {
	dudeGlyphInfo_t		glyphs [GLYPHS_PER_FONT];
	float				glyphScale;
	char				name[64];
} dudeFontInfo_t;

typedef struct {
	dudeFontInfo_t		fontInfoSmall;
	dudeFontInfo_t		fontInfoMedium;
	dudeFontInfo_t		fontInfoLarge;
	int					maxHeight;
	int					maxWidth;
	int					maxHeightSmall;
	int					maxWidthSmall;
	int					maxHeightMedium;
	int					maxWidthMedium;
	int					maxHeightLarge;
	int					maxWidthLarge;
	char				name[64];
} dudeFontInfoEx_t;

// loads the 12, 24 and 48 point sheets of a font (id's fontImage_<pt>.dat or Raven's
// <font>_<pt>.fontdat); what idRenderSystem::RegisterFont converts to the public layout
bool R_RegisterEngineFont( const char *fontName, dudeFontInfoEx_t &font );

#endif /* !__FONTINTERNAL_H__ */
