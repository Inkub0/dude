/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.h for license header)
===========================================================================
*/

// DUDE offline ambient-occlusion baker (docs/occlusion-maps.md).
//
// Ray-casts self-occlusion for a render model and writes a grayscale AO texture per
// surface into generated/aomaps/<surface-material-name>.tga. The occlusion-map runtime
// (RhiWorld.cpp) auto-loads those for model-entity surfaces when the material has no
// explicit `occlusionmap` stage. This is an offline precompute (like renderbump / dmap),
// plus a lazy on-demand entry point (AO_BakeModelToCache) the renderer calls under
// r_occlusionMapsAutoBake. Static models (LWO/ASE) bake their resident geometry directly;
// MD5 characters are instantiated in their reference/bind pose first (that snapshot carries
// the deformed geometry -- the base MD5 model exposes none). Pure CPU + filesystem — no GL —
// so it is safe to call mid-frame.
//
// The approach mirrors renderbump: rasterize each triangle into UV/texture space, and for
// every covered texel find the object-space point + interpolated normal, then integrate
// visibility over the hemisphere. Here the rays are cast against the model's OWN triangles
// (self-occlusion) with a bounded distance + falloff, so it reads as contact/cavity
// darkening — complementary to screen-space SSAO.

#include "sys/platform.h"
#include "idlib/geometry/JointTransform.h"		// idJointMat / idJointQuat (MD5 bind-pose bake)
#include "renderer/ModelManager.h"
#include "renderer/tr_local.h"
#include "framework/FileSystem.h"

#include "tools/compilers/compiler_public.h"
#include "tools/compilers/aobake/aobake.h"

#include <thread>		// per-texel parallel bake (fork-join worker pool)
#include <atomic>
#include <vector>

// Bake tunables, shared by the console commands (overridable per-invocation via args) and
// the lazy on-demand path (which uses these values as-is). Kept here with the baker.
idCVar r_occlusionMapBakeSize( "r_occlusionMapBakeSize", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "AO baker: output texture resolution (square); 0 = auto (match each surface's diffuse map resolution, 1:1 with the shipped textures)", 0, 4096 );
idCVar r_occlusionMapBakeRays( "r_occlusionMapBakeRays", "128", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "AO baker: hemisphere rays cast per texel (more = smoother, slower)", 1, 4096 );
idCVar r_occlusionMapBakeDist( "r_occlusionMapBakeDist", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "AO baker: max occlusion ray distance in world units (0 = auto: 25% of the model's longest bounds axis)", 0.0f, 100000.0f );
idCVar r_occlusionMapBakeContrast( "r_occlusionMapBakeContrast", "1.0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "AO baker: contrast/gamma applied to the visibility term (>1 deepens creases)", 0.1f, 8.0f );
idCVar r_occlusionMapBakeDilate( "r_occlusionMapBakeDilate", "4", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "AO baker: pixels of edge dilation/padding across UV seams so bilinear/mips don't bleed", 0, 32 );
idCVar r_occlusionMapBakeDetail( "r_occlusionMapBakeDetail", "1.5", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "AO baker: strength of surface-detail cavity taken from the material's bump/normal map (0 = geometry only; grooves/seams/rivets in the normal map darken the AO)", 0.0f, 4.0f );
// NOT archived on purpose: the ideal worker count is a property of the machine, and archiving it
// would freeze whatever value a past run happened to use. 0 = auto (all hardware threads).
idCVar r_occlusionMapBakeThreads( "r_occlusionMapBakeThreads", "0", CVAR_RENDERER | CVAR_INTEGER, "AO baker: worker threads for the per-texel bake (0 = auto = all hardware threads; 1 = serial)", 0, 256 );

/*
====================================================================================

	A flat triangle soup + uniform grid for ray queries against the whole model.

====================================================================================
*/

struct aoTri_t {
	idVec3	v[3];
	idVec3	normal;		// geometric (for the tiny backface/self bias)
};

struct aoGrid_t {
	idBounds			bounds;
	int					dim[3];
	idVec3				cellSize;
	idList<aoTri_t>		tris;
	// cells[x][y][z] -> list of triangle indices; stored flat
	idList< idList<int> > cells;

	int CellIndex( int x, int y, int z ) const { return ( z * dim[1] + y ) * dim[0] + x; }
};

static void AO_ClampCell( const aoGrid_t &g, int c[3] ) {
	for ( int i = 0; i < 3; i++ ) {
		if ( c[i] < 0 ) c[i] = 0;
		if ( c[i] >= g.dim[i] ) c[i] = g.dim[i] - 1;
	}
}

// bucket a triangle into every cell its AABB touches
static void AO_InsertTri( aoGrid_t &g, int triIndex ) {
	const aoTri_t &t = g.tris[triIndex];
	idBounds tb;
	tb.Clear();
	tb.AddPoint( t.v[0] );
	tb.AddPoint( t.v[1] );
	tb.AddPoint( t.v[2] );

	int lo[3], hi[3];
	for ( int i = 0; i < 3; i++ ) {
		lo[i] = (int)floor( ( tb[0][i] - g.bounds[0][i] ) / g.cellSize[i] );
		hi[i] = (int)floor( ( tb[1][i] - g.bounds[0][i] ) / g.cellSize[i] );
	}
	AO_ClampCell( g, lo );
	AO_ClampCell( g, hi );

	for ( int z = lo[2]; z <= hi[2]; z++ ) {
		for ( int y = lo[1]; y <= hi[1]; y++ ) {
			for ( int x = lo[0]; x <= hi[0]; x++ ) {
				g.cells[ g.CellIndex( x, y, z ) ].Append( triIndex );
			}
		}
	}
}

// Build one grid over ALL surfaces of the model (whole-model self-occlusion).
static void AO_BuildGrid( const idRenderModel *model, aoGrid_t &g ) {
	g.bounds = model->Bounds( NULL );
	// pad a hair so surface points sitting exactly on the bounds land in-range
	g.bounds.ExpandSelf( 1.0f );

	idVec3 size = g.bounds[1] - g.bounds[0];
	// target ~ a few thousand cells; scale each axis by its share of the extent
	const float targetCells = 24.0f;
	float maxAxis = Max( size[0], Max( size[1], size[2] ) );
	if ( maxAxis <= 0.0f ) maxAxis = 1.0f;
	for ( int i = 0; i < 3; i++ ) {
		g.dim[i] = (int)( targetCells * size[i] / maxAxis );
		if ( g.dim[i] < 1 ) g.dim[i] = 1;
		g.cellSize[i] = size[i] / g.dim[i];
		if ( g.cellSize[i] <= 0.0f ) g.cellSize[i] = 1.0f;
	}
	g.cells.SetNum( g.dim[0] * g.dim[1] * g.dim[2] );

	for ( int s = 0; s < model->NumSurfaces(); s++ ) {
		const modelSurface_t *surf = model->Surface( s );
		const srfTriangles_t *tri = surf->geometry;
		if ( !tri || !tri->verts || !tri->indexes ) {
			continue;
		}
		// Skip non-drawn surfaces (collision hulls, nodraw). A model's collision surface is
		// a crude box that ENCLOSES the visual mesh; letting it occlude blackens everything
		// (worst on faces wrapped tight against it). AO must come from the visible geometry
		// only. (docs/occlusion-maps.md)
		if ( !surf->shader || !surf->shader->IsDrawn() ) {
			continue;
		}
		for ( int i = 0; i + 2 < tri->numIndexes + 0; i += 3 ) {
			if ( i + 2 >= tri->numIndexes ) break;
			aoTri_t at;
			at.v[0] = tri->verts[ tri->indexes[i + 0] ].xyz;
			at.v[1] = tri->verts[ tri->indexes[i + 1] ].xyz;
			at.v[2] = tri->verts[ tri->indexes[i + 2] ].xyz;
			at.normal = ( at.v[1] - at.v[0] ).Cross( at.v[2] - at.v[0] );
			at.normal.Normalize();
			int idx = g.tris.Append( at );
			AO_InsertTri( g, idx );
		}
	}
}

// Moller-Trumbore. Returns the hit distance along dir (>0) or -1 for no hit.
static float AO_RayTri( const idVec3 &orig, const idVec3 &dir, const aoTri_t &t, float maxDist ) {
	const float EPS = 1e-6f;
	idVec3 e1 = t.v[1] - t.v[0];
	idVec3 e2 = t.v[2] - t.v[0];
	idVec3 p = dir.Cross( e2 );
	float det = e1 * p;
	if ( det > -EPS && det < EPS ) {
		return -1.0f;			// parallel
	}
	float inv = 1.0f / det;
	idVec3 tvec = orig - t.v[0];
	float u = ( tvec * p ) * inv;
	if ( u < 0.0f || u > 1.0f ) {
		return -1.0f;
	}
	idVec3 q = tvec.Cross( e1 );
	float v = ( dir * q ) * inv;
	if ( v < 0.0f || u + v > 1.0f ) {
		return -1.0f;
	}
	float dist = ( e2 * q ) * inv;
	if ( dist <= 0.0f || dist > maxDist ) {
		return -1.0f;
	}
	return dist;
}

// Walk the grid along the ray, return the nearest hit distance within maxDist (or -1).
// Uses a bounded stepped traversal (like renderbump's SampleHighMesh) with a per-query
// visited stamp so each triangle is tested once. Small models -> a handful of cells/tris.
// Per-thread scratch for the grid walk. These were file-static globals; making them per-worker
// is what lets the bake run many texels in parallel. The grid itself is read-only during tracing
// and stays shared -- only this visited-stamp state must be private to each thread.
struct aoTraceCtx_t {
	int			rayStamp;
	idList<int>	cellStamp;		// parallel to grid cells
	aoTraceCtx_t() : rayStamp( 0 ) {}
};

static float AO_TraceNearest( const aoGrid_t &g, aoTraceCtx_t &ctx, const idVec3 &orig, const idVec3 &dir, float maxDist ) {
	ctx.rayStamp++;
	if ( ctx.cellStamp.Num() != g.cells.Num() ) {
		ctx.cellStamp.SetNum( g.cells.Num() );
		for ( int i = 0; i < ctx.cellStamp.Num(); i++ ) ctx.cellStamp[i] = 0;
	}

	float best = -1.0f;
	// step in ~half-cell increments so we don't skip a cell
	float step = Min( g.cellSize[0], Min( g.cellSize[1], g.cellSize[2] ) ) * 0.5f;
	if ( step <= 0.0f ) step = maxDist;
	const int maxSteps = 4096;
	int steps = (int)( maxDist / step ) + 2;
	if ( steps > maxSteps ) steps = maxSteps;

	for ( int i = 0; i <= steps; i++ ) {
		idVec3 p = orig + dir * ( step * i );
		int c[3];
		for ( int k = 0; k < 3; k++ ) {
			c[k] = (int)floor( ( p[k] - g.bounds[0][k] ) / g.cellSize[k] );
		}
		if ( c[0] < 0 || c[0] >= g.dim[0] || c[1] < 0 || c[1] >= g.dim[1] || c[2] < 0 || c[2] >= g.dim[2] ) {
			continue;
		}
		int ci = g.CellIndex( c[0], c[1], c[2] );
		if ( ctx.cellStamp[ci] == ctx.rayStamp ) {
			continue;		// already tested this cell for this ray
		}
		ctx.cellStamp[ci] = ctx.rayStamp;

		const idList<int> &cell = g.cells[ci];
		for ( int j = 0; j < cell.Num(); j++ ) {
			const aoTri_t &tri = g.tris[ cell[j] ];
			// Backface cull: only count a hit on an occluder's FRONT face (its normal
			// opposes the ray). This is the standard cure for self-occlusion on hollow /
			// single-sided / inverted-normal prop geometry -- otherwise a downward ray from
			// an exterior bottom face exits through the model's own far/interior walls and
			// counts them as occlusion, blackening whole faces. Front-facing occluders
			// (real cavities, drawer recesses) still block correctly.
			if ( tri.normal * dir >= 0.0f ) {
				continue;
			}
			float d = AO_RayTri( orig, dir, tri, maxDist );
			if ( d > 0.0f && ( best < 0.0f || d < best ) ) {
				best = d;
			}
		}
	}
	return best;
}

/*
====================================================================================

	Hemisphere sampling (deterministic, cosine-weighted).

====================================================================================
*/

// van der Corput / Hammersley: low-discrepancy, no RNG -> reproducible bakes.
static float AO_RadicalInverse( unsigned int bits ) {
	bits = ( bits << 16u ) | ( bits >> 16u );
	bits = ( ( bits & 0x55555555u ) << 1u ) | ( ( bits & 0xAAAAAAAAu ) >> 1u );
	bits = ( ( bits & 0x33333333u ) << 2u ) | ( ( bits & 0xCCCCCCCCu ) >> 2u );
	bits = ( ( bits & 0x0F0F0F0Fu ) << 4u ) | ( ( bits & 0xF0F0F0F0u ) >> 4u );
	bits = ( ( bits & 0x00FF00FFu ) << 8u ) | ( ( bits & 0xFF00FF00u ) >> 8u );
	return (float)( (double)bits * 2.3283064365386963e-10 );	// / 2^32
}

// Cosine-weighted hemisphere direction around +Z for sample i of n (Malley's method).
static idVec3 AO_CosineHemisphere( int i, int n ) {
	float u1 = ( i + 0.5f ) / (float)n;
	float u2 = AO_RadicalInverse( (unsigned int)i );
	float r = idMath::Sqrt( u1 );			// cosine weighting baked into the radius
	float phi = 2.0f * idMath::PI * u2;
	return idVec3( r * idMath::Cos( phi ), r * idMath::Sin( phi ), idMath::Sqrt( Max( 0.0f, 1.0f - u1 ) ) );
}

// Build an orthonormal basis with n as +Z (Duff et al.).
static void AO_Basis( const idVec3 &n, idVec3 &t, idVec3 &b ) {
	float sign = ( n.z >= 0.0f ) ? 1.0f : -1.0f;
	float a = -1.0f / ( sign + n.z );
	float d = n.x * n.y * a;
	t = idVec3( 1.0f + sign * n.x * n.x * a, sign * d, -sign * n.x );
	b = idVec3( d, sign + n.y * n.y * a, -n.y );
}

// Integrate visibility at a surface point. 1 = fully open, 0 = fully occluded.
static float AO_SampleVisibility( const aoGrid_t &g, aoTraceCtx_t &ctx, const idVec3 &point, const idVec3 &normal,
                                  int rays, float maxDist, float bias ) {
	idVec3 n = normal;
	n.Normalize();
	idVec3 t, b;
	AO_Basis( n, t, b );
	idVec3 orig = point + n * bias;			// lift off the source surface

	float occ = 0.0f;
	for ( int i = 0; i < rays; i++ ) {
		idVec3 local = AO_CosineHemisphere( i, rays );
		idVec3 dir = t * local.x + b * local.y + n * local.z;
		float d = AO_TraceNearest( g, ctx, orig, dir, maxDist );
		if ( d > 0.0f ) {
			occ += 1.0f - ( d / maxDist );		// distance falloff: near hits occlude most
		}
	}
	return 1.0f - occ / (float)rays;
}

/*
====================================================================================

	Per-surface UV rasterization + output.

====================================================================================
*/

// Signed area helper for barycentric coords in UV space.
static float AO_Edge( float ax, float ay, float bx, float by, float cx, float cy ) {
	return ( bx - ax ) * ( cy - ay ) - ( by - ay ) * ( cx - ax );
}

// Nearest-valid dilation so bilinear/mip sampling doesn't bleed the background across seams.
static void AO_Dilate( byte *ao, byte *covered, int size, int pixels ) {
	idList<byte> src;
	for ( int pass = 0; pass < pixels; pass++ ) {
		src.SetNum( size * size );
		memcpy( src.Ptr(), ao, size * size );
		idList<byte> cov;
		cov.SetNum( size * size );
		memcpy( cov.Ptr(), covered, size * size );

		for ( int y = 0; y < size; y++ ) {
			for ( int x = 0; x < size; x++ ) {
				int o = y * size + x;
				if ( cov[o] ) {
					continue;
				}
				int sum = 0, cnt = 0;
				for ( int dy = -1; dy <= 1; dy++ ) {
					for ( int dx = -1; dx <= 1; dx++ ) {
						int nx = x + dx, ny = y + dy;
						if ( nx < 0 || ny < 0 || nx >= size || ny >= size ) continue;
						int no = ny * size + nx;
						if ( cov[no] ) { sum += src[no]; cnt++; }
					}
				}
				if ( cnt > 0 ) {
					ao[o] = (byte)( sum / cnt );
					covered[o] = 1;
				}
			}
		}
	}
}

/*
====================================================================================

	Surface-detail cavity from the material's bump/normal map.

	The geometric AO above only sees the low-poly mesh, so all the detail authored into
	the normal map (panel seams, rivets, wear) is invisible to it. This adds a cavity term
	read straight from the bump map: for each texel, sample the tangent-space normals around
	it -- where the neighbours lean back toward the centre (a groove / concave crease) the
	texel darkens; convex bumps don't. It works even on flat panels, where perturbing the
	ray hemisphere alone would show nothing. Multiplied into the geometric AO.

	In Doom 3 the bump stage IS the normal map, so this uses GetBumpStage(); if a surface has
	no bump stage, the cavity term is simply skipped (geometry-only, as before).

====================================================================================
*/

struct aoBump_t {
	byte	*pic;
	int		w, h;
};

// CPU-load a surface's bump/normal map (evaluating any addnormals()/heightmap() program).
// Returns false (and leaves bump zeroed) if the surface has no usable bump stage.
static bool AO_LoadBump( const idMaterial *mat, aoBump_t &bump ) {
	bump.pic = NULL; bump.w = bump.h = 0;
	if ( !mat ) {
		return false;
	}
	const shaderStage_t *bs = mat->GetBumpStage();
	if ( !bs || !bs->texture.image ) {
		return false;
	}
	const idStr &name = bs->texture.image->imgName;
	if ( name.Length() == 0 ) {
		return false;
	}
	byte *pic = NULL;
	int w = 0, h = 0;
	ID_TIME_T ts = 0;
	R_LoadImageProgram( name.c_str(), &pic, &w, &h, &ts, NULL );
	if ( !pic || w <= 0 || h <= 0 ) {
		if ( pic ) R_StaticFree( pic );
		return false;
	}
	bump.pic = pic; bump.w = w; bump.h = h;
	return true;
}

static void AO_FreeBump( aoBump_t &bump ) {
	if ( bump.pic ) {
		R_StaticFree( bump.pic );
		bump.pic = NULL;
	}
}

// Texture resolution of the material (for r_occlusionMapBakeSize 0 = auto: bake the AO at the
// same size as the texture it will multiply against, so it is 1:1 with the shipped art and never
// larger than can show). Prefers the diffuse stage (the AO is baked in its UV set), then the bump
// stage (same authored resolution in Doom 3), then any stage with an image -- so eye/decal
// materials that carry only a `blend filter` stage still resolve to their real, small texture
// instead of a fat default. Loads the image only to read its dimensions, then frees it. Returns
// false only when the material has no textured stage at all.
static bool AO_MaterialTexSize( const idMaterial *mat, int &outW, int &outH ) {
	if ( !mat ) {
		return false;
	}
	const shaderStage_t *pick = NULL;
	for ( int pass = 0; pass < 3 && !pick; pass++ ) {
		for ( int i = 0; i < mat->GetNumStages(); i++ ) {
			const shaderStage_t *s = mat->GetStage( i );
			if ( !s->texture.image ) {
				continue;
			}
			if ( pass == 0 && s->lighting != SL_DIFFUSE ) continue;	// prefer diffuse
			if ( pass == 1 && s->lighting != SL_BUMP ) continue;	// then bump
			pick = s;												// then anything textured
			break;
		}
	}
	if ( !pick || !pick->texture.image ) {
		return false;
	}
	const idStr &name = pick->texture.image->imgName;
	if ( name.Length() == 0 ) {
		return false;
	}
	byte *pic = NULL;
	int w = 0, h = 0;
	ID_TIME_T ts = 0;
	R_LoadImageProgram( name.c_str(), &pic, &w, &h, &ts, NULL );
	if ( !pic || w <= 0 || h <= 0 ) {
		if ( pic ) R_StaticFree( pic );
		return false;
	}
	R_StaticFree( pic );
	outW = w;
	outH = h;
	return true;
}

// Bilinear-sample the tangent-space normal at UV (wrapped). RGB -> normal = rgb*2-1.
static idVec3 AO_BumpNormalAt( const aoBump_t &b, float u, float v ) {
	u -= idMath::Floor( u );
	v -= idMath::Floor( v );
	float fx = u * b.w - 0.5f, fy = v * b.h - 0.5f;
	int x0 = (int)idMath::Floor( fx ), y0 = (int)idMath::Floor( fy );
	float tx = fx - x0, ty = fy - y0;
	idVec3 acc( 0.0f, 0.0f, 0.0f );
	for ( int j = 0; j < 2; j++ ) {
		for ( int i = 0; i < 2; i++ ) {
			int xx = ( ( x0 + i ) % b.w + b.w ) % b.w;
			int yy = ( ( y0 + j ) % b.h + b.h ) % b.h;
			const byte *p = &b.pic[ ( yy * b.w + xx ) * 4 ];
			idVec3 n( p[0] / 127.5f - 1.0f, p[1] / 127.5f - 1.0f, p[2] / 127.5f - 1.0f );
			float wgt = ( i ? tx : 1.0f - tx ) * ( j ? ty : 1.0f - ty );
			acc += n * wgt;
		}
	}
	if ( acc.Normalize() == 0.0f ) {
		acc.Set( 0.0f, 0.0f, 1.0f );
	}
	return acc;
}

// Cavity at UV from the bump map: neighbours whose normals lean back toward the centre
// (concave crease) occlude it. 1 = open, <1 = in a groove. radiusUV in [0,1] texture space.
static float AO_DetailCavity( const aoBump_t &b, float u, float v, float radiusUV, float strength ) {
	const int N = 8;
	float occ = 0.0f;
	for ( int i = 0; i < N; i++ ) {
		float ang = ( 2.0f * idMath::PI * i ) / (float)N;
		float dx = idMath::Cos( ang ), dy = idMath::Sin( ang );
		idVec3 nj = AO_BumpNormalAt( b, u + dx * radiusUV, v + dy * radiusUV );
		float t = -( nj.x * dx + nj.y * dy );	// >0 when the neighbour faces back toward centre
		if ( t > 0.0f ) {
			occ += t;
		}
	}
	occ /= (float)N;
	return idMath::ClampFloat( 0.0f, 1.0f, 1.0f - strength * occ );
}

// Per-triangle raster setup, precomputed once (serial) so the parallel per-row pass is pure
// integration -- the barycentric edge/area and clipped bbox don't have to be recomputed per row.
struct aoTriSetup_t {
	float	ax, ay, bx, by, cx, cy;		// UV-space vertex positions (in texels)
	float	invArea;					// 1 / signed UV area (barycentric denominator)
	int		minX, maxX, minY, maxY;		// covered-texel bbox, clipped to the image
	idVec3	p0, p1, p2;					// object-space positions (for interpolation)
	idVec3	n0, n1, n2;					// vertex normals
};

// Integrate AO for a single output row `y` across every triangle that covers it, writing into
// ao/covered. The parallel bake assigns each row to exactly one worker, so this thread owns row
// `y`'s texels exclusively -- no locking is needed. `ctx` is this thread's private grid-walk
// scratch. The overlap rule (keep the darker value) is order-independent, so the result is
// byte-identical to a serial bake regardless of how rows are scheduled.
static void AO_BakeRow( int y, const aoGrid_t &g, const idList<aoTriSetup_t> &setups,
                        byte *ao, byte *covered, int size, int rays, float maxDist, float bias,
                        float contrast, const aoBump_t *bump, float detail,
                        aoTraceCtx_t &ctx, bool &wroteAny ) {
	const float py = y + 0.5f;
	for ( int ti = 0; ti < setups.Num(); ti++ ) {
		const aoTriSetup_t &s = setups[ti];
		if ( y < s.minY || y >= s.maxY ) {
			continue;
		}
		for ( int x = s.minX; x < s.maxX; x++ ) {
			float px = x + 0.5f;
			float w0 = AO_Edge( s.bx, s.by, s.cx, s.cy, px, py ) * s.invArea;
			float w1 = AO_Edge( s.cx, s.cy, s.ax, s.ay, px, py ) * s.invArea;
			float w2 = AO_Edge( s.ax, s.ay, s.bx, s.by, px, py ) * s.invArea;
			if ( w0 < 0.0f || w1 < 0.0f || w2 < 0.0f ) {
				continue;		// texel center outside this triangle
			}

			idVec3 point  = s.p0 * w0 + s.p1 * w1 + s.p2 * w2;
			idVec3 normal = s.n0 * w0 + s.n1 * w1 + s.n2 * w2;

			float vis = AO_SampleVisibility( g, ctx, point, normal, rays, maxDist, bias );

			// surface-detail cavity from the bump/normal map (grooves/seams darken).
			if ( bump && detail > 0.0f ) {
				float u = ( x + 0.5f ) / (float)size;
				float v = ( y + 0.5f ) / (float)size;
				vis *= AO_DetailCavity( *bump, u, v, 2.0f / (float)size, detail );
			}

			vis = idMath::Pow( idMath::ClampFloat( 0.0f, 1.0f, vis ), contrast );

			int o = y * size + x;
			byte val = (byte)( idMath::ClampFloat( 0.0f, 1.0f, vis ) * 255.0f + 0.5f );
			// overlapping UVs: keep the darker (more-occluded) contribution
			if ( !covered[o] || val < ao[o] ) {
				ao[o] = val;
			}
			covered[o] = 1;
			wroteAny = true;
		}
	}
}

// Bake one surface: rasterize its triangles, integrate AO per texel, dilate, write TGA to
// outPath. Returns false if nothing was written (degenerate UVs / no coverage). `bump` may be
// NULL (no bump stage) -> geometry-only AO.
static bool AO_BakeSurface( const aoGrid_t &g, const srfTriangles_t *tri, const char *outPath,
                            int size, int rays, float maxDist, float bias, float contrast, int dilate,
                            const aoBump_t *bump, float detail ) {
	if ( !tri || !tri->verts || !tri->indexes || tri->numIndexes < 3 ) {
		return false;
	}

	idList<byte> ao;      ao.SetNum( size * size );      memset( ao.Ptr(), 255, size * size );
	idList<byte> covered; covered.SetNum( size * size ); memset( covered.Ptr(), 0, size * size );

	bool wroteAny = false;

	// Precompute each triangle's raster setup once (serial, cheap). AO bakes into the 0..1 UV
	// region (tiling UVs outside 0..1 aren't supported -- props use unique/atlas UVs).
	idList<aoTriSetup_t> setups;
	for ( int i = 0; i + 2 < tri->numIndexes; i += 3 ) {
		const idDrawVert &a  = tri->verts[ tri->indexes[i + 0] ];
		const idDrawVert &bv = tri->verts[ tri->indexes[i + 1] ];
		const idDrawVert &c  = tri->verts[ tri->indexes[i + 2] ];

		aoTriSetup_t s;
		s.ax = a.st.x * size;  s.ay = a.st.y * size;
		s.bx = bv.st.x * size; s.by = bv.st.y * size;
		s.cx = c.st.x * size;  s.cy = c.st.y * size;

		float area = AO_Edge( s.ax, s.ay, s.bx, s.by, s.cx, s.cy );
		if ( idMath::Fabs( area ) < 1e-6f ) {
			continue;		// degenerate in UV
		}
		s.invArea = 1.0f / area;

		s.minX = (int)floor( Min( s.ax, Min( s.bx, s.cx ) ) );
		s.maxX = (int)ceil ( Max( s.ax, Max( s.bx, s.cx ) ) );
		s.minY = (int)floor( Min( s.ay, Min( s.by, s.cy ) ) );
		s.maxY = (int)ceil ( Max( s.ay, Max( s.by, s.cy ) ) );
		if ( s.minX < 0 ) s.minX = 0;
		if ( s.minY < 0 ) s.minY = 0;
		if ( s.maxX > size ) s.maxX = size;
		if ( s.maxY > size ) s.maxY = size;
		if ( s.minX >= s.maxX || s.minY >= s.maxY ) {
			continue;		// no covered texels
		}

		s.p0 = a.xyz;    s.p1 = bv.xyz;    s.p2 = c.xyz;
		s.n0 = a.normal; s.n1 = bv.normal; s.n2 = c.normal;
		setups.Append( s );
	}

	if ( setups.Num() == 0 ) {
		return false;
	}

	byte *aoPtr  = ao.Ptr();
	byte *covPtr = covered.Ptr();

	// Integrate AO per output row, fanning the rows across worker threads. The ray casting is the
	// whole cost and every texel is independent; rows are disjoint texel sets so no synchronization
	// is needed and the output stays byte-identical to a serial bake. Model loading/instantiation
	// happened serially upstream -- only this pure math runs wide.
	int nThreads = r_occlusionMapBakeThreads.GetInteger();
	if ( nThreads <= 0 ) {
		nThreads = (int)std::thread::hardware_concurrency();
	}
	if ( nThreads < 1 ) nThreads = 1;
	if ( nThreads > size ) nThreads = size;		// no point in more workers than rows

	if ( nThreads <= 1 ) {
		aoTraceCtx_t ctx;
		for ( int y = 0; y < size; y++ ) {
			AO_BakeRow( y, g, setups, aoPtr, covPtr, size, rays, maxDist, bias, contrast,
			            bump, detail, ctx, wroteAny );
		}
	} else {
		std::atomic<int> nextRow( 0 );
		idList<byte> wroteFlags;
		wroteFlags.SetNum( nThreads );
		memset( wroteFlags.Ptr(), 0, nThreads );

		std::vector<std::thread> pool;
		pool.reserve( nThreads );
		for ( int t = 0; t < nThreads; t++ ) {
			pool.emplace_back( [&, t]() {
				aoTraceCtx_t ctx;			// private grid-walk scratch per worker
				bool wrote = false;
				for ( ;; ) {
					int y = nextRow.fetch_add( 1 );
					if ( y >= size ) {
						break;
					}
					AO_BakeRow( y, g, setups, aoPtr, covPtr, size, rays, maxDist, bias, contrast,
					            bump, detail, ctx, wrote );
				}
				wroteFlags[t] = wrote ? 1 : 0;
			} );
		}
		for ( size_t t = 0; t < pool.size(); t++ ) {
			pool[t].join();
		}
		for ( int t = 0; t < nThreads; t++ ) {
			if ( wroteFlags[t] ) {
				wroteAny = true;
			}
		}
	}

	if ( !wroteAny ) {
		return false;
	}

	AO_Dilate( ao.Ptr(), covered.Ptr(), size, dilate );

	// pack into RGBA (grayscale in RGB, opaque A) and write
	idList<byte> rgba;
	rgba.SetNum( size * size * 4 );
	for ( int i = 0; i < size * size; i++ ) {
		byte v = ao[i];
		rgba[i * 4 + 0] = v;
		rgba[i * 4 + 1] = v;
		rgba[i * 4 + 2] = v;
		rgba[i * 4 + 3] = 255;
	}

	R_WriteTGA( outPath, rgba.Ptr(), size, size, false );
	common->Printf( "  wrote %s (%dx%d)\n", outPath, size, size );
	return true;
}

/*
====================================================================================

	Public entry points.

====================================================================================
*/

void AO_GeneratedPathForSurface( const char *modelName, int surfaceIndex, idStr &out ) {
	idStr name = modelName;
	name.BackSlashesToSlashes();
	name.StripLeading( '/' );		// keep the path under generated/aomaps/
	name.StripFileExtension();		// drop .lwo/.ase -> stable per model+surface
	out = va( "generated/aomaps/%s_s%d.tga", name.c_str(), surfaceIndex );
}

// Instantiate an MD5 (or any DM_CACHED/DM_CONTINUOUS) model in its reference/bind pose and
// return the resulting static snapshot (caller owns it; delete when done). The snapshot's
// surfaces carry the deformed bind-pose geometry the bake needs -- the base MD5 model exposes
// none. NULL if the model has no usable skeleton. `skin` (if given) is applied so a skin that
// makes a mesh nodraw is culled here too; geometry is otherwise skin-independent.
//
// The global bind pose is rebuilt from the model's relative default pose exactly as
// idRenderModelMD5::LoadModel does: local joint matrices accumulated down the hierarchy (in
// MD5 a parent always precedes its children, so a single forward pass suffices).
static idRenderModel *AO_InstantiateBindPose( idRenderModel *model, const idDeclSkin *skin ) {
	const int numJoints = model->NumJoints();
	if ( numJoints <= 0 ) {
		return NULL;
	}
	const idJointQuat *pose = model->GetDefaultPose();
	const idMD5Joint  *mj   = model->GetJoints();
	if ( !pose || !mj ) {
		return NULL;
	}

	// SIMDProcessor->TransformVerts (inside UpdateSurface) reads the joint matrices with
	// aligned SSE loads, so this array must be 16-byte aligned.
	idJointMat *jm = (idJointMat *)Mem_Alloc16( numJoints * sizeof( jm[0] ) );
	for ( int i = 0; i < numJoints; i++ ) {
		jm[i].SetRotation( pose[i].q.ToMat3() );
		jm[i].SetTranslation( pose[i].t );
	}
	for ( int i = 0; i < numJoints; i++ ) {
		if ( mj[i].parent ) {
			jm[i] *= jm[ mj[i].parent - mj ];		// local -> global (parents precede children)
		}
	}

	renderEntity_t ent;
	memset( &ent, 0, sizeof( ent ) );
	ent.axis      = mat3_identity;
	ent.numJoints = numJoints;
	ent.joints    = jm;
	ent.customSkin = skin;
	ent.bounds    = model->Bounds( NULL );

	// The game defers tangent/normal generation to the backend (r_useDeferredTangents), but the
	// CPU bake reads per-vertex normals straight off the snapshot, so force them to be built now.
	// Also neutralize r_showSkel, which would otherwise make InstantiateDynamicModel hand back an
	// empty (skeleton-only) model and bake nothing.
	const bool prevDefer = r_useDeferredTangents.GetBool();
	const int  prevSkel  = r_showSkel.GetInteger();
	r_useDeferredTangents.SetBool( false );
	r_showSkel.SetInteger( 0 );
	idRenderModel *snap = model->InstantiateDynamicModel( &ent, NULL, NULL );
	r_useDeferredTangents.SetBool( prevDefer );
	r_showSkel.SetInteger( prevSkel );

	Mem_Free16( jm );
	return snap;
}

// Bake every drawn surface of `geoModel` (self-occlusion: rays cast against its own triangles)
// and write one grayscale map per surface, keyed by `pathName` + a surface index. `keyById`
// picks the index: the surface's position for static models, or its persistent id (== MD5 mesh
// index) for the bind-pose snapshot -- the runtime resolver matches the same id, so a skin that
// culls some meshes still lines the rest up. `skin` (if given) only selects which material's
// bump feeds the detail cavity; the geometry, hence the AO, is skin-independent.
static bool AO_BakeGeoModel( const idRenderModel *geoModel, const char *pathName, bool keyById,
                             const idDeclSkin *skin ) {
	if ( geoModel->NumSurfaces() == 0 ) {
		return false;
	}

	// size 0 = auto: pick each surface's size from its diffuse map below (per-surface, since a
	// head mixes 256px skin with smaller teeth/eye maps). A positive value forces every surface.
	const int   sizeCvar = r_occlusionMapBakeSize.GetInteger();
	const bool  autoSize = ( sizeCvar <= 0 );
	const int   rays     = idMath::ClampInt( 1, 4096, r_occlusionMapBakeRays.GetInteger() );
	const int   dilate   = idMath::ClampInt( 0, 32, r_occlusionMapBakeDilate.GetInteger() );
	const float contrast = r_occlusionMapBakeContrast.GetFloat();

	idBounds b = geoModel->Bounds( NULL );
	idVec3 ext = b[1] - b[0];
	float longest = Max( ext[0], Max( ext[1], ext[2] ) );
	float maxDist = r_occlusionMapBakeDist.GetFloat();
	if ( maxDist <= 0.0f ) {
		maxDist = longest * 0.25f;		// auto: quarter of the longest axis = "nearby"
	}
	if ( maxDist <= 0.0f ) {
		maxDist = 8.0f;
	}
	const float bias = Max( longest * 1e-4f, 0.05f );

	int reqThreads = r_occlusionMapBakeThreads.GetInteger();
	int effThreads = ( reqThreads > 0 ) ? reqThreads : (int)std::thread::hardware_concurrency();
	if ( effThreads < 1 ) effThreads = 1;

	common->Printf( "bakeAO: '%s' (%d surfaces, %d rays, %s, dist %.1f, %d threads)\n",
	                pathName, geoModel->NumSurfaces(), rays,
	                autoSize ? "auto px" : va( "%dpx", idMath::ClampInt( 16, 4096, sizeCvar ) ), maxDist,
	                effThreads );

	aoGrid_t grid;
	AO_BuildGrid( geoModel, grid );

	const float detail = idMath::ClampFloat( 0.0f, 4.0f, r_occlusionMapBakeDetail.GetFloat() );

	bool any = false;
	for ( int s = 0; s < geoModel->NumSurfaces(); s++ ) {
		const modelSurface_t *surf = geoModel->Surface( s );
		// skip collision/nodraw surfaces (also excluded from the occlusion grid above)
		if ( !surf->shader || !surf->shader->IsDrawn() ) {
			continue;
		}
		// The skin (if given) only selects which bump/diffuse feeds the size + detail cavity.
		const idMaterial *bumpMat = surf->shader;
		if ( skin ) {
			const idMaterial *m = R_RemapShaderBySkin( surf->shader, skin, NULL );
			if ( m ) {
				bumpMat = m;
			}
		}

		// output resolution: fixed (sizeCvar) or auto (this surface's texture size, so the AO is
		// 1:1 with the shipped art). 256 is a neutral fallback for a material with no texture.
		int size;
		if ( autoSize ) {
			int dw = 0, dh = 0;
			size = AO_MaterialTexSize( bumpMat, dw, dh ) ? Max( dw, dh ) : 256;
		} else {
			size = sizeCvar;
		}
		size = idMath::ClampInt( 16, 4096, size );

		idStr outPath;
		AO_GeneratedPathForSurface( pathName, keyById ? surf->id : s, outPath );

		aoBump_t bump;
		const bool hasBump = ( detail > 0.0f ) && AO_LoadBump( bumpMat, bump );
		if ( AO_BakeSurface( grid, surf->geometry, outPath.c_str(),
		                     size, rays, maxDist, bias, contrast, dilate,
		                     hasBump ? &bump : NULL, detail ) ) {
			any = true;
		}
		if ( hasBump ) {
			AO_FreeBump( bump );
		}
	}
	return any;
}

// Core bake, shared by the commands and the lazy path. Uses the r_occlusionMapBake* cvars
// for parameters (the commands may have already set them from args). Returns true if it
// wrote at least one map. Handles static models directly and MD5 characters via a bind-pose
// snapshot -- output is keyed by model + surface index either way, so it is skin- and
// material-independent (one bake serves every skin/instance and never collides with another
// model that shares a material). `skin` (optional) only selects which bump feeds the cavity.
bool AO_BakeModelToCache( const idRenderModel *model, const idDeclSkin *skin ) {
	if ( !model || model->IsDefaultModel() ) {
		return false;
	}

	// static (LWO/ASE): the surface geometry is already resident -- bake it in place, keyed by
	// surface position (what the runtime resolver walks for static model entities).
	if ( model->IsDynamicModel() == DM_STATIC ) {
		return AO_BakeGeoModel( model, model->Name(), false, skin );
	}

	// dynamic (MD5): the base model has no surface geometry -- instantiate the reference/bind
	// pose and bake that snapshot, keyed by the persistent surface id (== mesh index). Baked
	// self-occlusion is where MD5 characters pay off most (concave organic folds/recesses).
	idRenderModel *snap = AO_InstantiateBindPose( const_cast<idRenderModel *>( model ), skin );
	if ( !snap ) {
		common->Printf( "bakeAO: '%s' has no bind-pose geometry to bake\n", model->Name() );
		return false;
	}
	bool any = AO_BakeGeoModel( snap, model->Name(), true, skin );
	delete snap;
	return any;
}

// Parse "-skin <name>" out of the arg list (returns NULL if absent). Skinned entities
// remap materials at runtime, so the maps must be written under the remapped name.
static const idDeclSkin *AO_ParseSkinArg( const idCmdArgs &args ) {
	for ( int i = 2; i < args.Argc() - 1; i++ ) {
		if ( !idStr::Icmp( args.Argv( i ), "-skin" ) ) {
			const char *skinName = args.Argv( i + 1 );
			const idDeclSkin *skin = declManager->FindSkin( skinName, false );
			if ( !skin ) {
				common->Printf( "bakeAO: skin '%s' not found\n", skinName );
			}
			return skin;
		}
	}
	return NULL;
}

/*
===============
BakeAO_f

bakeAO <model> [-size N] [-rays N] [-dist N] [-contrast f] [-skin skins/name]
===============
*/
void BakeAO_f( const idCmdArgs &args ) {
	if ( args.Argc() < 2 ) {
		common->Printf( "usage: bakeAO <model> [-size N] [-rays N] [-dist N] [-contrast f] [-skin skins/name]\n" );
		return;
	}
	// optional flag overrides -> write the shared cvars for this invocation
	for ( int i = 2; i < args.Argc() - 1; i++ ) {
		const char *a = args.Argv( i );
		if ( !idStr::Icmp( a, "-size" ) )     { r_occlusionMapBakeSize.SetString( args.Argv( ++i ) ); }
		else if ( !idStr::Icmp( a, "-rays" ) )     { r_occlusionMapBakeRays.SetString( args.Argv( ++i ) ); }
		else if ( !idStr::Icmp( a, "-dist" ) )     { r_occlusionMapBakeDist.SetString( args.Argv( ++i ) ); }
		else if ( !idStr::Icmp( a, "-contrast" ) ) { r_occlusionMapBakeContrast.SetString( args.Argv( ++i ) ); }
	}
	const idDeclSkin *skin = AO_ParseSkinArg( args );

	const char *modelName = args.Argv( 1 );
	idRenderModel *model = renderModelManager->FindModel( modelName );
	if ( !model || model->IsDefaultModel() ) {
		common->Printf( "bakeAO: couldn't load model '%s'\n", modelName );
		return;
	}

	int start = Sys_Milliseconds();
	bool any = AO_BakeModelToCache( model, skin );
	common->Printf( "bakeAO: %s in %d ms\n", any ? "done" : "nothing baked", Sys_Milliseconds() - start );

	// let the running renderer pick up freshly baked maps without a restart
	R_ResetOcclusionMapCache();
}

/*
===============
BakeAOFolder_f

bakeAOFolder <path> -- bake every static model under a folder (e.g. models/mapobjects/foo)
===============
*/
void BakeAOFolder_f( const idCmdArgs &args ) {
	if ( args.Argc() < 2 ) {
		common->Printf( "usage: bakeAOFolder <path> [-size N] [-rays N] [-dist N] [-contrast f]\n" );
		return;
	}
	for ( int i = 2; i < args.Argc() - 1; i++ ) {
		const char *a = args.Argv( i );
		if ( !idStr::Icmp( a, "-size" ) )     { r_occlusionMapBakeSize.SetString( args.Argv( ++i ) ); }
		else if ( !idStr::Icmp( a, "-rays" ) )     { r_occlusionMapBakeRays.SetString( args.Argv( ++i ) ); }
		else if ( !idStr::Icmp( a, "-dist" ) )     { r_occlusionMapBakeDist.SetString( args.Argv( ++i ) ); }
		else if ( !idStr::Icmp( a, "-contrast" ) ) { r_occlusionMapBakeContrast.SetString( args.Argv( ++i ) ); }
	}

	const char *folder = args.Argv( 1 );
	int total = 0, baked = 0;
	int start = Sys_Milliseconds();

	static const char *exts[] = { "lwo", "ase", "md5mesh", NULL };
	for ( int e = 0; exts[e]; e++ ) {
		idFileList *files = fileSystem->ListFilesTree( folder, va( ".%s", exts[e] ) );
		for ( int i = 0; i < files->GetNumFiles(); i++ ) {
			const char *m = files->GetFile( i );
			total++;
			idRenderModel *model = renderModelManager->FindModel( m );
			if ( model && !model->IsDefaultModel() && AO_BakeModelToCache( model ) ) {
				baked++;
			}
		}
		fileSystem->FreeFileList( files );
	}

	common->Printf( "bakeAOFolder: baked %d of %d models in %d ms\n", baked, total, Sys_Milliseconds() - start );
	R_ResetOcclusionMapCache();
}
