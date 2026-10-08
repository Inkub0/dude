// DUDE ReSTIR DI shared material terms (docs/rtx-restir-di.md Stage D). Included by the gather (restir_di.frag)
// and the apply (restir_di_apply.frag), which must agree BIT-FOR-BIT: the gather divides its shaded radiance by
// this per-pixel factor before NRD (so the denoiser sees irradiance-like signal without the albedo's texture
// detail, which a spatial blur would smear) and the apply multiplies it back, exactly. Any positive factor is
// exact; this one is the material's own reflectance share, which is what makes the quotient smooth.
//
// G-buffer encodings (gbuffer.frag): albedo = att3 rgb; material att1 = ( roughness, metalness, spec-map luminance ).
//   kd        = 1 - r_pbrMetalDiffuse (how much of a metal's diffuse is killed, interaction.frag u_pbrParms2.x)
//   specScale = r_pbrSpecScale (dielectric-weighted lobe energy, u_pbrParms.w)
vec3 diDemodFactor( vec3 albedo, float metal, float specMask, float kd, float specScale ) {
	vec3 F0 = mix( vec3( 0.04 ), albedo, metal );
	vec3 B  = albedo * ( 1.0 - metal * kd )									// the diffuse share (per-light (1-F) stays in the signal)
	        + F0 * ( specMask * mix( specScale, 1.0, metal ) * 2.0 );		// the specular share at F0 (the lobe stays in the signal)
	return max( B, vec3( 0.02 ) );
}
