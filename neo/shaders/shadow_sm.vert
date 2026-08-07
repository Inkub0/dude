// DUDE Phase 3.5 shadow-map caster pass. Renders occluder depth from a projected
// light's point of view. Depth is the light's linear falloff (distance along the
// light axis), written in the fragment shader — the very value interaction.frag
// compares against in shadowVisibility(). XY uses the same projection texgen as
// the light cookie, so the shadow map aligns exactly with the lit cone.

#include "renderparms.glsl"

layout(location = 0) in vec4 attr_Position;	// w defaults to 1 (vec3 attribute)
layout(location = 1) in vec2 attr_TexCoord;
layout(location = 2) in vec3 attr_Normal;

VARY(0) out float var_Falloff;
VARY(1) out vec2 var_TexCoord;			// diffuse UV for perforated (alpha-tested) casters
// model-space control net for the tessellation stages (DUDE tessellation): a
// tessellated (PN-smoothed + displaced) character must cast from its DEFORMED
// surface here too, or its shadow keeps the low-poly silhouette while the lit body
// is rounded. shadow_sm.tese runs the same dudeTessPN + dudeTessDisplace as
// zfill.tese. Unconsumed by shadow_sm.frag in the flat pipeline.
VARY(2) out vec3 var_ModelPos;
VARY(3) out vec3 var_ModelNormal;
VARY(4) out vec2 var_TexBump;

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

	var_ModelPos = attr_Position.xyz;
	var_ModelNormal = attr_Normal;
	var_TexBump = vec2( dot( st, u_bumpMatrixS ), dot( st, u_bumpMatrixT ) );

	// ndc.xy = 2*(s/q, t/q) - 1  → rasterize at (cookie UV * map size). z is unused
	// (gl_FragDepth overrides it); w = q clips anything behind the light apex.
	gl_Position = vec4( 2.0 * s - q, 2.0 * t - q, 0.0, q );
}
