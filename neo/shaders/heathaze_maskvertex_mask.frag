// Fragment half for the maskvertex-VS + mask-FS pairing (see
// heathaze_maskvertex_mask.vert). Identical to heathaze_mask.frag: the mask is
// NOT scaled by vertex color here (that is the difference from
// heathaze_maskvertex.frag), matching heatHazeWithMask.vfp's fragment program.

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_currentRender;
SAMPLER_BINDING(1) uniform sampler2D u_normalMap;
SAMPLER_BINDING(2) uniform sampler2D u_maskMap;

VARY(0) in vec2 var_TexMask;
VARY(1) in vec2 var_TexDistort;
VARY(2) in vec2 var_DeformMag;

layout(location = 0) out vec4 fragColor;

void main() {
	// kill the pixel if the distortion wound up being very small
	vec2 mask = texture( u_maskMap, var_TexMask ).xy;
	if ( mask.x - 0.01 < 0.0 || mask.y - 0.01 < 0.0 ) {
		discard;
	}

	vec4 bump = texture( u_normalMap, var_TexDistort );
	bump.x = bump.a;
	vec2 localNormal = ( bump.xy * 2.0 - 1.0 ) * mask;

	// u_windowCoord.w flips the row on Vulkan (top-down gl_FragCoord vs the
	// GL-layout _currentRender capture); 0 on GL, so the term is inert there
	vec2 screenTc = gl_FragCoord.xy * u_windowCoord.xy + vec2( 0.0, u_windowCoord.w );
	screenTc = clamp( localNormal * var_DeformMag + screenTc, 0.0, 1.0 );
	screenTc *= u_screenCorrection.xy;

	fragColor = vec4( texture( u_currentRender, screenTc ).xyz, 1.0 );
}
