/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.h for license header)
===========================================================================
*/

#ifndef __RENDERPARAMS_H__
#define __RENDERPARAMS_H__

// C++ mirror of the shared per-draw uniform block in shaders/renderparms.glsl.
// The block is all mat4/vec4 members, so the std140 layout is trivially packed
// (no padding anywhere) and this plain struct matches it byte for byte on
// every backend — asserted below. Keep member order in lockstep with the GLSL.
//
// Member comments documenting which ARB program.env[N]/local[N] slot each
// value replaces live in renderparms.glsl.

namespace rhi {

struct RenderParams {
	float	mvpMatrix[16];
	float	modelViewMatrix[16];
	float	projectionMatrix[16];

	float	localLightOrigin[4];
	float	localViewOrigin[4];
	float	lightProjectionS[4];
	float	lightProjectionT[4];
	float	lightProjectionQ[4];
	float	lightFalloffS[4];
	float	bumpMatrixS[4];
	float	bumpMatrixT[4];
	float	diffuseMatrixS[4];
	float	diffuseMatrixT[4];
	float	specularMatrixS[4];
	float	specularMatrixT[4];
	float	vertexColorModulate[4];
	float	vertexColorAdd[4];
	float	modelMatrixRow0[4];
	float	modelMatrixRow1[4];
	float	modelMatrixRow2[4];

	float	diffuseModifier[4];
	float	specularModifier[4];
	float	screenCorrection[4];
	float	windowCoord[4];

	float	localParam0[4];
	float	localParam1[4];

	float	depthTexRecip[4];
	float	particleRadius[4];
	float	channelMask[4];

	float	color[4];
	float	alphaTest[4];
	float	texGen0S[4];
	float	texGen0T[4];
	float	texGen0Q[4];
	float	texGen1S[4];
	float	texGen1T[4];

	float	clipPlane[4];	// subview near-clip plane in model-local space
							// (zfill gl_ClipDistance; 0 when no clip plane)

	float	specularParms[4];	// interaction specular tuning: x = scale, y = exponent,
								// z = shading model (0 LUT / 1 Blinn-Phong / 2 Phong), w unused

	float	shadowParms[4];		// x = shadow technique (0 none / 1 projected-2D /
								// 2 point-cube), y = texel size (1/res), z = depth-
								// compare bias, w = light range (point-cube radial
								// normalizer; unused for the 2D path). The 2D lookup
								// reuses the light-projection texgen (S/T/Q + falloff);
								// the cube lookup uses the world-space light->frag dir.

	float	occlusionParms[4];	// DUDE material AO map (docs/occlusion-maps.md): x = enable
								// (this surface has an occlusion stage and r_occlusionMaps is
								// on), y = ambient-term strength, z = direct-diffuse strength,
								// w = r_pbrEnvScale (PBR Phase C.1 metal environment floor,
								// riding the spare slot). Map sampled on unit 10, diffuse UV.

	float	pbrParms[4];		// DUDE PBR interaction path (docs/pbr-materials.md): x =
								// metalness (sanity-clamped to [0,1]), y = roughness,
								// z = enable (r_pbr, non-ambient interactions only), w =
								// specular energy scale (r_pbrSpecScale). Phase A fills global
								// fallbacks; Phase B swaps in per-material values.
	float	pbrParms2[4];		// DUDE PBR extras: x = metal diffuse-kill strength kd
								// (= 1 - r_pbrMetalDiffuse); diffuse *= 1 - metal*kd, so
								// kd < 1 retains the asset albedo colour on metals. yzw spare.
};

// 3 mat4 (192) + 39 vec4 (624) = 816 bytes, zero padding
static_assert( sizeof( RenderParams ) == 816, "RenderParams must match the std140 layout of renderparms.glsl" );

} // namespace rhi

#endif /* !__RENDERPARAMS_H__ */
