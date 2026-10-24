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

#include "renderer/tr_local.h"
#include "renderer/FontInternal.h"

#define FILESIZE_fontInfo_t (20548)

// DUDE: Raven's Quake 4 font sheet, <font>_<pt>.fontdat. 256 glyphs of 9 little-endian
// float32 (imageWidth, imageHeight, horiAdvance, horiBearingX, horiBearingY, s, t, s2, t2),
// then pointSize, fontHeight, ascender, descender (float32) and 4 bytes of padding. Every
// glyph samples the one image <font>_<pt>.tga. Layout read from the retail files
// (docs/quake4-port.md section 1.1); metrics are fractional pixels.
#define FILESIZE_ravenFontInfo_t	( GLYPHS_PER_FONT * 9 * 4 + 5 * 4 )	// 9236

#ifdef BUILD_FREETYPE
#include "../ft2/fterrors.h"
#include "../ft2/ftsystem.h"
#include "../ft2/ftimage.h"
#include "../ft2/freetype.h"
#include "../ft2/ftoutln.h"

#define _FLOOR(x)  ((x) & -64)
#define _CEIL(x)   (((x)+63) & -64)
#define _TRUNC(x)  ((x) >> 6)

FT_Library ftLibrary = NULL;
#endif


#ifdef BUILD_FREETYPE

/*
============
R_GetGlyphInfo
============
*/
void R_GetGlyphInfo(FT_GlyphSlot glyph, int *left, int *right, int *width, int *top, int *bottom, int *height, int *pitch) {

	*left  = _FLOOR( glyph->metrics.horiBearingX );
	*right = _CEIL( glyph->metrics.horiBearingX + glyph->metrics.width );
	*width = _TRUNC(*right - *left);

	*top    = _CEIL( glyph->metrics.horiBearingY );
	*bottom = _FLOOR( glyph->metrics.horiBearingY - glyph->metrics.height );
	*height = _TRUNC( *top - *bottom );
	*pitch  = ( qtrue ? (*width+3) & -4 : (*width+7) >> 3 );
}

/*
============
R_RenderGlyph
============
*/
FT_Bitmap *R_RenderGlyph(FT_GlyphSlot glyph, glyphInfo_t* glyphOut) {
	FT_Bitmap  *bit2;
	int left, right, width, top, bottom, height, pitch, size;

	R_GetGlyphInfo(glyph, &left, &right, &width, &top, &bottom, &height, &pitch);

	if ( glyph->format == ft_glyph_format_outline ) {
		size   = pitch*height;

		bit2 = Mem_Alloc(sizeof(FT_Bitmap));

		bit2->width      = width;
		bit2->rows       = height;
		bit2->pitch      = pitch;
		bit2->pixel_mode = ft_pixel_mode_grays;
		//bit2->pixel_mode = ft_pixel_mode_mono;
		bit2->buffer     = Mem_Alloc(pitch*height);
		bit2->num_grays = 256;

		memset( bit2->buffer, 0, size );

		FT_Outline_Translate( &glyph->outline, -left, -bottom );

		FT_Outline_Get_Bitmap( ftLibrary, &glyph->outline, bit2 );

		glyphOut->height = height;
		glyphOut->pitch = pitch;
		glyphOut->top = (glyph->metrics.horiBearingY >> 6) + 1;
		glyphOut->bottom = bottom;

		return bit2;
	}
	else {
		common->Printf( "Non-outline fonts are not supported\n" );
	}
	return NULL;
}

/*
============
RE_ConstructGlyphInfo
============
*/
glyphInfo_t *RE_ConstructGlyphInfo( unsigned char *imageOut, int *xOut, int *yOut, int *maxHeight, FT_Face face, const unsigned char c, qboolean calcHeight ) {
	int i;
	static glyphInfo_t glyph;
	unsigned char *src, *dst;
	float scaled_width, scaled_height;
	FT_Bitmap *bitmap = NULL;

	memset(&glyph, 0, sizeof(glyphInfo_t));
	// make sure everything is here
	if (face != NULL) {
		FT_Load_Glyph(face, FT_Get_Char_Index( face, c), FT_LOAD_DEFAULT );
		bitmap = R_RenderGlyph(face->glyph, &glyph);
		if (bitmap) {
			glyph.xSkip = (face->glyph->metrics.horiAdvance >> 6) + 1;
		} else {
			return &glyph;
		}

		if (glyph.height > *maxHeight) {
			*maxHeight = glyph.height;
		}

		if (calcHeight) {
			Mem_Free(bitmap->buffer);
			Mem_Free(bitmap);
			return &glyph;
		}

/*
		// need to convert to power of 2 sizes so we do not get
		// any scaling from the gl upload
		for (scaled_width = 1 ; scaled_width < glyph.pitch ; scaled_width<<=1)
			;
		for (scaled_height = 1 ; scaled_height < glyph.height ; scaled_height<<=1)
			;
*/

		scaled_width = glyph.pitch;
		scaled_height = glyph.height;

		// we need to make sure we fit
		if (*xOut + scaled_width + 1 >= 255) {
			if (*yOut + *maxHeight + 1 >= 255) {
				*yOut = -1;
				*xOut = -1;
				Mem_Free(bitmap->buffer);
				Mem_Free(bitmap);
				return &glyph;
			} else {
			*xOut = 0;
			*yOut += *maxHeight + 1;
			}
		} else if (*yOut + *maxHeight + 1 >= 255) {
			*yOut = -1;
			*xOut = -1;
			Mem_Free(bitmap->buffer);
			Mem_Free(bitmap);
			return &glyph;
		}

		src = bitmap->buffer;
		dst = imageOut + (*yOut * 256) + *xOut;

		if (bitmap->pixel_mode == ft_pixel_mode_mono) {
			for (i = 0; i < glyph.height; i++) {
				int j;
				unsigned char *_src = src;
				unsigned char *_dst = dst;
				unsigned char mask = 0x80;
				unsigned char val = *_src;
				for (j = 0; j < glyph.pitch; j++) {
					if (mask == 0x80) {
						val = *_src++;
					}
					if (val & mask) {
						*_dst = 0xff;
					}
					mask >>= 1;

					if ( mask == 0 ) {
						mask = 0x80;
					}
					_dst++;
				}

				src += glyph.pitch;
				dst += 256;

			}
		} else {
			for (i = 0; i < glyph.height; i++) {
				memcpy( dst, src, glyph.pitch );
				src += glyph.pitch;
				dst += 256;
			}
		}

		// we now have an 8 bit per pixel grey scale bitmap
		// that is width wide and pf->ftSize->metrics.y_ppem tall

		glyph.imageHeight = scaled_height;
		glyph.imageWidth = scaled_width;
		glyph.s = (float)*xOut / 256;
		glyph.t = (float)*yOut / 256;
		glyph.s2 = glyph.s + (float)scaled_width / 256;
		glyph.t2 = glyph.t + (float)scaled_height / 256;

		*xOut += scaled_width + 1;
	}

	Mem_Free(bitmap->buffer);
	Mem_Free(bitmap);

	return &glyph;
}

#endif

static int fdOffset;
static byte	*fdFile;

/*
============
readInt
============
*/
int readInt( void ) {
	int i = fdFile[fdOffset]+(fdFile[fdOffset+1]<<8)+(fdFile[fdOffset+2]<<16)+(fdFile[fdOffset+3]<<24);
	fdOffset += 4;
	return i;
}

/*
============
readFloat
============
*/
float readFloat( void ) {
	float f;
	memcpy(&f, &fdFile[fdOffset], sizeof(float));
	fdOffset += sizeof(float);
	return LittleFloat(f);
}

/*
============
R_ParseRavenFontSheet

DUDE: fills one point size of a font from a Quake 4 .fontdat already in fdFile. Raven's
metrics map onto id's glyph fields: horiAdvance is the advance (xSkip), horiBearingY the
height above the baseline (top), horiBearingX the new left bearing; the bitmap size is
both the drawn quad (imageWidth/Height) and the glyph height. Returns the tallest glyph
and widest advance, rounded up to whole pixels, for the font's layout metrics.
============
*/
static void R_ParseRavenFontSheet( const char *fontName, int pointSize, dudeFontInfo_t *outFont, int &maxHeight, int &maxWidth ) {
	const idMaterial *sheet = declManager->FindMaterial( va( "%s_%i.tga", fontName, pointSize ) );
	sheet->SetSort( SS_GUI );

	const char *leaf = fontName;
	for ( const char *c = fontName; *c; c++ ) {
		if ( *c == '/' || *c == '\\' ) {
			leaf = c + 1;
		}
	}

	float mh = 0.0f, mw = 0.0f;
	for ( int i = 0; i < GLYPHS_PER_FONT; i++ ) {
		dudeGlyphInfo_t &g = outFont->glyphs[i];
		const float width		= readFloat();
		const float height		= readFloat();
		const float advance		= readFloat();
		const float bearingX	= readFloat();
		const float bearingY	= readFloat();
		g.s				= readFloat();
		g.t				= readFloat();
		g.s2			= readFloat();
		g.t2			= readFloat();
		g.imageWidth	= width;
		g.imageHeight	= height;
		g.height		= height;
		g.top			= bearingY;
		g.bottom		= height - bearingY;
		g.pitch			= width;
		g.xSkip			= advance;
		g.left			= bearingX;
		g.glyph			= sheet;
		idStr::snPrintf( g.shaderName, sizeof( g.shaderName ), "%s_%i.tga", leaf, pointSize );
		if ( i < GLYPH_END ) {			// same range id's loader measures
			mh = Max( mh, height );
			mw = Max( mw, advance );
		}
	}

	const float filePointSize = readFloat();	// then fontHeight, ascender, descender, padding: unused
	if ( idMath::Fabs( filePointSize - pointSize ) > 0.5f ) {
		common->Warning( "RegisterFont: '%s_%i.fontdat' says it is %g pt", fontName, pointSize, filePointSize );
	}

	// id's sheets carry glyphScale in the file; Raven's do not. It is the same
	// 48 / point size id uses (the GUI is authored against the 48 pt sheet).
	outFont->glyphScale = 48.0f / pointSize;
	maxHeight = (int)ceilf( mh );
	maxWidth = (int)ceilf( mw );
}

/*
============
R_RegisterEngineFont

Loads 3 point sizes, 12, 24, and 48, into the engine's float-metric font
(renderer/FontInternal.h): id's fontImage_<pt>.dat or Raven's <font>_<pt>.fontdat.
============
*/
bool R_RegisterEngineFont( const char *fontName, dudeFontInfoEx_t &font ) {
	void *faceData;
	ID_TIME_T ftime;
	int i, len, fontCount;
	char name[1024];

	int pointSize = 12;
/*
	if ( registeredFontCount >= MAX_FONTS ) {
		common->Warning( "RegisterFont: Too many fonts registered already." );
		return false;
	}

	int pointSize = 12;
	idStr::snPrintf( name, sizeof(name), "%s/fontImage_%i.dat", fontName, pointSize );
	for ( i = 0; i < registeredFontCount; i++ ) {
		if ( idStr::Icmp(name, registeredFont[i].fontInfoSmall.name) == 0 ) {
			memcpy( &font, &registeredFont[i], sizeof( fontInfoEx_t ) );
			return true;
		}
	}
*/
	assert(sizeof(float) == 4 && "if this is false, readFloat() won't work");

	memset( &font, 0, sizeof( font ) );

	for ( fontCount = 0; fontCount < 3; fontCount++ ) {

		if ( fontCount == 0) {
			pointSize = 12;
		} else if ( fontCount == 1 ) {
			pointSize = 24;
		} else {
			pointSize = 48;
		}
		// we also need to adjust the scale based on point size relative to 48 points as the ui scaling is based on a 48 point font
		float glyphScale = 1.0f;		// change the scale to be relative to 1 based on 72 dpi ( so dpi of 144 means a scale of .5 )
		glyphScale *= 48.0f / pointSize;

		idStr::snPrintf( name, sizeof(name), "%s/fontImage_%i.dat", fontName, pointSize );

		// DUDE: id's fontImage_<pt>.dat first; if it is not there, Raven's Quake 4 layout
		bool ravenFormat = false;
		len = fileSystem->ReadFile( name, NULL, &ftime );
		if ( len != FILESIZE_fontInfo_t ) {
			char ravenName[1024];
			idStr::snPrintf( ravenName, sizeof(ravenName), "%s_%i.fontdat", fontName, pointSize );
			if ( fileSystem->ReadFile( ravenName, NULL, &ftime ) == FILESIZE_ravenFontInfo_t ) {
				idStr::Copynz( name, ravenName, sizeof( name ) );
				ravenFormat = true;
			}
		}

		dudeFontInfo_t *outFont;
		if ( fontCount == 0 ) {
			outFont = &font.fontInfoSmall;
		}
		else if ( fontCount == 1 ) {
			outFont = &font.fontInfoMedium;
		}
		else {
			outFont = &font.fontInfoLarge;
		}

		idStr::Copynz( outFont->name, name, sizeof( outFont->name ) );

		len = fileSystem->ReadFile( name, NULL, &ftime );
		if ( len != ( ravenFormat ? FILESIZE_ravenFontInfo_t : FILESIZE_fontInfo_t ) ) {
			common->Warning( "RegisterFont: couldn't find font: '%s' (nor %s_%i.fontdat)", name, fontName, pointSize );
			return false;
		}

		fileSystem->ReadFile( name, &faceData, &ftime );
		fdOffset = 0;
		fdFile = reinterpret_cast<unsigned char*>(faceData);

		if ( ravenFormat ) {
			int mh = 0, mw = 0;
			R_ParseRavenFontSheet( fontName, pointSize, outFont, mh, mw );
			if ( fontCount == 0 ) {
				font.maxWidthSmall = mw;
				font.maxHeightSmall = mh;
			} else if ( fontCount == 1 ) {
				font.maxWidthMedium = mw;
				font.maxHeightMedium = mh;
			} else {
				font.maxWidthLarge = mw;
				font.maxHeightLarge = mh;
			}
			fileSystem->FreeFile( faceData );
			continue;
		}

		for( i = 0; i < GLYPHS_PER_FONT; i++ ) {
			outFont->glyphs[i].height		= readInt();
			outFont->glyphs[i].top			= readInt();
			outFont->glyphs[i].bottom		= readInt();
			outFont->glyphs[i].pitch		= readInt();
			outFont->glyphs[i].xSkip		= readInt();
			outFont->glyphs[i].imageWidth	= readInt();
			outFont->glyphs[i].imageHeight	= readInt();
			outFont->glyphs[i].s			= readFloat();
			outFont->glyphs[i].t			= readFloat();
			outFont->glyphs[i].s2			= readFloat();
			outFont->glyphs[i].t2			= readFloat();
			outFont->glyphs[i].left			= 0.0f;		// DUDE: id fonts have no bearing
			/* font.glyphs[i].glyph			= */ readInt();
			//FIXME: the +6, -6 skips the embedded fonts/
			memcpy( outFont->glyphs[i].shaderName, &fdFile[fdOffset + 6], 32 - 6 );
			fdOffset += 32;
		}
		outFont->glyphScale = readFloat();

		int mw = 0;
		int mh = 0;
		for (i = GLYPH_START; i < GLYPH_END; i++) {
			idStr::snPrintf(name, sizeof(name), "%s/%s", fontName, outFont->glyphs[i].shaderName);
			outFont->glyphs[i].glyph = declManager->FindMaterial(name);
			outFont->glyphs[i].glyph->SetSort( SS_GUI );
			// DUDE: metrics are float now; id's are whole pixels, so this is exact
			if (mh < outFont->glyphs[i].height) {
				mh = (int)outFont->glyphs[i].height;
			}
			if (mw < outFont->glyphs[i].xSkip) {
				mw = (int)outFont->glyphs[i].xSkip;
			}
		}
		if (fontCount == 0) {
			font.maxWidthSmall = mw;
			font.maxHeightSmall = mh;
		} else if (fontCount == 1) {
			font.maxWidthMedium = mw;
			font.maxHeightMedium = mh;
		} else {
			font.maxWidthLarge = mw;
			font.maxHeightLarge = mh;
		}
		fileSystem->FreeFile( faceData );
	}

	//memcpy( &registeredFont[registeredFontCount++], &font, sizeof( fontInfoEx_t ) );

	return true ;
}

/*
============
R_GlyphMetricToInt

DUDE: a float glyph metric in the public, whole-pixel glyphInfo_t; exact for id's fonts
============
*/
static int R_GlyphMetricToInt( float v ) {
	return (int)( v >= 0.0f ? v + 0.5f : v - 0.5f );
}

static void R_FontInfoToPublic( const dudeFontInfo_t &in, fontInfo_t &out ) {
	for ( int i = 0; i < GLYPHS_PER_FONT; i++ ) {
		const dudeGlyphInfo_t &g = in.glyphs[i];
		glyphInfo_t &o = out.glyphs[i];
		o.height		= R_GlyphMetricToInt( g.height );
		o.top			= R_GlyphMetricToInt( g.top );
		o.bottom		= R_GlyphMetricToInt( g.bottom );
		o.pitch			= R_GlyphMetricToInt( g.pitch );
		o.xSkip			= R_GlyphMetricToInt( g.xSkip );
		o.imageWidth	= R_GlyphMetricToInt( g.imageWidth );
		o.imageHeight	= R_GlyphMetricToInt( g.imageHeight );
		o.s				= g.s;
		o.t				= g.t;
		o.s2			= g.s2;
		o.t2			= g.t2;
		o.glyph			= g.glyph;
		memcpy( o.shaderName, g.shaderName, sizeof( o.shaderName ) );
	}
	out.glyphScale = in.glyphScale;
	memcpy( out.name, in.name, sizeof( out.name ) );
}

/*
============
RegisterFont

Loads 3 point sizes, 12, 24, and 48
============
*/
bool idRenderSystemLocal::RegisterFont( const char *fontName, fontInfoEx_t &font ) {
#ifdef BUILD_FREETYPE
	FT_Face face;
	int j, k, xOut, yOut, lastStart, imageNumber;
	int scaledSize, newSize, maxHeight, left, satLevels;
	unsigned char *out, *imageBuff;
	glyphInfo_t *glyph;
	idImage *image;
	idMaterial *h;
	float max;
#endif

	// DUDE: the font loads into the engine's float-metric form (R_RegisterEngineFont); a game
	// library gets dhewm3's whole-pixel layout of it (tools/check_dhewm3_abi.py)
	dudeFontInfoEx_t *engineFont = (dudeFontInfoEx_t *)Mem_ClearedAlloc( sizeof( *engineFont ) );
	const bool loaded = R_RegisterEngineFont( fontName, *engineFont );
	memset( &font, 0, sizeof( font ) );
	if ( loaded ) {
		R_FontInfoToPublic( engineFont->fontInfoSmall, font.fontInfoSmall );
		R_FontInfoToPublic( engineFont->fontInfoMedium, font.fontInfoMedium );
		R_FontInfoToPublic( engineFont->fontInfoLarge, font.fontInfoLarge );
		font.maxHeight = engineFont->maxHeight;
		font.maxWidth = engineFont->maxWidth;
		font.maxHeightSmall = engineFont->maxHeightSmall;
		font.maxWidthSmall = engineFont->maxWidthSmall;
		font.maxHeightMedium = engineFont->maxHeightMedium;
		font.maxWidthMedium = engineFont->maxWidthMedium;
		font.maxHeightLarge = engineFont->maxHeightLarge;
		font.maxWidthLarge = engineFont->maxWidthLarge;
		memcpy( font.name, engineFont->name, sizeof( font.name ) );
	}
	Mem_Free( engineFont );
	return loaded;

#ifndef BUILD_FREETYPE
	common->Warning( "RegisterFont: couldn't load FreeType code %s", fontName );
#else

	if (ftLibrary == NULL) {
		common->Warning( "RegisterFont: FreeType not initialized." );
		return;
	}

	len = fileSystem->ReadFile(fontName, &faceData, &ftime);
	if ( len <= 0 ) {
		common->Warning( "RegisterFont: Unable to read font file" );
		return;
	}

	// allocate on the stack first in case we fail
	if ( FT_New_Memory_Face( ftLibrary, faceData, len, 0, &face ) ) {
		common->Warning( "RegisterFont: FreeType2, unable to allocate new face." );
		return;
	}


	if ( FT_Set_Char_Size( face, pointSize << 6, pointSize << 6, dpi, dpi) ) {
		common->Warning( "RegisterFont: FreeType2, Unable to set face char size." );
		return;
	}

	// font = registeredFonts[registeredFontCount++];

	// make a 256x256 image buffer, once it is full, register it, clean it and keep going
	// until all glyphs are rendered

	out = Mem_Alloc( 1024*1024 );
	if ( out == NULL ) {
		common->Warning( "RegisterFont: Mem_Alloc failure during output image creation." );
		return;
	}
	memset( out, 0, 1024*1024 );

	maxHeight = 0;

	for (i = GLYPH_START; i < GLYPH_END; i++) {
		glyph = RE_ConstructGlyphInfo(out, &xOut, &yOut, &maxHeight, face, (unsigned char)i, qtrue);
	}

	xOut = 0;
	yOut = 0;
	i = GLYPH_START;
	lastStart = i;
	imageNumber = 0;

	while ( i <= GLYPH_END ) {

		glyph = RE_ConstructGlyphInfo(out, &xOut, &yOut, &maxHeight, face, (unsigned char)i, qfalse);

		if (xOut == -1 || yOut == -1 || i == GLYPH_END)  {
			// ran out of room
			// we need to create an image from the bitmap, set all the handles in the glyphs to this point
			//

			scaledSize = 256*256;
			newSize = scaledSize * 4;
			imageBuff = Mem_Alloc(newSize);
			left = 0;
			max = 0;
			satLevels = 255;
			for ( k = 0; k < (scaledSize) ; k++ ) {
				if (max < out[k]) {
					max = out[k];
				}
			}

			if (max > 0) {
				max = 255/max;
			}

			for ( k = 0; k < (scaledSize) ; k++ ) {
				imageBuff[left++] = 255;
				imageBuff[left++] = 255;
				imageBuff[left++] = 255;
				imageBuff[left++] = ((float)out[k] * max);
			}

			idStr::snprintf( name, sizeof(name), "fonts/fontImage_%i_%i.tga", imageNumber++, pointSize );
			if (r_saveFontData->integer) {
				R_WriteTGA(name, imageBuff, 256, 256);
			}

			//idStr::snprintf( name, sizeof(name), "fonts/fontImage_%i_%i", imageNumber++, pointSize );
			image = R_CreateImage(name, imageBuff, 256, 256, qfalse, qfalse, GL_CLAMP);
			h = RE_RegisterShaderFromImage(name, LIGHTMAP_2D, image, qfalse);
			for (j = lastStart; j < i; j++) {
				font.glyphs[j].glyph = h;
				idStr::Copynz( font.glyphs[j].shaderName, name, sizeof( font.glyphs[j].shaderName ) );
			}
			lastStart = i;
			memset( out, 0, 1024*1024 );
			xOut = 0;
			yOut = 0;
			Mem_Free( imageBuff );
			i++;
		} else {
			memcpy( &font.glyphs[i], glyph, sizeof( glyphInfo_t ) );
			i++;
		}
	}

	registeredFont[registeredFontCount].glyphScale = glyphScale;
	font.glyphScale = glyphScale;
	memcpy( &registeredFont[registeredFontCount++], &font, sizeof( fontInfo_t ) );

	if ( r_saveFontData->integer ) {
		common->Warning( "FIXME: font saving doesnt respect alignment!" );
		fileSystem->WriteFile( va( "fonts/fontImage_%i.dat", pointSize), &font, sizeof( fontInfo_t ) );
	}

	Mem_Free( out );

	fileSystem->FreeFile( faceData );
#endif
	return true;
}

/*
============
R_InitFreeType
============
*/
void R_InitFreeType( void ) {
#ifdef BUILD_FREETYPE
	if ( FT_Init_FreeType( &ftLibrary ) ) {
		common->Printf( "R_InitFreeType: Unable to initialize FreeType.\n" );
	}
#endif
//	registeredFontCount = 0;
}

/*
============
R_DoneFreeType
============
*/
void R_DoneFreeType( void ) {
#ifdef BUILD_FREETYPE
	if ( ftLibrary ) {
		FT_Done_FreeType( ftLibrary );
		ftLibrary = NULL;
	}
#endif
//	registeredFontCount = 0;
}
