// Phase 3.2b BDA consume (docs/gpu-offload-plan.md §3.2b): the depth-prepass fill,
// but the vertex is fetched from a raw GPU pointer (buffer_device_address) instead
// of bound vertex attributes. Produces BIT-IDENTICAL clip-space positions to
// zfill.vert — same bytes, same `u_mvpMatrix * vec4(pos,1)`, and compile_spv.py
// injects `invariant gl_Position;` for both — so r_vkBdaZfill is a pixel-identical
// A/B and the DEPTHFUNC_EQUAL ambient pass still matches.
//
// idDrawVert is 60 bytes = 15 uints: xyz @0..2, st @3..4 (normal @5..7, tangents
// @8..13, color @14). The push constant carries the address of the surface's
// idDrawVert[0] (GetBufferDeviceAddress(vb) + vertexOffset); gl_VertexIndex is the
// resolved index, matching what the bound-attribute path would fetch.

#include "renderparms.glsl"

layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer VertRef {
	uint w[];			// idDrawVert as a raw uint array (stride 15 uints)
};
layout(push_constant) uniform PushConstants {
	VertRef verts;		// GPU address of idDrawVert[0] for this surface
} pc;

VARY(0) out vec2 var_TexCoord;

void main() {
	uint base = uint( gl_VertexIndex ) * 15u;		// 60-byte idDrawVert stride / 4
	vec3 xyz = vec3( uintBitsToFloat( pc.verts.w[base + 0u] ),
	                 uintBitsToFloat( pc.verts.w[base + 1u] ),
	                 uintBitsToFloat( pc.verts.w[base + 2u] ) );
	vec2 stc = vec2( uintBitsToFloat( pc.verts.w[base + 3u] ),
	                 uintBitsToFloat( pc.verts.w[base + 4u] ) );

	// identical to zfill.vert so perforated alpha-test surfaces sample the same UV
	vec4 st = vec4( stc, 0.0, 1.0 );
	var_TexCoord = vec2( dot( st, u_diffuseMatrixS ), dot( st, u_diffuseMatrixT ) );

	gl_ClipDistance[0] = dot( vec4( xyz, 1.0 ), u_clipPlane );
	gl_Position = u_mvpMatrix * vec4( xyz, 1.0 );
}
