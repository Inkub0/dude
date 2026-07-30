// DUDE Phase 3.5 point-light (omni) shadow-map caster pass — one cube face.
// Renders occluder depth from a point light's point of view. Depth is the linear
// radial distance from the light, normalized by the light range, written in the
// fragment shader — the same value interaction.frag compares against when it
// samples the cube. u_mvpMatrix is the 90-degree view-projection for this face.

#include "renderparms.glsl"

layout(location = 0) in vec4 attr_Position;	// w defaults to 1 (vec3 attribute)
layout(location = 1) in vec2 attr_TexCoord;

VARY(0) out vec3 var_LightVec;		// light-relative position; fragment takes length()
VARY(1) out vec2 var_TexCoord;		// diffuse UV for perforated (alpha-tested) casters

void main() {
	// world-oriented, light-relative position: rotate the model-space
	// (vertex - light) vector by the model->world rotation. Translation cancels in
	// the difference, so u_modelMatrixRow*.xyz (the rotation) is enough. This is the
	// exact space interaction.frag samples the cube in (var_ShadowCubeVec).
	vec3 d = attr_Position.xyz - u_localLightOrigin.xyz;
	vec3 lr = vec3( dot( u_modelMatrixRow0.xyz, d ),
	                dot( u_modelMatrixRow1.xyz, d ),
	                dot( u_modelMatrixRow2.xyz, d ) );

	// Pass the POSITION and let the fragment shader compute length(). Interpolating
	// the scalar length() itself (a convex function) overestimates depth inside big
	// triangles — catastrophically when the light sits close to a large caster (a
	// grate card next to its bulb stored ~corner distance at its center), carving a
	// shadowless dead zone around the light that no bias could close. The vector
	// interpolates linearly (it IS the position), so per-fragment length is exact —
	// mirroring the receiver side (interaction.frag var_ShadowCubeVec).
	var_LightVec = lr;

	// coverage lookup for grates/fences; opaque casters bind white + a disabled test
	vec4 st = vec4( attr_TexCoord, 0.0, 1.0 );
	var_TexCoord = vec2( dot( st, u_diffuseMatrixS ), dot( st, u_diffuseMatrixT ) );

	gl_Position = u_mvpMatrix * vec4( lr, 1.0 );
}
