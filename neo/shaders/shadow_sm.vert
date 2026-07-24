// DUDE Phase 3.5 shadow-map caster pass. Renders occluder depth from a projected
// light's point of view. Depth is the light's linear falloff (distance along the
// light axis), written in the fragment shader — the very value interaction.frag
// compares against in shadowVisibility(). XY uses the same projection texgen as
// the light cookie, so the shadow map aligns exactly with the lit cone.

#include "renderparms.glsl"

layout(location = 0) in vec4 attr_Position;	// w defaults to 1 (vec3 attribute)
layout(location = 1) in vec2 attr_TexCoord;

VARY(0) out float var_Falloff;
VARY(1) out vec2 var_TexCoord;			// diffuse UV for perforated (alpha-tested) casters

void main() {
	// light-projective coordinates in this surface's model space (the planes were
	// transformed per-surface by R_GlobalPlaneToLocal, same as the interaction pass)
	float s = dot( attr_Position, u_lightProjectionS );
	float t = dot( attr_Position, u_lightProjectionT );
	float q = dot( attr_Position, u_lightProjectionQ );
	var_Falloff = dot( attr_Position, u_lightFalloffS );

	// coverage lookup for grates/fences/foliage; opaque casters bind white + a
	// disabled alpha test, so this is harmless there
	vec4 st = vec4( attr_TexCoord, 0.0, 1.0 );
	var_TexCoord = vec2( dot( st, u_diffuseMatrixS ), dot( st, u_diffuseMatrixT ) );

	// ndc.xy = 2*(s/q, t/q) - 1  → rasterize at (cookie UV * map size). z is unused
	// (gl_FragDepth overrides it); w = q clips anything behind the light apex.
	gl_Position = vec4( 2.0 * s - q, 2.0 * t - q, 0.0, q );
}
