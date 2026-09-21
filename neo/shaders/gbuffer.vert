// DUDE SSAO normal G-buffer (docs/ssao-gtao.md). Renders opaque geometry writing the
// bump-mapped view-space normal, so SSAO uses real per-pixel normals (including normal-map
// detail) instead of reconstructing flat geometry from depth. Foundation for POM/displacement.

#ifdef VULKAN
#extension GL_EXT_buffer_reference_uvec2 : require	// skinned motion vectors: previous-pose buffer by device address
#endif

#include "renderparms.glsl"

#ifdef VULKAN
// Skinned motion vectors (r_skinnedMotionVectors): for a GPU-skinned surface the skin kernel keeps each
// vertex's PREVIOUS-frame model-space position in a per-surface buffer; its device address arrives
// bit-cast in u_rtParms.xy (0 = none: rigid / static / CPU-skinned geometry). gpuSkinVB is bound at byte
// offset 0 with no base vertex, so gl_VertexIndex indexes that buffer directly.
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer PrevPosRef { vec4 p[]; };
#endif

layout(location = 0) in vec4 attr_Position;
layout(location = 1) in vec2 attr_TexCoord;
layout(location = 2) in vec3 attr_Normal;
layout(location = 3) in vec3 attr_Tangent;
layout(location = 4) in vec3 attr_Bitangent;
// DUDE tessellation (docs/tessellation.md): the alpha channel carries the
// per-vertex UV-seam displacement mask idMD5Mesh stamps in (see tess.glsl).
// Unused otherwise by this stage.
layout(location = 5) in vec4 attr_Color;

VARY(0) out vec2 var_TexBump;
VARY(1) out vec3 var_T;   // view-space tangent
VARY(2) out vec3 var_B;   // view-space bitangent
VARY(3) out vec3 var_N;   // view-space normal
VARY(4) out vec2 var_TexCoverage;   // diffuse UV for perforated (alpha-tested) surfaces
// model-space position + normal for the tessellation stages (DUDE tessellation,
// docs/tessellation.md) — so the SSAO normal G-buffer subdivides on exactly the same
// PN surface as zfill/interaction. Unconsumed by gbuffer.frag in the flat pipeline.
VARY(5) out vec3 var_ModelPos;
VARY(6) out vec4 var_ModelNormal;	// .w = UV-seam displacement mask
// DUDE motion vectors (docs/fsr-temporal-pipeline.md R1/A2): current + previous frame's clip
// position, interpolated perspective-correctly so gbuffer.frag divides per-pixel for the
// velocity write. Harmless (discarded) on the non-tessellated SSAO/SSR-only path where no
// velocity attachment is bound; the tessellation path regenerates them in gbuffer.tese.
VARY(7) out vec4 var_CurClip;
VARY(8) out vec4 var_PrevClip;
// previous-frame minus current model-space position (the ANIMATION's own motion; 0 for rigid geometry).
// The flat pipeline folds it into var_PrevClip below; the tessellation stages interpolate it across the
// patch and add it to the displaced position (gbuffer.tese), so skinned + tessellated monsters work too.
VARY(9) out vec3 var_PrevDelta;

void main() {
	vec4 st = vec4( attr_TexCoord, 0.0, 1.0 );
	var_TexBump = vec2( dot( st, u_bumpMatrixS ), dot( st, u_bumpMatrixT ) );
	var_TexCoverage = vec2( dot( st, u_diffuseMatrixS ), dot( st, u_diffuseMatrixT ) );

	// model -> view rotation (Doom 3 models carry no non-uniform scale, so the 3x3 is
	// an orthonormal rotation and normals/tangents transform with it directly)
	mat3 mv = mat3( u_modelViewMatrix );
	var_T = mv * attr_Tangent;
	var_B = mv * attr_Bitangent;
	var_N = mv * attr_Normal;

	var_ModelPos = attr_Position.xyz;
	var_ModelNormal = vec4( attr_Normal, attr_Color.a );

	// motion vectors (R1/A2): project through this frame's MVP (== un-jittered while jitter is
	// off; increment B must keep jitter out of the velocity) and the previous frame's cached
	// un-jittered MVP for this space. u_prevMvpMatrix is 0 on non-velocity draws → a dummy
	// prev clip that gbuffer.frag discards where there is no 3rd MRT.
	vec3 prevDelta = vec3( 0.0 );
#ifdef VULKAN
	uvec2 prevAddr = uvec2( floatBitsToUint( u_rtParms.x ), floatBitsToUint( u_rtParms.y ) );
	if ( ( prevAddr.x | prevAddr.y ) != 0u ) {
		prevDelta = PrevPosRef( prevAddr ).p[ gl_VertexIndex ].xyz - attr_Position.xyz;
	}
#endif
	var_PrevDelta = prevDelta;
	vec4 curClip = u_mvpMatrix * attr_Position;
	var_CurClip  = curClip;
	var_PrevClip = u_prevMvpMatrix * vec4( attr_Position.xyz + prevDelta, 1.0 );
	gl_Position  = curClip;
}
