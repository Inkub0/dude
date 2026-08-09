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
// Opt-in, off by default; Vulkan only (the compute lane) — GL3 always keeps the CPU skinner.
// NOT archived during bring-up: this feature device-lost the GPU twice, so it must default to 0
// on every launch and be enabled explicitly per-session (no persisted "1" can auto-enable it).
static idCVar r_gpuSkinning( "r_gpuSkinning", "0", CVAR_RENDERER | CVAR_BOOL,
	"skin animated (MD5) models on the GPU via the compute lane (Vulkan only; option-B TBN, docs/gpu-offload-plan.md Phase 2)" );

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
	TransformVerts( tri->verts, bindJoints );
	for ( int i = 0; i < numMir; i++ ) {
		tri->verts[base + i] = tri->verts[deformInfo->mirroredVerts[i]];
	}
	R_DeriveTangents( tri );

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
		if ( skinGpuWeights )  { r->DestroyBuffer( skinGpuWeights ); }
		if ( skinGpuWDesc )    { r->DestroyBuffer( skinGpuWDesc ); }
		if ( skinGpuWStart )   { r->DestroyBuffer( skinGpuWStart ); }
		if ( skinGpuLocalTBN ) { r->DestroyBuffer( skinGpuLocalTBN ); }
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
retained expanded-stream data + this frame's joints, then compare the read-back result against
the CPU reference (cpuRef, already skinned + tangent-derived). Prints the max position error
(should be ~1e-5 units) AND the normal/tangent angular divergence in degrees — the latter
quantifies the option-B fidelity cost (blended bind TBN vs stock's per-frame re-derive).

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

	rhi::BufferHandle bJoints  = r->CreateBuffer( rhi::BU_STORAGE, numJoints * (int)sizeof( idJointMat ), entJoints );
	rhi::BufferHandle bWeights = r->CreateBuffer( rhi::BU_STORAGE, E * (int)sizeof( idVec4 ), skinExpandWeights );
	rhi::BufferHandle bWDesc   = r->CreateBuffer( rhi::BU_STORAGE, E * 2 * (int)sizeof( int ), skinExpandWDesc );
	rhi::BufferHandle bWStart  = r->CreateBuffer( rhi::BU_STORAGE, numOut * (int)sizeof( unsigned int ), skinWeightStart );
	rhi::BufferHandle bTBN     = r->CreateBuffer( rhi::BU_STORAGE, E * 3 * (int)sizeof( idVec4 ), skinExpandLocalTBN );

	if ( bJoints && bWeights && bWDesc && bWStart && bTBN ) {
		// Control: skin into a BU_STORAGE buffer (the proven Phase-1 readback path).
		idDrawVert *outStorage = R_MD5_GpuSkinRunOnce( r, "storage", rhi::BU_STORAGE, r_md5SkinShader,
			numOut, numJoints, skinTemplate, cpuRef, bJoints, bWeights, bWDesc, bWStart, bTBN );

		// SAFE ISOLATION TEST: skin into a BU_SKIN buffer — the exact memory type the render path
		// uses (STORAGE|VERTEX, host-visible write-combined). Compute writes it, we read it back on
		// the host. There is NO draw, so the giant-triangle rasterization hang cannot occur here.
		// If this matches the BU_STORAGE control, the compute correctly writes BU_SKIN memory and
		// the device-lost bug lives in the DRAW/vertex-fetch path (barrier / cross-frame / async).
		// If it diverges, the BU_SKIN buffer's memory or usage is itself the culprit.
		idDrawVert *outSkin = R_MD5_GpuSkinRunOnce( r, "skin", rhi::BU_SKIN, r_md5SkinShader,
			numOut, numJoints, skinTemplate, cpuRef, bJoints, bWeights, bWDesc, bWStart, bTBN );

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

		if ( outStorage ) { Mem_Free16( outStorage ); }
		if ( outSkin )    { Mem_Free16( outSkin ); }
	}

	if ( bJoints )  { r->DestroyBuffer( bJoints ); }
	if ( bWeights ) { r->DestroyBuffer( bWeights ); }
	if ( bWDesc )   { r->DestroyBuffer( bWDesc ); }
	if ( bWStart )  { r->DestroyBuffer( bWStart ); }
	if ( bTBN )     { r->DestroyBuffer( bTBN ); }
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
	}

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
	if ( !r_useDeferredTangents.GetBool() ) {
		// set face planes, vertex normals, tangents
		R_DeriveTangents( tri );
	}

	// DUDE tessellation (docs/tessellation.md): weld coincident normals so this
	// mesh's mirror/UV seams don't pull open under PN tessellation + displacement.
	// Must run after normals exist; if they were deferred, derive now (and mark them
	// done) so the weld lands before R_CreateAmbientCache builds the streamed cache.
	if ( r_tessWeldSeams.GetBool() ) {
		if ( !tri->tangentsCalculated ) {
			R_DeriveTangents( tri );
		}
		R_WeldSeamNormals( tri, r_tessWeldThreshold.GetFloat() );
	}

	// Phase 2 GPU skinning (docs/gpu-offload-plan.md): additionally skin this surface on the GPU
	// into a persistent BU_SKIN buffer that the draw passes prefer (RB_RHI_StreamAmbient). The CPU
	// path above stays intact as the safety net (stencil, bounds, fallback). With r_gpuSkinning
	// off, or on GL3, or before the skin data is built, gpuSkinVB stays 0 = no behavior change.
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
