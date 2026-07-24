// DUDE Phase 3.5 shadow-map caster pass. Renders occluder depth from a projected
// light's point of view. Depth is the light's linear falloff (distance along the
// light axis), written in the fragment shader — the very value interaction.frag
// compares against in shadowVisibility(). XY uses the same projection texgen as
// the light cookie, so the shadow map aligns exactly with the lit cone.

#include "renderparms.glsl"

layout(location = 0) in vec4 attr_Position;	// w defaults to 1 (vec3 attribute)

VARY(0) out float var_Falloff;

void main() {
	// light-projective coordinates in this surface's model space (the planes were
	// transformed per-surface by R_GlobalPlaneToLocal, same as the interaction pass)
	float s = dot( attr_Position, u_lightProjectionS );
	float t = dot( attr_Position, u_lightProjectionT );
	float q = dot( attr_Position, u_lightProjectionQ );
	var_Falloff = dot( attr_Position, u_lightFalloffS );

	// ndc.xy = 2*(s/q, t/q) - 1  → rasterize at (cookie UV * map size). z is unused
	// (gl_FragDepth overrides it); w = q clips anything behind the light apex.
	gl_Position = vec4( 2.0 * s - q, 2.0 * t - q, 0.0, q );
}
