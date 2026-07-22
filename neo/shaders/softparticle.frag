// Translated from the soft-particle ARB fragment shader embedded in
// draw_arb2.cpp (from The Dark Mod 2.04, (C) Broken Glass Studios).
// Fades particles where they approach scene geometry, using _currentDepth.
//
// CAUTION: depth_consts is derived from Doom 3's hard-coded near-infinite-zFar
// projection matrix in GL clip conventions. The Vulkan backend's 0..1 depth
// projection changes these constants — recompute them there (see README).

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_diffuseMap;   // particle diffuse
SAMPLER_BINDING(1) uniform sampler2D u_currentDepth; // _currentDepth copy

VARY(0) in vec2 var_TexCoord;
VARY(1) in vec4 var_Color;

layout(location = 0) out vec4 fragColor;

void main() {
	const vec2 depth_consts = vec2(0.33333333, -0.33316667);
	 vec2 depthTc = gl_FragCoord.xy * u_depthTexRecip.xy;
	 float sceneDepth = min(texture(u_currentDepth, depthTc).x, 0.9994);

	 float invScene = 1.0 / (sceneDepth * depth_consts.x + depth_consts.y);
	 float particleDepth = 1.0 / (gl_FragCoord.z * depth_consts.x + depth_consts.y);

	 float fade = clamp((particleDepth - sceneDepth + u_particleRadius.x) * u_particleRadius.y, 0.0, 1.0);
	 float nearFade = clamp(particleDepth * -u_particleRadius.z, 0.0, 1.0);

	 vec4 fadeCol = clamp(vec4(nearFade * fade) + u_channelMask, 0.0, 1.0);
	 fragColor = texture(u_diffuseMap, var_TexCoord) * fadeCol * var_Color;
}
