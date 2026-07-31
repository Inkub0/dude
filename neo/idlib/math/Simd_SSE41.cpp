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
#include "idlib/geometry/DrawVert.h"
#include "idlib/geometry/JointTransform.h"
#include "idlib/math/Vector.h"
#include "idlib/math/Plane.h"

#include "idlib/math/Simd_SSE41.h"

#if defined(__GNUC__) && defined(__SSE4_1__)

#include <smmintrin.h>

// lane selectors: result lane0 gets source lane a, lane1 gets b, etc.
#define SHUF( a, b, c, d )		_MM_SHUFFLE( (d), (c), (b), (a) )

// rsqrtps followed by one Newton-Raphson iteration, accurate to ~1e-6 relative
static ID_INLINE __m128 SSE41_RSqrt( __m128 x ) {
	__m128 r = _mm_rsqrt_ps( x );
	// r = r * ( 1.5 - 0.5 * x * r * r )
	__m128 h = _mm_mul_ps( _mm_set1_ps( 0.5f ), x );
	r = _mm_mul_ps( r, _mm_sub_ps( _mm_set1_ps( 1.5f ), _mm_mul_ps( h, _mm_mul_ps( r, r ) ) ) );
	return r;
}

// stores lanes 0-2 without touching the 4 bytes after them
static ID_INLINE void SSE41_StoreVec3( float *dst, __m128 v ) {
	_mm_storel_pi( (__m64 *)dst, v );
	_mm_store_ss( dst + 2, _mm_shuffle_ps( v, v, SHUF( 2, 2, 2, 2 ) ) );
}

/*
============
idSIMD_SSE41::GetName
============
*/
const char * idSIMD_SSE41::GetName( void ) const {
	return "MMX & SSE & SSE2 & SSE3 & SSE4.1";
}

/*
============
idSIMD_SSE41::ConvertJointQuatsToJointMats

	Same arithmetic as idQuat::ToMat3 + idJointMat::SetRotation, but written
	straight into the joint matrix without the transposed idMat3 temporary.
============
*/
void VPCALL idSIMD_SSE41::ConvertJointQuatsToJointMats( idJointMat *jointMats, const idJointQuat *jointQuats, const int numJoints ) {
	for ( int i = 0; i < numJoints; i++ ) {
		const idQuat &q = jointQuats[i].q;
		const idVec3 &t = jointQuats[i].t;
		float *m = jointMats[i].ToFloatPtr();

		float x2 = q.x + q.x;
		float y2 = q.y + q.y;
		float z2 = q.z + q.z;

		float xx = q.x * x2;
		float xy = q.x * y2;
		float xz = q.x * z2;
		float yy = q.y * y2;
		float yz = q.y * z2;
		float zz = q.z * z2;
		float wx = q.w * x2;
		float wy = q.w * y2;
		float wz = q.w * z2;

		m[0] = 1.0f - ( yy + zz );
		m[1] = xy + wz;
		m[2] = xz - wy;
		m[3] = t.x;

		m[4] = xy - wz;
		m[5] = 1.0f - ( xx + zz );
		m[6] = yz + wx;
		m[7] = t.y;

		m[8] = xz + wy;
		m[9] = yz - wx;
		m[10] = 1.0f - ( xx + yy );
		m[11] = t.z;
	}
}

/*
============
idSIMD_SSE41::TransformJoints

	jointMats[i] = jointMats[parents[i]] * jointMats[i]
	Same per-component evaluation order as idJointMat::operator*=.
============
*/
void VPCALL idSIMD_SSE41::TransformJoints( idJointMat *jointMats, const int *parents, const int firstJoint, const int lastJoint ) {
	for ( int i = firstJoint; i <= lastJoint; i++ ) {
		assert( parents[i] < i );
		const float *p = jointMats[parents[i]].ToFloatPtr();
		float *c = jointMats[i].ToFloatPtr();

		__m128 c0 = _mm_loadu_ps( c + 0 );
		__m128 c1 = _mm_loadu_ps( c + 4 );
		__m128 c2 = _mm_loadu_ps( c + 8 );

		for ( int r = 0; r < 3; r++ ) {
			__m128 pr = _mm_loadu_ps( p + r * 4 );
			__m128 n;
			n = _mm_mul_ps( _mm_shuffle_ps( pr, pr, SHUF( 0, 0, 0, 0 ) ), c0 );
			n = _mm_add_ps( n, _mm_mul_ps( _mm_shuffle_ps( pr, pr, SHUF( 1, 1, 1, 1 ) ), c1 ) );
			n = _mm_add_ps( n, _mm_mul_ps( _mm_shuffle_ps( pr, pr, SHUF( 2, 2, 2, 2 ) ), c2 ) );
			// translation of the parent only affects the last column
			n = _mm_add_ps( n, _mm_blend_ps( _mm_setzero_ps(), pr, 0x8 ) );
			_mm_storeu_ps( c + r * 4, n );
		}
	}
}

/*
============
idSIMD_SSE41::UntransformJoints

	jointMats[i] = inverse(jointMats[parents[i]]) * jointMats[i]
	Same per-component evaluation order as idJointMat::operator/=.
============
*/
void VPCALL idSIMD_SSE41::UntransformJoints( idJointMat *jointMats, const int *parents, const int firstJoint, const int lastJoint ) {
	for ( int i = lastJoint; i >= firstJoint; i-- ) {
		assert( parents[i] < i );
		const float *p = jointMats[parents[i]].ToFloatPtr();
		float *c = jointMats[i].ToFloatPtr();

		__m128 p0 = _mm_loadu_ps( p + 0 );
		__m128 p1 = _mm_loadu_ps( p + 4 );
		__m128 p2 = _mm_loadu_ps( p + 8 );

		// subtract the parent translation from the last column
		__m128 c0 = _mm_sub_ps( _mm_loadu_ps( c + 0 ), _mm_blend_ps( _mm_setzero_ps(), p0, 0x8 ) );
		__m128 c1 = _mm_sub_ps( _mm_loadu_ps( c + 4 ), _mm_blend_ps( _mm_setzero_ps(), p1, 0x8 ) );
		__m128 c2 = _mm_sub_ps( _mm_loadu_ps( c + 8 ), _mm_blend_ps( _mm_setzero_ps(), p2, 0x8 ) );

		// multiply by the transpose of the parent rotation
		__m128 n0, n1, n2;

		n0 = _mm_mul_ps( _mm_shuffle_ps( p0, p0, SHUF( 0, 0, 0, 0 ) ), c0 );
		n0 = _mm_add_ps( n0, _mm_mul_ps( _mm_shuffle_ps( p1, p1, SHUF( 0, 0, 0, 0 ) ), c1 ) );
		n0 = _mm_add_ps( n0, _mm_mul_ps( _mm_shuffle_ps( p2, p2, SHUF( 0, 0, 0, 0 ) ), c2 ) );

		n1 = _mm_mul_ps( _mm_shuffle_ps( p0, p0, SHUF( 1, 1, 1, 1 ) ), c0 );
		n1 = _mm_add_ps( n1, _mm_mul_ps( _mm_shuffle_ps( p1, p1, SHUF( 1, 1, 1, 1 ) ), c1 ) );
		n1 = _mm_add_ps( n1, _mm_mul_ps( _mm_shuffle_ps( p2, p2, SHUF( 1, 1, 1, 1 ) ), c2 ) );

		n2 = _mm_mul_ps( _mm_shuffle_ps( p0, p0, SHUF( 2, 2, 2, 2 ) ), c0 );
		n2 = _mm_add_ps( n2, _mm_mul_ps( _mm_shuffle_ps( p1, p1, SHUF( 2, 2, 2, 2 ) ), c1 ) );
		n2 = _mm_add_ps( n2, _mm_mul_ps( _mm_shuffle_ps( p2, p2, SHUF( 2, 2, 2, 2 ) ), c2 ) );

		_mm_storeu_ps( c + 0, n0 );
		_mm_storeu_ps( c + 4, n1 );
		_mm_storeu_ps( c + 8, n2 );
	}
}

/*
============
idSIMD_SSE41::TransformVerts
============
*/
void VPCALL idSIMD_SSE41::TransformVerts( idDrawVert *verts, const int numVerts, const idJointMat *joints, const idVec4 *weights, const int *index, const int numWeights ) {
	const byte *jointsPtr = (byte *)joints;
	int j = 0;

	for ( int i = 0; i < numVerts; i++ ) {
		const float *m = ( (idJointMat *)( jointsPtr + index[j*2+0] ) )->ToFloatPtr();
		__m128 w = _mm_loadu_ps( weights[j].ToFloatPtr() );

		__m128 r0 = _mm_mul_ps( _mm_loadu_ps( m + 0 ), w );
		__m128 r1 = _mm_mul_ps( _mm_loadu_ps( m + 4 ), w );
		__m128 r2 = _mm_mul_ps( _mm_loadu_ps( m + 8 ), w );

		while ( index[j*2+1] == 0 ) {
			j++;
			m = ( (idJointMat *)( jointsPtr + index[j*2+0] ) )->ToFloatPtr();
			w = _mm_loadu_ps( weights[j].ToFloatPtr() );

			r0 = _mm_add_ps( r0, _mm_mul_ps( _mm_loadu_ps( m + 0 ), w ) );
			r1 = _mm_add_ps( r1, _mm_mul_ps( _mm_loadu_ps( m + 4 ), w ) );
			r2 = _mm_add_ps( r2, _mm_mul_ps( _mm_loadu_ps( m + 8 ), w ) );
		}
		j++;

		__m128 sum = _mm_hadd_ps( _mm_hadd_ps( r0, r1 ), _mm_hadd_ps( r2, r2 ) );
		SSE41_StoreVec3( verts[i].xyz.ToFloatPtr(), sum );
	}
}

/*
============
idSIMD_SSE41::TracePointCull
============
*/
void VPCALL idSIMD_SSE41::TracePointCull( byte *cullBits, byte &totalOr, const float radius, const idPlane *planes, const idDrawVert *verts, const int numVerts ) {
	__m128 nx = _mm_loadu_ps( planes[0].ToFloatPtr() );
	__m128 ny = _mm_loadu_ps( planes[1].ToFloatPtr() );
	__m128 nz = _mm_loadu_ps( planes[2].ToFloatPtr() );
	__m128 nd = _mm_loadu_ps( planes[3].ToFloatPtr() );
	_MM_TRANSPOSE4_PS( nx, ny, nz, nd );

	__m128 rad = _mm_set1_ps( radius );
	int tOr = 0;

	for ( int i = 0; i < numVerts; i++ ) {
		const float *v = verts[i].xyz.ToFloatPtr();
		__m128 vx = _mm_set1_ps( v[0] );
		__m128 vy = _mm_set1_ps( v[1] );
		__m128 vz = _mm_set1_ps( v[2] );

		__m128 d = _mm_add_ps( _mm_add_ps( _mm_add_ps( _mm_mul_ps( nx, vx ), _mm_mul_ps( ny, vy ) ), _mm_mul_ps( nz, vz ) ), nd );

		int bits = _mm_movemask_ps( _mm_add_ps( d, rad ) );
		bits |= _mm_movemask_ps( _mm_sub_ps( d, rad ) ) << 4;
		bits ^= 0x0F;		// flip lower four bits

		tOr |= bits;
		cullBits[i] = (byte)bits;
	}

	totalOr = (byte)tOr;
}

/*
============
idSIMD_SSE41::DecalPointCull
============
*/
void VPCALL idSIMD_SSE41::DecalPointCull( byte *cullBits, const idPlane *planes, const idDrawVert *verts, const int numVerts ) {
	__m128 nx = _mm_loadu_ps( planes[0].ToFloatPtr() );
	__m128 ny = _mm_loadu_ps( planes[1].ToFloatPtr() );
	__m128 nz = _mm_loadu_ps( planes[2].ToFloatPtr() );
	__m128 nd = _mm_loadu_ps( planes[3].ToFloatPtr() );
	_MM_TRANSPOSE4_PS( nx, ny, nz, nd );

	__m128 mx = _mm_loadu_ps( planes[4].ToFloatPtr() );
	__m128 my = _mm_loadu_ps( planes[5].ToFloatPtr() );
	__m128 mz = _mm_setzero_ps();
	__m128 md = _mm_setzero_ps();
	_MM_TRANSPOSE4_PS( mx, my, mz, md );

	for ( int i = 0; i < numVerts; i++ ) {
		const float *v = verts[i].xyz.ToFloatPtr();
		__m128 vx = _mm_set1_ps( v[0] );
		__m128 vy = _mm_set1_ps( v[1] );
		__m128 vz = _mm_set1_ps( v[2] );

		__m128 d0 = _mm_add_ps( _mm_add_ps( _mm_add_ps( _mm_mul_ps( nx, vx ), _mm_mul_ps( ny, vy ) ), _mm_mul_ps( nz, vz ) ), nd );
		__m128 d1 = _mm_add_ps( _mm_add_ps( _mm_add_ps( _mm_mul_ps( mx, vx ), _mm_mul_ps( my, vy ) ), _mm_mul_ps( mz, vz ) ), md );

		int bits = _mm_movemask_ps( d0 );
		bits |= ( _mm_movemask_ps( d1 ) & 3 ) << 4;

		cullBits[i] = (byte)( bits ^ 0x3F );		// flip lower 6 bits
	}
}

/*
============
idSIMD_SSE41::OverlayPointCull
============
*/
void VPCALL idSIMD_SSE41::OverlayPointCull( byte *cullBits, idVec2 *texCoords, const idPlane *planes, const idDrawVert *verts, const int numVerts ) {
	__m128 nx = _mm_loadu_ps( planes[0].ToFloatPtr() );
	__m128 ny = _mm_loadu_ps( planes[1].ToFloatPtr() );
	__m128 nz = _mm_setzero_ps();
	__m128 nd = _mm_setzero_ps();
	_MM_TRANSPOSE4_PS( nx, ny, nz, nd );

	__m128 one = _mm_set1_ps( 1.0f );

	for ( int i = 0; i < numVerts; i++ ) {
		const float *v = verts[i].xyz.ToFloatPtr();
		__m128 vx = _mm_set1_ps( v[0] );
		__m128 vy = _mm_set1_ps( v[1] );
		__m128 vz = _mm_set1_ps( v[2] );

		// lanes 0,1 = d0,d1
		__m128 d = _mm_add_ps( _mm_add_ps( _mm_add_ps( _mm_mul_ps( nx, vx ), _mm_mul_ps( ny, vy ) ), _mm_mul_ps( nz, vz ) ), nd );

		_mm_storel_pi( (__m64 *)texCoords[i].ToFloatPtr(), d );

		// [ d0, d1, 1-d0, 1-d1 ]
		__m128 sel = _mm_movelh_ps( d, _mm_sub_ps( one, d ) );
		cullBits[i] = (byte)_mm_movemask_ps( sel );
	}
}

/*
============
idSIMD_SSE41::DeriveTriPlanes
============
*/
void VPCALL idSIMD_SSE41::DeriveTriPlanes( idPlane *planes, const idDrawVert *verts, const int numVerts, const int *indexes, const int numIndexes ) {
	__m128 signMask = _mm_castsi128_ps( _mm_set1_epi32( 0x80000000 ) );

	for ( int i = 0; i < numIndexes; i += 3 ) {
		const idDrawVert *a = verts + indexes[i + 0];
		const idDrawVert *b = verts + indexes[i + 1];
		const idDrawVert *c = verts + indexes[i + 2];

		// reads 4 bytes into st[], always in bounds
		__m128 va = _mm_loadu_ps( a->xyz.ToFloatPtr() );
		__m128 d0 = _mm_sub_ps( _mm_loadu_ps( b->xyz.ToFloatPtr() ), va );
		__m128 d1 = _mm_sub_ps( _mm_loadu_ps( c->xyz.ToFloatPtr() ), va );

		// n = d1.yzx * d0.zxy - d1.zxy * d0.yzx
		__m128 n = _mm_sub_ps(
			_mm_mul_ps( _mm_shuffle_ps( d1, d1, SHUF( 1, 2, 0, 3 ) ), _mm_shuffle_ps( d0, d0, SHUF( 2, 0, 1, 3 ) ) ),
			_mm_mul_ps( _mm_shuffle_ps( d1, d1, SHUF( 2, 0, 1, 3 ) ), _mm_shuffle_ps( d0, d0, SHUF( 1, 2, 0, 3 ) ) ) );

		__m128 len = _mm_dp_ps( n, n, 0x7F );
		n = _mm_mul_ps( n, SSE41_RSqrt( len ) );

		// dist = -( n . a )
		__m128 dist = _mm_xor_ps( _mm_dp_ps( n, va, 0x7F ), signMask );

		_mm_storeu_ps( planes->ToFloatPtr(), _mm_insert_ps( n, dist, ( 0 << 6 ) | ( 3 << 4 ) ) );
		planes++;
	}
}

/*
============
idSIMD_SSE41::DeriveTangents
============
*/
void VPCALL idSIMD_SSE41::DeriveTangents( idPlane *planes, idDrawVert *verts, const int numVerts, const int *indexes, const int numIndexes ) {
	__m128 signMask = _mm_castsi128_ps( _mm_set1_epi32( 0x80000000 ) );

	bool *used = (bool *)_alloca16( numVerts * sizeof( used[0] ) );
	memset( used, 0, numVerts * sizeof( used[0] ) );

	idPlane *planesPtr = planes;
	for ( int i = 0; i < numIndexes; i += 3 ) {
		int v0 = indexes[i + 0];
		int v1 = indexes[i + 1];
		int v2 = indexes[i + 2];

		idDrawVert *a = verts + v0;
		idDrawVert *b = verts + v1;
		idDrawVert *c = verts + v2;

		// [ x, y, z, s ]
		__m128 va = _mm_loadu_ps( a->xyz.ToFloatPtr() );
		__m128 d0 = _mm_sub_ps( _mm_loadu_ps( b->xyz.ToFloatPtr() ), va );
		__m128 d1 = _mm_sub_ps( _mm_loadu_ps( c->xyz.ToFloatPtr() ), va );

		float d0t = b->st[1] - a->st[1];
		float d1t = c->st[1] - a->st[1];
		float d0s = _mm_cvtss_f32( _mm_shuffle_ps( d0, d0, SHUF( 3, 3, 3, 3 ) ) );
		float d1s = _mm_cvtss_f32( _mm_shuffle_ps( d1, d1, SHUF( 3, 3, 3, 3 ) ) );

		// normal
		__m128 n = _mm_sub_ps(
			_mm_mul_ps( _mm_shuffle_ps( d1, d1, SHUF( 1, 2, 0, 3 ) ), _mm_shuffle_ps( d0, d0, SHUF( 2, 0, 1, 3 ) ) ),
			_mm_mul_ps( _mm_shuffle_ps( d1, d1, SHUF( 2, 0, 1, 3 ) ), _mm_shuffle_ps( d0, d0, SHUF( 1, 2, 0, 3 ) ) ) );

		// first tangent: t0 = d0 * d1t - d0t * d1
		__m128 t0 = _mm_sub_ps( _mm_mul_ps( d0, _mm_set1_ps( d1t ) ), _mm_mul_ps( _mm_set1_ps( d0t ), d1 ) );

		// second tangent: t1 = d0s * d1 - d1s * d0
		__m128 t1 = _mm_sub_ps( _mm_mul_ps( _mm_set1_ps( d0s ), d1 ), _mm_mul_ps( d0, _mm_set1_ps( d1s ) ) );

		// normalize all three at once
		__m128 lens = _mm_dp_ps( n, n, 0x71 );
		lens = _mm_insert_ps( lens, _mm_dp_ps( t0, t0, 0x71 ), ( 0 << 6 ) | ( 1 << 4 ) );
		lens = _mm_insert_ps( lens, _mm_dp_ps( t1, t1, 0x71 ), ( 0 << 6 ) | ( 2 << 4 ) );
		lens = _mm_insert_ps( lens, _mm_set_ss( 1.0f ), ( 0 << 6 ) | ( 3 << 4 ) );
		__m128 rs = SSE41_RSqrt( lens );

		// area sign flips the tangents
		float area = d0s * d1t - d0t * d1s;
		__m128 areaSign = _mm_and_ps( _mm_set1_ps( area ), signMask );

		n = _mm_mul_ps( n, _mm_shuffle_ps( rs, rs, SHUF( 0, 0, 0, 0 ) ) );
		t0 = _mm_mul_ps( t0, _mm_xor_ps( _mm_shuffle_ps( rs, rs, SHUF( 1, 1, 1, 1 ) ), areaSign ) );
		t1 = _mm_mul_ps( t1, _mm_xor_ps( _mm_shuffle_ps( rs, rs, SHUF( 2, 2, 2, 2 ) ), areaSign ) );

		// plane through vertex a
		__m128 dist = _mm_xor_ps( _mm_dp_ps( n, va, 0x7F ), signMask );
		_mm_storeu_ps( planesPtr->ToFloatPtr(), _mm_insert_ps( n, dist, ( 0 << 6 ) | ( 3 << 4 ) ) );
		planesPtr++;

		// accumulate into the vertices; normal and tangents are 9 contiguous floats
		__m128 pack0 = _mm_insert_ps( n, t0, ( 0 << 6 ) | ( 3 << 4 ) );			// [ n.x n.y n.z t0.x ]
		__m128 pack1 = _mm_shuffle_ps( t0, t1, SHUF( 1, 2, 0, 1 ) );			// [ t0.y t0.z t1.x t1.y ]
		float t1z = _mm_cvtss_f32( _mm_shuffle_ps( t1, t1, SHUF( 2, 2, 2, 2 ) ) );

		idDrawVert *tri[3] = { a, b, c };
		int triv[3] = { v0, v1, v2 };
		for ( int k = 0; k < 3; k++ ) {
			float *base = tri[k]->normal.ToFloatPtr();
			if ( used[triv[k]] ) {
				_mm_storeu_ps( base + 0, _mm_add_ps( _mm_loadu_ps( base + 0 ), pack0 ) );
				_mm_storeu_ps( base + 4, _mm_add_ps( _mm_loadu_ps( base + 4 ), pack1 ) );
				base[8] += t1z;
			} else {
				_mm_storeu_ps( base + 0, pack0 );
				_mm_storeu_ps( base + 4, pack1 );
				base[8] = t1z;
				used[triv[k]] = true;
			}
		}
	}
}

/*
============
idSIMD_SSE41::NormalizeTangents
============
*/
void VPCALL idSIMD_SSE41::NormalizeTangents( idDrawVert *verts, const int numVerts ) {
	for ( int i = 0; i < numVerts; i++ ) {
		float *np = verts[i].normal.ToFloatPtr();

		__m128 n = _mm_loadu_ps( np );
		n = _mm_mul_ps( n, SSE41_RSqrt( _mm_dp_ps( n, n, 0x7F ) ) );
		SSE41_StoreVec3( np, n );

		for ( int j = 0; j < 2; j++ ) {
			float *tp = verts[i].tangents[j].ToFloatPtr();

			__m128 t = _mm_loadu_ps( tp );
			t = _mm_sub_ps( t, _mm_mul_ps( _mm_dp_ps( t, n, 0x7F ), n ) );
			t = _mm_mul_ps( t, SSE41_RSqrt( _mm_dp_ps( t, t, 0x7F ) ) );
			SSE41_StoreVec3( tp, t );
		}
	}
}

/*
============
idSIMD_SSE41::MixSoundTwoSpeakerMono
============
*/
void VPCALL idSIMD_SSE41::MixSoundTwoSpeakerMono( float *mixBuffer, const float *samples, const int numSamples, const float lastV[2], const float currentV[2] ) {
	float incL = ( currentV[0] - lastV[0] ) / MIXBUFFER_SAMPLES;
	float incR = ( currentV[1] - lastV[1] ) / MIXBUFFER_SAMPLES;

	assert( numSamples == MIXBUFFER_SAMPLES );

	__m128 inc = _mm_set_ps( incR, incL, incR, incL );
	__m128 vol1 = _mm_add_ps( _mm_set_ps( lastV[1], lastV[0], lastV[1], lastV[0] ),
	                          _mm_mul_ps( inc, _mm_set_ps( 1.0f, 1.0f, 0.0f, 0.0f ) ) );
	__m128 inc2 = _mm_add_ps( inc, inc );
	__m128 vol2 = _mm_add_ps( vol1, inc2 );
	__m128 inc4 = _mm_add_ps( inc2, inc2 );

	for ( int j = 0; j < MIXBUFFER_SAMPLES; j += 4 ) {
		__m128 s = _mm_loadu_ps( samples + j );
		__m128 sa = _mm_shuffle_ps( s, s, SHUF( 0, 0, 1, 1 ) );
		__m128 sb = _mm_shuffle_ps( s, s, SHUF( 2, 2, 3, 3 ) );

		float *mb = mixBuffer + j * 2;
		_mm_storeu_ps( mb + 0, _mm_add_ps( _mm_loadu_ps( mb + 0 ), _mm_mul_ps( sa, vol1 ) ) );
		_mm_storeu_ps( mb + 4, _mm_add_ps( _mm_loadu_ps( mb + 4 ), _mm_mul_ps( sb, vol2 ) ) );

		vol1 = _mm_add_ps( vol1, inc4 );
		vol2 = _mm_add_ps( vol2, inc4 );
	}
}

/*
============
idSIMD_SSE41::MixSoundTwoSpeakerStereo
============
*/
void VPCALL idSIMD_SSE41::MixSoundTwoSpeakerStereo( float *mixBuffer, const float *samples, const int numSamples, const float lastV[2], const float currentV[2] ) {
	float incL = ( currentV[0] - lastV[0] ) / MIXBUFFER_SAMPLES;
	float incR = ( currentV[1] - lastV[1] ) / MIXBUFFER_SAMPLES;

	assert( numSamples == MIXBUFFER_SAMPLES );

	__m128 inc = _mm_set_ps( incR, incL, incR, incL );
	__m128 vol1 = _mm_add_ps( _mm_set_ps( lastV[1], lastV[0], lastV[1], lastV[0] ),
	                          _mm_mul_ps( inc, _mm_set_ps( 1.0f, 1.0f, 0.0f, 0.0f ) ) );
	__m128 inc2 = _mm_add_ps( inc, inc );
	__m128 vol2 = _mm_add_ps( vol1, inc2 );
	__m128 inc4 = _mm_add_ps( inc2, inc2 );

	for ( int j = 0; j < MIXBUFFER_SAMPLES; j += 4 ) {
		const float *sp = samples + j * 2;
		float *mb = mixBuffer + j * 2;

		_mm_storeu_ps( mb + 0, _mm_add_ps( _mm_loadu_ps( mb + 0 ), _mm_mul_ps( _mm_loadu_ps( sp + 0 ), vol1 ) ) );
		_mm_storeu_ps( mb + 4, _mm_add_ps( _mm_loadu_ps( mb + 4 ), _mm_mul_ps( _mm_loadu_ps( sp + 4 ), vol2 ) ) );

		vol1 = _mm_add_ps( vol1, inc4 );
		vol2 = _mm_add_ps( vol2, inc4 );
	}
}

/*
============
idSIMD_SSE41::MixSoundSixSpeakerMono
============
*/
void VPCALL idSIMD_SSE41::MixSoundSixSpeakerMono( float *mixBuffer, const float *samples, const int numSamples, const float lastV[6], const float currentV[6] ) {
	float inc[6], vol[6];

	assert( numSamples == MIXBUFFER_SAMPLES );

	for ( int k = 0; k < 6; k++ ) {
		inc[k] = ( currentV[k] - lastV[k] ) / MIXBUFFER_SAMPLES;
		vol[k] = lastV[k];
	}

	// two samples (12 floats) per iteration
	__m128 volA = _mm_set_ps( vol[3], vol[2], vol[1], vol[0] );
	__m128 volB = _mm_set_ps( vol[1] + inc[1], vol[0] + inc[0], vol[5], vol[4] );
	__m128 volC = _mm_set_ps( vol[5] + inc[5], vol[4] + inc[4], vol[3] + inc[3], vol[2] + inc[2] );

	__m128 incA = _mm_set_ps( inc[3] * 2.0f, inc[2] * 2.0f, inc[1] * 2.0f, inc[0] * 2.0f );
	__m128 incB = _mm_set_ps( inc[1] * 2.0f, inc[0] * 2.0f, inc[5] * 2.0f, inc[4] * 2.0f );
	__m128 incC = _mm_set_ps( inc[5] * 2.0f, inc[4] * 2.0f, inc[3] * 2.0f, inc[2] * 2.0f );

	for ( int i = 0; i < MIXBUFFER_SAMPLES; i += 2 ) {
		__m128 s0 = _mm_set1_ps( samples[i + 0] );
		__m128 s1 = _mm_set1_ps( samples[i + 1] );
		__m128 sB = _mm_shuffle_ps( s0, s1, SHUF( 0, 0, 0, 0 ) );

		float *mb = mixBuffer + i * 6;
		_mm_storeu_ps( mb + 0, _mm_add_ps( _mm_loadu_ps( mb + 0 ), _mm_mul_ps( s0, volA ) ) );
		_mm_storeu_ps( mb + 4, _mm_add_ps( _mm_loadu_ps( mb + 4 ), _mm_mul_ps( sB, volB ) ) );
		_mm_storeu_ps( mb + 8, _mm_add_ps( _mm_loadu_ps( mb + 8 ), _mm_mul_ps( s1, volC ) ) );

		volA = _mm_add_ps( volA, incA );
		volB = _mm_add_ps( volB, incB );
		volC = _mm_add_ps( volC, incC );
	}
}

/*
============
idSIMD_SSE41::MixSoundSixSpeakerStereo
============
*/
void VPCALL idSIMD_SSE41::MixSoundSixSpeakerStereo( float *mixBuffer, const float *samples, const int numSamples, const float lastV[6], const float currentV[6] ) {
	float inc[6], vol[6];

	assert( numSamples == MIXBUFFER_SAMPLES );

	for ( int k = 0; k < 6; k++ ) {
		inc[k] = ( currentV[k] - lastV[k] ) / MIXBUFFER_SAMPLES;
		vol[k] = lastV[k];
	}

	// speaker mapping per sample: L R L L L R
	__m128 volA = _mm_set_ps( vol[3], vol[2], vol[1], vol[0] );
	__m128 volB = _mm_set_ps( vol[1] + inc[1], vol[0] + inc[0], vol[5], vol[4] );
	__m128 volC = _mm_set_ps( vol[5] + inc[5], vol[4] + inc[4], vol[3] + inc[3], vol[2] + inc[2] );

	__m128 incA = _mm_set_ps( inc[3] * 2.0f, inc[2] * 2.0f, inc[1] * 2.0f, inc[0] * 2.0f );
	__m128 incB = _mm_set_ps( inc[1] * 2.0f, inc[0] * 2.0f, inc[5] * 2.0f, inc[4] * 2.0f );
	__m128 incC = _mm_set_ps( inc[5] * 2.0f, inc[4] * 2.0f, inc[3] * 2.0f, inc[2] * 2.0f );

	for ( int i = 0; i < MIXBUFFER_SAMPLES; i += 2 ) {
		__m128 s = _mm_loadu_ps( samples + i * 2 );		// [ L0 R0 L1 R1 ]
		__m128 sA = _mm_shuffle_ps( s, s, SHUF( 0, 1, 0, 0 ) );
		__m128 sB = _mm_shuffle_ps( s, s, SHUF( 0, 1, 2, 3 ) );
		__m128 sC = _mm_shuffle_ps( s, s, SHUF( 2, 2, 2, 3 ) );

		float *mb = mixBuffer + i * 6;
		_mm_storeu_ps( mb + 0, _mm_add_ps( _mm_loadu_ps( mb + 0 ), _mm_mul_ps( sA, volA ) ) );
		_mm_storeu_ps( mb + 4, _mm_add_ps( _mm_loadu_ps( mb + 4 ), _mm_mul_ps( sB, volB ) ) );
		_mm_storeu_ps( mb + 8, _mm_add_ps( _mm_loadu_ps( mb + 8 ), _mm_mul_ps( sC, volC ) ) );

		volA = _mm_add_ps( volA, incA );
		volB = _mm_add_ps( volB, incB );
		volC = _mm_add_ps( volC, incC );
	}
}

/*
============
idSIMD_SSE41::MixedSoundToSamples
============
*/
void VPCALL idSIMD_SSE41::MixedSoundToSamples( short *samples, const float *mixBuffer, const int numSamples ) {
	__m128 minVal = _mm_set1_ps( -32768.0f );
	__m128 maxVal = _mm_set1_ps( 32767.0f );

	int i = 0;
	for ( ; i + 8 <= numSamples; i += 8 ) {
		__m128 f0 = _mm_max_ps( _mm_min_ps( _mm_loadu_ps( mixBuffer + i + 0 ), maxVal ), minVal );
		__m128 f1 = _mm_max_ps( _mm_min_ps( _mm_loadu_ps( mixBuffer + i + 4 ), maxVal ), minVal );

		// truncation, same as the (short) cast in the generic path
		__m128i packed = _mm_packs_epi32( _mm_cvttps_epi32( f0 ), _mm_cvttps_epi32( f1 ) );
		_mm_storeu_si128( (__m128i *)( samples + i ), packed );
	}

	for ( ; i < numSamples; i++ ) {
		if ( mixBuffer[i] <= -32768.0f ) {
			samples[i] = -32768;
		} else if ( mixBuffer[i] >= 32767.0f ) {
			samples[i] = 32767;
		} else {
			samples[i] = (short) mixBuffer[i];
		}
	}
}

#endif /* GCC && SSE4.1 */
