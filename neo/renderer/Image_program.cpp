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
#include "idlib/containers/HashTable.h"	// DUDE: parallax height source table
#include "renderer/tr_local.h"

#include "renderer/Image.h"

/*

all uncompressed
uncompressed normal maps

downsample images

16 meg Dynamic cache

Anisotropic texturing

Trilinear on all
Trilinear on normal maps, bilinear on others
Bilinear on all


Manager

->List
->Print
->Reload( bool force )

Anywhere that an image name is used (diffusemaps, bumpmaps, specularmaps, lights, etc),
an imageProgram can be specified.

This allows load time operations, like heightmap-to-normalmap conversion and image
composition, to be automatically handled in a way that supports timestamped reloads.

*/

/*
=================
R_HeightmapToNormalMap

it is not possible to convert a heightmap into a normal map
properly without knowing the texture coordinate stretching.
We can assume constant and equal ST vectors for walls, but not for characters.
=================
*/
static void R_HeightmapToNormalMap( byte *data, int width, int height, float scale ) {
	int		i, j;
	byte	*depth;

	scale = scale / 256;

	// copy and convert to grey scale
	j = width * height;
	depth = (byte *)R_StaticAlloc( j );
	for ( i = 0 ; i < j ; i++ ) {
		depth[i] = ( data[i*4] + data[i*4+1] + data[i*4+2] ) / 3;
	}

	idVec3	dir, dir2;
	for ( i = 0 ; i < height ; i++ ) {
		for ( j = 0 ; j < width ; j++ ) {
			int		d1, d2, d3, d4;
			int		a1, a3, a4;

			// FIXME: look at five points?

			// look at three points to estimate the gradient
			a1 = d1 = depth[ ( i * width + j ) ];
			d2 = depth[ ( i * width + ( ( j + 1 ) & ( width - 1 ) ) ) ];
			a3 = d3 = depth[ ( ( ( i + 1 ) & ( height - 1 ) ) * width + j ) ];
			a4 = d4 = depth[ ( ( ( i + 1 ) & ( height - 1 ) ) * width + ( ( j + 1 ) & ( width - 1 ) ) ) ];

			d2 -= d1;
			d3 -= d1;

			dir[0] = -d2 * scale;
			dir[1] = -d3 * scale;
			dir[2] = 1;
			dir.NormalizeFast();

			a1 -= a3;
			a4 -= a3;

			dir2[0] = -a4 * scale;
			dir2[1] = a1 * scale;
			dir2[2] = 1;
			dir2.NormalizeFast();

			dir += dir2;
			dir.NormalizeFast();

			a1 = ( i * width + j ) * 4;
			data[ a1 + 0 ] = (byte)(dir[0] * 127 + 128);
			data[ a1 + 1 ] = (byte)(dir[1] * 127 + 128);
			data[ a1 + 2 ] = (byte)(dir[2] * 127 + 128);
			data[ a1 + 3 ] = 255;
		}
	}


	R_StaticFree( depth );
}


/*
=================
R_ImageScale
=================
*/
static void R_ImageScale( byte *data, int width, int height, float scale[4] ) {
	int		i, j;
	int		c;

	c = width * height * 4;

	for ( i = 0 ; i < c ; i++ ) {
		j = (byte)(data[i] * scale[i&3]);
		if ( j < 0 ) {
			j = 0;
		} else if ( j > 255 ) {
			j = 255;
		}
		data[i] = j;
	}
}

/*
=================
R_InvertAlpha
=================
*/
static void R_InvertAlpha( byte *data, int width, int height ) {
	int		i;
	int		c;

	c = width * height* 4;

	for ( i = 0 ; i < c ; i+=4 ) {
		data[i+3] = 255 - data[i+3];
	}
}

/*
=================
R_InvertColor
=================
*/
static void R_InvertColor( byte *data, int width, int height ) {
	int		i;
	int		c;

	c = width * height* 4;

	for ( i = 0 ; i < c ; i+=4 ) {
		data[i+0] = 255 - data[i+0];
		data[i+1] = 255 - data[i+1];
		data[i+2] = 255 - data[i+2];
	}
}


/*
===================
R_AddNormalMaps

===================
*/
static void R_AddNormalMaps( byte *data1, int width1, int height1, byte *data2, int width2, int height2 ) {
	int		i, j;
	byte	*newMap;

	// resample pic2 to the same size as pic1
	if ( width2 != width1 || height2 != height1 ) {
		newMap = R_Dropsample( data2, width2, height2, width1, height1 );
		data2 = newMap;
	} else {
		newMap = NULL;
	}

	// add the normal change from the second and renormalize
	for ( i = 0 ; i < height1 ; i++ ) {
		for ( j = 0 ; j < width1 ; j++ ) {
			byte	*d1, *d2;
			idVec3	n;
			float   len;

			d1 = data1 + ( i * width1 + j ) * 4;
			d2 = data2 + ( i * width1 + j ) * 4;

			n[0] = ( d1[0] - 128 ) / 127.0;
			n[1] = ( d1[1] - 128 ) / 127.0;
			n[2] = ( d1[2] - 128 ) / 127.0;

			// There are some normal maps that blend to 0,0,0 at the edges
			// this screws up compression, so we try to correct that here by instead fading it to 0,0,1
			len = n.LengthFast();
			if ( len < 1.0f ) {
				n[2] = idMath::Sqrt(1.0 - (n[0]*n[0]) - (n[1]*n[1]));
			}

			n[0] += ( d2[0] - 128 ) / 127.0;
			n[1] += ( d2[1] - 128 ) / 127.0;
			n.Normalize();

			d1[0] = (byte)(n[0] * 127 + 128);
			d1[1] = (byte)(n[1] * 127 + 128);
			d1[2] = (byte)(n[2] * 127 + 128);
			d1[3] = 255;
		}
	}

	if ( newMap ) {
		R_StaticFree( newMap );
	}
}

/*
================
R_SmoothNormalMap
================
*/
static void R_SmoothNormalMap( byte *data, int width, int height ) {
	byte	*orig;
	int		i, j, k, l;
	idVec3	normal;
	byte	*out;
	static float	factors[3][3] = {
		{ 1, 1, 1 },
		{ 1, 1, 1 },
		{ 1, 1, 1 }
	};

	orig = (byte *)R_StaticAlloc( width * height * 4 );
	memcpy( orig, data, width * height * 4 );

	for ( i = 0 ; i < width ; i++ ) {
		for ( j = 0 ; j < height ; j++ ) {
			normal = vec3_origin;
			for ( k = -1 ; k < 2 ; k++ ) {
				for ( l = -1 ; l < 2 ; l++ ) {
					byte	*in;

					in = orig + ( ((j+l)&(height-1))*width + ((i+k)&(width-1)) ) * 4;

					// ignore 000 and -1 -1 -1
					if ( in[0] == 0 && in[1] == 0 && in[2] == 0 ) {
						continue;
					}
					if ( in[0] == 128 && in[1] == 128 && in[2] == 128 ) {
						continue;
					}

					normal[0] += factors[k+1][l+1] * ( in[0] - 128 );
					normal[1] += factors[k+1][l+1] * ( in[1] - 128 );
					normal[2] += factors[k+1][l+1] * ( in[2] - 128 );
				}
			}
			normal.Normalize();
			out = data + ( j * width + i ) * 4;
			out[0] = (byte)(128 + 127 * normal[0]);
			out[1] = (byte)(128 + 127 * normal[1]);
			out[2] = (byte)(128 + 127 * normal[2]);
		}
	}

	R_StaticFree( orig );
}


/*
===================
R_ImageAdd

===================
*/
static void R_ImageAdd( byte *data1, int width1, int height1, byte *data2, int width2, int height2 ) {
	int		i, j;
	int		c;
	byte	*newMap;

	// resample pic2 to the same size as pic1
	if ( width2 != width1 || height2 != height1 ) {
		newMap = R_Dropsample( data2, width2, height2, width1, height1 );
		data2 = newMap;
	} else {
		newMap = NULL;
	}


	c = width1 * height1 * 4;

	for ( i = 0 ; i < c ; i++ ) {
		j = data1[i] + data2[i];
		if ( j > 255 ) {
			j = 255;
		}
		data1[i] = j;
	}

	if ( newMap ) {
		R_StaticFree( newMap );
	}
}


// we build a canonical token form of the image program here
static char parseBuffer[MAX_IMAGE_NAME];

/*
===================
AppendToken
===================
*/
static void AppendToken( idToken &token ) {
	// add a leading space if not at the beginning
	if ( parseBuffer[0] ) {
		idStr::Append( parseBuffer, MAX_IMAGE_NAME, " " );
	}
	idStr::Append( parseBuffer, MAX_IMAGE_NAME, token.c_str() );
}

/*
===================
MatchAndAppendToken
===================
*/
static void MatchAndAppendToken( idLexer &src, const char *match ) {
	if ( !src.ExpectTokenString( match ) ) {
		return;
	}
	// a matched token won't need a leading space
	idStr::Append( parseBuffer, MAX_IMAGE_NAME, match );
}

/*
===================
R_ParseImageProgram_r

If pic is NULL, the timestamps will be filled in, but no image will be generated
If both pic and timestamps are NULL, it will just advance past it, which can be
used to parse an image program from a text stream.
===================
*/
static bool R_ParseImageProgram_r( idLexer &src, byte **pic, int *width, int *height,
								  ID_TIME_T *timestamps, textureDepth_t *depth ) {
	idToken		token;
	float		scale;
	ID_TIME_T		timestamp;

	src.ReadToken( &token );
	AppendToken( token );

	if ( !token.Icmp( "heightmap" ) ) {
		MatchAndAppendToken( src, "(" );

		if ( !R_ParseImageProgram_r( src, pic, width, height, timestamps, depth ) ) {
			return false;
		}

		MatchAndAppendToken( src, "," );

		src.ReadToken( &token );
		AppendToken( token );
		scale = token.GetFloatValue();

		// process it
		if ( pic ) {
			R_HeightmapToNormalMap( *pic, *width, *height, scale );
			if ( depth ) {
				*depth = TD_BUMP;
			}
		}

		MatchAndAppendToken( src, ")" );
		return true;
	}

	if ( !token.Icmp( "addnormals" ) ) {
		byte	*pic2 = NULL;
		int		width2, height2;

		MatchAndAppendToken( src, "(" );

		if ( !R_ParseImageProgram_r( src, pic, width, height, timestamps, depth ) ) {
			return false;
		}

		MatchAndAppendToken( src, "," );

		if ( !R_ParseImageProgram_r( src, pic ? &pic2 : NULL, &width2, &height2, timestamps, depth ) ) {
			if ( pic ) {
				R_StaticFree( *pic );
				*pic = NULL;
			}
			return false;
		}

		// process it
		if ( pic ) {
			R_AddNormalMaps( *pic, *width, *height, pic2, width2, height2 );
			R_StaticFree( pic2 );
			if ( depth ) {
				*depth = TD_BUMP;
			}
		}

		MatchAndAppendToken( src, ")" );
		return true;
	}

	if ( !token.Icmp( "smoothnormals" ) ) {
		MatchAndAppendToken( src, "(" );

		if ( !R_ParseImageProgram_r( src, pic, width, height, timestamps, depth ) ) {
			return false;
		}

		if ( pic ) {
			R_SmoothNormalMap( *pic, *width, *height );
			if ( depth ) {
				*depth = TD_BUMP;
			}
		}

		MatchAndAppendToken( src, ")" );
		return true;
	}

	if ( !token.Icmp( "add" ) ) {
		byte	*pic2 = NULL;
		int		width2, height2;

		MatchAndAppendToken( src, "(" );

		if ( !R_ParseImageProgram_r( src, pic, width, height, timestamps, depth ) ) {
			return false;
		}

		MatchAndAppendToken( src, "," );

		if ( !R_ParseImageProgram_r( src, pic ? &pic2 : NULL, &width2, &height2, timestamps, depth ) ) {
			if ( pic ) {
				R_StaticFree( *pic );
				*pic = NULL;
			}
			return false;
		}

		// process it
		if ( pic ) {
			R_ImageAdd( *pic, *width, *height, pic2, width2, height2 );
			R_StaticFree( pic2 );
		}

		MatchAndAppendToken( src, ")" );
		return true;
	}

	if ( !token.Icmp( "scale" ) ) {
		float	scale[4];
		int		i;

		MatchAndAppendToken( src, "(" );

		R_ParseImageProgram_r( src, pic, width, height, timestamps, depth );

		for ( i = 0 ; i < 4 ; i++ ) {
			MatchAndAppendToken( src, "," );
			src.ReadToken( &token );
			AppendToken( token );
			scale[i] = token.GetFloatValue();
		}

		// process it
		if ( pic ) {
			R_ImageScale( *pic, *width, *height, scale );
		}

		MatchAndAppendToken( src, ")" );
		return true;
	}

	if ( !token.Icmp( "invertAlpha" ) ) {
		MatchAndAppendToken( src, "(" );

		R_ParseImageProgram_r( src, pic, width, height, timestamps, depth );

		// process it
		if ( pic ) {
			R_InvertAlpha( *pic, *width, *height );
		}

		MatchAndAppendToken( src, ")" );
		return true;
	}

	if ( !token.Icmp( "invertColor" ) ) {
		MatchAndAppendToken( src, "(" );

		R_ParseImageProgram_r( src, pic, width, height, timestamps, depth );

		// process it
		if ( pic ) {
			R_InvertColor( *pic, *width, *height );
		}

		MatchAndAppendToken( src, ")" );
		return true;
	}

	if ( !token.Icmp( "makeIntensity" ) ) {
		int		i;

		MatchAndAppendToken( src, "(" );

		R_ParseImageProgram_r( src, pic, width, height, timestamps, depth );

		// copy red to green, blue, and alpha
		if ( pic ) {
			int		c;
			c = *width * *height * 4;
			for ( i = 0 ; i < c ; i+=4 ) {
				(*pic)[i+1] =
				(*pic)[i+2] =
				(*pic)[i+3] = (*pic)[i];
			}
		}

		MatchAndAppendToken( src, ")" );
		return true;
	}

	if ( !token.Icmp( "makeAlpha" ) ) {
		int		i;

		MatchAndAppendToken( src, "(" );

		R_ParseImageProgram_r( src, pic, width, height, timestamps, depth );

		// average RGB into alpha, then set RGB to white
		if ( pic ) {
			int		c;
			c = *width * *height * 4;
			for ( i = 0 ; i < c ; i+=4 ) {
				(*pic)[i+3] = ( (*pic)[i+0] + (*pic)[i+1] + (*pic)[i+2] ) / 3;
				(*pic)[i+0] =
				(*pic)[i+1] =
				(*pic)[i+2] = 255;
			}
		}

		MatchAndAppendToken( src, ")" );
		return true;
	}

	// if we are just parsing instead of loading or checking,
	// don't do the R_LoadImage
	if ( !timestamps && !pic ) {
		return true;
	}

	// load it as an image
	R_LoadImage( token.c_str(), pic, width, height, &timestamp, true );

	if ( timestamp == FILE_NOT_FOUND_TIMESTAMP ) {
		return false;
	}

	// add this to the timestamp
	if ( timestamps ) {
		if ( timestamp > *timestamps ) {
			*timestamps = timestamp;
		}
	}

	return true;
}


/*
===================
R_LoadImageProgram
===================
*/
void R_LoadImageProgram( const char *name, byte **pic, int *width, int *height, ID_TIME_T *timestamps, textureDepth_t *depth ) {
	idLexer src;

	src.LoadMemory( name, strlen(name), name );
	src.SetFlags( LEXFL_NOFATALERRORS | LEXFL_NOSTRINGCONCAT | LEXFL_NOSTRINGESCAPECHARS | LEXFL_ALLOWPATHNAMES );

	parseBuffer[0] = 0;
	if ( timestamps ) {
		*timestamps = 0;
	}

	R_ParseImageProgram_r( src, pic, width, height, timestamps, depth );

	src.FreeSource();
}

/*
===================
R_ParsePastImageProgram
===================
*/
const char *R_ParsePastImageProgram( idLexer &src ) {
	parseBuffer[0] = 0;
	R_ParseImageProgram_r( src, NULL, NULL, NULL, NULL, NULL );
	return parseBuffer;
}

/*
===============================================================================

	DUDE: parallax height maps from normal maps (docs/parallax.md)

	Doom 3 keeps its fine surface detail in the *normal* map (the `_local`
	component of `addnormals(...)`), not in the coarse `_h` height source. To
	make parallax occlusion mapping follow the detail the player actually sees
	(the same map SSAO shows), recover a scalar height field by inverting the
	R_HeightmapToNormalMap stencil above and marching that instead.

===============================================================================
*/

// name (the generated image) -> source bump image program, so the generator can
// re-load the normal map when the image is (re)built (level load, vid_restart).
static idHashTable<idStr>	r_parallaxSources;

/*
===============
R_BoxBlurAxis

Separable box blur of a float plane along one axis, via a running sum, with
WRAP-AROUND edges (the height field tiles, so the low-pass must too, else the
high-pass leaves a seam at the texture border). radius texels each side.
===============
*/
static void R_BoxBlurAxis( float *buf, float *tmp, int width, int height, int radius, bool horizontal ) {
	const int major = horizontal ? height : width;		// lines to sweep
	const int minor = horizontal ? width : height;		// length of each line
	const int step  = horizontal ? 1 : width;			// stride within a line
	const float inv = 1.0f / ( 2 * radius + 1 );
	for ( int m = 0; m < major; m++ ) {
		const int base = horizontal ? m * width : m;
		// initial window sum [-radius, radius], wrapped
		float sum = 0.0f;
		for ( int k = -radius; k <= radius; k++ ) {
			int idx = ( ( k % minor ) + minor ) % minor;
			sum += buf[ base + idx * step ];
		}
		for ( int i = 0; i < minor; i++ ) {
			tmp[ base + i * step ] = sum * inv;
			int add = ( ( ( i + radius + 1 ) % minor ) + minor ) % minor;
			int sub = ( ( ( i - radius ) % minor ) + minor ) % minor;
			sum += buf[ base + add * step ] - buf[ base + sub * step ];
		}
	}
	memcpy( buf, tmp, width * height * sizeof( float ) );
}

/*
===============
R_NormalMapToHeightMap

Invert R_HeightmapToNormalMap: recover a height field from a tangent-space normal
map. Per texel the surface slope is sx = -nx/nz, sy = -ny/nz (the inverse of the
forward stencil). Rather than a cheap directional integration -- which streaks
along the integration axis and does not tile -- solve the Poisson equation
laplacian(H) = div(slope) by Jacobi relaxation with WRAP-AROUND boundaries: the
result is isotropic (no streaks) and seamless (tiles), which matters because
self-shadowing turns any height step at a tile seam into a dark line. A high-pass
(wrap-aware box blur, subtracted) then keeps only local relief -- the low
frequencies the relaxation hasn't resolved are exactly what we discard anyway, so
a modest iteration count suffices. Overwrites data[] with grayscale height (a=255).
===============
*/
static void R_NormalMapToHeightMap( byte *data, int width, int height ) {
	const int n = width * height;
	float *f   = (float *)R_StaticAlloc( n * sizeof( float ) );	// divergence of the slope field
	float *h   = (float *)R_StaticAlloc( n * sizeof( float ) );	// solution (ping)
	float *h2  = (float *)R_StaticAlloc( n * sizeof( float ) );	// solution (pong)

	// slope divergence f = d(sx)/dx + d(sy)/dy, backward differences, wrapped. sx,sy are
	// recomputed on the fly from the normal (sx = -nx/nz, sy = -ny/nz) to avoid two more
	// full-size buffers.
	#define PX_SLOPE_X( px ) ( -( ( data[(px)*4+0] - 128 ) / 127.0f ) / PX_NZ(px) )
	#define PX_SLOPE_Y( px ) ( -( ( data[(px)*4+1] - 128 ) / 127.0f ) / PX_NZ(px) )
	#define PX_NZ( px ) ( ( ( data[(px)*4+2] - 128 ) / 127.0f ) < 0.05f ? 0.05f : ( ( data[(px)*4+2] - 128 ) / 127.0f ) )
	for ( int i = 0; i < height; i++ ) {
		const int iu = ( ( i - 1 + height ) % height ) * width;
		const int ic = i * width;
		for ( int j = 0; j < width; j++ ) {
			const int jl = ( j - 1 + width ) % width;
			int p = ic + j;
			f[p] = ( PX_SLOPE_X( p ) - PX_SLOPE_X( ic + jl ) )
			     + ( PX_SLOPE_Y( p ) - PX_SLOPE_Y( iu + j ) );
			h[p] = 0.0f;
		}
	}
	#undef PX_SLOPE_X
	#undef PX_SLOPE_Y
	#undef PX_NZ

	// Jacobi: H[p] = ( sum of 4 wrapped neighbours - f[p] ) / 4. High frequencies converge
	// in a handful of sweeps; the low frequencies (slow to converge) are high-passed out.
	const int iterations = 64;
	for ( int it = 0; it < iterations; it++ ) {
		for ( int i = 0; i < height; i++ ) {
			const int ic = i * width;
			const int iu = ( ( i - 1 + height ) % height ) * width;
			const int id = ( ( i + 1 ) % height ) * width;
			for ( int j = 0; j < width; j++ ) {
				const int jl = ( j - 1 + width ) % width;
				const int jr = ( j + 1 ) % width;
				h2[ ic + j ] = 0.25f * ( h[ ic + jl ] + h[ ic + jr ] + h[ iu + j ] + h[ id + j ] - f[ ic + j ] );
			}
		}
		float *sw = h; h = h2; h2 = sw;
	}
	R_StaticFree( f );

	// high-pass: subtract a large wrap-aware box blur (two passes ~ Gaussian). Radius ~1/6
	// of the smaller dimension keeps panel-scale relief while dropping the low frequencies.
	int radius = ( width < height ? width : height ) / 6;
	if ( radius < 1 ) { radius = 1; }
	float *low = h2;					// reuse the pong buffer for the low-pass
	memcpy( low, h, n * sizeof( float ) );
	float *scratch = (float *)R_StaticAlloc( n * sizeof( float ) );
	for ( int pass = 0; pass < 2; pass++ ) {
		R_BoxBlurAxis( low, scratch, width, height, radius, true );
		R_BoxBlurAxis( low, scratch, width, height, radius, false );
	}
	R_StaticFree( scratch );

	// H = local relief; find range for normalization
	float mn = 1e30f, mx = -1e30f;
	for ( int i = 0; i < n; i++ ) {
		h[i] -= low[i];
		if ( h[i] < mn ) { mn = h[i]; }
		if ( h[i] > mx ) { mx = h[i]; }
	}
	float range = mx - mn;
	if ( range < 1e-5f ) { range = 1.0f; }

	for ( int i = 0; i < n; i++ ) {
		int v = (int)( ( h[i] - mn ) / range * 255.0f + 0.5f );
		v = v < 0 ? 0 : ( v > 255 ? 255 : v );
		data[ i*4 + 0 ] = data[ i*4 + 1 ] = data[ i*4 + 2 ] = (byte)v;
		data[ i*4 + 3 ] = 255;
	}

	R_StaticFree( h2 );
	R_StaticFree( h );
}

/*
===============
R_ParallaxHeightImage

Generator: load the source bump program's *combined* normal map and convert it to
a height field (docs/parallax.md). Re-runs on level load / vid_restart like any
other generated image. Falls back to a flat mid-gray (no relief) if the source is
unknown or fails to load.
===============
*/
static void R_ParallaxHeightImage( idImage *image ) {
	idStr *bumpProgram = NULL;
	byte  *pic = NULL;
	int    w = 0, hgt = 0;

	if ( r_parallaxSources.Get( image->imgName, &bumpProgram ) && bumpProgram ) {
		R_LoadImageProgram( bumpProgram->c_str(), &pic, &w, &hgt, NULL, NULL );
	}

	if ( !pic ) {
		// flat height: parallax offset collapses to zero
		byte flat[4] = { 128, 128, 128, 255 };
		image->GenerateImage( flat, 1, 1, TF_DEFAULT, true, TR_REPEAT, TD_HIGH_QUALITY );
		return;
	}

	R_NormalMapToHeightMap( pic, w, hgt );
	image->GenerateImage( pic, w, hgt, TF_DEFAULT, true, TR_REPEAT, TD_HIGH_QUALITY );
	R_StaticFree( pic );
}

/*
===============
R_CreateParallaxHeightImage

Build (or fetch) the parallax height image derived from a bump stage's combined
normal-map program. Returns NULL if the program is empty. The generated image is
named with a `_parallaxHeight/` prefix so it never collides with the bump image
that shares the same underlying program string (docs/parallax.md).
===============
*/
idImage *R_CreateParallaxHeightImage( const char *bumpProgram ) {
	if ( !bumpProgram || !bumpProgram[0] ) {
		return NULL;
	}
	// Match the normalization ImageFromFunction applies to the name (strips ".tga",
	// backslashes -> slashes) so the generator's image->imgName lookup key agrees.
	idStr name = idStr( "_parallaxHeight/" ) + bumpProgram;
	name.Replace( ".tga", "" );
	name.BackSlashesToSlashes();
	idStr source = bumpProgram;
	r_parallaxSources.Set( name, source );
	return globalImages->ImageFromFunction( name, R_ParallaxHeightImage );
}
