/*
===========================================================================

Doom 3 GPL Source Code
Copyright (C) 1999-2011 id Software LLC, a ZeniMax Media company.

This file is part of the Doom 3 GPL Source Code ("Doom 3 Source Code").

Doom 3 Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

#include "sys/platform.h"
#include "framework/FileSystem.h"
#include "framework/Common.h"
#include "framework/Session.h"

#include "sound/snd_local.h"

#include <thread>
#include <atomic>
#include <vector>
#include <mutex>
#include <cstdlib>

#define USE_SOUND_CACHE_ALLOCATOR

#ifdef USE_SOUND_CACHE_ALLOCATOR
static idDynamicBlockAlloc<byte, 1<<20, 1<<10>	soundCacheAllocator;
#else
static idDynamicAlloc<byte, 1<<20, 1<<10>		soundCacheAllocator;
#endif

// DUDE: guards the shared block allocators (soundCacheAllocator above and the sample-decoder
// pools behind idSampleDecoder::Alloc/Free) while LoadPendingParallel() runs worker threads.
// Only the cheap alloc/free bookkeeping is serialized -- the WAV read and stb_vorbis OGG decode,
// which dominate the time, run lock-free (stb_vorbis uses libc malloc). Uncontended off the
// parallel path, so the serial Load()/Reload() callers pay effectively nothing.
static std::mutex								soundCacheAllocMutex;

/*
===================
idSoundCache::idSoundCache()
===================
*/
idSoundCache::idSoundCache() {
	soundCacheAllocator.Init();
	soundCacheAllocator.SetLockMemory( true );
	listCache.AssureSize( 1024, NULL );
	listCache.SetGranularity( 256 );
	insideLevelLoad = false;
}

/*
===================
idSoundCache::~idSoundCache()
===================
*/
idSoundCache::~idSoundCache() {
	listCache.DeleteContents( true );
	soundCacheAllocator.Shutdown();
}

/*
===================
idSoundCache::::GetObject

returns a single cached object pointer
===================
*/
const idSoundSample* idSoundCache::GetObject( const int index ) const {
	if (index<0 || index>listCache.Num()) {
		return NULL;
	}
	return listCache[index];
}

/*
===================
idSoundCache::FindSound

Adds a sound object to the cache and returns a handle for it.
===================
*/
idSoundSample *idSoundCache::FindSound( const idStr& filename, bool loadOnDemandOnly ) {
	idStr fname;

	fname = filename;
	fname.BackSlashesToSlashes();
	fname.ToLower();

	declManager->MediaPrint( "%s\n", fname.c_str() );

	// check to see if object is already in cache
	for( int i = 0; i < listCache.Num(); i++ ) {
		idSoundSample *def = listCache[i];
		if ( def && def->name == fname ) {
			def->levelLoadReferenced = true;
			if ( def->purged && !loadOnDemandOnly ) {
				// DUDE: during a level load, queue the decode for LoadPendingParallel() instead of
				// decoding inline here (pendingDecode dedups repeat references); otherwise load now.
				if ( insideLevelLoad ) {
					if ( !def->pendingDecode ) {
						def->pendingDecode = true;
						samplesToLoad.Append( def );
					}
				} else {
					def->Load();
				}
			}
			return def;
		}
	}

	// create a new entry
	idSoundSample *def = new idSoundSample;

	int shandle = listCache.FindNull();
	if ( shandle != -1 ) {
		listCache[shandle] = def;
	} else {
		shandle = listCache.Append( def );
	}

	def->name = fname;
	def->levelLoadReferenced = true;
	def->onDemand = loadOnDemandOnly;
	def->purged = true;

	if ( !loadOnDemandOnly ) {
		// DUDE: during a level load, queue for the parallel decode pass; otherwise load inline.
		// this may make it a default sound if it can't be loaded
		if ( insideLevelLoad ) {
			def->pendingDecode = true;
			samplesToLoad.Append( def );
		} else {
			def->Load();
		}
	}

	return def;
}

/*
===================
idSoundCache::ReloadSounds

Completely nukes the current cache
===================
*/
void idSoundCache::ReloadSounds( bool force ) {
	int i;

	for( i = 0; i < listCache.Num(); i++ ) {
		idSoundSample *def = listCache[i];
		if ( def ) {
			def->Reload( force );
		}
	}
}

/*
====================
BeginLevelLoad

Mark all file based images as currently unused,
but don't free anything.  Calls to ImageFromFile() will
either mark the image as used, or create a new image without
loading the actual data.
====================
*/
void idSoundCache::BeginLevelLoad() {
	insideLevelLoad = true;

	// DUDE: drop any queue left behind by a load that aborted (common->Error longjmp) before
	// LoadPendingParallel() drained it -- those sample pointers may now be stale. Normal loads
	// leave this empty. Also reset the per-sample queued flag so re-referenced samples re-queue.
	for ( int i = 0 ; i < samplesToLoad.Num() ; i++ ) {
		if ( samplesToLoad[i] ) {
			samplesToLoad[i]->pendingDecode = false;
		}
	}
	samplesToLoad.SetNum( 0, false );

	for ( int i = 0 ; i < listCache.Num() ; i++ ) {
		idSoundSample *sample = listCache[ i ];
		if ( !sample ) {
			continue;
		}

		if ( com_purgeAll.GetBool() ) {
			sample->PurgeSoundSample();
		}

		sample->levelLoadReferenced = false;
	}

	soundCacheAllocator.FreeEmptyBaseBlocks();
}

/*
====================
LoadPendingParallel

DUDE: decode the samples FindSound() queued during the level load across worker threads, then
upload their OpenAL buffers serially on this (main) thread. Mirrors r_parallelImageLoad: the
CPU-heavy WAV read / OGG (stb_vorbis) decode happens in parallel under a thread-safe heap /
filesystem / print window, while the non-thread-safe OpenAL submits stay on the main thread.

This replaces the old inline def->Load() inside FindSound(), which decoded every sound serially
while parsing sound decls -- the fat "sound=" line in the load-phase report.
====================
*/
void idSoundCache::LoadPendingParallel() {
	if ( samplesToLoad.Num() == 0 ) {
		return;
	}

	idSoundSample ** const	list = samplesToLoad.Ptr();
	const int				count = samplesToLoad.Num();

	int nThreads = (int)std::thread::hardware_concurrency();
	if ( nThreads < 2 ) { nThreads = 2; }
	if ( nThreads > 32 ) { nThreads = 32; }
	if ( nThreads > count ) { nThreads = count; }

	const double			t0 = Sys_MillisecondsPrecise();
	std::atomic<int>	nextIndex( 0 );

	// heap / filesystem / print become thread-safe only for this window (same helpers the
	// parallel image loader uses). Snd_SetParallelDecode lets OpenOGG skip its CRITICAL_SECTION_ONE,
	// now redundant under these windows and otherwise a per-open serialization point.
	Mem_SetThreadSafe( true );
	FS_SetThreadReadSafe( true );
	Snd_SetParallelDecode( true );

	// phase 1: worker threads open + decode each sample to PCM (no OpenAL)
	std::vector<std::thread> pool;
	pool.reserve( nThreads );
	for ( int k = 0 ; k < nThreads ; k++ ) {
		pool.push_back( std::thread( [&]() {
			Com_BeginThreadCapture();
			for ( ;; ) {
				const int idx = nextIndex.fetch_add( 1, std::memory_order_relaxed );
				if ( idx >= count ) {
					break;
				}
				list[idx]->DecodeCpu();
			}
			Com_EndThreadCapture();
		} ) );
	}
	for ( size_t k = 0 ; k < pool.size() ; k++ ) {
		pool[k].join();
	}

	Snd_SetParallelDecode( false );
	Mem_SetThreadSafe( false );
	FS_SetThreadReadSafe( false );

	// surface any prints/warnings the workers buffered (order-independent)
	const bool hadErr = Com_ThreadCaptureHadError();
	Com_FlushThreadCapture();
	if ( hadErr ) {
		common->Warning( "parallel sound load: a worker hit an error during decode (see above); affected sounds use the default beep" );
	}

	// phase 2: create + upload the OpenAL buffers on the main thread (OpenAL is not thread-safe)
	for ( int i = 0 ; i < count ; i++ ) {
		list[i]->UploadAL();
		list[i]->pendingDecode = false;
		if ( ( i & 31 ) == 0 ) {
			session->PacifierUpdate();
		}
	}

	samplesToLoad.SetNum( 0, false );

	// the decl-parse timing no longer sees this work (the decode moved out of sound-decl parsing
	// into here), so report it on its own line, like the image loader's "all images loaded in".
	common->Printf( "  parallel sound decode: %d samples on %d threads in %.0f msec\n",
					count, nThreads, Sys_MillisecondsPrecise() - t0 );
}

/*
====================
EndLevelLoad

Free all samples marked as unused
====================
*/
void idSoundCache::EndLevelLoad() {
	int	useCount, purgeCount;
	common->Printf( "----- idSoundCache::EndLevelLoad -----\n" );

	insideLevelLoad = false;

	// DUDE: decode everything FindSound() queued this level across worker threads before the purge
	LoadPendingParallel();

	// purge the ones we don't need
	useCount = 0;
	purgeCount = 0;
	for ( int i = 0 ; i < listCache.Num() ; i++ ) {
		idSoundSample	*sample = listCache[ i ];
		if ( !sample ) {
			continue;
		}
		if ( sample->purged ) {
			continue;
		}
		if ( !sample->levelLoadReferenced ) {
//			common->Printf( "Purging %s\n", sample->name.c_str() );
			purgeCount += sample->objectMemSize;
			sample->PurgeSoundSample();
		} else {
			useCount += sample->objectMemSize;
		}
	}

	soundCacheAllocator.FreeEmptyBaseBlocks();

	common->Printf( "%5ik referenced\n", useCount / 1024 );
	common->Printf( "%5ik purged\n", purgeCount / 1024 );
}

/*
===================
idSoundCache::PrintMemInfo
===================
*/
void idSoundCache::PrintMemInfo( MemInfo_t *mi ) {
	int i, j, num = 0, total = 0;
	int *sortIndex;
	idFile *f;

	f = fileSystem->OpenFileWrite( mi->filebase + "_sounds.txt" );
	if ( !f ) {
		return;
	}

	// count
	for ( i = 0; i < listCache.Num(); i++, num++ ) {
		if ( !listCache[i] ) {
			break;
		}
	}

	// sort first
	sortIndex = new int[num];

	for ( i = 0; i < num; i++ ) {
		sortIndex[i] = i;
	}

	for ( i = 0; i < num - 1; i++ ) {
		for ( j = i + 1; j < num; j++ ) {
			if ( listCache[sortIndex[i]]->objectMemSize < listCache[sortIndex[j]]->objectMemSize ) {
				int temp = sortIndex[i];
				sortIndex[i] = sortIndex[j];
				sortIndex[j] = temp;
			}
		}
	}

	// print next
	for ( i = 0; i < num; i++ ) {
		idSoundSample *sample = listCache[sortIndex[i]];

		// this is strange
		if ( !sample ) {
			continue;
		}

		total += sample->objectMemSize;
		f->Printf( "%s %s\n", idStr::FormatNumber( sample->objectMemSize ).c_str(), sample->name.c_str() );
	}

	mi->soundAssetsTotal = total;

	f->Printf( "\nTotal sound bytes allocated: %s\n", idStr::FormatNumber( total ).c_str() );
	fileSystem->CloseFile( f );
	delete[] sortIndex;
}


/*
==========================================================================

idSoundSample

==========================================================================
*/

/*
===================
idSoundSample::idSoundSample
===================
*/
idSoundSample::idSoundSample() {
	memset( &objectInfo, 0, sizeof(waveformatex_t) );
	objectSize = 0;
	objectMemSize = 0;
	nonCacheData = NULL;
	amplitudeData = NULL;
	openalBuffer = 0;
	hardwareBuffer = false;
	defaultSound = false;
	onDemand = false;
	purged = false;
	levelLoadReferenced = false;
	pendingDecode = false;
	decodeFailed = false;
	nonCacheMalloced = false;
	alUploadData = NULL;
	alUploadBytes = 0;
	alUploadFreeTemp = false;
}

/*
===================
idSoundSample::~idSoundSample
===================
*/
idSoundSample::~idSoundSample() {
	PurgeSoundSample();
}

/*
===================
idSoundSample::LengthIn44kHzSamples
===================
*/
int idSoundSample::LengthIn44kHzSamples( void ) const {
	// objectSize is samples
	if ( objectInfo.nSamplesPerSec == 11025 ) {
		return objectSize << 2;
	} else if ( objectInfo.nSamplesPerSec == 22050 ) {
		return objectSize << 1;
	} else {
		return objectSize << 0;
	}
}

/*
===================
idSoundSample::MakeDefault
===================
*/
void idSoundSample::MakeDefault( void ) {
	int		i;
	float	v;
	int		sample;

	memset( &objectInfo, 0, sizeof( objectInfo ) );

	objectInfo.nChannels = 1;
	objectInfo.wBitsPerSample = 16;
	objectInfo.nSamplesPerSec = 44100;

	objectSize = MIXBUFFER_SAMPLES * 2;
	objectMemSize = objectSize * sizeof( short );

	nonCacheData = (byte *)soundCacheAllocator.Alloc( objectMemSize );
	nonCacheMalloced = false;		// DUDE: allocator-backed, unlike DecodeCpu's malloc path

	short *ncd = (short *)nonCacheData;

	for ( i = 0; i < MIXBUFFER_SAMPLES; i ++ ) {
		v = sin( idMath::PI * 2 * i / 64 );
		sample = v * 0x4000;
		ncd[i*2+0] = sample;
		ncd[i*2+1] = sample;
	}

	alGetError();
	alGenBuffers( 1, &openalBuffer );
	if ( alGetError() != AL_NO_ERROR ) {
		common->Error( "idSoundCache: error generating OpenAL hardware buffer" );
	}

	alGetError();
	alBufferData( openalBuffer, objectInfo.nChannels==1?AL_FORMAT_MONO16:AL_FORMAT_STEREO16, nonCacheData, objectMemSize, objectInfo.nSamplesPerSec );
	if ( alGetError() != AL_NO_ERROR ) {
		common->Warning( "idSoundCache: error loading data into OpenAL hardware buffer" );
		hardwareBuffer = false;
	} else {
		hardwareBuffer = true;
	}

	defaultSound = true;
}

/*
===================
idSoundSample::CheckForDownSample
===================
*/
void idSoundSample::CheckForDownSample( void ) {
	if ( !idSoundSystemLocal::s_force22kHz.GetBool() ) {
		return;
	}
	if ( objectInfo.wFormatTag != WAVE_FORMAT_TAG_PCM || objectInfo.nSamplesPerSec != 44100 ) {
		return;
	}
	int shortSamples = objectSize >> 1;
	// DUDE: lock-free like DecodeCpu's nonCacheData -- runs on worker threads under LoadPendingParallel().
	// converted replaces nonCacheData, so it is malloc'd too and the sample stays nonCacheMalloced.
	short *converted = (short *)malloc( shortSamples * sizeof( short ) );

	if ( objectInfo.nChannels == 1 ) {
		for ( int i = 0; i < shortSamples; i++ ) {
			converted[i] = ((short *)nonCacheData)[i*2];
		}
	} else {
		for ( int i = 0; i < shortSamples; i += 2 ) {
			converted[i+0] = ((short *)nonCacheData)[i*2+0];
			converted[i+1] = ((short *)nonCacheData)[i*2+1];
		}
	}
	if ( nonCacheMalloced ) {
		free( nonCacheData );
	} else {
		soundCacheAllocMutex.lock();
		soundCacheAllocator.Free( nonCacheData );
		soundCacheAllocMutex.unlock();
	}
	nonCacheData = (byte *)converted;
	nonCacheMalloced = true;
	objectSize >>= 1;
	objectMemSize >>= 1;
	objectInfo.nAvgBytesPerSec >>= 1;
	objectInfo.nSamplesPerSec >>= 1;
}

/*
===================
idSoundSample::GetNewTimeStamp
===================
*/
ID_TIME_T idSoundSample::GetNewTimeStamp( void ) const {
	ID_TIME_T timestamp;

	fileSystem->ReadFile( name, NULL, &timestamp );
	if ( timestamp == FILE_NOT_FOUND_TIMESTAMP ) {
		idStr oggName = name;
		oggName.SetFileExtension( ".ogg" );
		fileSystem->ReadFile( oggName, NULL, &timestamp );
	}
	return timestamp;
}

/*
===================
idSoundSample::Load

Loads based on name, possibly doing a MakeDefault if necessary
===================
*/
void idSoundSample::Load( void ) {
	// serial path (on-demand / reload): decode then upload on the same (main) thread.
	DecodeCpu();
	UploadAL();
}

/*
===================
idSoundSample::DecodeCpu

DUDE: the worker-thread-safe half of Load() -- opens the file and decodes the WAV/OGG into PCM,
touching no OpenAL state. Runs under LoadPendingParallel()'s thread-safe heap/filesystem/print
window; the shared block allocators are guarded by soundCacheAllocMutex. On any failure it sets
decodeFailed so the main-thread UploadAL() can MakeDefault() (which must create an OpenAL buffer).
===================
*/
void idSoundSample::DecodeCpu( void ) {
	defaultSound = false;
	hardwareBuffer = false;
	decodeFailed = false;
	alUploadData = NULL;
	alUploadBytes = 0;
	alUploadFreeTemp = false;

	// DUDE: no separate GetNewTimeStamp() probe -- it cost two serialized file opens per sound
	// (the original .wav name misses the whole search path, then the .ogg hits), on top of the
	// two inside Open() itself. Open() resolves and opens the real file once; take its timestamp
	// and let an Open() failure stand in for the old FILE_NOT_FOUND check.
	idWaveFile	fh;
	waveformatex_t info;

	if ( fh.Open( name, &info ) == -1 ) {
		common->Warning( "Couldn't load sound '%s' using default", name.c_str() );
		decodeFailed = true;
		return;
	}

	timestamp = fh.GetTimestamp();

	if ( info.nChannels != 1 && info.nChannels != 2 ) {
		common->Warning( "idSoundSample: %s has %i channels, using default", name.c_str(), info.nChannels );
		fh.Close();
		decodeFailed = true;
		return;
	}

	if ( info.wBitsPerSample != 16 ) {
		common->Warning( "idSoundSample: %s is %dbits, expected 16bits using default", name.c_str(), info.wBitsPerSample );
		fh.Close();
		decodeFailed = true;
		return;
	}

	if ( info.nSamplesPerSec != 44100 && info.nSamplesPerSec != 22050 && info.nSamplesPerSec != 11025 ) {
		common->Warning( "idSoundCache: %s is %dHz, expected 11025, 22050 or 44100 Hz. Using default", name.c_str(), info.nSamplesPerSec );
		fh.Close();
		decodeFailed = true;
		return;
	}

	objectInfo = info;
	objectSize = fh.GetOutputSize();
	objectMemSize = fh.GetMemorySize();

	// DUDE: nonCacheData persists as the sample's data, but allocate it lock-free here. The shared
	// soundCacheAllocator funneled every worker through one mutex, which serialized the whole decode;
	// libc malloc has per-thread arenas. PurgeSoundSample/CheckForDownSample free it per nonCacheMalloced.
	nonCacheData = (byte *)malloc( objectMemSize );
	nonCacheMalloced = true;

	fh.Read( nonCacheData, objectMemSize, NULL );

	// optionally convert it to 22kHz to save memory
	CheckForDownSample();

	// PCM uploads its samples directly
	if ( objectInfo.wFormatTag == WAVE_FORMAT_TAG_PCM ) {
		alUploadData = nonCacheData;
		alUploadBytes = objectMemSize;
		alUploadFreeTemp = false;
	} else if ( objectInfo.wFormatTag == WAVE_FORMAT_TAG_OGG ) {
		// OGG decompressed at load time (when smaller than s_decompressionLimit seconds, 6 seconds by
		// default); larger ones stay compressed in nonCacheData and are streamed at playback.
		if ( objectSize < ( (int) objectInfo.nSamplesPerSec * idSoundSystemLocal::s_decompressionLimit.GetInteger() ) ) {
			// DUDE: destData is transient (freed right after the OpenAL upload) and allocated with
			// libc malloc, not the shared soundCacheAllocator -- lock-free per worker.
			float *destData = (float *)malloc( ( LengthIn44kHzSamples() + 1 ) * sizeof( float ) );

			// Decode the whole sample to 44 kHz via a local stb_vorbis handle (lock-free: no shared
			// decoder pool, no CRITICAL_SECTION_ONE), unlike the streaming idSampleDecoder path.
			idSampleDecoder::DecodeFullOGG( this, destData );

			// Downsample back to original frequency (save memory)
			if ( objectInfo.nSamplesPerSec == 11025 ) {
				for ( int i = 0; i < objectSize; i++ ) {
					if ( destData[i*4] < -32768.0f )
						((short *)destData)[i] = -32768;
					else if ( destData[i*4] > 32767.0f )
						((short *)destData)[i] = 32767;
					else
						((short *)destData)[i] = idMath::FtoiFast( destData[i*4] );
				}
			} else if ( objectInfo.nSamplesPerSec == 22050 ) {
				for ( int i = 0; i < objectSize; i++ ) {
					if ( destData[i*2] < -32768.0f )
						((short *)destData)[i] = -32768;
					else if ( destData[i*2] > 32767.0f )
						((short *)destData)[i] = 32767;
					else
						((short *)destData)[i] = idMath::FtoiFast( destData[i*2] );
				}
			} else {
				for ( int i = 0; i < objectSize; i++ ) {
					if ( destData[i] < -32768.0f )
						((short *)destData)[i] = -32768;
					else if ( destData[i] > 32767.0f )
						((short *)destData)[i] = 32767;
					else
						((short *)destData)[i] = idMath::FtoiFast( destData[i] );
				}
			}

			// hand the decoded buffer to UploadAL(), which frees it after the OpenAL upload
			alUploadData = (byte *)destData;
			alUploadBytes = objectSize * sizeof( short );
			alUploadFreeTemp = true;
		}
	}

	fh.Close();
}

/*
===================
idSoundSample::UploadAL

DUDE: the main-thread half of Load() -- creates the OpenAL buffer and uploads the PCM that
DecodeCpu() produced (or MakeDefault()s if the decode failed). OpenAL calls are not thread-safe,
so this never runs on a worker. Safe to call right after DecodeCpu() on the same thread too (the
serial Load() path does exactly that).
===================
*/
void idSoundSample::UploadAL( void ) {
	if ( decodeFailed ) {
		MakeDefault();
		purged = false;
		return;
	}

	if ( alUploadData != NULL ) {
		alGetError();
		alGenBuffers( 1, &openalBuffer );
		if ( alGetError() != AL_NO_ERROR )
			common->Error( "idSoundCache: error generating OpenAL hardware buffer" );
		if ( alIsBuffer( openalBuffer ) ) {
			alGetError();
			alBufferData( openalBuffer, objectInfo.nChannels==1?AL_FORMAT_MONO16:AL_FORMAT_STEREO16, alUploadData, alUploadBytes, objectInfo.nSamplesPerSec );
			if ( alGetError() != AL_NO_ERROR ) {
				common->Warning( "idSoundCache: error loading data into OpenAL hardware buffer" );
				hardwareBuffer = false;
			} else {
				hardwareBuffer = true;
			}
		}

		if ( alUploadFreeTemp ) {
			// DUDE: destData was malloc'd in DecodeCpu (not the soundCacheAllocator), free it the same way
			free( alUploadData );
		}
	}

	alUploadData = NULL;
	alUploadBytes = 0;
	alUploadFreeTemp = false;
	purged = false;
}

/*
===================
idSoundSample::PurgeSoundSample
===================
*/
void idSoundSample::PurgeSoundSample() {
	purged = true;

	alGetError();
	alDeleteBuffers( 1, &openalBuffer );
	if ( alGetError() != AL_NO_ERROR ) {
		common->Warning( "idSoundCache: error unloading data from OpenAL hardware buffer" );
	}

	openalBuffer = 0;
	hardwareBuffer = false;

	if ( amplitudeData ) {
		soundCacheAllocator.Free( amplitudeData );
		amplitudeData = NULL;
	}

	if ( nonCacheData ) {
		// DUDE: match the allocator DecodeCpu()/MakeDefault() used (see nonCacheMalloced)
		if ( nonCacheMalloced ) {
			free( nonCacheData );
		} else {
			soundCacheAllocator.Free( nonCacheData );
		}
		nonCacheData = NULL;
		nonCacheMalloced = false;
	}
}

/*
===================
idSoundSample::Reload
===================
*/
void idSoundSample::Reload( bool force ) {
	if ( !force ) {
		ID_TIME_T newTimestamp;

		// check the timestamp
		newTimestamp = GetNewTimeStamp();

		if ( newTimestamp == FILE_NOT_FOUND_TIMESTAMP ) {
			if ( !defaultSound ) {
				common->Warning( "Couldn't load sound '%s' using default", name.c_str() );
				MakeDefault();
			}
			return;
		}
		if ( newTimestamp == timestamp ) {
			return;	// don't need to reload it
		}
	}

	common->Printf( "reloading %s\n", name.c_str() );
	PurgeSoundSample();
	Load();
}

/*
===================
idSoundSample::FetchFromCache

Returns true on success.
===================
*/
bool idSoundSample::FetchFromCache( int offset, const byte **output, int *position, int *size, const bool allowIO ) {
	offset &= 0xfffffffe;

	if ( objectSize == 0 || offset < 0 || offset > objectSize * (int)sizeof( short ) || !nonCacheData ) {
		return false;
	}

	if ( output ) {
		*output = nonCacheData + offset;
	}
	if ( position ) {
		*position = 0;
	}
	if ( size ) {
		*size = objectSize * sizeof( short ) - offset;
		if ( *size > SCACHE_SIZE ) {
			*size = SCACHE_SIZE;
		}
	}
	return true;
}
