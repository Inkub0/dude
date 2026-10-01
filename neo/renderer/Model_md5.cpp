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
#include "framework/Session.h"
#include "renderer/tr_local.h"
#include "renderer/rhi/RHI.h"

#include "renderer/Model_local.h"
#include "framework/LoadTiming.h"		// DUDE: md5 load sub-split timers

// FIDELITY NOTE: GPU skinning uses option B (blended-LBS of a stored bind-pose TBN), which
// diverges from stock's per-frame re-derive by a few degrees of surface normal on heavily
// deforming meshes (imperceptible under normal maps in motion; see docs/gpu-offload-plan.md).
// The bind TBN is additionally seam-welded at bake (see BuildGpuSkinData), a second small
// divergence: coincident UV/mirror pairs shade as one instead of stock's two. That is what keeps
// animated meshes from tearing open under PN tessellation, and it was chosen over an exact
// per-frame GPU re-derive on the strength of a side-by-side look, not a metric. That re-derive was
// built, verified against idSIMD_SSE41::DeriveUnsmoothedTangents to 0.006 deg, and REMOVED: being
// stock-exact, it also reproduced stock's refusal to weld dupVerts, so the seams still opened.
// See docs/gpu-offload-plan.md "TBN source: why the faithful one lost" before rebuilding it.
// Vulkan only (the compute lane) — GL3 always keeps the CPU skinner (the r_gpuSkinning term is
// VK-gated everywhere it is read), so this cvar is inert on GL3 regardless of its value.
// DEFAULT ON (Vulkan): the seam-weld work landed and the feature is verified, so GPU skinning is now
// the Vulkan default. It offloads the per-frame skin DRAW to the GPU while the CPU position skin still
// runs (that is the monster hit surface — r_gpuSkinStripCpu, which would have stripped it and left
// monsters invulnerable, was retired 2026-08-18). Deliberately NOT archived: the debug toggle
// (Enhancements tab) flips it per-session for A/B, but it always returns to ON at launch; the bring-up
// device-losses that once pinned it to 0 are long fixed. Global (extern in tr_local.h) so the
// quality-preset system can still force it off — option-B TBN is a fidelity divergence, so the
// faithful-floor tiers (Potato/Low) keep it 0 while every enhanced tier turns it on.
idCVar r_gpuSkinning( "r_gpuSkinning", "1", CVAR_RENDERER | CVAR_BOOL,
	"skin animated (MD5) models on the GPU via the compute lane (Vulkan only, on by default; option-B TBN, docs/gpu-offload-plan.md Phase 2)" );

// Roadmap B ("deform once, draw everywhere", docs/tessellation.md): evaluate PN + displacement once
// per frame in a compute pass into a single expanded buffer every pass draws, instead of re-running
// the fixed-function .tesc/.tese in each depth-EQUAL pass. Vulkan only, opt-in, off = the shipping
// fixed-function tessellation path (fully intact fallback). B-1 wires only the bake + validator; the
// draw substitution is B-2. Archived so a preset can drive it, and rebuilt when r_tessLevel /
// r_tessMinEdge change (they alter the baked topology). NOT enabled per-frame for the validator.
idCVar r_tessDeform( "r_tessDeform", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_ARCHIVE,
	"deform tessellated meshes once/frame in a compute pass instead of per-pass fixed-function tess (Vulkan only; docs/tessellation.md)" );

// GPU MD5 skinning kernel (option B, blended-LBS): one invocation per OUTPUT vertex, walking
// that vertex's run in the EXPANDED weight stream (per-vertex start from wstart, terminator from
// wi[].y). In a single loop it accumulates:
//  * position — the full affine jointMat*scaledWeight, mirroring idJointMat::operator*(idVec4)
//    exactly (row-major 3x4, base = joint*12 floats). Bit-exact vs the CPU TransformVerts.
//    skinScale scales the offset (xyz) not the weight (w), matching TransformScaledVerts.
//  * TBN — proper linear-blend skinning: sum_w weight * (jointRot * jointLocalBindTBN), then
//    normalize. Each expanded weight carries the joint-local N/T0/T1 of the OWNING output vertex
//    (so mirror-seam handedness is preserved). At bind pose this reproduces the bind TBN exactly
//    (weights sum to 1). It is option B — a fidelity divergence from stock's per-frame re-derive
//    — so it is opt-in and off by default.
// The output SSBO is a raw idDrawVert float array (15 floats/vert); the kernel writes only the
// animated fields (xyz, normal, tangents) and leaves st + color as CreateBuffer seeded them.
static const char *MD5_SKIN_SRC =
	"#version 450\n"
	"layout(local_size_x = 64) in;\n"
	"layout(std430, binding = 0) readonly buffer Joints   { float jm[]; };\n"
	"layout(std430, binding = 1) readonly buffer Weights  { vec4 sw[]; };\n"
	"layout(std430, binding = 2) readonly buffer WIndex   { ivec2 wi[]; };\n"
	"layout(std430, binding = 3) readonly buffer WStart   { uint wstart[]; };\n"
	"layout(std430, binding = 4)          buffer OutVert  { float v[]; };\n"
	"layout(std430, binding = 5) readonly buffer LocalTBN { vec4 lt[]; };\n"
	"#ifdef PREVPOS\n"							// skinned motion vectors: last frame's model-space position per vert
	"layout(std430, binding = 6)          buffer PrevPos  { vec4 pp[]; };\n"
	"#endif\n"
	"layout(push_constant) uniform PC { uint numVerts; float skinScale; uint prevValid; } pc;\n"
	"vec3 jrot( uint b, vec3 n ) {\n"		// rotate n by the joint's 3x3 (rows), no translation
	"    return vec3( dot(vec3(jm[b+0u], jm[b+1u], jm[b+2u]),  n),\n"
	"                 dot(vec3(jm[b+4u], jm[b+5u], jm[b+6u]),  n),\n"
	"                 dot(vec3(jm[b+8u], jm[b+9u], jm[b+10u]), n) );\n"
	"}\n"
	"void main() {\n"
	"    uint i = gl_GlobalInvocationID.x;\n"
	"    if ( i >= pc.numVerts ) return;\n"
	"    uint j = wstart[i];\n"
	"    vec3 p = vec3(0.0), nrm = vec3(0.0), t0 = vec3(0.0), t1 = vec3(0.0);\n"
	"    for ( int guard = 0; guard < 256; guard++ ) {\n"
	"        int base = wi[j].x;\n"
	"        vec4 w = sw[j];\n"
	"        vec4 ws = vec4( w.xyz * pc.skinScale, w.w );\n"
	"        p.x += dot( vec4(jm[base+0], jm[base+1], jm[base+2],  jm[base+3]),  ws );\n"
	"        p.y += dot( vec4(jm[base+4], jm[base+5], jm[base+6],  jm[base+7]),  ws );\n"
	"        p.z += dot( vec4(jm[base+8], jm[base+9], jm[base+10], jm[base+11]), ws );\n"
	"        uint b = uint(base);\n"
	"        nrm += w.w * jrot( b, lt[j*3u+0u].xyz );\n"
	"        t0  += w.w * jrot( b, lt[j*3u+1u].xyz );\n"
	"        t1  += w.w * jrot( b, lt[j*3u+2u].xyz );\n"
	"        if ( wi[j].y == 1 ) break;\n"
	"        j++;\n"
	"    }\n"
	"    nrm = normalize( nrm );  t0 = normalize( t0 );  t1 = normalize( t1 );\n"
	"    uint o = i*15u;\n"
	"#ifdef PREVPOS\n"							// what is in the output buffer right now IS last frame's pose
	"    pp[i] = ( pc.prevValid != 0u ) ? vec4( v[o+0u], v[o+1u], v[o+2u], 1.0 ) : vec4( p, 1.0 );\n"
	"#endif\n"
	"    v[o+0u]=p.x;   v[o+1u]=p.y;   v[o+2u]=p.z;\n"		// st = v[o+3..4] (seeded, untouched)
	"    v[o+5u]=nrm.x; v[o+6u]=nrm.y; v[o+7u]=nrm.z;\n"
	"    v[o+8u]=t0.x;  v[o+9u]=t0.y;  v[o+10u]=t0.z;\n"
	"    v[o+11u]=t1.x; v[o+12u]=t1.y; v[o+13u]=t1.z;\n"	// color = v[o+14] (seeded, untouched)
	"}\n";

static rhi::ShaderHandle r_md5SkinShader = 0;		// compiled once, shared by all meshes
static bool r_md5SkinShaderTried = false;
static rhi::ShaderHandle r_md5SkinPrevShader = 0;	// PREVPOS variant (skinned motion vectors): binding 6 must be bound
static bool r_md5SkinPrevShaderTried = false;

idCVar r_skinnedMotionVectors( "r_skinnedMotionVectors", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "GPU-skinned characters write per-BONE motion into the velocity buffer: the skin kernel keeps each vertex's previous-frame position and the normal/velocity prepass projects THAT through the previous frame's matrices, instead of assuming the mesh moved rigidly with its entity. Removes the ghosting / smearing of swinging limbs in everything temporal (RTAO denoiser, FSR2, reflection + haze accumulation). Needs r_gpuSkinning (Vulkan) and a velocity consumer (r_fsr / r_motionVectors); ~16 bytes per skinned vertex" );

// Lazily compile the shared skinning compute shader (once). Returns 0 if there is no compute
// lane (GL3) or it failed to compile — callers then keep the CPU skinner.
static rhi::ShaderHandle R_MD5_SkinShader( rhi::RHI *r ) {
	if ( !r_md5SkinShaderTried ) {
		r_md5SkinShaderTried = true;
		r_md5SkinShader = r->CreateComputeShader( "cs_md5skin", MD5_SKIN_SRC );
		if ( r_md5SkinShader == 0 ) {
			common->Printf( "gpuSkin: no compute lane (GL3, or the kernel failed to compile)\n" );
		}
	}
	return r_md5SkinShader;
}


// R^T * v : transform a bind-pose model-space vector into the joint's local space using the
// inverse of the (orthonormal) bind rotation. idJointMat is row-major 3x4, so R[r][c]=m[r*4+c]
// and (R^T v)[r] = sum_c m[c*4+r] v[c]. The runtime kernel undoes this with the current rotation.
static ID_INLINE idVec3 R_MD5_InvRotate( const float *m, const idVec3 &v ) {
	return idVec3( m[0*4+0]*v[0] + m[1*4+0]*v[1] + m[2*4+0]*v[2],
	               m[0*4+1]*v[0] + m[1*4+1]*v[1] + m[2*4+1]*v[2],
	               m[0*4+2]*v[0] + m[1*4+2]*v[1] + m[2*4+2]*v[2] );
}

// The PREVPOS variant of the skin kernel (same source, one #define after the #version line). Separate
// program because a storage binding a shader statically uses must be bound at dispatch.
static rhi::ShaderHandle R_MD5_SkinPrevShader( rhi::RHI *r ) {
	if ( !r_md5SkinPrevShaderTried ) {
		r_md5SkinPrevShaderTried = true;
		idStr src = MD5_SKIN_SRC;
		src.Replace( "#version 450\n", "#version 450\n#define PREVPOS 1\n" );
		r_md5SkinPrevShader = r->CreateComputeShader( "cs_md5skin_prevpos", src.c_str() );
	}
	return r_md5SkinPrevShader;
}

static const char *MD5_SnapshotName = "_MD5_Snapshot_";

/***********************************************************************

	idMD5Mesh

***********************************************************************/

static int c_numVerts = 0;
static int c_numWeights = 0;
static int c_numWeightJoints = 0;

typedef struct vertexWeight_s {
	int							vert;
	int							joint;
	idVec3						offset;
	float						jointWeight;
} vertexWeight_t;

/*
====================
idMD5Mesh::idMD5Mesh
====================
*/
idMD5Mesh::idMD5Mesh() {
	scaledWeights	= NULL;
	weightIndex		= NULL;
	shader			= NULL;
	numTris			= 0;
	deformInfo		= NULL;
	surfaceNum		= 0;
	numOutputVerts		= 0;
	skinExpandCount		= 0;
	skinWeightStart		= NULL;
	skinExpandWeights	= NULL;
	skinExpandWDesc		= NULL;
	skinExpandLocalTBN	= NULL;
	skinTemplate		= NULL;
	skinGpuWeights		= 0;
	skinGpuWDesc		= 0;
	skinGpuWStart		= 0;
	skinGpuLocalTBN		= 0;
	numTessOutVerts		= 0;
	numTessOutTris		= 0;
	tessBakeLevel		= 0;
	tessBakeMinEdge		= -1.0f;
	tessBarySeam		= NULL;
	tessSrcTri			= NULL;
	tessHeight			= NULL;
	tessExpandIndexes	= NULL;
	tessGpuBarySeam		= 0;
	tessGpuSrcTri		= 0;
	tessGpuHeight		= 0;
}

/*
====================
idMD5Mesh::~idMD5Mesh
====================
*/
idMD5Mesh::~idMD5Mesh() {
	Mem_Free16( scaledWeights );
	Mem_Free16( weightIndex );
	FreeGpuSkinData();
	FreeTessData();
	if ( deformInfo ) {
		R_FreeDeformInfo( deformInfo );
		deformInfo = NULL;
	}
}

/*
====================
idMD5Mesh::ParseMesh
====================
*/
void idMD5Mesh::ParseMesh( idLexer &parser, int numJoints, const idJointMat *joints ) {
	idToken		token;
	idToken		name;
	int			num;
	int			count;
	int			jointnum;
	idStr		shaderName;
	int			i, j;
	idList<int>	tris;
	idList<int>	firstWeightForVertex;
	idList<int>	numWeightsForVertex;
	int			maxweight;
	idList<vertexWeight_t> tempWeights;

	parser.ExpectTokenString( "{" );

	//
	// parse name
	//
	if ( parser.CheckTokenString( "name" ) ) {
		parser.ReadToken( &name );
	}

	//
	// parse shader
	//
	parser.ExpectTokenString( "shader" );

	parser.ReadToken( &token );
	shaderName = token;

	shader = declManager->FindMaterial( shaderName );

	//
	// parse texture coordinates
	//
	parser.ExpectTokenString( "numverts" );
	count = parser.ParseInt();
	if ( count < 0 ) {
		parser.Error( "Invalid size: %s", token.c_str() );
	}

	texCoords.SetNum( count );
	firstWeightForVertex.SetNum( count );
	numWeightsForVertex.SetNum( count );

	numWeights = 0;
	maxweight = 0;
	for( i = 0; i < texCoords.Num(); i++ ) {
		parser.ExpectTokenString( "vert" );
		parser.ParseInt();

		parser.Parse1DMatrix( 2, texCoords[ i ].ToFloatPtr() );

		firstWeightForVertex[ i ]	= parser.ParseInt();
		numWeightsForVertex[ i ]	= parser.ParseInt();

		if ( !numWeightsForVertex[ i ] ) {
			parser.Error( "Vertex without any joint weights." );
		}

		numWeights += numWeightsForVertex[ i ];
		if ( numWeightsForVertex[ i ] + firstWeightForVertex[ i ] > maxweight ) {
			maxweight = numWeightsForVertex[ i ] + firstWeightForVertex[ i ];
		}
	}

	//
	// parse tris
	//
	parser.ExpectTokenString( "numtris" );
	count = parser.ParseInt();
	if ( count < 0 ) {
		parser.Error( "Invalid size: %d", count );
	}

	tris.SetNum( count * 3 );
	numTris = count;
	for( i = 0; i < count; i++ ) {
		parser.ExpectTokenString( "tri" );
		parser.ParseInt();

		tris[ i * 3 + 0 ] = parser.ParseInt();
		tris[ i * 3 + 1 ] = parser.ParseInt();
		tris[ i * 3 + 2 ] = parser.ParseInt();
	}

	//
	// parse weights
	//
	parser.ExpectTokenString( "numweights" );
	count = parser.ParseInt();
	if ( count < 0 ) {
		parser.Error( "Invalid size: %d", count );
	}

	if ( maxweight > count ) {
		parser.Warning( "Vertices reference out of range weights in model (%d of %d weights).", maxweight, count );
	}

	tempWeights.SetNum( count );

	for( i = 0; i < count; i++ ) {
		parser.ExpectTokenString( "weight" );
		parser.ParseInt();

		jointnum = parser.ParseInt();
		if ( ( jointnum < 0 ) || ( jointnum >= numJoints ) ) {
			parser.Error( "Joint Index out of range(%d): %d", numJoints, jointnum );
		}

		tempWeights[ i ].joint			= jointnum;
		tempWeights[ i ].jointWeight	= parser.ParseFloat();

		parser.Parse1DMatrix( 3, tempWeights[ i ].offset.ToFloatPtr() );
	}

	// create pre-scaled weights and an index for the vertex/joint lookup
	scaledWeights = (idVec4 *) Mem_Alloc16( numWeights * sizeof( scaledWeights[0] ) );
	weightIndex = (int *) Mem_Alloc16( numWeights * 2 * sizeof( weightIndex[0] ) );
	memset( weightIndex, 0, numWeights * 2 * sizeof( weightIndex[0] ) );

	count = 0;
	for( i = 0; i < texCoords.Num(); i++ ) {
		num = firstWeightForVertex[i];
		for( j = 0; j < numWeightsForVertex[i]; j++, num++, count++ ) {
			scaledWeights[count].ToVec3() = tempWeights[num].offset * tempWeights[num].jointWeight;
			scaledWeights[count].w = tempWeights[num].jointWeight;
			weightIndex[count * 2 + 0] = tempWeights[num].joint * sizeof( idJointMat );
		}
		weightIndex[count * 2 - 1] = 1;
	}

	tempWeights.Clear();
	numWeightsForVertex.Clear();
	firstWeightForVertex.Clear();

	parser.ExpectTokenString( "}" );

	// update counters
	c_numVerts += texCoords.Num();
	c_numWeights += numWeights;
	c_numWeightJoints++;
	for ( i = 0; i < numWeights; i++ ) {
		c_numWeightJoints += weightIndex[i*2+1];
	}

	//
	// build the information that will be common to all animations of this mesh:
	// silhouette edge connectivity and normal / tangent generation information
	//
	bool onStack;
	idDrawVert *verts = (idDrawVert*)Mem_MallocA( texCoords.Num()*sizeof(idDrawVert), onStack );

	for ( i = 0; i < texCoords.Num(); i++ ) {
		verts[i].Clear();
		verts[i].st = texCoords[i];
	}
	TransformVerts( verts, joints );
	{
		// DUDE: time the deform-template build -- the sil-edge / mirror / dup-vert pass over the
		// bind pose that an MD5 binary cache would skip
		idLoadTimingScope deformScope( loadTiming.md5DeformInfoMs, loadTiming.md5DeformInfoCount );
		deformInfo = R_BuildDeformInfo( texCoords.Num(), verts, tris.Num(), tris.Ptr(), shader->UseUnsmoothedTangents() );
	}
	Mem_FreeA( verts, onStack );

	// Phase 2 (docs/gpu-offload-plan.md): retain the bind-pose data the GPU skinning kernel
	// needs but the stock CPU path discards. Vulkan only (the sole backend with a compute
	// lane); GL3 never pays the memory. Uses the just-built deformInfo + the bind joints.
	if ( rhi::GetActiveBackendType() == rhi::BT_VULKAN ) {
		{
			idLoadTimingScope skinScope( loadTiming.md5GpuSkinMs, loadTiming.md5GpuSkinCount );	// DUDE: md5 split
			BuildGpuSkinData( joints );
		}
		// Roadmap B: bake the uniform-level deform topology when the feature is on. Needs the bind
		// pose here (BuildTessTopology is pose-invariant but reads bind-pose st/seam).
		if ( r_tessDeform.GetBool() ) {
			idLoadTimingScope tessScope( loadTiming.md5TessMs, loadTiming.md5TessCount );	// DUDE: md5 split
			BuildTessTopology( joints );
		}
	}
}

/*
====================
idMD5Mesh::StampTessSeamMask

DUDE tessellation (docs/tessellation.md): stamp the per-vertex UV-seam displacement mask
into color[3], where the tessellation vertex stages pick it up as var_ModelNormal.w.

dudeTessDisplace reads its height from the bump map AT THE VERTEX UV. A UV seam is by
definition a pair of position-coincident verts carrying DIFFERENT texcoords, so the two
halves sample different texels, displace by different amounts, and pull apart -- the gaps
that show around hands and shoulders, where the UV charts are cut and stretched most.
Welding the bind normals (BuildGpuSkinData) made both halves move in the same DIRECTION;
only pinning them equalises the DISTANCE. So: 0 on every vert of a dupVerts pair, 255
everywhere else, and the tese interpolates between them so displacement ramps back to full
one triangle in from the seam instead of stepping.

Mirror-seam verts need nothing: UpdateSurface / BuildGpuSkinData replicate them by copying
the whole source vertex, texcoord included, so a mirror pair samples the same texel already.
That was always the intent, but until 2026-09-25 the code pinned them anyway, because dupVerts
is NOT just the UV splits: R_DuplicateMirroredVertexes leaves silIndexes pointing at the
original ("Silindexes are unchanged by this", tr_trisurf.cpp), so the R_CreateDupVerts pass
that follows records every mirror COPY as a dupVert paired with its original -- and the stamp
zeroed both. Measured cost: 100% of the mirror verts on betruger / playerhead / scihead02, one
exactly-coplanar curve (y-spread 0.000 on betruger) running crown to chin over the head's full
extent, held at zero displacement while the surface around it displaced. So pin on the real
condition instead -- the two coincident verts disagree on UV -- rather than on dupVerts
membership. Bit-identical texcoords mean the identical texel and so the identical height; such
a pair cannot diverge, and pinning it only costs relief.

FIDELITY NOTE: md5 verts are Clear()ed to color 0, so vertex colour on a base md5 surface
would render black (SVC_MODULATE) -- and sure enough nothing uses it: every vertexColor /
inverseVertexColor stage in the stock materials is either world/terrain blending, a projected
DECAL_MACRO, or an additive spawn effect (alpha ignored), never an md5 body/head skin. The
alpha byte is therefore free real estate here; RGB is left untouched, so even a mod material
reading vertexColor sees the same rgb it does today. The real cost is reduced relief in a band
around each UV seam, which on the imp is still 44% of the verts -- not narrow. Kept unconditional
anyway: a partial pin leaves a proportionally smaller gap, which is still a gap.

Measured effect of the UV-difference rule (share of source verts pinned, before -> after):
imp 49.5% -> 44.4%, betruger head 16.9% -> 5.3%, player head 13.6% -> 0%, scientist head02
40.4% -> 0%. Bodies barely move because their pinning really is UV charts; HEADS go to zero,
because on a head essentially every dupVerts pair was the mirror centreline. That matters more
since r_tessMinEdge dropped to 0.1 and faces tessellate ~100% instead of ~52%.
====================
*/
void idMD5Mesh::StampTessSeamMask( idDrawVert *verts ) const {
	for ( int i = 0; i < deformInfo->numSourceVerts; i++ ) {
		verts[i].color[3] = 255;
	}
	const int mirBase = deformInfo->numOutputVerts - deformInfo->numMirroredVerts;
	for ( int i = 0; i < deformInfo->numDupVerts; i++ ) {
		const int a = deformInfo->dupVerts[i * 2 + 0];
		const int b = deformInfo->dupVerts[i * 2 + 1];
		// A mirror copy has no st of its own yet -- the replication that fills it runs after this
		// stamp -- but by construction it is a verbatim copy of its source, so resolve through
		// mirroredVerts to compare the texcoord it WILL carry.
		const int sa = ( a >= mirBase && deformInfo->mirroredVerts ) ? deformInfo->mirroredVerts[a - mirBase] : a;
		const int sb = ( b >= mirBase && deformInfo->mirroredVerts ) ? deformInfo->mirroredVerts[b - mirBase] : b;
		if ( verts[sa].st == verts[sb].st ) {
			continue;		// same texel => same height => this pair cannot pull apart
		}
		verts[a].color[3] = 0;
		verts[b].color[3] = 0;
	}
}

/*
====================
idMD5Mesh::BuildGpuSkinData

Precompute, once at load, everything the blended-LBS skinning kernel reads that stock frees.
Builds an EXPANDED weight stream: every output vertex (source verts, then appended mirror-seam
duplicates) gets its own contiguous run of weights, each entry carrying the joint index+weight
(replicated from the source vert's run) AND the joint-local bind N/T0/T1 of THAT output vertex.
Replicating per output vertex is what lets mirror verts keep their own tangent handedness while
sharing the source's joints. Uses a bind-pose R_DeriveTangents to get the model-space TBN, then
undoes each contributing joint's bind rotation so the kernel can re-apply the current rotation.
Vulkan only; see the kernel MD5_SKIN_SRC.
====================
*/
static int R_MD5_UFFind( int *parent, int x ) {
	while ( parent[x] != x ) {
		parent[x] = parent[parent[x]];		// path-halving
		x = parent[x];
	}
	return x;
}

void idMD5Mesh::BuildGpuSkinData( const idJointMat *bindJoints ) {
	FreeGpuSkinData();
	if ( !deformInfo || numWeights <= 0 ) {
		return;
	}
	const int numSrc = deformInfo->numSourceVerts;
	const int numOut = deformInfo->numOutputVerts;
	const int numMir = deformInfo->numMirroredVerts;
	const int base   = numOut - numMir;			// R_DuplicateMirroredVertexes appends -> base == numSrc
	if ( numOut <= 0 || base != numSrc ) {
		return;								// unexpected layout; leave CPU skinning as the path
	}
	numOutputVerts = numOut;

	// per-source-vertex run boundaries in the ORIGINAL weight stream (sequential, terminator-flagged)
	bool onStack;
	int *origStart = (int *)Mem_MallocA( numSrc * sizeof( int ), onStack );
	int *origCount = (int *)Mem_MallocA( numSrc * sizeof( int ), onStack );
	{
		int v = 0, runStart = 0;
		for ( int j = 0; j < numWeights && v < numSrc; j++ ) {
			if ( weightIndex[j * 2 + 1] == 1 ) {
				origStart[v] = runStart;
				origCount[v] = j - runStart + 1;
				v++;
				runStart = j + 1;
			}
		}
	}

	// total size of the expanded stream (source verts + a replicated run per mirror vert)
	int expand = 0;
	for ( int o = 0; o < numOut; o++ ) {
		const int src = ( o < numSrc ) ? o : deformInfo->mirroredVerts[o - base];
		expand += origCount[src];
	}
	skinExpandCount = expand;

	skinWeightStart    = (unsigned int *)Mem_Alloc16( numOut * sizeof( unsigned int ) );
	skinExpandWeights  = (idVec4 *)      Mem_Alloc16( expand * sizeof( idVec4 ) );
	skinExpandWDesc    = (int *)         Mem_Alloc16( expand * 2 * sizeof( int ) );
	skinExpandLocalTBN = (idVec4 *)      Mem_Alloc16( expand * 3 * sizeof( idVec4 ) );
	skinTemplate       = (idDrawVert *)  Mem_Alloc16( numOut * sizeof( idDrawVert ) );

	// build the bind-pose OUTPUT mesh with derived tangents exactly as UpdateSurface would, so the
	// per-output-vertex model-space N/T0/T1 match what the runtime CPU path would produce at bind.
	srfTriangles_t *tri = R_AllocStaticTriSurf();
	tri->deformedSurface = true;			// protects the referenced deformInfo arrays from the free
	tri->tangentsCalculated = false;
	tri->numIndexes    = deformInfo->numIndexes;
	tri->indexes       = deformInfo->indexes;
	tri->silIndexes    = deformInfo->silIndexes;
	tri->numMirroredVerts = deformInfo->numMirroredVerts;
	tri->mirroredVerts = deformInfo->mirroredVerts;
	tri->numDupVerts   = deformInfo->numDupVerts;
	tri->dupVerts      = deformInfo->dupVerts;
	tri->numSilEdges   = deformInfo->numSilEdges;
	tri->silEdges      = deformInfo->silEdges;
	tri->dominantTris  = deformInfo->dominantTris;
	tri->numVerts      = numOut;
	R_AllocStaticTriSurfVerts( tri, numOut );
	for ( int i = 0; i < numSrc; i++ ) {
		tri->verts[i].Clear();
		tri->verts[i].st = texCoords[i];
	}
	// before the mirror replication below, so the mirror copies inherit it
	StampTessSeamMask( tri->verts );
	TransformVerts( tri->verts, bindJoints );
	for ( int i = 0; i < numMir; i++ ) {
		tri->verts[base + i] = tri->verts[deformInfo->mirroredVerts[i]];
	}
	R_DeriveTangents( tri );

	// Pre-weld the baked bind-pose normals across every authored coincident group (dupVerts =
	// UV splits, mirroredVerts = mirror seams). Coincident verts share a weight run, so giving them
	// one identical bind normal makes their skinned normals bit-identical at any pose -- the pair
	// physically cannot pull apart under PN tessellation. Deliberately NO angle gate: a dot-product
	// threshold rejects exactly the worst pairs -- the 49.54 deg split measured on the cracking mesh
	// sits just past the 45 deg an 0.7 gate allows. (A gated variant shipped briefly as
	// r_tessWeldSeams and was deleted 2026-09-25, unused.) Union-find because a 3-way coincident
	// group welded pairwise in the wrong order leaves two members disagreeing.
	if ( deformInfo->numDupVerts > 0 || numMir > 0 ) {
		int *parent  = (int *)Mem_Alloc16( numOut * sizeof( int ) );
		idVec3 *sum  = (idVec3 *)Mem_Alloc16( numOut * sizeof( idVec3 ) );
		for ( int i = 0; i < numOut; i++ ) { parent[i] = i; sum[i].Zero(); }

		for ( int i = 0; i < deformInfo->numDupVerts; i++ ) {
			const int a = deformInfo->dupVerts[i * 2 + 0];
			const int b = deformInfo->dupVerts[i * 2 + 1];
			if ( a < numOut && b < numOut ) {
				const int ra = R_MD5_UFFind( parent, a ), rb = R_MD5_UFFind( parent, b );
				if ( ra != rb ) { parent[rb] = ra; }
			}
		}
		for ( int i = 0; i < numMir; i++ ) {
			const int a = base + i;
			const int b = deformInfo->mirroredVerts[i];
			if ( a < numOut && b < numOut ) {
				const int ra = R_MD5_UFFind( parent, a ), rb = R_MD5_UFFind( parent, b );
				if ( ra != rb ) { parent[rb] = ra; }
			}
		}

		for ( int i = 0; i < numOut; i++ ) { sum[R_MD5_UFFind( parent, i )] += tri->verts[i].normal; }
		for ( int i = 0; i < numOut; i++ ) {
			idVec3 n = sum[R_MD5_UFFind( parent, i )];
			// a group whose normals cancel (a fold welded back-to-back) has no meaningful average;
			// leaving those alone is better than handing the kernel a zero/NaN normal
			if ( n.LengthSqr() > 1e-12f ) {
				n.Normalize();
				tri->verts[i].normal = n;
			}
		}
		Mem_Free16( parent );
		Mem_Free16( sum );
	}

	// emit each output vertex's run into the expanded stream
	int e = 0;
	for ( int o = 0; o < numOut; o++ ) {
		skinWeightStart[o] = (unsigned int)e;
		const int src = ( o < numSrc ) ? o : deformInfo->mirroredVerts[o - base];
		const idDrawVert &dv = tri->verts[o];
		const int s = origStart[src];
		const int n = origCount[src];
		for ( int k = 0; k < n; k++ ) {
			const int w = s + k;					// index into the original weight stream
			const int jointIdx = weightIndex[w * 2 + 0] / (int)sizeof( idJointMat );
			skinExpandWeights[e]       = scaledWeights[w];
			skinExpandWDesc[e * 2 + 0] = weightIndex[w * 2 + 0] / 4;				// joint*48 bytes -> joint*12 floats
			skinExpandWDesc[e * 2 + 1] = ( k == n - 1 ) ? 1 : 0;					// terminator at this vert's run end
			const float *m = bindJoints[jointIdx].ToFloatPtr();
			skinExpandLocalTBN[e * 3 + 0].ToVec3() = R_MD5_InvRotate( m, dv.normal );      skinExpandLocalTBN[e * 3 + 0].w = 0.0f;
			skinExpandLocalTBN[e * 3 + 1].ToVec3() = R_MD5_InvRotate( m, dv.tangents[0] ); skinExpandLocalTBN[e * 3 + 1].w = 0.0f;
			skinExpandLocalTBN[e * 3 + 2].ToVec3() = R_MD5_InvRotate( m, dv.tangents[1] ); skinExpandLocalTBN[e * 3 + 2].w = 0.0f;
			e++;
		}
		skinTemplate[o] = dv;					// carries st + color; xyz/normal/tangents re-skinned per frame
	}

	R_FreeStaticTriSurf( tri );				// deformedSurface==true -> shared deformInfo arrays kept
	Mem_FreeA( origStart, onStack );
	Mem_FreeA( origCount, onStack );
}

/*
====================
idMD5Mesh::FreeGpuSkinData
====================
*/
void idMD5Mesh::FreeGpuSkinData( void ) {
	Mem_Free16( skinWeightStart );		skinWeightStart = NULL;
	Mem_Free16( skinExpandWeights );	skinExpandWeights = NULL;
	Mem_Free16( skinExpandWDesc );		skinExpandWDesc = NULL;
	Mem_Free16( skinExpandLocalTBN );	skinExpandLocalTBN = NULL;
	Mem_Free16( skinTemplate );			skinTemplate = NULL;
	rhi::RHI *r = rhi::GetRHI();
	if ( r ) {
		if ( skinGpuWeights )   { r->DestroyBuffer( skinGpuWeights ); }
		if ( skinGpuWDesc )     { r->DestroyBuffer( skinGpuWDesc ); }
		if ( skinGpuWStart )    { r->DestroyBuffer( skinGpuWStart ); }
		if ( skinGpuLocalTBN )  { r->DestroyBuffer( skinGpuLocalTBN ); }
	}
	skinGpuWeights = skinGpuWDesc = skinGpuWStart = skinGpuLocalTBN = 0;
	numOutputVerts = 0;
	skinExpandCount = 0;
}

/*
====================
idMD5Mesh::EnsureSkinBuffersUploaded

Upload the per-mesh static skinning SSBOs (expanded weights/descriptor/starts/local-TBN) to the
GPU once; they never change, so all entities using this model share them. Returns false if the
data isn't built or a buffer couldn't be created (caller falls back to the CPU skinner).
====================
*/
bool idMD5Mesh::EnsureSkinBuffersUploaded( void ) {
	if ( skinGpuWeights ) {
		return true;						// already uploaded
	}
	if ( !skinExpandLocalTBN || skinExpandCount <= 0 || numOutputVerts <= 0 ) {
		return false;
	}
	rhi::RHI *r = rhi::GetRHI();
	if ( !r ) {
		return false;
	}
	const int E = skinExpandCount;
	skinGpuWeights  = r->CreateBuffer( rhi::BU_STORAGE, E * (int)sizeof( idVec4 ), skinExpandWeights );
	skinGpuWDesc    = r->CreateBuffer( rhi::BU_STORAGE, E * 2 * (int)sizeof( int ), skinExpandWDesc );
	skinGpuWStart   = r->CreateBuffer( rhi::BU_STORAGE, numOutputVerts * (int)sizeof( unsigned int ), skinWeightStart );
	skinGpuLocalTBN = r->CreateBuffer( rhi::BU_STORAGE, E * 3 * (int)sizeof( idVec4 ), skinExpandLocalTBN );
	if ( !skinGpuWeights || !skinGpuWDesc || !skinGpuWStart || !skinGpuLocalTBN ) {
		if ( skinGpuWeights )  { r->DestroyBuffer( skinGpuWeights ); }
		if ( skinGpuWDesc )    { r->DestroyBuffer( skinGpuWDesc ); }
		if ( skinGpuWStart )   { r->DestroyBuffer( skinGpuWStart ); }
		if ( skinGpuLocalTBN ) { r->DestroyBuffer( skinGpuLocalTBN ); }
		skinGpuWeights = skinGpuWDesc = skinGpuWStart = skinGpuLocalTBN = 0;
		return false;
	}
	return true;
}

/*
====================
idMD5Mesh::TransformVerts
====================
*/
void idMD5Mesh::TransformVerts( idDrawVert *verts, const idJointMat *entJoints ) {
	SIMDProcessor->TransformVerts( verts, texCoords.Num(), entJoints, scaledWeights, weightIndex, numWeights );
}

/*
====================
idMD5Mesh::TransformScaledVerts

Special transform to make the mesh seem fat or skinny.  May be used for zombie deaths
====================
*/
void idMD5Mesh::TransformScaledVerts( idDrawVert *verts, const idJointMat *entJoints, float scale ) {
	idVec4 *tmpScaledWeights = (idVec4 *) _alloca16( numWeights * sizeof( scaledWeights[0] ) );
	//SIMDProcessor->Mul( tmpScaledWeights[0].ToFloatPtr(), scale, this->scaledWeights[0].ToFloatPtr(), numWeights * 4 );
	// DG: for this effect to work, we must scale x, y, z but not w (when also scaling w it just shrinks)
	//     (still doesn't look right with all monsters, e.g. the fat zombies face looks real weird,
	//      but I don't think that can be fixed here)
	for( int i=0, n=numWeights; i < n; ++i ) {
		const idVec4& sw = this->scaledWeights[i];
		tmpScaledWeights[i].x = sw.x * scale;
		tmpScaledWeights[i].y = sw.y * scale;
		tmpScaledWeights[i].z = sw.z * scale;
		tmpScaledWeights[i].w = sw.w;
	}
	SIMDProcessor->TransformVerts( verts, texCoords.Num(), entJoints, tmpScaledWeights, weightIndex, numWeights );
}

/*
===============================================================================

	Roadmap B: compute "deform once, draw everywhere" tessellation
	(docs/tessellation.md roadmap; Vulkan only, opt-in r_tessDeform).

	Instead of re-running fixed-function PN + displacement in every depth-EQUAL
	pass (.tesc/.tese), a STATIC uniform-level topology is baked once at load and a
	compute kernel evaluates PN + displacement into one expanded buffer every pass
	draws. B-1 (below) is validate-only: bake + a headless GPU-vs-CPU-reference
	check, NO draw wire-up.

	Crack-free by construction: EVERY source triangle subdivides at the same uniform
	level L (equal_spacing), so a shared edge gets L+1 coincident points from both
	sides and PN evaluates the identical cubic there (edge control points depend only
	on the shared endpoints). The per-triangle r_tessMinEdge gate the fixed-function
	path uses is deliberately NOT applied here: it is per-EDGE on the runtime, and any
	per-triangle mix of subdivided/flat neighbours T-junction-cracks on a shared short
	edge. r_tessMinEdge stays a fixed-function-path control; the eye/dense-cluster
	over-inflation it guarded is handled by the classifier's name/path exclusions.

	FIDELITY: uniform integer L == equal_spacing, bit-identical to the shipping
	fractional_odd_spacing only for ODD L (default r_tessLevel 5 is odd). Even L shifts
	the vertex distribution and loses the smooth LOD slide-in (adaptive LOD is out of
	scope for this milestone).

===============================================================================
*/

// The deform compute kernel. One invocation per OUTPUT (generated) vertex. Reads the
// current-pose source verts (binding 0, the CPU-skinned tri->verts or gpuSkinVB, as a raw
// 15-float idDrawVert stream), the baked barycentric coord + seam mask (binding 1), the 3
// source-corner output-vert indices (binding 2), and the CPU-sampled displacement relief
// (binding 3); writes the PN + displaced vertex (binding 4). dudeTessPN + the geometric
// normal + the displacement are ported bit-identically from neo/shaders/tess.glsl so the one
// resulting buffer can feed every depth-EQUAL pass (B-2). st/normal/tangents are the linear
// barycentric interpolation of the source corners (matching interaction.tese's flat interp).
static const char *MD5_TESSDEFORM_SRC =
	"#version 450\n"
	"layout(local_size_x = 64) in;\n"
	"layout(std430, binding = 0) readonly buffer SrcVerts { float sv[]; };\n"		// 15 floats/vert
	"layout(std430, binding = 1) readonly buffer BarySeam { vec4 bs[]; };\n"		// (u,v,w, seam)
	"layout(std430, binding = 2) readonly buffer SrcTri   { int   sti[]; };\n"		// 3 corner indices/vert
	"layout(std430, binding = 3) readonly buffer Height   { float ht[]; };\n"		// relief scalar/vert
	"layout(std430, binding = 4)          buffer OutVert  { float ov[]; };\n"		// 15 floats/vert
	"layout(push_constant) uniform PC { uint numVerts; float dispStrength; } pc;\n"
	"vec3 gPos( int v ) { uint o = uint(v)*15u; return vec3( sv[o],     sv[o+1u],  sv[o+2u]  ); }\n"
	"vec2 gST(  int v ) { uint o = uint(v)*15u; return vec2( sv[o+3u],  sv[o+4u]             ); }\n"
	"vec3 gNrm( int v ) { uint o = uint(v)*15u; return vec3( sv[o+5u],  sv[o+6u],  sv[o+7u]  ); }\n"
	"vec3 gT0(  int v ) { uint o = uint(v)*15u; return vec3( sv[o+8u],  sv[o+9u],  sv[o+10u] ); }\n"
	"vec3 gT1(  int v ) { uint o = uint(v)*15u; return vec3( sv[o+11u], sv[o+12u], sv[o+13u] ); }\n"
	// dudeTessPN, verbatim from tess.glsl (op order preserved for bit-parity)
	"vec3 dudeTessPN( vec3 p0, vec3 p1, vec3 p2, vec3 n0, vec3 n1, vec3 n2, vec3 tc ) {\n"
	"    vec3 b300 = p0; vec3 b030 = p1; vec3 b003 = p2;\n"
	"    vec3 b210 = ( 2.0 * p0 + p1 - dot( p1 - p0, n0 ) * n0 ) / 3.0;\n"
	"    vec3 b120 = ( 2.0 * p1 + p0 - dot( p0 - p1, n1 ) * n1 ) / 3.0;\n"
	"    vec3 b021 = ( 2.0 * p1 + p2 - dot( p2 - p1, n1 ) * n1 ) / 3.0;\n"
	"    vec3 b012 = ( 2.0 * p2 + p1 - dot( p1 - p2, n2 ) * n2 ) / 3.0;\n"
	"    vec3 b102 = ( 2.0 * p2 + p0 - dot( p0 - p2, n2 ) * n2 ) / 3.0;\n"
	"    vec3 b201 = ( 2.0 * p0 + p2 - dot( p2 - p0, n0 ) * n0 ) / 3.0;\n"
	"    vec3 e = ( b210 + b120 + b021 + b012 + b102 + b201 ) / 6.0;\n"
	"    vec3 vmid = ( p0 + p1 + p2 ) / 3.0;\n"
	"    vec3 b111 = e + ( e - vmid ) * 0.5;\n"
	"    float a = tc.x, b = tc.y, c = tc.z;\n"
	"    float a2 = a * a, b2 = b * b, c2 = c * c;\n"
	"    return b300 * ( a2 * a ) + b030 * ( b2 * b ) + b003 * ( c2 * c )\n"
	"         + b210 * ( 3.0 * a2 * b ) + b120 * ( 3.0 * a * b2 )\n"
	"         + b021 * ( 3.0 * b2 * c ) + b012 * ( 3.0 * b * c2 )\n"
	"         + b102 * ( 3.0 * a * c2 ) + b201 * ( 3.0 * a2 * c )\n"
	"         + b111 * ( 6.0 * a * b * c );\n"
	"}\n"
	"void main() {\n"
	"    uint i = gl_GlobalInvocationID.x;\n"
	"    if ( i >= pc.numVerts ) return;\n"
	"    int c0 = sti[i*3u+0u], c1 = sti[i*3u+1u], c2 = sti[i*3u+2u];\n"
	"    vec3 p0 = gPos(c0), p1 = gPos(c1), p2 = gPos(c2);\n"
	"    vec3 rn0 = gNrm(c0), rn1 = gNrm(c1), rn2 = gNrm(c2);\n"
	"    vec3 n0 = normalize(rn0), n1 = normalize(rn1), n2 = normalize(rn2);\n"
	"    vec3 bary = bs[i].xyz;\n"
	"    vec3 pos = dudeTessPN( p0, p1, p2, n0, n1, n2, bary );\n"
	"    vec3 geoN = normalize( n0 * bary.x + n1 * bary.y + n2 * bary.z );\n"
	"    pos += geoN * ( ht[i] * pc.dispStrength );\n"
	"    vec3 ot0 = gT0(c0) * bary.x + gT0(c1) * bary.y + gT0(c2) * bary.z;\n"
	"    vec3 ot1 = gT1(c0) * bary.x + gT1(c1) * bary.y + gT1(c2) * bary.z;\n"
	"    vec2 ost = gST(c0) * bary.x + gST(c1) * bary.y + gST(c2) * bary.z;\n"
	"    uint o = i*15u;\n"
	"    ov[o+0u]=pos.x;  ov[o+1u]=pos.y;  ov[o+2u]=pos.z;\n"
	"    ov[o+3u]=ost.x;  ov[o+4u]=ost.y;\n"
	"    ov[o+5u]=geoN.x; ov[o+6u]=geoN.y; ov[o+7u]=geoN.z;\n"
	"    ov[o+8u]=ot0.x;  ov[o+9u]=ot0.y;  ov[o+10u]=ot0.z;\n"
	"    ov[o+11u]=ot1.x; ov[o+12u]=ot1.y; ov[o+13u]=ot1.z;\n"		// color (float 14) left seeded
	"}\n";

static rhi::ShaderHandle r_md5TessShader = 0;
static bool r_md5TessShaderTried = false;

// Lazily compile the shared deform compute shader (once). 0 if there's no compute lane (GL3).
static rhi::ShaderHandle R_MD5_TessShader( rhi::RHI *r ) {
	if ( !r_md5TessShaderTried ) {
		r_md5TessShaderTried = true;
		r_md5TessShader = r->CreateComputeShader( "cs_md5tessdeform", MD5_TESSDEFORM_SRC );
		if ( r_md5TessShader == 0 ) {
			common->Printf( "tessDeform: no compute lane (GL3, or the kernel failed to compile)\n" );
		}
	}
	return r_md5TessShader;
}

/*
====================
R_MD5_ResetGpuShaders

Drop the once-compiled GPU compute shaders (skin, the PREVPOS skinned-motion-vector
variant, and tess-deform) so they are recompiled against a fresh RHI backend. A full
backend teardown (full vid_restart, or a mod switch via ReloadEngine) destroys the
backend's shader table, leaving these once-cached frontend handles dangling; without
resetting the *Tried guards the gates keep returning dead handles, the skin/tess compute
dispatches against destroyed pipelines, gpuSkinVB is never written, and every MD5 model
draws at bind pose (T-pose) — with skinned motion vectors and GPU tessellation likewise
dead. The backend already freed the modules, so just drop the cached handles; they
recompile lazily on next use against the new backend.
====================
*/
void R_MD5_ResetGpuShaders( void ) {
	r_md5SkinShader = 0;		r_md5SkinShaderTried = false;
	r_md5SkinPrevShader = 0;	r_md5SkinPrevShaderTried = false;
	r_md5TessShader = 0;		r_md5TessShaderTried = false;
}

// Bilinear sample of the SIGNED tess height map (mirrors tess.glsl dudeTessDisplace).
// uv wraps (repeat) to match the material sampler; returns (h - 0.5) with h the red channel,
// BEFORE the seam scale (applied by the caller). pic is RGBA8, row-major.
// MUST stay in lockstep with dudeTessDisplace: this feeds the r_tessDeform pre-deform path,
// which draws into the same depth the fixed-function tess passes seal.
static float R_TessSampleRelief( const byte *pic, int w, int h, float u, float v ) {
	if ( !pic || w <= 0 || h <= 0 ) {
		return 0.0f;
	}
	// repeat wrap into [0,1)
	u = u - idMath::Floor( u );
	v = v - idMath::Floor( v );
	const float fx = u * (float)w - 0.5f;
	const float fy = v * (float)h - 0.5f;
	const int x0 = (int)idMath::Floor( fx );
	const int y0 = (int)idMath::Floor( fy );
	const float tx = fx - (float)x0;
	const float ty = fy - (float)y0;
	const int ix0 = ( ( x0 % w ) + w ) % w;
	const int iy0 = ( ( y0 % h ) + h ) % h;
	const int ix1 = ( ix0 + 1 ) % w;
	const int iy1 = ( iy0 + 1 ) % h;
	const float b00 = pic[( iy0 * w + ix0 ) * 4 + 0] / 255.0f;
	const float b10 = pic[( iy0 * w + ix1 ) * 4 + 0] / 255.0f;
	const float b01 = pic[( iy1 * w + ix0 ) * 4 + 0] / 255.0f;
	const float b11 = pic[( iy1 * w + ix1 ) * 4 + 0] / 255.0f;
	const float hs = ( b00 * ( 1.0f - tx ) + b10 * tx ) * ( 1.0f - ty )
	               + ( b01 * ( 1.0f - tx ) + b11 * tx ) * ty;
	return hs - 0.5f;			// 0.5 = flat; >0 raised, <0 recessed
}

/*
====================
idMD5Mesh::FreeTessData
====================
*/
void idMD5Mesh::FreeTessData( void ) {
	Mem_Free16( tessBarySeam );		tessBarySeam = NULL;
	Mem_Free16( tessSrcTri );		tessSrcTri = NULL;
	Mem_Free16( tessHeight );		tessHeight = NULL;
	Mem_Free16( tessExpandIndexes );	tessExpandIndexes = NULL;
	rhi::RHI *r = rhi::GetRHI();
	if ( r ) {
		if ( tessGpuBarySeam ) { r->DestroyBuffer( tessGpuBarySeam ); }
		if ( tessGpuSrcTri )   { r->DestroyBuffer( tessGpuSrcTri ); }
		if ( tessGpuHeight )   { r->DestroyBuffer( tessGpuHeight ); }
	}
	tessGpuBarySeam = tessGpuSrcTri = tessGpuHeight = 0;
	numTessOutVerts = 0;
	numTessOutTris = 0;
	tessBakeLevel = 0;
	tessBakeMinEdge = -1.0f;
}

/*
====================
idMD5Mesh::BuildTessTopology

Bake the static uniform-level subdivision once at load (Vulkan, r_tessDeform on).
Every source triangle is subdivided at level L into a regular barycentric grid (crack-free by
construction). Per generated vertex: the barycentric coord, the interpolated UV-seam mask, the 3
source-corner OUTPUT-vert indices its PN control net reads at runtime, and a CPU-sampled displacement
relief scalar (bump blue channel at the interpolated bump UV, seam-scaled). Also emits the expanded
32-bit index list (B-2 draw). Uses bind-pose positions only so the topology is pose-invariant.
====================
*/
void idMD5Mesh::BuildTessTopology( const idJointMat *bindJoints ) {
	FreeTessData();
	if ( !deformInfo || deformInfo->numOutputVerts <= 0 || deformInfo->numIndexes <= 0 ) {
		return;
	}
	int L = r_tessLevel.GetInteger();
	if ( L < 1 ) { L = 1; }
	if ( L > 32 ) { L = 32; }				// TESS_MAX_LEVEL, matches the shader clamp
	tessBakeLevel = L;
	tessBakeMinEdge = r_tessMinEdge.GetFloat();		// recorded only; not used to gate (see header note)

	const int numOut = deformInfo->numOutputVerts;
	const int numSrc = deformInfo->numSourceVerts;
	const int numMir = deformInfo->numMirroredVerts;
	const int mbase  = numOut - numMir;
	const int numSrcTris = deformInfo->numIndexes / 3;
	const int vPerTri = ( L + 1 ) * ( L + 2 ) / 2;		// generated verts per source triangle
	const int tPerTri = L * L;							// generated tris per source triangle

	numTessOutVerts = numSrcTris * vPerTri;
	numTessOutTris  = numSrcTris * tPerTri;
	if ( numTessOutVerts <= 0 || numTessOutTris <= 0 ) {
		FreeTessData();
		return;
	}

	tessBarySeam      = (idVec4 *)   Mem_Alloc16( numTessOutVerts * sizeof( idVec4 ) );
	tessSrcTri        = (int *)      Mem_Alloc16( numTessOutVerts * 3 * sizeof( int ) );
	tessHeight        = (float *)    Mem_Alloc16( numTessOutVerts * sizeof( float ) );
	tessExpandIndexes = (glIndex_t *)Mem_Alloc16( numTessOutTris * 3 * sizeof( glIndex_t ) );

	// bind-pose OUTPUT-vertex positions + st + seam mask (positions for nothing runtime, but the st
	// and seam are load-time; positions are only used to satisfy the same layout as UpdateSurface).
	bool onStack;
	idDrawVert *bindVerts = (idDrawVert *)Mem_MallocA( numOut * sizeof( idDrawVert ), onStack );
	for ( int i = 0; i < numSrc; i++ ) {
		bindVerts[i].Clear();
		bindVerts[i].st = texCoords[i];
	}
	StampTessSeamMask( bindVerts );
	TransformVerts( bindVerts, bindJoints );
	for ( int i = 0; i < numMir; i++ ) {
		bindVerts[mbase + i] = bindVerts[deformInfo->mirroredVerts[i]];
	}

	// load the material's bump map pixels once (CPU) for the displacement relief. Absent bump =>
	// relief 0 (no displacement); the material sampler matrix is treated as identity (character
	// bumps have no texture matrix), so the bump UV is the interpolated source st.
	byte *bumpPic = NULL;
	int bumpW = 0, bumpH = 0;
	if ( shader ) {
		for ( int s = 0; s < shader->GetNumStages(); s++ ) {
			const shaderStage_t *st = shader->GetStage( s );
			if ( st && st->lighting == SL_BUMP && st->tessHeightImage ) {
				// DUDE: the SIGNED height field, NOT the normal map -- the same pixels the
				// fixed-function tess passes sample, so this pre-deform path displaces
				// identically (docs/tessellation.md).
				R_TessHeightPixelsForImage( st->tessHeightImage, &bumpPic, &bumpW, &bumpH );
				break;
			}
		}
	}

	int vOut = 0, iOut = 0;
	for ( int t = 0; t < numSrcTris; t++ ) {
		const int i0 = deformInfo->indexes[t * 3 + 0];
		const int i1 = deformInfo->indexes[t * 3 + 1];
		const int i2 = deformInfo->indexes[t * 3 + 2];
		const idVec2 st0 = bindVerts[i0].st, st1 = bindVerts[i1].st, st2 = bindVerts[i2].st;
		const float c0 = bindVerts[i0].color[3] / 255.0f;
		const float c1 = bindVerts[i1].color[3] / 255.0f;
		const float c2 = bindVerts[i2].color[3] / 255.0f;
		const int vBase = vOut;

		// regular barycentric grid, row-major (j = 0..L outer, i = 0..L-j inner), (u,v,w) ->
		// (p0,p1,p2) == (tc.x,tc.y,tc.z). See docs/tessellation.md roadmap.
		for ( int j = 0; j <= L; j++ ) {
			for ( int ii = 0; ii <= L - j; ii++ ) {
				const float u = (float)( L - ii - j ) / (float)L;
				const float v = (float)ii / (float)L;
				const float w = (float)j / (float)L;
				const float seam = c0 * u + c1 * v + c2 * w;
				tessBarySeam[vOut].x = u;
				tessBarySeam[vOut].y = v;
				tessBarySeam[vOut].z = w;
				tessBarySeam[vOut].w = seam;
				tessSrcTri[vOut * 3 + 0] = i0;
				tessSrcTri[vOut * 3 + 1] = i1;
				tessSrcTri[vOut * 3 + 2] = i2;
				// relief at the interpolated bump UV (identity bump matrix), seam-scaled
				const idVec2 uv = st0 * u + st1 * v + st2 * w;
				float relief = R_TessSampleRelief( bumpPic, bumpW, bumpH, uv.x, uv.y );
				relief *= idMath::ClampFloat( 0.0f, 1.0f, seam );
				tessHeight[vOut] = relief;
				vOut++;
			}
		}

		// expanded index list, winding 'cw' to match interaction.tese. idx(i,j) is the O(1)
		// row-major inverse into this triangle's vertex block.
		#define TESS_IDX( ii, jj ) ( vBase + (jj) * ( L + 1 ) - ( (jj) * ( (jj) - 1 ) ) / 2 + (ii) )
		for ( int j = 0; j < L; j++ ) {
			for ( int ii = 0; ii < L - j; ii++ ) {
				const int a = TESS_IDX( ii,     j     );
				const int b = TESS_IDX( ii + 1, j     );
				const int c = TESS_IDX( ii,     j + 1 );
				tessExpandIndexes[iOut++] = (glIndex_t)a;
				tessExpandIndexes[iOut++] = (glIndex_t)b;
				tessExpandIndexes[iOut++] = (glIndex_t)c;
				if ( ii < L - j - 1 ) {
					const int d = TESS_IDX( ii + 1, j + 1 );
					tessExpandIndexes[iOut++] = (glIndex_t)b;
					tessExpandIndexes[iOut++] = (glIndex_t)d;
					tessExpandIndexes[iOut++] = (glIndex_t)c;
				}
			}
		}
		#undef TESS_IDX
	}

	if ( bumpPic ) { R_StaticFree( bumpPic ); }
	Mem_FreeA( bindVerts, onStack );

	// sanity: we filled exactly what we sized
	if ( vOut != numTessOutVerts || iOut != numTessOutTris * 3 ) {
		common->Warning( "BuildTessTopology: count mismatch (v %d/%d, i %d/%d) -- disabling deform for this mesh",
		                 vOut, numTessOutVerts, iOut, numTessOutTris * 3 );
		FreeTessData();
	}
}

/*
====================
idMD5Mesh::EnsureTessBuffersUploaded

Upload the per-mesh static topology SSBOs once (bary+seam, source-corner indices, relief). Shared by
all entities using this model. false if not built or a buffer failed (caller falls back to fixed-func).
====================
*/
bool idMD5Mesh::EnsureTessBuffersUploaded( void ) {
	if ( tessGpuBarySeam ) {
		return true;
	}
	if ( !tessBarySeam || numTessOutVerts <= 0 ) {
		return false;
	}
	rhi::RHI *r = rhi::GetRHI();
	if ( !r ) {
		return false;
	}
	tessGpuBarySeam = r->CreateBuffer( rhi::BU_STORAGE, numTessOutVerts * (int)sizeof( idVec4 ), tessBarySeam );
	tessGpuSrcTri   = r->CreateBuffer( rhi::BU_STORAGE, numTessOutVerts * 3 * (int)sizeof( int ), tessSrcTri );
	tessGpuHeight   = r->CreateBuffer( rhi::BU_STORAGE, numTessOutVerts * (int)sizeof( float ), tessHeight );
	if ( !tessGpuBarySeam || !tessGpuSrcTri || !tessGpuHeight ) {
		if ( tessGpuBarySeam ) { r->DestroyBuffer( tessGpuBarySeam ); }
		if ( tessGpuSrcTri )   { r->DestroyBuffer( tessGpuSrcTri ); }
		if ( tessGpuHeight )   { r->DestroyBuffer( tessGpuHeight ); }
		tessGpuBarySeam = tessGpuSrcTri = tessGpuHeight = 0;
		return false;
	}
	return true;
}

/*
====================
idMD5Mesh::UpdateSurface
====================
*/
void idMD5Mesh::UpdateSurface( const struct renderEntity_s *ent, const idJointMat *entJoints, modelSurface_t *surf ) {
	int i, base;
	srfTriangles_t *tri;

	tr.pc.c_deformedSurfaces++;
	tr.pc.c_deformedVerts += deformInfo->numOutputVerts;
	tr.pc.c_deformedIndexes += deformInfo->numIndexes;

	surf->shader = shader;

	if ( surf->geometry ) {
		// if the number of verts and indexes are the same we can re-use the triangle surface
		// the number of indexes must be the same to assure the correct amount of memory is allocated for the facePlanes
		if ( surf->geometry->numVerts == deformInfo->numOutputVerts && surf->geometry->numIndexes == deformInfo->numIndexes ) {
			R_FreeStaticTriSurfVertexCaches( surf->geometry );
		} else {
			R_FreeStaticTriSurf( surf->geometry );
			surf->geometry = R_AllocStaticTriSurf();
		}
	} else {
		surf->geometry = R_AllocStaticTriSurf();
	}

	tri = surf->geometry;

	// note that some of the data is references, and should not be freed
	tri->deformedSurface = true;
	tri->tangentsCalculated = false;
	tri->facePlanesCalculated = false;

	tri->numIndexes = deformInfo->numIndexes;
	tri->indexes = deformInfo->indexes;
	tri->silIndexes = deformInfo->silIndexes;
	tri->numMirroredVerts = deformInfo->numMirroredVerts;
	tri->mirroredVerts = deformInfo->mirroredVerts;
	tri->numDupVerts = deformInfo->numDupVerts;
	tri->dupVerts = deformInfo->dupVerts;
	tri->numSilEdges = deformInfo->numSilEdges;
	tri->silEdges = deformInfo->silEdges;
	tri->dominantTris = deformInfo->dominantTris;
	tri->numVerts = deformInfo->numOutputVerts;

	if ( tri->verts == NULL ) {
		R_AllocStaticTriSurfVerts( tri, tri->numVerts );
		for ( i = 0; i < deformInfo->numSourceVerts; i++ ) {
			tri->verts[i].Clear();
			tri->verts[i].st = texCoords[i];
		}
		// st + color are seeded once and survive every re-skin (TransformVerts only writes
		// xyz); the mirror replication below copies whole verts, so it inherits the mask
		StampTessSeamMask( tri->verts );
	}

	// The CPU position skin ALWAYS runs, even when the GPU skinner below is active: it writes
	// tri->verts.xyz, which front-end consumers read every frame with no way to see gpuSkinVB (a
	// GPU-side buffer with no CPU mapping):
	//   - light culling      R_CalcInteractionCullBits / R_ClipTriangleToLight (Interaction.cpp)
	//                        -- per interaction, PER LIGHT; a PURE cull (skipping it draws all tris)
	//   - stencil shadows    R_CreateShadowVolume, R_CreateVertexProgramShadowCache
	//                        -- absent for shadow-mapped lights, present for stencil/sun ones
	//   - surface bounds     R_BoundTriSurf, just below
	//   - blood/burn decals  idRenderModelOverlay::AddOverlaySurfacesToModel (reads xyz each frame)
	//
	// CRITICALLY, the position skin can NEVER be stripped: the game traces player hits against these
	// exact verts. A living monster's combat clip model IS its animated render model (idActor builds it
	// from the render-model handle, CONTENTS_RENDERMODEL; player attacks trace MASK_SHOT_RENDERMODEL ->
	// idClip::TraceRenderModel -> ModelTrace -> R_LocalTrace reads tri->verts[].xyz directly). The
	// retired r_gpuSkinStripCpu left them at bind pose, so shots passed through every monster
	// (invulnerable; user-found 2026-08-18). GPU skinning still offloads the DRAW (gpuSkinVB, below) and
	// r_gpuSkinNoUpload still skips the redundant ambient upload -- both leave tri->verts posed. See
	// docs/gpu-offload-plan.md.
	if ( ent->shaderParms[ SHADERPARM_MD5_SKINSCALE ] != 0.0f ) {
		TransformScaledVerts( tri->verts, entJoints, ent->shaderParms[ SHADERPARM_MD5_SKINSCALE ] );
	} else {
		TransformVerts( tri->verts, entJoints );
	}

	// replicate the mirror seam vertexes
	base = deformInfo->numOutputVerts - deformInfo->numMirroredVerts;
	for ( i = 0; i < deformInfo->numMirroredVerts; i++ ) {
		tri->verts[base + i] = tri->verts[deformInfo->mirroredVerts[i]];
	}

	R_BoundTriSurf( tri );

	// If a surface is going to be have a lighting interaction generated, it will also have to call
	// R_DeriveTangents() to get normals, tangents, and face planes.  If it only
	// needs shadows generated, it will only have to generate face planes.  If it only
	// has ambient drawing, or is culled, no additional work will be necessary
	//
	// DUDE tessellation (docs/tessellation.md): the PN + displacement tese reads the
	// per-vertex NORMAL. With deferred tangents, R_DeriveTangents is otherwise put off
	// to R_CreateAmbientCache, which only runs it for materials that ReceivesLighting().
	// An UNLIT character material (a cinematic/ambient skin, or a hair/fx sub-mesh) then
	// reaches the tessellator with the Clear()ed ZERO normal — PN collapses to flat and
	// displacement (along the zero normal) vanishes, so the surface subdivides but looks
	// exactly like the un-tessellated mesh. Derive here, unconditionally, whenever
	// tessellation is active so every tessellated surface has real corner normals no
	// matter its lighting. Cheap (MD5 carries dominantTris -> the unsmoothed path).
	if ( !r_useDeferredTangents.GetBool()
	     || ( r_tessellation.GetBool() && rhi::GetActiveBackendType() == rhi::BT_VULKAN ) ) {
		// set face planes, vertex normals, tangents
		R_DeriveTangents( tri );
	}

	// DUDE tessellation / blood-decal parity (docs/tessellation.md): weld this frame's coincident
	// (UV-split + mirror-seam) vertex NORMALS with the SAME union-find groups the GPU bind weld uses
	// (BuildGpuSkinData, no angle gate). Two reasons: (1) it keeps the mesh's seams from opening under
	// the CPU-deform path; (2) crucially, a blood-overlay decal copies its normals straight off
	// tri->verts (ModelOverlay.cpp), so welding here makes the decal follow the SAME welded PN surface
	// as the GPU-skinned body (whose gpuSkinVB bind normals are welded) instead of clipping through it.
	// Gated on tessellation/deform/skinning so vanilla (all-off) lighting is byte-identical. NORMAL ONLY:
	// the body's gpuSkinVB keeps per-vertex (unwelded) tangents, so welding tangents would re-introduce a
	// mismatch. With gpuSkinning on the body draws gpuSkinVB (not tri->verts), so the weld is a no-op on the
	// drawn body -- it only changes the decal copy and the (unused) ambient cache. With plain tessellation
	// (r_tessellation on, gpuSkinning off -- the shipped High+/Ultra preset path) the body instead draws
	// tri->verts via the ambient cache, so this weld is exactly what closes the arm/shoulder seams. WITHOUT
	// it, unsmoothed-tangent skins (labcoat/hazmat/marine/imp/... -- 39 of 52 stock character materials)
	// reach the tessellator with disagreeing coincident normals -- R_DeriveTangents routes MD5 dominantTris
	// meshes to R_DeriveUnsmoothedTangents, which never welds dupVerts -- and PN pulls the halves apart. The
	// always-on CPU consumers (culling/shadow/bounds) read tri->verts.xyz only, so they are unaffected. Must
	// run after normals exist and before R_CreateAmbientCache; R_DeriveUnsmoothedTangents sets
	// tangentsCalculated, so the downstream ambient-cache derive will NOT re-run and clobber the welded normals.
	if ( r_tessDeform.GetBool()
	     || ( ( r_gpuSkinning.GetBool() || r_tessellation.GetBool() ) && rhi::GetActiveBackendType() == rhi::BT_VULKAN ) ) {
		// Derive first: this also sets tangentsCalculated so R_CreateAmbientCache below does NOT re-derive
		// and clobber the normals we are about to write. (The decal copies both normal + tangents.)
		if ( !tri->tangentsCalculated ) {
			R_DeriveTangents( tri );
		}
		if ( r_gpuSkinning.GetBool() && skinExpandLocalTBN && skinWeightStart && skinExpandWDesc
		     && skinExpandWeights && skinExpandCount > 0 && numOutputVerts == deformInfo->numOutputVerts ) {
			// EXACT decal-vs-body parity: reconstruct on the CPU the SAME LBS-of-welded-bind normal the
			// GPU skin kernel (MD5_SKIN_SRC) writes into gpuSkinVB, and store it in tri->verts.normal --
			// the source the blood decal copies (ModelOverlay.cpp). The geometry-derived R_DeriveTangents
			// normal drifts a few degrees from the LBS normal on heavily-deforming multi-joint regions
			// (shoulders/hands), and dudeTessPN bends the surface using ONLY corner normals, so that drift
			// pushed the decal's PN silhouette off the body's and it clipped through. Copying the identical
			// LBS normal makes the decal ride the exact body surface (the union-find weld below only killed
			// the coincident-seam-split share; this closes the residual method mismatch). No-op on the drawn
			// body (it rasterizes gpuSkinVB, not tri->verts) and on the always-on CPU consumers (culling/
			// shadow/bounds read xyz only); only the decal copy + the unused ambient cache change.
			// skinExpandLocalTBN is already welded, so this yields welded normals for free. NORMAL ONLY:
			// PN ignores tangents and the body's gpuSkinVB tangents are unwelded.
			const int numOut = numOutputVerts;
			for ( int i = 0; i < numOut; i++ ) {
				idVec3 nrm( 0.0f, 0.0f, 0.0f );
				for ( unsigned int j = skinWeightStart[i]; j < (unsigned int)skinExpandCount; j++ ) {
					const int jointIdx = skinExpandWDesc[j * 2 + 0] / 12;	// FLOAT base = joint*12 (not the byte joint*48)
					const float w = skinExpandWeights[j].w;
					nrm += w * ( entJoints[jointIdx] * skinExpandLocalTBN[j * 3 + 0].ToVec3() );	// jrot = idJointMat::operator*(vec3)
					if ( skinExpandWDesc[j * 2 + 1] == 1 ) { break; }		// terminator: last weight of this vert's run
				}
				nrm.Normalize();
				tri->verts[i].normal = nrm;
			}
		} else if ( deformInfo->numDupVerts > 0 || deformInfo->numMirroredVerts > 0 ) {
			// CPU-deform path (r_tessDeform without gpuSkinning): no gpuSkinVB to match, so body AND decal
			// both draw the geometry-derived normal -- just weld coincident groups so they agree (kills the
			// seam split). Same union-find groups as the bind weld, no angle gate, 1e-12 cancel guard.
			const int numOut = deformInfo->numOutputVerts;
			const int numMir = deformInfo->numMirroredVerts;
			const int weldBase = numOut - numMir;
			int *parent = (int *)Mem_Alloc16( numOut * sizeof( int ) );
			idVec3 *sum = (idVec3 *)Mem_Alloc16( numOut * sizeof( idVec3 ) );
			for ( int wi = 0; wi < numOut; wi++ ) { parent[wi] = wi; sum[wi].Zero(); }
			for ( int wi = 0; wi < deformInfo->numDupVerts; wi++ ) {
				const int a = deformInfo->dupVerts[wi * 2 + 0];
				const int b = deformInfo->dupVerts[wi * 2 + 1];
				if ( a < numOut && b < numOut ) {
					const int ra = R_MD5_UFFind( parent, a ), rb = R_MD5_UFFind( parent, b );
					if ( ra != rb ) { parent[rb] = ra; }
				}
			}
			for ( int wi = 0; wi < numMir; wi++ ) {
				const int a = weldBase + wi;
				const int b = deformInfo->mirroredVerts[wi];
				if ( a < numOut && b < numOut ) {
					const int ra = R_MD5_UFFind( parent, a ), rb = R_MD5_UFFind( parent, b );
					if ( ra != rb ) { parent[rb] = ra; }
				}
			}
			for ( int wi = 0; wi < numOut; wi++ ) { sum[R_MD5_UFFind( parent, wi )] += tri->verts[wi].normal; }
			for ( int wi = 0; wi < numOut; wi++ ) {
				idVec3 n = sum[R_MD5_UFFind( parent, wi )];
				if ( n.LengthSqr() > 1e-12f ) {		// a fold whose normals cancel: leave it, never hand a NaN
					n.Normalize();
					tri->verts[wi].normal = n;
				}
			}
			Mem_Free16( parent );
			Mem_Free16( sum );
		}
	}

	// Phase 2 GPU skinning (docs/gpu-offload-plan.md): additionally skin this surface on the GPU
	// into a persistent BU_SKIN buffer that the draw passes prefer (RB_RHI_StreamAmbient). The CPU
	// path above stays intact as the safety net (stencil, bounds, fallback). With r_gpuSkinning
	// off, or on GL3, or before the skin data is built, gpuSkinVB stays 0 = no behavior change.
	if ( !r_gpuSkinning.GetBool() && tri->gpuSkinVB ) {
		// The toggle just went off. RB_RHI_StreamAmbient prefers gpuSkinVB whenever it exists, but
		// nothing below refreshes it any more, so leaving it attached would rasterize the frozen
		// last GPU pose forever (the mesh stops animating). Release it here, ahead of
		// R_CreateAmbientCache, so that cache is built (r_gpuSkinNoUpload also keys off gpuSkinVB)
		// and the CPU-skinned verts drive the draw again.
		rhi::GetRHI()->DestroyBuffer( tri->gpuSkinVB );
		tri->gpuSkinVB = 0;
		tri->gpuSkinVerts = 0;
		tri->gpuSkinFrame = -1;
		if ( tri->gpuSkinPrevVB ) { rhi::GetRHI()->DestroyBuffer( tri->gpuSkinPrevVB ); }
		tri->gpuSkinPrevVB = 0;	tri->gpuSkinPrevAddr = 0;	tri->gpuSkinPrevValid = false;
	}

	if ( r_gpuSkinning.GetBool() && rhi::GetActiveBackendType() == rhi::BT_VULKAN
	     && skinExpandLocalTBN && R_MD5_SkinShader( rhi::GetRHI() ) != 0 && EnsureSkinBuffersUploaded() ) {
		rhi::RHI *r = rhi::GetRHI();
		const rhi::ShaderHandle skinShader = r_md5SkinShader;
		const int numOut = numOutputVerts;
		// (re)allocate the per-surface output buffer if missing or resized
		if ( tri->gpuSkinVB && tri->gpuSkinVerts != numOut ) {
			r->DestroyBuffer( tri->gpuSkinVB );
			tri->gpuSkinVB = 0;
			if ( tri->gpuSkinPrevVB ) { r->DestroyBuffer( tri->gpuSkinPrevVB ); tri->gpuSkinPrevVB = 0; tri->gpuSkinPrevAddr = 0; }
		}
		if ( !tri->gpuSkinVB ) {
			tri->gpuSkinVB = r->CreateBuffer( rhi::BU_SKIN, numOut * (int)sizeof( idDrawVert ), skinTemplate );
			tri->gpuSkinVerts = numOut;
			tri->gpuSkinFrame = -1;
		}
		// one dispatch per surface per frame (an entity in several views instantiates once)
		// skinned motion vectors: a second per-surface buffer receives last frame's positions (the kernel
		// copies them out of gpuSkinVB before overwriting it). Needs a device address for gbuffer.vert.
		rhi::ShaderHandle jobShader = skinShader;
		if ( r_skinnedMotionVectors.GetBool() && tri->gpuSkinVB && R_MD5_SkinPrevShader( r ) != 0 ) {
			if ( !tri->gpuSkinPrevVB ) {
				tri->gpuSkinPrevVB   = r->CreateBuffer( rhi::BU_STORAGE, numOut * 16, NULL );
				tri->gpuSkinPrevAddr = tri->gpuSkinPrevVB ? r->GetBufferDeviceAddress( tri->gpuSkinPrevVB ) : 0;
				tri->gpuSkinPrevValid = false;
				if ( tri->gpuSkinPrevVB && tri->gpuSkinPrevAddr == 0 ) {		// no buffer-device-address: unusable
					r->DestroyBuffer( tri->gpuSkinPrevVB );
					tri->gpuSkinPrevVB = 0;
				}
			}
			if ( tri->gpuSkinPrevVB ) { jobShader = r_md5SkinPrevShader; }
		} else if ( tri->gpuSkinPrevVB ) {										// toggled off
			r->DestroyBuffer( tri->gpuSkinPrevVB );
			tri->gpuSkinPrevVB = 0;	tri->gpuSkinPrevAddr = 0;	tri->gpuSkinPrevValid = false;
		}
		if ( tri->gpuSkinVB && tri->gpuSkinFrame != tr.frameCount ) {
			// gpuSkinVB holds the PREVIOUS frame's pose only if it was skinned last frame too; after a gap
			// (entity out of view) the kernel stores the current pose instead = zero animation velocity
			const bool prevPoseInVB = ( tri->gpuSkinFrame == tr.frameCount - 1 );
			tri->gpuSkinPrevValid = ( tri->gpuSkinPrevVB != 0 );
			tri->gpuSkinFrame = tr.frameCount;
			const int numJoints = ent->numJoints;
			// Share ONE frame-snapshot of the joint palette across all of this entity's sub-meshes.
			// InstantiateDynamicModel updates a model's meshes contiguously with the same ent->joints,
			// so memoizing the last (source,frame,count) snapshot hands every sub-mesh job the SAME
			// jointData pointer -- RB_RHI_FlushSkinJobs then uploads the palette ONCE per entity instead
			// of once per surface. The snapshot itself is still needed (the backend flush is deferred and
			// ent->joints is game-owned); this only stops copying + uploading it N times. Single-threaded
			// front-end (R_EntityDefDynamicModel), so the static memo is safe; the (ptr,frame,count) key
			// re-snapshots on any entity/frame/joint-count change, and a snapshot is a plain copy so a
			// shared pointer is always the correct palette. INPUT only -- each surface still writes its
			// own gpuSkinVB (the per-surface skinned output a future RTX animated-BLAS path consumes).
			static const idJointMat *s_snapSrc   = NULL;
			static idJointMat       *s_snap      = NULL;
			static int               s_snapFrame = -1;
			static int               s_snapNum   = 0;
			if ( s_snapSrc != entJoints || s_snapFrame != tr.frameCount || s_snapNum != numJoints ) {
				s_snap = (idJointMat *)R_FrameAlloc( numJoints * (int)sizeof( idJointMat ) );
				memcpy( s_snap, entJoints, numJoints * sizeof( idJointMat ) );
				s_snapSrc = entJoints; s_snapFrame = tr.frameCount; s_snapNum = numJoints;
			}
			idJointMat *jointSnap = s_snap;
			const float skinScale = ( ent->shaderParms[ SHADERPARM_MD5_SKINSCALE ] != 0.0f )
			                      ? ent->shaderParms[ SHADERPARM_MD5_SKINSCALE ] : 1.0f;
			RB_RHI_AddSkinJob( jobShader, tri->gpuSkinVB, numOut,
			                   skinGpuWeights, skinGpuWDesc, skinGpuWStart, skinGpuLocalTBN,
			                   jointSnap, numJoints, skinScale,
			                   ( jobShader != skinShader ) ? tri->gpuSkinPrevVB : 0, prevPoseInVB );
		}
	}

	// Roadmap B (docs/tessellation.md): deform this surface once/frame in a compute pass into an
	// expanded buffer every classifier-approved pass draws (RB_RHI_ApplyDeform), retiring the per-pass
	// fixed-function tessellation. Source verts are the current pose: gpuSkinVB when GPU-skinned
	// (fast, GPU-resident, ordered by the skin->deform barrier), else the CPU-skinned tri->verts
	// (needs current-pose tangents; snapshotted to frame memory, uploaded by the flush). The
	// fixed-function tess path stays intact as the fallback (r_tessDeform off / not baked / GL3).
	//
	// GUARDRAIL: deform-once REQUIRES GPU skinning. Without it the source is the CPU-skinned verts,
	// which the flush must upload as a fresh storage buffer (CreateBuffer/DestroyBuffer) per surface
	// EVERY frame — allocation churn that made deform-once ~20x SLOWER than plain fixed-function tess
	// in a multi-NPC scene (measured, Phobos reception). Gating on r_gpuSkinning removes that footgun:
	// with skinning off, no deform buffer is ever built and every pass falls back to fixed-function
	// tessellation (which also has the distance LOD the fixed-baked deform topology lacks). So the two
	// GPU-offload features are wired together — deform-once only ever runs on top of GPU-resident source.
	const bool tessDeformActive = r_tessDeform.GetBool() && r_gpuSkinning.GetBool();
	if ( !tessDeformActive && tri->tessDeformVB ) {
		// toggled off: release so the draw stops binding the frozen last deform and falls back
		rhi::RHI *r = rhi::GetRHI();
		if ( r ) {
			r->DestroyBuffer( tri->tessDeformVB );
			if ( tri->tessDeformIB ) { r->DestroyBuffer( tri->tessDeformIB ); }
		}
		tri->tessDeformVB = tri->tessDeformIB = 0;
		tri->tessDeformVerts = tri->tessDeformIndexes = 0;
		tri->tessDeformFrame = -1;
	}

	if ( tessDeformActive && rhi::GetActiveBackendType() == rhi::BT_VULKAN
	     && tessBarySeam && numTessOutVerts > 0 && R_MD5_TessShader( rhi::GetRHI() ) != 0 && EnsureTessBuffersUploaded() ) {
		rhi::RHI *r = rhi::GetRHI();
		const int numTessV = numTessOutVerts;
		const int numTessIdx = numTessOutTris * 3;
		// (re)allocate the per-surface output VB + IB if missing or resized (e.g. re-baked at another L)
		if ( tri->tessDeformVB && tri->tessDeformVerts != numTessV ) {
			r->DestroyBuffer( tri->tessDeformVB );
			if ( tri->tessDeformIB ) { r->DestroyBuffer( tri->tessDeformIB ); }
			tri->tessDeformVB = tri->tessDeformIB = 0;
		}
		if ( !tri->tessDeformVB ) {
			idDrawVert *seed = (idDrawVert *)Mem_Alloc16( numTessV * sizeof( idDrawVert ) );
			for ( int k = 0; k < numTessV; k++ ) { seed[k].Clear(); }
			tri->tessDeformVB = r->CreateBuffer( rhi::BU_SKIN, numTessV * (int)sizeof( idDrawVert ), seed );
			Mem_Free16( seed );
			tri->tessDeformIB = r->CreateBuffer( rhi::BU_INDEX, numTessIdx * (int)sizeof( glIndex_t ), tessExpandIndexes );
			tri->tessDeformVerts = numTessV;
			tri->tessDeformIndexes = numTessIdx;
			tri->tessDeformFrame = -1;
			if ( !tri->tessDeformVB || !tri->tessDeformIB ) {
				if ( tri->tessDeformVB ) { r->DestroyBuffer( tri->tessDeformVB ); }
				if ( tri->tessDeformIB ) { r->DestroyBuffer( tri->tessDeformIB ); }
				tri->tessDeformVB = tri->tessDeformIB = 0;
				tri->tessDeformVerts = tri->tessDeformIndexes = 0;
			}
		}
		// one deform dispatch per surface per frame
		if ( tri->tessDeformVB && tri->tessDeformFrame != tr.frameCount ) {
			tri->tessDeformFrame = tr.frameCount;
			const int nSrc = deformInfo->numOutputVerts;
			const float disp = r_tessDisplace.GetFloat();
			if ( tri->gpuSkinVB ) {
				// GPU source: read the skinned buffer directly (skin flush + its barrier run first)
				RB_RHI_AddTessJob( r_md5TessShader, tri->gpuSkinVB, NULL, nSrc,
				                   tri->tessDeformVB, tessGpuBarySeam, tessGpuSrcTri, tessGpuHeight, numTessV, disp );
			} else {
				// CPU source: snapshot this frame's deformed verts (with current-pose tangents) to
				// frame memory; the flush uploads it as the source SSBO.
				if ( !tri->tangentsCalculated ) {
					R_DeriveTangents( tri );
				}
				idDrawVert *snap = (idDrawVert *)R_FrameAlloc( nSrc * (int)sizeof( idDrawVert ) );
				memcpy( snap, tri->verts, nSrc * sizeof( idDrawVert ) );
				RB_RHI_AddTessJob( r_md5TessShader, 0, snap, nSrc,
				                   tri->tessDeformVB, tessGpuBarySeam, tessGpuSrcTri, tessGpuHeight, numTessV, disp );
			}
		}
	}

}

/*
====================
idMD5Mesh::CalcBounds
====================
*/
idBounds idMD5Mesh::CalcBounds( const idJointMat *entJoints ) {
	idBounds	bounds;
	bool onStack;
	idDrawVert *verts = (idDrawVert*)Mem_MallocA( texCoords.Num()*sizeof(idDrawVert), onStack );

	TransformVerts( verts, entJoints );

	SIMDProcessor->MinMax( bounds[0], bounds[1], verts, texCoords.Num() );

	Mem_FreeA( verts, onStack );

	return bounds;
}

/*
====================
idMD5Mesh::NearestJoint
====================
*/
int idMD5Mesh::NearestJoint( int a, int b, int c ) const {
	int i, bestJoint, vertNum, weightVertNum;
	float bestWeight;

	// duplicated vertices might not have weights
	if ( a >= 0 && a < texCoords.Num() ) {
		vertNum = a;
	} else if ( b >= 0 && b < texCoords.Num() ) {
		vertNum = b;
	} else if ( c >= 0 && c < texCoords.Num() ) {
		vertNum = c;
	} else {
		// all vertices are duplicates which shouldn't happen
		return 0;
	}

	// find the first weight for this vertex
	weightVertNum = 0;
	for( i = 0; weightVertNum < vertNum; i++ ) {
		weightVertNum += weightIndex[i*2+1];
	}

	// get the joint for the largest weight
	bestWeight = scaledWeights[i].w;
	bestJoint = weightIndex[i*2+0] / sizeof( idJointMat );
	for( ; weightIndex[i*2+1] == 0; i++ ) {
		if ( scaledWeights[i].w > bestWeight ) {
			bestWeight = scaledWeights[i].w;
			bestJoint = weightIndex[i*2+0] / sizeof( idJointMat );
		}
	}
	return bestJoint;
}

/*
====================
idMD5Mesh::NumVerts
====================
*/
int idMD5Mesh::NumVerts( void ) const {
	return texCoords.Num();
}

/*
====================
idMD5Mesh::NumTris
====================
*/
int	idMD5Mesh::NumTris( void ) const {
	return numTris;
}

/*
====================
idMD5Mesh::NumWeights
====================
*/
int	idMD5Mesh::NumWeights( void ) const {
	return numWeights;
}

/***********************************************************************

	idRenderModelMD5

***********************************************************************/

/*
====================
idRenderModelMD5::ParseJoint
====================
*/
void idRenderModelMD5::ParseJoint( idLexer &parser, idMD5Joint *joint, idJointQuat *defaultPose ) {
	idToken	token;
	int		num;

	//
	// parse name
	//
	parser.ReadToken( &token );
	joint->name = token;

	//
	// parse parent
	//
	num = parser.ParseInt();
	if ( num < 0 ) {
		joint->parent = NULL;
	} else {
		if ( num >= joints.Num() - 1 ) {
			parser.Error( "Invalid parent for joint '%s'", joint->name.c_str() );
		}
		joint->parent = &joints[ num ];
	}

	//
	// parse default pose
	//
	parser.Parse1DMatrix( 3, defaultPose->t.ToFloatPtr() );
	parser.Parse1DMatrix( 3, defaultPose->q.ToFloatPtr() );
	defaultPose->q.w = defaultPose->q.CalcW();
}

/*
====================
idRenderModelMD5::InitFromFile
====================
*/
void idRenderModelMD5::InitFromFile( const char *fileName ) {
	name = fileName;
	LoadModel();
}

/*
====================
idRenderModelMD5::LoadModel

used for initial loads, reloadModel, and reloading the data of purged models
Upon exit, the model will absolutely be valid, but possibly as a default model
====================
*/
void idRenderModelMD5::LoadModel() {
	int			version;
	int			i;
	int			num;
	int			parentNum;
	idToken		token;
	idLexer		parser( LEXFL_ALLOWPATHNAMES | LEXFL_NOSTRINGESCAPECHARS );
	idJointQuat	*pose;
	idMD5Joint	*joint;
	idJointMat *poseMat3;

	if ( !purged ) {
		PurgeModel();
	}
	purged = false;

	if ( !parser.LoadFile( name ) ) {
		MakeDefaultModel();
		return;
	}

	parser.ExpectTokenString( MD5_VERSION_STRING );
	version = parser.ParseInt();

	if ( version != MD5_VERSION ) {
		parser.Error( "Invalid version %d.  Should be version %d\n", version, MD5_VERSION );
	}

	//
	// skip commandline
	//
	parser.ExpectTokenString( "commandline" );
	parser.ReadToken( &token );

	// parse num joints
	parser.ExpectTokenString( "numJoints" );
	num  = parser.ParseInt();
	joints.SetGranularity( 1 );
	joints.SetNum( num );
	defaultPose.SetGranularity( 1 );
	defaultPose.SetNum( num );
	poseMat3 = ( idJointMat * )_alloca16( num * sizeof( *poseMat3 ) );

	// parse num meshes
	parser.ExpectTokenString( "numMeshes" );
	num = parser.ParseInt();
	if ( num < 0 ) {
		parser.Error( "Invalid size: %d", num );
	}
	meshes.SetGranularity( 1 );
	meshes.SetNum( num );

	//
	// parse joints
	//
	parser.ExpectTokenString( "joints" );
	parser.ExpectTokenString( "{" );
	pose = defaultPose.Ptr();
	joint = joints.Ptr();
	for( i = 0; i < joints.Num(); i++, joint++, pose++ ) {
		ParseJoint( parser, joint, pose );
		poseMat3[ i ].SetRotation( pose->q.ToMat3() );
		poseMat3[ i ].SetTranslation( pose->t );
		if ( joint->parent ) {
			parentNum = joint->parent - joints.Ptr();
			pose->q = ( poseMat3[ i ].ToMat3() * poseMat3[ parentNum ].ToMat3().Transpose() ).ToQuat();
			pose->t = ( poseMat3[ i ].ToVec3() - poseMat3[ parentNum ].ToVec3() ) * poseMat3[ parentNum ].ToMat3().Transpose();
		}
	}
	parser.ExpectTokenString( "}" );

	for( i = 0; i < meshes.Num(); i++ ) {
		parser.ExpectTokenString( "mesh" );
		meshes[ i ].ParseMesh( parser, defaultPose.Num(), poseMat3 );
	}

	//
	// calculate the bounds of the model
	//
	CalculateBounds( poseMat3 );

	// set the timestamp for reloadmodels
	fileSystem->ReadFile( name, NULL, &timeStamp );
}

/*
==============
idRenderModelMD5::Print
==============
*/
void idRenderModelMD5::Print() const {
	const idMD5Mesh	*mesh;
	int			i;

	common->Printf( "%s\n", name.c_str() );
	common->Printf( "Dynamic model.\n" );
	common->Printf( "Generated smooth normals.\n" );
	common->Printf( "    verts  tris weights material\n" );
	int	totalVerts = 0;
	int	totalTris = 0;
	int	totalWeights = 0;
	for( mesh = meshes.Ptr(), i = 0; i < meshes.Num(); i++, mesh++ ) {
		totalVerts += mesh->NumVerts();
		totalTris += mesh->NumTris();
		totalWeights += mesh->NumWeights();
		common->Printf( "%2i: %5i %5i %7i %s\n", i, mesh->NumVerts(), mesh->NumTris(), mesh->NumWeights(), mesh->shader->GetName() );
	}
	common->Printf( "-----\n" );
	common->Printf( "%4i verts.\n", totalVerts );
	common->Printf( "%4i tris.\n", totalTris );
	common->Printf( "%4i weights.\n", totalWeights );
	common->Printf( "%4i joints.\n", joints.Num() );
}

/*
==============
idRenderModelMD5::List
==============
*/
void idRenderModelMD5::List() const {
	int			i;
	const idMD5Mesh	*mesh;
	int			totalTris = 0;
	int			totalVerts = 0;

	for( mesh = meshes.Ptr(), i = 0; i < meshes.Num(); i++, mesh++ ) {
		totalTris += mesh->numTris;
		totalVerts += mesh->NumVerts();
	}
	common->Printf( " %4ik %3i %4i %4i %s(MD5)", Memory()/1024, meshes.Num(), totalVerts, totalTris, Name() );

	if ( defaulted ) {
		common->Printf( " (DEFAULTED)" );
	}

	common->Printf( "\n" );
}

/*
====================
idRenderModelMD5::CalculateBounds
====================
*/
void idRenderModelMD5::CalculateBounds( const idJointMat *entJoints ) {
	int			i;
	idMD5Mesh	*mesh;

	bounds.Clear();
	for( mesh = meshes.Ptr(), i = 0; i < meshes.Num(); i++, mesh++ ) {
		bounds.AddBounds( mesh->CalcBounds( entJoints ) );
	}
}

/*
====================
idRenderModelMD5::Bounds

This calculates a rough bounds by using the joint radii without
transforming all the points
====================
*/
idBounds idRenderModelMD5::Bounds( const renderEntity_t *ent ) const {
#if 0
	// we can't calculate a rational bounds without an entity,
	// because joints could be positioned to deform it into an
	// arbitrarily large shape
	if ( !ent ) {
		common->Error( "idRenderModelMD5::Bounds: called without entity" );
	}
#endif

	if ( !ent ) {
		// this is the bounds for the reference pose
		return bounds;
	}

	return ent->bounds;
}

/*
====================
idRenderModelMD5::DrawJoints
====================
*/
void idRenderModelMD5::DrawJoints( const renderEntity_t *ent, const struct viewDef_s *view ) const {
	int					i;
	int					num;
	idVec3				pos;
	const idJointMat	*joint;
	const idMD5Joint	*md5Joint;
	int					parentNum;

	num = ent->numJoints;
	joint = ent->joints;
	md5Joint = joints.Ptr();
	for( i = 0; i < num; i++, joint++, md5Joint++ ) {
		pos = ent->origin + joint->ToVec3() * ent->axis;
		if ( md5Joint->parent ) {
			parentNum = md5Joint->parent - joints.Ptr();
			session->rw->DebugLine( colorWhite, ent->origin + ent->joints[ parentNum ].ToVec3() * ent->axis, pos );
		}

		session->rw->DebugLine( colorRed,	pos, pos + joint->ToMat3()[ 0 ] * 2.0f * ent->axis );
		session->rw->DebugLine( colorGreen,	pos, pos + joint->ToMat3()[ 1 ] * 2.0f * ent->axis );
		session->rw->DebugLine( colorBlue,	pos, pos + joint->ToMat3()[ 2 ] * 2.0f * ent->axis );
	}

	idBounds bounds;

	bounds.FromTransformedBounds( ent->bounds, vec3_zero, ent->axis );
	session->rw->DebugBounds( colorMagenta, bounds, ent->origin );

	if ( ( r_jointNameScale.GetFloat() != 0.0f ) && ( bounds.Expand( 128.0f ).ContainsPoint( view->renderView.vieworg - ent->origin ) ) ) {
		idVec3	offset( 0, 0, r_jointNameOffset.GetFloat() );
		float	scale;

		scale = r_jointNameScale.GetFloat();
		joint = ent->joints;
		num = ent->numJoints;
		for( i = 0; i < num; i++, joint++ ) {
			pos = ent->origin + joint->ToVec3() * ent->axis;
			session->rw->DrawText( joints[ i ].name, pos + offset, scale, colorWhite, view->renderView.viewaxis, 1 );
		}
	}
}

/*
====================
idRenderModelMD5::InstantiateDynamicModel
====================
*/
idRenderModel *idRenderModelMD5::InstantiateDynamicModel( const struct renderEntity_s *ent, const struct viewDef_s *view, idRenderModel *cachedModel ) {
	int					i, surfaceNum;
	idMD5Mesh			*mesh;
	idRenderModelStatic	*staticModel;

	if ( cachedModel && !r_useCachedDynamicModels.GetBool() ) {
		delete cachedModel;
		cachedModel = NULL;
	}

	if ( purged ) {
		common->DWarning( "model %s instantiated while purged", Name() );
		LoadModel();
	}

	if ( !ent->joints ) {
		common->Printf( "idRenderModelMD5::InstantiateDynamicModel: NULL joints on renderEntity for '%s'\n", Name() );
		delete cachedModel;
		return NULL;
	} else if ( ent->numJoints != joints.Num() ) {
		common->Printf( "idRenderModelMD5::InstantiateDynamicModel: renderEntity has different number of joints than model for '%s'\n", Name() );
		delete cachedModel;
		return NULL;
	}

	tr.pc.c_generateMd5++;

	if ( cachedModel ) {
		assert( dynamic_cast<idRenderModelStatic *>(cachedModel) != NULL );
		assert( idStr::Icmp( cachedModel->Name(), MD5_SnapshotName ) == 0 );
		staticModel = static_cast<idRenderModelStatic *>(cachedModel);
	} else {
		staticModel = new idRenderModelStatic;
		staticModel->InitEmpty( MD5_SnapshotName );
	}

	staticModel->bounds.Clear();

	if ( r_showSkel.GetInteger() ) {
		if ( ( view != NULL ) && ( !r_skipSuppress.GetBool() || !ent->suppressSurfaceInViewID || ( ent->suppressSurfaceInViewID != view->renderView.viewID ) ) ) {
			// only draw the skeleton
			DrawJoints( ent, view );
		}

		if ( r_showSkel.GetInteger() > 1 ) {
			// turn off the model when showing the skeleton
			staticModel->InitEmpty( MD5_SnapshotName );
			return staticModel;
		}
	}

	// create all the surfaces
	for( mesh = meshes.Ptr(), i = 0; i < meshes.Num(); i++, mesh++ ) {
		// avoid deforming the surface if it will be a nodraw due to a skin remapping
		// FIXME: may have to still deform clipping hulls
		const idMaterial *shader = mesh->shader;

		shader = R_RemapShaderBySkin( shader, ent->customSkin, ent->customShader );

		if ( !shader || ( !shader->IsDrawn() && !shader->SurfaceCastsShadow() ) ) {
			staticModel->DeleteSurfaceWithId( i );
			mesh->surfaceNum = -1;
			continue;
		}

		modelSurface_t *surf;

		if ( staticModel->FindSurfaceWithId( i, surfaceNum ) ) {
			mesh->surfaceNum = surfaceNum;
			surf = &staticModel->surfaces[surfaceNum];
		} else {

			// Remove Overlays before adding new surfaces
			idRenderModelOverlay::RemoveOverlaySurfacesFromModel( staticModel );

			mesh->surfaceNum = staticModel->NumSurfaces();
			surf = &staticModel->surfaces.Alloc();
			surf->geometry = NULL;
			surf->shader = NULL;
			surf->id = i;
		}

		mesh->UpdateSurface( ent, ent->joints, surf );

		staticModel->bounds.AddPoint( surf->geometry->bounds[0] );
		staticModel->bounds.AddPoint( surf->geometry->bounds[1] );
	}

	return staticModel;
}

/*
====================
idRenderModelMD5::IsDynamicModel
====================
*/
dynamicModel_t idRenderModelMD5::IsDynamicModel() const {
	return DM_CACHED;
}

/*
====================
idRenderModelMD5::NumJoints
====================
*/
int idRenderModelMD5::NumJoints( void ) const {
	return joints.Num();
}

/*
====================
idRenderModelMD5::GetJoints
====================
*/
const idMD5Joint *idRenderModelMD5::GetJoints( void ) const {
	return joints.Ptr();
}

/*
====================
idRenderModelMD5::GetDefaultPose
====================
*/
const idJointQuat *idRenderModelMD5::GetDefaultPose( void ) const {
	return defaultPose.Ptr();
}

/*
====================
idRenderModelMD5::GetJointHandle
====================
*/
jointHandle_t idRenderModelMD5::GetJointHandle( const char *name ) const {
	const idMD5Joint *joint;
	int	i;

	joint = joints.Ptr();
	for( i = 0; i < joints.Num(); i++, joint++ ) {
		if ( idStr::Icmp( joint->name.c_str(), name ) == 0 ) {
			return ( jointHandle_t )i;
		}
	}

	return INVALID_JOINT;
}

/*
=====================
idRenderModelMD5::GetJointName
=====================
*/
const char *idRenderModelMD5::GetJointName( jointHandle_t handle ) const {
	if ( ( handle < 0 ) || ( handle >= joints.Num() ) ) {
		return "<invalid joint>";
	}

	return joints[ handle ].name;
}

/*
====================
idRenderModelMD5::NearestJoint
====================
*/
int idRenderModelMD5::NearestJoint( int surfaceNum, int a, int b, int c ) const {
	int i;
	const idMD5Mesh *mesh;

	if ( surfaceNum > meshes.Num() ) {
		common->Error( "idRenderModelMD5::NearestJoint: surfaceNum > meshes.Num()" );
	}

	for ( mesh = meshes.Ptr(), i = 0; i < meshes.Num(); i++, mesh++ ) {
		if ( mesh->surfaceNum == surfaceNum ) {
			return mesh->NearestJoint( a, b, c );
		}
	}
	return 0;
}

/*
====================
idRenderModelMD5::TouchData

models that are already loaded at level start time
will still touch their materials to make sure they
are kept loaded
====================
*/
void idRenderModelMD5::TouchData() {
	idMD5Mesh	*mesh;
	int			i;

	for( mesh = meshes.Ptr(), i = 0; i < meshes.Num(); i++, mesh++ ) {
		declManager->FindMaterial( mesh->shader->GetName() );
	}
}

/*
===================
idRenderModelMD5::PurgeModel

frees all the data, but leaves the class around for dangling references,
which can regenerate the data with LoadModel()
===================
*/
void idRenderModelMD5::PurgeModel() {
	purged = true;
	joints.Clear();
	defaultPose.Clear();
	meshes.Clear();
}

/*
===================
idRenderModelMD5::Memory
===================
*/
int	idRenderModelMD5::Memory() const {
	int		total, i;

	total = sizeof( *this );
	total += joints.MemoryUsed() + defaultPose.MemoryUsed() + meshes.MemoryUsed();

	// count up strings
	for ( i = 0; i < joints.Num(); i++ ) {
		total += joints[i].name.DynamicMemoryUsed();
	}

	// count up meshes
	for ( i = 0 ; i < meshes.Num() ; i++ ) {
		const idMD5Mesh *mesh = &meshes[i];

		total += mesh->texCoords.MemoryUsed() + mesh->numWeights * ( sizeof( mesh->scaledWeights[0] ) + sizeof( mesh->weightIndex[0] ) * 2 );

		// sum up deform info
		total += sizeof( mesh->deformInfo );
		total += R_DeformInfoMemoryUsed( mesh->deformInfo );
	}
	return total;
}
