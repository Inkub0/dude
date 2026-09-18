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
SAMPLER_BINDING(2) uniform sampler2D u_sceneColor;   // _currentRender copy (smoke-darkness blend)

VARY(0) in vec2 var_TexCoord;
VARY(1) in vec4 var_Color;

layout(location = 0) out vec4 fragColor;

void main() {
	vec2 depth_consts = DUDE_DEPTH_CONSTS();	// this view's pair (u_depthParms); falls back to the play-time constants
	 vec2 depthTc = gl_FragCoord.xy * u_depthTexRecip.xy;
	 float rawSceneDepth = min(texture(u_currentDepth, depthTc).x, 0.9994);

	 // recover linear eye depth (Doom units) for both the occluding scene and
	 // this fragment. Negative and growing into the screen; the RCP is why the
	 // ARB overwrote scene_depth in place — the fade below must use the *linear*
	 // scene depth, not the raw depth-buffer value.
	 float sceneDepth = 1.0 / (rawSceneDepth * depth_consts.x + depth_consts.y);
	 float particleDepth = 1.0 / (gl_FragCoord.z * depth_consts.x + depth_consts.y);

	 float fade = clamp((particleDepth - sceneDepth + u_particleRadius.x) * u_particleRadius.y, 0.0, 1.0);
	 float nearFade = clamp(particleDepth * -u_particleRadius.z, 0.0, 1.0);

	 vec4 fadeCol = clamp(vec4(nearFade * fade) + u_channelMask, 0.0, 1.0);
	 fragColor = texture(u_diffuseMap, var_TexCoord) * fadeCol * var_Color;

	 // DUDE smoke-darkness blend: dim smoke/steam/dust where the scene behind it is
	 // dark, so puffs fade into shadow instead of reading as grey blobs. Enabled by
	 // the backend (u_localParam0.x) for alpha-blended smoke and for additive
	 // particles whose material reads as smoke; additive fire/sparks/glares keep it
	 // at 0 and are left bright. Params: localParam0 (x = enable, y = floor opacity
	 // on black, z = knee luminance -> full), localParam1.xy (1/_currentRender size).
	 // See RB_RHI_RenderSoftParticleStage.
	 if (u_localParam0.x > 0.5) {
		 // u_localParam1.w flips the row on Vulkan (top-down gl_FragCoord vs the
		 // GL-layout _currentRender capture); 0 on GL, so the term is inert there
		 vec2 sceneTc = gl_FragCoord.xy * u_localParam1.xy + vec2(0.0, u_localParam1.w);
		 vec3 bg = texture(u_sceneColor, sceneTc).rgb;
		 float bgLum = dot(bg, vec3(0.299, 0.587, 0.114));
		 float lit = clamp(bgLum / u_localParam0.z, 0.0, 1.0);   // 0 on black -> 1 at knee
		 float opacity = mix(u_localParam0.y, 1.0, lit);          // floor..1
		 // additive smoke ('blend add') carries its visibility in RGB; alpha-blended
		 // smoke carries it in alpha. channelMask.a marks additive (backend sets
		 // additive -> (0,0,0,1), alpha -> (1,1,1,0)).
		 if (u_channelMask.a > 0.5) {
			 fragColor.rgb *= opacity;
		 } else {
			 fragColor.a *= opacity;
		 }
	 }
}
