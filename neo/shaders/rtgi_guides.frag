// DUDE RT GI checkerboard guide pass (docs/rtx-rtgi.md H6g).
//
// NRD checkerboard mode needs the NRD GUIDES (IN_VIEWZ, IN_NORMAL_ROUGHNESS) at FULL resolution while the
// noisy radiance is traced for only half the pixels. The normal r_rtgiResScale path writes the guides from
// the gather pass itself (same reduced res as the radiance), which can't work here — so this cheap, ray-free
// full-res pass produces them from the sealed depth + the bump-mapped normal G-buffer, exactly as rtgi_ray
// does, just for every pixel and with no bounce.
//
//   u_currentDepth (unit 0)  = sealed scene depth (POT)
//   u_normalBuffer (unit 1)  = view-space normal G-buffer (xyz normal, a = weapon/AO mask)
//   u_modelViewMatrix        = inverse view (view -> world), to lift the normal to world for NRD encoding 2
//   u_depthTexRecip.xy       = gl_FragCoord -> _currentDepth tc ; u_screenCorrection.xy = gl_FragCoord -> normal tc

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_currentDepth;
SAMPLER_BINDING(1) uniform sampler2D u_normalBuffer;

VARY(0) in vec2 var_TexCoord;

// Same MRT layout as the gather (CreateRenderTargetRtgiMrt): att0 radiance (unused here), att1 viewZ,
// att2 normal — so NRD reads the guides from att1/att2 exactly as it does off the gather in the resScale path.
layout(location = 0) out vec4 outRadiance;	// unused (radiance att); written 0
layout(location = 1) out vec4 outViewZ;		// IN_VIEWZ: signed linear view z (negative), R16F
layout(location = 2) out vec4 outNormal;	// IN_NORMAL_ROUGHNESS: world normal, NRD encoding 2, RGBA16F

#define depth_consts DUDE_DEPTH_CONSTS()

// world normal -> NRD normal/roughness encoding 2 (signed octahedron, z sign in roughness). Identical to
// rtgi_ray.frag's packNrdNormal so the checkerboard guides match the r_rtgiResScale path bit for bit.
vec4 packNrdNormal( vec3 Nraw ) {
	float nl2 = dot( Nraw, Nraw );
	vec3  n;
	if ( !( nl2 > 1e-4 ) ) {
		n = vec3( 0.0, 0.0, 1.0 );
	} else {
		n = normalize( mat3( u_modelViewMatrix ) * ( Nraw * inversesqrt( nl2 ) ) );
	}
	n /= abs( n.x ) + abs( n.y ) + abs( n.z );
	vec3 r;
	r.y = n.y * 0.5 + 0.5;
	r.x = n.x * 0.5 + r.y;
	r.y -= n.x * 0.5;
	float roughness = 1.0;
	float s = n.z < 0.0 ? -roughness : roughness;
	r.z = s * 0.5 + 0.5;
	return vec4( r, 0.0 );
}

void main() {
	vec2  frag = gl_FragCoord.xy;
	float raw  = texture( u_currentDepth, frag * u_depthTexRecip.xy ).x;
	vec4  nt   = texture( u_normalBuffer, frag * u_screenCorrection.xy );
	outRadiance = vec4( 0.0 );
	outViewZ    = vec4( 1.0 / ( min( raw, 0.9994 ) * depth_consts.x + depth_consts.y ), 0.0, 0.0, 1.0 );
	outNormal   = packNrdNormal( nt.xyz * 2.0 - 1.0 );
}
