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

#ifndef __MATH_SIMD_SSE41_H__
#define __MATH_SIMD_SSE41_H__

#include "idlib/math/Simd_SSE3.h"

/*
===============================================================================

	SSE4.1 implementation of idSIMDProcessor

	Unlike the older SSE/SSE2/SSE3 processors, whose x86 assembly only
	compiles on 32 bit MSVC, this one is written with compiler intrinsics
	and covers the routines that are hot on modern 64 bit builds:
	skeletal animation, vertex skinning, tangent derivation, point culling,
	bounds computation, shadow volume caches, the idMatX kernels behind the
	LCP/articulated-figure physics solvers, and sound mixing.

===============================================================================
*/

class idSIMD_SSE41 : public idSIMD_SSE3 {
public:
#if defined(__GNUC__) && defined(__SSE4_1__)
	virtual const char * VPCALL GetName( void ) const;

	virtual void VPCALL ConvertJointQuatsToJointMats( idJointMat *jointMats, const idJointQuat *jointQuats, const int numJoints );
	virtual void VPCALL TransformJoints( idJointMat *jointMats, const int *parents, const int firstJoint, const int lastJoint );
	virtual void VPCALL UntransformJoints( idJointMat *jointMats, const int *parents, const int firstJoint, const int lastJoint );
	virtual void VPCALL TransformVerts( idDrawVert *verts, const int numVerts, const idJointMat *joints, const idVec4 *weights, const int *index, const int numWeights );

	using idSIMD_SSE3::MinMax;	// the float / idVec2 overloads stay inherited
	virtual void VPCALL MinMax( idVec3 &min, idVec3 &max, const idVec3 *src, const int count );
	virtual void VPCALL MinMax( idVec3 &min, idVec3 &max, const idDrawVert *src, const int count );
	virtual void VPCALL MinMax( idVec3 &min, idVec3 &max, const idDrawVert *src, const int *indexes, const int count );

	virtual void VPCALL MatX_MultiplyVecX( idVecX &dst, const idMatX &mat, const idVecX &vec );
	virtual void VPCALL MatX_MultiplyAddVecX( idVecX &dst, const idMatX &mat, const idVecX &vec );
	virtual void VPCALL MatX_MultiplySubVecX( idVecX &dst, const idMatX &mat, const idVecX &vec );
	virtual void VPCALL MatX_TransposeMultiplyVecX( idVecX &dst, const idMatX &mat, const idVecX &vec );
	virtual void VPCALL MatX_TransposeMultiplyAddVecX( idVecX &dst, const idMatX &mat, const idVecX &vec );
	virtual void VPCALL MatX_TransposeMultiplySubVecX( idVecX &dst, const idMatX &mat, const idVecX &vec );
	virtual void VPCALL MatX_LowerTriangularSolve( const idMatX &L, float *x, const float *b, const int n, int skip = 0 );
	virtual void VPCALL MatX_LowerTriangularSolveTranspose( const idMatX &L, float *x, const float *b, const int n );
	virtual bool VPCALL MatX_LDLTFactor( idMatX &mat, idVecX &invDiag, const int n );

	virtual void VPCALL TracePointCull( byte *cullBits, byte &totalOr, const float radius, const idPlane *planes, const idDrawVert *verts, const int numVerts );
	virtual void VPCALL DecalPointCull( byte *cullBits, const idPlane *planes, const idDrawVert *verts, const int numVerts );
	virtual void VPCALL OverlayPointCull( byte *cullBits, idVec2 *texCoords, const idPlane *planes, const idDrawVert *verts, const int numVerts );

	virtual void VPCALL DeriveTriPlanes( idPlane *planes, const idDrawVert *verts, const int numVerts, const int *indexes, const int numIndexes );
	virtual void VPCALL DeriveTangents( idPlane *planes, idDrawVert *verts, const int numVerts, const int *indexes, const int numIndexes );
	virtual void VPCALL DeriveUnsmoothedTangents( idDrawVert *verts, const dominantTri_s *dominantTris, const int numVerts );
	virtual void VPCALL NormalizeTangents( idDrawVert *verts, const int numVerts );

	virtual int  VPCALL CreateShadowCache( idVec4 *vertexCache, int *vertRemap, const idVec3 &lightOrigin, const idDrawVert *verts, const int numVerts );
	virtual int  VPCALL CreateVertexProgramShadowCache( idVec4 *vertexCache, const idDrawVert *verts, const int numVerts );

	virtual void VPCALL MixSoundTwoSpeakerMono( float *mixBuffer, const float *samples, const int numSamples, const float lastV[2], const float currentV[2] );
	virtual void VPCALL MixSoundTwoSpeakerStereo( float *mixBuffer, const float *samples, const int numSamples, const float lastV[2], const float currentV[2] );
	virtual void VPCALL MixSoundSixSpeakerMono( float *mixBuffer, const float *samples, const int numSamples, const float lastV[6], const float currentV[6] );
	virtual void VPCALL MixSoundSixSpeakerStereo( float *mixBuffer, const float *samples, const int numSamples, const float lastV[6], const float currentV[6] );
	virtual void VPCALL MixedSoundToSamples( short *samples, const float *mixBuffer, const int numSamples );
#endif
};

#endif /* !__MATH_SIMD_SSE41_H__ */
