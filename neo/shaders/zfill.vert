// New shader (was fixed function): depth prepass fill, with texcoords for
// the optional alpha-tested (perforated) surfaces.

#include "renderparms.glsl"

layout(location = 0) in vec4 attr_Position;
layout(location = 1) in vec2 attr_TexCoord;
layout(location = 2) in vec3 attr_Normal;
// DUDE tessellation (docs/tessellation.md): the alpha channel carries the
// per-vertex UV-seam displacement mask idMD5Mesh stamps in (see tess.glsl).
// Unused otherwise by this stage.
layout(location = 5) in vec4 attr_Color;

VARY(0) out vec2 var_TexCoord;
// model-space position + normal for the tessellation stages (DUDE tessellation,
// docs/tessellation.md); var_TexBump feeds Phase 2 displacement. All unconsumed by
// zfill.frag in the flat pipeline.
VARY(1) out vec3 var_ModelPos;
VARY(2) out vec4 var_ModelNormal;	// .w = UV-seam displacement mask
VARY(3) out vec2 var_TexBump;

void main() {
	var_ModelPos = attr_Position.xyz;
	var_ModelNormal = vec4( attr_Normal, attr_Color.a );

	vec4 st = vec4( attr_TexCoord, 0.0, 1.0 );
	var_TexCoord = vec2( dot( st, u_diffuseMatrixS ), dot( st, u_diffuseMatrixT ) );
	var_TexBump = vec2( dot( st, u_bumpMatrixS ), dot( st, u_bumpMatrixT ) );

	// subview near-clip plane (mirrors / camera views): signed distance in
	// model-local space. Ignored unless the backend enables GL_CLIP_DISTANCE0,
	// and u_clipPlane is 0 for ordinary (non-subview) depth fills anyway.
	gl_ClipDistance[0] = dot( vec4( attr_Position.xyz, 1.0 ), u_clipPlane );

	gl_Position = u_mvpMatrix * attr_Position;
}
