/*
===========================================================================

DUDE: the one worker pool behind the parallel level-load passes (the image decode, the sound
decode, the tessellation bake). Header-only so the work can be a lambda.

===========================================================================
*/
#ifndef __PARALLELFOR_H__
#define __PARALLELFOR_H__

#include <thread>
#include <atomic>
#include <vector>

#include "framework/Common.h"		// Com_*ThreadCapture
#include "framework/FileSystem.h"	// FS_SetThreadReadSafe

// how many workers for count items: the hardware's thread count (or `requested` when > 0), clamped
// to 2..32 and to count
inline int Com_ParallelThreadCount( int count, int requested = 0 ) {
	int nThreads = ( requested > 0 ) ? requested : (int)std::thread::hardware_concurrency();
	if ( nThreads < 2 ) { nThreads = 2; }
	if ( nThreads > 32 ) { nThreads = 32; }
	if ( nThreads > count ) { nThreads = count; }
	return nThreads;
}

// the window the workers run in: the heap and filesystem reads are thread-safe only while one is
// alive (the main thread holds it around the Com_ParallelFor calls, then flushes the captures)
class idParallelWindow {
public:
	idParallelWindow() { Mem_SetThreadSafe( true ); FS_SetThreadReadSafe( true ); }
	~idParallelWindow() { Mem_SetThreadSafe( false ); FS_SetThreadReadSafe( false ); }
};

// run work( index ) for index 0..count-1 on worker threads, each capturing its prints
// (Com_BeginThreadCapture; the caller flushes them after, Com_FlushThreadCaptureWarn). Returns the
// thread count.
template< class workFunc_t >
inline int Com_ParallelFor( int count, const workFunc_t &work, int requested = 0 ) {
	const int nThreads = Com_ParallelThreadCount( count, requested );
	std::atomic<int> nextIndex( 0 );
	std::vector<std::thread> pool;
	pool.reserve( nThreads );
	for ( int k = 0; k < nThreads; k++ ) {
		pool.push_back( std::thread( [&]() {
			Com_BeginThreadCapture();
			for ( ;; ) {
				const int idx = nextIndex.fetch_add( 1, std::memory_order_relaxed );
				if ( idx >= count ) {
					break;
				}
				work( idx );
			}
			Com_EndThreadCapture();
		} ) );
	}
	for ( size_t k = 0; k < pool.size(); k++ ) {
		pool[k].join();
	}
	return nThreads;
}

#endif /* !__PARALLELFOR_H__ */
