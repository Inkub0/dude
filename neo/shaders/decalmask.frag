// DUDE decal surface mask. Writes what the surface under this decal KEEPS of its PBR response
// (channels in decalsurface.glsl): 1 = untouched, 0 = fully covered. Drawn with a multiply
// blend so overlapping decals compound.
//
// Coverage is what the decal's own blend would hide of the surface (u_localParam0.x):
//   0  alpha blend (src_alpha, 1-src_alpha)   -> alpha
//   1  darken (zero, 1-src_color)             -> luminance
//   2  filter / modulate (dst_color, zero)    -> 1 - luminance
// u_localParam0.yz = how hard coverage bites on metalness / smoothness. The target has no depth attachment, so occlusion is tested here
// against the sealed scene depth (_currentDepth), in linear view units like softparticle.frag.

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_map;
SAMPLER_BINDING(1) uniform sampler2D u_currentDepth;

VARY(0) in vec2 var_TexCoord;
VARY(1) in vec4 var_Color;

layout(location = 0) out vec4 fragColor;

void main() {
	vec2 depth_consts = DUDE_DEPTH_CONSTS();
	float rawScene = min( texture( u_currentDepth, gl_FragCoord.xy * u_depthTexRecip.xy ).x, 0.9994 );
	// linear eye depth, negative and growing into the screen
	float sceneZ = 1.0 / ( rawScene * depth_consts.x + depth_consts.y );
	float decalZ = 1.0 / ( min( gl_FragCoord.z, 0.9994 ) * depth_consts.x + depth_consts.y );
	// the decal is coplanar with its wall (no polygon offset here): keep it unless something
	// sits clearly in front. Tolerance grows with distance to ride out depth quantisation.
	if ( decalZ < sceneZ - ( 1.0 + 0.01 * abs( sceneZ ) ) ) {
		discard;
	}

	vec4 c = clamp( texture( u_map, var_TexCoord ) * var_Color, 0.0, 1.0 );
	float lum = dot( c.rgb, vec3( 0.2126, 0.7152, 0.0722 ) );
	float cov = c.a;
	if ( u_localParam0.x > 1.5 ) {
		cov = 1.0 - lum;
	} else if ( u_localParam0.x > 0.5 ) {
		cov = lum;
	}
	fragColor = vec4( 1.0 - clamp( cov * u_localParam0.yz, 0.0, 1.0 ), 1.0, 1.0 );
}
