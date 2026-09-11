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

// Phase 2 (docs/gpu-offload-plan.md): validate GPU MD5 skinning against the CPU. Once/sec
// (r_gpuSkinTest) a mesh being skinned is ALSO skinned on the GPU via the compute lane and
// the positions are compared bit-close to idMD5Mesh::TransformVerts — proving the kernel +
// data layout before any of it touches the render path. Vulkan only (GL3 has no compute).
static idCVar r_gpuSkinTest( "r_gpuSkinTest", "0", CVAR_RENDERER | CVAR_BOOL,
	"validate GPU MD5 skinning vs the CPU once/sec, printing the max position error (Vulkan; docs/gpu-offload-plan.md Phase 2)" );

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

// Dev/validation gate (mirrors r_gpuSkinTest): once/sec run the B-1 headless validator -- DispatchSync
// the deform kernel, ReadBuffer, and diff GPU-vs-CPU-reference and BU_STORAGE-vs-BU_SKIN, with NO draw.
// Kernel-correctness only (GPU vs a CPU port of the SAME PN+displacement math at the SAME baked bary
// coords), not fidelity vs the hardware tessellator. Stalls the GPU, so never enable per-frame in ship.
static idCVar r_tessDeformTest( "r_tessDeformTest", "0", CVAR_RENDERER | CVAR_BOOL,
	"validate the compute deform (Roadmap B) vs a CPU reference once/sec, no draw (Vulkan; docs/tessellation.md)" );

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
	"layout(push_constant) uniform PC { uint numVerts; float skinScale; } pc;\n"
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
	"    v[o+0u]=p.x;   v[o+1u]=p.y;   v[o+2u]=p.z;\n"		// st = v[o+3..4] (seeded, untouched)
	"    v[o+5u]=nrm.x; v[o+6u]=nrm.y; v[o+7u]=nrm.z;\n"
	"    v[o+8u]=t0.x;  v[o+9u]=t0.y;  v[o+10u]=t0.z;\n"
	"    v[o+11u]=t1.x; v[o+12u]=t1.y; v[o+13u]=t1.z;\n"	// color = v[o+14] (seeded, untouched)
	"}\n";

static rhi::ShaderHandle r_md5SkinShader = 0;		// compiled once, shared by all meshes
static bool r_md5SkinShaderTried = false;

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
	deformInfo = R_BuildDeformInfo( texCoords.Num(), verts, tris.Num(), tris.Ptr(), shader->UseUnsmoothedTangents() );
	Mem_FreeA( verts, onStack );

	// Phase 2 (docs/gpu-offload-plan.md): retain the bind-pose data the GPU skinning kernel
	// needs but the stock CPU path discards. Vulkan only (the sole backend with a compute
	// lane); GL3 never pays the memory. Uses the just-built deformInfo + the bind joints.
	if ( rhi::GetActiveBackendType() == rhi::BT_VULKAN ) {
		BuildGpuSkinData( joints );
		// Roadmap B: bake the uniform-level deform topology when the feature (or its validator) is on.
		// Needs the bind pose here (BuildTessTopology is pose-invariant but reads bind-pose st/seam).
		if ( r_tessDeform.GetBool() || r_tessDeformTest.GetBool() ) {
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

FIDELITY NOTE: md5 verts are Clear()ed to color 0, so vertex colour on a base md5 surface
would render black (SVC_MODULATE) -- and sure enough nothing uses it: every vertexColor /
inverseVertexColor stage in the stock materials is either world/terrain blending, a projected
DECAL_MACRO, or an additive spawn effect (alpha ignored), never an md5 body/head skin. The
alpha byte is therefore free real estate here; RGB is left untouched, so even a mod material
reading vertexColor sees the same rgb it does today. The real cost is reduced relief in a band
around each UV seam, which on the imp is 44% of the verts -- not narrow. Kept unconditional
anyway: a partial pin leaves a proportionally smaller gap, which is still a gap.
====================
*/
void idMD5Mesh::StampTessSeamMask( idDrawVert *verts ) const {
	for ( int i = 0; i < deformInfo->numSourceVerts; i++ ) {
		verts[i].color[3] = 255;
	}
	for ( int i = 0; i < deformInfo->numDupVerts; i++ ) {
		verts[deformInfo->dupVerts[i * 2 + 0]].color[3] = 0;
		verts[deformInfo->dupVerts[i * 2 + 1]].color[3] = 0;
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
	// physically cannot pull apart under PN tessellation. Deliberately NOT R_WeldSeamNormals: its
	// dot-product gate (default 0.7 ~ 45 deg) rejects exactly the worst pairs, and the 49.54 deg
	// split measured on the cracking mesh sits just past it. Union-find because a 3-way coincident
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

	// Milestone D (docs/gpu-offload-plan.md): precompute the per-used-joint reach for CalcBoundsFast.
	// scaledWeights[w].xyz == weightValue * (joint-LOCAL vertex position), .w == weightValue, so the
	// local offset is xyz/w and its magnitude is the vertex's distance from the joint origin (rotation
	// preserves length). reach[joint] = max of that over the joint's weights; the runtime bound unions
	// (jointOrigin +/- reach) per used joint. The AABB over those spheres contains the convex hull of
	// every weighted vertex position -> a strict superset of the true skinned bound.
	skinBoundJoint.Clear();
	skinBoundReach.Clear();
	int maxJoint = -1;
	for ( int w = 0; w < numWeights; w++ ) {
		const int ji = weightIndex[w * 2 + 0] / (int)sizeof( idJointMat );
		if ( ji > maxJoint ) { maxJoint = ji; }
	}
	if ( maxJoint >= 0 ) {
		bool reachOnStack;
		float *reach = (float *)Mem_MallocA( ( maxJoint + 1 ) * sizeof( float ), reachOnStack );
		for ( int i = 0; i <= maxJoint; i++ ) { reach[i] = -1.0f; }		// -1 = joint unused by this mesh
		for ( int w = 0; w < numWeights; w++ ) {
			const int ji = weightIndex[w * 2 + 0] / (int)sizeof( idJointMat );
			const idVec4 &sw = scaledWeights[w];
			float mag = 0.0f;
			if ( idMath::Fabs( sw.w ) > 1e-8f ) {
				const idVec3 local( sw.x / sw.w, sw.y / sw.w, sw.z / sw.w );
				mag = local.Length();
			}
			if ( mag > reach[ji] ) { reach[ji] = mag; }		// -1 sentinel -> first real value (>=0) always wins
		}
		for ( int i = 0; i <= maxJoint; i++ ) {
			if ( reach[i] >= 0.0f ) {
				skinBoundJoint.Append( i );
				skinBoundReach.Append( reach[i] );
			}
		}
		Mem_FreeA( reach, reachOnStack );
	}
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
	skinBoundJoint.Clear();
	skinBoundReach.Clear();
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
====================
R_MD5_DeriveUnsmoothedRef

CPU reference derive: the math of idSIMD_SSE41::DeriveUnsmoothedTangents run over an arbitrary
vert array, in place. The harness needs its own copy because it cannot trust the normals sitting
in the CPU surface — r_useDeferredTangents (default 1) defers R_DeriveTangents to
R_CreateAmbientCache, which runs AFTER the validate hook, so those normals belong to the PREVIOUS
pose. Comparing against them measures a frame of animation, not skinning error.

Writing normal/tangents while reading only xyz/st makes the in-place pass order-independent,
exactly as it is in the GPU kernel.
====================
*/
static void R_MD5_DeriveUnsmoothedRef( idDrawVert *verts, const dominantTri_t *dom, int numVerts ) {
	for ( int i = 0; i < numVerts; i++ ) {
		const dominantTri_t &dt = dom[i];
		idDrawVert *a = verts + i;
		const idDrawVert *b = verts + dt.v2;
		const idDrawVert *c = verts + dt.v3;
		const idVec3 db = b->xyz - a->xyz;
		const idVec3 dc = c->xyz - a->xyz;
		const float d4 = b->st[1] - a->st[1];
		const float d9 = c->st[1] - a->st[1];
		const idVec3 n = dt.normalizationScale[2] * dc.Cross( db );
		const idVec3 t = dt.normalizationScale[0] * ( d9 * db - d4 * dc );
		const idVec3 bt = dt.normalizationScale[1] * t.Cross( n );
		a->normal = n;
		a->tangents[0] = t;
		a->tangents[1] = bt;
	}
}

/*
====================
R_MD5_GpuSkinRunOnce

Dispatches the blended-LBS skin kernel into a fresh output buffer of `outUsage`, reads it back
on the host, and prints its divergence vs the CPU reference. GpuSkinValidate runs it twice — once
into a BU_STORAGE control buffer, once into a BU_SKIN buffer (the exact memory type the render
path uses) — with NO draw, so the write-combined device-read that caused the device-lost hang is
probed at ZERO risk. Returns the read-back verts (caller Mem_Free16s) or NULL. The five read-only
input buffers are owned + shared by the caller. Vulkan only.
====================
*/
static idDrawVert *R_MD5_GpuSkinRunOnce(
		rhi::RHI *r, const char *label, rhi::BufferUsage outUsage, rhi::ShaderHandle shader,
		int numOut, int numJoints, const idDrawVert *seed, const struct srfTriangles_s *cpuRef,
		rhi::BufferHandle bJoints, rhi::BufferHandle bWeights, rhi::BufferHandle bWDesc,
		rhi::BufferHandle bWStart, rhi::BufferHandle bTBN ) {
	rhi::BufferHandle bOut = r->CreateBuffer( outUsage, numOut * (int)sizeof( idDrawVert ), seed );
	if ( bOut == 0 ) {
		common->Printf( "gpuSkin[%s]: out-buffer alloc failed\n", label );
		return NULL;
	}

	struct { unsigned int numVerts; float skinScale; } pc = { (unsigned int)numOut, 1.0f };
	rhi::ComputeArgs ca = {};
	ca.shader = shader;
	ca.storage[0] = bJoints; ca.storage[1] = bWeights; ca.storage[2] = bWDesc;
	ca.storage[3] = bWStart; ca.storage[4] = bOut; ca.storage[5] = bTBN;
	ca.pushConstants = &pc;
	ca.pushConstantSize = (int)sizeof( pc );
	ca.groupsX = ( numOut + 63 ) / 64; ca.groupsY = 1; ca.groupsZ = 1;
	r->DispatchSync( ca );

	idDrawVert *out = (idDrawVert *)Mem_Alloc16( numOut * sizeof( idDrawVert ) );
	const bool ok = r->ReadBuffer( bOut, out, numOut * (int)sizeof( idDrawVert ) );
	r->DestroyBuffer( bOut );
	if ( !ok ) {
		common->Printf( "gpuSkin[%s]: readback failed\n", label );
		Mem_Free16( out );
		return NULL;
	}

	float maxPos = 0.0f;
	int worst = -1;
	double nSum = 0.0, nMax = 0.0, tSum = 0.0, tMax = 0.0;
	double nSumClean = 0.0; int nClean = 0, nOutliers = 0;	// >30 deg = a pathological vert
	for ( int i = 0; i < numOut; i++ ) {
		const float e = ( out[i].xyz - cpuRef->verts[i].xyz ).Length();
		if ( e > maxPos ) { maxPos = e; worst = i; }

		float nd = out[i].normal * cpuRef->verts[i].normal;
		nd = nd < -1.0f ? -1.0f : ( nd > 1.0f ? 1.0f : nd );
		const double na = RAD2DEG( idMath::ACos( nd ) );
		nSum += na; if ( na > nMax ) { nMax = na; }
		if ( na > 30.0 ) { nOutliers++; } else { nSumClean += na; nClean++; }

		float td = out[i].tangents[0] * cpuRef->verts[i].tangents[0];
		td = td < -1.0f ? -1.0f : ( td > 1.0f ? 1.0f : td );
		const double ta = RAD2DEG( idMath::ACos( td ) );
		tSum += ta; if ( ta > tMax ) { tMax = ta; }
	}
	common->Printf( "gpuSkin[%s]: %d out-verts, %d joints -- pos err %.6f (worst %d) | normal avg %.2f (clean %.2f, %d>30deg) max %.2f | tangent avg %.2f max %.2f\n",
	                label, numOut, numJoints, maxPos, worst, nSum / numOut, nClean ? nSumClean / nClean : 0.0, nOutliers, nMax, tSum / numOut, tMax );
	return out;
}

/*
====================
idMD5Mesh::GpuSkinValidate

Dev harness (r_gpuSkinTest): skin this mesh on the GPU with the blended-LBS kernel using the
retained expanded-stream data + this frame's joints, then compare the read-back result against the
CPU reference. The reference's TBN is re-derived here from cpuRef's own current-pose positions
rather than read out of cpuRef — see R_MD5_DeriveUnsmoothedRef for why the stored one is a frame
stale. Prints the max position error (should be ~1e-5 units) AND the normal/tangent angular
divergence in degrees, which should now also be ~0 since the GPU runs stock's per-frame re-derive.

Runs the skin TWICE: into a BU_STORAGE control buffer (the proven Phase-1 readback path) and
into a BU_SKIN buffer (the exact STORAGE|VERTEX write-combined memory the render path uses), then
diffs the two read-backs. This isolates the device-lost hang WITHOUT a draw: if the two match, the
compute correctly writes BU_SKIN memory and the bug is in the vertex-fetch/draw path; if they
diverge, the BU_SKIN buffer itself is the culprit.

Rate-limited once/sec by the caller because DispatchSync stalls the GPU. Vulkan only.
====================
*/
void idMD5Mesh::GpuSkinValidate( const idJointMat *entJoints, const struct srfTriangles_s *cpuRef ) {
	rhi::RHI *r = rhi::GetRHI();
	if ( !r || !skinExpandLocalTBN || numOutputVerts <= 0 || skinExpandCount <= 0 ) {
		return;
	}
	if ( !cpuRef || cpuRef->numVerts != numOutputVerts || cpuRef->verts == NULL ) {
		return;
	}
	if ( R_MD5_SkinShader( r ) == 0 ) {
		return;
	}

	const int numOut = numOutputVerts;
	const int E = skinExpandCount;
	int maxJoint = 0;
	for ( int j = 0; j < E; j++ ) {
		const int ji = skinExpandWDesc[j * 2] / 12;
		if ( ji > maxJoint ) { maxJoint = ji; }
	}
	const int numJoints = maxJoint + 1;

	// Rebuild the CPU reference's TBN from ITS OWN current-pose positions before comparing anything.
	// cpuRef->verts carries last frame's derive (see R_MD5_DeriveUnsmoothedRef), which made every
	// GPU-vs-CPU number below read as ~15 deg of "error" that was really one frame of animation.
	// This stays an independent check: the positions are the CPU's, not the GPU's.
	idDrawVert *refVerts = NULL;
	srfTriangles_t refTri;
	const srfTriangles_t *ref = cpuRef;
	if ( deformInfo && deformInfo->dominantTris ) {
		refVerts = (idDrawVert *)Mem_Alloc16( numOut * sizeof( idDrawVert ) );
		memcpy( refVerts, cpuRef->verts, numOut * sizeof( idDrawVert ) );
		R_MD5_DeriveUnsmoothedRef( refVerts, deformInfo->dominantTris, numOut );
		refTri = *cpuRef;
		refTri.verts = refVerts;
		ref = &refTri;
	}

	rhi::BufferHandle bJoints  = r->CreateBuffer( rhi::BU_STORAGE, numJoints * (int)sizeof( idJointMat ), entJoints );
	rhi::BufferHandle bWeights = r->CreateBuffer( rhi::BU_STORAGE, E * (int)sizeof( idVec4 ), skinExpandWeights );
	rhi::BufferHandle bWDesc   = r->CreateBuffer( rhi::BU_STORAGE, E * 2 * (int)sizeof( int ), skinExpandWDesc );
	rhi::BufferHandle bWStart  = r->CreateBuffer( rhi::BU_STORAGE, numOut * (int)sizeof( unsigned int ), skinWeightStart );
	rhi::BufferHandle bTBN     = r->CreateBuffer( rhi::BU_STORAGE, E * 3 * (int)sizeof( idVec4 ), skinExpandLocalTBN );

	if ( bJoints && bWeights && bWDesc && bWStart && bTBN ) {
		// Control: skin into a BU_STORAGE buffer (the proven Phase-1 readback path).
		idDrawVert *outStorage = R_MD5_GpuSkinRunOnce( r, "storage", rhi::BU_STORAGE, r_md5SkinShader,
			numOut, numJoints, skinTemplate, ref, bJoints, bWeights, bWDesc, bWStart, bTBN );

		// SAFE ISOLATION TEST: skin into a BU_SKIN buffer — the exact memory type the render path
		// uses (STORAGE|VERTEX, host-visible write-combined). Compute writes it, we read it back on
		// the host. There is NO draw, so the giant-triangle rasterization hang cannot occur here.
		// If this matches the BU_STORAGE control, the compute correctly writes BU_SKIN memory and
		// the device-lost bug lives in the DRAW/vertex-fetch path (barrier / cross-frame / async).
		// If it diverges, the BU_SKIN buffer's memory or usage is itself the culprit.
		idDrawVert *outSkin = R_MD5_GpuSkinRunOnce( r, "skin", rhi::BU_SKIN, r_md5SkinShader,
			numOut, numJoints, skinTemplate, ref, bJoints, bWeights, bWDesc, bWStart, bTBN );

		if ( outStorage && outSkin ) {
			float maxDelta = 0.0f;
			int nDiff = 0;
			for ( int i = 0; i < numOut; i++ ) {
				const float d = ( outSkin[i].xyz - outStorage[i].xyz ).Length();
				if ( d > maxDelta ) { maxDelta = d; }
				if ( d > 0.001f ) { nDiff++; }
			}
			common->Printf( "gpuSkin[compare]: BU_SKIN vs BU_STORAGE -- max pos delta %.6f, %d/%d verts differ => %s\n",
			                maxDelta, nDiff, numOut,
			                nDiff == 0 ? "IDENTICAL (BU_SKIN compute-write CORRECT; device-lost bug is in the DRAW path)"
			                           : "DIVERGENT (BU_SKIN memory/usage is the bug)" );
		}

		// SEAM-SPLIT DIAGNOSTIC (the tessellation crack question). PN tessellation tears wherever
		// two COINCIDENT vertices disagree with each other, so what matters is not GPU-vs-CPU
		// divergence (measured above) but the split WITHIN each result. dupVerts holds exactly the
		// UV-split coincident pairs and mirroredVerts the mirror-seam ones; a nonzero GPU split
		// against a ~0 CPU split is the crack, and whether it shows in pos or normal says which.
		if ( outStorage && deformInfo ) {
			float gpuPosMax = 0.0f, cpuPosMax = 0.0f;
			double gpuNrmMax = 0.0, cpuNrmMax = 0.0, brokenMax = 0.0;
			int pairs = 0, gpuSplit = 0;

			struct measure_t {
				static void Pair( int a, int b, const idDrawVert *g, const idDrawVert *c,
				                  float &gp, float &cp, double &gn, double &cn, int &split, double &bmax ) {
					const float gd = ( g[a].xyz - g[b].xyz ).Length();
					const float cd = ( c[a].xyz - c[b].xyz ).Length();
					if ( gd > gp ) { gp = gd; }
					if ( cd > cp ) { cp = cd; }
					float gdot = g[a].normal * g[b].normal;
					gdot = gdot < -1.0f ? -1.0f : ( gdot > 1.0f ? 1.0f : gdot );
					const double ga = RAD2DEG( idMath::ACos( gdot ) );
					float cdot = c[a].normal * c[b].normal;
					cdot = cdot < -1.0f ? -1.0f : ( cdot > 1.0f ? 1.0f : cdot );
					const double ca = RAD2DEG( idMath::ACos( cdot ) );
					if ( ga > gn ) { gn = ga; }
					if ( ca > cn ) { cn = ca; }
					// The signal: a pair the CPU holds together (agrees to within a degree) but the
					// GPU pulls apart. Pairs that are legitimately opposite on BOTH paths (coincident
					// back-to-back sheets read 180 deg on the CPU too) are not cracks and must not
					// count, which a bare max/threshold over all pairs would wrongly flag.
					// bmax records HOW FAR the flagged pairs actually split: a count alone cannot
					// distinguish a mesh torn open from one sitting a degree or two above threshold.
					if ( ca <= 1.0 && ( gd > 0.001f || ga > 1.0 ) ) {
						split++;
						if ( ga > bmax ) { bmax = ga; }
					}
				}
			};

			for ( int i = 0; i < deformInfo->numDupVerts; i++ ) {
				const int a = deformInfo->dupVerts[i * 2 + 0];
				const int b = deformInfo->dupVerts[i * 2 + 1];
				if ( a < numOut && b < numOut ) {
					measure_t::Pair( a, b, outStorage, ref->verts, gpuPosMax, cpuPosMax, gpuNrmMax, cpuNrmMax, gpuSplit, brokenMax );
					pairs++;
				}
			}
			const int mirBase = numOut - deformInfo->numMirroredVerts;
			for ( int i = 0; i < deformInfo->numMirroredVerts; i++ ) {
				const int a = mirBase + i;
				const int b = deformInfo->mirroredVerts[i];
				if ( a < numOut && b < numOut ) {
					measure_t::Pair( a, b, outStorage, ref->verts, gpuPosMax, cpuPosMax, gpuNrmMax, cpuNrmMax, gpuSplit, brokenMax );
					pairs++;
				}
			}

			common->Printf( "gpuSkin[seams]: %d coincident pairs (%d dup, %d mirrored) -- GPU split: pos %.6f, normal %.2f deg (%d CPU-agreed pairs BROKEN by GPU, worst %.2f deg) | CPU split: pos %.6f, normal %.2f deg\n",
			                pairs, deformInfo->numDupVerts, deformInfo->numMirroredVerts,
			                gpuPosMax, gpuNrmMax, gpuSplit, brokenMax, cpuPosMax, cpuNrmMax );
		}

		if ( outStorage ) { Mem_Free16( outStorage ); }
		if ( outSkin )    { Mem_Free16( outSkin ); }
	}

	if ( refVerts ) { Mem_Free16( refVerts ); }

	if ( bJoints )  { r->DestroyBuffer( bJoints ); }
	if ( bWeights ) { r->DestroyBuffer( bWeights ); }
	if ( bWDesc )   { r->DestroyBuffer( bWDesc ); }
	if ( bWStart )  { r->DestroyBuffer( bWStart ); }
	if ( bTBN )     { r->DestroyBuffer( bTBN ); }
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

// Full-precision normalize for the CPU reference. idVec3::Normalize() uses idMath::InvSqrt (an
// 8-bit-seed Newton-Raphson, ~1e-7 relative error) which, propagated through the PN edge control
// points on model-space corners tens of units apart, inflates the validator's printed position
// delta to ~1e-5..1e-4 -- reading as kernel drift when the kernel is correct. GLSL normalize() is
// near-full IEEE, so match it with 1/sqrtf to keep the diagnostic number honest.
static ID_INLINE idVec3 R_TessNormalizeFP( const idVec3 &v ) {
	const float lsq = v.x * v.x + v.y * v.y + v.z * v.z;
	if ( lsq <= 0.0f ) {
		return idVec3( 0.0f, 0.0f, 0.0f );
	}
	const float inv = 1.0f / sqrtf( lsq );
	return idVec3( v.x * inv, v.y * inv, v.z * inv );
}

// C++ port of dudeTessPN (tess.glsl:47) in the identical op order, for the validator's reference.
static idVec3 R_TessPN_CpuRef( const idVec3 &p0, const idVec3 &p1, const idVec3 &p2,
                               const idVec3 &n0, const idVec3 &n1, const idVec3 &n2, const idVec3 &tc ) {
	idVec3 b300 = p0, b030 = p1, b003 = p2;
	idVec3 b210 = ( 2.0f * p0 + p1 - ( ( p1 - p0 ) * n0 ) * n0 ) / 3.0f;
	idVec3 b120 = ( 2.0f * p1 + p0 - ( ( p0 - p1 ) * n1 ) * n1 ) / 3.0f;
	idVec3 b021 = ( 2.0f * p1 + p2 - ( ( p2 - p1 ) * n1 ) * n1 ) / 3.0f;
	idVec3 b012 = ( 2.0f * p2 + p1 - ( ( p1 - p2 ) * n2 ) * n2 ) / 3.0f;
	idVec3 b102 = ( 2.0f * p2 + p0 - ( ( p0 - p2 ) * n2 ) * n2 ) / 3.0f;
	idVec3 b201 = ( 2.0f * p0 + p2 - ( ( p2 - p0 ) * n0 ) * n0 ) / 3.0f;
	idVec3 e = ( b210 + b120 + b021 + b012 + b102 + b201 ) / 6.0f;
	idVec3 vmid = ( p0 + p1 + p2 ) / 3.0f;
	idVec3 b111 = e + ( e - vmid ) * 0.5f;
	const float a = tc.x, b = tc.y, c = tc.z;
	const float a2 = a * a, b2 = b * b, c2 = c * c;
	return b300 * ( a2 * a ) + b030 * ( b2 * b ) + b003 * ( c2 * c )
	     + b210 * ( 3.0f * a2 * b ) + b120 * ( 3.0f * a * b2 )
	     + b021 * ( 3.0f * b2 * c ) + b012 * ( 3.0f * b * c2 )
	     + b102 * ( 3.0f * a * c2 ) + b201 * ( 3.0f * a2 * c )
	     + b111 * ( 6.0f * a * b * c );
}

// Bilinear sample of a normal-map's blue channel (relief source, tess.glsl dudeTessDisplace).
// uv wraps (repeat) to match the material sampler; returns relief = (1 - clamp(bz,0,1)) with
// bz = blue*2-1, BEFORE the seam scale (applied by the caller). pic is RGBA8, row-major.
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
	const float b00 = pic[( iy0 * w + ix0 ) * 4 + 2] / 255.0f;
	const float b10 = pic[( iy0 * w + ix1 ) * 4 + 2] / 255.0f;
	const float b01 = pic[( iy1 * w + ix0 ) * 4 + 2] / 255.0f;
	const float b11 = pic[( iy1 * w + ix1 ) * 4 + 2] / 255.0f;
	const float blue = ( b00 * ( 1.0f - tx ) + b10 * tx ) * ( 1.0f - ty )
	                 + ( b01 * ( 1.0f - tx ) + b11 * tx ) * ty;
	const float bz = blue * 2.0f - 1.0f;
	float relief = 1.0f - idMath::ClampFloat( 0.0f, 1.0f, bz );
	return relief;
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

Bake the static uniform-level subdivision once at load (Vulkan, r_tessDeform/r_tessDeformTest on).
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
			if ( st && st->lighting == SL_BUMP && st->texture.image && st->texture.image->imgName.Length() ) {
				R_LoadImageProgram( st->texture.image->imgName.c_str(), &bumpPic, &bumpW, &bumpH, NULL, NULL );
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
R_TessDeform_RunOnce

Dispatch the deform kernel into a fresh output buffer of `outUsage`, read it back, and print its
divergence vs a CPU reference that evaluates the SAME PN + displacement math at the SAME baked bary
coords (NOT the hardware tessellator). Mirrors R_MD5_GpuSkinRunOnce. `srcVB` is the current-pose
source-vert SSBO (owned by the caller); the three static topology SSBOs are the mesh's. Returns the
read-back verts (caller Mem_Free16s) or NULL. Vulkan only.
====================
*/
static idDrawVert *R_TessDeform_RunOnce(
		rhi::RHI *r, const char *label, rhi::BufferUsage outUsage, rhi::ShaderHandle shader,
		int numTessVerts, float dispStrength, const idDrawVert *seedTemplate,
		const idVec4 *barySeam, const int *srcTri, const idDrawVert *srcVerts, int numSrcVerts,
		rhi::BufferHandle srcVB, rhi::BufferHandle bBary, rhi::BufferHandle bSrcTri, rhi::BufferHandle bHeight,
		const float *height ) {
	rhi::BufferHandle bOut = r->CreateBuffer( outUsage, numTessVerts * (int)sizeof( idDrawVert ), seedTemplate );
	if ( bOut == 0 ) {
		common->Printf( "tessDeform[%s]: out-buffer alloc failed\n", label );
		return NULL;
	}
	struct { unsigned int numVerts; float dispStrength; } pc = { (unsigned int)numTessVerts, dispStrength };
	rhi::ComputeArgs ca = {};
	ca.shader = shader;
	ca.storage[0] = srcVB; ca.storage[1] = bBary; ca.storage[2] = bSrcTri; ca.storage[3] = bHeight; ca.storage[4] = bOut;
	ca.pushConstants = &pc;
	ca.pushConstantSize = (int)sizeof( pc );
	ca.groupsX = ( numTessVerts + 63 ) / 64; ca.groupsY = 1; ca.groupsZ = 1;
	r->DispatchSync( ca );

	idDrawVert *out = (idDrawVert *)Mem_Alloc16( numTessVerts * sizeof( idDrawVert ) );
	const bool ok = r->ReadBuffer( bOut, out, numTessVerts * (int)sizeof( idDrawVert ) );
	r->DestroyBuffer( bOut );
	if ( !ok ) {
		common->Printf( "tessDeform[%s]: readback failed\n", label );
		Mem_Free16( out );
		return NULL;
	}

	// CPU reference at the same baked bary coords, same op order.
	float maxPos = 0.0f;   int worst = -1;
	double nSum = 0.0, nMax = 0.0;  int nOutliers = 0, nNaN = 0;
	for ( int i = 0; i < numTessVerts; i++ ) {
		const int c0 = srcTri[i * 3 + 0], c1 = srcTri[i * 3 + 1], c2 = srcTri[i * 3 + 2];
		if ( c0 < 0 || c0 >= numSrcVerts || c1 < 0 || c1 >= numSrcVerts || c2 < 0 || c2 >= numSrcVerts ) {
			continue;
		}
		const idVec3 p0 = srcVerts[c0].xyz, p1 = srcVerts[c1].xyz, p2 = srcVerts[c2].xyz;
		// full-precision normalize to mirror GLSL normalize() (see R_TessNormalizeFP) -- NOT
		// idVec3::Normalize(), whose approximate InvSqrt would inflate the printed parity delta
		const idVec3 n0 = R_TessNormalizeFP( srcVerts[c0].normal );
		const idVec3 n1 = R_TessNormalizeFP( srcVerts[c1].normal );
		const idVec3 n2 = R_TessNormalizeFP( srcVerts[c2].normal );
		const idVec3 bary( barySeam[i].x, barySeam[i].y, barySeam[i].z );
		idVec3 pos = R_TessPN_CpuRef( p0, p1, p2, n0, n1, n2, bary );
		idVec3 geoN = R_TessNormalizeFP( n0 * bary.x + n1 * bary.y + n2 * bary.z );
		pos += geoN * ( height[i] * dispStrength );

		const idVec3 &g = out[i].xyz;
		if ( g.x != g.x || g.y != g.y || g.z != g.z ) { nNaN++; continue; }		// NaN bucket (zero source normal etc.)
		const float e = ( g - pos ).Length();
		if ( e > maxPos ) { maxPos = e; worst = i; }

		idVec3 gn = out[i].normal;
		float nd = gn * geoN;
		nd = nd < -1.0f ? -1.0f : ( nd > 1.0f ? 1.0f : nd );
		const double na = RAD2DEG( idMath::ACos( nd ) );
		nSum += na; if ( na > nMax ) { nMax = na; }
		if ( na > 30.0 ) { nOutliers++; }
	}
	common->Printf( "tessDeform[%s]: %d out-verts -- pos err %.6f (worst %d) | normal avg %.3f max %.3f (%d>30deg) | %d NaN\n",
	                label, numTessVerts, maxPos, worst,
	                ( numTessVerts - nNaN ) ? nSum / ( numTessVerts - nNaN ) : 0.0, nMax, nOutliers, nNaN );
	return out;
}

/*
====================
idMD5Mesh::TessDeformValidate

Dev harness (r_tessDeformTest): run the deform kernel over this frame's CPU-deformed verts and compare
the read-back against a CPU reference of the same PN + displacement math. Runs it into a BU_STORAGE
control buffer and a BU_SKIN buffer (the exact render memory type) and diffs the two, isolating any
device-lost/vertex-fetch bug class WITHOUT a draw. Kernel-correctness only. Rate-limited once/sec by
the caller (DispatchSync stalls). Vulkan only.
====================
*/
void idMD5Mesh::TessDeformValidate( const struct srfTriangles_s *cpuRef ) {
	rhi::RHI *r = rhi::GetRHI();
	if ( !r || !tessBarySeam || numTessOutVerts <= 0 ) {
		return;
	}
	if ( !cpuRef || cpuRef->verts == NULL || cpuRef->numVerts != deformInfo->numOutputVerts ) {
		return;
	}
	if ( R_MD5_TessShader( r ) == 0 || !EnsureTessBuffersUploaded() ) {
		return;
	}
	const int numOut = deformInfo->numOutputVerts;
	const float dispStrength = r_tessDisplace.GetFloat();

	// current-pose source verts as an SSBO (raw idDrawVert stream)
	rhi::BufferHandle srcVB = r->CreateBuffer( rhi::BU_STORAGE, numOut * (int)sizeof( idDrawVert ), cpuRef->verts );
	if ( !srcVB ) {
		return;
	}

	// seed the output with anything (kernel overwrites xyz/st/normal/tangents; color stays)
	idDrawVert *seed = (idDrawVert *)Mem_Alloc16( numTessOutVerts * sizeof( idDrawVert ) );
	for ( int i = 0; i < numTessOutVerts; i++ ) { seed[i].Clear(); }

	idDrawVert *outStorage = R_TessDeform_RunOnce( r, "storage", rhi::BU_STORAGE, r_md5TessShader,
		numTessOutVerts, dispStrength, seed, tessBarySeam, tessSrcTri, cpuRef->verts, numOut,
		srcVB, tessGpuBarySeam, tessGpuSrcTri, tessGpuHeight, tessHeight );
	idDrawVert *outSkin = R_TessDeform_RunOnce( r, "skin", rhi::BU_SKIN, r_md5TessShader,
		numTessOutVerts, dispStrength, seed, tessBarySeam, tessSrcTri, cpuRef->verts, numOut,
		srcVB, tessGpuBarySeam, tessGpuSrcTri, tessGpuHeight, tessHeight );

	if ( outStorage && outSkin ) {
		float maxDelta = 0.0f;  int nDiff = 0;
		for ( int i = 0; i < numTessOutVerts; i++ ) {
			const float d = ( outSkin[i].xyz - outStorage[i].xyz ).Length();
			if ( d > maxDelta ) { maxDelta = d; }
			if ( d > 0.001f ) { nDiff++; }
		}
		common->Printf( "tessDeform[compare]: BU_SKIN vs BU_STORAGE -- max pos delta %.6f, %d/%d verts differ => %s\n",
		                maxDelta, nDiff, numTessOutVerts,
		                nDiff == 0 ? "IDENTICAL (BU_SKIN compute-write correct)" : "DIVERGENT (BU_SKIN memory/usage suspect)" );
	}

	if ( outStorage ) { Mem_Free16( outStorage ); }
	if ( outSkin )    { Mem_Free16( outSkin ); }
	Mem_Free16( seed );
	r->DestroyBuffer( srcVB );
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

	// This CPU skin normally runs even when the GPU skinner below is active, because it writes tri->
	// verts.xyz and front-end consumers read those positions every frame with no way to see gpuSkinVB
	// (a GPU-side buffer with no CPU mapping):
	//   - light culling      R_CalcInteractionCullBits / R_ClipTriangleToLight (Interaction.cpp)
	//                        -- per interaction, PER LIGHT; a PURE cull (skipping it draws all tris)
	//   - stencil shadows    R_CreateShadowVolume, R_CreateVertexProgramShadowCache
	//                        -- absent for shadow-mapped lights, present for stencil/sun ones
	//   - surface bounds     R_BoundTriSurf, just below
	//   - blood/burn decals  idRenderModelOverlay::AddOverlaySurfacesToModel (reads xyz each frame)
	//
	// Milestone D (r_gpuSkinStripCpu, docs/gpu-offload-plan.md): retire ALL of that when the frame
	// proved every reader is absent for this surface. R_EntityDefDynamicModel set r_skinStripThisModel
	// only when the view has no stencil-shadow light (pin: stencil) and this entity has no overlay
	// (pin: decals); the light-cull reads are routed around by R_CreateLightTris' full-index path
	// (pin: light cull) and the bound comes from the O(joints) joint-reach CalcBoundsFast (pin: bounds,
	// a conservative superset every consumer tolerates). Surfaces whose MATERIAL reads posed positions on
	// the CPU keep the full skin: a vertex deform (deform eyeBall on every character/monster eye sub-mesh,
	// expand/move/turb/flare) rebuilds geometry from tri->verts in R_DeformDrawSurf every frame, and
	// subview/GUI materials read them in R_PreciseCullSurface -> gate on Deform()==DFRM_NONE && !HasSubview()
	// && !HasGui(). PER-MESH on the mesh's own material, which is exactly right: the eye is its own idMD5Mesh
	// with the eyeBall material (excluded), while the body mesh (plain) still strips. Skin-scaled deaths keep
	// the CPU path too (the fast bound carries no MD5 skin-scale). Requires the GPU skin to materialize below
	// (shader + SSBOs ready) or the stripped surface would have no geometry at all. When stripped the surface
	// rasterizes solely from gpuSkinVB, and the redundant CPU TransformVerts + R_DeriveTangents +
	// ambient upload are all gone -- the actual per-frame CPU skin cost this whole project set out to
	// remove. HARD-GATED on r_gpuSkinning so the OFF path is byte-for-byte the stock CPU skinner.
	// DUDE: the CPU POSITION skin (TransformVerts) is non-negotiable and can NEVER be stripped.
	// A living monster's player-hit collision IS its animated render model: idActor's combat clip
	// model is built from the render-model handle (CONTENTS_RENDERMODEL, d3xp/Actor.cpp), player
	// attacks trace with MASK_SHOT_RENDERMODEL, and idClip::TraceRenderModel ->
	// idRenderWorldLocal::ModelTrace -> R_LocalTrace reads tri->verts[].xyz DIRECTLY (tr_trace.cpp).
	// The former r_gpuSkinStripCpu left those verts at BIND POSE, so every monster's hittable
	// surface was a T-pose at the model origin and the player's shots passed straight through it
	// -- monsters were invulnerable while still able to attack (user-found 2026-08-18). The old
	// strip-safety audit only checked RENDER readers of tri->verts and missed this game hit path.
	// So the position skin always runs. GPU skinning still offloads the DRAW (gpuSkinVB, below) and
	// r_gpuSkinNoUpload still skips the redundant ambient upload -- both leave tri->verts posed.
	const bool stripCpu = false;
	tri->cpuSkinStripped = false;

	if ( !stripCpu ) {
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
	} else {
		// stripped: verts[].xyz stays as-is (unread this frame); bound from the joint palette only
		tri->bounds = CalcBoundsFast( entJoints );
		if ( r_gpuSkinProfile.GetBool() ) {
			R_GpuSkinProfileAddStrip( deformInfo->numOutputVerts );
		}
	}

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
	if ( !stripCpu
	     && ( !r_useDeferredTangents.GetBool()
	          || ( r_tessellation.GetBool() && rhi::GetActiveBackendType() == rhi::BT_VULKAN ) ) ) {
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
	if ( !stripCpu && ( r_tessDeform.GetBool()
	     || ( ( r_gpuSkinning.GetBool() || r_tessellation.GetBool() ) && rhi::GetActiveBackendType() == rhi::BT_VULKAN ) ) ) {
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
			// shadow/bounds read xyz only); only the decal copy + the unused ambient cache change. Mirrors
			// GpuSkinValidate's walk; skinExpandLocalTBN is already welded, so this yields welded normals for
			// free. NORMAL ONLY: PN ignores tangents and the body's gpuSkinVB tangents are unwelded.
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
		}
		if ( !tri->gpuSkinVB ) {
			tri->gpuSkinVB = r->CreateBuffer( rhi::BU_SKIN, numOut * (int)sizeof( idDrawVert ), skinTemplate );
			tri->gpuSkinVerts = numOut;
			tri->gpuSkinFrame = -1;
		}
		// one dispatch per surface per frame (an entity in several views instantiates once)
		if ( tri->gpuSkinVB && tri->gpuSkinFrame != tr.frameCount ) {
			tri->gpuSkinFrame = tr.frameCount;
			const int numJoints = ent->numJoints;
			idJointMat *jointSnap = (idJointMat *)R_FrameAlloc( numJoints * (int)sizeof( idJointMat ) );
			memcpy( jointSnap, entJoints, numJoints * sizeof( idJointMat ) );
			const float skinScale = ( ent->shaderParms[ SHADERPARM_MD5_SKINSCALE ] != 0.0f )
			                      ? ent->shaderParms[ SHADERPARM_MD5_SKINSCALE ] : 1.0f;
			RB_RHI_AddSkinJob( skinShader, tri->gpuSkinVB, numOut,
			                   skinGpuWeights, skinGpuWDesc, skinGpuWStart, skinGpuLocalTBN,
			                   jointSnap, numJoints, skinScale );
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

	// Phase 2 validation (r_gpuSkinTest): once/sec, skin this mesh on the GPU with the full
	// option-B kernel and compare against this CPU result (positions bit-exact, TBN divergence
	// reported in degrees). Needs derived tangents on the reference, and stalls the GPU, so it
	// is rate-limited and dev-only. Vulkan only (skin data only built there).
	if ( r_gpuSkinTest.GetBool() && skinExpandLocalTBN ) {
		static int s_lastMs = 0;
		const int now = Sys_Milliseconds();
		if ( now - s_lastMs >= 1000 ) {
			s_lastMs = now;
			if ( !tri->tangentsCalculated ) {
				R_DeriveTangents( tri );
			}
			GpuSkinValidate( entJoints, tri );
		}
	}

	// Roadmap B validation (r_tessDeformTest): once/sec, deform this mesh on the GPU and compare the
	// read-back against a CPU reference of the same PN + displacement math. Kernel-correctness only,
	// no draw. Needs the reference's current-pose tangents. The bake is load-time (needs bind pose),
	// so r_tessDeformTest must be set BEFORE the map/model loads; a change to r_tessLevel needs a
	// reload too (the baked topology is level-keyed). Vulkan only.
	if ( r_tessDeformTest.GetBool() && tessBarySeam ) {
		static int s_lastTessMs = 0;
		const int now = Sys_Milliseconds();
		if ( now - s_lastTessMs >= 1000 ) {
			s_lastTessMs = now;
			if ( !tri->tangentsCalculated ) {
				R_DeriveTangents( tri );
			}
			TessDeformValidate( tri );
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
idMD5Mesh::CalcBoundsFast

Milestone D (docs/gpu-offload-plan.md): a conservative bound from the joint palette ALONE — no
per-vertex skin, O(joints this mesh uses). Unions each used joint's world origin +/- its precomputed
reach radius (built in BuildGpuSkinData). The AABB over those spheres contains the convex hull of
every weighted vertex position, so it is a strict superset of the true skinned bound (every consumer
of tri->bounds only ever UNDER-culls with a larger box, never wrong). Empty skinBoundJoint (data not
built) returns a cleared/inverted bound — callers must gate on skinBoundJoint.Num() > 0.
====================
*/
idBounds idMD5Mesh::CalcBoundsFast( const idJointMat *entJoints ) const {
	idBounds bounds;
	bounds.Clear();
	const int n = skinBoundJoint.Num();
	for ( int i = 0; i < n; i++ ) {
		const idVec3 o = entJoints[ skinBoundJoint[i] ].ToVec3();		// joint world origin (translation column)
		const float r = skinBoundReach[i];
		bounds.AddPoint( o + idVec3( r, r, r ) );
		bounds.AddPoint( o - idVec3( r, r, r ) );
	}
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
