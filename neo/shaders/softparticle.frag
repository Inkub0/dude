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
SAMPLER_BINDING(2) uniform sampler2D u_sceneColor;   // _currentRender copy (smoke-darkness blend, lit smoke)
SAMPLER_BINDING(3) uniform sampler2D u_smokeLight;   // lit smoke: Path Tracing's albedo-free DI buffer (read when localParam1.z > 0)

// DUDE lit smoke (r_smokeLit): the light around a puff, averaged over five taps spread across its size on screen, so
// the smoke takes the room's light rather than stencilling the detail of the wall behind it. Path Tracing
// (u_localParam1.z > 0): the DI buffer - direct + one bounce, no surface colour (1 = NRD YCoCg, 2 = linear), times its
// scale and a mid-grey 0.5 so its level compares with a lit background. Otherwise the background capture, half
// desaturated: it carries the wall's own colour, which should not paint the smoke.
vec3 smokeLightTap( vec2 frag ) {
	if ( u_localParam1.z > 0.5 ) {
		vec3 c = texture( u_smokeLight, frag * u_screenCorrection.xy ).rgb;
		if ( u_localParam1.z < 1.5 ) {
			float t = c.x - c.z;
			c = vec3( t + c.y, c.x + c.z, t - c.y );		// NRD YCoCg -> linear (as ambientlight.frag's floor)
		}
		return max( c, vec3( 0.0 ) ) * ( u_screenCorrection.z * 0.5 );
	}
	return texture( u_sceneColor, frag * u_localParam1.xy + vec2( 0.0, u_localParam1.w ) ).rgb;
}

vec3 smokeLight( float particleDist ) {
	// the puff's radius on screen, 4..64 px (windowCoord.x = focal length in pixels)
	float r = clamp( u_particleRadius.x * u_windowCoord.x / max( particleDist, 1.0 ), 4.0, 64.0 );
	vec2 f = gl_FragCoord.xy;
	vec3 c = smokeLightTap( f ) + smokeLightTap( f + vec2( r, 0.0 ) ) + smokeLightTap( f - vec2( r, 0.0 ) )
	       + smokeLightTap( f + vec2( 0.0, r ) ) + smokeLightTap( f - vec2( 0.0, r ) );
	c *= 0.2;
	if ( u_localParam1.z < 0.5 ) {
		float l = dot( c, vec3( 0.2126, 0.7152, 0.0722 ) );
		c = mix( vec3( l ), c, 0.5 );
	}
	return c;
}

// smoke density: the scene behind the puff (the pre-smoke capture), blurred over half the puff's radius on screen -
// what scattering through the smoke leaves of its detail. Centre + an 8-tap ring.
vec3 sceneBlur( float particleDist ) {
	float r = 0.5 * clamp( u_particleRadius.x * u_windowCoord.x / max( particleDist, 1.0 ), 4.0, 64.0 );
	vec2 f = gl_FragCoord.xy;
	vec3 c = vec3( 0.0 );
	c += texture( u_sceneColor, f * u_localParam1.xy + vec2( 0.0, u_localParam1.w ) ).rgb;
	for ( int i = 0; i < 8; i++ ) {
		float a = float( i ) * 0.785398;	// 45 degrees
		vec2 o = vec2( cos( a ), sin( a ) ) * r;
		c += texture( u_sceneColor, ( f + o ) * u_localParam1.xy + vec2( 0.0, u_localParam1.w ) ).rgb;
	}
	return c / 9.0;
}

VARY(0) in vec2 var_TexCoord;
VARY(1) in vec4 var_Color;

layout(location = 0) out vec4 fragColor;

void main() {
	vec2 depth_consts = DUDE_DEPTH_CONSTS();	// this view's pair (u_depthParms); falls back to the play-time constants
	 // .zw: the view's first pixel in the target (a cropped view on Vulkan; else 0)
	 vec2 depthTc = ( gl_FragCoord.xy - u_depthTexRecip.zw ) * u_depthTexRecip.xy;
	 float rawSceneDepth = min(texture(u_currentDepth, depthTc).x, 0.9994);

	 // recover linear eye depth (Doom units) for both the occluding scene and
	 // this fragment. Negative and growing into the screen; the RCP is why the
	 // ARB overwrote scene_depth in place — the fade below must use the *linear*
	 // scene depth, not the raw depth-buffer value.
	 float sceneDepth = 1.0 / (rawSceneDepth * depth_consts.x + depth_consts.y);
	 float particleDepth = 1.0 / (gl_FragCoord.z * depth_consts.x + depth_consts.y);

	 // DUDE: the particle is drawn depth-tested, as without softening, and fades out only over the last
	 // few units in front of what it cuts into (1/u_particleRadius.y; 0 where it meets it), so only the
	 // hard intersection line goes. The Dark Mod's drew with no depth test and faded a sphere of the
	 // particle's radius about the sprite (plus a fade near the viewer), which let it show through
	 // anything nearer than that radius.
	 float fade = clamp((particleDepth - sceneDepth) * u_particleRadius.y, 0.0, 1.0);

	 vec4 fadeCol = clamp(vec4(fade) + u_channelMask, 0.0, 1.0);
	 fragColor = texture(u_diffuseMap, var_TexCoord) * fadeCol * var_Color;
	 // DUDE smoke density (below): how thick the puff is, from the puff itself before any light scales its colour
	 float density = clamp( max( fragColor.r, max( fragColor.g, fragColor.b ) ) * u_windowCoord.y, 0.0, 0.95 );

	 // DUDE lit smoke (r_smokeLit = u_localParam0.w): the smoke takes the HUE of the light around it and its LEVEL
	 // relative to the reference (u_screenCorrection.w: authored brightness there, up to 2x in brighter light, dimmer
	 // in darker), blended in by the strength. Same particles as the darkness blend (the backend decides both). Hue and
	 // level by the brightest channel, not luminance: a saturated light (pure blue) then gives hue (0,0,1) at its own
	 // level instead of blowing the blue channel up 14x.
	 if (u_localParam0.w > 0.0) {
		 vec3 light = smokeLight( -particleDepth );
		 float level = max( light.r, max( light.g, light.b ) );
		 vec3 hue = level > 1e-4 ? light / level : vec3( 1.0 );
		 vec3 lit = hue * clamp( level / u_screenCorrection.w, 0.0, 2.0 );
		 fragColor.rgb *= mix( vec3( 1.0 ), lit, u_localParam0.w );
	 }

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
		 density *= opacity;
	 }

	 // DUDE smoke density (r_smokeDensity; u_windowCoord.y = gain, 0 = off): ADDITIVE smoke only ever adds light, so
	 // what is behind a puff stays sharp and just gets brighter. With a gain the backend draws the puff premultiplied
	 // (ONE, ONE_MINUS_SRC_ALPHA): alpha = its density hides that share of the scene behind it, and most of that share
	 // comes back as the scene behind BLURRED over part of the puff's size - the light scattered forward through the
	 // smoke - so detail behind a thick puff softens and dims while its own glow (the RGB above) stays. The rest is
	 // absorbed (SMOKE_SCATTER). The blur reads the pre-smoke capture, so stacked puffs each blur the unsmoked scene.
	 if (u_windowCoord.y > 0.0 && u_channelMask.a > 0.5) {
		 const float SMOKE_SCATTER = 0.7;
		 fragColor.rgb += density * SMOKE_SCATTER * sceneBlur( -particleDepth );
		 fragColor.a = density;
	 } else if (u_windowCoord.y > 0.0) {
		 // ALPHA smoke (barrelpoof - the dark smoke over fires and burning monsters) already hides what is behind it by
		 // its alpha, but what shows through stays sharp. Replace a share b of that see-through part with the scene
		 // behind BLURRED - none at the thin edges, most of it in the dense core: b = (1 - a) * saturate( a * gain ).
		 // Drawn with its own SRC_ALPHA blend: out = a * rgb + b * blur + (1 - a - b) * dst, energy-preserving (detail
		 // softens, the brightness stays).
		 float a = fragColor.a;
		 float b = ( 1.0 - a ) * clamp( a * u_windowCoord.y, 0.0, 1.0 );
		 float a2 = a + b;
		 if ( a2 > 1e-4 ) {
			 fragColor.rgb = ( a * fragColor.rgb + b * sceneBlur( -particleDepth ) ) / a2;
			 fragColor.a = a2;
		 }
	 }
}
