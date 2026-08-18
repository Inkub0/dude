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

r_rtWorld: build and KEEP the static-world acceleration structure (one BLAS per _areaN model,
opaque surfaces only, identity-instance TLAS) - the scene R3's per-frame shadow rays will
consume. Built lazily on the first primary view (a full map is ~50-65 ms, load-time class),
rebuilt automatically on map change, torn down when switched off. The backend owns the AS
objects, so a vid_restart self-heals: Shutdown frees the scene, GetTlasAddress() reads 0
here, and the next primary view rebuilds. Infrastructure only - no consumer yet.

r_rtWorldTest: one-shot validator (self-clears). Traces an 8x8 ray grid across the CURRENT
view frustum against the world scene - the persistent one when r_rtWorld is live, else a
transient build - and diffs every hit distance against a CPU Moller-Trumbore sweep over the
same triangle soup. A hit/miss flip that a slightly perturbed CPU ray reproduces is bucketed
boundary-FP (edge/crack graze), mirroring the GPU-cull validator; PASS iff 0 genuine.
Vulkan + RT hardware only.
=================
*/
static idCVar r_rtWorld( "r_rtWorld", "0", CVAR_RENDERER | CVAR_BOOL,
	"build + keep the static-world ray-query acceleration structure (Vulkan + RT hardware; prereq for RT shadows)" );
static idCVar r_rtWorldTest( "r_rtWorldTest", "0", CVAR_RENDERER | CVAR_BOOL,
	"validate ray-query BLAS/TLAS over the loaded map's static world vs a CPU ray trace (Vulkan + RT hardware; one-shot, self-clears)" );

struct rtAreaSlice_t { int vertStart, numVerts, idxStart, numIdx; };

// Gather the loaded map's opaque static world triangles (the _areaN models) into a packed
// float3/int soup, indices area-local so each slice feeds CreateBlas directly. Returns false
// with nothing allocated when there is no opaque world geometry; otherwise the caller
// Mem_Free16's pos/idx/slices.
static bool R_RtGatherWorld( float *&pos, int *&idx, rtAreaSlice_t *&slices, int &numSlices,
		int &worldVerts, int &worldIndexes ) {
	pos = NULL; idx = NULL; slices = NULL;
	numSlices = 0; worldVerts = 0; worldIndexes = 0;
	const int numAreas = tr.primaryWorld->NumAreas();
	for ( int a = 0; a < numAreas; a++ ) {
		const idRenderModel *model = renderModelManager->FindModel( va( "_area%i", a ) );
		for ( int s = 0; model && s < model->NumSurfaces(); s++ ) {
			const modelSurface_t *surf = model->Surface( s );
			if ( surf->geometry == NULL || surf->shader == NULL || surf->shader->Coverage() != MC_OPAQUE
				|| surf->geometry->verts == NULL || surf->geometry->indexes == NULL ) {
				continue;
			}
			worldVerts += surf->geometry->numVerts;
			worldIndexes += surf->geometry->numIndexes;
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
			if ( surf->geometry == NULL || surf->shader == NULL || surf->shader->Coverage() != MC_OPAQUE
				|| surf->geometry->verts == NULL || surf->geometry->indexes == NULL ) {
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

// Build one BLAS per slice + the identity-instance TLAS. Returns the TLAS address (0 =
// failure); *blasFail counts slices whose BLAS build failed.
static unsigned long long R_RtBuildWorldScene( rhi::RHI *r, const float *pos, const int *idx,
		const rtAreaSlice_t *slices, int numSlices, int *blasFail ) {
	rhi::RHI::RtInstance *inst = (rhi::RHI::RtInstance *)Mem_Alloc16( numSlices * (int)sizeof( rhi::RHI::RtInstance ) );
	int numInst = 0;
	*blasFail = 0;
	for ( int s = 0; s < numSlices; s++ ) {
		rhi::BlasHandle blas = r->CreateBlas( pos + slices[s].vertStart * 3, slices[s].numVerts,
			3 * (int)sizeof( float ), idx + slices[s].idxStart, slices[s].numIdx );
		if ( blas == 0 ) {
			( *blasFail )++;
			continue;
		}
		rhi::RHI::RtInstance &in = inst[numInst++];
		memset( &in, 0, sizeof( in ) );
		in.transform[0] = 1.0f; in.transform[5] = 1.0f; in.transform[10] = 1.0f;	// identity 3x4
		in.blas = blas;
		in.mask = 0xFF;
	}
	unsigned long long tlasAddr = ( numInst > 0 ) ? r->BuildTlas( inst, numInst ) : 0;
	Mem_Free16( inst );
	return tlasAddr;
}

/*
=================
R_RtWorldUpdate

Keeps the persistent world scene in sync with r_rtWorld: lazy build on the first primary
view, rebuild on map change (mapName comparison), teardown when switched off.
=================
*/
static idStr s_rtWorldMap;
static void R_RtWorldUpdate( void ) {
	rhi::RHI *r = rhi::GetRHI();
	if ( r == NULL ) {
		return;
	}
	if ( !r_rtWorld.GetBool() ) {
		if ( r_rtWorld.IsModified() ) {
			r_rtWorld.ClearModified();
			r->DestroyRtScene();		// switched off: free the scene (no-op when never built)
			s_rtWorldMap.Clear();
		}
		return;
	}
	if ( tr.viewDef == NULL || tr.viewDef->isSubview || tr.primaryWorld == NULL
		|| tr.primaryWorld->NumAreas() <= 0 ) {
		return;
	}
	if ( !r->SupportsRayQuery() ) {
		return;							// stays pending; a backend/hardware change re-evaluates
	}
	if ( r->GetTlasAddress() != 0 && s_rtWorldMap.Icmp( tr.primaryWorld->mapName ) == 0 ) {
		return;							// scene live and current
	}
	r_rtWorld.ClearModified();
	r->DestroyRtScene();

	float *pos; int *idx; rtAreaSlice_t *slices;
	int numSlices, worldVerts, worldIndexes;
	if ( !R_RtGatherWorld( pos, idx, slices, numSlices, worldVerts, worldIndexes ) ) {
		return;
	}
	const int msStart = Sys_Milliseconds();
	int blasFail = 0;
	const unsigned long long tlasAddr = R_RtBuildWorldScene( r, pos, idx, slices, numSlices, &blasFail );
	Mem_Free16( pos ); Mem_Free16( idx ); Mem_Free16( slices );
	if ( tlasAddr == 0 ) {
		common->Warning( "r_rtWorld: world scene build failed (%d BLAS failures) - disabling", blasFail );
		r->DestroyRtScene();
		r_rtWorld.SetBool( false );
		return;
	}
	s_rtWorldMap = tr.primaryWorld->mapName;
	common->Printf( "r_rtWorld: %d BLAS, %d tris, %d ms build%s\n",
		numSlices - blasFail, worldIndexes / 3, Sys_Milliseconds() - msStart,
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

static void R_RtWorldValidate( void ) {
	if ( !r_rtWorldTest.GetBool() ) {
		if ( r_rtWorldTest.IsModified() ) {
			r_rtWorldTest.ClearModified();
		}
		return;
	}
	if ( tr.viewDef == NULL || tr.viewDef->isSubview || tr.primaryWorld == NULL ) {
		return;							// stay armed until a primary in-game view renders
	}
	// consume: run once, flip the cvar back off so a re-enable re-runs
	r_rtWorldTest.SetBool( false );
	r_rtWorldTest.ClearModified();

	rhi::RHI *r = rhi::GetRHI();
	if ( r == NULL || !r->SupportsRayQuery() ) {
		common->Printf( "r_rtWorldTest: unavailable (needs Vulkan + KHR_ray_query hardware)\n" );
		return;
	}
	if ( tr.primaryWorld->NumAreas() <= 0 ) {
		common->Printf( "r_rtWorldTest: no map loaded\n" );
		return;
	}

	float *pos; int *idx; rtAreaSlice_t *slices;
	int numSlices, worldVerts, worldIndexes;
	if ( !R_RtGatherWorld( pos, idx, slices, numSlices, worldVerts, worldIndexes ) ) {
		common->Printf( "r_rtWorldTest: no opaque world geometry found\n" );
		return;
	}

	// scene: reuse the persistent one when live (R_RtWorldUpdate ran just before us, so it is
	// current for this map), else a transient build torn down at the end
	const bool persistent = r_rtWorld.GetBool() && r->GetTlasAddress() != 0;
	int blasFail = 0, msBuild = 0;
	unsigned long long tlasAddr;
	if ( persistent ) {
		tlasAddr = r->GetTlasAddress();
	} else {
		const int msStart = Sys_Milliseconds();
		tlasAddr = R_RtBuildWorldScene( r, pos, idx, slices, numSlices, &blasFail );
		msBuild = Sys_Milliseconds() - msStart;
	}
	if ( tlasAddr == 0 || blasFail > 0 ) {
		common->Printf( "r_rtWorldTest: FAIL (AS build: %d/%d BLAS failed, tlas %s)\n",
			blasFail, numSlices, tlasAddr ? "ok" : "failed" );
		if ( !persistent ) {
			r->DestroyRtScene();
		}
		Mem_Free16( pos ); Mem_Free16( idx ); Mem_Free16( slices );
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
		Mem_Free16( pos ); Mem_Free16( idx ); Mem_Free16( slices );
		return;
	}

	// CPU reference over the identical soup + boundary classification of any disagreement
	int gpuHits = 0, mismatch = 0, boundary = 0, genuine = 0, firstGenuine = -1;
	for ( int i = 0; i < NR; i++ ) {
		float cpuT = -1.0f;
		for ( int sl = 0; sl < numSlices; sl++ ) {
			const float t = R_RtCpuTrace( rv.vieworg, dirs[i], TMAX,
				pos, idx, slices[sl].idxStart, slices[sl].numIdx, slices[sl].vertStart );
			if ( t > 0.0f && ( cpuT < 0.0f || t < cpuT ) ) {
				cpuT = t;
			}
		}
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
			float pT = -1.0f;
			for ( int sl = 0; sl < numSlices; sl++ ) {
				const float t = R_RtCpuTrace( rv.vieworg, pd, TMAX,
					pos, idx, slices[sl].idxStart, slices[sl].numIdx, slices[sl].vertStart );
				if ( t > 0.0f && ( pT < 0.0f || t < pT ) ) {
					pT = t;
				}
			}
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

	common->Printf( "r_rtWorldTest: %d BLAS (%d tris, %s); %d/%d rays hit -- %s "
		"(mismatch %d = %d boundary-FP + %d genuine, first genuine @%d)\n",
		numSlices - blasFail, worldIndexes / 3,
		persistent ? "persistent scene" : va( "%d ms build", msBuild ),
		gpuHits, NR,
		( genuine == 0 && gpuHits > 0 ) ? "PASS" : "FAIL",
		mismatch, boundary, genuine, firstGenuine );

	if ( !persistent ) {
		r->DestroyRtScene();
	}
	Mem_Free16( pos ); Mem_Free16( idx ); Mem_Free16( slices );
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
	R_RtWorldUpdate();
	R_RtWorldValidate();

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
