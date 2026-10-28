/*
	Offline test of the AO baker's GPU tracer (neo/tools/compilers/aobake/aobake_gpu.cpp +
	neo/shaders/aobake.comp) against the engine's own CPU tracer.

	test_aobake_gpu.sh copies the CPU tracer out of aobake.cpp verbatim (aoTri_t, the grid, the
	Moller-Trumbore grid walk, the Hammersley hemisphere and AO_SampleVisibility) into
	aobake_cpu_extract.inc; this file builds synthetic models, traces the same texels both ways and
	compares visibility and the buried flag:

	  1. facing  - a quad over a point: occludes when its front (engine-outward) face looks at the
	               point, not at all when it faces away. Proves the BLAS winding + back-face cull
	               reproduce the CPU's front-face-only rule.
	  2. models  - a displaced sphere (creases), a box room with a sub-mesh buried under its floor,
	               and an open crate with clutter, many texels each.

	The occluder triangles go through the same packing as AO_BakeGeoModel. No game, no window.
*/

#include <cmath>
#include <cstdio>
#include <vector>
#include <chrono>
#include <algorithm>
#include <thread>

#include "sys/platform.h"
#include "framework/Common.h"
#include "idlib/math/Math.h"
#include "idlib/math/Vector.h"
#include "idlib/bv/Bounds.h"
#include "idlib/containers/List.h"

#include "common_stub.h"

#include "tools/compilers/aobake/aobake_gpu.h"

#include "aobake_cpu_extract.inc"

// ---- model building -------------------------------------------------------------------------

struct testTri_t {
	idVec3	v[3];
	idVec3	n;			// outward ("vertex") normal used to orient the face like AO_BuildGrid
};

struct testModel_t {
	const char *			name;
	std::vector<testTri_t>	tris;
};

static void AddTri( testModel_t &m, const idVec3 &a, const idVec3 &b, const idVec3 &c, const idVec3 &outward ) {
	testTri_t t;
	t.v[0] = a; t.v[1] = b; t.v[2] = c;
	t.n = outward;
	m.tris.push_back( t );
}

// quad a-b-c-d with the given outward normal (winding deliberately arbitrary: orientation comes
// from the normal, as in the engine)
static void AddQuad( testModel_t &m, const idVec3 &a, const idVec3 &b, const idVec3 &c, const idVec3 &d, const idVec3 &outward ) {
	AddTri( m, a, b, c, outward );
	AddTri( m, a, c, d, outward );
}

// axis-aligned box; inward = true makes a room (normals point inside)
static void AddBox( testModel_t &m, const idVec3 &lo, const idVec3 &hi, bool inward, bool skipTop = false ) {
	const float s = inward ? -1.0f : 1.0f;
	idVec3 p[8];
	for ( int i = 0; i < 8; i++ ) {
		p[i].Set( ( i & 1 ) ? hi.x : lo.x, ( i & 2 ) ? hi.y : lo.y, ( i & 4 ) ? hi.z : lo.z );
	}
	AddQuad( m, p[0], p[1], p[3], p[2], idVec3( 0, 0, -s ) );			// bottom
	if ( !skipTop ) {
		AddQuad( m, p[4], p[5], p[7], p[6], idVec3( 0, 0, s ) );		// top
	}
	AddQuad( m, p[0], p[1], p[5], p[4], idVec3( 0, -s, 0 ) );
	AddQuad( m, p[2], p[3], p[7], p[6], idVec3( 0, s, 0 ) );
	AddQuad( m, p[0], p[2], p[6], p[4], idVec3( -s, 0, 0 ) );
	AddQuad( m, p[1], p[3], p[7], p[5], idVec3( s, 0, 0 ) );
}

static idVec3 SpherePoint( float th, float ph, float r ) {
	float bump = 1.0f + 0.18f * sinf( 5.0f * th ) * sinf( 4.0f * ph );		// deep lobes -> creases
	return idVec3( r * bump * sinf( th ) * cosf( ph ), r * bump * sinf( th ) * sinf( ph ), r * bump * cosf( th ) );
}

static void AddBumpySphere( testModel_t &m, float r, int rings, int segs ) {
	for ( int i = 0; i < rings; i++ ) {
		for ( int j = 0; j < segs; j++ ) {
			float t0 = idMath::PI * i / rings, t1 = idMath::PI * ( i + 1 ) / rings;
			float p0 = idMath::TWO_PI * j / segs, p1 = idMath::TWO_PI * ( j + 1 ) / segs;
			idVec3 a = SpherePoint( t0, p0, r ), b = SpherePoint( t1, p0, r ), c = SpherePoint( t1, p1, r ), d = SpherePoint( t0, p1, r );
			idVec3 n = ( a + b + c + d ) * 0.25f;			// outward-ish (star-shaped surface)
			n.Normalize();
			// keep the face normal (not the radial one): the bumps make them differ a lot
			idVec3 fn = ( c - a ).Cross( b - a );
			if ( fn * n < 0.0f ) fn = -fn;
			fn.Normalize();
			if ( i > 0 ) AddTri( m, a, b, c, fn );
			if ( i < rings - 1 ) AddTri( m, a, c, d, fn );
		}
	}
}

// ---- the engine's packing (AO_BuildGrid orientation + AO_BakeGeoModel winding) ---------------

static void BuildGrid( const testModel_t &m, aoGrid_t &g ) {
	idBounds b;
	b.Clear();
	for ( size_t i = 0; i < m.tris.size(); i++ ) {
		for ( int k = 0; k < 3; k++ ) b.AddPoint( m.tris[i].v[k] );
	}
	g.bounds = b;
	g.bounds.ExpandSelf( 1.0f );
	idVec3 size = g.bounds[1] - g.bounds[0];
	const float targetCells = 24.0f;
	float maxAxis = Max( size[0], Max( size[1], size[2] ) );
	for ( int i = 0; i < 3; i++ ) {
		g.dim[i] = Max( 1, (int)( targetCells * size[i] / maxAxis ) );
		g.cellSize[i] = size[i] / g.dim[i];
	}
	g.cells.SetNum( g.dim[0] * g.dim[1] * g.dim[2] );
	for ( size_t i = 0; i < m.tris.size(); i++ ) {
		aoTri_t at;
		for ( int k = 0; k < 3; k++ ) at.v[k] = m.tris[i].v[k];
		at.edge1 = at.v[1] - at.v[0];
		at.edge2 = at.v[2] - at.v[0];
		at.normal = ( at.v[2] - at.v[0] ).Cross( at.v[1] - at.v[0] );
		at.normal.Normalize();
		if ( m.tris[i].n * at.normal < 0.0f ) at.normal = -at.normal;
		int idx = g.tris.Append( at );
		AO_InsertTri( g, idx );
	}
}

static std::vector<float> PackForGpu( const aoGrid_t &grid ) {
	std::vector<float> xyz( grid.tris.Num() * 9 );
	for ( int i = 0; i < grid.tris.Num(); i++ ) {
		const aoTri_t &t = grid.tris[i];
		const bool keep = ( t.v[1] - t.v[0] ).Cross( t.v[2] - t.v[0] ) * t.normal >= 0.0f;
		const idVec3 *order[3] = { &t.v[0], keep ? &t.v[1] : &t.v[2], keep ? &t.v[2] : &t.v[1] };
		for ( int k = 0; k < 3; k++ ) {
			xyz[i * 9 + k * 3 + 0] = order[k]->x;
			xyz[i * 9 + k * 3 + 1] = order[k]->y;
			xyz[i * 9 + k * 3 + 2] = order[k]->z;
		}
	}
	return xyz;
}

// ---- comparison ------------------------------------------------------------------------------

struct testSample_t {
	idVec3 p, n;
};

static unsigned rng = 12345u;
static float Rand01() { rng = rng * 1664525u + 1013904223u; return ( rng >> 8 ) * ( 1.0f / 16777216.0f ); }

// texels: random barycentric points on random triangles of the model (what the UV raster hands out)
static std::vector<testSample_t> SampleSurface( const testModel_t &m, int count ) {
	std::vector<testSample_t> s;
	for ( int i = 0; i < count; i++ ) {
		const testTri_t &t = m.tris[ (size_t)( Rand01() * m.tris.size() ) % m.tris.size() ];
		float a = Rand01(), b = Rand01();
		if ( a + b > 1.0f ) { a = 1.0f - a; b = 1.0f - b; }
		testSample_t ts;
		ts.p = t.v[0] + ( t.v[1] - t.v[0] ) * a + ( t.v[2] - t.v[0] ) * b;
		ts.n = t.n;
		s.push_back( ts );
	}
	return s;
}

static int failures = 0;

// Trace `samples` both ways; tolerances: the CPU grid walk steps in half cells and can step past a
// thin cell corner, and the hardware intersector is watertight where Moller-Trumbore has an epsilon
// band -- so individual texels may differ slightly, the bulk must not.
static void Compare( const char *name, const testModel_t &m, const std::vector<testSample_t> &samples, int rays,
                     float meanTol, float p99Tol, bool expectBuried ) {
	aoGrid_t grid;
	BuildGrid( m, grid );
	idBounds b = grid.bounds;
	b.ExpandSelf( -1.0f );
	idVec3 ext = b[1] - b[0];
	float longest = Max( ext[0], Max( ext[1], ext[2] ) );
	const float maxDist = longest * 0.25f;
	const float bias = Max( longest * 1e-4f, 0.05f );
	const float contact = Min( 1.0f, longest * 0.02f );

	std::vector<idVec3> hemi( rays );
	std::vector<float> dirs( rays * 3 );
	for ( int i = 0; i < rays; i++ ) {
		hemi[i] = AO_CosineHemisphere( i, rays );
		dirs[i * 3 + 0] = hemi[i].x; dirs[i * 3 + 1] = hemi[i].y; dirs[i * 3 + 2] = hemi[i].z;
	}

	const int n = (int)samples.size();
	std::vector<float> cpuVis( n );
	std::vector<unsigned char> cpuBur( n );
	// the CPU side runs on every hardware thread, like the baker (r_occlusionMapBakeThreads 0)
	auto c0 = std::chrono::steady_clock::now();
	const int nThreads = Max( 1, (int)std::thread::hardware_concurrency() );
	std::vector<std::thread> pool;
	for ( int t = 0; t < nThreads; t++ ) {
		pool.emplace_back( [&, t]() {
			aoTraceCtx_t ctx;
			for ( int i = t; i < n; i += nThreads ) {
				bool buried = false;
				cpuVis[i] = AO_SampleVisibility( grid, ctx, samples[i].p, samples[i].n, hemi.data(), rays, maxDist, bias, contact, &buried );
				cpuBur[i] = buried ? 1 : 0;
			}
		} );
	}
	for ( size_t t = 0; t < pool.size(); t++ ) {
		pool[t].join();
	}
	auto c1 = std::chrono::steady_clock::now();

	std::vector<aoGpuSample_t> gs( n );
	for ( int i = 0; i < n; i++ ) {
		gs[i].pos[0] = samples[i].p.x; gs[i].pos[1] = samples[i].p.y; gs[i].pos[2] = samples[i].p.z; gs[i].pos[3] = 0.0f;
		gs[i].nrm[0] = samples[i].n.x; gs[i].nrm[1] = samples[i].n.y; gs[i].nrm[2] = samples[i].n.z; gs[i].nrm[3] = 0.0f;
	}
	std::vector<float> xyz = PackForGpu( grid );
	std::vector<float> gpuVis( n );
	std::vector<unsigned char> gpuBur( n );
	auto g0 = std::chrono::steady_clock::now();
	bool ok = AOGpu_SetOccluders( xyz.data(), grid.tris.Num() ) && AOGpu_SetHemisphere( dirs.data(), rays )
	       && AOGpu_Trace( gs.data(), n, maxDist, bias, contact, gpuVis.data(), gpuBur.data() );
	auto g1 = std::chrono::steady_clock::now();
	if ( !ok ) {
		printf( "FAIL %-10s GPU trace failed\n", name );
		failures++;
		return;
	}

	std::vector<float> diff( n );
	double sum = 0.0, cpuMean = 0.0;
	int burMismatch = 0, cpuBuried = 0;
	for ( int i = 0; i < n; i++ ) {
		diff[i] = fabsf( cpuVis[i] - gpuVis[i] );
		sum += diff[i];
		cpuMean += cpuVis[i];
		burMismatch += ( cpuBur[i] != gpuBur[i] );
		cpuBuried += cpuBur[i];
	}
	std::vector<float> sorted = diff;
	std::sort( sorted.begin(), sorted.end() );
	const float mean = (float)( sum / n );
	const float p99 = sorted[ (size_t)( 0.99 * ( n - 1 ) ) ];
	const float worst = sorted.back();
	const bool pass = mean <= meanTol && p99 <= p99Tol && burMismatch <= n / 200 && ( !expectBuried || cpuBuried > 0 );
	printf( "%s %-10s %5d tris %6d texels x %d rays | mean vis %.3f | |cpu-gpu| mean %.4f p99 %.4f max %.4f | buried %d, mismatch %d | cpu %.0f ms (%d threads), gpu %.1f ms\n",
	        pass ? "ok  " : "FAIL", name, grid.tris.Num(), n, rays, cpuMean / n, mean, p99, worst, cpuBuried, burMismatch,
	        std::chrono::duration<double, std::milli>( c1 - c0 ).count(), nThreads, std::chrono::duration<double, std::milli>( g1 - g0 ).count() );
	if ( !pass ) {
		failures++;
	}
}

// Facing: one quad 2 units above a texel at the origin (normal +Z). Facing DOWN (front face toward
// the texel) it occludes; facing UP the CPU skips it as a back face and the GPU must too.
static void FacingTest() {
	const int rays = 64;
	for ( int facing = 0; facing < 2; facing++ ) {
		for ( int winding = 0; winding < 2; winding++ ) {
			testModel_t m;
			m.name = "facing";
			const idVec3 out( 0, 0, facing ? 1.0f : -1.0f );
			idVec3 a( -50, -50, 2 ), b( 50, -50, 2 ), c( 50, 50, 2 ), d( -50, 50, 2 );
			if ( winding ) AddQuad( m, a, d, c, b, out );		// the opposite input winding must not matter
			else           AddQuad( m, a, b, c, d, out );
			// a far floor so the model's bounds give a long enough maxDist
			AddQuad( m, idVec3( -50, -50, -60 ), idVec3( 50, -50, -60 ), idVec3( 50, 50, -60 ), idVec3( -50, 50, -60 ), idVec3( 0, 0, 1 ) );
			std::vector<testSample_t> s( 1 );
			s[0].p.Set( 0, 0, 0 );
			s[0].n.Set( 0, 0, 1 );
			char label[32];
			idStr::snPrintf( label, sizeof( label ), "facing-%s%d", facing ? "up" : "dn", winding );
			Compare( label, m, s, rays, 0.02f, 0.02f, false );
		}
	}
}

int main() {
	idMath::Init();
	if ( !AOGpu_Acquire() ) {
		printf( "FAIL no GPU tracer\n" );
		AOGpu_Release();
		return 1;
	}

	FacingTest();

	{
		testModel_t m;
		m.name = "sphere";
		AddBumpySphere( m, 40.0f, 48, 96 );
		Compare( "sphere", m, SampleSurface( m, 20000 ), 128, 0.01f, 0.06f, false );
	}
	{
		// a room with a box on the floor and a plate lying 0.3 units over part of the floor (like a
		// skull a hair under a zombie's skin): the floor texels under it are buried
		testModel_t m;
		m.name = "room";
		AddBox( m, idVec3( -64, -64, 0 ), idVec3( 64, 64, 96 ), true );
		AddBox( m, idVec3( -16, -16, 0 ), idVec3( 16, 16, 24 ), false );
		AddQuad( m, idVec3( 20, 20, 0.3f ), idVec3( 60, 20, 0.3f ), idVec3( 60, 60, 0.3f ), idVec3( 20, 60, 0.3f ), idVec3( 0, 0, -1 ) );
		std::vector<testSample_t> s = SampleSurface( m, 20000 );
		// plus texels on the floor over the buried plate
		for ( int i = 0; i < 2000; i++ ) {
			testSample_t t;
			t.p.Set( 22 + 36 * Rand01(), 22 + 36 * Rand01(), 0.0f );
			t.n.Set( 0, 0, 1 );
			s.push_back( t );
		}
		Compare( "room", m, s, 128, 0.01f, 0.06f, true );
	}
	{
		testModel_t m;
		m.name = "crate";
		AddBox( m, idVec3( -30, -20, 0 ), idVec3( 30, 20, 30 ), false, true );	// open-top outer shell
		AddBox( m, idVec3( -28, -18, 2 ), idVec3( 28, 18, 30 ), true, true );		// inner walls
		for ( int i = 0; i < 12; i++ ) {
			idVec3 lo( -26 + 4.2f * i, -14 + 2.0f * ( i % 3 ), 2 );
			AddBox( m, lo, lo + idVec3( 3, 6 + ( i % 4 ), 4 + 2 * ( i % 5 ) ), false );
		}
		Compare( "crate", m, SampleSurface( m, 20000 ), 128, 0.01f, 0.06f, false );
	}
	{
		// throughput at the size of a 1024^2 weapon map (both sides on all their hardware)
		testModel_t m;
		m.name = "big";
		AddBumpySphere( m, 40.0f, 96, 192 );
		Compare( "big", m, SampleSurface( m, 1 << 20 ), 128, 0.01f, 0.06f, false );
	}

	AOGpu_Release();
	printf( failures ? "%d FAILED\n" : "all passed\n", failures );
	return failures ? 1 : 0;
}
