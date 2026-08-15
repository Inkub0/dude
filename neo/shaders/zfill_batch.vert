// Phase 3.2b consume — Increment 2 (docs/gpu-offload-plan.md §3.2b): the batched,
// fully-bindless depth prepass. One vkCmdDrawIndirect draws the whole solid-opaque
// world-static bucket; each sub-draw's per-object data (vertex ptr, index ptr, MVP)
// lives in an SSBO addressed by gl_InstanceIndex (= the indirect command's
// firstInstance, since instanceCount is 1). BOTH vertices and indices are fetched
// through device-address pointers (GL_EXT_buffer_reference), so surfaces with
// different vertex/index buffers batch into one non-indexed draw with no bound vb/ib.
//
// Bit-identical to zfill.vert for these surfaces: same MVP bytes (RB_RHI_SpaceMvp,
// already VK z-remapped), same `invariant gl_Position` (injected by compile_spv.py),
// same `mvp * vec4(pos,1)`. Solid opaque only: no texcoord, no clip plane (0 here).

// idDrawVert as raw uints (stride 60 B = 15 uints): xyz @0..2.
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer VertRef { uint w[]; };
// glIndex_t index list. Hardcoded 32-bit to match the backend's VK_INDEX_TYPE_UINT32 bind
// and Model.h's glIndex_t == int; if glIndex_t ever became 16-bit this (and the VK index
// path) would need updating in lockstep.
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer IdxRef  { uint i[]; };

struct ObjRec {
	VertRef	vb;			// device address of idDrawVert[0] for this surface (+vertexOffset)
	IdxRef	ib;			// device address of the surface's first index (+firstIndex bytes)
	mat4	mvp;		// RB_RHI_SpaceMvp for the surface's space (VK z-remapped)
};
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer ObjArray { ObjRec o[]; };

layout(push_constant) uniform PushConstants {
	ObjArray objs;		// device address of the per-frame object array
} pc;

void main() {
	ObjRec obj = pc.objs.o[gl_InstanceIndex];
	uint idx = obj.ib.i[gl_VertexIndex];			// manual index fetch (non-indexed draw)
	uint b = idx * 15u;								// 60-byte idDrawVert stride / 4
	vec3 xyz = vec3( uintBitsToFloat( obj.vb.w[b + 0u] ),
	                 uintBitsToFloat( obj.vb.w[b + 1u] ),
	                 uintBitsToFloat( obj.vb.w[b + 2u] ) );
	gl_Position = obj.mvp * vec4( xyz, 1.0 );
}
