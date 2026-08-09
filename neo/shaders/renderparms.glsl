// Shared per-draw uniform block for all shaders.
//
// Each member documents the ARB program.env[N] / program.local[N] slot it
// replaces. The ARB slots were overloaded with different meanings per program;
// here every purpose gets its own named member so one block layout serves all
// shaders. The backend fills only the members the bound shader consumes.

UBO_BINDING(0) uniform RenderParams {
	mat4 u_mvpMatrix;             // state.matrix.mvp / ARB_position_invariant
	mat4 u_modelViewMatrix;       // state.matrix.modelview   (heatHaze)
	mat4 u_projectionMatrix;      // state.matrix.projection  (heatHaze)

	vec4 u_localLightOrigin;      // vp env[4]
	vec4 u_localViewOrigin;       // vp env[5]
	vec4 u_lightProjectionS;      // vp env[6]
	vec4 u_lightProjectionT;      // vp env[7]
	vec4 u_lightProjectionQ;      // vp env[8]
	vec4 u_lightFalloffS;         // vp env[9]
	vec4 u_bumpMatrixS;           // vp env[10]
	vec4 u_bumpMatrixT;           // vp env[11]
	vec4 u_diffuseMatrixS;        // vp env[12]
	vec4 u_diffuseMatrixT;        // vp env[13]
	vec4 u_specularMatrixS;       // vp env[14]
	vec4 u_specularMatrixT;       // vp env[15]
	vec4 u_vertexColorModulate;   // vp env[16]
	vec4 u_vertexColorAdd;        // vp env[17]
	vec4 u_modelMatrixRow0;       // vp env[20] (ambientLight) / env[6] (bumpyEnvironment)
	vec4 u_modelMatrixRow1;       // vp env[21] / env[7]
	vec4 u_modelMatrixRow2;       // vp env[22] / env[8]

	vec4 u_diffuseModifier;       // fp env[0] (interaction, ambientLight)
	vec4 u_specularModifier;      // fp env[1] (interaction)
	vec4 u_screenCorrection;      // fp env[0] (post shaders: 1.0 -> _currentRender NPOT adjust)
	vec4 u_windowCoord;           // fp env[1] (post shaders: fragment.position -> 0..1)

	vec4 u_localParam0;           // program.local[0] (material stage parm: scroll / fraction)
	vec4 u_localParam1;           // program.local[1] (material stage parm: magnitude / target color)

	vec4 u_depthTexRecip;         // fp env[22] (soft particles: 1/currentDepth size + NPOT adjust)
	vec4 u_particleRadius;        // fp env[23] (soft particles: radius, 1/fadeRange, 1/radius)
	vec4 u_channelMask;           // fp env[24] (soft particles: additive vs alpha channel mask)

	vec4 u_color;                 // fixed-function glColor replacement (new shaders)
	vec4 u_alphaTest;             // x = alpha test ref, y != 0 -> test enabled (zfill/generic),
	                             // z = perforated shadow strength (shadow_sm*.frag dither)
	vec4 u_texGen0S;              // fixed-function texgen planes (fog/blendlight)
	vec4 u_texGen0T;
	vec4 u_texGen0Q;
	vec4 u_texGen1S;
	vec4 u_texGen1T;              // fog "enter" plane T (per-vertex fade); blendlight leaves this unused

	vec4 u_clipPlane;            // subview near-clip plane in model-local space
	                            // (zfill gl_ClipDistance[0]; 0 when unused)

	vec4 u_specularParms;        // interaction specular tuning (DUDE Phase 3.5):
	                            // x = scale, y = exponent, z = shading model
	                            // (0 LUT / 1 Blinn-Phong),
	                            // w = point-light cube shadow PCF tap count (1..16)

	vec4 u_shadowParms;          // x = technique (0 none / 1 projected-2D / 2 point-cube),
	                            // y = texel size (1/res), z = depth-compare bias,
	                            // w = light range (point-cube radial normalizer). The 2D
	                            // path reuses the light-projection texgen (S/T/Q + falloff);
	                            // the cube path uses the world-space light->frag direction.

	vec4 u_occlusionParms;       // DUDE material AO map (docs/occlusion-maps.md):
	                            // x = enable, y = ambient strength, z = direct-diffuse
	                            // strength. w = r_pbrEnvScale (PBR Phase C.1 metal
	                            // environment floor; rides the spare slot).
	                            // Map sampled on unit 10 (diffuse UV).

	vec4 u_pbrParms;             // DUDE PBR interaction path (docs/pbr-materials.md):
	                            // x = metalness (sanity-clamped to [0,1]),
	                            // y = roughness, z = enable (r_pbr, non-ambient
	                            // interactions only), w = specular energy scale
	                            // (r_pbrSpecScale). Phase A fills global fallbacks;
	                            // Phase B swaps in per-material values.
	vec4 u_pbrParms2;            // DUDE PBR extras: x = metal diffuse-kill strength
	                            // kd (= 1 - r_pbrMetalDiffuse). diffuse *= 1 - metal*kd,
	                            // so kd < 1 keeps the asset's albedo colour on metals
	                            // instead of the physical full kill. y = shadow
	                            // slope-scaled bias strength (r_shadowMapSlopeBias;
	                            // set in the shadow block, independent of r_pbr). zw spare.

	vec4 u_tessParms;            // DUDE tessellation (docs/tessellation.md):
	                            // x = tess level (subdivision cap; 1 = flat),
	                            // y = max view distance for the LOD falloff,
	                            // z = displacement strength (0 = pure PN smoothing),
	                            // w = min model-space edge length to subdivide.
	                            // Consumed by the .tesc / .tese stages only.

	vec4 u_parallaxParms;        // DUDE parallax occlusion mapping (docs/parallax.md):
	                            // x = enable, y = height depth in UV units,
	                            // z = min march steps (head-on), w = max march steps
	                            // (grazing). Height map on unit 11 (u_parallaxMap).
	vec4 u_parallaxParms2;       // DUDE parallax extras: x = self-shadow strength
	                            // (0 = off). yzw spare.

	vec4 u_shadowProjectionS;    // UNBAKED light-projection texgen for the 2D shadow-map
	vec4 u_shadowProjectionT;    // lookup. The cookie texgen (u_lightProjectionS/T/Q) carries
	vec4 u_shadowProjectionQ;    // the light stage's texture matrix (rotating fan gobos etc.);
	                            // the shadow map is rendered raw, so it must be sampled raw or
	                            // the shadow swims with the animation. Filled only on the
	                            // projected-2D shadow path (0 otherwise).
};
