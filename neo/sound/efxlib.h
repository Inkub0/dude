/*
*/

#ifndef __EFXLIBH
#define __EFXLIBH

#include "idlib/containers/List.h"
#include "idlib/Str.h"
#include "idlib/Lexer.h"
#include "idlib/Heap.h"
#include "framework/Common.h"
#include "sound/sound.h"

#define EFX_VERBOSE 0

#if EFX_VERBOSE
#define EFXprintf(...) do { common->Printf(__VA_ARGS__); } while (false)
#else
#define EFXprintf(...) do { } while (false)
#endif

struct idSoundEffect {
	idSoundEffect();
	~idSoundEffect();

	bool alloc();

	idStr name;
	ALuint effect;
};

class idEFXFile {
public:
	idEFXFile();
	~idEFXFile();

	bool FindEffect( idStr &name, ALuint *effect );
	bool LoadFile( const char *filename, bool OSPath = false );
	void Clear( void );

private:
	bool ReadEffect( idLexer &lexer, idSoundEffect *effect );

	idList<idSoundEffect *>effects;
};

/*
===============
idMapReverb - DUDE: Quake 4's per-map reverb table, maps/<map>.reverb

reverb
{
	{ 0 Plain }
	{ 1 Mountains }
	...
}

Names the EFX preset (from the title's preset library, efxs/default.efx) each portal area
uses. Doom 3 keys its per-map .efx file by area instead.
===============
*/
class idMapReverb {
public:
	bool			LoadFile( const char *filename );
	void			Clear( void ) { presets.Clear(); }
	int				NumAreas( void ) const { return presets.Num(); }
	// preset name for an area, NULL when the table does not name one
	const char *	PresetForArea( int area ) const;

private:
	idStrList		presets;		// indexed by area, "" = unset
};

#endif // __EFXLIBH
