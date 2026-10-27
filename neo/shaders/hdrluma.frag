// HDR eye adaptation (Phase B1, docs/hdr-pipeline.md): level 0 of the luminance reduction.
// Sample the HDR scene and write its LOG luminance; hdrlumadown then box-averages this down
// the mip chain to a 1x1 texel = the average log-luma, whose exp() is the geometric-mean
// scene luminance (robust to a few very bright pixels). Rendered at the luma target's base
// resolution, so var_TexCoord (0..1) maps the whole scene into this coarse sampling grid.

// u_localParam0.x = central metering fraction (r_hdrAdaptCenter): the grid maps to this centred
// crop of the scene, so the periphery is ignored and what you look at drives the exposure.
// u_localParam1 = the part of the image the scene fills (offset.xy, scale.zw; 0,0,1,1 for a full
// view): a frame whose world view was rendered into a crop (Quake 4's double vision, RoE/LM's
// low-res fullscreen FX) meters only the crop's rows. Clamped half a texel inside it.

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_hdrScene;

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

void main() {
	vec2  uv   = 0.5 + ( var_TexCoord - 0.5 ) * u_localParam0.x;   // centred metering crop
	vec2  ht   = 0.5 / vec2( textureSize( u_hdrScene, 0 ) );
	uv = clamp( u_localParam1.xy + uv * u_localParam1.zw, u_localParam1.xy + ht, u_localParam1.xy + u_localParam1.zw - ht );
	vec3  c    = texture( u_hdrScene, uv ).rgb;
	float luma = dot( c, vec3( 0.2126, 0.7152, 0.0722 ) );
	// .r = log-luminance (floor keeps log() finite) — box-averaged down the chain to a geometric mean.
	// .g = LINEAR luminance — MAX'd down the chain to the scene's brightest metered spot, for the
	// experimental DUDE adaptive white point (r_hdrAdaptWhitePoint). Cheap: rides the same reduction.
	fragColor  = vec4( log( max( luma, 1e-4 ) ), luma, 0.0, 1.0 );
}
