// DUDE glass screen-space reflections (docs/ssr.md, glass extension). Shared
// march for the environment_ssr / bumpyenvironment_ssr fragment shaders: the
// TG_REFLECT_CUBE (glass) stages draw AFTER the SSR translucent-split snapshot,
// so _currentRender already holds the lit opaque scene and _currentDepth the
// opaque depth — a per-fragment march from the glass surface can mirror the
// real scene, with the material's cubemap as the miss fallback.
//
// The march is the ssr.frag recipe (armed crossing + binary refinement) with two
// differences: the source position/normal come from the glass geometry itself
// (interpolated varyings — glass is not in the G-buffer or the depth buffer),
// and instead of premultiplying the edge/range/facing fades into the color, the
// result returns them as a confidence in .a so the caller can mix() back to the
// cubemap where the screen-space data runs out.
//
// Uniform packing (RB_RHI_RenderTexgenStage, glass-SSR branch):
//   u_projectionMatrix    = view -> clip, for projecting march points to screen
//   u_modelViewMatrix     = model -> view (position/normal varyings, .vert side)
//   u_localParam0.zw      = ( maxDistance, thickness )
//   u_localParam1.xy      = ( steps, intensity )
//   u_screenCorrection.xy = 1 / viewSize (gl_FragCoord -> [0,1] uv)
//   u_screenCorrection.zw = view/POT scale ([0,1] uv -> _currentRender texcoord)
//   u_depthTexRecip.xy    = gl_FragCoord -> _currentDepth texcoord
//
// Samplers: unit 0 (and 1 for the bumpy variant) stay the cube/bump maps of the
// base shaders; the march data rides on 2/3/4.

SAMPLER_BINDING(2) uniform sampler2D u_currentRender;   // split-point opaque scene snapshot
SAMPLER_BINDING(3) uniform sampler2D u_currentDepth;
SAMPLER_BINDING(4) uniform sampler2D u_normalBuffer;    // xyz = view normal, a = weapon mask

#define GLASS_MAX_MARCH_STEPS  64
#define GLASS_REFINE_STEPS     4

// Doom 3's fixed near / near-infinite far projection in GL clip depth -> linear
// eye z (negative). Same constants as ssao.frag / ssr.frag.
const vec2 glass_depth_consts = vec2( 0.33333333, -0.33316667 );

float GlassRawDepth( vec2 frag ) {
	return texture( u_currentDepth, frag * u_depthTexRecip.xy ).x;
}

float GlassViewZFromRaw( float raw ) {
	return 1.0 / ( min( raw, 0.9994 ) * glass_depth_consts.x + glass_depth_consts.y );   // negative
}

// interleaved gradient noise — per-pixel jitter that hides the march banding
float GlassIgn( vec2 p ) {
	return fract( 52.9829189 * fract( dot( p, vec2( 0.06711056, 0.00583715 ) ) ) );
}

// project a view-space point to fragment coordinates; z < 0 means behind the eye
vec3 GlassProjectToFrag( vec3 viewPos ) {
	vec4 clip = u_projectionMatrix * vec4( viewPos, 1.0 );
	if ( clip.w <= 0.0 ) {
		return vec3( -1.0, -1.0, -1.0 );
	}
	vec2 uv01 = ( clip.xy / clip.w ) * 0.5 + 0.5;
	return vec3( uv01 / u_screenCorrection.xy, 1.0 );
}

// March the reflection of view-space position P about view-space normal N.
// Returns rgb = reflected scene color * intensity, a = hit confidence (0 = miss,
// use the cubemap). The glass surface itself never writes depth, so the ray
// starts in front of the opaque scene and the armed-crossing test (see ssr.frag:
// first seen in FRONT of the depth surface, then behind it within the thickness)
// arms naturally on the first samples.
vec4 GlassSsrMarch( vec3 P, vec3 N ) {
	vec3 V = normalize( -P );
	// twosided glass (glass1 etc.): the geometry normal faces one side only but
	// the pane is viewed from both. A normal facing away from the eye reflects
	// the ray to nonsense and every march misses — vanilla's cube lookup hid
	// this because a cubemap reads plausibly either way. Flip toward the viewer.
	if ( dot( N, V ) < 0.0 ) {
		N = -N;
	}
	vec3 R = reflect( -V, N );

	// rays aimed almost straight back at the eye only ever produce false
	// self-hits; fade them out (same window as ssr.frag)
	float facing = 1.0 - smoothstep( 0.9, 1.0, dot( R, V ) );
	if ( facing < 0.002 ) {
		return vec4( 0.0 );
	}

	int   steps     = int( u_localParam1.x );
	float maxDist   = u_localParam0.z;
	float thickness = u_localParam0.w;
	float stepLen   = maxDist / float( steps );

	float tPrev = stepLen * GlassIgn( gl_FragCoord.xy );
	float t     = tPrev + stepLen;
	float tHit  = -1.0;
	bool  armed = false;
	for ( int i = 0; i < GLASS_MAX_MARCH_STEPS; i++ ) {
		if ( i >= steps ) {
			break;
		}
		vec3 rayPos = P + R * t;
		if ( rayPos.z > -1.0 ) {
			break;                                 // in front of the near plane
		}
		vec3 pf = GlassProjectToFrag( rayPos );
		if ( pf.z < 0.0 ) {
			break;
		}
		vec2 suv = pf.xy * u_screenCorrection.xy;
		if ( suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0 ) {
			break;                                 // left the screen
		}
		float sceneZ = GlassViewZFromRaw( GlassRawDepth( pf.xy ) );
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
		return vec4( 0.0 );
	}

	// binary refinement between the last miss and the hit
	float lo = tPrev, hi = tHit;
	for ( int i = 0; i < GLASS_REFINE_STEPS; i++ ) {
		float mid = 0.5 * ( lo + hi );
		vec3 pf = GlassProjectToFrag( P + R * mid );
		float dz = GlassViewZFromRaw( GlassRawDepth( pf.xy ) ) - ( P + R * mid ).z;
		if ( dz > 0.0 ) {
			hi = mid;
		} else {
			lo = mid;
		}
	}
	vec3 pfHit = GlassProjectToFrag( P + R * hi );
	vec2 hitUv = pfHit.xy * u_screenCorrection.xy;

	// reject hits on surfaces facing away from the ray (marched through a backface)
	vec3 hitN = normalize( texture( u_normalBuffer, hitUv ).xyz * 2.0 - 1.0 );
	if ( dot( hitN, R ) > 0.0 ) {
		return vec4( 0.0 );
	}

	// screen-edge fade (reflection data ends at the viewport) + range fade; both go
	// into the confidence so the cubemap takes back over where the data runs out
	vec2  eDist = min( hitUv, 1.0 - hitUv );
	float edge  = smoothstep( 0.0, 0.08, min( eDist.x, eDist.y ) );
	float range = 1.0 - clamp( hi / maxDist, 0.0, 1.0 );

	vec3 scene = texture( u_currentRender, hitUv * u_screenCorrection.zw ).rgb;
	return vec4( scene * u_localParam1.y, edge * range * facing );
}
