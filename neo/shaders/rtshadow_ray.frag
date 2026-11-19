// DUDE RT shadow blur, ray pass (r_rtShadowBlur, docs/rtx-shadow-blur.md): for ONE light that
// the engine already serves with hard ray-traced shadows (RT sun shadows, RT moving lights), trace
// that same shadow ray for every screen pixel of the light - toward the light's CENTRE, so it is
// deterministic - and record what the blur needs. The interaction shader then looks the blurred
// result up (mode 5) instead of tracing inline (mode 4): the ray count is the same, it just moves
// from the interaction draws to this pass.
//
// Output (RGBA16F):
//   R = visibility (1 lit / 0 occluded)
//   G = penumbra half-width on screen, HORIZONTAL, in pixels (0 = lit: nothing to spread)
//   B = view distance d (the blur's depth guide; 0 = never traced)
//   A = penumbra half-width on screen, VERTICAL, in pixels
// Half-width = blurScale * dOccluder / ( dLight - dOccluder ): zero where the caster touches the
// receiver, growing with the gap - projected to pixels at this depth and foreshortened per screen
// axis by the receiver's geometric normal (a floor at a grazing angle blurs far less vertically).
//
// The ray origin is rebuilt from the depth buffer (mode 4 has the interpolated surface position),
// which needs two precautions mode 4 doesn't: the unprojection must include this frame's FSR2
// sub-pixel jitter (the projection's shear terms), and the origin is pulled toward the camera /
// pushed along the normal by the depth buffer's quantization error, which grows with distance -
// without both, far terrain self-shadows in a per-frame flicker.
//
// VK + RT hardware only (ray_query). NaN guards mirror rtao_ray.frag (a ray launched with a NaN
// origin/direction can hang RT traversal: Xid 109 / DEVICE_LOST).
//
// Uniform packing (RB_RHI_RtShadowBlurLight):
//   u_rtParms.xy          = TLAS device address (bit-cast lo/hi)
//   u_rtParms.z           = ray-origin normal offset (r_rtSunShadowOffset, shared with mode 4)
//   u_color.y             = 0: every hit counts; 1: r_rtShadowOutsideHits; 2: that, and r_rtShadowNearStart - where the
//                           surface faces the light the ray starts DUDE_RT_NEAR_START off it instead of u_rtParms.z
//   u_contactParms2.w     = r_rtShadowOriginDistScale: extra origin offset per unit of view distance
//   u_rtParms.w           = 1: units 2/3 hold this light's falloff / projection images and the lookup
//                           is the plain one (single stage, no texture matrix) - skip the ray where
//                           they are black
//   u_modelViewMatrix     = inverse view (view -> world)
//   u_localLightOrigin    = WORLD-space light origin
//   u_lightProjectionS/T/Q, u_lightFalloffS = the light's WORLD-space projection planes
//                           (vLight->lightProject[0..3]) - the light-volume test
//   u_localParam0         = ( 1/proj00, 1/proj11, blurScale (world units, from r_rtShadowBlurIntensity), world units -> pixels at view distance 1 )
//   u_localParam1         = ( max half-width in pixels, proj[8], proj[9], curve exponent )
//                           curve shapes HOW the blur grows with the gap (r_rtShadowBlurCurve): the width is
//                           blurScale * g^curve, g = dOccluder / ( dLight - dOccluder ). 1 = the geometric
//                           (physical) growth; < 1 = soft sooner, then levelling off; > 1 = crisp for longer
//                           near the caster, then widening fast
//   u_shadowFalloffS      = perforated casters (rt_perforated.glsl): xy = geometry-table address,
//                           z = their shadow strength (r_shadowMapPerforatedStrength); 0 = off
//   u_shadowProjectionQ.xy = address of this light's grate boxes; u_shadowFalloffS.w = how many
//   u_screenCorrection.xy = 1 / viewSize
//   u_depthTexRecip.xy    = gl_FragCoord -> _currentDepth tc
//   u_depthParms.xy       = window depth -> 1 / view z for THIS view (renderparms.glsl): the game
//                           lowers the near plane for cinematic cameras (r_znear 3 -> 1), and a hard-coded
//                           play-time pair rebuilt every point 3x too far along its view ray
#extension GL_EXT_ray_query : require
#extension GL_EXT_buffer_reference_uvec2 : require	// perforated casters: geo-table pointer
#extension GL_EXT_nonuniform_qualifier : require		// perforated casters: bindless coverage texture

#include "renderparms.glsl"
#include "rt_perforated.glsl"

SAMPLER_BINDING(0) uniform sampler2D u_currentDepth;
SAMPLER_BINDING(1) uniform sampler2D u_normalBuffer;   // xyz = view-space normal, a = weapon/AO mask
SAMPLER_BINDING(2) uniform sampler2D u_lightFalloff;   // the light's falloff image      } read only when
SAMPLER_BINDING(3) uniform sampler2D u_lightProjection; // the light's projection image  } u_rtParms.w = 1

VARY(0) in vec2 var_TexCoord;

layout(location = 0) out vec4 fragColor;
// perf: a compact per-pixel SUMMARY for the tile classification (rtshadow_tiles.frag pass 0) so it
// reads 2 bytes/texel here instead of the 8-byte fragColor. R = visibility, G = penumbra half-width
// / HW_MAX (0 = lit, nothing to spread), and G = 1.0 is the reserved "this pixel was never traced"
// sentinel (sky / weapon / outside the light) - the tile pass skips those, matching fragColor.b == 0.
layout(location = 1) out vec2 out_summary;
const float RTSHADOW_HW_MAX = 24.0;		// = HW_MAX in rtshadow_tiles.frag / K in rtshadow_blur.frag
// r_rtShadowNearStart: the ray's start, world units off the surface - the RT contact shadows' own start
// (interaction.frag rtContactOcclusion), which the outside-hits rule makes safe
const float DUDE_RT_NEAR_START = 0.05;

// Doom 3's near-infinite far projection in GL clip depth -> linear eye z. The pair comes from the
// view's own projection (u_depthParms); ( 0.33333333, -0.33316667 ) is its play-time value.

void main() {
	fragColor = vec4( 1.0, 0.0, 0.0, 0.0 );		// every early-out = lit
	out_summary = vec2( 1.0, 1.0 );				// until proven traced: lit, G = untraced sentinel
	vec2  frag = gl_FragCoord.xy;
	// The surface point and its GEOMETRIC normal first, for every pixel of the quad: the normal comes from
	// screen-space derivatives of the point, which are undefined once a quad neighbour has returned - and the
	// checkerboard below returns two of every four (issue #25: the ray origin is pushed along this normal now).
	float raw  = texture( u_currentDepth, frag * u_depthTexRecip.xy ).x;
	float vz  = 1.0 / ( min( raw, 0.9994 ) * DUDE_DEPTH_CONSTS().x + DUDE_DEPTH_CONSTS().y );   // negative
	float d   = -vz;
	vec2  ndc = frag * ( u_screenCorrection.xy * 2.0 ) - 1.0;
	// jitter-exact unprojection (VK: top-down window y, hence the sign): ndc.x = P00*x/d - P8
	vec3  P   = vec3( ( ndc.x + u_localParam1.y ) * d * u_localParam0.x,
	                  ( -ndc.y + u_localParam1.z ) * d * u_localParam0.y, vz );
	// depth quantization: ~2e-8 * d^2 world units along the view ray, doubled by the float32
	// cancellation in the linearization - err in FRONT of the surface, never behind it
	// (the 6e-8 is for the play-time near plane; depth resolution scales with 1 / zNear)
	P *= max( 1.0 - 6e-8 * ( DUDE_DEPTH_CONSTS().x * 3.0 ) * d * d / max( length( P ), 1.0 ), 0.5 );
	// geometric normal from the depth derivatives, taken before ANY branching (uniform control flow)
	vec3  gN   = cross( dFdx( P ), dFdy( P ) );

	// r_rtShadowBlurCheckerboard (u_color.x): trace a STATIC screen checkerboard - skip half the pixels,
	// the reconstruct pass (rtshadow_fill) fills them from their traced neighbours (always the opposite
	// parity). Static (no per-frame flip) so the deterministic "same camera -> same mask" holds. A skipped
	// pixel is tagged with a negative distance; tiles/blur only ever see the reconstructed buffer.
	if ( u_color.x > 0.5 && ( ( int( frag.x ) ^ int( frag.y ) ) & 1 ) == 1 ) {
		fragColor.b = -1.0;						// "skipped, reconstruct me"
		return;
	}
	if ( raw >= 0.9994 ) {						// sky / no geometry
		return;
	}
	vec4 nt = texture( u_normalBuffer, frag * u_screenCorrection.xy );
	if ( nt.a < 0.5 ) {
		// view weapon: its depth lives in the compressed depth-hack range, so the world position
		// can't be rebuilt from it. Weapon surfaces never sample the mask - they keep mode 4.
		return;
	}

	fragColor.b = min( d, 65000.0 );			// lit or not, this pixel has a surface the blur can weigh
	out_summary.y = 0.0;						// traced from here on (G < 1); lit until a width is found
	vec3  Nraw = nt.xyz * 2.0 - 1.0;
	float nl2  = dot( Nraw, Nraw );
	if ( !( nl2 > 1e-4 ) ) {
		return;
	}
	vec3 N  = Nraw * inversesqrt( nl2 );
	vec3 Pw = ( u_modelViewMatrix * vec4( P, 1.0 ) ).xyz;
	vec3 Nw = normalize( mat3( u_modelViewMatrix ) * N );

	// outside the light's volume nothing is lit by it, so nothing needs a shadow
	vec4  Pw4 = vec4( Pw, 1.0 );
	float q = dot( Pw4, u_lightProjectionQ );
	if ( !( q > 0.0 ) ) {
		return;
	}
	float s = dot( Pw4, u_lightProjectionS ) / q;
	float t = dot( Pw4, u_lightProjectionT ) / q;
	float f = dot( Pw4, u_lightFalloffS );
	const float M = 0.02;
	if ( s < -M || s > 1.0 + M || t < -M || t > 1.0 + M || f < -M || f > 1.0 + M ) {
		return;
	}
	// inside the box but where the light's images are black (the corners around a round falloff):
	// the interaction multiplies by exactly this product, so no shadow can show. Read two mips
	// down - explicit lod, this is non-uniform control flow - so the zero region is the
	// conservative one: a texel the interaction's filtered fetch could still see stays traced.
	if ( u_rtParms.w > 0.5 ) {
		vec3 at = textureLod( u_lightProjection, vec2( s, t ), 2.0 ).rgb
		        * textureLod( u_lightFalloff, vec2( f, 0.5 ), 2.0 ).rgb;
		if ( !( at.r + at.g + at.b > 0.0 ) ) {
			return;
		}
	}

	// The geometric normal in world space, turned toward the camera (the visible side of the surface).
	float gLen = length( gN );
	vec3  gn   = ( gLen > 1e-6 ) ? gN / gLen : vec3( 0.0, 0.0, 1.0 );		// view space
	if ( dot( gn, -P ) < 0.0 ) { gn = -gn; }
	vec3  gNw  = mat3( u_modelViewMatrix ) * gn;
	// Push the ray origin off the surface along the GEOMETRIC normal, not the G-buffer's BUMP-MAPPED one (issue #25).
	// A strong bump (Quake 4's rock terrain: heightmap scale 10) tilts the shading normal far off the real surface, so
	// a push along it runs partly sideways or into the slope - by a different amount every texel - and far terrain
	// shadowed itself in a crawl: a small r_rtSunShadowOffset couldn't clear it, a big one lifted real shadows too.
	// The mode-4 ray (interaction.frag) pushes along the interpolated VERTEX normal and never had it. The derivative
	// normal is unreliable where its 2x2 quad straddles a depth edge; there it disagrees with the shading normal, and
	// the bump normal is kept as before.
	bool  gOk  = gLen > 1e-6 && dot( gNw, Nw ) > 0.25;
	vec3  Noff = gOk ? gNw : Nw;
	// r_rtShadowNearStart (u_color.y = 2, docs/rtx-shadow-blur.md "Near ray start"): the constant part of the push was
	// r_rtSunShadowOffset, 1 unit - a self-intersection guard from when every hit counted. It also lifted the start
	// over every occluder closer than that (a nose over a lip, a panel's edge on a wall), which is what the RT contact
	// shadows were then needed for. With the outside-hits rule a start a hair off the surface is safe (the contact
	// ray has always started there), and this ray sees those occluders itself. The distance term stays: it covers
	// the depth buffer's error, not the mesh's.
	// Only where the flat triangle FACES the light, fading back to the old start as it turns edge-on (cosine 0.15
	// to 0): on the lit side of a low-poly round shape's terminator the triangle already faces away while its
	// shading normal still takes light, and a ray from 0.05 units lands on that same triangle - whole facets went
	// dark; the 1-unit start clears the triangle's edge. Ignoring hits past the triangle's own plane instead (as
	// the scatter band below does) cured the facets but let that band's light through a real wall behind it.
	// Both measured in tools/tests/rt_shadow_terminator_sim.py. A pixel without a reliable flat normal (its
	// quad straddles a depth edge) keeps the old start too.
	bool  nearStart = u_color.y > 1.5;
	float pushC = u_rtParms.z;
	if ( nearStart && gOk ) {
		vec3  L0   = u_localLightOrigin.xyz - Pw;
		float gNL0 = dot( gNw, L0 ) * inversesqrt( max( dot( L0, L0 ), 1e-8 ) );
		pushC = mix( u_rtParms.z, min( DUDE_RT_NEAR_START, u_rtParms.z ), smoothstep( 0.0, 0.15, gNL0 ) );
	}
	float push = pushC + u_contactParms2.w * d;	// .w = r_rtShadowOriginDistScale
	vec3  O    = Pw + Noff * push;
	vec3  toL  = u_localLightOrigin.xyz - O;
	float dL2  = dot( toL, toL );
	if ( !( dL2 > 1.0 ) ) {						// on top of the light (also catches NaN)
		return;
	}
	float dL   = sqrt( dL2 );
	vec3  Ldir = toL / dL;

	// surfaces facing away from the light receive exactly zero from it (the interaction multiplies
	// by N.L): no ray. Bump normal AND geometric normal must agree, so only whole faces are skipped.
	// EXCEPT within a skin's scatter wrap (u_color.w = the widest wrap, r_skinScatter * 1.3; 0 = skin shading
	// off): skin / flesh is still lit past the terminator (interaction.frag skinScatterExcess, positive down to
	// N.L = -wrap), and "no ray = lit" there dropped every shadow that falls on that band - the jaw's on a neck
	// turned a little from the light vanished as soon as the light was softened. No per-pixel skin flag reaches
	// this pass, so every surface's band is traced; wrapBand marks those pixels, and below only an occluder
	// standing OVER their surface counts for them - anything else stays lit, exactly as the skipped ray left it.
	bool  wrapBand = false;
	float bNL = dot( Nw, Ldir );
	if ( bNL < -0.05 ) {
		vec3  gNs = ( dot( gNw, Nw ) < 0.0 ) ? -gNw : gNw;
		float gNL = dot( gNs, Ldir );
		if ( gLen > 1e-6 && gNL < -0.15 ) {
			if ( !( u_color.w > 0.0 ) || ( bNL < -0.05 - u_color.w && gNL < -0.15 - u_color.w ) ) {
				return;
			}
			wrapBand = true;
		}
	}

	// the mode-4 ray, except CLOSEST hit instead of first hit: the penumbra is sized by the
	// occluder nearest the receiver. Stops 1 unit short of the light, like mode 4.
	// Perforated casters (grates / fences): their triangles arrive as candidates; a solid texel is
	// never confirmed - it dims the light to 1 - strength, and the nearest one sizes the penumbra
	// when no real occluder does. One closer to the light than DUDE_RT_PERF_NEAR is clipped, as the caster pass clips it.
	uvec2 geoAddr = uvec2( floatBitsToUint( u_shadowFalloffS.x ), floatBitsToUint( u_shadowFalloffS.y ) );
	bool  perfOn  = u_shadowFalloffS.z > 0.0 && ( geoAddr.x | geoAddr.y ) != 0u;
	float tPerf   = -1.0;
	float tMax    = min( max( dL - 1.0, 0.01 ), 100000.0 );
	bool  solidHit;
	float tSolid  = 0.0;
	// per pixel: u_shadowFalloffS.w boxes of this light's grates at u_shadowProjectionQ.xy - a segment
	// that passes through none of them can't meet a grate ( w < 0: no list, all pixels )
	if ( perfOn && u_shadowFalloffS.w > 0.5 ) {
		perfOn = dudeSegmentMeetsPerforated(
			uvec2( floatBitsToUint( u_shadowProjectionQ.x ), floatBitsToUint( u_shadowProjectionQ.y ) ),
			int( u_shadowFalloffS.w + 0.5 ), O, Ldir, tMax );
	}
	if ( perfOn && nearStart ) {
		// a pixel whose ray can meet a grate takes the perforated traversal below, which commits solid triangles
		// itself - no outside-hits test - so it keeps the start that never needed one
		push = u_rtParms.z + u_contactParms2.w * d;
		O    = Pw + Noff * push;
		toL  = u_localLightOrigin.xyz - O;
		dL2  = dot( toL, toL );
		if ( !( dL2 > 1.0 ) ) {
			return;
		}
		dL   = sqrt( dL2 );
		Ldir = toL / dL;
		tMax = min( max( dL - 1.0, 0.01 ), 100000.0 );
	}
	if ( !perfOn && u_color.y > 0.5 ) {
		// r_rtShadowOutsideHits (u_color.y): count only hits on the OUTSIDE of a mesh - Doom 3's outward
		// side is Vulkan's BACK face (R_DeriveFacePlanes winding; the RT contact shadows use the same rule).
		// The start point above comes from the depth buffer, which on a tessellated character is the smoothed
		// surface while the ray-traced copy is the coarse posed mesh: in a fold that point sits INSIDE it, and
		// in a crease the slope-normal push can cross into the opposite side. Either way the first thing the
		// ray met was the inside of a mesh, and the texel went black (eye corners, collar, lips). A real
		// occluder between the surface and the light is entered from outside, so it still casts. But only
		// NEAR the start (u_color.z = r_rtShadowInsideSkip, world units): a buried start sees the inside of
		// the WHOLE mesh, and ignoring inside hits everywhere also dropped real self-shadows - the jaw over a
		// neck point that sits inside the coarse head. Beyond the skip distance an inside hit casts. Candidates
		// are confirmed one by one (slower than the all-opaque traversal below); confirming shrinks the ray,
		// so the committed hit is the CLOSEST counted one, as the penumbra sizing needs.
		rayQueryEXT rqf;
		dudeRayQueryInit( rqf,
			accelerationStructureEXT( uvec2( floatBitsToUint( u_rtParms.x ), floatBitsToUint( u_rtParms.y ) ) ),
			gl_RayFlagsNoOpaqueEXT, 0xFDu, O, 0.0, Ldir, tMax );
		while ( rayQueryProceedEXT( rqf ) ) {
			if ( rayQueryGetIntersectionTypeEXT( rqf, false ) == gl_RayQueryCandidateIntersectionTriangleEXT
					&& ( !rayQueryGetIntersectionFrontFaceEXT( rqf, false )
					     || rayQueryGetIntersectionTEXT( rqf, false ) > u_color.z ) ) {
				rayQueryConfirmIntersectionEXT( rqf );
			}
		}
		solidHit = rayQueryGetIntersectionTypeEXT( rqf, true ) == gl_RayQueryCommittedIntersectionTriangleEXT;
		if ( solidHit ) {
			tSolid = rayQueryGetIntersectionTEXT( rqf, true );
		}
	} else if ( !perfOn ) {
		// no grate in this light's volume (decided per light on the CPU): the ray this pass always
		// traced, flags and mask CONSTANT - forcing everything opaque is the hardware's fastest
		// traversal, and a runtime-selected flag word can't promise the driver that
		rayQueryEXT rqo;
		dudeRayQueryInit( rqo,
			accelerationStructureEXT( uvec2( floatBitsToUint( u_rtParms.x ), floatBitsToUint( u_rtParms.y ) ) ),
			gl_RayFlagsOpaqueEXT, 0xFDu, O, 0.0, Ldir, tMax );
		while ( rayQueryProceedEXT( rqo ) ) { }
		solidHit = rayQueryGetIntersectionTypeEXT( rqo, true ) == gl_RayQueryCommittedIntersectionTriangleEXT;
		if ( solidHit ) {
			tSolid = rayQueryGetIntersectionTEXT( rqo, true );
		}
	} else {
		rayQueryEXT rq;
		dudeRayQueryInit( rq,
			accelerationStructureEXT( uvec2( floatBitsToUint( u_rtParms.x ), floatBitsToUint( u_rtParms.y ) ) ),
			gl_RayFlagsNoneEXT, 0xFFu, O, 0.0, Ldir, tMax );
		while ( rayQueryProceedEXT( rq ) ) {
			if ( rayQueryGetIntersectionTypeEXT( rq, false ) == gl_RayQueryCandidateIntersectionTriangleEXT ) {
				float tc = rayQueryGetIntersectionTEXT( rq, false );
				if ( tc < dL - DUDE_RT_PERF_NEAR && ( tPerf < 0.0 || tc < tPerf )
						&& dudePerforatedSolid( geoAddr,
							uint( rayQueryGetIntersectionInstanceCustomIndexEXT( rq, false ) ),
							uint( rayQueryGetIntersectionGeometryIndexEXT( rq, false ) ),
							uint( rayQueryGetIntersectionPrimitiveIndexEXT( rq, false ) ),
							rayQueryGetIntersectionBarycentricsEXT( rq, false ) ) ) {
					tPerf = tc;
				}
			}
		}
		solidHit = rayQueryGetIntersectionTypeEXT( rq, true ) == gl_RayQueryCommittedIntersectionTriangleEXT;
		if ( solidHit ) {
			tSolid = rayQueryGetIntersectionTEXT( rq, true );
		}
	}
	if ( !solidHit && tPerf < 0.0 ) {
		return;									// reached the light: lit
	}

	float dOcc = solidHit ? tSolid : tPerf;
	if ( wrapBand ) {
		// This pixel faces away from the light: its ray starts `push` above the surface and sinks back through the
		// surface's own plane at push / sink. A hit from there on is the pixel's own body - a wall's back face, a
		// head past its terminator - and stays lit, as before. Only a hit BEFORE that is something standing over
		// the surface (a jaw over a neck), and shadows the skin's wrap light.
		float sink = -dot( Noff, Ldir );
		if ( sink > 1e-4 && dOcc > 0.85 * push / sink ) {
			return;
		}
	}
	float gap   = dOcc / max( dL - dOcc, 1e-3 );
	float halfW = u_localParam0.z * pow( gap, max( u_localParam1.w, 0.05 ) );
	float rPx  = min( halfW * u_localParam0.w / max( d, 1.0 ), u_localParam1.x );
	// per screen axis, the extent of a disc lying in the receiver's plane: r * sqrt( 1 - n.axis^2 )
	vec2  ext  = sqrt( max( vec2( 1.0 ) - gn.xy * gn.xy, vec2( 0.04 ) ) );
	fragColor.r = solidHit ? 0.0 : 1.0 - u_shadowFalloffS.z;	// behind a grate only: dimmed, not dark
	fragColor.g = rPx * ext.x;
	fragColor.a = rPx * ext.y;
	out_summary.x = fragColor.r;
	out_summary.y = min( max( fragColor.g, fragColor.a ) / RTSHADOW_HW_MAX, 254.0 / 255.0 );	// < the untraced sentinel
}
