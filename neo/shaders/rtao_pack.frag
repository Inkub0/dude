// DUDE RTAO normal-roughness guide pass (docs/rtx-rtao.md H4b): convert the G-buffer's
// view-space normal to WORLD space and pack it exactly as NRD's
// NRD_FrontEnd_PackNormalAndRoughness does for NRD_NORMAL_ENCODING=2 (the signed-
// octahedron 101010 layout the vendored SPIR-V decodes) — ported verbatim from
// libs/nrd/Shaders/NRD.hlsli::_NRD_EncodeNormalRoughness101010. RGBA16F target carries
// the same 0..1 values an R10G10B10A2_UNORM would, without a new RHI target format.
// Roughness is a constant 1.0 — diffuse occlusion only cares that the C term is identity.
//
//   u_modelViewMatrix     = inverse view (view -> world)
//   u_screenCorrection.xy = 1 / viewSize (gl_FragCoord -> normal-buffer uv)

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_normalBuffer;	// xyz = view-space normal, a = weapon mask

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

void main() {
	vec4 nt = texture( u_normalBuffer, gl_FragCoord.xy * u_screenCorrection.xy );
	// degenerate-normal guard (mirrors rtao_ray.frag): a NaN here would poison the
	// denoiser's history through temporal accumulation — fall back to world up
	vec3  Nraw = nt.xyz * 2.0 - 1.0;
	float nl2  = dot( Nraw, Nraw );
	vec3  n;
	if ( !( nl2 > 1e-4 ) ) {
		n = vec3( 0.0, 0.0, 1.0 );
	} else {
		n = normalize( mat3( u_modelViewMatrix ) * ( Nraw * inversesqrt( nl2 ) ) );	// world-space
	}

	// _NRD_EncodeNormalRoughness101010( n, roughness=1 ), materialID 0
	n /= abs( n.x ) + abs( n.y ) + abs( n.z );
	vec3 r;
	r.y = n.y * 0.5 + 0.5;
	r.x = n.x * 0.5 + r.y;
	r.y -= n.x * 0.5;
	float roughness = 1.0;										// can't be 0: z carries n.z's sign
	float s = n.z < 0.0 ? -roughness : roughness;
	r.z = s * 0.5 + 0.5;

	fragColor = vec4( r, 0.0 );									// w = materialID/3 = 0
}
