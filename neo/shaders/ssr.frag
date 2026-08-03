// DUDE screen-space reflections — MARCH pass (docs/ssr.md, PBR Phase C.2/C.2.1).
// Renders into the offscreen SSR buffer at r_ssrResScale of the view: reconstruct
// the view-space surface from _currentDepth + the normal G-buffer, reflect the eye
// ray, march the depth buffer for the first thing the ray hits, and write that
// scene color with the per-ray fades (edge/range/facing) premultiplied. Miss =
// the cleared 0. The Fresnel x gloss x intensity weighting happens at FULL
// resolution in ssr_composite.frag, so a low-res march doesn't soften the
// material response; ssr_temporal.frag optionally accumulates between them.
//
// Uniform packing (RB_RHI_ScreenSpaceReflections):
//   u_projectionMatrix    = view -> clip, for projecting march points to screen
//   u_localParam0         = ( 1/proj00, 1/proj11, maxDistance, thickness )
//   u_localParam1         = ( steps, unused, maxRoughness, 0 )
//   u_screenCorrection.xy = 1 / ssrTargetSize (gl_FragCoord -> [0,1] uv)
//   u_screenCorrection.zw = view/POT scale ([0,1] uv -> _currentRender texcoord)
//   u_depthTexRecip.xy    = ratio / depthUploadSize (gl_FragCoord -> depth tc)
//   u_windowCoord.y       = per-frame jitter phase (temporal; 0 = static dither)

#include "renderparms.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_currentRender;   // lit opaque scene snapshot
SAMPLER_BINDING(1) uniform sampler2D u_currentDepth;
SAMPLER_BINDING(2) uniform sampler2D u_normalBuffer;    // xyz = view normal, a = weapon mask
SAMPLER_BINDING(3) uniform sampler2D u_materialBuffer;  // x = roughness, y = metalness

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;

#define MAX_MARCH_STEPS  64
#define REFINE_STEPS     4

// Doom 3's fixed near / near-infinite far projection in GL clip depth -> linear
// eye z (negative). Same constants as ssao.frag / softparticle.frag.
const vec2 depth_consts = vec2( 0.33333333, -0.33316667 );

float rawDepth( vec2 frag ) {
	return texture( u_currentDepth, frag * u_depthTexRecip.xy ).x;
}

float viewZFromRaw( float raw ) {
	return 1.0 / ( min( raw, 0.9994 ) * depth_consts.x + depth_consts.y );   // negative
}

// Convert a GL-convention (bottom-up, clip-derived) row to the device framebuffer
// row: identity on GL (u_windowCoord.z > 0), flipped on Vulkan where gl_FragCoord
// and the depth / G-buffer targets are top-down. Used to project march samples onto
// those device-oriented buffers, and again when sampling the bottom-up _currentRender
// capture (the two flips cancel back to GL rows for that one read). Inert on GL.
float deviceRow( float glRow ) {
	return ( u_windowCoord.z > 0.0 ) ? glRow : ( 1.0 - glRow );
}

vec3 viewPosFromRaw( vec2 frag, float raw ) {
	float vz  = viewZFromRaw( raw );
	vec2  ndc = frag * ( u_screenCorrection.xy * 2.0 ) - 1.0;
	float d   = -vz;
	// u_windowCoord.z = view-Y sign (+1 GL / -1 Vulkan): flips the reconstructed Y so
	// it agrees with the view-space G-buffer normal on VK's top-down framebuffer
	return vec3( ndc.x * d * u_localParam0.x, ndc.y * u_windowCoord.z * d * u_localParam0.y, vz );
}

// interleaved gradient noise — per-pixel jitter that hides the march banding
float ign( vec2 p ) {
	return fract( 52.9829189 * fract( dot( p, vec2( 0.06711056, 0.00583715 ) ) ) );
}

// project a view-space point to fragment coordinates; w <= 0 means behind the eye.
// The clip->uv is GL-convention (y-up); deviceRow re-orients the row so the returned
// frag addresses the device-oriented depth / G-buffer (top-down on Vulkan).
vec3 projectToFrag( vec3 viewPos ) {
	vec4 clip = u_projectionMatrix * vec4( viewPos, 1.0 );
	if ( clip.w <= 0.0 ) {
		return vec3( -1.0, -1.0, -1.0 );
	}
	vec2 uv01 = ( clip.xy / clip.w ) * 0.5 + 0.5;
	uv01.y = deviceRow( uv01.y );
	return vec3( uv01 / u_screenCorrection.xy, 1.0 );
}

void main() {
	vec2  frag = gl_FragCoord.xy;
	float raw  = rawDepth( frag );
	if ( raw >= 0.9994 ) {
		discard;                                   // sky / no geometry
	}

	vec2 uv = frag * u_screenCorrection.xy;
	vec4 nt = texture( u_normalBuffer, uv );
	if ( nt.a < 0.5 ) {
		discard;                                   // view weapon: depth-hacked, don't reflect on it
	}
	vec4  mt    = texture( u_materialBuffer, uv );
	float rough = mt.x;
	float metal = mt.y;

	// gloss window: full strength up to 70% of the roughness cutoff, fading to 0 at it
	float maxRough = u_localParam1.z;
	float gloss = 1.0 - smoothstep( maxRough * 0.7, maxRough, rough );
	if ( gloss < 0.004 ) {
		discard;
	}

	vec3 P = viewPosFromRaw( frag, raw );
	vec3 N = normalize( nt.xyz * 2.0 - 1.0 );
	vec3 V = normalize( -P );

	// Early-out on the same Schlick Fresnel the composite will apply at full res:
	// if no visible weight can result, skip the march entirely.
	float NdotV = clamp( dot( N, V ), 0.0, 1.0 );
	float F0 = mix( 0.04, 0.9, metal );
	float F  = F0 + ( 1.0 - F0 ) * pow( 1.0 - NdotV, 5.0 );

	vec3 R = reflect( -V, N );
	// rays aimed almost straight back at the eye march through the near field and
	// only ever produce false self-hits; fade them out (objects standing between the
	// eye and the surface — the barrel-on-the-floor case — survive well below 0.9)
	float facing = 1.0 - smoothstep( 0.9, 1.0, dot( R, V ) );
	if ( F * gloss * facing < 0.002 ) {
		discard;
	}

	int   steps     = int( u_localParam1.x );
	float maxDist   = u_localParam0.z;
	float thickness = u_localParam0.w;
	float stepLen   = maxDist / float( steps );

	// Jittered linear march with an ARMED crossing test: a sample only counts as a
	// hit when (a) some earlier sample was genuinely in FRONT of the depth surface
	// and (b) this one is behind it by less than the assumed thickness. The arming
	// requirement kills the self-hit haze: bump-mapped normals (tile edges) tilt
	// some reflection rays straight into their own surface, and those rays start
	// behind the floor, never arm, and correctly miss — instead of "hitting" the
	// adjacent floor and smearing it across the grazing band. Overshoots beyond
	// the thickness window keep marching so rays can pass behind thin foreground
	// objects (railings, pipes).
	// per-frame jitter rotation (u_windowCoord.y, golden-ratio walk) gives the
	// temporal pass different march offsets to average; 0 = the plain static dither
	float tPrev = stepLen * fract( ign( frag ) + u_windowCoord.y );
	float t     = tPrev + stepLen;
	float tHit  = -1.0;
	bool  armed = false;
	for ( int i = 0; i < MAX_MARCH_STEPS; i++ ) {
		if ( i >= steps ) {
			break;
		}
		vec3 rayPos = P + R * t;
		if ( rayPos.z > -1.0 ) {
			break;                                 // in front of the near plane
		}
		vec3 pf = projectToFrag( rayPos );
		if ( pf.z < 0.0 ) {
			break;
		}
		vec2 suv = pf.xy * u_screenCorrection.xy;
		if ( suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0 ) {
			break;                                 // left the screen
		}
		float sceneZ = viewZFromRaw( rawDepth( pf.xy ) );
		float dz = sceneZ - rayPos.z;              // > 0: ray is behind the surface here
		if ( armed && dz > 0.0 && dz < thickness + stepLen ) {
			// ignore the depth-hacked view weapon as a reflection source
			if ( texture( u_normalBuffer, suv ).a >= 0.5 ) {
				tHit = t;
			}
			break;
		}
		if ( dz <= 0.0 ) {
			armed = true;                          // seen in front; a crossing can now hit
		}
		tPrev = t;
		t += stepLen;
	}
	if ( tHit < 0.0 ) {
		discard;
	}

	// binary refinement between the last miss and the hit
	float lo = tPrev, hi = tHit;
	for ( int i = 0; i < REFINE_STEPS; i++ ) {
		float mid = 0.5 * ( lo + hi );
		vec3 pf = projectToFrag( P + R * mid );
		float dz = viewZFromRaw( rawDepth( pf.xy ) ) - ( P + R * mid ).z;
		if ( dz > 0.0 ) {
			hi = mid;
		} else {
			lo = mid;
		}
	}
	vec3 pfHit = projectToFrag( P + R * hi );
	vec2 hitUv = pfHit.xy * u_screenCorrection.xy;

	// reject hits on surfaces facing away from the ray (marched through a backface)
	vec3 hitN = normalize( texture( u_normalBuffer, hitUv ).xyz * 2.0 - 1.0 );
	if ( dot( hitN, R ) > 0.0 ) {
		discard;
	}

	// screen-edge fade (reflection data ends at the viewport) + range fade
	vec2  eDist = min( hitUv, 1.0 - hitUv );
	float edge  = smoothstep( 0.0, 0.08, min( eDist.x, eDist.y ) );
	float range = 1.0 - clamp( hi / maxDist, 0.0, 1.0 );

	// per-ray fades premultiplied; material weighting happens in ssr_composite.frag.
	// hitUv is a device-oriented row (projectToFrag); deviceRow flips it back to the
	// bottom-up GL layout of the _currentRender capture (inert on GL).
	vec2 crUv = vec2( hitUv.x, deviceRow( hitUv.y ) ) * u_screenCorrection.zw;
	vec3 scene = texture( u_currentRender, crUv ).rgb;
	fragColor = vec4( scene * ( edge * range * facing ), 1.0 );
}
