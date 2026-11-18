// New shader (was fixed function): textured draw modulated by color,
// with optional alpha test.

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_map;

VARY(0) in vec2 var_TexCoord;
VARY(1) in vec4 var_Color;

#ifdef DUDE_GENERIC_PARALLAX
// generic_parallax.frag (docs/parallax.md "Pass parity"): an emissive / overlay stage of a surface whose lit passes
// march its relief samples where they shifted it, so a glow stays on the panel it lights
SAMPLER_BINDING(11) uniform sampler2D u_parallaxMap;
VARY(4) in vec2 var_TexBump;		// the height's UV (generic.vert)
VARY(5) in vec3 var_TexViewVec;
// parallaxUV / dudeParallaxCarry (needs u_parallaxMap above)
#include "parallax.glsl"
#endif

layout(location = 0) out vec4 fragColor;

void main() {
	vec2 uv = var_TexCoord;
#ifdef DUDE_GENERIC_PARALLAX
	if ( u_parallaxParms.x > 0.5 ) {
		vec2 hDx = dFdx( var_TexBump ), hDy = dFdy( var_TexBump );
		vec2 off = parallaxUV( var_TexBump, normalize( var_TexViewVec ), hDx, hDy );
		uv += dudeParallaxCarry( off, hDx, hDy, dFdx( var_TexCoord ), dFdy( var_TexCoord ) );
	}
#endif
	vec4 c = texture( u_map, uv ) * var_Color;
	if ( u_alphaTest.y != 0.0 && c.a < u_alphaTest.x ) {
		discard;
	}
	// HDR overbright saturation compensation (r_hdrOverbrightSat via localParam0.x > 1): the tonemap
	// desaturates the very bright colours overbright pushes into, so pre-saturate here. Pushing the
	// colour away from its own luma boosts saturation; white (luma == colour) is untouched. 0 (the
	// default for every other generic stage) is a no-op.
	if ( u_localParam0.x > 1.0 ) {
		float luma = dot( c.rgb, vec3( 0.2126, 0.7152, 0.0722 ) );
		c.rgb = max( mix( vec3( luma ), c.rgb, u_localParam0.x ), 0.0 );
	}
	fragColor = c;
}
