// New shader (was fixed function): depth prepass fill, with texcoords for
// the optional alpha-tested (perforated) surfaces.

#include "renderparms.glsl"

layout(location = 0) in vec4 attr_Position;
layout(location = 1) in vec2 attr_TexCoord;

VARY(0) out vec2 var_TexCoord;

void main() {
	vec4 st = vec4( attr_TexCoord, 0.0, 1.0 );
	var_TexCoord = vec2( dot( st, u_diffuseMatrixS ), dot( st, u_diffuseMatrixT ) );

	// subview near-clip plane (mirrors / camera views): signed distance in
	// model-local space. Ignored unless the backend enables GL_CLIP_DISTANCE0,
	// and u_clipPlane is 0 for ordinary (non-subview) depth fills anyway.
	gl_ClipDistance[0] = dot( vec4( attr_Position.xyz, 1.0 ), u_clipPlane );

	gl_Position = u_mvpMatrix * attr_Position;
}
