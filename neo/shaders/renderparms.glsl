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

	vec4 u_depthTexRecip;         // fp env[22] (soft particles: 1/currentDepth size, .zw the view's first pixel in a cropped Vulkan view)
	vec4 u_particleRadius;        // fp env[23] (soft particles: radius, 1/fade depth in front of the scene)
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
	                            // (0 = off), y = height source (0 = runtime field R in
	                            // bump space, 1 = sidecar ALPHA in diffuse space;
	                            // parallax.glsl), z = the height level kept on
	                            // the polygon (0.5 runtime field, a sidecar's main
	                            // face). w spare.

	vec4 u_shadowProjectionS;    // UNBAKED light-projection texgen for the 2D shadow-map
	vec4 u_shadowProjectionT;    // lookup. The cookie texgen (u_lightProjectionS/T/Q) carries
	vec4 u_shadowProjectionQ;    // the light stage's texture matrix (rotating fan gobos etc.);
	                            // the shadow map is rendered raw, so it must be sampled raw or
	                            // the shadow swims with the animation. Filled only on the
	                            // projected-2D shadow path (0 otherwise).
	vec4 u_shadowFalloffS;       // DUDE sun shadows: the virtual projection's depth plane —
	                            // the compare reference for the mode-3 (sun) shadow lookup.
	                            // Filled only on the sun path (0 otherwise).

	mat4 u_prevMvpMatrix;        // DUDE motion vectors (docs/fsr-temporal-pipeline.md R1/A2): the
	                            // PREVIOUS rendered frame's model->clip for this surface's space
	                            // (un-jittered). gbuffer.{vert,tese} write the per-object screen
	                            // velocity (currUV - prevUV, +Y-up) into the 3rd MRT. Filled only by
	                            // the VK normal prepass when r_motionVectors is on; 0 elsewhere.

	vec4 u_rtParms;              // DUDE RT sun shadows (docs/rtx-shadow-roadmap.md R3): xy = the
	                            // scene TLAS device address as raw bit patterns (floatBitsToUint),
	                            // z = ray-origin normal offset (world units), w = max ray length.
	                            // Read only by interaction_rt.frag when shadowParms.x == 4.

	vec4 u_depthParms;           // window depth -> view z for THIS view: 1/vz = raw * x + y, from its
	                            // projection ( -2/P[14], (1-P[10])/P[14] ). NOT a constant: the
	                            // near plane is r_znear = 3 in play - which gives the pair
	                            // (0.33333333, -0.33316667) every depth-reading shader used to
	                            // hard-code - but the game sets r_znear to 1 while a cinematic
	                            // camera is active (idGameLocal::SetCamera), where the constants
	                            // put every reconstructed point 3x too far away (and
	                            // renderView.cramZNear would quarter it again). x == 0 = not provided:
	                            // use DUDE_DEPTH_CONSTS( ) below, which falls back to the play pair.

	vec4 u_skinParms;            // DUDE skin / flesh subsurface scattering (docs/skin-shading.md):
	                            // x = blend strength (0 = off), yzw = per-channel diffuse wrap widths
	                            // (red widest, so the shading terminator warms to red like real skin).
	                            // interaction.frag adds the wrapped-diffuse excess over Lambert; 0 on
	                            // every non-skin/flesh, ambient or film draw. Appended LAST.

	vec4 u_contactParms;         // DUDE screen-space contact shadows (docs/contact-shadows.md):
	                            // x = march step count (0 = off), y = march length (world units),
	                            // z = depth-compare thickness (world units), w = darkening strength
	                            // (0..1). interaction.frag marches _currentDepth (unit 16) from the
	                            // fragment toward the light; 0 on ambient/film/every non-enabled draw.

	vec4 u_tessParms2;           // DUDE GPU-skin weld decoupling (docs/gpu-offload-plan.md): x = 1 when this
	                            // draw's gpuSkinVB normal was seam-WELDED (PN tessellation stays crack-free);
	                            // the lit vertex shaders then rebuild the UN-welded normal from the tangents
	                            // for LIGHTING only (dudeUnweldedLightNormal), so the shading keeps no
	                            // hard-edge "cut". 0 = use attr_Normal as-is. yzw spare.

	vec4 u_vertexColorModulate2; // ReSTIR DI/RTGI albedo capture: the SECOND diffuse stage's vertex-colour mode
	vec4 u_vertexColorAdd2;      // (u_vertexColorModulate/Add is the FIRST stage's). gbuffer.frag weights each
	                            // diffuse stage of a vertex-COLOUR BLEND by its OWN term, so sand*vertexColor +
	                            // rock*inverseVertexColor is captured as the forward path blends it.

	vec4 u_ormParms;             // DUDE Phase D PBR sidecar (docs/pbr-materials.md sec. 7): unit 10 (u_occlusionMap)
	                            // holds a packed texture. x = layout (0 = plain AO map in R, 1 = ORM: R AO / G rough /
	                            // B metal, 2 = RMAO: R rough / G metal / B AO), y = 1: its roughness/metalness replace
	                            // u_pbrParms.yx. z = 1: the PBR editor's live channel edit applies (u_ormAdj*). w = 1: write the AO to the material attachment for r_pbrDebugView. Read through dudeUnit10Ao / dudeUnit10RoughMetal below.
	vec4 u_ormAdjB;              // in-game PBR editor live channel edit (u_ormParms.z): brightness, contrast, gamma;
	vec4 u_ormAdjC;              // xyz = AO, roughness, metalness. Same formula as "Save to texture"
	vec4 u_ormAdjG;              // (R_PbrSidecarAdjustValue).
	vec4 u_ormGlobB;             // global sidecar adjust (r_pbrSidecarAO / Rough / Metal), applied to every sidecar
	vec4 u_ormGlobC;             // (u_ormParms.x >= 1) BEFORE the per-texture edit; preview of the extractor's
	vec4 u_ormGlobG;             // --*-adjust flags. Identity (0, 1, 1) when off.
	vec4 u_ormBakedB;            // what the sidecars were baked with (pbr/pbr_sidecar_baked.cfg), undone before the
	vec4 u_ormBakedC;            // global adjust applies: sliders equal to the bake show the textures as baked.
	vec4 u_ormBakedG;            // Identity (0, 1, 1) when none recorded.
	vec4 u_contactParms2;        // DUDE contact shadows: x = tan(penumbra cone half-angle) (r_contactShadowSoftness),
	                             // y = grazing-angle start bias (r_contactShadowGrazeBias, 0 = off),
	                             // z = distance-fade max (r_contactShadowMaxDist, 0 = off), w spare. Appended LAST.
};

float dudeOrmAdjust( float v, float b, float c, float g ) {
	return pow( clamp( ( v - 0.5 ) * c + 0.5 + b, 0.0, 1.0 ), 1.0 / max( g, 0.05 ) );
}

// dudeOrmAdjust undone (exact where the bake did not clip; a clipped value stays at its end)
float dudeOrmUnadjust( float v, float b, float c, float g ) {
	float x = pow( clamp( v, 0.0, 1.0 ), max( g, 0.05 ) );
	return ( c > 0.001 ) ? clamp( ( x - 0.5 - b ) / c + 0.5, 0.0, 1.0 ) : 0.5;
}

// the global step on channel ch (0 AO, 1 rough, 2 metal): back to the un-adjusted bake, then the sliders
float dudeOrmGlobal( float v, int ch ) {
	v = dudeOrmUnadjust( v, u_ormBakedB[ch], u_ormBakedC[ch], u_ormBakedG[ch] );
	return dudeOrmAdjust( v, u_ormGlobB[ch], u_ormGlobC[ch], u_ormGlobG[ch] );
}

// Unit 10's AO channel: R for a plain AO map and an ORM sidecar, B for an RMAO one.
float dudeUnit10Ao( vec4 t ) {
	float a = ( u_ormParms.x > 1.5 ) ? t.b : t.r;
	if ( u_ormParms.x > 0.5 ) {		// a sidecar (not a plain AO map): the global adjust
		a = dudeOrmGlobal( a, 0 );
	}
	return ( u_ormParms.z > 0.5 ) ? dudeOrmAdjust( a, u_ormAdjB.x, u_ormAdjC.x, u_ormAdjG.x ) : a;
}

// A unit-10 PBR sidecar's ( roughness, metalness ).
vec2 dudeUnit10RoughMetal( vec4 t ) {
	vec2 rm = ( u_ormParms.x > 1.5 ) ? t.rg : t.gb;
	rm.x = dudeOrmGlobal( rm.x, 1 );		// global adjust (identity when the sliders equal the bake)
	rm.y = dudeOrmGlobal( rm.y, 2 );
	if ( u_ormParms.z > 0.5 ) {
		rm.x = dudeOrmAdjust( rm.x, u_ormAdjB.y, u_ormAdjC.y, u_ormAdjG.y );
		rm.y = dudeOrmAdjust( rm.y, u_ormAdjB.z, u_ormAdjC.z, u_ormAdjG.z );
	}
	return rm;
}

// GPU-skin weld decoupling (u_tessParms2.x): when the skinned vertex normal was seam-welded for
// crack-free PN tessellation, rebuild the un-welded per-vertex normal for LIGHTING from the
// (un-welded) tangents. The derive frame is orthonormal, so cross(T,B) is +/- the face normal;
// orient it to the welded normal (same hemisphere within the weld angle) to recover the exact
// un-welded normal. A no-op (returns n) when the flag is off or the tangents are degenerate.
vec3 dudeUnweldedLightNormal( vec3 n, vec3 t, vec3 b ) {
	if ( u_tessParms2.x == 0.0 ) {
		return n;
	}
	vec3 c = cross( t, b );
	float l = length( c );
	if ( l < 1e-8 ) {
		return n;
	}
	c /= l;
	return ( dot( c, n ) < 0.0 ) ? -c : c;
}

// the depth-linearisation pair for this draw (see u_depthParms)
#define DUDE_DEPTH_CONSTS() ( ( u_depthParms.x != 0.0 ) ? u_depthParms.xy : vec2( 0.33333333, -0.33316667 ) )
