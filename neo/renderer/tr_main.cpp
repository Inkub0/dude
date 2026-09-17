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

#if defined(__ppc__) && defined(__APPLE__)
#include <vecLib/vecLib.h>
#endif
#if defined(__GNUC__) && defined(__SSE2__)
#include <xmmintrin.h>
#endif

#include "sys/platform.h"
#include "framework/Session.h"
#include "renderer/RenderWorld_local.h"
#include "renderer/ModelManager.h"
#include "renderer/VertexCache.h"		// R3.5 r_rtAnimBlasTest: index-buffer handle from tri->indexCache

#include "renderer/tr_local.h"
#include "renderer/rhi/RHI.h"

//====================================================================

/*
======================
idScreenRect::Clear
======================
*/
void idScreenRect::Clear() {
	x1 = y1 = 32000;
	x2 = y2 = -32000;
	zmin = 0.0f; zmax = 1.0f;
}

/*
======================
idScreenRect::AddPoint
======================
*/
void idScreenRect::AddPoint( float x, float y ) {
	int	ix = idMath::FtoiFast( x );
	int iy = idMath::FtoiFast( y );

	if ( ix < x1 ) {
		x1 = ix;
	}
	if ( ix > x2 ) {
		x2 = ix;
	}
	if ( iy < y1 ) {
		y1 = iy;
	}
	if ( iy > y2 ) {
		y2 = iy;
	}
}

/*
======================
idScreenRect::Expand
======================
*/
void idScreenRect::Expand() {
	x1--;
	y1--;
	x2++;
	y2++;
}

/*
======================
idScreenRect::Intersect
======================
*/
void idScreenRect::Intersect( const idScreenRect &rect ) {
	if ( rect.x1 > x1 ) {
		x1 = rect.x1;
	}
	if ( rect.x2 < x2 ) {
		x2 = rect.x2;
	}
	if ( rect.y1 > y1 ) {
		y1 = rect.y1;
	}
	if ( rect.y2 < y2 ) {
		y2 = rect.y2;
	}
}

/*
======================
idScreenRect::Union
======================
*/
void idScreenRect::Union( const idScreenRect &rect ) {
	if ( rect.x1 < x1 ) {
		x1 = rect.x1;
	}
	if ( rect.x2 > x2 ) {
		x2 = rect.x2;
	}
	if ( rect.y1 < y1 ) {
		y1 = rect.y1;
	}
	if ( rect.y2 > y2 ) {
		y2 = rect.y2;
	}
}

/*
======================
idScreenRect::Equals
======================
*/
bool idScreenRect::Equals( const idScreenRect &rect ) const {
	return ( x1 == rect.x1 && x2 == rect.x2 && y1 == rect.y1 && y2 == rect.y2 );
}

/*
======================
idScreenRect::IsEmpty
======================
*/
bool idScreenRect::IsEmpty() const {
	return ( x1 > x2 || y1 > y2 );
}

/*
======================
R_ScreenRectFromViewFrustumBounds
======================
*/
idScreenRect R_ScreenRectFromViewFrustumBounds( const idBounds &bounds ) {
	idScreenRect screenRect;

	screenRect.x1 = idMath::FtoiFast( 0.5f * ( 1.0f - bounds[1].y ) * ( tr.viewDef->viewport.x2 - tr.viewDef->viewport.x1 ) );
	screenRect.x2 = idMath::FtoiFast( 0.5f * ( 1.0f - bounds[0].y ) * ( tr.viewDef->viewport.x2 - tr.viewDef->viewport.x1 ) );
	screenRect.y1 = idMath::FtoiFast( 0.5f * ( 1.0f + bounds[0].z ) * ( tr.viewDef->viewport.y2 - tr.viewDef->viewport.y1 ) );
	screenRect.y2 = idMath::FtoiFast( 0.5f * ( 1.0f + bounds[1].z ) * ( tr.viewDef->viewport.y2 - tr.viewDef->viewport.y1 ) );

	if ( r_useDepthBoundsTest.GetInteger() ) {
		R_TransformEyeZToWin( -bounds[0].x, tr.viewDef->projectionMatrix, screenRect.zmin );
		R_TransformEyeZToWin( -bounds[1].x, tr.viewDef->projectionMatrix, screenRect.zmax );
	}

	return screenRect;
}

/*
======================
R_ShowColoredScreenRect
======================
*/
void R_ShowColoredScreenRect( const idScreenRect &rect, int colorIndex ) {
	if ( !rect.IsEmpty() ) {
		static idVec4 colors[] = { colorRed, colorGreen, colorBlue, colorYellow, colorMagenta, colorCyan, colorWhite, colorPurple };
		tr.viewDef->renderWorld->DebugScreenRect( colors[colorIndex & 7], rect, tr.viewDef );
	}
}

/*
====================
R_ToggleSmpFrame
====================
*/
void R_ToggleSmpFrame( void ) {
	R_FreeDeferredTriSurfs( frameData );

	// clear frame-temporary data
	frameData_t		*frame;
	frameMemoryBlock_t	*block;

	// update the highwater mark
	R_CountFrameData();

	frame = frameData;

	// reset the memory allocation to the first block
	frame->alloc = frame->memory;

	// clear all the blocks
	for ( block = frame->memory ; block ; block = block->next ) {
		block->used = 0;
	}

	R_ClearCommandChain();
}


//=====================================================

#define	MEMORY_BLOCK_SIZE	0x100000

/*
=====================
R_ShutdownFrameData
=====================
*/
void R_ShutdownFrameData( void ) {
	frameData_t *frame;
	frameMemoryBlock_t *block;

	// free any current data
	frame = frameData;
	if ( !frame ) {
		return;
	}

	R_FreeDeferredTriSurfs( frame );

	frameMemoryBlock_t *nextBlock;
	for ( block = frame->memory ; block ; block = nextBlock ) {
		nextBlock = block->next;
		Mem_Free( block );
	}
	Mem_Free( frame );
	frameData = NULL;
}

/*
=====================
R_InitFrameData
=====================
*/
void R_InitFrameData( void ) {
	int size;
	frameData_t *frame;
	frameMemoryBlock_t *block;

	R_ShutdownFrameData();

	frameData = (frameData_t *)Mem_ClearedAlloc( sizeof( *frameData ));
	frame = frameData;
	size = MEMORY_BLOCK_SIZE;
	block = (frameMemoryBlock_t *)Mem_Alloc( size + sizeof( *block ) );
	if ( !block ) {
		common->FatalError( "R_InitFrameData: Mem_Alloc() failed" );
	}
	block->size = size;
	block->used = 0;
	block->next = NULL;
	frame->memory = block;
	frame->memoryHighwater = 0;

	R_ToggleSmpFrame();
}

/*
================
R_CountFrameData
================
*/
int R_CountFrameData( void ) {
	frameData_t		*frame;
	frameMemoryBlock_t	*block;
	int				count;

	count = 0;
	frame = frameData;
	for ( block = frame->memory ; block ; block=block->next ) {
		count += block->used;
		if ( block == frame->alloc ) {
			break;
		}
	}

	// note if this is a new highwater mark
	if ( count > frame->memoryHighwater ) {
		frame->memoryHighwater = count;
	}

	return count;
}

/*
=================
R_StaticAlloc
=================
*/
void *R_StaticAlloc( int bytes ) {
	void	*buf;

	tr.pc.c_alloc++;

	tr.staticAllocCount += bytes;

	buf = Mem_Alloc( bytes );

	// don't exit on failure on zero length allocations since the old code didn't
	if ( !buf && ( bytes != 0 ) ) {
		common->FatalError( "R_StaticAlloc failed on %i bytes", bytes );
	}
	return buf;
}

/*
=================
R_ClearedStaticAlloc
=================
*/
void *R_ClearedStaticAlloc( int bytes ) {
	void	*buf;

	buf = R_StaticAlloc( bytes );
	SIMDProcessor->Memset( buf, 0, bytes );
	return buf;
}

/*
=================
R_StaticFree
=================
*/
void R_StaticFree( void *data ) {
	tr.pc.c_free++;
	Mem_Free( data );
}

/*
================
R_FrameAlloc

This data will be automatically freed when the
current frame's back end completes.

This should only be called by the front end.  The
back end shouldn't need to allocate memory.

If we passed smpFrame in, the back end could
alloc memory, because it will always be a
different frameData than the front end is using.

All temporary data, like dynamic tesselations
and local spaces are allocated here.

The memory will not move, but it may not be
contiguous with previous allocations even
from this frame.

The memory is NOT zero filled.
Should part of this be inlined in a macro?
================
*/
void *R_FrameAlloc( int bytes ) {
	frameData_t		*frame;
	frameMemoryBlock_t	*block;
	void			*buf;

	bytes = (bytes+16)&~15;
	// see if it can be satisfied in the current block
	frame = frameData;
	block = frame->alloc;

	if ( block->size - block->used >= bytes ) {
		buf = block->base + block->used;
		block->used += bytes;
		return buf;
	}

	// advance to the next memory block if available
	block = block->next;
	// create a new block if we are at the end of
	// the chain
	if ( !block ) {
		int		size;

		size = MEMORY_BLOCK_SIZE;
		block = (frameMemoryBlock_t *)Mem_Alloc( size + sizeof( *block ) );
		if ( !block ) {
			common->FatalError( "R_FrameAlloc: Mem_Alloc() failed" );
		}
		block->size = size;
		block->used = 0;
		block->next = NULL;
		frame->alloc->next = block;
	}

	// we could fix this if we needed to...
	if ( bytes > block->size ) {
		common->FatalError( "R_FrameAlloc of %i exceeded MEMORY_BLOCK_SIZE",
			bytes );
	}

	frame->alloc = block;

	block->used = bytes;

	return block->base;
}

/*
==================
R_ClearedFrameAlloc
==================
*/
void *R_ClearedFrameAlloc( int bytes ) {
	void	*r;

	r = R_FrameAlloc( bytes );
	SIMDProcessor->Memset( r, 0, bytes );
	return r;
}


/*
==================
R_FrameFree

This does nothing at all, as the frame data is reused every frame
and can only be stack allocated.

The only reason for it's existance is so functions that can
use either static or frame memory can set function pointers
to both alloc and free.
==================
*/
void R_FrameFree( void *data ) {
}



//==========================================================================

void R_AxisToModelMatrix( const idMat3 &axis, const idVec3 &origin, float modelMatrix[16] ) {
	modelMatrix[0] = axis[0][0];
	modelMatrix[4] = axis[1][0];
	modelMatrix[8] = axis[2][0];
	modelMatrix[12] = origin[0];

	modelMatrix[1] = axis[0][1];
	modelMatrix[5] = axis[1][1];
	modelMatrix[9] = axis[2][1];
	modelMatrix[13] = origin[1];

	modelMatrix[2] = axis[0][2];
	modelMatrix[6] = axis[1][2];
	modelMatrix[10] = axis[2][2];
	modelMatrix[14] = origin[2];

	modelMatrix[3] = 0;
	modelMatrix[7] = 0;
	modelMatrix[11] = 0;
	modelMatrix[15] = 1;
}


// FIXME: these assume no skewing or scaling transforms

void R_LocalPointToGlobal( const float modelMatrix[16], const idVec3 &in, idVec3 &out ) {
#if defined(__GNUC__) && defined(__SSE2__)
	__m128 m0, m1, m2, m3;
	__m128 in0, in1, in2;
	float i0,i1,i2;
	i0 = in[0];
	i1 = in[1];
	i2 = in[2];

	m0 = _mm_loadu_ps(&modelMatrix[0]);
	m1 = _mm_loadu_ps(&modelMatrix[4]);
	m2 = _mm_loadu_ps(&modelMatrix[8]);
	m3 = _mm_loadu_ps(&modelMatrix[12]);

	in0 = _mm_load1_ps(&i0);
	in1 = _mm_load1_ps(&i1);
	in2 = _mm_load1_ps(&i2);

	m0 = _mm_mul_ps(m0, in0);
	m1 = _mm_mul_ps(m1, in1);
	m2 = _mm_mul_ps(m2, in2);

	m0 = _mm_add_ps(m0, m1);
	m0 = _mm_add_ps(m0, m2);
	m0 = _mm_add_ps(m0, m3);

	_mm_store_ss(&out[0], m0);
	m1 = (__m128) _mm_shuffle_epi32((__m128i)m0, 0x55);
	_mm_store_ss(&out[1], m1);
	m2 = _mm_movehl_ps(m2, m0);
	_mm_store_ss(&out[2], m2);
#else
	out[0] = in[0] * modelMatrix[0] + in[1] * modelMatrix[4]
		+ in[2] * modelMatrix[8] + modelMatrix[12];
	out[1] = in[0] * modelMatrix[1] + in[1] * modelMatrix[5]
		+ in[2] * modelMatrix[9] + modelMatrix[13];
	out[2] = in[0] * modelMatrix[2] + in[1] * modelMatrix[6]
		+ in[2] * modelMatrix[10] + modelMatrix[14];
#endif
}

void R_PointTimesMatrix( const float modelMatrix[16], const idVec4 &in, idVec4 &out ) {
	out[0] = in[0] * modelMatrix[0] + in[1] * modelMatrix[4]
		+ in[2] * modelMatrix[8] + modelMatrix[12];
	out[1] = in[0] * modelMatrix[1] + in[1] * modelMatrix[5]
		+ in[2] * modelMatrix[9] + modelMatrix[13];
	out[2] = in[0] * modelMatrix[2] + in[1] * modelMatrix[6]
		+ in[2] * modelMatrix[10] + modelMatrix[14];
	out[3] = in[0] * modelMatrix[3] + in[1] * modelMatrix[7]
		+ in[2] * modelMatrix[11] + modelMatrix[15];
}

void R_GlobalPointToLocal( const float modelMatrix[16], const idVec3 &in, idVec3 &out ) {
	idVec3	temp;

	VectorSubtract( in, &modelMatrix[12], temp );

	out[0] = DotProduct( temp, &modelMatrix[0] );
	out[1] = DotProduct( temp, &modelMatrix[4] );
	out[2] = DotProduct( temp, &modelMatrix[8] );
}

void R_LocalVectorToGlobal( const float modelMatrix[16], const idVec3 &in, idVec3 &out ) {
	out[0] = in[0] * modelMatrix[0] + in[1] * modelMatrix[4]
		+ in[2] * modelMatrix[8];
	out[1] = in[0] * modelMatrix[1] + in[1] * modelMatrix[5]
		+ in[2] * modelMatrix[9];
	out[2] = in[0] * modelMatrix[2] + in[1] * modelMatrix[6]
		+ in[2] * modelMatrix[10];
}

void R_GlobalVectorToLocal( const float modelMatrix[16], const idVec3 &in, idVec3 &out ) {
	out[0] = DotProduct( in, &modelMatrix[0] );
	out[1] = DotProduct( in, &modelMatrix[4] );
	out[2] = DotProduct( in, &modelMatrix[8] );
}

void R_GlobalPlaneToLocal( const float modelMatrix[16], const idPlane &in, idPlane &out ) {
	out[0] = DotProduct( in, &modelMatrix[0] );
	out[1] = DotProduct( in, &modelMatrix[4] );
	out[2] = DotProduct( in, &modelMatrix[8] );
	out[3] = in[3] + modelMatrix[12] * in[0] + modelMatrix[13] * in[1] + modelMatrix[14] * in[2];
}

void R_LocalPlaneToGlobal( const float modelMatrix[16], const idPlane &in, idPlane &out ) {
	float	offset;

	R_LocalVectorToGlobal( modelMatrix, in.Normal(), out.Normal() );

	offset = modelMatrix[12] * out[0] + modelMatrix[13] * out[1] + modelMatrix[14] * out[2];
	out[3] = in[3] - offset;
}

// transform Z in eye coordinates to window coordinates
void R_TransformEyeZToWin( float src_z, const float *projectionMatrix, float &dst_z ) {
	float clip_z, clip_w;

	// projection
	clip_z = src_z * projectionMatrix[ 2 + 2 * 4 ] + projectionMatrix[ 2 + 3 * 4 ];
	clip_w = src_z * projectionMatrix[ 3 + 2 * 4 ] + projectionMatrix[ 3 + 3 * 4 ];

	if ( clip_w <= 0.0f ) {
		dst_z = 0.0f;					// clamp to near plane
	} else {
		dst_z = clip_z / clip_w;
		dst_z = dst_z * 0.5f + 0.5f;	// convert to window coords
	}
}

/*
=================
R_RadiusCullLocalBox

A fast, conservative center-to-corner culling test
Returns true if the box is outside the given global frustum, (positive sides are out)
=================
*/
bool R_RadiusCullLocalBox( const idBounds &bounds, const float modelMatrix[16], int numPlanes, const idPlane *planes ) {
	int			i;
	float		d;
	idVec3		worldOrigin;
	float		worldRadius;
	const idPlane	*frust;

	if ( r_useCulling.GetInteger() == 0 ) {
		return false;
	}

	// transform the surface bounds into world space
	idVec3	localOrigin = ( bounds[0] + bounds[1] ) * 0.5;

	R_LocalPointToGlobal( modelMatrix, localOrigin, worldOrigin );

	worldRadius = (bounds[0] - localOrigin).Length();	// FIXME: won't be correct for scaled objects

	for ( i = 0 ; i < numPlanes ; i++ ) {
		frust = planes + i;
		d = frust->Distance( worldOrigin );
		if ( d > worldRadius ) {
			return true;	// culled
		}
	}

	return false;		// no culled
}

/*
=================
R_CornerCullLocalBox

Tests all corners against the frustum.
Can still generate a few false positives when the box is outside a corner.
Returns true if the box is outside the given global frustum, (positive sides are out)
=================
*/
bool R_CornerCullLocalBox( const idBounds &bounds, const float modelMatrix[16], int numPlanes, const idPlane *planes ) {
	int			i, j;
	idVec3		transformed[8];
	float		dists[8];
	idVec3		v;
	const idPlane *frust;

	// we can disable box culling for experimental timing purposes
	if ( r_useCulling.GetInteger() < 2 ) {
		return false;
	}

	// transform into world space
	for ( i = 0 ; i < 8 ; i++ ) {
		v[0] = bounds[i&1][0];
		v[1] = bounds[(i>>1)&1][1];
		v[2] = bounds[(i>>2)&1][2];

		R_LocalPointToGlobal( modelMatrix, v, transformed[i] );
	}

	// check against frustum planes
	for ( i = 0 ; i < numPlanes ; i++ ) {
		frust = planes + i;
		for ( j = 0 ; j < 8 ; j++ ) {
			dists[j] = frust->Distance( transformed[j] );
			if ( dists[j] < 0 ) {
				break;
			}
		}
		if ( j == 8 ) {
			// all points were behind one of the planes
			tr.pc.c_box_cull_out++;
			return true;
		}
	}

	tr.pc.c_box_cull_in++;

	return false;		// not culled
}

/*
=================
R_CullLocalBox

Performs quick test before expensive test
Returns true if the box is outside the given global frustum, (positive sides are out)
=================
*/
bool R_CullLocalBox( const idBounds &bounds, const float modelMatrix[16], int numPlanes, const idPlane *planes ) {
	if ( R_RadiusCullLocalBox( bounds, modelMatrix, numPlanes, planes ) ) {
		return true;
	}
	return R_CornerCullLocalBox( bounds, modelMatrix, numPlanes, planes );
}

/*
=================
GPU frustum-cull validation (Phase 3.1 / 3.2 — docs/gpu-offload-plan.md)

Two once/sec, no-draw validators share one compute kernel (cs_gpucull) and one compare/report
helper (R_GpuCull_RunAndReport). Both diff the GPU survivor set against the CPU R_CullLocalBox
reference; a wrong kernel can only FAIL because the reference IS the shipping cull.

 - r_gpuCullTest (Phase 3.1): a deterministic SYNTHETIC object set. Validates the cull machinery
   in isolation — per-object table upload, cull-math parity (radius + corner, r_useCulling-gated
   so GPU==CPU under every mode), atomic compaction into a VkDrawIndexedIndirectCommand[] + count.
 - r_gpuCullLive (Phase 3.2): the REAL per-frame surface set. The front-end (R_AddAmbientDrawsurfs)
   records every ambient-cull candidate — real tri->bounds, real vEntity->modelMatrix, real
   tri->numIndexes, and the actual CPU decision — and this proves the GPU cull reproduces
   R_CullLocalBox on live geometry: dynamic/animated bounds, parented transforms, the live frustum.
   This is the real-data half of wiring the cull in. (Indirect *consumption* is a further step:
   vkCmdDrawIndexedIndirect binds one vb/ib for the whole multi-draw, but RB_RHI_StreamAmbient
   hands back a different buffer per surface, so it needs a unified geometry buffer first — see
   docs/gpu-offload-plan.md Phase 3.2b/3.3.)

Vulkan only (GL3 has no compute lane); mirrors r_gpuSkinTest / r_tessDeformTest. The model matrix
is id column-major (basis vectors as columns, translation in [12..14]), which is exactly GLSL's
mat4 convention, so `m * vec4(p,1)` reproduces R_LocalPointToGlobal with NO transpose. idPlane
test dot(pl.xyz,w)+pl.w == idPlane::Distance; positive side is OUT.
=================
*/
static idCVar r_gpuCullTest( "r_gpuCullTest", "0", CVAR_RENDERER | CVAR_BOOL,
	"validate GPU frustum cull vs CPU R_CullLocalBox over a synthetic object set, no draw (Vulkan; once/sec)" );
static idCVar r_gpuCullLive( "r_gpuCullLive", "0", CVAR_RENDERER | CVAR_BOOL,
	"validate GPU frustum cull vs CPU R_CullLocalBox over the REAL per-frame surface set, no draw (Vulkan; once/sec)" );

static const char *GPUCULL_SRC =
	"#version 450\n"
	"layout(local_size_x = 64) in;\n"
	"struct Obj { vec4 bmin; vec4 bmax; mat4 model; };\n"
	"layout(std430, binding=0) readonly  buffer Objects  { Obj  objs[]; };\n"
	"layout(std430, binding=1) readonly  buffer Frustum  { vec4 planes[]; };\n"
	"layout(std430, binding=2) writeonly buffer OutCmds  { uint cmds[]; };\n"		// 5 uints/survivor = VkDrawIndexedIndirectCommand
	"layout(std430, binding=3)           buffer OutCount { uint count; };\n"
	"layout(std430, binding=4) readonly  buffer DrawParm { uvec2 dp[]; };\n"		// (indexCount, firstIndex==objIndex)
	"layout(push_constant) uniform PC { uint numObjects; uint numPlanes; uint useCulling; uint pad; } pc;\n"
	"void main() {\n"
	"    uint i = gl_GlobalInvocationID.x;\n"
	"    if ( i >= pc.numObjects ) return;\n"
	"    Obj o = objs[i];\n"
	"    vec3 mn = o.bmin.xyz, mx = o.bmax.xyz;\n"
	"    bool culled = false;\n"
	"    if ( pc.useCulling >= 1u ) {\n"							// R_RadiusCullLocalBox
	"        vec3 lc = (mn + mx) * 0.5;\n"
	"        vec3 wc = (o.model * vec4(lc,1.0)).xyz;\n"
	"        float rad = length(mn - lc);\n"
	"        for ( uint p = 0u; p < pc.numPlanes; p++ )\n"
	"            if ( dot(planes[p].xyz, wc) + planes[p].w > rad ) { culled = true; break; }\n"
	"    }\n"
	"    if ( !culled && pc.useCulling >= 2u ) {\n"					// R_CornerCullLocalBox
	"        vec3 cw[8];\n"
	"        for ( uint k = 0u; k < 8u; k++ ) {\n"
	"            vec3 lp = vec3( ((k&1u)!=0u)?mx.x:mn.x, ((k&2u)!=0u)?mx.y:mn.y, ((k&4u)!=0u)?mx.z:mn.z );\n"
	"            cw[k] = (o.model * vec4(lp,1.0)).xyz;\n"
	"        }\n"
	"        for ( uint p = 0u; p < pc.numPlanes; p++ ) {\n"
	"            bool allOut = true;\n"
	"            for ( uint k = 0u; k < 8u; k++ )\n"
	"                if ( dot(planes[p].xyz, cw[k]) + planes[p].w < 0.0 ) { allOut = false; break; }\n"
	"            if ( allOut ) { culled = true; break; }\n"
	"        }\n"
	"    }\n"
	"    if ( !culled ) {\n"
	"        uint s = atomicAdd(count, 1u) * 5u;\n"
	"        cmds[s+0u] = dp[i].x;\n"			// indexCount
	"        cmds[s+1u] = 1u;\n"				// instanceCount
	"        cmds[s+2u] = dp[i].y;\n"			// firstIndex == object index (survivor tag)
	"        cmds[s+3u] = 0u;\n"				// vertexOffset
	"        cmds[s+4u] = 0u;\n"				// firstInstance
	"    }\n"
	"}\n";

static rhi::ShaderHandle R_GpuCullShader( rhi::RHI *r ) {
	static rhi::ShaderHandle handle = 0;
	static bool tried = false;
	if ( !tried ) {
		tried = true;
		handle = r->CreateComputeShader( "cs_gpucull", GPUCULL_SRC );
	}
	return handle;
}

/*
=================
R_GpuCull_RunAndReport

Shared by both validators: upload the object table + the live view frustum, dispatch the cull
kernel, read back the compacted survivor list, and diff it (by survivor tag == object index)
against the CPU cpuVis[] reference. Disagreements are bucketed — a box straddling a frustum plane
(a corner within an epsilon of Distance==0) can legitimately flip between the CPU SSE path and the
GPU's IEEE floats (FP boundary noise, not a logic bug); anything else is a genuine divergence. PASS
iff 0 genuine + 0 bad tags. DispatchSync / ReadBuffer stall the GPU, so this is dev-only.

objBlob is N * (vec4 bmin, vec4 bmax, mat4 model) = N*24 floats (std430 struct Obj, 96 B each);
dpBlob is N * uvec2 (indexCount, tag). The boundary classifier reads each object's bounds+matrix
straight back out of objBlob, so it works for any object set (synthetic or live).
=================
*/
static void R_GpuCull_RunAndReport( rhi::RHI *r, rhi::ShaderHandle shader, const char *label,
		int N, const float *objBlob, const unsigned int *dpBlob, const bool *cpuVis, int cpuVisCount ) {
	const int NP = 5;					// the view path uses viewDef->frustum[5]

	rhi::BufferHandle bObjs  = r->CreateBuffer( rhi::BU_STORAGE, N * 24 * (int)sizeof( float ), objBlob );
	rhi::BufferHandle bFrust = r->CreateBuffer( rhi::BU_STORAGE, NP * 4 * (int)sizeof( float ), tr.viewDef->frustum );
	rhi::BufferHandle bCmds  = r->CreateBuffer( rhi::BU_STORAGE, N * 5 * (int)sizeof( unsigned int ), NULL );
	unsigned int zero = 0;
	rhi::BufferHandle bCount = r->CreateBuffer( rhi::BU_STORAGE, (int)sizeof( unsigned int ), &zero );
	rhi::BufferHandle bDP    = r->CreateBuffer( rhi::BU_STORAGE, N * 2 * (int)sizeof( unsigned int ), dpBlob );

	struct { unsigned int numObjects, numPlanes, useCulling, pad; } pc;
	pc.numObjects = (unsigned int)N;
	pc.numPlanes  = (unsigned int)NP;
	pc.useCulling = (unsigned int)r_useCulling.GetInteger();
	pc.pad = 0;

	rhi::ComputeArgs ca;
	memset( &ca, 0, sizeof( ca ) );
	ca.shader = shader;
	ca.storage[0] = bObjs;
	ca.storage[1] = bFrust;
	ca.storage[2] = bCmds;
	ca.storage[3] = bCount;
	ca.storage[4] = bDP;
	ca.pushConstants = &pc;
	ca.pushConstantSize = (int)sizeof( pc );
	ca.groupsX = ( N + 63 ) / 64;
	ca.groupsY = 1;
	ca.groupsZ = 1;
	r->DispatchSync( ca );

	unsigned int gpuCount = 0;
	r->ReadBuffer( bCount, &gpuCount, (int)sizeof( unsigned int ) );
	if ( (int)gpuCount > N ) {
		gpuCount = (unsigned int)N;
	}
	unsigned int *cmds = (unsigned int *)Mem_Alloc16( N * 5 * (int)sizeof( unsigned int ) );
	r->ReadBuffer( bCmds, cmds, N * 5 * (int)sizeof( unsigned int ) );

	// GPU survivor set keyed by the firstIndex tag (== object index); atomic order is nondeterministic
	bool *gpuVis = (bool *)Mem_Alloc16( N * (int)sizeof( bool ) );
	memset( gpuVis, 0, N * (int)sizeof( bool ) );
	int badTag = 0;
	for ( unsigned int c = 0; c < gpuCount; c++ ) {
		unsigned int idx = cmds[c * 5 + 2];
		if ( (int)idx < N ) {
			gpuVis[idx] = true;
		} else {
			badTag++;
		}
	}

	// Classify each disagreement (see header): |cornerMargin| ~ 0 is the decision boundary. Read
	// this object's bounds+matrix back out of objBlob so the same code serves synthetic and live.
	int mismatch = 0, boundary = 0, genuine = 0, firstGenuine = -1;
	for ( int i = 0; i < N; i++ ) {
		if ( gpuVis[i] == cpuVis[i] ) {
			continue;
		}
		mismatch++;
		const float *o = objBlob + i * 24;
		const idBounds lb( idVec3( o[0], o[1], o[2] ), idVec3( o[4], o[5], o[6] ) );
		const float *m = o + 8;
		float cornerMargin = -1e30f;			// max over planes of ( min over corners of Distance )
		for ( int p = 0; p < NP; p++ ) {
			const idPlane &pl = tr.viewDef->frustum[p];
			float minCorner = 1e30f;
			for ( int k = 0; k < 8; k++ ) {
				idVec3 lv( ( k & 1 ) ? lb[1][0] : lb[0][0],
				           ( k & 2 ) ? lb[1][1] : lb[0][1],
				           ( k & 4 ) ? lb[1][2] : lb[0][2] );
				idVec3 wv;
				R_LocalPointToGlobal( m, lv, wv );
				const float d = pl.Distance( wv );
				if ( d < minCorner ) {
					minCorner = d;
				}
			}
			if ( minCorner > cornerMargin ) {
				cornerMargin = minCorner;
			}
		}
		if ( idMath::Fabs( cornerMargin ) < 0.05f ) {
			boundary++;
		} else {
			genuine++;
			if ( firstGenuine < 0 ) {
				firstGenuine = i;
			}
		}
	}

	common->Printf( "%s: %d objs, r_useCulling %d, CPU vis %d, GPU vis %u -- %s "
		"(mismatch %d = %d boundary-FP + %d genuine, first genuine @%d, badTag %d)\n",
		label, N, r_useCulling.GetInteger(), cpuVisCount, gpuCount,
		( genuine == 0 && badTag == 0 ) ? "PASS" : "FAIL",
		mismatch, boundary, genuine, firstGenuine, badTag );

	r->DestroyBuffer( bObjs );
	r->DestroyBuffer( bFrust );
	r->DestroyBuffer( bCmds );
	r->DestroyBuffer( bCount );
	r->DestroyBuffer( bDP );
	Mem_Free16( gpuVis );
	Mem_Free16( cmds );
}

static void R_GpuCullValidate( void ) {
	if ( !r_gpuCullTest.GetBool() || tr.viewDef == NULL || tr.viewDef->viewEntitys == NULL ) {
		return;
	}
	static int s_lastMs = 0;
	const int now = Sys_Milliseconds();
	if ( now - s_lastMs < 1000 ) {
		return;						// DispatchSync stalls; dev-only, rate-limited like the other validators
	}
	s_lastMs = now;

	rhi::RHI *r = rhi::GetRHI();
	if ( r == NULL ) {
		return;
	}
	rhi::ShaderHandle shader = R_GpuCullShader( r );
	if ( shader == 0 ) {
		common->Printf( "r_gpuCullTest: no compute lane (GL3) - Vulkan only\n" );
		r_gpuCullTest.SetBool( false );
		return;
	}

	const int N = 2048;
	const int NP = 5;					// the view path uses viewDef->frustum[5]
	const idVec3 vieworg = tr.viewDef->renderView.vieworg;
	const idBounds localBounds( idVec3( -16.0f, -16.0f, -16.0f ), idVec3( 16.0f, 16.0f, 16.0f ) );

	// vec4 bmin, vec4 bmax, mat4 model (std430 struct Obj = 96 B); the CPU ref + boundary classifier
	// read each matrix back out of objBlob at +8, so there is no parallel matAll[]
	float *objBlob = (float *)Mem_Alloc16( N * 24 * (int)sizeof( float ) );
	unsigned int *dpBlob = (unsigned int *)Mem_Alloc16( N * 2 * (int)sizeof( unsigned int ) );

	unsigned int rng = 2463534242u;		// deterministic xorshift — reproducible object set
	for ( int i = 0; i < N; i++ ) {
		float f[6];
		for ( int j = 0; j < 6; j++ ) {
			rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
			f[j] = ( ( rng >> 8 ) & 0xFFFFFF ) / (float)0x1000000;	// [0,1)
		}
		idVec3 origin( vieworg[0] + ( f[0] * 2.0f - 1.0f ) * 600.0f,
		               vieworg[1] + ( f[1] * 2.0f - 1.0f ) * 600.0f,
		               vieworg[2] + ( f[2] * 2.0f - 1.0f ) * 600.0f );
		idAngles ang( ( f[3] * 2.0f - 1.0f ) * 180.0f, ( f[4] * 2.0f - 1.0f ) * 180.0f, ( f[5] * 2.0f - 1.0f ) * 180.0f );

		float *o = objBlob + i * 24;
		o[0] = localBounds[0][0]; o[1] = localBounds[0][1]; o[2] = localBounds[0][2]; o[3] = 0.0f;
		o[4] = localBounds[1][0]; o[5] = localBounds[1][1]; o[6] = localBounds[1][2]; o[7] = 0.0f;
		R_AxisToModelMatrix( ang.ToMat3(), origin, o + 8 );

		dpBlob[i * 2 + 0] = (unsigned int)( i * 3 );	// placeholder indexCount (synthetic geometry has none)
		dpBlob[i * 2 + 1] = (unsigned int)i;			// tag doubles as firstIndex/survivor key
	}

	// CPU reference: the actual renderer cull over the same objects + live view frustum
	bool *cpuVis = (bool *)Mem_Alloc16( N * (int)sizeof( bool ) );
	int cpuVisCount = 0;
	for ( int i = 0; i < N; i++ ) {
		cpuVis[i] = !R_CullLocalBox( localBounds, objBlob + i * 24 + 8, NP, tr.viewDef->frustum );
		if ( cpuVis[i] ) {
			cpuVisCount++;
		}
	}

	R_GpuCull_RunAndReport( r, shader, "r_gpuCullTest", N, objBlob, dpBlob, cpuVis, cpuVisCount );

	Mem_Free16( objBlob );
	Mem_Free16( dpBlob );
	Mem_Free16( cpuVis );
}

/*
=================
Live GPU-cull candidate collector (Phase 3.2 — r_gpuCullLive)

The front-end (R_AddAmbientDrawsurfs) pushes every ambient-cull candidate here so R_GpuCullLive()
can replay the exact per-frame surface set on the GPU. Armed at most once/sec — the first view of
that second wins — so the per-surface recording cost is paid only on the validated frame; otherwise
R_GpuCullLiveActive() is false and R_GpuCull_RecordCandidate() early-outs. The collector is a
grow-only dev scratch buffer: objBlob-format bounds+matrix (std430 struct Obj), dpBlob
(numIndexes, tag), and the CPU R_CullLocalBox decision per candidate.
=================
*/
static float *			s_liveObj = NULL;		// N * 24 floats (vec4 bmin, vec4 bmax, mat4 model)
static unsigned int *	s_liveDP = NULL;		// N * uvec2 (indexCount, tag)
static bool *			s_liveCulled = NULL;	// N bools (front-end R_CullLocalBox result)
static int				s_liveCount = 0;
static int				s_liveCap = 0;
static bool				s_liveArmed = false;
static int				s_liveLastMs = 0;

void R_GpuCull_ResetLive( void ) {
	s_liveArmed = false;
	if ( !r_gpuCullLive.GetBool() ) {
		return;
	}
	const int now = Sys_Milliseconds();
	if ( now - s_liveLastMs < 1000 ) {
		return;						// DispatchSync stalls; arm the collector at most once/sec
	}
	s_liveLastMs = now;				// claim this second so nested subviews don't also record/fire
	s_liveArmed = true;
	s_liveCount = 0;
}

bool R_GpuCullLiveActive( void ) {
	return s_liveArmed;
}

void R_GpuCull_RecordCandidate( const idBounds &bounds, const float modelMatrix[16], int numIndexes, bool culled ) {
	if ( !s_liveArmed ) {
		return;
	}
	if ( s_liveCount >= s_liveCap ) {
		const int newCap = s_liveCap ? s_liveCap * 2 : 4096;
		float *no        = (float *)Mem_Alloc16( newCap * 24 * (int)sizeof( float ) );
		unsigned int *nd = (unsigned int *)Mem_Alloc16( newCap * 2 * (int)sizeof( unsigned int ) );
		bool *nc         = (bool *)Mem_Alloc16( newCap * (int)sizeof( bool ) );
		if ( s_liveCount > 0 ) {
			memcpy( no, s_liveObj,    s_liveCount * 24 * sizeof( float ) );
			memcpy( nd, s_liveDP,     s_liveCount * 2 * sizeof( unsigned int ) );
			memcpy( nc, s_liveCulled, s_liveCount * sizeof( bool ) );
		}
		if ( s_liveObj )    { Mem_Free16( s_liveObj ); }
		if ( s_liveDP )     { Mem_Free16( s_liveDP ); }
		if ( s_liveCulled ) { Mem_Free16( s_liveCulled ); }
		s_liveObj = no; s_liveDP = nd; s_liveCulled = nc; s_liveCap = newCap;
	}
	float *o = s_liveObj + s_liveCount * 24;
	o[0] = bounds[0][0]; o[1] = bounds[0][1]; o[2] = bounds[0][2]; o[3] = 0.0f;
	o[4] = bounds[1][0]; o[5] = bounds[1][1]; o[6] = bounds[1][2]; o[7] = 0.0f;
	for ( int k = 0; k < 16; k++ ) {
		o[8 + k] = modelMatrix[k];
	}
	s_liveDP[s_liveCount * 2 + 0] = (unsigned int)numIndexes;	// real index count — feeds a future indirect draw
	s_liveDP[s_liveCount * 2 + 1] = (unsigned int)s_liveCount;	// tag == firstIndex/survivor key
	s_liveCulled[s_liveCount] = culled;
	s_liveCount++;
}

/*
=================
R_GpuCullLive (Phase 3.2 — r_gpuCullLive)

Once/sec, replay the collected real per-frame surface set through the same cull kernel and prove
the GPU survivor set matches the front-end's own R_CullLocalBox decisions. No draw. Hooked right
after R_AddModelSurfaces (tr.viewDef live, frustum already constrained, no backend pass open, so
DispatchSync is safe). Vulkan only; self-gates when off.
=================
*/
static void R_GpuCullLive( void ) {
	if ( !s_liveArmed || tr.viewDef == NULL ) {
		return;
	}
	s_liveArmed = false;			// consume the armed slot; nested subviews this second stay off
	if ( s_liveCount <= 0 ) {
		return;
	}

	rhi::RHI *r = rhi::GetRHI();
	if ( r == NULL ) {
		return;
	}
	rhi::ShaderHandle shader = R_GpuCullShader( r );
	if ( shader == 0 ) {
		common->Printf( "r_gpuCullLive: no compute lane (GL3) - Vulkan only\n" );
		r_gpuCullLive.SetBool( false );
		return;
	}

	const int N = s_liveCount;
	bool *cpuVis = (bool *)Mem_Alloc16( N * (int)sizeof( bool ) );
	int cpuVisCount = 0;
	for ( int i = 0; i < N; i++ ) {
		cpuVis[i] = !s_liveCulled[i];		// the front-end already ran R_CullLocalBox on this surface
		if ( cpuVis[i] ) {
			cpuVisCount++;
		}
	}

	R_GpuCull_RunAndReport( r, shader, "r_gpuCullLive", N, s_liveObj, s_liveDP, cpuVis, cpuVisCount );

	Mem_Free16( cpuVis );
}

/*
=================
R2 world ray-query scene + validator (docs/rtx-shadow-roadmap.md)

r_rtWorld: build and KEEP the scene acceleration structure the RT shadow rays consume:
one BLAS per _areaN world model (identity instances) PLUS one BLAS per unique STATIC
ENTITY model (func_statics, inline brush models - doors, crates, rocks, buildings) with
one TLAS instance per entity. Outdoor sun shadows come mostly from those entity models,
not worldspawn - a worldspawn-only scene silently deletes them (found by the user on
mars_city1). Movers are snapshotted at build time (a door captured closed stays closed
to the rays) until the per-frame TLAS rebuild lands. Built lazily on the first primary
view, rebuilt on map change, torn down when switched off; a vid_restart self-heals
(backend owns the AS objects; GetTlasAddress()==0 here means rebuild).

r_rtWorldTest: one-shot validator (self-clears). Traces an 8x8 ray grid across the
CURRENT view frustum against the scene - the persistent one when live, else a transient
build - and diffs every hit distance against a CPU Moller-Trumbore sweep over the same
triangle soup, entity instances included (rays inverse-transformed into model space; the
affine map preserves the ray parameter, so t compares directly). A hit/miss flip that a
slightly perturbed CPU ray reproduces is bucketed boundary-FP, mirroring the GPU-cull
validator; PASS iff 0 genuine. Caveat: against the persistent scene, an entity that
MOVED since the build (a door) can flag genuine - re-run r_rtWorld to resync.
Vulkan + RT hardware only.
=================
*/
static idCVar r_rtWorld( "r_rtWorld", "0", CVAR_RENDERER | CVAR_BOOL,
	"build + keep the world ray-query acceleration structure: worldspawn areas + static entity models (Vulkan + RT hardware; prereq for RT shadows, auto-implied by r_rtSunShadows)" );
static idCVar r_rtWorldTest( "r_rtWorldTest", "0", CVAR_RENDERER | CVAR_BOOL,
	"validate the ray-query world scene (areas + static entity instances) vs a CPU ray trace (Vulkan + RT hardware; one-shot, self-clears)" );
static idCVar r_rtAnimBlasTest( "r_rtAnimBlasTest", "0", CVAR_RENDERER | CVAR_BOOL,
	"validate an animated monster's BLAS built from its GPU-skinned gpuSkinVB (build + refit) vs a CPU trace of the same buffers (Vulkan + RT hardware; needs a GPU-skinned caster in view; one-shot, self-clears)" );
static idCVar r_rtAnimBlas( "r_rtAnimBlas", "1", CVAR_RENDERER | CVAR_BOOL,
	"S3/S5 (docs/rtx-animated-blas.md): maintain a per-entity animated BLAS cache from GPU-skinned casters' gpuSkinVB (built/refit after the skin flush) and instance it into the TLAS. ON BY DEFAULT (S5 flip): its consumer landed - RT reflections need it to see monsters (without it a reflection ray hits a defer row and shades the monster BLACK). Inert on GL3 / non-RT hardware and when no RT scene (TLAS) is built, so it only costs where an RT feature is already active. 0 = monsters absent from the RT geometry table (black in reflections, no RT monster shadows)" );
static idCVar r_rtReflTest( "r_rtReflTest", "0", CVAR_RENDERER | CVAR_BOOL,
	"RR1 (docs/rtx-reflections.md): validate the reflection-hit attribute fetch — trace rays at a GPU-skinned monster, fetch the hit's interpolated world normal from gpuSkinVB via the geometry table + buffer_reference, and diff vs a CPU barycentric reference (Vulkan + RT hardware; needs a GPU-skinned monster in view; one-shot, self-clears)" );

struct rtAreaSlice_t { int vertStart, numVerts, idxStart, numIdx; };
struct rtModelSlice_t {
	const idRenderModel *	model;			// unique-model key
	int						vertStart, numVerts, idxStart, numIdx;
	rhi::BlasHandle			blas;			// filled by R_RtBuildScene
};
struct rtModelInst_t {
	int						slice;			// index into the model-slice array
	float					mat[16];		// id column-major model matrix (world = M * local)
};

// persistent BLAS registry for the per-frame TLAS refresh (movers): filled by the
// persistent-scene build, then walked every frame to re-instance with CURRENT entity poses
struct rtModelBlas_t { const idRenderModel *model; rhi::BlasHandle blas; };
static idList<rhi::BlasHandle>	s_rtAreaBlas;
static idList<rtModelBlas_t>	s_rtModelBlas;
// RR5: dedicated device-addressable buffers holding the static world's full idDrawVert + 32-bit indices,
// feeding the per-surface world BLAS so a reflection ray can fetch st + material at a world hit. Persist
// with the scene (the geo table references them each frame); freed on scene (re)build via R_RtFreeWorldGeo.
static rhi::BufferHandle		s_rtWorldVB = 0;
static rhi::BufferHandle		s_rtWorldIB = 0;

// shared caster filter: opaque OR perforated + shadow-casting, CPU data resident.
// PERFORATED (alpha-tested) surfaces cast too - Doom 3's own stencil shadows treat them as
// SOLID occluders (the shadow volume is the full triangle silhouette, ignoring the alpha
// holes), so admitting them as solid ray occluders is faithful, and it is essential for
// characters: monster skins are mostly perforated (a body's 9 surfaces are typically 1 opaque
// + 8 perforated), so an opaque-only filter cast a holey, glitchy partial silhouette or nothing.
// Only TRANSLUCENT (glass/glows/additive) is excluded. SurfaceCastsShadow() still drops
// noshadows materials - critically the SKY, or every RT sun ray would end in the dome.
static bool R_RtSurfCasts( const modelSurface_t *surf ) {
	return surf->geometry != NULL && surf->shader != NULL
		&& ( surf->shader->Coverage() == MC_OPAQUE || surf->shader->Coverage() == MC_PERFORATED )
		&& surf->shader->SurfaceCastsShadow()
		&& surf->geometry->verts != NULL && surf->geometry->indexes != NULL;
}

// R3.5 S4: a GPU-skinned casting surface the animated per-entity BLAS path owns (r_rtAnimBlas on,
// device-addressable gpuSkinVB + static index buffer). The CPU-soup monster gather must SKIP these
// (else a caster shows a duplicate shadow), and R_RtStageAnimCasters must gather exactly these — so
// both call this one predicate to partition casting surfaces cleanly. Assumes R_RtSurfCasts already true.
static bool R_RtSurfViaAnimBlas( const modelSurface_t *surf ) {
	const srfTriangles_t *tri = surf->geometry;
	return r_rtAnimBlas.GetBool() && tri != NULL && tri->gpuSkinVB != 0
		&& tri->indexCache != NULL && tri->indexCache->vbo != 0;
}

// True when this view renders a world the persistent RT scene may bind to: the primary pass
// of a world with a REAL loaded map. GUI render worlds (menu/PDA 3D scenes) also render
// non-subview views and even claim tr.primaryWorld (RenderWorld.cpp RenderScene), but they
// are ClearWorld()-built - one bare area, empty mapName - and their _areaN FindModel lookups
// would resolve to the GAME map's models still resident in the global model manager. The
// mapName gate excludes them outright (found the hard way: a menu view rebuilt the scene
// down to _area0's 12 triangles).
static idRenderWorldLocal *R_RtSceneWorld( void ) {
	if ( tr.viewDef == NULL || tr.viewDef->isSubview || tr.viewDef->renderWorld == NULL ) {
		return NULL;
	}
	idRenderWorldLocal *world = tr.viewDef->renderWorld;
	if ( world->mapName.Length() == 0 || world->NumAreas() <= 0 ) {
		return NULL;
	}
	return world;
}

// Gather the loaded map's opaque shadow-casting worldspawn triangles (the _areaN models)
// into a packed float3/int soup, indices area-local so each slice feeds CreateBlas directly.
// Returns false with nothing allocated when there is no opaque world geometry; otherwise
// the caller Mem_Free16's pos/idx/slices.
static bool R_RtGatherWorld( const idRenderWorldLocal *world, float *&pos, int *&idx,
		rtAreaSlice_t *&slices, int &numSlices, int &worldVerts, int &worldIndexes ) {
	pos = NULL; idx = NULL; slices = NULL;
	numSlices = 0; worldVerts = 0; worldIndexes = 0;
	const int numAreas = world->NumAreas();
	for ( int a = 0; a < numAreas; a++ ) {
		const idRenderModel *model = renderModelManager->FindModel( va( "_area%i", a ) );
		for ( int s = 0; model && s < model->NumSurfaces(); s++ ) {
			if ( !R_RtSurfCasts( model->Surface( s ) ) ) {
				continue;
			}
			worldVerts += model->Surface( s )->geometry->numVerts;
			worldIndexes += model->Surface( s )->geometry->numIndexes;
		}
	}
	if ( worldIndexes < 3 ) {
		return false;
	}
	pos = (float *)Mem_Alloc16( worldVerts * 3 * (int)sizeof( float ) );
	idx = (int *)Mem_Alloc16( worldIndexes * (int)sizeof( int ) );
	slices = (rtAreaSlice_t *)Mem_Alloc16( numAreas * (int)sizeof( rtAreaSlice_t ) );
	int vertBase = 0, idxBase = 0;
	for ( int a = 0; a < numAreas; a++ ) {
		const idRenderModel *model = renderModelManager->FindModel( va( "_area%i", a ) );
		rtAreaSlice_t sl;
		sl.vertStart = vertBase; sl.idxStart = idxBase; sl.numVerts = 0; sl.numIdx = 0;
		for ( int s = 0; model && s < model->NumSurfaces(); s++ ) {
			const modelSurface_t *surf = model->Surface( s );
			if ( !R_RtSurfCasts( surf ) ) {
				continue;
			}
			const srfTriangles_t *tri = surf->geometry;
			for ( int k = 0; k < tri->numVerts; k++ ) {
				const idVec3 &p = tri->verts[k].xyz;
				float *dst = pos + ( sl.vertStart + sl.numVerts + k ) * 3;
				dst[0] = p[0]; dst[1] = p[1]; dst[2] = p[2];
			}
			for ( int n = 0; n < tri->numIndexes; n++ ) {
				idx[sl.idxStart + sl.numIdx + n] = sl.numVerts + tri->indexes[n];
			}
			sl.numVerts += tri->numVerts;
			sl.numIdx += tri->numIndexes;
		}
		if ( sl.numIdx < 3 ) {
			continue;
		}
		vertBase += sl.numVerts;
		idxBase += sl.numIdx;
		slices[numSlices++] = sl;
	}
	return numSlices > 0;
}

// Gather the STATIC ENTITY casters: every entityDef with a resident DM_STATIC model
// (func_statics, inline brush models - doors/movers snapshot at their current pose) and
// noShadow off. Unique models become soup slices (model space); every qualifying entity
// becomes an instance carrying its model matrix. All outputs may legitimately be empty
// (numSlices/numInsts 0, pointers NULL) - a map can have no static entity casters.
// Caller Mem_Free16's pos/idx/slices/insts when non-NULL.
static void R_RtGatherEntities( const idRenderWorldLocal *world, float *&pos, int *&idx,
		rtModelSlice_t *&slices, int &numSlices, rtModelInst_t *&insts, int &numInsts ) {
	pos = NULL; idx = NULL; slices = NULL; insts = NULL;
	numSlices = 0; numInsts = 0;

	// pass 1: qualifying entities, unique models, soup sizes
	int entCount = 0, uniqueCount = 0, mVerts = 0, mIndexes = 0;
	const int maxUnique = world->entityDefs.Num();
	const idRenderModel **unique = (const idRenderModel **)Mem_Alloc16( ( maxUnique > 0 ? maxUnique : 1 ) * (int)sizeof( void * ) );
	for ( int i = 0; i < world->entityDefs.Num(); i++ ) {
		const idRenderEntityLocal *def = world->entityDefs[i];
		if ( def == NULL || def->parms.hModel == NULL || def->parms.noShadow ) {
			continue;
		}
		const idRenderModel *model = def->parms.hModel;
		if ( model->IsStaticWorldModel() || model->IsDynamicModel() != DM_STATIC ) {
			continue;						// _areaN already gathered; animated/generated models later
		}
		int surfIdx = 0;
		for ( int s = 0; s < model->NumSurfaces(); s++ ) {
			if ( R_RtSurfCasts( model->Surface( s ) ) ) {
				surfIdx += model->Surface( s )->geometry->numIndexes;
			}
		}
		if ( surfIdx < 3 ) {
			continue;
		}
		entCount++;
		bool seen = false;
		for ( int u = 0; u < uniqueCount; u++ ) {
			if ( unique[u] == model ) { seen = true; break; }
		}
		if ( !seen ) {
			unique[uniqueCount++] = model;
			for ( int s = 0; s < model->NumSurfaces(); s++ ) {
				if ( R_RtSurfCasts( model->Surface( s ) ) ) {
					mVerts += model->Surface( s )->geometry->numVerts;
					mIndexes += model->Surface( s )->geometry->numIndexes;
				}
			}
		}
	}
	if ( entCount == 0 || mIndexes < 3 ) {
		Mem_Free16( (void *)unique );
		return;
	}

	// pass 2: fill slices (one per unique model) + instances
	pos = (float *)Mem_Alloc16( mVerts * 3 * (int)sizeof( float ) );
	idx = (int *)Mem_Alloc16( mIndexes * (int)sizeof( int ) );
	slices = (rtModelSlice_t *)Mem_Alloc16( uniqueCount * (int)sizeof( rtModelSlice_t ) );
	insts = (rtModelInst_t *)Mem_Alloc16( entCount * (int)sizeof( rtModelInst_t ) );
	int vertBase = 0, idxBase = 0;
	for ( int u = 0; u < uniqueCount; u++ ) {
		const idRenderModel *model = unique[u];
		rtModelSlice_t sl;
		sl.model = model;
		sl.vertStart = vertBase; sl.idxStart = idxBase; sl.numVerts = 0; sl.numIdx = 0;
		sl.blas = 0;
		for ( int s = 0; s < model->NumSurfaces(); s++ ) {
			const modelSurface_t *surf = model->Surface( s );
			if ( !R_RtSurfCasts( surf ) ) {
				continue;
			}
			const srfTriangles_t *tri = surf->geometry;
			for ( int k = 0; k < tri->numVerts; k++ ) {
				const idVec3 &p = tri->verts[k].xyz;
				float *dst = pos + ( sl.vertStart + sl.numVerts + k ) * 3;
				dst[0] = p[0]; dst[1] = p[1]; dst[2] = p[2];
			}
			for ( int n = 0; n < tri->numIndexes; n++ ) {
				idx[sl.idxStart + sl.numIdx + n] = sl.numVerts + tri->indexes[n];
			}
			sl.numVerts += tri->numVerts;
			sl.numIdx += tri->numIndexes;
		}
		vertBase += sl.numVerts;
		idxBase += sl.numIdx;
		slices[numSlices++] = sl;
	}
	for ( int i = 0; i < world->entityDefs.Num(); i++ ) {
		const idRenderEntityLocal *def = world->entityDefs[i];
		if ( def == NULL || def->parms.hModel == NULL || def->parms.noShadow ) {
			continue;
		}
		const idRenderModel *model = def->parms.hModel;
		if ( model->IsStaticWorldModel() || model->IsDynamicModel() != DM_STATIC ) {
			continue;
		}
		int slice = -1;
		for ( int u = 0; u < numSlices; u++ ) {
			if ( slices[u].model == model ) { slice = u; break; }
		}
		if ( slice < 0 || slices[slice].numIdx < 3 ) {
			continue;
		}
		rtModelInst_t &in = insts[numInsts++];
		in.slice = slice;
		R_AxisToModelMatrix( def->parms.axis, def->parms.origin, in.mat );
	}
	Mem_Free16( (void *)unique );
}

// 3x4 inverse of an id column-major model matrix (local = inv * world). Full 3x3 inverse,
// so scaled/sheared entity axes stay exact; false on a degenerate matrix.
static bool R_RtInvertModelMatrix( const float *mm, float inv[12] ) {
	const float a = mm[0], b = mm[4], c = mm[8];
	const float d = mm[1], e = mm[5], f = mm[9];
	const float g = mm[2], h = mm[6], i = mm[10];
	const float A = e * i - f * h, B = f * g - d * i, C = d * h - e * g;
	const float det = a * A + b * B + c * C;
	if ( det > -1e-12f && det < 1e-12f ) {
		return false;
	}
	const float s = 1.0f / det;
	inv[0] = A * s;               inv[1] = ( c * h - b * i ) * s; inv[2]  = ( b * f - c * e ) * s;
	inv[4] = B * s;               inv[5] = ( a * i - c * g ) * s; inv[6]  = ( c * d - a * f ) * s;
	inv[8] = C * s;               inv[9] = ( b * g - a * h ) * s; inv[10] = ( a * e - b * d ) * s;
	const float tx = mm[12], ty = mm[13], tz = mm[14];
	inv[3]  = -( inv[0] * tx + inv[1] * ty + inv[2]  * tz );
	inv[7]  = -( inv[4] * tx + inv[5] * ty + inv[6]  * tz );
	inv[11] = -( inv[8] * tx + inv[9] * ty + inv[10] * tz );
	return true;
}

// Build every BLAS (areas as identity instances, one per unique entity model) and the TLAS.
// Returns the TLAS address (0 = failure); *blasFail counts failed BLAS builds. When
// registerForRefresh is set (the persistent build), the handles land in the refresh
// registry so R_RtRefreshInstances can re-instance them per frame; the validator's
// transient build must NOT touch the registry.
static unsigned long long R_RtBuildScene( rhi::RHI *r,
		const float *aPos, const int *aIdx, const rtAreaSlice_t *aSlices, int numASlices,
		const float *mPos, const int *mIdx, rtModelSlice_t *mSlices, int numMSlices,
		const rtModelInst_t *mInsts, int numMInsts, int *blasFail, bool registerForRefresh,
		bool areasPreBuilt ) {
	if ( registerForRefresh ) {
		if ( !areasPreBuilt ) { s_rtAreaBlas.Clear(); }	// RR5: pre-built world areas already filled s_rtAreaBlas
		s_rtModelBlas.Clear();
	}
	const int maxInst = ( areasPreBuilt ? s_rtAreaBlas.Num() : numASlices ) + numMInsts;
	rhi::RHI::RtInstance *inst = (rhi::RHI::RtInstance *)Mem_Alloc16( ( maxInst > 0 ? maxInst : 1 ) * (int)sizeof( rhi::RHI::RtInstance ) );
	int numInst = 0;
	*blasFail = 0;
	// RR5: the world areas are either pre-built per-surface (persistent scene — s_rtAreaBlas already filled by
	// R_RtBuildWorldAreaBlas, just instance them identity) or built here positions-only from the CPU soup (the
	// r_rtWorldTest validator path).
	const int nAreas = areasPreBuilt ? s_rtAreaBlas.Num() : numASlices;
	for ( int s = 0; s < nAreas; s++ ) {
		rhi::BlasHandle blas;
		if ( areasPreBuilt ) {
			blas = s_rtAreaBlas[s];
			if ( blas == 0 ) { continue; }
		} else {
			blas = r->CreateBlas( aPos + aSlices[s].vertStart * 3, aSlices[s].numVerts,
				3 * (int)sizeof( float ), aIdx + aSlices[s].idxStart, aSlices[s].numIdx );
			if ( blas == 0 ) {
				( *blasFail )++;
				continue;
			}
			if ( registerForRefresh ) {
				s_rtAreaBlas.Append( blas );
			}
		}
		rhi::RHI::RtInstance &in = inst[numInst++];
		memset( &in, 0, sizeof( in ) );
		in.transform[0] = 1.0f; in.transform[5] = 1.0f; in.transform[10] = 1.0f;	// identity 3x4
		in.blas = blas;
		in.mask = 0xFF;
	}
	for ( int s = 0; s < numMSlices; s++ ) {
		mSlices[s].blas = r->CreateBlas( mPos + mSlices[s].vertStart * 3, mSlices[s].numVerts,
			3 * (int)sizeof( float ), mIdx + mSlices[s].idxStart, mSlices[s].numIdx );
		if ( mSlices[s].blas == 0 ) {
			( *blasFail )++;
		} else if ( registerForRefresh ) {
			rtModelBlas_t mb;
			mb.model = mSlices[s].model;
			mb.blas = mSlices[s].blas;
			s_rtModelBlas.Append( mb );
		}
	}
	for ( int i = 0; i < numMInsts; i++ ) {
		const rtModelInst_t &mi = mInsts[i];
		if ( mSlices[mi.slice].blas == 0 ) {
			continue;
		}
		rhi::RHI::RtInstance &in = inst[numInst++];
		memset( &in, 0, sizeof( in ) );
		// id column-major mm -> row-major 3x4 (VkTransformMatrixKHR): row r = (mm[r], mm[r+4], mm[r+8], mm[r+12])
		for ( int row = 0; row < 3; row++ ) {
			in.transform[row * 4 + 0] = mi.mat[row + 0];
			in.transform[row * 4 + 1] = mi.mat[row + 4];
			in.transform[row * 4 + 2] = mi.mat[row + 8];
			in.transform[row * 4 + 3] = mi.mat[row + 12];
		}
		in.blas = mSlices[mi.slice].blas;
		in.mask = 0xFF;
	}
	unsigned long long tlasAddr = ( numInst > 0 ) ? r->BuildTlas( inst, numInst ) : 0;
	Mem_Free16( inst );
	return tlasAddr;
}

// Gather this frame's VIEW animated characters (monsters) into ONE combined WORLD-space
// triangle soup for the per-frame dynamic BLAS (R3 animated casters). Walks the view-entity
// chain (tr.viewDef->viewEntitys) - every entity affecting this view, INCLUDING off-screen
// ones casting shadows in - so a visible monster always qualifies whether or not its pose was
// re-instantiated THIS exact frame. (The old dynamicModelFrameCount == tr.frameCount gate
// flickered: a held/idle pose is not regenerated, so a stationary monster dropped out on the
// frames it wasn't re-posed - the sabaoth blinked on and off.) vEnt->modelMatrix is the exact
// model->world render transform; the CPU-skinned verts (always posed - never stripped, they ARE
// the game's hit surface) are baked to world here, so the backend builds a single
// identity-instance BLAS. Returns false with nothing allocated when there are no monster casters;
// else the caller Mem_Free16's pos/idx.
static bool R_RtGatherMonstersWorld( float *&pos, int *&idx, int &numVerts, int &numIndexes ) {
	pos = NULL; idx = NULL; numVerts = 0; numIndexes = 0;
	if ( tr.viewDef == NULL ) {
		return false;
	}

	for ( const viewEntity_t *vEnt = tr.viewDef->viewEntitys; vEnt; vEnt = vEnt->next ) {
		const idRenderEntityLocal *def = vEnt->entityDef;
		if ( def == NULL || def->parms.hModel == NULL || def->parms.noShadow || def->parms.weaponDepthHack
			|| def->parms.hModel->IsDynamicModel() != DM_CACHED || def->dynamicModel == NULL ) {
			continue;
		}
		const idRenderModel *dm = def->dynamicModel;
		for ( int s = 0; s < dm->NumSurfaces(); s++ ) {
			// R3.5 S4: surfaces the animated per-entity BLAS owns are cast that way, not via this soup
			if ( R_RtSurfCasts( dm->Surface( s ) ) && !R_RtSurfViaAnimBlas( dm->Surface( s ) ) ) {
				numVerts += dm->Surface( s )->geometry->numVerts;
				numIndexes += dm->Surface( s )->geometry->numIndexes;
			}
		}
	}
	if ( numIndexes < 3 ) {
		numVerts = numIndexes = 0;
		return false;
	}

	pos = (float *)Mem_Alloc16( numVerts * 3 * (int)sizeof( float ) );
	idx = (int *)Mem_Alloc16( numIndexes * (int)sizeof( int ) );
	int vbase = 0, ibase = 0;
	for ( const viewEntity_t *vEnt = tr.viewDef->viewEntitys; vEnt; vEnt = vEnt->next ) {
		const idRenderEntityLocal *def = vEnt->entityDef;
		if ( def == NULL || def->parms.hModel == NULL || def->parms.noShadow || def->parms.weaponDepthHack
			|| def->parms.hModel->IsDynamicModel() != DM_CACHED || def->dynamicModel == NULL ) {
			continue;
		}
		const float *mm = vEnt->modelMatrix;		// model->world (local coords to global coords), id column-major
		const idRenderModel *dm = def->dynamicModel;
		for ( int s = 0; s < dm->NumSurfaces(); s++ ) {
			const modelSurface_t *surf = dm->Surface( s );
			if ( !R_RtSurfCasts( surf ) || R_RtSurfViaAnimBlas( surf ) ) {
				continue;
			}
			const srfTriangles_t *tri = surf->geometry;
			for ( int k = 0; k < tri->numVerts; k++ ) {
				const idVec3 &v = tri->verts[k].xyz;
				float *dst = pos + ( vbase + k ) * 3;
				dst[0] = mm[0] * v.x + mm[4] * v.y + mm[8]  * v.z + mm[12];
				dst[1] = mm[1] * v.x + mm[5] * v.y + mm[9]  * v.z + mm[13];
				dst[2] = mm[2] * v.x + mm[6] * v.y + mm[10] * v.z + mm[14];
			}
			for ( int m = 0; m < tri->numIndexes; m++ ) {
				idx[ibase + m] = vbase + tri->indexes[m];
			}
			vbase += tri->numVerts;
			ibase += tri->numIndexes;
		}
	}
	return true;
}

// Re-instance the TLAS with CURRENT entity transforms once per game frame (movers: doors,
// lifts, crushers). Uses the BLAS registry the persistent build filled - geometry never
// rebuilds, only the instance list; the backend records the TLAS build on the next frame's
// command buffer (RHI::UpdateTlas), so this never stalls. Entities spawned after the scene
// build have no BLAS yet and are skipped until the next rebuild. Animated characters are added
// separately via the per-frame dynamic BLAS (R_RtGatherMonstersWorld + UpdateDynamicGeometry).
// TLAS dirty state (r_rtTlasDirty): the signature of the instance set last actually built,
// so an idle frame (no mover moved, no animated caster) can skip the rebuild. File scope so
// R_RtDirtyReset can clear it on a scene (re)build - a fresh map must never inherit a stale
// signature and wrongly skip its first build.
static unsigned long long	s_rtLastTlasSig = 0;
static bool					s_rtTlasEverBuilt = false;
static bool					s_rtLastHadDyn = false;
static void R_RtDirtyReset( void ) {
	s_rtTlasEverBuilt = false;
	s_rtLastHadDyn = false;
	s_rtLastTlasSig = 0;
}
static unsigned long long R_RtHashBytes( unsigned long long h, const void *data, size_t n ) {
	const unsigned char *p = (const unsigned char *)data;
	for ( size_t i = 0; i < n; i++ ) {
		h ^= p[i];
		h *= 1099511628211ULL;			// FNV-1a prime
	}
	return h;
}

static void R_RtRefreshInstances( const idRenderWorldLocal *world, rhi::RHI *r ) {
	static int lastFrame = -1;
	if ( tr.frameCount == lastFrame ) {
		return;						// subviews re-enter R_RenderView; once per frame is enough
	}
	lastFrame = tr.frameCount;
	if ( s_rtAreaBlas.Num() == 0 && s_rtModelBlas.Num() == 0 ) {
		return;
	}
	const int maxInst = s_rtAreaBlas.Num() + world->entityDefs.Num();
	rhi::RHI::RtInstance *inst = (rhi::RHI::RtInstance *)Mem_Alloc16( ( maxInst > 0 ? maxInst : 1 ) * (int)sizeof( rhi::RHI::RtInstance ) );
	int n = 0;
	for ( int i = 0; i < s_rtAreaBlas.Num(); i++ ) {
		rhi::RHI::RtInstance &in = inst[n++];
		memset( &in, 0, sizeof( in ) );
		in.transform[0] = 1.0f; in.transform[5] = 1.0f; in.transform[10] = 1.0f;	// identity 3x4
		in.blas = s_rtAreaBlas[i];
		in.mask = 0xFF;
	}
	for ( int i = 0; i < world->entityDefs.Num(); i++ ) {
		const idRenderEntityLocal *def = world->entityDefs[i];
		if ( def == NULL || def->parms.hModel == NULL || def->parms.noShadow ) {
			continue;
		}
		const idRenderModel *model = def->parms.hModel;
		if ( model->IsStaticWorldModel() || model->IsDynamicModel() != DM_STATIC ) {
			continue;
		}
		rhi::BlasHandle blas = 0;
		for ( int u = 0; u < s_rtModelBlas.Num(); u++ ) {
			if ( s_rtModelBlas[u].model == model ) {
				blas = s_rtModelBlas[u].blas;
				break;
			}
		}
		if ( blas == 0 ) {
			continue;
		}
		float mm[16];
		R_AxisToModelMatrix( def->parms.axis, def->parms.origin, mm );
		rhi::RHI::RtInstance &in = inst[n++];
		memset( &in, 0, sizeof( in ) );
		for ( int row = 0; row < 3; row++ ) {
			in.transform[row * 4 + 0] = mm[row + 0];
			in.transform[row * 4 + 1] = mm[row + 4];
			in.transform[row * 4 + 2] = mm[row + 8];
			in.transform[row * 4 + 3] = mm[row + 12];
		}
		in.blas = blas;
		in.mask = 0xFF;
	}
	if ( n > 0 ) {
		// Signature of the instance set we're about to consider: count + every instance's
		// 3x4 transform + its BLAS handle. A still scene reproduces this bit-for-bit frame to
		// frame (R_AxisToModelMatrix of a constant pose is deterministic), so an unchanged
		// signature means no mover moved.
		unsigned long long sig = 1469598103934665603ULL;	// FNV-1a basis
		sig = R_RtHashBytes( sig, &n, sizeof( n ) );
		for ( int i = 0; i < n; i++ ) {
			sig = R_RtHashBytes( sig, inst[i].transform, sizeof( inst[i].transform ) );
			sig = R_RtHashBytes( sig, &inst[i].blas, sizeof( inst[i].blas ) );
		}

		// R3 animated casters: gather this frame's visible monsters into a world-space soup and
		// hand it to the backend (rebuilds a per-frame dynamic BLAS + appends one identity
		// instance to the TLAS). MUST precede UpdateTlas, which consumes the dyn-caster arm.
		// Also the dirty signal: an animated caster changes its geometry every frame, so its
		// presence forces the rebuild (dyn == true never skips).
		bool dyn = false;
		if ( ( r_rtSunShadows.GetBool() || r_rtMovingLights.GetBool() || r_rtao.GetBool() ) && r_rtMonsterShadows.GetBool() ) {
			float *mpos; int *midx; int mnv = 0, mni = 0;
			if ( R_RtGatherMonstersWorld( mpos, midx, mnv, mni ) ) {
				r->UpdateDynamicGeometry( mpos, mnv, midx, mni );
				Mem_Free16( mpos );
				Mem_Free16( midx );
				dyn = true;
			}
		}

		// Dirty decision (r_rtTlasDirty): rebuild only when something changed. dyn forces it
		// (animated geometry); s_rtLastHadDyn forces one more rebuild the frame AFTER monsters
		// leave (to drop their instance from the TLAS); a changed static signature is a mover
		// that moved. On a clean idle frame we skip UpdateTlas entirely - the backend's
		// rtCurrentAddr still points at the last-built slot, which holds exactly these poses
		// and references only persistent static BLASes, so the fragment rays read valid data.
		const bool dirty = !s_rtTlasEverBuilt || dyn || s_rtLastHadDyn || sig != s_rtLastTlasSig;
		static int accBuilt = 0, accSkipped = 0, accStartMs = 0;
		if ( !r_rtTlasDirty.GetBool() || dirty ) {
			r->UpdateTlas( inst, n );
			s_rtLastTlasSig = sig;
			s_rtLastHadDyn = dyn;
			s_rtTlasEverBuilt = true;
			accBuilt++;
		} else {
			accSkipped++;
		}
		// once/sec readout, shared with the cube-cache debug the shadow work already uses
		if ( r_shadowMapCacheDebug.GetBool() ) {
			const int nowMs = Sys_Milliseconds();
			if ( accStartMs == 0 ) {
				accStartMs = nowMs;
			} else if ( nowMs - accStartMs >= 1000 ) {
				common->Printf( "rtTlas/s: built %d, skipped %d (dirty-flag %s)\n",
				                accBuilt, accSkipped, r_rtTlasDirty.GetBool() ? "on" : "OFF" );
				accBuilt = accSkipped = 0;
				accStartMs = nowMs;
			}
		}
	}
	Mem_Free16( inst );
}

// RR3: pack a surface material's diffuse average colour (idImage::averageColor, already computed per
// texture) into RGBA8 for the geometry table, so a reflected monster hit shades with roughly its skin
// colour instead of flat grey. Mid-grey fallback when the material has no diffuse image. Byte order
// matches the shader's unpackUnorm4x8 (R at bits 0-7, G 8-15, B 16-23, A 24-31).
static unsigned int R_RtMaterialBaseColor( const idMaterial *mat ) {
	float rgb[3] = { 0.55f, 0.55f, 0.55f };
	if ( mat != NULL ) {
		const int n = mat->GetNumStages();
		for ( int i = 0; i < n; i++ ) {
			const shaderStage_t *st = mat->GetStage( i );
			if ( st->lighting == SL_DIFFUSE && st->texture.image != NULL ) {
				rgb[0] = st->texture.image->averageColor[0];
				rgb[1] = st->texture.image->averageColor[1];
				rgb[2] = st->texture.image->averageColor[2];
				break;
			}
		}
	}
	unsigned int c = 0xFF000000u;	// A = 255
	for ( int k = 0; k < 3; k++ ) {
		const int v = (int)( idMath::ClampFloat( 0.0f, 1.0f, rgb[k] ) * 255.0f + 0.5f );
		c |= ( (unsigned int)v ) << ( k * 8 );
	}
	return c;
}

// RR4: the diffuse stage's texture handle (idImage::rhiHandle = the Vulkan backend's bindless slot + 1),
// so a reflected monster hit samples its real diffuse at the interpolated st. 0 = no diffuse image yet
// (not uploaded, or none) -> the reflection shader falls back to the RR3 average colour.
static unsigned int R_RtMaterialTexIndex( const idMaterial *mat ) {
	if ( mat != NULL ) {
		const int n = mat->GetNumStages();
		for ( int i = 0; i < n; i++ ) {
			const shaderStage_t *st = mat->GetStage( i );
			if ( st->lighting == SL_DIFFUSE && st->texture.image != NULL ) {
				return (unsigned int)st->texture.image->rhiHandle;
			}
		}
	}
	return 0;
}

// R3.5 S3: stage this frame's GPU-skinned shadow casters for the backend's per-entity animated BLAS
// cache. Runs once/frame regardless of the persistent-scene state (so the cache retires stale entries
// when the feature or the view goes away). The backend copies the descriptors and builds/refits each
// caster's BLAS from its gpuSkinVB after the skin flush (RefreshAnimBlas). Inert until S4 puts these
// BLASes into the TLAS; for now it is a lifecycle soak (watch r_shadowMapCacheDebug).
static void R_RtStageAnimCasters( void ) {
	rhi::RHI *r = rhi::GetRHI();
	if ( r == NULL || !r->SupportsRayQuery() ) {
		return;
	}
	static int lastFrame = -1;
	if ( tr.frameCount == lastFrame ) {
		return;						// subviews re-enter R_RenderView; once per frame is enough
	}
	lastFrame = tr.frameCount;

	// once/sec lifecycle readout, shared with the cube-cache + rtTlas debug
	if ( r_shadowMapCacheDebug.GetBool() ) {
		static int accStartMs = 0, prevB = 0, prevR = 0, prevRet = 0;
		int b, rf, ret, live;
		r->AnimBlasStats( b, rf, ret, live );
		const int nowMs = Sys_Milliseconds();
		if ( accStartMs == 0 ) {
			accStartMs = nowMs;
		} else if ( nowMs - accStartMs >= 1000 ) {
			int geoRows = 0, geoMon = 0;
			r->RtReflStats( geoRows, geoMon );		// RR0: per-instance geometry table population
			common->Printf( "rtAnimBlas/s: %d builds, %d refits, %d retires; %d live | geoTable %d rows (%d monster)\n",
			                b - prevB, rf - prevR, ret - prevRet, live, geoRows, geoMon );
			prevB = b; prevR = rf; prevRet = ret; accStartMs = nowMs;
		}
	}

	if ( !r_rtAnimBlas.GetBool() || R_RtSceneWorld() == NULL || tr.viewDef == NULL ) {
		r->UpdateAnimCasters( NULL, 0 );	// off / not a world view: clear staged, let the cache retire
		return;
	}

	// pass 1: count eligible casters + their GPU-skinned casting surfaces
	int nCaster = 0, nGeom = 0;
	for ( const viewEntity_t *vEnt = tr.viewDef->viewEntitys; vEnt; vEnt = vEnt->next ) {
		const idRenderEntityLocal *def = vEnt->entityDef;
		if ( def == NULL || def->parms.hModel == NULL || def->parms.noShadow || def->parms.weaponDepthHack
			|| def->parms.hModel->IsDynamicModel() != DM_CACHED || def->dynamicModel == NULL ) {
			continue;
		}
		const idRenderModel *dm = def->dynamicModel;
		int surfCount = 0;
		for ( int s = 0; s < dm->NumSurfaces(); s++ ) {
			if ( R_RtSurfCasts( dm->Surface( s ) ) && R_RtSurfViaAnimBlas( dm->Surface( s ) ) ) {
				surfCount++;
			}
		}
		if ( surfCount > 0 ) {
			nCaster++;
			nGeom += surfCount;
		}
	}
	if ( nCaster == 0 ) {
		r->UpdateAnimCasters( NULL, 0 );
		return;
	}

	rhi::RHI::AnimCaster *casters = (rhi::RHI::AnimCaster *)Mem_Alloc16( nCaster * (int)sizeof( rhi::RHI::AnimCaster ) );
	rhi::RHI::BlasGeometry *geoms = (rhi::RHI::BlasGeometry *)Mem_Alloc16( nGeom * (int)sizeof( rhi::RHI::BlasGeometry ) );
	int ci = 0, gi = 0;
	for ( const viewEntity_t *vEnt = tr.viewDef->viewEntitys; vEnt; vEnt = vEnt->next ) {
		const idRenderEntityLocal *def = vEnt->entityDef;
		if ( def == NULL || def->parms.hModel == NULL || def->parms.noShadow || def->parms.weaponDepthHack
			|| def->parms.hModel->IsDynamicModel() != DM_CACHED || def->dynamicModel == NULL ) {
			continue;
		}
		const idRenderModel *dm = def->dynamicModel;
		const int gStart = gi;
		// topology signature: model + each surface's gpuSkinVB handle + counts. A change (model swap /
		// LOD / gpuSkinVB realloc) forces a rebuild instead of refitting a mismatched AS.
		unsigned long long sig = 1469598103934665603ULL;			// FNV-1a basis
		const void *modelPtr = dm;
		sig = R_RtHashBytes( sig, &modelPtr, sizeof( modelPtr ) );
		for ( int s = 0; s < dm->NumSurfaces() && ( gi - gStart ) < 64; s++ ) {
			const srfTriangles_t *tri = dm->Surface( s )->geometry;
			if ( !R_RtSurfCasts( dm->Surface( s ) ) || !R_RtSurfViaAnimBlas( dm->Surface( s ) ) ) {
				continue;
			}
			const unsigned long long va = r->GetBufferDeviceAddress( tri->gpuSkinVB );
			const unsigned long long ia = r->GetBufferDeviceAddress( (rhi::BufferHandle)tri->indexCache->vbo );
			if ( va == 0 || ia == 0 ) {
				continue;
			}
			geoms[gi].vertexAddress = va;
			geoms[gi].vertexStride = (unsigned int)sizeof( idDrawVert );
			geoms[gi].vertexCount = (unsigned int)tri->numVerts;
			geoms[gi].indexAddress = ia;
			geoms[gi].indexCount = (unsigned int)tri->numIndexes;
			geoms[gi].baseColor = R_RtMaterialBaseColor( dm->Surface( s )->shader );	// RR3: skin colour
			geoms[gi].texIndex = R_RtMaterialTexIndex( dm->Surface( s )->shader );	// RR4: diffuse bindless slot

			sig = R_RtHashBytes( sig, &tri->gpuSkinVB, sizeof( tri->gpuSkinVB ) );
			sig = R_RtHashBytes( sig, &tri->numVerts, sizeof( tri->numVerts ) );
			sig = R_RtHashBytes( sig, &tri->numIndexes, sizeof( tri->numIndexes ) );
			gi++;
		}
		const int gCount = gi - gStart;
		if ( gCount == 0 ) {
			continue;					// no device-addressable GPU-skinned surface after all
		}
		rhi::RHI::AnimCaster &c = casters[ci++];
		c.key = (unsigned int)( def->index + 1 );	// +1: key 0 marks a free cache slot
		c.topoSig = sig;
		c.geoms = geoms + gStart;
		c.geomCount = gCount;
		const float *mm = vEnt->modelMatrix;		// model->world, id column-major
		for ( int row = 0; row < 3; row++ ) {
			c.transform[row * 4 + 0] = mm[row + 0];
			c.transform[row * 4 + 1] = mm[row + 4];
			c.transform[row * 4 + 2] = mm[row + 8];
			c.transform[row * 4 + 3] = mm[row + 12];
		}
		c.mask = 0xFF;
	}
	r->UpdateAnimCasters( casters, ci );
	Mem_Free16( casters );
	Mem_Free16( geoms );
}

/*
=================
R_RtWorldUpdate

Keeps the persistent world scene in sync with r_rtWorld / r_rtSunShadows: lazy build on the
first primary view, rebuild on map change (mapName comparison), teardown when both are off.
While the scene is live, every frame re-instances the TLAS with current mover poses.
=================
*/
// RR5: free the dedicated world geometry buffers (called on scene teardown / before a rebuild; the device
// is idle here — R_RtWorldUpdate always DestroyRtScene's first). No-op when never allocated.
static void R_RtFreeWorldGeo( void ) {
	rhi::RHI *r = rhi::GetRHI();
	if ( r == NULL ) { s_rtWorldVB = 0; s_rtWorldIB = 0; return; }
	if ( s_rtWorldVB != 0 ) { r->DestroyBuffer( s_rtWorldVB ); s_rtWorldVB = 0; }
	if ( s_rtWorldIB != 0 ) { r->DestroyBuffer( s_rtWorldIB ); s_rtWorldIB = 0; }
}

// RR5: build the static worldspawn (_areaN) as PER-SURFACE multi-geometry BLASes fed from device-
// addressable buffers, so a reflection ray resolving table[customIndex+geometryIndex] can fetch the hit
// surface's st + material texIndex (unlike the positions-only shadow BLAS). Gathers full idDrawVert from
// tri->verts (always resident — covers non-visible areas the ambient cache never populated) into one
// dedicated vertex + one index buffer, then chunks surfaces into <=MAX_BLAS_GEOMS-geometry BLASes. Fills
// s_rtAreaBlas; the caller instances them (identity). Returns false (nothing built) on empty/failure.
static bool R_RtBuildWorldAreaBlas( rhi::RHI *r, const idRenderWorldLocal *world, int &numAreaBlas, int &worldTris ) {
	numAreaBlas = 0; worldTris = 0;
	R_RtFreeWorldGeo();
	s_rtAreaBlas.Clear();
	const int numAreas = world->NumAreas();
	int numSurfs = 0, totVerts = 0, totIdx = 0;
	for ( int a = 0; a < numAreas; a++ ) {
		const idRenderModel *model = renderModelManager->FindModel( va( "_area%i", a ) );
		for ( int s = 0; model && s < model->NumSurfaces(); s++ ) {
			if ( !R_RtSurfCasts( model->Surface( s ) ) ) { continue; }
			numSurfs++;
			totVerts += model->Surface( s )->geometry->numVerts;
			totIdx += model->Surface( s )->geometry->numIndexes;
		}
	}
	if ( numSurfs == 0 || totIdx < 3 ) {
		return false;
	}
	idDrawVert *verts = (idDrawVert *)Mem_Alloc16( totVerts * (int)sizeof( idDrawVert ) );
	unsigned int *idxs = (unsigned int *)Mem_Alloc16( totIdx * (int)sizeof( unsigned int ) );
	struct SurfDesc { int vOff, nV, iOff, nI; unsigned int tex, col; };
	SurfDesc *sd = (SurfDesc *)Mem_Alloc16( numSurfs * (int)sizeof( SurfDesc ) );
	int si = 0, vBase = 0, iBase = 0;
	for ( int a = 0; a < numAreas; a++ ) {
		const idRenderModel *model = renderModelManager->FindModel( va( "_area%i", a ) );
		for ( int s = 0; model && s < model->NumSurfaces(); s++ ) {
			const modelSurface_t *surf = model->Surface( s );
			if ( !R_RtSurfCasts( surf ) ) { continue; }
			const srfTriangles_t *tri = surf->geometry;
			memcpy( verts + vBase, tri->verts, tri->numVerts * (int)sizeof( idDrawVert ) );
			for ( int m = 0; m < tri->numIndexes; m++ ) { idxs[iBase + m] = (unsigned int)tri->indexes[m]; }	// surface-local
			sd[si].vOff = vBase; sd[si].nV = tri->numVerts;
			sd[si].iOff = iBase; sd[si].nI = tri->numIndexes;
			sd[si].tex = R_RtMaterialTexIndex( surf->shader );
			sd[si].col = R_RtMaterialBaseColor( surf->shader );
			si++; vBase += tri->numVerts; iBase += tri->numIndexes;
		}
	}
	worldTris = totIdx / 3;
	s_rtWorldVB = r->CreateBuffer( rhi::BU_VERTEX, totVerts * (int)sizeof( idDrawVert ), verts );
	s_rtWorldIB = r->CreateBuffer( rhi::BU_INDEX, totIdx * (int)sizeof( unsigned int ), idxs );
	Mem_Free16( verts ); Mem_Free16( idxs );
	const unsigned long long vbAddr = ( s_rtWorldVB != 0 ) ? r->GetBufferDeviceAddress( s_rtWorldVB ) : 0;
	const unsigned long long ibAddr = ( s_rtWorldIB != 0 ) ? r->GetBufferDeviceAddress( s_rtWorldIB ) : 0;
	if ( vbAddr == 0 || ibAddr == 0 ) {
		R_RtFreeWorldGeo();
		Mem_Free16( sd );
		return false;
	}
	const int MAXG = 64;			// mirrors the backend's MAX_BLAS_GEOMS
	rhi::RHI::BlasGeometry geoms[64];
	for ( int base = 0; base < numSurfs; base += MAXG ) {
		const int cnt = ( numSurfs - base < MAXG ) ? ( numSurfs - base ) : MAXG;
		for ( int g = 0; g < cnt; g++ ) {
			const SurfDesc &d = sd[base + g];
			geoms[g].vertexAddress = vbAddr + (unsigned long long)d.vOff * (unsigned long long)sizeof( idDrawVert );
			geoms[g].vertexStride = (unsigned int)sizeof( idDrawVert );
			geoms[g].vertexCount = (unsigned int)d.nV;
			geoms[g].indexAddress = ibAddr + (unsigned long long)d.iOff * (unsigned long long)sizeof( unsigned int );
			geoms[g].indexCount = (unsigned int)d.nI;
			geoms[g].baseColor = d.col;
			geoms[g].texIndex = d.tex;
		}
		rhi::BlasHandle blas = r->CreateBlasFromBuffers( geoms, cnt, false );
		if ( blas != 0 ) {
			s_rtAreaBlas.Append( blas );
			numAreaBlas++;
		}
	}
	Mem_Free16( sd );
	return numAreaBlas > 0;
}

static idStr s_rtWorldMap;
static void R_RtWorldUpdate( void ) {
	rhi::RHI *r = rhi::GetRHI();
	if ( r == NULL ) {
		return;
	}
	// r_rtSunShadows (R3) / r_rtMovingLights (Option A) / r_rtao (H4) imply the scene:
	// the consumers auto-build their prerequisite
	if ( !r_rtWorld.GetBool() && !r_rtSunShadows.GetBool() && !r_rtMovingLights.GetBool() && !r_rtao.GetBool() ) {
		if ( r_rtWorld.IsModified() || r_rtSunShadows.IsModified() || r_rtMovingLights.IsModified() || r_rtao.IsModified() ) {
			r_rtWorld.ClearModified();
			r_rtSunShadows.ClearModified();
			r_rtMovingLights.ClearModified();
			r_rtao.ClearModified();
			r->DestroyRtScene();		// switched off: free the scene (no-op when never built)
			s_rtWorldMap.Clear();
			s_rtAreaBlas.Clear();
			s_rtModelBlas.Clear();
			R_RtFreeWorldGeo();			// RR5: drop the dedicated world geometry buffers
		}
		return;
	}
	idRenderWorldLocal *world = R_RtSceneWorld();
	if ( world == NULL ) {
		return;							// GUI/subview/empty world: never bind the scene here
	}
	if ( !r->SupportsRayQuery() ) {
		return;							// stays pending; a backend/hardware change re-evaluates
	}
	if ( r->GetStaticTlasAddress() != 0 && s_rtWorldMap.Icmp( world->mapName ) == 0 ) {
		// static scene live and current: re-instance the per-frame TLAS with this frame's mover
		// poses + animated-monster casters (GetStaticTlasAddress is the persistent-scene anchor;
		// GetTlasAddress would return the per-frame slot address once the lane is running)
		R_RtRefreshInstances( world, r );
		return;
	}
	r_rtWorld.ClearModified();
	r_rtSunShadows.ClearModified();
	r_rtMovingLights.ClearModified();
	r->DestroyRtScene();

	// RR5: build the static worldspawn as PER-SURFACE multi-geometry BLASes (device buffers), so a
	// reflection ray can fetch st + material at a world hit and the RT reflection shader shades the real
	// room (docs/rtx-reflections.md). Entities (func_static / movers) still gather positions-only for now.
	// terrain-as-models maps legitimately have ZERO worldspawn surfaces — the scene still builds from the
	// entity casters alone, so neither gates the other; only both-empty means there is nothing to trace.
	int numAreaBlas = 0, worldTris = 0;
	const bool areasOk = R_RtBuildWorldAreaBlas( r, world, numAreaBlas, worldTris );
	float *mPos; int *mIdx; rtModelSlice_t *mSlices; rtModelInst_t *mInsts;
	int numMSlices, numMInsts;
	R_RtGatherEntities( world, mPos, mIdx, mSlices, numMSlices, mInsts, numMInsts );
	if ( !areasOk && numMInsts == 0 ) {
		R_RtFreeWorldGeo();
		return;
	}

	const int msStart = Sys_Milliseconds();
	int blasFail = 0;
	const unsigned long long tlasAddr = R_RtBuildScene( r, NULL, NULL, NULL, 0,
		mPos, mIdx, mSlices, numMSlices, mInsts, numMInsts, &blasFail, true, /*areasPreBuilt=*/true );
	const int msBuild = Sys_Milliseconds() - msStart;
	int mTris = 0;
	for ( int s = 0; s < numMSlices; s++ ) {
		mTris += mSlices[s].numIdx / 3;
	}
	if ( mPos ) { Mem_Free16( mPos ); Mem_Free16( mIdx ); Mem_Free16( mSlices ); Mem_Free16( mInsts ); }
	if ( tlasAddr == 0 ) {
		common->Warning( "r_rtWorld: world scene build failed (%d BLAS failures) - disabling", blasFail );
		r->DestroyRtScene();
		s_rtAreaBlas.Clear();
		s_rtModelBlas.Clear();
		R_RtFreeWorldGeo();
		r_rtWorld.SetBool( false );
		r_rtSunShadows.SetBool( false );
		return;
	}
	s_rtWorldMap = world->mapName;
	R_RtDirtyReset();		// fresh scene: force the first per-frame TLAS build, drop any stale signature
	common->Printf( "r_rtWorld: %d area BLAS (%d tris, per-surface) + %d model BLAS (%d tris, %d instances), %d ms build%s\n",
		numAreaBlas, worldTris, numMSlices, mTris, numMInsts, msBuild,
		blasFail ? va( " (%d BLAS failed)", blasFail ) : "" );
}

static const char *RTWORLD_SRC =
	"#version 460\n"
	"#extension GL_EXT_ray_query : require\n"
	"layout(local_size_x = 64) in;\n"
	"layout(std430, binding = 0) writeonly buffer Out { float t[]; } outb;\n"
	"layout(std430, binding = 1) readonly buffer Rays { vec4 dir[]; } rays;\n"
	"layout(push_constant) uniform PC { uvec2 tlas; uint count; float ox, oy, oz, tmax; } pc;\n"
	"void main() {\n"
	"    uint i = gl_GlobalInvocationID.x;\n"
	"    if ( i >= pc.count ) { return; }\n"
	"    rayQueryEXT rq;\n"
	"    rayQueryInitializeEXT( rq, accelerationStructureEXT( pc.tlas ), gl_RayFlagsOpaqueEXT, 0xFFu,\n"
	"                           vec3( pc.ox, pc.oy, pc.oz ), 0.0, rays.dir[i].xyz, pc.tmax );\n"
	"    while ( rayQueryProceedEXT( rq ) ) { }\n"
	"    outb.t[i] = ( rayQueryGetIntersectionTypeEXT( rq, true ) == gl_RayQueryCommittedIntersectionTriangleEXT )\n"
	"        ? rayQueryGetIntersectionTEXT( rq, true ) : -1.0;\n"
	"}\n";

// Moller-Trumbore closest hit of one ray against a packed float3/int soup slice; returns
// the closest t in (0, tmax) or -1. Same math as the synthetic self-test's reference.
static float R_RtCpuTrace( const idVec3 &org, const idVec3 &dir, float tmax,
		const float *pos, const int *idx, int idxStart, int idxCount, int vertStart ) {
	float best = -1.0f;
	for ( int n = idxStart; n < idxStart + idxCount; n += 3 ) {
		const float *v0 = pos + ( vertStart + idx[n + 0] ) * 3;
		const float *v1 = pos + ( vertStart + idx[n + 1] ) * 3;
		const float *v2 = pos + ( vertStart + idx[n + 2] ) * 3;
		const idVec3 e1( v1[0]-v0[0], v1[1]-v0[1], v1[2]-v0[2] );
		const idVec3 e2( v2[0]-v0[0], v2[1]-v0[1], v2[2]-v0[2] );
		const idVec3 pv = dir.Cross( e2 );
		const float det = e1 * pv;
		if ( det > -1e-8f && det < 1e-8f ) {
			continue;
		}
		const float inv = 1.0f / det;
		const idVec3 tv( org[0]-v0[0], org[1]-v0[1], org[2]-v0[2] );
		const float u = ( tv * pv ) * inv;
		if ( u < 0.0f || u > 1.0f ) {
			continue;
		}
		const idVec3 qv = tv.Cross( e1 );
		const float w = ( dir * qv ) * inv;
		if ( w < 0.0f || u + w > 1.0f ) {
			continue;
		}
		const float t = ( e2 * qv ) * inv;
		if ( t > 0.0f && t < tmax && ( best < 0.0f || t < best ) ) {
			best = t;
		}
	}
	return best;
}

// CPU closest hit over the WHOLE scene: worldspawn slices (identity) + entity instances
// (ray inverse-transformed into model space; the affine map preserves the ray parameter,
// so the model-space t IS the world-space t - the direction is deliberately NOT renormalized).
static float R_RtCpuSceneTrace( const idVec3 &org, const idVec3 &dir, float tmax,
		const float *aPos, const int *aIdx, const rtAreaSlice_t *aSlices, int numASlices,
		const float *mPos, const int *mIdx, const rtModelSlice_t *mSlices,
		const rtModelInst_t *mInsts, int numMInsts ) {
	float best = -1.0f;
	for ( int s = 0; s < numASlices; s++ ) {
		const float t = R_RtCpuTrace( org, dir, tmax,
			aPos, aIdx, aSlices[s].idxStart, aSlices[s].numIdx, aSlices[s].vertStart );
		if ( t > 0.0f && ( best < 0.0f || t < best ) ) {
			best = t;
		}
	}
	for ( int i = 0; i < numMInsts; i++ ) {
		const rtModelInst_t &mi = mInsts[i];
		float inv[12];
		if ( !R_RtInvertModelMatrix( mi.mat, inv ) ) {
			continue;
		}
		const idVec3 lorg(
			inv[0] * org[0] + inv[1] * org[1] + inv[2]  * org[2] + inv[3],
			inv[4] * org[0] + inv[5] * org[1] + inv[6]  * org[2] + inv[7],
			inv[8] * org[0] + inv[9] * org[1] + inv[10] * org[2] + inv[11] );
		const idVec3 ldir(
			inv[0] * dir[0] + inv[1] * dir[1] + inv[2]  * dir[2],
			inv[4] * dir[0] + inv[5] * dir[1] + inv[6]  * dir[2],
			inv[8] * dir[0] + inv[9] * dir[1] + inv[10] * dir[2] );
		const float t = R_RtCpuTrace( lorg, ldir, tmax,
			mPos, mIdx, mSlices[mi.slice].idxStart, mSlices[mi.slice].numIdx, mSlices[mi.slice].vertStart );
		if ( t > 0.0f && ( best < 0.0f || t < best ) ) {
			best = t;
		}
	}
	return best;
}

static void R_RtWorldValidate( void ) {
	if ( !r_rtWorldTest.GetBool() ) {
		if ( r_rtWorldTest.IsModified() ) {
			r_rtWorldTest.ClearModified();
		}
		return;
	}
	idRenderWorldLocal *world = R_RtSceneWorld();
	if ( world == NULL ) {
		return;							// stay armed until a map-world primary view renders
	}
	// consume: run once, flip the cvar back off so a re-enable re-runs
	r_rtWorldTest.SetBool( false );
	r_rtWorldTest.ClearModified();

	rhi::RHI *r = rhi::GetRHI();
	if ( r == NULL || !r->SupportsRayQuery() ) {
		common->Printf( "r_rtWorldTest: unavailable (needs Vulkan + KHR_ray_query hardware)\n" );
		return;
	}

	// mirror R_RtWorldUpdate: worldspawn-empty maps (terrain-as-models) still validate
	// against their entity casters; only both-empty means nothing to trace
	float *aPos; int *aIdx; rtAreaSlice_t *aSlices;
	int numASlices, worldVerts, worldIndexes;
	R_RtGatherWorld( world, aPos, aIdx, aSlices, numASlices, worldVerts, worldIndexes );
	float *mPos; int *mIdx; rtModelSlice_t *mSlices; rtModelInst_t *mInsts;
	int numMSlices, numMInsts;
	R_RtGatherEntities( world, mPos, mIdx, mSlices, numMSlices, mInsts, numMInsts );
	if ( numASlices == 0 && numMInsts == 0 ) {
		common->Printf( "r_rtWorldTest: no shadow-casting geometry found\n" );
		return;
	}

	// scene: reuse the persistent STATIC one when live (R_RtWorldUpdate ran just before us, so
	// it is current for this map), else a transient build torn down at the end. Deliberately the
	// STATIC scene, not the per-frame TLAS: the CPU reference below is the static soup, so tracing
	// the per-frame slot (which also carries animated monster casters) would false-positive every
	// monster hit as a "genuine" mismatch.
	const bool persistent = ( r_rtWorld.GetBool() || r_rtSunShadows.GetBool() || r_rtMovingLights.GetBool() ) && r->GetStaticTlasAddress() != 0;
	int blasFail = 0, msBuild = 0;
	unsigned long long tlasAddr;
	if ( persistent ) {
		tlasAddr = r->GetStaticTlasAddress();
	} else {
		const int msStart = Sys_Milliseconds();
		tlasAddr = R_RtBuildScene( r, aPos, aIdx, aSlices, numASlices,
			mPos, mIdx, mSlices, numMSlices, mInsts, numMInsts, &blasFail, false, /*areasPreBuilt=*/false );
		msBuild = Sys_Milliseconds() - msStart;
	}
	int mTris = 0;
	for ( int s = 0; s < numMSlices; s++ ) {
		mTris += mSlices[s].numIdx / 3;
	}
	// single cleanup path: every exit below runs this
	struct rtCleanup_t {
		float *aPos; int *aIdx; rtAreaSlice_t *aSlices;
		float *mPos; int *mIdx; rtModelSlice_t *mSlices; rtModelInst_t *mInsts;
	} cl = { aPos, aIdx, aSlices, mPos, mIdx, mSlices, mInsts };
	#define RTTEST_CLEANUP() do { \
		Mem_Free16( cl.aPos ); Mem_Free16( cl.aIdx ); Mem_Free16( cl.aSlices ); \
		if ( cl.mPos ) { Mem_Free16( cl.mPos ); Mem_Free16( cl.mIdx ); Mem_Free16( cl.mSlices ); Mem_Free16( cl.mInsts ); } \
	} while ( 0 )

	if ( tlasAddr == 0 || blasFail > 0 ) {
		common->Printf( "r_rtWorldTest: FAIL (AS build: %d BLAS failed, tlas %s)\n",
			blasFail, tlasAddr ? "ok" : "failed" );
		if ( !persistent ) {
			r->DestroyRtScene();
		}
		RTTEST_CLEANUP();
		return;
	}

	// 8x8 ray grid across the live view frustum (0.9 x the half-FOV so every ray stays in-view)
	const renderView_t &rv = tr.viewDef->renderView;
	const float tanX = idMath::Tan( DEG2RAD( rv.fov_x * 0.5f ) ) * 0.9f;
	const float tanY = idMath::Tan( DEG2RAD( rv.fov_y * 0.5f ) ) * 0.9f;
	const int NR = 64;
	const float TMAX = 100000.0f;
	idVec3 dirs[NR];
	float dirBlob[NR * 4];
	for ( int i = 0; i < NR; i++ ) {
		const float u = ( ( (float)( i % 8 ) + 0.5f ) / 8.0f * 2.0f - 1.0f ) * tanX;
		const float v = ( ( (float)( i / 8 ) + 0.5f ) / 8.0f * 2.0f - 1.0f ) * tanY;
		dirs[i] = rv.viewaxis[0] - u * rv.viewaxis[1] + v * rv.viewaxis[2];
		dirs[i].Normalize();
		dirBlob[i * 4 + 0] = dirs[i][0]; dirBlob[i * 4 + 1] = dirs[i][1];
		dirBlob[i * 4 + 2] = dirs[i][2]; dirBlob[i * 4 + 3] = 0.0f;
	}

	static rhi::ShaderHandle s_shader = 0;
	if ( s_shader == 0 ) {
		s_shader = r->CreateComputeShader( "cs_rtworldtest", RTWORLD_SRC );
	}
	float gpuT[NR];
	bool traced = false;
	if ( s_shader != 0 ) {
		float seed[NR];
		for ( int i = 0; i < NR; i++ ) { seed[i] = -3.0f; }		// sentinel: dead dispatch != miss
		rhi::BufferHandle bOut = r->CreateBuffer( rhi::BU_STORAGE, NR * (int)sizeof( float ), seed );
		rhi::BufferHandle bRays = r->CreateBuffer( rhi::BU_STORAGE, NR * 4 * (int)sizeof( float ), dirBlob );
		struct { unsigned int tlasLo, tlasHi, count; float ox, oy, oz, tmax; } pc;
		pc.tlasLo = (unsigned int)( tlasAddr & 0xFFFFFFFFu );
		pc.tlasHi = (unsigned int)( tlasAddr >> 32 );
		pc.count = NR;
		pc.ox = rv.vieworg[0]; pc.oy = rv.vieworg[1]; pc.oz = rv.vieworg[2];
		pc.tmax = TMAX;
		rhi::ComputeArgs ca;
		memset( &ca, 0, sizeof( ca ) );
		ca.shader = s_shader;
		ca.storage[0] = bOut;
		ca.storage[1] = bRays;
		ca.pushConstants = &pc;
		ca.pushConstantSize = (int)sizeof( pc );
		ca.groupsX = 1; ca.groupsY = 1; ca.groupsZ = 1;
		r->DispatchSync( ca );
		traced = r->ReadBuffer( bOut, gpuT, NR * (int)sizeof( float ) );
		r->DestroyBuffer( bOut );
		r->DestroyBuffer( bRays );
	}
	if ( !traced ) {
		common->Printf( "r_rtWorldTest: FAIL (trace dispatch/readback failed)\n" );
		if ( !persistent ) {
			r->DestroyRtScene();
		}
		RTTEST_CLEANUP();
		return;
	}

	// CPU reference over the identical soup (areas + instances) + boundary classification
	int gpuHits = 0, mismatch = 0, boundary = 0, genuine = 0, firstGenuine = -1;
	for ( int i = 0; i < NR; i++ ) {
		const float cpuT = R_RtCpuSceneTrace( rv.vieworg, dirs[i], TMAX,
			aPos, aIdx, aSlices, numASlices, mPos, mIdx, mSlices, mInsts, numMInsts );
		const bool gpuHit = gpuT[i] >= 0.0f;
		if ( gpuHit ) {
			gpuHits++;
		}
		const float tol = 0.05f + ( cpuT > 0.0f ? cpuT : 0.0f ) * 0.001f;
		const bool ok = ( gpuHit == ( cpuT >= 0.0f ) )
			&& ( !gpuHit || ( gpuT[i] > cpuT - tol && gpuT[i] < cpuT + tol ) );
		if ( ok ) {
			continue;
		}
		mismatch++;
		// perturb the ray slightly; if any perturbed CPU trace reproduces the GPU result the
		// ray grazes an edge/crack (legit FP divergence), not a broken AS
		bool isBoundary = false;
		for ( int p = 0; p < 4 && !isBoundary; p++ ) {
			idVec3 pd = dirs[i]
				+ ( ( p & 1 ) ? 0.002f : -0.002f ) * ( ( p & 2 ) ? rv.viewaxis[1] : rv.viewaxis[2] );
			pd.Normalize();
			const float pT = R_RtCpuSceneTrace( rv.vieworg, pd, TMAX,
				aPos, aIdx, aSlices, numASlices, mPos, mIdx, mSlices, mInsts, numMInsts );
			const float ptol = 0.05f + ( pT > 0.0f ? pT : 0.0f ) * 0.005f;	// looser: the ray moved
			isBoundary = ( gpuHit == ( pT >= 0.0f ) )
				&& ( !gpuHit || ( gpuT[i] > pT - ptol && gpuT[i] < pT + ptol ) );
		}
		if ( isBoundary ) {
			boundary++;
		} else {
			genuine++;
			if ( firstGenuine < 0 ) {
				firstGenuine = i;
			}
		}
	}

	common->Printf( "r_rtWorldTest: %d area + %d model BLAS (%d world + %d model tris, %d instances, %s); "
		"%d/%d rays hit -- %s (mismatch %d = %d boundary-FP + %d genuine, first genuine @%d)\n",
		numASlices, numMSlices, worldIndexes / 3, mTris, numMInsts,
		persistent ? "persistent scene" : va( "%d ms build", msBuild ),
		gpuHits, NR,
		( genuine == 0 && gpuHits > 0 ) ? "PASS" : "FAIL",
		mismatch, boundary, genuine, firstGenuine );

	if ( !persistent ) {
		r->DestroyRtScene();
	}
	RTTEST_CLEANUP();
	#undef RTTEST_CLEANUP
}

// ---- r_rtAnimBlasTest (S2): validate a device-buffer-fed animated BLAS --------------------------
// The claim S3+ stands on: a BLAS built/refit straight from a monster's GPU-skinned gpuSkinVB traces
// the SAME geometry the buffers hold. We read gpuSkinVB back to CPU (the exact bytes the BLAS build
// consumes), build a model-space CPU soup from it, then trace a grid against the GPU BLAS and diff
// every hit vs a CPU Moller-Trumbore sweep of that soup. This isolates the device-buffer BLAS path
// (build + in-place refit) from skin correctness (the GPU-skin work already established that) and
// sidesteps any one-frame pose lag between gpuSkinVB and tri->verts.

// One visible GPU-skinned shadow caster + its casting surfaces that have a gpuSkinVB and a
// device-addressable static index buffer. NULL if none in view.
static const idRenderEntityLocal *R_RtFindSkinnedCaster( idList<const srfTriangles_t *> &outSurfs ) {
	outSurfs.Clear();
	if ( tr.viewDef == NULL ) {
		return NULL;
	}
	for ( const viewEntity_t *vEnt = tr.viewDef->viewEntitys; vEnt; vEnt = vEnt->next ) {
		const idRenderEntityLocal *def = vEnt->entityDef;
		if ( def == NULL || def->parms.hModel == NULL || def->parms.noShadow || def->parms.weaponDepthHack
			|| def->parms.hModel->IsDynamicModel() != DM_CACHED || def->dynamicModel == NULL ) {
			continue;
		}
		const idRenderModel *dm = def->dynamicModel;
		idList<const srfTriangles_t *> surfs;
		for ( int s = 0; s < dm->NumSurfaces(); s++ ) {
			const modelSurface_t *surf = dm->Surface( s );
			const srfTriangles_t *tri = surf->geometry;
			if ( R_RtSurfCasts( surf ) && tri->gpuSkinVB != 0
				&& tri->indexCache != NULL && tri->indexCache->vbo != 0 ) {
				surfs.Append( tri );
			}
		}
		if ( surfs.Num() > 0 ) {
			outSurfs = surfs;
			return def;
		}
	}
	return NULL;
}

// Trace GxG rays from `origin` toward the bounds center-plane against tlasAddr on the GPU, and diff
// each committed t vs a CPU trace of the same soup. Hit/miss flips a slightly-perturbed CPU ray
// reproduces are bucketed boundary-FP (edge grazes), like the other RT validators. Accumulates.
static bool R_RtAnimTraceCompare( rhi::RHI *r, rhi::ShaderHandle shader, unsigned long long tlasAddr,
		const idVec3 &origin, int G, const idVec3 &center, float radius,
		const float *cpuPos, const int *cpuIdx, int totalIdx,
		int &hits, int &mismatch, int &boundary, int &genuine, int &firstBad ) {
	const int NR = G * G;
	const float TMAX = 100000.0f;
	idVec3 D = center - origin;
	D.Normalize();
	idVec3 e1 = D.Cross( idVec3( 0.0f, 0.0f, 1.0f ) );
	if ( e1.LengthSqr() < 1e-6f ) {
		e1 = D.Cross( idVec3( 0.0f, 1.0f, 0.0f ) );
	}
	e1.Normalize();
	idVec3 e2 = D.Cross( e1 );
	e2.Normalize();
	idVec3 *dirs = (idVec3 *)Mem_Alloc16( NR * (int)sizeof( idVec3 ) );
	float *dirBlob = (float *)Mem_Alloc16( NR * 4 * (int)sizeof( float ) );
	for ( int i = 0; i < NR; i++ ) {
		const float sx = ( ( (float)( i % G ) + 0.5f ) / (float)G * 2.0f - 1.0f ) * 1.4f * radius;
		const float sy = ( ( (float)( i / G ) + 0.5f ) / (float)G * 2.0f - 1.0f ) * 1.4f * radius;
		idVec3 target = center + sx * e1 + sy * e2;
		dirs[i] = target - origin;
		dirs[i].Normalize();
		dirBlob[i * 4 + 0] = dirs[i][0]; dirBlob[i * 4 + 1] = dirs[i][1];
		dirBlob[i * 4 + 2] = dirs[i][2]; dirBlob[i * 4 + 3] = 0.0f;
	}
	float *gpuT = (float *)Mem_Alloc16( NR * (int)sizeof( float ) );
	float *seed = (float *)Mem_Alloc16( NR * (int)sizeof( float ) );
	for ( int i = 0; i < NR; i++ ) { seed[i] = -3.0f; }		// sentinel: dead dispatch != miss
	rhi::BufferHandle bOut = r->CreateBuffer( rhi::BU_STORAGE, NR * (int)sizeof( float ), seed );
	rhi::BufferHandle bRays = r->CreateBuffer( rhi::BU_STORAGE, NR * 4 * (int)sizeof( float ), dirBlob );
	struct { unsigned int tlasLo, tlasHi, count; float ox, oy, oz, tmax; } pc;
	pc.tlasLo = (unsigned int)( tlasAddr & 0xFFFFFFFFu );
	pc.tlasHi = (unsigned int)( tlasAddr >> 32 );
	pc.count = (unsigned int)NR;
	pc.ox = origin[0]; pc.oy = origin[1]; pc.oz = origin[2];
	pc.tmax = TMAX;
	rhi::ComputeArgs ca;
	memset( &ca, 0, sizeof( ca ) );
	ca.shader = shader;
	ca.storage[0] = bOut;
	ca.storage[1] = bRays;
	ca.pushConstants = &pc;
	ca.pushConstantSize = (int)sizeof( pc );
	ca.groupsX = ( NR + 63 ) / 64; ca.groupsY = 1; ca.groupsZ = 1;
	r->DispatchSync( ca );
	const bool traced = r->ReadBuffer( bOut, gpuT, NR * (int)sizeof( float ) );
	r->DestroyBuffer( bOut );
	r->DestroyBuffer( bRays );
	if ( traced ) {
		for ( int i = 0; i < NR; i++ ) {
			const float cpuT = R_RtCpuTrace( origin, dirs[i], TMAX, cpuPos, cpuIdx, 0, totalIdx, 0 );
			const bool gpuHit = gpuT[i] >= 0.0f;
			if ( gpuHit ) {
				hits++;
			}
			const float tol = 0.05f + ( cpuT > 0.0f ? cpuT : 0.0f ) * 0.001f;
			if ( ( gpuHit == ( cpuT >= 0.0f ) ) && ( !gpuHit || ( gpuT[i] > cpuT - tol && gpuT[i] < cpuT + tol ) ) ) {
				continue;
			}
			mismatch++;
			bool isBoundary = false;
			for ( int p = 0; p < 4 && !isBoundary; p++ ) {
				idVec3 pd = dirs[i] + ( ( p & 1 ) ? 0.002f : -0.002f ) * ( ( p & 2 ) ? e1 : e2 );
				pd.Normalize();
				const float pT = R_RtCpuTrace( origin, pd, TMAX, cpuPos, cpuIdx, 0, totalIdx, 0 );
				const float ptol = 0.05f + ( pT > 0.0f ? pT : 0.0f ) * 0.005f;
				isBoundary = ( gpuHit == ( pT >= 0.0f ) ) && ( !gpuHit || ( gpuT[i] > pT - ptol && gpuT[i] < pT + ptol ) );
			}
			if ( isBoundary ) {
				boundary++;
			} else {
				genuine++;
				if ( firstBad < 0 ) { firstBad = i; }
			}
		}
	}
	Mem_Free16( dirs );
	Mem_Free16( dirBlob );
	Mem_Free16( gpuT );
	Mem_Free16( seed );
	return traced;
}

static void R_RtAnimBlasValidate( void ) {
	if ( !r_rtAnimBlasTest.GetBool() ) {
		if ( r_rtAnimBlasTest.IsModified() ) {
			r_rtAnimBlasTest.ClearModified();
		}
		return;
	}
	if ( R_RtSceneWorld() == NULL ) {
		return;							// stay armed until a map-world primary view renders
	}
	r_rtAnimBlasTest.SetBool( false );	// consume: one-shot
	r_rtAnimBlasTest.ClearModified();

	rhi::RHI *r = rhi::GetRHI();
	if ( r == NULL || !r->SupportsRayQuery() ) {
		common->Printf( "r_rtAnimBlasTest: unavailable (needs Vulkan + KHR_ray_query hardware)\n" );
		return;
	}
	idList<const srfTriangles_t *> surfs;
	const idRenderEntityLocal *def = R_RtFindSkinnedCaster( surfs );
	if ( def == NULL ) {
		common->Printf( "r_rtAnimBlasTest: no visible GPU-skinned shadow caster (need r_gpuSkinning on + a monster in view)\n" );
		return;
	}
	const int MAXG = 64;			// mirrors the backend's MAX_BLAS_GEOMS
	int nSurf = surfs.Num();
	const bool truncated = nSurf > MAXG;
	if ( truncated ) { nSurf = MAXG; }

	// build BlasGeometry[] (device addresses) + a model-space CPU soup from the read-back gpuSkinVB
	rhi::RHI::BlasGeometry geoms[64];
	int totalVerts = 0, totalIdx = 0;
	for ( int s = 0; s < nSurf; s++ ) {
		totalVerts += surfs[s]->numVerts;
		totalIdx += surfs[s]->numIndexes;
	}
	float *cpuPos = (float *)Mem_Alloc16( totalVerts * 3 * (int)sizeof( float ) );
	int *cpuIdx = (int *)Mem_Alloc16( totalIdx * (int)sizeof( int ) );
	idDrawVert *tmp = (idDrawVert *)Mem_Alloc16( totalVerts * (int)sizeof( idDrawVert ) );
	int vbase = 0, ibase = 0;
	float skinDelta = 0.0f;			// informational: |gpuSkinVB.xyz - tri->verts.xyz| (skin path + 1-frame lag)
	bool readOk = true;
	for ( int s = 0; s < nSurf; s++ ) {
		const srfTriangles_t *tri = surfs[s];
		geoms[s].vertexAddress = r->GetBufferDeviceAddress( tri->gpuSkinVB );
		geoms[s].vertexStride = (unsigned int)sizeof( idDrawVert );
		geoms[s].vertexCount = (unsigned int)tri->numVerts;
		geoms[s].indexAddress = r->GetBufferDeviceAddress( (rhi::BufferHandle)tri->indexCache->vbo );
		geoms[s].indexCount = (unsigned int)tri->numIndexes;
		if ( geoms[s].vertexAddress == 0 || geoms[s].indexAddress == 0 ) {
			readOk = false;			// S0 usage flags missing, or index buffer fell back to host memory
			break;
		}
		idDrawVert *sv = tmp + vbase;
		if ( !r->ReadBuffer( tri->gpuSkinVB, sv, tri->numVerts * (int)sizeof( idDrawVert ) ) ) {
			readOk = false;
			break;
		}
		for ( int k = 0; k < tri->numVerts; k++ ) {
			float *dst = cpuPos + ( vbase + k ) * 3;
			dst[0] = sv[k].xyz.x; dst[1] = sv[k].xyz.y; dst[2] = sv[k].xyz.z;
			const idVec3 d = sv[k].xyz - tri->verts[k].xyz;
			const float mlen = d.Length();
			if ( mlen > skinDelta ) { skinDelta = mlen; }
		}
		for ( int m = 0; m < tri->numIndexes; m++ ) {
			cpuIdx[ibase + m] = vbase + tri->indexes[m];
		}
		vbase += tri->numVerts;
		ibase += tri->numIndexes;
	}
	Mem_Free16( tmp );
	if ( !readOk ) {
		common->Printf( "r_rtAnimBlasTest: FAIL (no device address / readback for gpuSkinVB or index buffer -- S0 usage flags?)\n" );
		Mem_Free16( cpuPos ); Mem_Free16( cpuIdx );
		return;
	}

	// model-space bounds of the soup -> synthetic ray origins (3 corners so rays hit >1 face)
	idVec3 mn( idMath::INFINITY, idMath::INFINITY, idMath::INFINITY );
	idVec3 mx( -idMath::INFINITY, -idMath::INFINITY, -idMath::INFINITY );
	for ( int k = 0; k < totalVerts; k++ ) {
		const idVec3 v( cpuPos[k * 3 + 0], cpuPos[k * 3 + 1], cpuPos[k * 3 + 2] );
		mn.x = Min( mn.x, v.x ); mn.y = Min( mn.y, v.y ); mn.z = Min( mn.z, v.z );
		mx.x = Max( mx.x, v.x ); mx.y = Max( mx.y, v.y ); mx.z = Max( mx.z, v.z );
	}
	const idVec3 center = ( mn + mx ) * 0.5f;
	float radius = ( ( mx - mn ) * 0.5f ).Length();
	if ( radius < 1.0f ) { radius = 1.0f; }
	const float stand = 3.0f * radius;
	idVec3 dirsCorner[3] = { idVec3( 1, 1, 1 ), idVec3( -1, -1, 1 ), idVec3( 1, -1, -1 ) };
	idVec3 origins[3];
	for ( int c = 0; c < 3; c++ ) {
		idVec3 n = dirsCorner[c]; n.Normalize();
		origins[c] = center + n * stand;
	}

	// build the GPU BLAS from gpuSkinVB, wrap in a standalone TLAS (does not touch the live scene)
	rhi::BlasHandle blas = r->CreateBlasFromBuffers( geoms, nSurf, true );
	if ( blas == 0 ) {
		common->Printf( "r_rtAnimBlasTest: FAIL (CreateBlasFromBuffers)\n" );
		Mem_Free16( cpuPos ); Mem_Free16( cpuIdx );
		return;
	}
	rhi::RHI::RtInstance inst;
	memset( &inst, 0, sizeof( inst ) );
	inst.transform[0] = 1.0f; inst.transform[5] = 1.0f; inst.transform[10] = 1.0f;	// identity 3x4
	inst.blas = blas;
	inst.mask = 0xFF;

	static rhi::ShaderHandle s_animShader = 0;
	if ( s_animShader == 0 ) {
		s_animShader = r->CreateComputeShader( "cs_rtanimblastest", RTWORLD_SRC );
	}
	const int G = 16;
	int hits = 0, mm = 0, bd = 0, gen = 0, fb = -1;
	bool traced = ( s_animShader != 0 );
	unsigned long long tlas = r->BuildStandaloneTlas( &inst, 1 );
	if ( tlas != 0 && traced ) {
		for ( int c = 0; c < 3; c++ ) {
			traced = R_RtAnimTraceCompare( r, s_animShader, tlas, origins[c], G, center, radius,
				cpuPos, cpuIdx, totalIdx, hits, mm, bd, gen, fb ) && traced;
		}
	}

	// refit the BLAS in place from the same buffers, rebuild the TLAS (bounds may shift), re-trace:
	// proves the UPDATE path does not corrupt the AS
	r->RefitBlas( blas, geoms, nSurf );		// out of frame -> synchronous
	int rhits = 0, rmm = 0, rbd = 0, rgen = 0, rfb = -1;
	bool rtraced = traced;
	unsigned long long tlas2 = r->BuildStandaloneTlas( &inst, 1 );
	if ( tlas2 != 0 && rtraced ) {
		for ( int c = 0; c < 3; c++ ) {
			rtraced = R_RtAnimTraceCompare( r, s_animShader, tlas2, origins[c], G, center, radius,
				cpuPos, cpuIdx, totalIdx, rhits, rmm, rbd, rgen, rfb ) && rtraced;
		}
	}

	const bool buildPass = traced && tlas != 0 && gen == 0 && hits > 0;
	const bool refitPass = rtraced && tlas2 != 0 && rgen == 0 && rhits > 0;
	common->Printf( "r_rtAnimBlasTest: '%s' %d surf%s, %d tris, %d verts; "
		"build %s (%d/%d hit, mismatch %d = %d bFP + %d genuine@%d); "
		"refit %s (%d/%d hit, %d genuine@%d); |gpuSkinVB-verts| max %.5f\n",
		def->parms.hModel->Name(), nSurf, truncated ? "(capped)" : "", totalIdx / 3, totalVerts,
		buildPass ? "PASS" : ( traced ? "FAIL" : "no-trace" ), hits, G * G * 3, mm, bd, gen, fb,
		refitPass ? "PASS" : ( rtraced ? "FAIL" : "no-trace" ), rhits, G * G * 3, rgen, rfb,
		skinDelta );

	r->DestroyBlas( blas );
	r->DestroyStandaloneTlas();
	Mem_Free16( cpuPos );
	Mem_Free16( cpuIdx );
}

// ---- r_rtReflTest (RR1): validate the reflection-hit attribute fetch --------------------------------
// The crux RT reflections stands on: at a ray-query hit, resolve the geometry table by the hit's
// instanceCustomIndex + geometryIndex, dereference the hit surface's gpuSkinVB by device address
// (GL_EXT_buffer_reference), interpolate the triangle's vertex normals by the hit barycentrics, and
// transform to world by ObjectToWorld. This shader does exactly that and writes the world normal + t;
// the CPU reference (R_RtCpuTraceHitNormal) computes the same over the read-back soup. Standalone
// (one identity instance, so ObjectToWorld = identity and both compare in model space), like
// r_rtAnimBlasTest. idDrawVert: stride 60 B = 15 uints, normal at uint offset 5 (byte 20).
static const char *RTREFL_TEST_SRC =
	"#version 460\n"
	"#extension GL_EXT_ray_query : require\n"
	"#extension GL_EXT_buffer_reference : require\n"
	"layout(local_size_x = 64) in;\n"
	"layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer VertRef { uint w[]; };\n"
	"layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer IdxRef  { uint i[]; };\n"
	"struct GeoDesc { VertRef vb; IdxRef ib; uint stride; uint flags; uint baseColor; uint texIndex; };\n"
	"layout(buffer_reference, std430, buffer_reference_align = 8) readonly buffer GeoTable { GeoDesc d[]; };\n"
	"layout(std430, binding = 0) writeonly buffer Out   { vec4 hit[]; } outb;\n"
	"layout(std430, binding = 1) readonly  buffer Rays  { vec4 dir[]; } rays;\n"
	"layout(std430, binding = 2) writeonly buffer StOut { vec4 stt[]; } stb;\n"	// RR4: st.xy, texIndex, _
	"layout(push_constant) uniform PC { GeoTable geoTable; uvec2 tlas; uint count; float ox, oy, oz, tmax; } pc;\n"
	"void main() {\n"
	"    uint id = gl_GlobalInvocationID.x;\n"
	"    if ( id >= pc.count ) { return; }\n"
	"    rayQueryEXT rq;\n"
	"    rayQueryInitializeEXT( rq, accelerationStructureEXT( pc.tlas ), gl_RayFlagsOpaqueEXT, 0xFFu,\n"
	"                           vec3( pc.ox, pc.oy, pc.oz ), 0.0, rays.dir[id].xyz, pc.tmax );\n"
	"    while ( rayQueryProceedEXT( rq ) ) { }\n"
	"    if ( rayQueryGetIntersectionTypeEXT( rq, true ) != gl_RayQueryCommittedIntersectionTriangleEXT ) {\n"
	"        outb.hit[id] = vec4( 0.0, 0.0, 0.0, -1.0 ); stb.stt[id] = vec4( 0.0 ); return;\n"		// miss
	"    }\n"
	"    uint ci   = uint( rayQueryGetIntersectionInstanceCustomIndexEXT( rq, true ) );\n"
	"    uint gi   = uint( rayQueryGetIntersectionGeometryIndexEXT( rq, true ) );\n"
	"    uint prim = uint( rayQueryGetIntersectionPrimitiveIndexEXT( rq, true ) );\n"
	"    vec2 bc   = rayQueryGetIntersectionBarycentricsEXT( rq, true );\n"
	"    mat4x3 o2w = rayQueryGetIntersectionObjectToWorldEXT( rq, true );\n"
	"    float t   = rayQueryGetIntersectionTEXT( rq, true );\n"
	"    GeoDesc g = pc.geoTable.d[ ci + gi ];\n"
	"    if ( ( g.flags & 1u ) == 0u ) { outb.hit[id] = vec4( 0.0, 0.0, 0.0, -2.0 ); stb.stt[id] = vec4( 0.0 ); return; }\n"	// static / no-attr row
	"    uint s  = g.stride >> 2u;\n"		// uints per vertex (60/4 = 15)
	"    uint i0 = g.ib.i[ 3u * prim + 0u ];\n"
	"    uint i1 = g.ib.i[ 3u * prim + 1u ];\n"
	"    uint i2 = g.ib.i[ 3u * prim + 2u ];\n"
	"    vec3 n0 = vec3( uintBitsToFloat( g.vb.w[ i0*s + 5u ] ), uintBitsToFloat( g.vb.w[ i0*s + 6u ] ), uintBitsToFloat( g.vb.w[ i0*s + 7u ] ) );\n"
	"    vec3 n1 = vec3( uintBitsToFloat( g.vb.w[ i1*s + 5u ] ), uintBitsToFloat( g.vb.w[ i1*s + 6u ] ), uintBitsToFloat( g.vb.w[ i1*s + 7u ] ) );\n"
	"    vec3 n2 = vec3( uintBitsToFloat( g.vb.w[ i2*s + 5u ] ), uintBitsToFloat( g.vb.w[ i2*s + 6u ] ), uintBitsToFloat( g.vb.w[ i2*s + 7u ] ) );\n"
	"    vec3 nm = ( 1.0 - bc.x - bc.y ) * n0 + bc.x * n1 + bc.y * n2;\n"
	"    vec3 wn = mat3( o2w ) * nm;\n"
	"    float l = length( wn );\n"
	"    vec2 t0 = vec2( uintBitsToFloat( g.vb.w[ i0*s + 3u ] ), uintBitsToFloat( g.vb.w[ i0*s + 4u ] ) );\n"	// RR4: st at uint offset 3
	"    vec2 t1 = vec2( uintBitsToFloat( g.vb.w[ i1*s + 3u ] ), uintBitsToFloat( g.vb.w[ i1*s + 4u ] ) );\n"
	"    vec2 t2 = vec2( uintBitsToFloat( g.vb.w[ i2*s + 3u ] ), uintBitsToFloat( g.vb.w[ i2*s + 4u ] ) );\n"
	"    vec2 stm = ( 1.0 - bc.x - bc.y ) * t0 + bc.x * t1 + bc.y * t2;\n"
	"    outb.hit[id] = vec4( ( l > 0.0 ) ? wn / l : vec3( 0.0 ), t );\n"
	"    stb.stt[id] = vec4( stm.x, stm.y, float( g.texIndex ), 0.0 );\n"
	"}\n";

// CPU closest-hit over the concatenated model-space soup, returning t and the barycentric-interpolated
// (normalized) normal at the hit — the reference for the GPU fetch above. Same Moller-Trumbore + same
// (1-u-v, u, v) -> (n0, n1, n2) weighting as the shader.
static float R_RtCpuTraceHitNormal( const idVec3 &org, const idVec3 &dir, float tmax,
		const float *pos, const int *idx, const float *nrm, const float *stArr, int idxCount,
		idVec3 &outNormal, idVec2 &outSt ) {
	float best = -1.0f;
	int bi0 = 0, bi1 = 0, bi2 = 0;
	float bu = 0.0f, bv = 0.0f;
	for ( int n = 0; n < idxCount; n += 3 ) {
		const int i0 = idx[n + 0], i1 = idx[n + 1], i2 = idx[n + 2];
		const float *v0 = pos + i0 * 3, *v1 = pos + i1 * 3, *v2 = pos + i2 * 3;
		const idVec3 e1( v1[0]-v0[0], v1[1]-v0[1], v1[2]-v0[2] );
		const idVec3 e2( v2[0]-v0[0], v2[1]-v0[1], v2[2]-v0[2] );
		const idVec3 pv = dir.Cross( e2 );
		const float det = e1 * pv;
		if ( det > -1e-8f && det < 1e-8f ) {
			continue;
		}
		const float inv = 1.0f / det;
		const idVec3 tv( org[0]-v0[0], org[1]-v0[1], org[2]-v0[2] );
		const float u = ( tv * pv ) * inv;
		if ( u < 0.0f || u > 1.0f ) {
			continue;
		}
		const idVec3 qv = tv.Cross( e1 );
		const float v = ( dir * qv ) * inv;
		if ( v < 0.0f || u + v > 1.0f ) {
			continue;
		}
		const float t = ( e2 * qv ) * inv;
		if ( t > 0.0f && t < tmax && ( best < 0.0f || t < best ) ) {
			best = t; bi0 = i0; bi1 = i1; bi2 = i2; bu = u; bv = v;
		}
	}
	if ( best < 0.0f ) {
		outNormal.Zero();
		outSt.Zero();
		return -1.0f;
	}
	const float bw = 1.0f - bu - bv;
	const idVec3 n0( nrm[bi0*3+0], nrm[bi0*3+1], nrm[bi0*3+2] );
	const idVec3 n1( nrm[bi1*3+0], nrm[bi1*3+1], nrm[bi1*3+2] );
	const idVec3 n2( nrm[bi2*3+0], nrm[bi2*3+1], nrm[bi2*3+2] );
	idVec3 nm = bw * n0 + bu * n1 + bv * n2;
	nm.Normalize();
	outNormal = nm;
	if ( stArr != NULL ) {
		outSt = bw * idVec2( stArr[bi0*2+0], stArr[bi0*2+1] )
		      + bu * idVec2( stArr[bi1*2+0], stArr[bi1*2+1] )
		      + bv * idVec2( stArr[bi2*2+0], stArr[bi2*2+1] );
	} else {
		outSt.Zero();
	}
	return best;
}

// Trace GxG reflection-probe rays from `origin` toward the bounds centre against tlasAddr, fetching the
// hit's world normal on the GPU (RTREFL_TEST_SRC) via geoTableAddr; diff each vs the CPU normal at the
// same hit. Same-hit gate: only compare normals when the GPU/CPU hit distances agree (else the ray
// grazed an edge onto a different triangle — bucket boundary). Accumulates.
static bool R_RtReflTraceCompare( rhi::RHI *r, rhi::ShaderHandle shader, unsigned long long tlasAddr,
		unsigned long long geoTableAddr, const idVec3 &origin, int G, const idVec3 &center, float radius,
		const float *cpuPos, const int *cpuIdx, const float *cpuNrm, const float *cpuSt, int totalIdx,
		int &hits, int &mismatch, int &boundary, int &genuine, float &maxAngErrDeg, int &firstBad,
		float &maxStErr, int &stMismatch, int &texSeen ) {
	const int NR = G * G;
	const float TMAX = 100000.0f;
	idVec3 D = center - origin; D.Normalize();
	idVec3 e1 = D.Cross( idVec3( 0.0f, 0.0f, 1.0f ) );
	if ( e1.LengthSqr() < 1e-6f ) { e1 = D.Cross( idVec3( 0.0f, 1.0f, 0.0f ) ); }
	e1.Normalize();
	idVec3 e2 = D.Cross( e1 ); e2.Normalize();
	idVec3 *dirs = (idVec3 *)Mem_Alloc16( NR * (int)sizeof( idVec3 ) );
	float *dirBlob = (float *)Mem_Alloc16( NR * 4 * (int)sizeof( float ) );
	for ( int i = 0; i < NR; i++ ) {
		const float sx = ( ( (float)( i % G ) + 0.5f ) / (float)G * 2.0f - 1.0f ) * 1.4f * radius;
		const float sy = ( ( (float)( i / G ) + 0.5f ) / (float)G * 2.0f - 1.0f ) * 1.4f * radius;
		idVec3 target = center + sx * e1 + sy * e2;
		dirs[i] = target - origin; dirs[i].Normalize();
		dirBlob[i*4+0] = dirs[i][0]; dirBlob[i*4+1] = dirs[i][1]; dirBlob[i*4+2] = dirs[i][2]; dirBlob[i*4+3] = 0.0f;
	}
	float *gpu = (float *)Mem_Alloc16( NR * 4 * (int)sizeof( float ) );	// vec4 per ray: xyz normal, w = t
	float *seed = (float *)Mem_Alloc16( NR * 4 * (int)sizeof( float ) );
	for ( int i = 0; i < NR * 4; i++ ) { seed[i] = -3.0f; }
	rhi::BufferHandle bOut = r->CreateBuffer( rhi::BU_STORAGE, NR * 4 * (int)sizeof( float ), seed );
	rhi::BufferHandle bRays = r->CreateBuffer( rhi::BU_STORAGE, NR * 4 * (int)sizeof( float ), dirBlob );
	float *gpuSt = (float *)Mem_Alloc16( NR * 4 * (int)sizeof( float ) );	// RR4: vec4/ray = st.xy, texIndex, _
	rhi::BufferHandle bStTex = r->CreateBuffer( rhi::BU_STORAGE, NR * 4 * (int)sizeof( float ), seed );	// reuse the -3 seed
	struct { unsigned long long geoTable; unsigned int tlasLo, tlasHi, count; float ox, oy, oz, tmax; } pc;
	pc.geoTable = geoTableAddr;
	pc.tlasLo = (unsigned int)( tlasAddr & 0xFFFFFFFFu );
	pc.tlasHi = (unsigned int)( tlasAddr >> 32 );
	pc.count = (unsigned int)NR;
	pc.ox = origin[0]; pc.oy = origin[1]; pc.oz = origin[2]; pc.tmax = TMAX;
	rhi::ComputeArgs ca;
	memset( &ca, 0, sizeof( ca ) );
	ca.shader = shader;
	ca.storage[0] = bOut;
	ca.storage[1] = bRays;
	ca.storage[2] = bStTex;
	ca.pushConstants = &pc;
	ca.pushConstantSize = (int)sizeof( pc );
	ca.groupsX = ( NR + 63 ) / 64; ca.groupsY = 1; ca.groupsZ = 1;
	r->DispatchSync( ca );
	const bool traced = r->ReadBuffer( bOut, gpu, NR * 4 * (int)sizeof( float ) );
	const bool gotSt = r->ReadBuffer( bStTex, gpuSt, NR * 4 * (int)sizeof( float ) );
	r->DestroyBuffer( bOut );
	r->DestroyBuffer( bRays );
	r->DestroyBuffer( bStTex );
	if ( traced ) {
		for ( int i = 0; i < NR; i++ ) {
			const idVec3 gpuN( gpu[i*4+0], gpu[i*4+1], gpu[i*4+2] );
			const float gpuT = gpu[i*4+3];
			idVec3 cpuN; idVec2 cpuStOut;
			const float cpuT = R_RtCpuTraceHitNormal( origin, dirs[i], TMAX, cpuPos, cpuIdx, cpuNrm, cpuSt, totalIdx, cpuN, cpuStOut );
			const bool gpuHit = gpuT >= 0.0f;
			const bool cpuHit = cpuT >= 0.0f;
			if ( gpuHit ) { hits++; }
			// same-hit gate: both must hit at the same distance for the normals to be comparable
			const float ttol = 0.05f + ( cpuT > 0.0f ? cpuT : 0.0f ) * 0.002f;
			if ( gpuHit && cpuHit && idMath::Fabs( gpuT - cpuT ) < ttol ) {
				const float d = idMath::ClampFloat( -1.0f, 1.0f, gpuN * cpuN );
				const float ang = RAD2DEG( idMath::ACos( d ) );
				if ( ang > maxAngErrDeg ) { maxAngErrDeg = ang; }
				if ( d < 0.985f ) {			// > ~10 deg apart at the same hit point = a real fetch/interp error
					mismatch++;
					genuine++;
					if ( firstBad < 0 ) { firstBad = i; }
				}
				// RR4: st fetched at the same hit must match the CPU-interpolated st; texIndex reads back nonzero
				if ( gotSt ) {
					const float se = Max( idMath::Fabs( gpuSt[i*4+0] - cpuStOut.x ), idMath::Fabs( gpuSt[i*4+1] - cpuStOut.y ) );
					if ( se > maxStErr ) { maxStErr = se; }
					if ( se > 0.01f ) { stMismatch++; }
					if ( gpuSt[i*4+2] != 0.0f ) { texSeen++; }
				}
			} else if ( gpuHit || cpuHit ) {
				// hit/miss flip, or both hit but at different distances (edge graze onto a different
				// triangle) — legit FP divergence between the GPU traversal and the CPU sweep, not a fetch bug
				mismatch++;
				boundary++;
			}
		}
	}
	Mem_Free16( dirs ); Mem_Free16( dirBlob ); Mem_Free16( gpu ); Mem_Free16( seed ); Mem_Free16( gpuSt );
	return traced;
}

static void R_RtReflValidate( void ) {
	if ( !r_rtReflTest.GetBool() ) {
		if ( r_rtReflTest.IsModified() ) { r_rtReflTest.ClearModified(); }
		return;
	}
	if ( R_RtSceneWorld() == NULL ) {
		return;
	}
	r_rtReflTest.SetBool( false );
	r_rtReflTest.ClearModified();

	rhi::RHI *r = rhi::GetRHI();
	if ( r == NULL || !r->SupportsRayQuery() ) {
		common->Printf( "r_rtReflTest: unavailable (needs Vulkan + KHR_ray_query hardware)\n" );
		return;
	}
	idList<const srfTriangles_t *> surfs;
	const idRenderEntityLocal *def = R_RtFindSkinnedCaster( surfs );
	if ( def == NULL ) {
		common->Printf( "r_rtReflTest: no visible GPU-skinned monster (need r_gpuSkinning on + a monster in view)\n" );
		return;
	}
	const int MAXG = 64;
	int nSurf = surfs.Num();
	if ( nSurf > MAXG ) { nSurf = MAXG; }

	// per-surface geometry table rows (device addresses) + a model-space CPU soup (pos + normal) from
	// the read-back gpuSkinVB — the exact bytes the shader dereferences.
	struct GeoRow { unsigned long long vtxAddr, idxAddr; unsigned int stride, flags, baseColor, texIndex; };	// matches shader GeoDesc (32 B)
	GeoRow *rows = (GeoRow *)Mem_Alloc16( nSurf * (int)sizeof( GeoRow ) );
	int totalVerts = 0, totalIdx = 0;
	for ( int s = 0; s < nSurf; s++ ) { totalVerts += surfs[s]->numVerts; totalIdx += surfs[s]->numIndexes; }
	float *cpuPos = (float *)Mem_Alloc16( totalVerts * 3 * (int)sizeof( float ) );
	float *cpuNrm = (float *)Mem_Alloc16( totalVerts * 3 * (int)sizeof( float ) );
	float *cpuSt = (float *)Mem_Alloc16( totalVerts * 2 * (int)sizeof( float ) );	// RR4: source st soup
	int *cpuIdx = (int *)Mem_Alloc16( totalIdx * (int)sizeof( int ) );
	rhi::RHI::BlasGeometry geoms[64];
	idDrawVert *tmp = (idDrawVert *)Mem_Alloc16( totalVerts * (int)sizeof( idDrawVert ) );
	int vbase = 0, ibase = 0;
	bool readOk = true;
	for ( int s = 0; s < nSurf; s++ ) {
		const srfTriangles_t *tri = surfs[s];
		const unsigned long long va = r->GetBufferDeviceAddress( tri->gpuSkinVB );
		const unsigned long long ia = r->GetBufferDeviceAddress( (rhi::BufferHandle)tri->indexCache->vbo );
		if ( va == 0 || ia == 0 || !r->ReadBuffer( tri->gpuSkinVB, tmp + vbase, tri->numVerts * (int)sizeof( idDrawVert ) ) ) {
			readOk = false;
			break;
		}
		geoms[s].vertexAddress = va; geoms[s].vertexStride = (unsigned int)sizeof( idDrawVert );
		geoms[s].vertexCount = (unsigned int)tri->numVerts;
		geoms[s].indexAddress = ia; geoms[s].indexCount = (unsigned int)tri->numIndexes;
		rows[s].vtxAddr = va; rows[s].idxAddr = ia; rows[s].stride = (unsigned int)sizeof( idDrawVert );
		rows[s].flags = 1u; rows[s].baseColor = 0u; rows[s].texIndex = (unsigned int)( s + 1 );	// RR4: sentinel proves the field reads back
		for ( int k = 0; k < tri->numVerts; k++ ) {
			const idDrawVert &v = tmp[vbase + k];
			cpuPos[(vbase+k)*3+0] = v.xyz.x; cpuPos[(vbase+k)*3+1] = v.xyz.y; cpuPos[(vbase+k)*3+2] = v.xyz.z;
			cpuNrm[(vbase+k)*3+0] = v.normal.x; cpuNrm[(vbase+k)*3+1] = v.normal.y; cpuNrm[(vbase+k)*3+2] = v.normal.z;
			cpuSt[(vbase+k)*2+0] = v.st.x; cpuSt[(vbase+k)*2+1] = v.st.y;
		}
		for ( int m = 0; m < tri->numIndexes; m++ ) { cpuIdx[ibase + m] = vbase + tri->indexes[m]; }
		vbase += tri->numVerts; ibase += tri->numIndexes;
	}
	Mem_Free16( tmp );
	if ( !readOk ) {
		common->Printf( "r_rtReflTest: FAIL (no device address / readback for gpuSkinVB or index buffer)\n" );
		Mem_Free16( rows ); Mem_Free16( cpuPos ); Mem_Free16( cpuNrm ); Mem_Free16( cpuIdx ); Mem_Free16( cpuSt );
		return;
	}

	// device-addressable geometry table (customIndex 0 for the single standalone instance -> rows[0..])
	rhi::BufferHandle geoBuf = r->CreateBuffer( rhi::BU_STORAGE, nSurf * (int)sizeof( GeoRow ), rows );
	const unsigned long long geoAddr = geoBuf ? r->GetBufferDeviceAddress( geoBuf ) : 0;
	rhi::BlasHandle blas = ( geoAddr != 0 ) ? r->CreateBlasFromBuffers( geoms, nSurf, false ) : 0;
	if ( blas == 0 || geoAddr == 0 ) {
		common->Printf( "r_rtReflTest: FAIL (%s)\n", geoAddr == 0 ? "geometry table buffer" : "CreateBlasFromBuffers" );
		if ( geoBuf ) { r->DestroyBuffer( geoBuf ); }
		if ( blas ) { r->DestroyBlas( blas ); }
		Mem_Free16( rows ); Mem_Free16( cpuPos ); Mem_Free16( cpuNrm ); Mem_Free16( cpuIdx ); Mem_Free16( cpuSt );
		return;
	}
	rhi::RHI::RtInstance inst;
	memset( &inst, 0, sizeof( inst ) );
	inst.transform[0] = 1.0f; inst.transform[5] = 1.0f; inst.transform[10] = 1.0f;	// identity (customIndex 0)
	inst.blas = blas; inst.mask = 0xFF;
	const unsigned long long tlas = r->BuildStandaloneTlas( &inst, 1 );

	static rhi::ShaderHandle s_reflShader = 0;
	if ( s_reflShader == 0 ) { s_reflShader = r->CreateComputeShader( "cs_rtrefltest", RTREFL_TEST_SRC ); }

	idVec3 mn( idMath::INFINITY, idMath::INFINITY, idMath::INFINITY ), mx( -idMath::INFINITY, -idMath::INFINITY, -idMath::INFINITY );
	for ( int k = 0; k < totalVerts; k++ ) {
		const idVec3 v( cpuPos[k*3+0], cpuPos[k*3+1], cpuPos[k*3+2] );
		mn.x = Min( mn.x, v.x ); mn.y = Min( mn.y, v.y ); mn.z = Min( mn.z, v.z );
		mx.x = Max( mx.x, v.x ); mx.y = Max( mx.y, v.y ); mx.z = Max( mx.z, v.z );
	}
	const idVec3 center = ( mn + mx ) * 0.5f;
	float radius = ( ( mx - mn ) * 0.5f ).Length();
	if ( radius < 1.0f ) { radius = 1.0f; }
	const float stand = 3.0f * radius;
	idVec3 corner[3] = { idVec3( 1, 1, 1 ), idVec3( -1, -1, 1 ), idVec3( 1, -1, -1 ) };

	const int G = 16;
	int hits = 0, mm = 0, bd = 0, gen = 0, fb = -1;
	float maxAng = 0.0f;
	float maxSt = 0.0f; int stmm = 0, texSeen = 0;	// RR4: st error + texIndex readback
	bool traced = ( s_reflShader != 0 && tlas != 0 );
	if ( traced ) {
		for ( int c = 0; c < 3; c++ ) {
			idVec3 nrm = corner[c]; nrm.Normalize();
			const idVec3 origin = center + nrm * stand;
			traced = R_RtReflTraceCompare( r, s_reflShader, tlas, geoAddr, origin, G, center, radius,
				cpuPos, cpuIdx, cpuNrm, cpuSt, totalIdx, hits, mm, bd, gen, maxAng, fb, maxSt, stmm, texSeen ) && traced;
		}
	}
	const bool pass = traced && gen == 0 && stmm == 0 && hits > 0;
	common->Printf( "r_rtReflTest: '%s' %d surf, %d tris, %d verts; %s (%d/%d hit, mismatch %d = %d bFP + %d genuine@%d, max normal err %.2f deg; st max err %.4f, %d bad, texIndex seen %d)\n",
		def->parms.hModel->Name(), nSurf, totalIdx / 3, totalVerts,
		pass ? "PASS" : ( traced ? "FAIL" : "no-trace" ), hits, G * G * 3, mm, bd, gen, fb, maxAng, maxSt, stmm, texSeen );

	r->DestroyBlas( blas );
	r->DestroyStandaloneTlas();
	r->DestroyBuffer( geoBuf );
	Mem_Free16( rows ); Mem_Free16( cpuPos ); Mem_Free16( cpuNrm ); Mem_Free16( cpuIdx ); Mem_Free16( cpuSt );
}

/*
==========================
R_TransformModelToClip
==========================
*/
void R_TransformModelToClip( const idVec3 &src, const float *modelMatrix, const float *projectionMatrix, idPlane &eye, idPlane &dst ) {
	int i;

	for ( i = 0 ; i < 4 ; i++ ) {
		eye[i] =
			src[0] * modelMatrix[ i + 0 * 4 ] +
			src[1] * modelMatrix[ i + 1 * 4 ] +
			src[2] * modelMatrix[ i + 2 * 4 ] +
			1 * modelMatrix[ i + 3 * 4 ];
	}

	for ( i = 0 ; i < 4 ; i++ ) {
		dst[i] =
			eye[0] * projectionMatrix[ i + 0 * 4 ] +
			eye[1] * projectionMatrix[ i + 1 * 4 ] +
			eye[2] * projectionMatrix[ i + 2 * 4 ] +
			eye[3] * projectionMatrix[ i + 3 * 4 ];
	}
}

/*
==========================
R_GlobalToNormalizedDeviceCoordinates

-1 to 1 range in x, y, and z
==========================
*/
void R_GlobalToNormalizedDeviceCoordinates( const idVec3 &global, idVec3 &ndc ) {
	int		i;
	idPlane	view;
	idPlane	clip;

	// _D3XP added work on primaryView when no viewDef
	if ( !tr.viewDef ) {

		for ( i = 0 ; i < 4 ; i ++ ) {
			view[i] =
				global[0] * tr.primaryView->worldSpace.modelViewMatrix[ i + 0 * 4 ] +
				global[1] * tr.primaryView->worldSpace.modelViewMatrix[ i + 1 * 4 ] +
				global[2] * tr.primaryView->worldSpace.modelViewMatrix[ i + 2 * 4 ] +
					tr.primaryView->worldSpace.modelViewMatrix[ i + 3 * 4 ];
		}

		for ( i = 0 ; i < 4 ; i ++ ) {
			clip[i] =
				view[0] * tr.primaryView->projectionMatrix[ i + 0 * 4 ] +
				view[1] * tr.primaryView->projectionMatrix[ i + 1 * 4 ] +
				view[2] * tr.primaryView->projectionMatrix[ i + 2 * 4 ] +
				view[3] * tr.primaryView->projectionMatrix[ i + 3 * 4 ];
		}

	} else {

		for ( i = 0 ; i < 4 ; i ++ ) {
			view[i] =
				global[0] * tr.viewDef->worldSpace.modelViewMatrix[ i + 0 * 4 ] +
				global[1] * tr.viewDef->worldSpace.modelViewMatrix[ i + 1 * 4 ] +
				global[2] * tr.viewDef->worldSpace.modelViewMatrix[ i + 2 * 4 ] +
				tr.viewDef->worldSpace.modelViewMatrix[ i + 3 * 4 ];
		}


		for ( i = 0 ; i < 4 ; i ++ ) {
			clip[i] =
				view[0] * tr.viewDef->projectionMatrix[ i + 0 * 4 ] +
				view[1] * tr.viewDef->projectionMatrix[ i + 1 * 4 ] +
				view[2] * tr.viewDef->projectionMatrix[ i + 2 * 4 ] +
				view[3] * tr.viewDef->projectionMatrix[ i + 3 * 4 ];
		}

	}

	ndc[0] = clip[0] / clip[3];
	ndc[1] = clip[1] / clip[3];
	ndc[2] = ( clip[2] + clip[3] ) / ( 2 * clip[3] );
}

/*
==========================
R_TransformClipToDevice

Clip to normalized device coordinates
==========================
*/
void R_TransformClipToDevice( const idPlane &clip, const viewDef_t *view, idVec3 &normalized ) {
	normalized[0] = clip[0] / clip[3];
	normalized[1] = clip[1] / clip[3];
	normalized[2] = clip[2] / clip[3];
}


/*
==========================
myGlMultMatrix
==========================
*/
void myGlMultMatrix( const float a[16], const float b[16], float out[16] ) {
#if 0
	int		i, j;

	for ( i = 0 ; i < 4 ; i++ ) {
		for ( j = 0 ; j < 4 ; j++ ) {
			out[ i * 4 + j ] =
				a [ i * 4 + 0 ] * b [ 0 * 4 + j ]
				+ a [ i * 4 + 1 ] * b [ 1 * 4 + j ]
				+ a [ i * 4 + 2 ] * b [ 2 * 4 + j ]
				+ a [ i * 4 + 3 ] * b [ 3 * 4 + j ];
		}
	}
#else
	out[0*4+0] = a[0*4+0]*b[0*4+0] + a[0*4+1]*b[1*4+0] + a[0*4+2]*b[2*4+0] + a[0*4+3]*b[3*4+0];
	out[0*4+1] = a[0*4+0]*b[0*4+1] + a[0*4+1]*b[1*4+1] + a[0*4+2]*b[2*4+1] + a[0*4+3]*b[3*4+1];
	out[0*4+2] = a[0*4+0]*b[0*4+2] + a[0*4+1]*b[1*4+2] + a[0*4+2]*b[2*4+2] + a[0*4+3]*b[3*4+2];
	out[0*4+3] = a[0*4+0]*b[0*4+3] + a[0*4+1]*b[1*4+3] + a[0*4+2]*b[2*4+3] + a[0*4+3]*b[3*4+3];
	out[1*4+0] = a[1*4+0]*b[0*4+0] + a[1*4+1]*b[1*4+0] + a[1*4+2]*b[2*4+0] + a[1*4+3]*b[3*4+0];
	out[1*4+1] = a[1*4+0]*b[0*4+1] + a[1*4+1]*b[1*4+1] + a[1*4+2]*b[2*4+1] + a[1*4+3]*b[3*4+1];
	out[1*4+2] = a[1*4+0]*b[0*4+2] + a[1*4+1]*b[1*4+2] + a[1*4+2]*b[2*4+2] + a[1*4+3]*b[3*4+2];
	out[1*4+3] = a[1*4+0]*b[0*4+3] + a[1*4+1]*b[1*4+3] + a[1*4+2]*b[2*4+3] + a[1*4+3]*b[3*4+3];
	out[2*4+0] = a[2*4+0]*b[0*4+0] + a[2*4+1]*b[1*4+0] + a[2*4+2]*b[2*4+0] + a[2*4+3]*b[3*4+0];
	out[2*4+1] = a[2*4+0]*b[0*4+1] + a[2*4+1]*b[1*4+1] + a[2*4+2]*b[2*4+1] + a[2*4+3]*b[3*4+1];
	out[2*4+2] = a[2*4+0]*b[0*4+2] + a[2*4+1]*b[1*4+2] + a[2*4+2]*b[2*4+2] + a[2*4+3]*b[3*4+2];
	out[2*4+3] = a[2*4+0]*b[0*4+3] + a[2*4+1]*b[1*4+3] + a[2*4+2]*b[2*4+3] + a[2*4+3]*b[3*4+3];
	out[3*4+0] = a[3*4+0]*b[0*4+0] + a[3*4+1]*b[1*4+0] + a[3*4+2]*b[2*4+0] + a[3*4+3]*b[3*4+0];
	out[3*4+1] = a[3*4+0]*b[0*4+1] + a[3*4+1]*b[1*4+1] + a[3*4+2]*b[2*4+1] + a[3*4+3]*b[3*4+1];
	out[3*4+2] = a[3*4+0]*b[0*4+2] + a[3*4+1]*b[1*4+2] + a[3*4+2]*b[2*4+2] + a[3*4+3]*b[3*4+2];
	out[3*4+3] = a[3*4+0]*b[0*4+3] + a[3*4+1]*b[1*4+3] + a[3*4+2]*b[2*4+3] + a[3*4+3]*b[3*4+3];
#endif
}

/*
================
R_TransposeGLMatrix
================
*/
void R_TransposeGLMatrix( const float in[16], float out[16] ) {
	int		i, j;

	for ( i = 0 ; i < 4 ; i++ ) {
		for ( j = 0 ; j < 4 ; j++ ) {
			out[i*4+j] = in[j*4+i];
		}
	}
}

/*
=================
R_SetViewMatrix

Sets up the world to view matrix for a given viewParm
=================
*/
void R_SetViewMatrix( viewDef_t *viewDef ) {
	idVec3	origin;
	viewEntity_t *world;
	float	viewerMatrix[16];
	static float	s_flipMatrix[16] = {
		// convert from our coordinate system (looking down X)
		// to OpenGL's coordinate system (looking down -Z)
		0, 0, -1, 0,
		-1, 0, 0, 0,
		0, 1, 0, 0,
		0, 0, 0, 1
	};

	world = &viewDef->worldSpace;

	memset( world, 0, sizeof(*world) );

	// the model matrix is an identity
	world->modelMatrix[0*4+0] = 1;
	world->modelMatrix[1*4+1] = 1;
	world->modelMatrix[2*4+2] = 1;

	// transform by the camera placement
	origin = viewDef->renderView.vieworg;

	viewerMatrix[0] = viewDef->renderView.viewaxis[0][0];
	viewerMatrix[4] = viewDef->renderView.viewaxis[0][1];
	viewerMatrix[8] = viewDef->renderView.viewaxis[0][2];
	viewerMatrix[12] = -origin[0] * viewerMatrix[0] + -origin[1] * viewerMatrix[4] + -origin[2] * viewerMatrix[8];

	viewerMatrix[1] = viewDef->renderView.viewaxis[1][0];
	viewerMatrix[5] = viewDef->renderView.viewaxis[1][1];
	viewerMatrix[9] = viewDef->renderView.viewaxis[1][2];
	viewerMatrix[13] = -origin[0] * viewerMatrix[1] + -origin[1] * viewerMatrix[5] + -origin[2] * viewerMatrix[9];

	viewerMatrix[2] = viewDef->renderView.viewaxis[2][0];
	viewerMatrix[6] = viewDef->renderView.viewaxis[2][1];
	viewerMatrix[10] = viewDef->renderView.viewaxis[2][2];
	viewerMatrix[14] = -origin[0] * viewerMatrix[2] + -origin[1] * viewerMatrix[6] + -origin[2] * viewerMatrix[10];

	viewerMatrix[3] = 0;
	viewerMatrix[7] = 0;
	viewerMatrix[11] = 0;
	viewerMatrix[15] = 1;

	// convert from our coordinate system (looking down X)
	// to OpenGL's coordinate system (looking down -Z)
	myGlMultMatrix( viewerMatrix, s_flipMatrix, world->modelViewMatrix );
}

// Radical-inverse Halton sample in [0,1) — the low-discrepancy sequence FSR2 uses for its
// sub-pixel jitter (docs/fsr-temporal-pipeline.md B). Base 2 on X, base 3 on Y. 1-indexed.
static float R_Halton( int index, int base ) {
	float f = 1.0f, r = 0.0f;
	while ( index > 0 ) {
		f /= (float)base;
		r += f * (float)( index % base );
		index /= base;
	}
	return r;
}

// Native-AA (render == display) jitter phase count; matches ffxFsr2GetJitterPhaseCount at scale 1.0.
static const int R_JITTER_PHASE = 8;

/*
===============
R_SetupProjection

This uses the "infinite far z" trick
===============
*/
void R_SetupProjection( viewDef_t * viewDef ) {
	float	xmin, xmax, ymin, ymax;
	float	width, height;
	float	zNear;
	float	jitterx, jittery;
	static	idRandom random;

	// sub-pixel projection jitter, in PIXELS ([-0.5,0.5] Halton, or [0,1) legacy). Stored on the
	// viewDef for FSR2 (C2); 0 when no jitter is active so projectionMatrix stays un-jittered.
	// The temporal Halton jitter (R1/B) is the FSR2 supersampling input — main fullscreen view
	// only (a jittered subview/probe/screenshot would misalign), indexed per RENDERED frame
	// (tr.frameCount, correct under com_interpolate) so each frame samples a fresh offset.
	// Legacy r_jitter is left untouched as an independent source; both default off -> no jitter.
	float jitterPixX = 0.0f, jitterPixY = 0.0f;
	// The temporal Halton jitter is driven solely by r_fsr (R1/C2, Vulkan): FSR2's
	// supersampling comes from the jittered sample positions, and it is the only
	// consumer that resolves them (the standalone r_temporalJitter toggle from
	// increment B was retired once C2 was verified — alone it just shimmers).
	const bool wantTemporalJitter = r_fsr.GetBool()
		&& rhi::GetActiveBackendType() == rhi::BT_VULKAN
		&& !viewDef->isSubview && !tr.takingEnvProbe && !tr.takingScreenshot;
	if ( wantTemporalJitter ) {
		const int idx = ( tr.frameCount % R_JITTER_PHASE ) + 1;		// Halton is 1-indexed
		jitterPixX = R_Halton( idx, 2 ) - 0.5f;
		jitterPixY = R_Halton( idx, 3 ) - 0.5f;
	} else if ( r_jitter.GetBool() ) {
		// random jittering is usefull when multiple frames are going to be blended together
		// for motion blurred anti-aliasing (order preserved: x then y advances the RNG as before)
		jitterPixX = random.RandomFloat();
		jitterPixY = random.RandomFloat();
	}
	jitterx = jitterPixX;
	jittery = jitterPixY;
	viewDef->jitter[0] = jitterPixX;
	viewDef->jitter[1] = jitterPixY;

	//
	// set up projection matrix
	//
	zNear	= r_znear.GetFloat();
	if ( viewDef->renderView.cramZNear ) {
		zNear *= 0.25;
	}

	ymax = zNear * tan( viewDef->renderView.fov_y * idMath::PI / 360.0f );
	ymin = -ymax;

	xmax = zNear * tan( viewDef->renderView.fov_x * idMath::PI / 360.0f );
	xmin = -xmax;

	width = xmax - xmin;
	height = ymax - ymin;

	jitterx = jitterx * width / ( viewDef->viewport.x2 - viewDef->viewport.x1 + 1 );
	xmin += jitterx;
	xmax += jitterx;
	jittery = jittery * height / ( viewDef->viewport.y2 - viewDef->viewport.y1 + 1 );
	ymin += jittery;
	ymax += jittery;

	viewDef->projectionMatrix[0] = 2 * zNear / width;
	viewDef->projectionMatrix[4] = 0;
	viewDef->projectionMatrix[8] = ( xmax + xmin ) / width;	// normally 0
	viewDef->projectionMatrix[12] = 0;

	viewDef->projectionMatrix[1] = 0;
	viewDef->projectionMatrix[5] = 2 * zNear / height;
	viewDef->projectionMatrix[9] = ( ymax + ymin ) / height;	// normally 0
	viewDef->projectionMatrix[13] = 0;

	// this is the far-plane-at-infinity formulation, and
	// crunches the Z range slightly so w=0 vertexes do not
	// rasterize right at the wraparound point
	viewDef->projectionMatrix[2] = 0;
	viewDef->projectionMatrix[6] = 0;
	viewDef->projectionMatrix[10] = -0.999f;
	viewDef->projectionMatrix[14] = -2.0f * zNear;

	viewDef->projectionMatrix[3] = 0;
	viewDef->projectionMatrix[7] = 0;
	viewDef->projectionMatrix[11] = -1;
	viewDef->projectionMatrix[15] = 0;

	// Un-jittered copy for the temporal consumers (docs/fsr-temporal-pipeline.md A1/B).
	// Only the frustum-shear terms [8]/[9] carry the jitter (this frustum is symmetric, so
	// they are 0 un-jittered); everything else is jitter-independent. Subtracting the jitter
	// back out of xmin/xmax/ymin/ymax is exactly a no-op when r_jitter is off (jitterx/y == 0),
	// so projectionMatrix and unjitteredProjectionMatrix are bit-for-bit identical today.
	memcpy( viewDef->unjitteredProjectionMatrix, viewDef->projectionMatrix, sizeof( viewDef->projectionMatrix ) );
	viewDef->unjitteredProjectionMatrix[8] = ( ( xmax - jitterx ) + ( xmin - jitterx ) ) / width;
	viewDef->unjitteredProjectionMatrix[9] = ( ( ymax - jittery ) + ( ymin - jittery ) ) / height;
}

/*
=================
R_SetupViewFrustum

Setup that culling frustum planes for the current view
FIXME: derive from modelview matrix times projection matrix
=================
*/
//static
void R_SetupViewFrustum( viewDef_t* viewDef ) {
	int		i;
	float	xs, xc;
	float	ang;

	ang = DEG2RAD( viewDef->renderView.fov_x ) * 0.5f;
	idMath::SinCos( ang, xs, xc );

	viewDef->frustum[0] = xs * viewDef->renderView.viewaxis[0] + xc * viewDef->renderView.viewaxis[1];
	viewDef->frustum[1] = xs * viewDef->renderView.viewaxis[0] - xc * viewDef->renderView.viewaxis[1];

	ang = DEG2RAD( viewDef->renderView.fov_y ) * 0.5f;
	idMath::SinCos( ang, xs, xc );

	viewDef->frustum[2] = xs * viewDef->renderView.viewaxis[0] + xc * viewDef->renderView.viewaxis[2];
	viewDef->frustum[3] = xs * viewDef->renderView.viewaxis[0] - xc * viewDef->renderView.viewaxis[2];

	// plane four is the front clipping plane
	viewDef->frustum[4] = /* vec3_origin - */ viewDef->renderView.viewaxis[0];

	for ( i = 0; i < 5; i++ ) {
		// flip direction so positive side faces out (FIXME: globally unify this)
		viewDef->frustum[i] = -viewDef->frustum[i].Normal();
		viewDef->frustum[i][3] = -( viewDef->renderView.vieworg * viewDef->frustum[i].Normal() );
	}

	// eventually, plane five will be the rear clipping plane for fog

	float dNear, dFar, dLeft, dUp;

	dNear = r_znear.GetFloat();
	if ( viewDef->renderView.cramZNear ) {
		dNear *= 0.25f;
	}

	dFar = MAX_WORLD_SIZE;
	dLeft = dFar * tan( DEG2RAD( viewDef->renderView.fov_x * 0.5f ) );
	dUp = dFar * tan( DEG2RAD( viewDef->renderView.fov_y * 0.5f ) );
	viewDef->viewFrustum.SetOrigin( viewDef->renderView.vieworg );
	viewDef->viewFrustum.SetAxis( viewDef->renderView.viewaxis );
	viewDef->viewFrustum.SetSize( dNear, dFar, dLeft, dUp );
}

/*
===================
R_ConstrainViewFrustum
===================
*/
static void R_ConstrainViewFrustum( void ) {
	idBounds bounds;

	// constrain the view frustum to the total bounds of all visible lights and visible entities
	bounds.Clear();
	for ( viewLight_t *vLight = tr.viewDef->viewLights; vLight; vLight = vLight->next ) {
		bounds.AddBounds( vLight->lightDef->frustumTris->bounds );
	}
	for ( viewEntity_t *vEntity = tr.viewDef->viewEntitys; vEntity; vEntity = vEntity->next ) {
		bounds.AddBounds( vEntity->entityDef->referenceBounds );
	}
	tr.viewDef->viewFrustum.ConstrainToBounds( bounds );

	if ( r_useFrustumFarDistance.GetFloat() > 0.0f ) {
		tr.viewDef->viewFrustum.MoveFarDistance( r_useFrustumFarDistance.GetFloat() );
	}
}

/*
==========================================================================================

DRAWSURF SORTING

==========================================================================================
*/


/*
=======================
R_QsortSurfaces

=======================
*/
static int R_QsortSurfaces( const void *a, const void *b ) {
	const drawSurf_t	*ea, *eb;

	ea = *(drawSurf_t **)a;
	eb = *(drawSurf_t **)b;

	if ( ea->sort < eb->sort ) {
		return -1;
	}
	if ( ea->sort > eb->sort ) {
		return 1;
	}
	return 0;
}


/*
=================
R_SortDrawSurfs
=================
*/
static void R_SortDrawSurfs( void ) {
	// sort the drawsurfs by sort type, then orientation, then shader
	qsort( tr.viewDef->drawSurfs, tr.viewDef->numDrawSurfs, sizeof( tr.viewDef->drawSurfs[0] ),
		R_QsortSurfaces );
}



//========================================================================


//==============================================================================



/*
================
R_RenderView

A view may be either the actual camera view,
a mirror / remote location, or a 3D view on a gui surface.

Parms will typically be allocated with R_FrameAlloc
================
*/
void R_RenderView( viewDef_t *parms ) {
	viewDef_t		*oldView;

	if ( parms->renderView.width <= 0 || parms->renderView.height <= 0 ) {
		return;
	}

	tr.viewCount++;

	// save view in case we are a subview
	oldView = tr.viewDef;

	tr.viewDef = parms;

	tr.sortOffset = 0;

	// set the matrix for world space to eye space
	R_SetViewMatrix( tr.viewDef );

	// the four sides of the view frustum are needed
	// for culling and portal visibility
	R_SetupViewFrustum( tr.viewDef );

	// we need to set the projection matrix before doing
	// portal-to-screen scissor box calculations
	R_SetupProjection( tr.viewDef );

	// identify all the visible portalAreas, and the entityDefs and
	// lightDefs that are in them and pass culling.
	static_cast<idRenderWorldLocal *>(parms->renderWorld)->FindViewLightsAndEntities();

	// constrain the view frustum to the view lights and entities
	R_ConstrainViewFrustum();

	// make sure that interactions exist for all light / entity combinations
	// that are visible
	// add any pre-generated light shadows, and calculate the light shader values
	R_AddLightSurfaces();

	// adds ambient surfaces and create any necessary interaction surfaces to add to the light
	// lists
	R_AddModelSurfaces();

	// GPU cull validation (docs/gpu-offload-plan.md): once/sec, frustum-cull an object set on the
	// GPU and diff against R_CullLocalBox. No draw; front-end (tr.viewDef live, frustum constrained,
	// no backend pass open, so DispatchSync is safe). Vulkan only; both self-gate when off.
	//   r_gpuCullTest (3.1): synthetic object set — validates the machinery in isolation.
	//   r_gpuCullLive (3.2): the REAL surfaces R_AddModelSurfaces just recorded — validates on live data.
	R_GpuCullValidate();
	R_GpuCullLive();
	// R2 ray-query world scene + validator (docs/rtx-shadow-roadmap.md): r_rtWorld keeps the
	// persistent static-world AS in sync (lazy build / map change / teardown); r_rtWorldTest
	// is the one-shot GPU-vs-CPU ray diff. Both self-gate; Vulkan + RT hardware only.
	// R3.5 S3/S4: stage GPU-skinned casters for the per-entity animated BLAS cache BEFORE R_RtWorldUpdate
	// (whose UpdateTlas reserves TLAS capacity for them and defers the build to RefreshAnimBlas). Runs
	// every frame so the cache retires when off; gated by r_rtAnimBlas.
	R_RtStageAnimCasters();
	R_RtWorldUpdate();
	R_RtWorldValidate();
	// R3.5 animated-BLAS validator (docs/rtx-animated-blas.md S2): one-shot diff of a monster's
	// gpuSkinVB-fed BLAS (build + refit) vs a CPU trace of the same buffers. Self-gates; VK + RT only.
	R_RtAnimBlasValidate();
	// RT reflections RR1 (docs/rtx-reflections.md): one-shot diff of the reflection-hit attribute fetch
	// (gpuSkinVB normal via the geometry table + buffer_reference) vs a CPU barycentric reference.
	R_RtReflValidate();

	// any viewLight that didn't have visible surfaces can have it's shadows removed
	R_RemoveUnecessaryViewLights();

	// sort all the ambient surfaces for translucency ordering
	R_SortDrawSurfs();

	// generate any subviews (mirrors, cameras, etc) before adding this view
	if ( R_GenerateSubViews() ) {
		// if we are debugging subviews, allow the skipping of the
		// main view draw
		if ( r_subviewOnly.GetBool() ) {
			return;
		}
	}

	// write everything needed to the demo file
	if ( session->writeDemo ) {
		static_cast<idRenderWorldLocal *>(parms->renderWorld)->WriteVisibleDefs( tr.viewDef );
	}

	// add the rendering commands for this viewDef
	R_AddDrawViewCmd( parms );

	// restore view in case we are a subview
	tr.viewDef = oldView;
}
