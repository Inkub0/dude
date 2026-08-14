// DUDE tessellation shared helpers (docs/tessellation.md).
//
// PN-triangle (Vlachos et al. 2001) position evaluation plus a crack-free,
// per-edge tessellation factor. Shared verbatim by the interaction and zfill
// tessellation stages so both passes generate identical positions — the depth
// prepass runs depth-LESS and the interaction pass depth-EQUAL against it, so
// any divergence would drop the lit surface. Include AFTER renderparms.glsl
// (references u_mvpMatrix / u_tessParms).

#ifndef TESS_GLSL
#define TESS_GLSL

// hard cap on generated subdivision, independent of the device limit and the
// r_tessLevel slider (which is clamped again on the C++ side)
#define TESS_MAX_LEVEL 32.0

// Per-edge tessellation factor from the edge's two endpoints (model space).
// Computed from the shared endpoints only and symmetric in (a,b), so two
// triangles meeting at an edge derive the exact same factor — no T-junction
// cracks. u_tessParms.x = base level, .y = distance at which subdivision starts
// rolling back toward flat (0 disables the falloff), .w = minimum model-space
// edge length to subdivide at all. clip.w from the MVP is the perspective
// distance proxy and is identical in both passes.
float dudeTessEdgeFactor( vec3 a, vec3 b ) {
	// Leave fine geometry flat. PN smoothing pays off on big low-poly silhouette
	// triangles; on small dense clusters (eyeballs, already-detailed features) it
	// just over-inflates them. An edge shorter than u_tessParms.w keeps factor 1,
	// so those triangles render as the original mesh (PN at level 1 = the corners).
	if ( length( b - a ) < u_tessParms.w ) {
		return 1.0;
	}
	float level = u_tessParms.x;
	if ( u_tessParms.y > 0.0 ) {
		vec3 mid = 0.5 * ( a + b );
		vec4 clip = u_mvpMatrix * vec4( mid, 1.0 );
		float dist = max( clip.w, 0.001 );
		float fade = clamp( u_tessParms.y / dist, 0.0, 1.0 );
		level = mix( 1.0, u_tessParms.x, fade );
	}
	return clamp( level, 1.0, TESS_MAX_LEVEL );
}

// PN-triangle position at barycentric tc (= gl_TessCoord). Corner p0 gets weight
// tc.x, p1 tc.y, p2 tc.z, matching a linear varying interp of
// in[0]*tc.x + in[1]*tc.y + in[2]*tc.z. n0..n2 are the (normalized) corner
// normals. With tess level 1 this returns the flat linear-interpolated point.
vec3 dudeTessPN( vec3 p0, vec3 p1, vec3 p2, vec3 n0, vec3 n1, vec3 n2, vec3 tc ) {
	// corner control points
	vec3 b300 = p0;
	vec3 b030 = p1;
	vec3 b003 = p2;
	// edge control points: the 1/3 division points projected onto the corner
	// tangent planes
	vec3 b210 = ( 2.0 * p0 + p1 - dot( p1 - p0, n0 ) * n0 ) / 3.0;
	vec3 b120 = ( 2.0 * p1 + p0 - dot( p0 - p1, n1 ) * n1 ) / 3.0;
	vec3 b021 = ( 2.0 * p1 + p2 - dot( p2 - p1, n1 ) * n1 ) / 3.0;
	vec3 b012 = ( 2.0 * p2 + p1 - dot( p1 - p2, n2 ) * n2 ) / 3.0;
	vec3 b102 = ( 2.0 * p2 + p0 - dot( p0 - p2, n2 ) * n2 ) / 3.0;
	vec3 b201 = ( 2.0 * p0 + p2 - dot( p2 - p0, n0 ) * n0 ) / 3.0;
	// center control point
	vec3 e = ( b210 + b120 + b021 + b012 + b102 + b201 ) / 6.0;
	vec3 vmid = ( p0 + p1 + p2 ) / 3.0;
	vec3 b111 = e + ( e - vmid ) * 0.5;

	float a = tc.x, b = tc.y, c = tc.z;
	float a2 = a * a, b2 = b * b, c2 = c * c;

	return b300 * ( a2 * a ) + b030 * ( b2 * b ) + b003 * ( c2 * c )
	     + b210 * ( 3.0 * a2 * b ) + b120 * ( 3.0 * a * b2 )
	     + b021 * ( 3.0 * b2 * c ) + b012 * ( 3.0 * b * c2 )
	     + b102 * ( 3.0 * a * c2 ) + b201 * ( 3.0 * a2 * c )
	     + b111 * ( 6.0 * a * b * c );
}

// Normal-map displacement (docs/tessellation.md Phase 2). Doom 3 ships no runtime
// height maps, so this approximates one from the bump (normal) map: the tangent-
// space up component (blue channel, unaffected by the RXGB swizzle the fragment
// path uses) is ~1 on flat areas and drops on detailed slopes, so relief = 1 - z
// pushes detail along the geometric normal. u_tessParms.z is the signed strength
// in world units (0 = off; + raises detail, - carves it in). Sampled at an
// explicit LOD (no derivatives in a tese) so every pass reads the same texel and
// the displaced depth stays identical across zfill / interaction / ambient.
//
// seam is the interpolated per-vertex UV-seam mask (var_ModelNormal.w, stamped by
// idMD5Mesh into the vertex alpha): 1 in the interior of a UV chart, 0 on a chart
// boundary. It exists because the height is read AT THE UV, and the two halves of a
// UV seam are position-coincident vertices with deliberately DIFFERENT UVs -- they
// sample different texels, displace by different amounts and pull apart, which is
// what opened the gaps around hands and shoulders. Welding the normals fixed the
// direction the two halves move in; only pinning them fixes the distance. The mask
// interpolates linearly across the patch, so displacement ramps back to full a
// triangle away from the seam rather than stepping. Deliberately not dialable:
// anything short of a full pin leaves a proportionally smaller gap, which is still a
// gap, so the only useful setting is the one that closes it.
vec3 dudeTessDisplace( vec3 pos, vec3 geoN, sampler2D bumpMap, vec2 uv, float seam ) {
	if ( u_tessParms.z == 0.0 ) {
		return pos;
	}
	float bz = textureLod( bumpMap, uv, 0.0 ).z * 2.0 - 1.0;
	float relief = 1.0 - clamp( bz, 0.0, 1.0 );
	relief *= clamp( seam, 0.0, 1.0 );
	return pos + geoN * ( relief * u_tessParms.z );
}

#endif
