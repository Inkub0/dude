// DUDE ReSTIR DI — RESOLVE pass (docs/rtx-restir-di.md): turn the reservoir (restir_di.frag) into shaded
// radiance. Per pixel: read the chosen light index y + its weight W + visibility from the reservoir,
// re-evaluate that light's unshadowed contribution rad(y) at THIS surface point, and output rad(y)*W*vis.
// Re-evaluating at the pixel (not storing radiance) is what makes a reused reservoir - in Stage 3, one
// reprojected from a neighbour - shade correctly for its own surface. Output is LINEAR HDR irradiance.
//
// r_rtReStirDIDebug 2 (u_localParam1.y > 0.5) overrides this with the brute UNSHADOWED sum over ALL
// lights - the clean ground truth the 1-spp RIS converges to (correctness/colour reference).
//
// Uniform packing (RB_RHI_ReStirDIPass, resolve draw):
//   u_modelViewMatrix     = inverse view (view -> world)
//   u_color.xy            = scene-light buffer device address (bit-cast lo/hi)
//   u_color.z             = light count
//   u_localParam0         = ( 1/proj00, 1/proj11, unused, unused )
//   u_localParam1.y       = brute ground-truth flag (>0.5 = sum all lights unshadowed; debug mode 2)
//   u_screenCorrection.xy = 1 / viewSize
//   u_depthTexRecip.xy    = gl_FragCoord -> _currentDepth tc
//   u_windowCoord.z       = view-Y sign (-1 Vulkan top-down)
#extension GL_EXT_buffer_reference_uvec2 : require

#include "renderparms.glsl"
#include "rt_light_eval.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_currentDepth;
SAMPLER_BINDING(1) uniform sampler2D u_normalBuffer;	// xyz = view-space normal, a = weapon/AO mask
SAMPLER_BINDING(2) uniform sampler2D u_reservoir;		// ( y|-1, W, M, vis ) from restir_di.frag

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;	// RGBA16F: linear direct irradiance (rgb), 1 (a)

#define depth_consts DUDE_DEPTH_CONSTS()

void main() {
	vec2  frag = gl_FragCoord.xy;
	float raw  = texture( u_currentDepth, frag * u_depthTexRecip.xy ).x;
	if ( raw >= 0.9994 ) { fragColor = vec4( 0.0, 0.0, 0.0, 1.0 ); return; }
	vec4 nt = texture( u_normalBuffer, frag * u_screenCorrection.xy );
	if ( nt.a < 0.5 ) { fragColor = vec4( 0.0, 0.0, 0.0, 1.0 ); return; }

	float vz  = 1.0 / ( min( raw, 0.9994 ) * depth_consts.x + depth_consts.y );
	vec2  ndc = frag * ( u_screenCorrection.xy * 2.0 ) - 1.0;
	float d   = -vz;
	vec3  P   = vec3( ndc.x * d * u_localParam0.x, ndc.y * u_windowCoord.z * d * u_localParam0.y, vz );
	vec3  Nraw = nt.xyz * 2.0 - 1.0;
	float nl2  = dot( Nraw, Nraw );
	if ( !( nl2 > 1e-4 ) ) { fragColor = vec4( 0.0, 0.0, 0.0, 1.0 ); return; }
	vec3  N   = Nraw * inversesqrt( nl2 );

	vec3 Pw = ( u_modelViewMatrix * vec4( P, 1.0 ) ).xyz;
	vec3 Nw = normalize( mat3( u_modelViewMatrix ) * N );

	RtLights lights = RtLights( uvec2( floatBitsToUint( u_color.x ), floatBitsToUint( u_color.y ) ) );
	uint nLights = uint( u_color.z );
	if ( nLights == 0u ) { fragColor = vec4( 0.0, 0.0, 0.0, 1.0 ); return; }

	// debug mode 2: brute UNSHADOWED ground truth over every light (no RIS, no noise)
	if ( u_localParam1.y > 0.5 ) {
		vec3 sum = vec3( 0.0 );
		for ( uint li = 0u; li < nLights; li++ ) {
			vec3 ldir; float dl;
			dudeLightDir( lights, li, Pw, ldir, dl );
			float ndl = max( dot( Nw, ldir ), 0.0 );
			if ( ndl <= 0.0 ) { continue; }
			float atten = dudeLightAtten( lights, li, Pw, dl, lights.l[ li ].flags, true, false );
			sum += lights.l[ li ].color * ( atten * ndl );
		}
		fragColor = vec4( sum, 1.0 );
		return;
	}

	// resolve the reservoir: exact texel (nearest - the stored light index must NOT be interpolated)
	vec4 res = texelFetch( u_reservoir, ivec2( frag ), 0 );
	if ( !( res.x >= 0.0 ) ) { fragColor = vec4( 0.0, 0.0, 0.0, 1.0 ); return; }	// empty reservoir
	uint li  = uint( res.x + 0.5 );
	if ( li >= nLights ) { fragColor = vec4( 0.0, 0.0, 0.0, 1.0 ); return; }		// stale index (light count shrank)
	float W   = res.y;
	float vis = res.w;

	// re-evaluate the chosen light's unshadowed contribution at THIS surface point
	vec3 ldir; float dl;
	dudeLightDir( lights, li, Pw, ldir, dl );
	float ndl = max( dot( Nw, ldir ), 0.0 );
	vec3  rad = vec3( 0.0 );
	if ( ndl > 0.0 ) {
		float atten = dudeLightAtten( lights, li, Pw, dl, lights.l[ li ].flags, true, false );
		rad = lights.l[ li ].color * ( atten * ndl );
	}
	fragColor = vec4( rad * ( W * vis ), 1.0 );
}
