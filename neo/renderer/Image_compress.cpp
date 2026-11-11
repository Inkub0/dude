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

/*
===========================================================================

DUDE: CPU block compression (BC1 / BC2 / BC3, the S3TC family)

The GL backend lets the driver compress a texture as it uploads it when a quality tier asks
for compression (image_useCompression); Vulkan has no such upload, so the worker threads
encode the mip levels here before the main thread uploads the blocks
(CreateTexture2DCompressed). Plain, dependable encoders rather than the best ones: the
colour block takes the principal axis of the 16 texels, its extremes as the endpoints, and
two rounds of least-squares endpoint refinement from the index assignment; the BC3 alpha
block is the 8-value min/max fit; BC2's alpha is the explicit 4-bit one. That is the class
of encoder a driver runs at upload time too.

Block layout is the DDS / Vulkan one: BC1 = two RGB565 endpoints then 16 2-bit indices,
row by row, texel 0 in the low bits; BC2 = 64 bits of 4-bit alpha (row words, texel 0 low)
then the colour block; BC3 = two alpha endpoints, 48 bits of 3-bit indices, then the
colour block. Levels smaller than a block repeat their edge texels.

===========================================================================
*/

#include "sys/platform.h"
#include "renderer/tr_local.h"
#include "renderer/rhi/RHI.h"

/*
===============
BC_FetchBlock

The 4x4 texels at (bx, by), the edge texels repeated past the level's size.
===============
*/
static void BC_FetchBlock( const byte *rgba, int w, int h, int bx, int by, byte texels[16][4] ) {
	for ( int y = 0; y < 4; y++ ) {
		int sy = by + y;
		if ( sy > h - 1 ) {
			sy = h - 1;
		}
		for ( int x = 0; x < 4; x++ ) {
			int sx = bx + x;
			if ( sx > w - 1 ) {
				sx = w - 1;
			}
			const byte *p = rgba + ( sy * w + sx ) * 4;
			texels[y * 4 + x][0] = p[0];
			texels[y * 4 + x][1] = p[1];
			texels[y * 4 + x][2] = p[2];
			texels[y * 4 + x][3] = p[3];
		}
	}
}

static ID_INLINE int BC_Clamp255( float v ) {
	const int i = (int)( v + 0.5f );
	return i < 0 ? 0 : ( i > 255 ? 255 : i );
}

static ID_INLINE unsigned short BC_Pack565( const float c[3] ) {
	int r = (int)( c[0] * ( 31.0f / 255.0f ) + 0.5f );
	int g = (int)( c[1] * ( 63.0f / 255.0f ) + 0.5f );
	int b = (int)( c[2] * ( 31.0f / 255.0f ) + 0.5f );
	r = r < 0 ? 0 : ( r > 31 ? 31 : r );
	g = g < 0 ? 0 : ( g > 63 ? 63 : g );
	b = b < 0 ? 0 : ( b > 31 ? 31 : b );
	return (unsigned short)( ( r << 11 ) | ( g << 5 ) | b );
}

// the 8-bit colour a 565 endpoint decodes to (the common replicate-the-top-bits expansion)
static ID_INLINE void BC_Unpack565( unsigned short c, int out[3] ) {
	const int r = c >> 11, g = ( c >> 5 ) & 63, b = c & 31;
	out[0] = ( r << 3 ) | ( r >> 2 );
	out[1] = ( g << 2 ) | ( g >> 4 );
	out[2] = ( b << 3 ) | ( b >> 2 );
}

/*
===============
BC_Palette4 / BC_AssignIndices

The four colours of a BC1 block in 4-colour mode (c0 > c1), and the nearest-colour index of
each texel with the summed squared error.
===============
*/
static void BC_Palette4( unsigned short c0, unsigned short c1, int palette[4][3] ) {
	BC_Unpack565( c0, palette[0] );
	BC_Unpack565( c1, palette[1] );
	for ( int k = 0; k < 3; k++ ) {
		palette[2][k] = ( 2 * palette[0][k] + palette[1][k] + 1 ) / 3;
		palette[3][k] = ( palette[0][k] + 2 * palette[1][k] + 1 ) / 3;
	}
}

static int BC_AssignIndices( const byte texels[16][4], const int palette[4][3], byte indices[16] ) {
	int total = 0;
	for ( int i = 0; i < 16; i++ ) {
		int best = 0x7fffffff, bestIdx = 0;
		for ( int p = 0; p < 4; p++ ) {
			const int dr = texels[i][0] - palette[p][0];
			const int dg = texels[i][1] - palette[p][1];
			const int db = texels[i][2] - palette[p][2];
			const int e = dr * dr + dg * dg + db * db;
			if ( e < best ) {
				best = e;
				bestIdx = p;
			}
		}
		indices[i] = (byte)bestIdx;
		total += best;
	}
	return total;
}

/*
===============
BC_EncodeColorBlock

A BC1 colour block (8 bytes) from 16 RGBA texels; alpha is ignored (4-colour mode always).
===============
*/
static void BC_EncodeColorBlock( const byte texels[16][4], byte out[8] ) {
	// a flat block: both endpoints the colour, every index 0
	bool flat = true;
	for ( int i = 1; i < 16 && flat; i++ ) {
		flat = texels[i][0] == texels[0][0] && texels[i][1] == texels[0][1] && texels[i][2] == texels[0][2];
	}
	if ( flat ) {
		const float c[3] = { (float)texels[0][0], (float)texels[0][1], (float)texels[0][2] };
		const unsigned short q = BC_Pack565( c );
		out[0] = (byte)( q & 255 );
		out[1] = (byte)( q >> 8 );
		out[2] = out[0];
		out[3] = out[1];
		out[4] = out[5] = out[6] = out[7] = 0;
		return;
	}

	// the principal axis of the texel cloud (power iteration on the covariance)
	float mean[3] = { 0.0f, 0.0f, 0.0f };
	for ( int i = 0; i < 16; i++ ) {
		mean[0] += texels[i][0];
		mean[1] += texels[i][1];
		mean[2] += texels[i][2];
	}
	mean[0] *= 1.0f / 16.0f;
	mean[1] *= 1.0f / 16.0f;
	mean[2] *= 1.0f / 16.0f;
	float cov[6] = { 0, 0, 0, 0, 0, 0 };		// xx xy xz yy yz zz
	for ( int i = 0; i < 16; i++ ) {
		const float dx = texels[i][0] - mean[0], dy = texels[i][1] - mean[1], dz = texels[i][2] - mean[2];
		cov[0] += dx * dx; cov[1] += dx * dy; cov[2] += dx * dz;
		cov[3] += dy * dy; cov[4] += dy * dz; cov[5] += dz * dz;
	}
	float axis[3] = { cov[0] + cov[1] + cov[2], cov[1] + cov[3] + cov[4], cov[2] + cov[4] + cov[5] };
	for ( int it = 0; it < 8; it++ ) {
		const float nx = cov[0] * axis[0] + cov[1] * axis[1] + cov[2] * axis[2];
		const float ny = cov[1] * axis[0] + cov[3] * axis[1] + cov[4] * axis[2];
		const float nz = cov[2] * axis[0] + cov[4] * axis[1] + cov[5] * axis[2];
		const float len = idMath::Sqrt( nx * nx + ny * ny + nz * nz );
		if ( len < 1e-6f ) {
			break;
		}
		axis[0] = nx / len;
		axis[1] = ny / len;
		axis[2] = nz / len;
	}
	{
		const float len = idMath::Sqrt( axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2] );
		if ( len < 1e-6f ) {
			axis[0] = axis[1] = axis[2] = 0.57735f;		// a degenerate cloud: the grey axis
		} else {
			axis[0] /= len;
			axis[1] /= len;
			axis[2] /= len;
		}
	}

	// the extremes along the axis are the first endpoints
	float tMin = 1e30f, tMax = -1e30f;
	for ( int i = 0; i < 16; i++ ) {
		const float t = ( texels[i][0] - mean[0] ) * axis[0] + ( texels[i][1] - mean[1] ) * axis[1] + ( texels[i][2] - mean[2] ) * axis[2];
		if ( t < tMin ) { tMin = t; }
		if ( t > tMax ) { tMax = t; }
	}
	float e0[3], e1[3];
	for ( int k = 0; k < 3; k++ ) {
		e0[k] = mean[k] + axis[k] * tMax;
		e1[k] = mean[k] + axis[k] * tMin;
	}

	unsigned short c0 = BC_Pack565( e0 ), c1 = BC_Pack565( e1 );
	if ( c0 < c1 ) {
		const unsigned short t = c0; c0 = c1; c1 = t;
	}
	int palette[4][3];
	byte indices[16];
	int bestError;
	unsigned short bestC0 = c0, bestC1 = c1;
	byte bestIndices[16];
	if ( c0 == c1 ) {
		BC_Palette4( c0, c1, palette );
		bestError = BC_AssignIndices( texels, palette, bestIndices );
	} else {
		BC_Palette4( c0, c1, palette );
		bestError = BC_AssignIndices( texels, palette, bestIndices );

		// least-squares refinement: the endpoints that best fit the current index assignment
		// (index -> weight on c0: 1, 0, 2/3, 1/3), then the indices of those endpoints
		static const float weights[4] = { 1.0f, 0.0f, 2.0f / 3.0f, 1.0f / 3.0f };
		memcpy( indices, bestIndices, sizeof( indices ) );
		for ( int round = 0; round < 2 && bestError > 0; round++ ) {
			float a = 0.0f, b = 0.0f, c = 0.0f, r0[3] = { 0, 0, 0 }, r1[3] = { 0, 0, 0 };
			for ( int i = 0; i < 16; i++ ) {
				const float wgt = weights[indices[i]], inv = 1.0f - wgt;
				a += wgt * wgt;
				b += wgt * inv;
				c += inv * inv;
				for ( int k = 0; k < 3; k++ ) {
					r0[k] += wgt * texels[i][k];
					r1[k] += inv * texels[i][k];
				}
			}
			const float det = a * c - b * b;
			if ( det < 1e-4f ) {
				break;		// every texel on one endpoint: nothing to fit
			}
			float n0[3], n1[3];
			for ( int k = 0; k < 3; k++ ) {
				n0[k] = (float)BC_Clamp255( ( c * r0[k] - b * r1[k] ) / det );
				n1[k] = (float)BC_Clamp255( ( a * r1[k] - b * r0[k] ) / det );
			}
			unsigned short q0 = BC_Pack565( n0 ), q1 = BC_Pack565( n1 );
			if ( q0 < q1 ) {
				const unsigned short t = q0; q0 = q1; q1 = t;
			}
			if ( q0 == q1 ) {
				break;
			}
			BC_Palette4( q0, q1, palette );
			const int err = BC_AssignIndices( texels, palette, indices );
			if ( err >= bestError ) {
				break;
			}
			bestError = err;
			bestC0 = q0;
			bestC1 = q1;
			memcpy( bestIndices, indices, sizeof( indices ) );
		}
	}

	out[0] = (byte)( bestC0 & 255 );
	out[1] = (byte)( bestC0 >> 8 );
	out[2] = (byte)( bestC1 & 255 );
	out[3] = (byte)( bestC1 >> 8 );
	for ( int y = 0; y < 4; y++ ) {
		out[4 + y] = (byte)( bestIndices[y * 4 + 0] | ( bestIndices[y * 4 + 1] << 2 ) | ( bestIndices[y * 4 + 2] << 4 ) | ( bestIndices[y * 4 + 3] << 6 ) );
	}
}

/*
===============
BC_EncodeAlphaBlock3

A BC3 alpha block (8 bytes): the 8-value interpolated fit between the block's extremes.
===============
*/
static void BC_EncodeAlphaBlock3( const byte texels[16][4], byte out[8] ) {
	int aMin = 255, aMax = 0;
	for ( int i = 0; i < 16; i++ ) {
		if ( texels[i][3] < aMin ) { aMin = texels[i][3]; }
		if ( texels[i][3] > aMax ) { aMax = texels[i][3]; }
	}
	out[0] = (byte)aMax;
	out[1] = (byte)aMin;
	if ( aMax == aMin ) {
		memset( out + 2, 0, 6 );		// every index 0 = a0
		return;
	}
	// a0 > a1: a0, a1, then six interpolated values
	int palette[8];
	palette[0] = aMax;
	palette[1] = aMin;
	for ( int k = 1; k <= 6; k++ ) {
		palette[k + 1] = ( ( 7 - k ) * aMax + k * aMin + 3 ) / 7;
	}
	unsigned long long bits = 0;
	for ( int i = 0; i < 16; i++ ) {
		int best = 0x7fffffff, bestIdx = 0;
		for ( int p = 0; p < 8; p++ ) {
			const int d = texels[i][3] - palette[p];
			const int e = d * d;
			if ( e < best ) {
				best = e;
				bestIdx = p;
			}
		}
		bits |= (unsigned long long)bestIdx << ( 3 * i );
	}
	for ( int k = 0; k < 6; k++ ) {
		out[2 + k] = (byte)( ( bits >> ( 8 * k ) ) & 255 );
	}
}

/*
===============
BC_EncodeAlphaBlock2

A BC2 alpha block (8 bytes): 4 bits per texel, row words, texel 0 in the low nibble.
===============
*/
static void BC_EncodeAlphaBlock2( const byte texels[16][4], byte out[8] ) {
	for ( int y = 0; y < 4; y++ ) {
		unsigned int word = 0;
		for ( int x = 0; x < 4; x++ ) {
			const unsigned int nibble = ( texels[y * 4 + x][3] * 15 + 127 ) / 255;
			word |= nibble << ( 4 * x );
		}
		out[2 * y] = (byte)( word & 255 );
		out[2 * y + 1] = (byte)( word >> 8 );
	}
}

/*
===============
R_BcLevelBytes

The block data size of a w x h level in the given format (rhi::RHI::CompressedFormat).
===============
*/
int R_BcLevelBytes( int format, int w, int h ) {
	const int blockBytes = ( format == rhi::RHI::CF_BC1_RGB || format == rhi::RHI::CF_BC1_RGBA ) ? 8 : 16;
	return ( ( w + 3 ) / 4 ) * ( ( h + 3 ) / 4 ) * blockBytes;
}

/*
===============
R_CompressBcLevel

Encodes a w x h RGBA8 level into out (R_BcLevelBytes bytes) as BC1 (RGB, alpha ignored),
BC2 (explicit alpha) or BC3 (interpolated alpha). Other formats return false.
===============
*/
bool R_CompressBcLevel( int format, const byte *rgba, int w, int h, byte *out ) {
	if ( format != rhi::RHI::CF_BC1_RGB && format != rhi::RHI::CF_BC2 && format != rhi::RHI::CF_BC3 ) {
		return false;
	}
	byte *dst = out;
	for ( int by = 0; by < h; by += 4 ) {
		for ( int bx = 0; bx < w; bx += 4 ) {
			byte texels[16][4];
			BC_FetchBlock( rgba, w, h, bx, by, texels );
			if ( format == rhi::RHI::CF_BC1_RGB ) {
				BC_EncodeColorBlock( texels, dst );
				dst += 8;
			} else {
				if ( format == rhi::RHI::CF_BC2 ) {
					BC_EncodeAlphaBlock2( texels, dst );
				} else {
					BC_EncodeAlphaBlock3( texels, dst );
				}
				BC_EncodeColorBlock( texels, dst + 8 );
				dst += 16;
			}
		}
	}
	return true;
}
