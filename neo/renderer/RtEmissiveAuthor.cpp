/*
===========================================================================

DUDE emissive mode AUTHORING (docs/rtx-light-editor.md, stages L1 + L2).

The old emissive census GUESSED, from geometry alone, which placed lights a glowing surface replaces; a day of fixes
(2026-10-29) showed the mapper's intent is not in the geometry. Emissive mode now reads only what was authored for the
map, in a per-map text file under the save path - emissive/<map>.cfg (e.g. base/emissive/game/mars_city1.cfg):

	light "tim_light_3701" delete				a placed light is gone (emissive mode only)
	light @-2688,-2144,282 delete				a light the .map names nothing for, keyed by its spawn position
	material "textures/base_light/striplight3" emit	every surface of the material emits...
	fixture 3fb7735ee5af2439 off				...except this one
	fixture 16e9a41f488c47e1 emit scale 2 reach 300 follow "light_4839"

A map with no file is unchanged: no surface emits, no light is deleted. Light names come from the map's .map file
(the renderer has no entity names and renderLight_t is frozen by the dhewm3 ABI): a light whose spawn origin matches a
"light" entity there takes its name, any other is keyed by position. Surfaces are the emitter table's fixtures (the
per-fixture split of each glow run; id = hash of the run's centroid) and their materials. Brightness: the emitters.cfg
material library as before, else a standalone power match (a light of the surface's own colour at the fixture's reach,
or r_rtEmissiveReachDefault), times the fixture's scale. "light ... color / brightness / radius" lines are kept but
not applied yet (stage L4).

LOOK MODE (L2): editLights toggles it (bind it to a key). Lights are spheres (white active, red deleted, yellow
overridden), glowing surfaces tinted (green emits, grey can emit, orange off by exception); the one under the crosshair
is cyan with its volume (a light's radius box or frustum, a surface's reach sphere) and a label naming it. The commands
below act on what is highlighted; lightPick keeps a light's volume drawn.

Editing without UI (L1; the Light Editor's panels are L3): console commands on what is under the
crosshair - lightPick / lightDelete / lightRestore (the light nearest the crosshair ray, or a named one), emitOn /
emitOff / emitClear / emitMaterial / emitScale / emitReach / emitFollow (the fixture under the crosshair), emissiveSave,
emissiveReload, emissiveInfo.

RECOVERY COPY (user, 2026-10-31, after edits were lost by quitting before emissiveSave): unsaved edits are written to
emissive/<map>.cfg.recovery when the map changes or the game quits; the next time you enter edit mode on that map (L1:
your first authoring command; L2: look mode) a notice offers emissiveRecover (load them back) or emissiveDiscardRecovery.
emissiveSave removes the copy (it is then out of date).

===========================================================================
*/

#include "sys/platform.h"
#include "idlib/Lexer.h"
#include "idlib/containers/StrList.h"
#include "framework/FileSystem.h"
#include "framework/DeclManager.h"
#include "renderer/tr_local.h"
#include "renderer/RenderWorld_local.h"
#include "renderer/rhi/RHI.h"
#include "sys/sys_imgui.h"

struct emAuthLight_t {
	idStr	key;							// a .map name, or "@x,y,z"
	bool	deleted;
	bool	hasColor;	idVec3 color;		// L4: kept, not applied yet
	float	brightness;						// L4 (0 = unset)
	bool	hasRadius;	idVec3 radius;		// L4
};
struct emAuthFixture_t {
	unsigned long long id;
	int		emit;							// 1 = emits, -1 = off (an exception to a material-wide emit), 0 = unset
	float	scale;							// <= 0: unset (1)
	float	reach;							// <= 0: unset (r_rtEmissiveReachDefault)
	idStr	follow;							// a light key ("" = none): the emitter follows that light's animation
	int		followIndex;	unsigned int followGen;		// resolved lightDef index (-1 = not found), cached per data generation
};
struct emAuthMaterial_t {
	idStr				name;
	const idMaterial *	mat;
	bool				emit;
};
struct emMapLight_t { idStr name; idVec3 origin; };
struct emLightCache_t { unsigned int serial; unsigned int gen; int auth; float animAvg; bool animDone; idStr key; };

static idList<emAuthLight_t>		s_emLights;
static idList<emAuthFixture_t>		s_emFixtures;
static idList<emAuthMaterial_t>		s_emMaterials;
static idList<emMapLight_t>			s_emMapLights;
static idList<emLightCache_t>		s_emLightCache;
static idStr						s_emMap;			// world->mapName the data belongs to ("" = nothing loaded)
static bool							s_emFileFound = false;
static bool							s_emDirty = false;
static bool							s_emRecoveryNoticed = false;	// the recovery notice was shown for this map load
static unsigned int					s_emGen = 1;		// bumps on every change: per-light / per-fixture caches re-resolve

// "maps/game/mars_city1" -> "emissive/game/mars_city1.cfg" (relative to the game dir; the save path wins on read)
static idStr EmAuth_FilePath( const char *mapName ) {
	idStr m = mapName;
	m.BackSlashesToSlashes();
	if ( m.Icmpn( "maps/", 5 ) == 0 ) { m = m.Right( m.Length() - 5 ); }
	m.StripFileExtension();
	return idStr( "emissive/" ) + m + ".cfg";
}

// The map's light entities: name + spawn origin. A light-weight pass over the .map: entity key / value pairs at brace
// depth 1, brushes and patches (depth >= 2) skipped token by token - a 5 MB map scans in well under a second.
// (tools/tests/test_emissive_author.sh compiles this function verbatim against the real idLexer.)
// EMAUTH-TEST-BEGIN
static void EmAuth_ScanMapLightsLexer( idLexer &src, idList<emMapLight_t> &out ) {
	out.SetNum( 0, false );
	idToken tok, val;
	int depth = 0;
	idStr classname, name, origin;
	while ( src.ReadToken( &tok ) ) {
		if ( tok == "{" ) {
			if ( depth == 0 ) { classname.Clear(); name.Clear(); origin.Clear(); }
			depth++;
			continue;
		}
		if ( tok == "}" ) {
			depth--;
			if ( depth == 0 && classname.Icmp( "light" ) == 0 && name.Length() > 0 && origin.Length() > 0 ) {
				emMapLight_t &ml = out.Alloc();
				ml.name = name;
				ml.origin.Zero();
				sscanf( origin.c_str(), "%f %f %f", &ml.origin.x, &ml.origin.y, &ml.origin.z );
			}
			if ( depth < 0 ) { depth = 0; }
			continue;
		}
		if ( depth == 1 && tok.type == TT_STRING ) {			// "key" "value"
			if ( !src.ReadToken( &val ) ) { break; }
			if ( tok.Icmp( "classname" ) == 0 ) { classname = val; }
			else if ( tok.Icmp( "name" ) == 0 ) { name = val; }
			else if ( tok.Icmp( "origin" ) == 0 ) { origin = val; }
		}
	}
}

// One line of the per-map file -> words: whitespace separated, "quoted strings" kept whole, // starts a comment.
// (Not idLexer: it reads a fixture id like 16e9a41f... as the number 16e9, and splits @x,y,z.)
static void EmAuth_Tokenize( const char *line, idStrList &out ) {
	out.Clear();
	const char *p = line;
	while ( *p ) {
		while ( *p == ' ' || *p == '\t' || *p == '\r' ) { p++; }
		if ( *p == '\0' || ( p[0] == '/' && p[1] == '/' ) ) { break; }
		idStr tok;
		if ( *p == '"' ) {
			p++;
			while ( *p && *p != '"' ) { tok += *p++; }
			if ( *p == '"' ) { p++; }
		} else {
			while ( *p && *p != ' ' && *p != '\t' && *p != '\r' ) { tok += *p++; }
		}
		out.Append( tok );
	}
}
// EMAUTH-TEST-END

static void EmAuth_ScanMapLights( const char *mapName ) {
	s_emMapLights.SetNum( 0, false );
	idStr path = mapName;
	path.SetFileExtension( ".map" );
	idLexer src( LEXFL_NOSTRINGCONCAT | LEXFL_NOSTRINGESCAPECHARS | LEXFL_NOFATALERRORS | LEXFL_ALLOWPATHNAMES );
	if ( !src.LoadFile( path ) ) {
		common->Printf( "emissive authoring: %s not found - lights are keyed by position only\n", path.c_str() );
		return;
	}
	EmAuth_ScanMapLightsLexer( src, s_emMapLights );
}

static int EmAuth_FindLight( const char *key ) {
	for ( int i = 0; i < s_emLights.Num(); i++ ) { if ( s_emLights[i].key.Icmp( key ) == 0 ) { return i; } }
	return -1;
}
static int EmAuth_FindFixture( unsigned long long id ) {
	for ( int i = 0; i < s_emFixtures.Num(); i++ ) { if ( s_emFixtures[i].id == id ) { return i; } }
	return -1;
}
static int EmAuth_FindMaterial( const char *name ) {
	for ( int i = 0; i < s_emMaterials.Num(); i++ ) { if ( s_emMaterials[i].name.Icmp( name ) == 0 ) { return i; } }
	return -1;
}

static void EmAuth_Changed( void ) {
	s_emGen++;
	s_emDirty = true;
}

static void EmAuth_NewLight( emAuthLight_t &l, const char *key ) {
	l.key = key; l.deleted = false; l.hasColor = false; l.brightness = 0.0f; l.hasRadius = false;
	l.color.Set( 1.0f, 1.0f, 1.0f ); l.radius.Zero();
}
static void EmAuth_NewFixture( emAuthFixture_t &f, unsigned long long id ) {
	f.id = id; f.emit = 0; f.scale = 0.0f; f.reach = 0.0f; f.follow.Clear(); f.followIndex = -1; f.followGen = 0;
}

static idStr EmAuth_RecoveryPath( const char *mapName ) {
	return EmAuth_FilePath( mapName ) + ".recovery";
}

// Read a per-map file (the map's own, or its recovery copy) into memory, replacing what is there. Unknown words are
// skipped with a warning. false = no such file (memory is then empty).
static bool EmAuth_ReadFileFrom( const idStr &path ) {
	s_emLights.SetNum( 0, false );
	s_emFixtures.SetNum( 0, false );
	s_emMaterials.SetNum( 0, false );
	s_emGen++;
	char *buf = NULL;
	const int len = fileSystem->ReadFile( path, (void **)&buf );
	if ( len < 0 || buf == NULL ) {
		return false;
	}
	idStr text( buf );
	fileSystem->FreeFile( buf );
	idStrList w;
	int lineNo = 0;
	int start = 0;
	while ( start <= text.Length() ) {
		int nl = text.Find( '\n', start );
		if ( nl < 0 ) { nl = text.Length(); }
		const idStr line = text.Mid( start, nl - start );
		start = nl + 1;
		lineNo++;
		EmAuth_Tokenize( line, w );
		if ( w.Num() == 0 ) { continue; }
		if ( w[0].Icmp( "light" ) == 0 && w.Num() >= 2 ) {
			emAuthLight_t &l = s_emLights.Alloc();
			EmAuth_NewLight( l, w[1] );
			for ( int k = 2; k < w.Num(); k++ ) {
				if ( w[k].Icmp( "delete" ) == 0 ) { l.deleted = true; }
				else if ( w[k].Icmp( "color" ) == 0 && k + 3 < w.Num() ) { l.hasColor = true; l.color.Set( atof( w[k+1] ), atof( w[k+2] ), atof( w[k+3] ) ); k += 3; }
				else if ( w[k].Icmp( "brightness" ) == 0 && k + 1 < w.Num() ) { l.brightness = atof( w[k+1] ); k += 1; }
				else if ( w[k].Icmp( "radius" ) == 0 && k + 3 < w.Num() ) { l.hasRadius = true; l.radius.Set( atof( w[k+1] ), atof( w[k+2] ), atof( w[k+3] ) ); k += 3; }
				else { common->Warning( "%s line %d: unknown light word '%s'", path.c_str(), lineNo, w[k].c_str() ); }
			}
		} else if ( w[0].Icmp( "material" ) == 0 && w.Num() >= 2 ) {
			emAuthMaterial_t &em = s_emMaterials.Alloc();
			em.name = w[1];
			em.mat = declManager->FindMaterial( w[1], false );
			em.emit = w.Num() >= 3 && w[2].Icmp( "emit" ) == 0;
		} else if ( w[0].Icmp( "fixture" ) == 0 && w.Num() >= 2 ) {
			emAuthFixture_t &f = s_emFixtures.Alloc();
			EmAuth_NewFixture( f, strtoull( w[1].c_str(), NULL, 16 ) );
			for ( int k = 2; k < w.Num(); k++ ) {
				if ( w[k].Icmp( "emit" ) == 0 ) { f.emit = 1; }
				else if ( w[k].Icmp( "off" ) == 0 ) { f.emit = -1; }
				else if ( w[k].Icmp( "scale" ) == 0 && k + 1 < w.Num() ) { f.scale = atof( w[k+1] ); k += 1; }
				else if ( w[k].Icmp( "reach" ) == 0 && k + 1 < w.Num() ) { f.reach = atof( w[k+1] ); k += 1; }
				else if ( w[k].Icmp( "follow" ) == 0 && k + 1 < w.Num() ) { f.follow = w[k+1]; k += 1; }
				else { common->Warning( "%s line %d: unknown fixture word '%s'", path.c_str(), lineNo, w[k].c_str() ); }
			}
		} else {
			common->Warning( "%s line %d: unknown entry '%s' (skipped)", path.c_str(), lineNo, w[0].c_str() );
		}
	}
	return true;
}

// the map's own file (what emissiveSave wrote)
static void EmAuth_ReadFile( void ) {
	s_emFileFound = EmAuth_ReadFileFrom( EmAuth_FilePath( s_emMap ) );
	s_emDirty = false;
}

static bool EmAuth_FileExists( const idStr &path ) {
	return fileSystem->ReadFile( path, NULL, NULL ) >= 0;
}

static bool EmAuth_WriteFile( const idStr &path, const char *what );

// Unsaved edits about to be lost (the map changes, the game quits): keep them in the recovery copy
static void EmAuth_FlushRecovery( void ) {
	if ( !s_emDirty || s_emMap.Length() == 0 ) { return; }
	const idStr path = EmAuth_RecoveryPath( s_emMap );
	if ( EmAuth_WriteFile( path, "RECOVERY COPY of unsaved edits (emissiveRecover loads it, emissiveDiscardRecovery deletes it)" ) ) {
		common->Printf( "emissive authoring: unsaved edits for %s kept in %s (save path) - emissiveRecover brings them back\n", s_emMap.c_str(), path.c_str() );
	}
	s_emDirty = false;
}

// The data follows the map: a new map scans its .map names and reads its file
static void EmAuth_Sync( const idRenderWorldLocal *world ) {
	if ( world == NULL || world->mapName.Length() == 0 ) { return; }
	if ( s_emMap.Icmp( world->mapName ) == 0 ) { return; }
	EmAuth_FlushRecovery();									// the previous map's unsaved edits
	s_emMap = world->mapName;
	s_emRecoveryNoticed = false;
	s_emLightCache.SetNum( 0, false );
	EmAuth_ScanMapLights( s_emMap );
	EmAuth_ReadFile();
	common->Printf( "emissive authoring: %s - %s (%d lights named in the .map)\n", s_emMap.c_str(),
		s_emFileFound ? va( "%s: %d lights, %d materials, %d fixtures", EmAuth_FilePath( s_emMap ).c_str(), s_emLights.Num(), s_emMaterials.Num(), s_emFixtures.Num() )
		              : va( "no %s: the map is unauthored (emissive mode changes nothing)", EmAuth_FilePath( s_emMap ).c_str() ),
		s_emMapLights.Num() );
}

// A placed light's key: its .map name when its spawn origin matches a light entity (within half a unit), else "@x,y,z"
static idStr EmAuth_LightKey( const idRenderLightLocal *ld ) {
	const idVec3 &o = ld->parms.origin;
	int best = -1; float bestD = 0.5f;
	for ( int i = 0; i < s_emMapLights.Num(); i++ ) {
		const float d = ( s_emMapLights[i].origin - o ).LengthFast();
		if ( d <= bestD ) { bestD = d; best = i; }
	}
	if ( best >= 0 ) { return s_emMapLights[ best ].name; }
	return va( "@%d,%d,%d", idMath::FtoiFast( floorf( o.x + 0.5f ) ), idMath::FtoiFast( floorf( o.y + 0.5f ) ), idMath::FtoiFast( floorf( o.z + 0.5f ) ) );
}

// The per-lightDef cache: its key and authored entry, valid while its slot holds the same light (serial) and the data
// has not changed (generation)
static emLightCache_t *EmAuth_LightCache( const idRenderWorldLocal *world, int index ) {
	if ( world == NULL || index < 0 || index >= world->lightDefs.Num() || world->lightDefs[ index ] == NULL ) { return NULL; }
	const idRenderLightLocal *ld = world->lightDefs[ index ];
	if ( s_emLightCache.Num() < world->lightDefs.Num() ) {
		const int was = s_emLightCache.Num();
		s_emLightCache.SetNum( world->lightDefs.Num(), false );
		for ( int i = was; i < s_emLightCache.Num(); i++ ) { s_emLightCache[i].serial = 0u; s_emLightCache[i].gen = 0u; }
	}
	emLightCache_t &c = s_emLightCache[ index ];
	if ( c.serial != ld->dudeSerial ) {
		c.serial = ld->dudeSerial;
		c.key = EmAuth_LightKey( ld );
		c.gen = 0u;
		c.animDone = false;
	}
	if ( c.gen != s_emGen ) {
		c.gen = s_emGen;
		c.auth = EmAuth_FindLight( c.key );
	}
	return &c;
}

/*
===================
R_EmAuthLightDeleted

Is placed light `index` deleted by the map's authored emissive setup? Emissive mode only.
===================
*/
bool R_EmAuthLightDeleted( const idRenderWorldLocal *world, int index ) {
	if ( !r_rtEmissiveLighting.GetBool() ) { return false; }
	EmAuth_Sync( world );
	const emLightCache_t *c = EmAuth_LightCache( world, index );
	return c != NULL && c->auth >= 0 && s_emLights[ c->auth ].deleted;
}

/*
===================
R_EmAuthLightAnimAverage

The time-average luminance of a light's animated colour (0 = steady), for the "follow" link; cached per light.
===================
*/
float R_EmAuthLightAnimAverage( const idRenderWorldLocal *world, int index ) {
	emLightCache_t *c = EmAuth_LightCache( world, index );
	if ( c == NULL ) { return 0.0f; }
	if ( !c->animDone ) {
		c->animAvg = R_RtEmissiveLightAnimatedAverage( world->lightDefs[ index ] );
		c->animDone = true;
	}
	return c->animAvg;
}

static int EmAuth_LightIndexByKey( const idRenderWorldLocal *world, const char *key ) {
	if ( world == NULL || key == NULL || key[0] == '\0' ) { return -1; }
	for ( int i = 0; i < world->lightDefs.Num(); i++ ) {
		const emLightCache_t *c = EmAuth_LightCache( world, i );
		if ( c != NULL && c->key.Icmp( key ) == 0 ) { return i; }
	}
	return -1;
}

/*
===================
R_EmAuthFixtureEmits

Does this emitter-table fixture emit under the map's authored setup? Its own emit / off line wins, else its material's.
Out: the fixture's scale (1 = unset), reach (0 = unset) and the lightDef index whose animation it follows (-1 = none).
===================
*/
bool R_EmAuthFixtureEmits( const idRenderWorldLocal *world, unsigned long long fixtureId, const idMaterial *mat, float *scale, float *reach, int *followLight ) {
	*scale = 1.0f; *reach = 0.0f; *followLight = -1;
	EmAuth_Sync( world );
	int emit = 0;
	const int fi = EmAuth_FindFixture( fixtureId );
	if ( fi >= 0 ) {
		emAuthFixture_t &f = s_emFixtures[ fi ];
		emit = f.emit;
		if ( f.scale > 0.0f ) { *scale = f.scale; }
		if ( f.reach > 0.0f ) { *reach = f.reach; }
		if ( f.follow.Length() > 0 ) {
			if ( f.followGen != s_emGen ) { f.followIndex = EmAuth_LightIndexByKey( world, f.follow ); f.followGen = s_emGen; }
			*followLight = f.followIndex;
		}
	}
	if ( emit == 0 && mat != NULL ) {
		for ( int i = 0; i < s_emMaterials.Num(); i++ ) {
			if ( s_emMaterials[i].mat == mat || ( s_emMaterials[i].mat == NULL && s_emMaterials[i].name.Icmp( mat->GetName() ) == 0 ) ) {
				emit = s_emMaterials[i].emit ? 1 : 0;
				break;
			}
		}
	}
	return emit > 0;
}

// a data generation for the gather's status line (changes when anything authored changes)
unsigned int R_EmAuthGeneration( void ) {
	return s_emGen;
}

/*
==============================================================================

Look mode (L2, editLights): every placed light as a sphere, every glowing surface tinted by its authored state, the
light or surface under the crosshair highlighted with its volume (a light's radius box / projection frustum, a
surface's reach sphere) and named by a one-line label under the crosshair. The frontend builds it all here, once per
primary view (R_EmAuthLookFrame, from R_RtEmissiveUpdate); the backend overlay draws it (RhiWorld.cpp
RB_RHI_RtEmissiveDebugOverlay) and the console's debug-text pass the label (Console.cpp). Vulkan only. You keep
playing; the light / emit commands act on what is highlighted.

==============================================================================
*/

static bool							s_emLook = false;
static int							s_emHoverLight = -1;		// the lightDef under the crosshair (-1 = none)
static unsigned long long			s_emHoverFixture = 0;		// else the fixture under the crosshair (0 = none)
static int							s_emSelLight = -1;			// lightPick in look mode: keeps that light's volume drawn
static unsigned int					s_emSelSerial = 0u;
static idList<rtEmAuthTint_t>		s_emLookTints;
static idList<idDrawVert>			s_emLookVerts;				// spheres + volume edges (world space; normal.y = x-ray)
static idList<glIndex_t>			s_emLookIndexes;
static char							s_emHoverText[200];
static int							s_emLookFrame = -1;			// tr.frameCount the gizmos were built for (none drawn from a stale frame)

static const byte EM_COL_ACTIVE[4]   = { 235, 235, 235, 255 };
static const byte EM_COL_DELETED[4]  = { 255,  40,  40, 255 };
static const byte EM_COL_OVERRIDE[4] = { 255, 220,  40, 255 };
static const byte EM_COL_HOVER[4]    = {  40, 230, 255, 255 };
static const byte EM_COL_EMITS[4]    = {  40, 220,  60, 150 };
static const byte EM_COL_CANEMIT[4]  = { 150, 150, 150, 130 };
static const byte EM_COL_OFF[4]      = { 255, 140,  20, 150 };

bool R_EmAuthLookMode( void ) {
	return s_emLook;
}

// a fixture's authored state: 1 = emits, -1 = off by its own exception line, 0 = can emit (nothing says it does)
static int EmAuth_FixtureState( const idRenderWorldLocal *world, unsigned long long id, const idMaterial *mat, bool *own ) {
	float sc, re; int fl;
	const int fi = EmAuth_FindFixture( id );
	*own = fi >= 0 && s_emFixtures[ fi ].emit != 0;
	if ( fi >= 0 && s_emFixtures[ fi ].emit < 0 ) { return -1; }
	return R_EmAuthFixtureEmits( world, id, mat, &sc, &re, &fl ) ? 1 : 0;
}

static bool EmAuth_LightOverridden( const emAuthLight_t &l ) {
	return l.hasColor || l.hasRadius || l.brightness > 0.0f;
}

static void EmAuth_LookVert( const idVec3 &p, const byte *c, bool xray ) {
	idDrawVert &v = s_emLookVerts.Alloc();
	v.Clear();
	v.xyz = p;
	v.normal.Set( 1.0f, xray ? 1.0f : 0.0f, 0.0f );
	v.color[0] = c[0]; v.color[1] = c[1]; v.color[2] = c[2]; v.color[3] = c[3];
}

static void EmAuth_LookSphere( const idVec3 &o, float r, const byte *c ) {
	static const float t = 0.8506508f, u = 0.5257311f;
	static const float ico[12][3] = { { -u, t, 0 }, { u, t, 0 }, { -u, -t, 0 }, { u, -t, 0 }, { 0, -u, t }, { 0, u, t }, { 0, -u, -t }, { 0, u, -t }, { t, 0, -u }, { t, 0, u }, { -t, 0, -u }, { -t, 0, u } };
	static const int icoTri[20][3] = { {0,11,5},{0,5,1},{0,1,7},{0,7,10},{0,10,11},{1,5,9},{5,11,4},{11,10,2},{10,7,6},{7,1,8},{3,9,4},{3,4,2},{3,2,6},{3,6,8},{3,8,9},{4,9,5},{2,4,11},{6,2,10},{8,6,7},{9,8,1} };
	const int base = s_emLookVerts.Num();
	for ( int v = 0; v < 12; v++ ) { EmAuth_LookVert( o + idVec3( ico[v][0], ico[v][1], ico[v][2] ) * r, c, true ); }
	for ( int f = 0; f < 20; f++ ) {
		for ( int k = 0; k < 3; k++ ) { s_emLookIndexes.Append( (glIndex_t)( base + icoTri[f][k] ) ); }
	}
}

// a line as a thin quad facing the eye, ~1.5 px wide at any distance
static void EmAuth_LookSegment( const idVec3 &a, const idVec3 &b, const idVec3 &eye, const byte *c ) {
	const idVec3 mid = ( a + b ) * 0.5f;
	idVec3 side = ( b - a ).Cross( mid - eye );
	if ( side.Normalize() < 1e-6f ) { return; }
	side *= Max( 0.2f, 0.0011f * ( mid - eye ).Length() );
	const int base = s_emLookVerts.Num();
	EmAuth_LookVert( a - side, c, true ); EmAuth_LookVert( a + side, c, true );
	EmAuth_LookVert( b + side, c, true ); EmAuth_LookVert( b - side, c, true );
	static const int q[6] = { 0, 1, 2, 0, 2, 3 };
	for ( int k = 0; k < 6; k++ ) { s_emLookIndexes.Append( (glIndex_t)( base + q[k] ) ); }
}

// a light's volume: the edges of its frustum windings (a point light's radius box, a projected light's frustum)
static void EmAuth_LookLightVolume( const idRenderLightLocal *ld, const idVec3 &eye, const byte *c ) {
	for ( int i = 0; i < 6; i++ ) {
		const idWinding *w = ld->frustumWindings[i];
		if ( w == NULL ) { continue; }
		const int n = w->GetNumPoints();
		for ( int k = 0; k < n; k++ ) { EmAuth_LookSegment( (*w)[k].ToVec3(), (*w)[ ( k + 1 ) % n ].ToVec3(), eye, c ); }
	}
}

// a surface's reach: three great circles of radius r
static void EmAuth_LookReach( const idVec3 &o, float r, const idVec3 &eye, const byte *c ) {
	const int SEG = 48;
	const idVec3 ax[3][2] = { { idVec3( 1, 0, 0 ), idVec3( 0, 1, 0 ) }, { idVec3( 1, 0, 0 ), idVec3( 0, 0, 1 ) }, { idVec3( 0, 1, 0 ), idVec3( 0, 0, 1 ) } };
	for ( int a = 0; a < 3; a++ ) {
		for ( int k = 0; k < SEG; k++ ) {
			const float t0 = idMath::TWO_PI * k / SEG, t1 = idMath::TWO_PI * ( k + 1 ) / SEG;
			EmAuth_LookSegment( o + ( ax[a][0] * idMath::Cos( t0 ) + ax[a][1] * idMath::Sin( t0 ) ) * r,
			                    o + ( ax[a][0] * idMath::Cos( t1 ) + ax[a][1] * idMath::Sin( t1 ) ) * r, eye, c );
		}
	}
}

static const char *EmAuth_ShortMaterial( const idMaterial *mat ) {
	if ( mat == NULL ) { return "?"; }
	const char *name = mat->GetName();
	if ( !idStr::Cmpn( name, "textures/", 9 ) ) { return name + 9; }
	if ( !idStr::Cmpn( name, "models/mapobjects/", 18 ) ) { return name + 18; }
	return name;
}

static bool EmAuth_LightShown( const idRenderLightLocal *ld ) {
	return ld != NULL && ld->lightShader != NULL && !ld->parms.parallel;
}

/*
===================
R_EmAuthLookFrame

Frontend, once per primary view while look mode is on: the hover pick (the nearer of a light sphere and the glowing
surface under the crosshair), the light spheres, the surface tints and the highlighted object's volume.
===================
*/
void R_EmAuthLookFrame( const idRenderWorldLocal *world, const viewDef_t *viewDef ) {
	s_emLookTints.SetNum( 0, false );
	s_emLookVerts.SetNum( 0, false );
	s_emLookIndexes.SetNum( 0, false );
	s_emHoverText[0] = '\0';
	s_emHoverLight = -1;
	s_emHoverFixture = 0;
	if ( !s_emLook || world == NULL || viewDef == NULL || world->mapName.Length() == 0 ) { return; }
	s_emLookFrame = tr.frameCount;
	EmAuth_Sync( world );
	const idVec3 eye = tr.primaryRenderView.vieworg;
	const idVec3 dir = tr.primaryRenderView.viewaxis[0];
	float wallDist = 4096.0f;									// a light sphere behind the first wall is not picked
	{
		modelTrace_t mt;
		if ( const_cast<idRenderWorldLocal *>( world )->Trace( mt, eye, eye + dir * 4096.0f, 0.0f, false, true ) ) { wallDist = ( mt.point - eye ).Length(); }
	}
	// the surface under the crosshair
	const idMaterial *fixMat = NULL; idVec3 fixPos; float fixDist = 0.0f;
	const unsigned long long fix = R_RtEmissivePickFixture( &fixMat, &fixPos, &fixDist );
	// the light spheres: lights in the areas this view reached (or in solid: inside a fixture's housing)
	struct sph_t { int light; idVec3 o; float r; };
	static idList<sph_t> sph;
	sph.SetNum( 0, false );
	int hover = -1; float hoverT = 1e30f;
	for ( int i = 0; i < world->lightDefs.Num(); i++ ) {
		const idRenderLightLocal *ld = world->lightDefs[i];
		if ( !EmAuth_LightShown( ld ) ) { continue; }
		const idVec3 o = ld->parms.origin;
		const idVec3 v = o - eye;
		const float dist = v.Length();
		if ( dist < 40.0f || dist > 3000.0f ) { continue; }		// < 40: the player's own lights (flashlight, muzzle flash)
		const int area = world->PointInArea( o );
		if ( area >= 0 && area < world->NumAreas() && world->portalAreas[area].viewCount != tr.viewCount ) { continue; }
		sph_t &s = sph.Alloc();
		s.light = i; s.o = o; s.r = Max( 5.0f, 0.009f * dist );
		const float t = v * dir;
		if ( t <= 0.0f ) { continue; }
		const float d = ( v - dir * t ).Length();
		if ( d <= s.r * 1.5f && t - s.r <= wallDist + 16.0f && t < hoverT ) { hoverT = t; hover = i; }
	}
	if ( hover >= 0 && ( fix == 0 || hoverT - 8.0f <= fixDist ) ) {
		s_emHoverLight = hover;
	} else if ( fix != 0 ) {
		s_emHoverFixture = fix;
	}
	if ( s_emSelLight >= 0 && ( s_emSelLight >= world->lightDefs.Num() || world->lightDefs[ s_emSelLight ] == NULL
			|| world->lightDefs[ s_emSelLight ]->dudeSerial != s_emSelSerial ) ) {
		s_emSelLight = -1;											// the selected light is gone
	}
	for ( int k = 0; k < sph.Num(); k++ ) {
		const emLightCache_t *c = EmAuth_LightCache( world, sph[k].light );
		const emAuthLight_t *a = ( c != NULL && c->auth >= 0 ) ? &s_emLights[ c->auth ] : NULL;
		const byte *col = ( sph[k].light == s_emHoverLight || sph[k].light == s_emSelLight ) ? EM_COL_HOVER
			: ( a != NULL && a->deleted ) ? EM_COL_DELETED : ( a != NULL && EmAuth_LightOverridden( *a ) ) ? EM_COL_OVERRIDE : EM_COL_ACTIVE;
		EmAuth_LookSphere( sph[k].o, sph[k].r, col );
	}
	if ( s_emHoverLight >= 0 ) { EmAuth_LookLightVolume( world->lightDefs[ s_emHoverLight ], eye, EM_COL_HOVER ); }
	if ( s_emSelLight >= 0 && s_emSelLight != s_emHoverLight ) { EmAuth_LookLightVolume( world->lightDefs[ s_emSelLight ], eye, EM_COL_HOVER ); }

	// the surface tints: one entry per run of rows with the same state (rows are in table order)
	const rtEmissiveRow_t *rows = NULL;
	const int numRows = R_RtEmissiveRows( &rows );
	const rtEmissiveRow_t *hoverRow = NULL;
	for ( int ri = 0; ri < numRows; ri++ ) {
		const rtEmissiveRow_t &row = rows[ri];
		bool own;
		const int st = EmAuth_FixtureState( world, row.fixtureId, row.mat, &own );
		const bool hov = s_emHoverFixture != 0 && row.fixtureId == s_emHoverFixture;
		if ( hov ) { hoverRow = &row; }
		const byte *col = hov ? EM_COL_HOVER : ( st > 0 ) ? EM_COL_EMITS : ( st < 0 ) ? EM_COL_OFF : EM_COL_CANEMIT;
		byte rgba[4] = { col[0], col[1], col[2], hov ? (byte)170 : col[3] };
		if ( row.dark ) { for ( int k = 0; k < 3; k++ ) { rgba[k] = rgba[k] / 2; } }	// switched off right now
		if ( s_emLookTints.Num() > 0 ) {
			rtEmAuthTint_t &last = s_emLookTints[ s_emLookTints.Num() - 1 ];
			if ( last.firstTri + last.numTris == row.firstTri && memcmp( last.rgba, rgba, 4 ) == 0 ) { last.numTris += row.numTris; continue; }
		}
		rtEmAuthTint_t &t = s_emLookTints.Alloc();
		t.firstTri = row.firstTri; t.numTris = row.numTris;
		memcpy( t.rgba, rgba, 4 );
	}

	// the label (and the hovered surface's reach)
	if ( s_emHoverLight >= 0 ) {
		const idRenderLightLocal *ld = world->lightDefs[ s_emHoverLight ];
		const emLightCache_t *c = EmAuth_LightCache( world, s_emHoverLight );
		const emAuthLight_t *a = ( c != NULL && c->auth >= 0 ) ? &s_emLights[ c->auth ] : NULL;
		const char *lname = ld->lightShader->GetName();
		if ( !idStr::Cmpn( lname, "lights/", 7 ) ) { lname += 7; }
		const idStr shape = ld->parms.pointLight ? idStr( va( "r %.0f %.0f %.0f", ld->parms.lightRadius.x, ld->parms.lightRadius.y, ld->parms.lightRadius.z ) )
			: idStr( va( "projected, reach %.0f", ld->parms.target.Length() ) );
		idStr::snPrintf( s_emHoverText, sizeof( s_emHoverText ), "light \"%s\"  %s  %s - %s%s",
			c ? c->key.c_str() : "?", lname, shape.c_str(),
			( a != NULL && a->deleted ) ? "DELETED in emissive mode" : ( a != NULL && EmAuth_LightOverridden( *a ) ) ? "overridden" : "active",
			s_emHoverLight == s_emSelLight ? "  [selected]" : "" );
	} else if ( s_emHoverFixture != 0 ) {
		bool own;
		const int st = EmAuth_FixtureState( world, s_emHoverFixture, fixMat, &own );
		const int fi = EmAuth_FindFixture( s_emHoverFixture );
		const emAuthFixture_t *f = ( fi >= 0 ) ? &s_emFixtures[ fi ] : NULL;
		const float reachDef = idMath::ClampFloat( 0.0f, 4096.0f, r_rtEmissiveReachDefault.GetFloat() );
		float reach = ( f != NULL && f->reach > 0.0f ) ? f->reach : reachDef;
		if ( !( reach > 0.0f ) && hoverRow != NULL ) { reach = Max( hoverRow->boundRadius * 4.0f, 64.0f ); }
		idStr state;
		if ( st > 0 ) {
			state = va( "emits%s", own ? "" : " (its material)" );
			if ( f != NULL && f->scale > 0.0f ) { state += va( " x%.2f", f->scale ); }
		} else if ( st < 0 ) {
			state = "off (exception to its material)";
		} else {
			state = "can emit (emitOn / emitMaterial)";
		}
		state += va( "  reach %.0f", reach );
		if ( f != NULL && f->follow.Length() ) { state += va( "  follows \"%s\"", f->follow.c_str() ); }
		idStr::snPrintf( s_emHoverText, sizeof( s_emHoverText ), "surface %s  fixture %016llx - %s", EmAuth_ShortMaterial( fixMat ), s_emHoverFixture, state.c_str() );
		if ( reach > 0.0f ) { EmAuth_LookReach( hoverRow != NULL ? hoverRow->centroid : fixPos, reach, eye, EM_COL_HOVER ); }
	}
}

int R_EmAuthLookTints( const rtEmAuthTint_t **out ) {
	*out = s_emLookTints.Ptr();
	return ( s_emLook && s_emLookFrame == tr.frameCount ) ? s_emLookTints.Num() : 0;
}

bool R_EmAuthLookGeometry( const idDrawVert **verts, int *numVerts, const glIndex_t **indexes, int *numIndexes ) {
	*verts = s_emLookVerts.Ptr(); *numVerts = s_emLookVerts.Num();
	*indexes = s_emLookIndexes.Ptr(); *numIndexes = s_emLookIndexes.Num();
	return s_emLook && s_emLookFrame == tr.frameCount && *numIndexes > 0;
}

const char *R_EmAuthHoverText( void ) {
	return ( s_emLook && s_emLookFrame >= tr.frameCount - 1 && s_emHoverText[0] != '\0' ) ? s_emHoverText : NULL;	// the console draws next frame's 2D
}

/*
==============================================================================

Console commands (L1: edit by command on what is under the crosshair)

==============================================================================
*/

/*
===================
R_EmAuthEnterEditMode

Entering edit mode on this map (L1: the first authoring command after a load; L2: look mode): if a recovery copy of
unsaved edits from an earlier session exists, say so once - on screen and in the console.
===================
*/
void R_EmAuthEnterEditMode( void ) {
	if ( s_emRecoveryNoticed || s_emMap.Length() == 0 ) { return; }
	s_emRecoveryNoticed = true;
	const idStr path = EmAuth_RecoveryPath( s_emMap );
	if ( !EmAuth_FileExists( path ) ) { return; }
	common->Printf( "emissive authoring: UNSAVED EDITS from an earlier session are in %s - emissiveRecover loads them (replacing "
		"what is in memory now), emissiveDiscardRecovery deletes the copy\n", path.c_str() );
	D3::ImGuiHooks::ShowWarningOverlay( "Unsaved emissive edits from your last session were kept.\n"
		"emissiveRecover loads them, emissiveDiscardRecovery deletes them." );
}

static idRenderWorldLocal *EmAuth_CmdWorld( void ) {
	idRenderWorldLocal *world = tr.primaryWorld;
	if ( world == NULL || world->mapName.Length() == 0 ) {
		common->Printf( "no map loaded\n" );
		return NULL;
	}
	EmAuth_Sync( world );
	R_EmAuthEnterEditMode();
	return world;
}

// The placed light nearest the crosshair ray (by angle; within ~3.5 degrees or 24 units), or the one named / keyed
// by the argument. -1 = none.
static int EmAuth_PickLight( idRenderWorldLocal *world, const idCmdArgs &args ) {
	if ( args.Argc() > 1 ) {
		const int i = EmAuth_LightIndexByKey( world, args.Argv( 1 ) );
		if ( i < 0 ) { common->Printf( "no light '%s' in this map\n", args.Argv( 1 ) ); }
		return i;
	}
	if ( s_emLook ) {
		if ( s_emHoverLight >= 0 && s_emHoverLight < world->lightDefs.Num() && world->lightDefs[ s_emHoverLight ] != NULL ) { return s_emHoverLight; }
		if ( s_emSelLight >= 0 ) { return s_emSelLight; }
	}
	const idVec3 eye = tr.primaryRenderView.vieworg;
	const idVec3 dir = tr.primaryRenderView.viewaxis[0];
	int best = -1; float bestScore = 1e30f;
	for ( int i = 0; i < world->lightDefs.Num(); i++ ) {
		const idRenderLightLocal *ld = world->lightDefs[i];
		if ( ld == NULL || ld->lightShader == NULL || ld->parms.parallel ) { continue; }
		const idVec3 v = ld->parms.origin - eye;
		const float t = v * dir;
		if ( t < 8.0f || t > 4096.0f ) { continue; }
		const float d = ( v - dir * t ).Length();
		if ( d > Max( 24.0f, 0.06f * t ) ) { continue; }
		const float score = d / t;
		if ( score < bestScore ) { bestScore = score; best = i; }
	}
	if ( best < 0 ) { common->Printf( "no placed light near the crosshair (aim at where the light sits, or name it)\n" ); }
	return best;
}

static void EmAuth_PrintLight( idRenderWorldLocal *world, int i, const char *verb ) {
	const idRenderLightLocal *ld = world->lightDefs[i];
	const emLightCache_t *c = EmAuth_LightCache( world, i );
	common->Printf( "%s light \"%s\" at %.0f %.0f %.0f  radius %.0f %.0f %.0f  '%s'%s\n", verb, c ? c->key.c_str() : "?",
		ld->parms.origin.x, ld->parms.origin.y, ld->parms.origin.z, ld->parms.lightRadius.x, ld->parms.lightRadius.y, ld->parms.lightRadius.z,
		ld->lightShader ? ld->lightShader->GetName() : "?", ( c && c->auth >= 0 && s_emLights[ c->auth ].deleted ) ? "  [DELETED in emissive mode]" : "" );
}

static void EmAuth_LightPick_f( const idCmdArgs &args ) {
	idRenderWorldLocal *world = EmAuth_CmdWorld();
	if ( world == NULL ) { return; }
	if ( s_emLook && args.Argc() <= 1 && s_emHoverLight < 0 ) {			// look mode, nothing under the crosshair: clear the selection
		if ( s_emSelLight >= 0 ) { common->Printf( "lightPick: selection cleared\n" ); }
		s_emSelLight = -1;
		return;
	}
	const int i = EmAuth_PickLight( world, args );
	if ( i >= 0 ) {
		EmAuth_PrintLight( world, i, "lightPick:" );
		if ( s_emLook ) { s_emSelLight = i; s_emSelSerial = world->lightDefs[i]->dudeSerial; }	// its volume stays drawn
	}
}

static void EmAuth_SetDeleted( const idCmdArgs &args, bool deleted ) {
	idRenderWorldLocal *world = EmAuth_CmdWorld();
	if ( world == NULL ) { return; }
	const int i = EmAuth_PickLight( world, args );
	if ( i < 0 ) { return; }
	const emLightCache_t *c = EmAuth_LightCache( world, i );
	if ( c == NULL ) { return; }
	int a = EmAuth_FindLight( c->key );
	if ( a < 0 ) {
		if ( !deleted ) { EmAuth_PrintLight( world, i, "lightRestore: already active:" ); return; }
		EmAuth_NewLight( s_emLights.Alloc(), c->key );
		a = s_emLights.Num() - 1;
	}
	s_emLights[a].deleted = deleted;
	if ( !deleted && !s_emLights[a].hasColor && !s_emLights[a].hasRadius && s_emLights[a].brightness <= 0.0f ) { s_emLights.RemoveIndex( a ); }
	EmAuth_Changed();
	EmAuth_PrintLight( world, i, deleted ? "lightDelete:" : "lightRestore:" );
}
static void EmAuth_LightDelete_f( const idCmdArgs &args ) { EmAuth_SetDeleted( args, true ); }
static void EmAuth_LightRestore_f( const idCmdArgs &args ) { EmAuth_SetDeleted( args, false ); }

// the fixture under the crosshair (its entry, created on demand)
static emAuthFixture_t *EmAuth_PickFixture( bool create, const idMaterial **matOut, idVec3 *posOut ) {
	if ( EmAuth_CmdWorld() == NULL ) { return NULL; }
	const unsigned long long id = R_RtEmissivePickFixture( matOut, posOut );
	if ( id == 0 ) {
		common->Printf( "no glowing surface under the crosshair (the emitter table needs emissive mode or r_rtEmissiveTable 1)\n" );
		return NULL;
	}
	int fi = EmAuth_FindFixture( id );
	if ( fi < 0 ) {
		if ( !create ) { return NULL; }
		EmAuth_NewFixture( s_emFixtures.Alloc(), id );
		fi = s_emFixtures.Num() - 1;
	}
	return &s_emFixtures[ fi ];
}

static void EmAuth_PrintFixture( const emAuthFixture_t &f, const idMaterial *mat, const idVec3 &pos, const char *verb ) {
	common->Printf( "%s fixture %016llx '%s' at %.0f %.0f %.0f: %s%s%s%s\n", verb, f.id, mat ? mat->GetName() : "?", pos.x, pos.y, pos.z,
		f.emit > 0 ? "emits" : f.emit < 0 ? "off (exception)" : "follows its material",
		f.scale > 0.0f ? va( ", scale %.2f", f.scale ) : "", f.reach > 0.0f ? va( ", reach %.0f", f.reach ) : "",
		f.follow.Length() ? va( ", follows \"%s\"", f.follow.c_str() ) : "" );
}

static void EmAuth_SetFixtureEmit( int emit, const char *verb ) {
	const idMaterial *mat = NULL; idVec3 pos;
	emAuthFixture_t *f = EmAuth_PickFixture( true, &mat, &pos );
	if ( f == NULL ) { return; }
	f->emit = emit;
	EmAuth_Changed();
	EmAuth_PrintFixture( *f, mat, pos, verb );
}
static void EmAuth_EmitOn_f( const idCmdArgs &args ) { EmAuth_SetFixtureEmit( 1, "emitOn:" ); }
static void EmAuth_EmitOff_f( const idCmdArgs &args ) { EmAuth_SetFixtureEmit( -1, "emitOff:" ); }

static void EmAuth_EmitClear_f( const idCmdArgs &args ) {
	if ( EmAuth_CmdWorld() == NULL ) { return; }
	const idMaterial *mat = NULL; idVec3 pos;
	const unsigned long long id = R_RtEmissivePickFixture( &mat, &pos );
	const int fi = ( id != 0 ) ? EmAuth_FindFixture( id ) : -1;
	if ( fi < 0 ) { common->Printf( "emitClear: no authored fixture under the crosshair\n" ); return; }
	s_emFixtures.RemoveIndex( fi );
	EmAuth_Changed();
	common->Printf( "emitClear: fixture %016llx '%s' back to its material's setting\n", id, mat ? mat->GetName() : "?" );
}

static void EmAuth_EmitMaterial_f( const idCmdArgs &args ) {
	if ( EmAuth_CmdWorld() == NULL ) { return; }
	const idMaterial *mat = NULL; idVec3 pos;
	if ( R_RtEmissivePickFixture( &mat, &pos ) == 0 || mat == NULL ) { common->Printf( "emitMaterial: no glowing surface under the crosshair\n" ); return; }
	int mi = EmAuth_FindMaterial( mat->GetName() );
	bool on;
	if ( args.Argc() > 1 ) { on = idStr::Icmp( args.Argv( 1 ), "off" ) != 0 && idStr::Icmp( args.Argv( 1 ), "0" ) != 0; }
	else { on = !( mi >= 0 && s_emMaterials[ mi ].emit ); }		// no argument: toggle
	if ( mi < 0 ) {
		emAuthMaterial_t &em = s_emMaterials.Alloc();
		em.name = mat->GetName(); em.mat = mat; em.emit = false;
		mi = s_emMaterials.Num() - 1;
	}
	s_emMaterials[ mi ].emit = on;
	if ( !on ) { s_emMaterials.RemoveIndex( mi ); }
	EmAuth_Changed();
	common->Printf( "emitMaterial: every '%s' surface in this map %s\n", mat->GetName(), on ? "EMITS" : "is back to off (per-fixture lines still apply)" );
}

static void EmAuth_EmitNumber( const idCmdArgs &args, bool reach ) {
	const idMaterial *mat = NULL; idVec3 pos;
	emAuthFixture_t *f = EmAuth_PickFixture( true, &mat, &pos );
	if ( f == NULL ) { return; }
	const float v = ( args.Argc() > 1 ) ? (float)atof( args.Argv( 1 ) ) : 0.0f;
	if ( reach ) { f->reach = Max( v, 0.0f ); } else { f->scale = Max( v, 0.0f ); }
	EmAuth_Changed();
	EmAuth_PrintFixture( *f, mat, pos, reach ? "emitReach:" : "emitScale:" );
}
static void EmAuth_EmitScale_f( const idCmdArgs &args ) { EmAuth_EmitNumber( args, false ); }
static void EmAuth_EmitReach_f( const idCmdArgs &args ) { EmAuth_EmitNumber( args, true ); }

static void EmAuth_EmitFollow_f( const idCmdArgs &args ) {
	const idMaterial *mat = NULL; idVec3 pos;
	emAuthFixture_t *f = EmAuth_PickFixture( true, &mat, &pos );
	if ( f == NULL ) { return; }
	f->follow = ( args.Argc() > 1 ) ? args.Argv( 1 ) : "";
	f->followGen = 0;
	EmAuth_Changed();
	EmAuth_PrintFixture( *f, mat, pos, "emitFollow:" );
	if ( f->follow.Length() && EmAuth_LightIndexByKey( tr.primaryWorld, f->follow ) < 0 ) {
		common->Printf( "  (no light '%s' in this map yet - lightPick names the one under the crosshair)\n", f->follow.c_str() );
	}
}

static bool EmAuth_WriteFile( const idStr &path, const char *what ) {
	idFile *f = fileSystem->OpenFileWrite( path, "fs_savepath" );
	if ( f == NULL ) { common->Warning( "emissive authoring: could not write %s", path.c_str() ); return false; }
	f->Printf( "// DUDE emissive setup for %s (docs/rtx-light-editor.md). Emissive mode only - a map without this file is unchanged.\n", s_emMap.c_str() );
	if ( what != NULL ) { f->Printf( "// %s\n", what ); }
	f->Printf( "// light <\"name\" | @x,y,z> delete    material \"<name>\" emit    fixture <idhex> emit|off [scale <x>] [reach <units>] [follow \"<light>\"]\n" );
	for ( int i = 0; i < s_emLights.Num(); i++ ) {
		const emAuthLight_t &l = s_emLights[i];
		if ( l.key[0] == '@' ) { f->Printf( "light %s", l.key.c_str() ); } else { f->Printf( "light \"%s\"", l.key.c_str() ); }
		if ( l.deleted ) { f->Printf( " delete" ); }
		if ( l.hasColor ) { f->Printf( " color %.3f %.3f %.3f", l.color.x, l.color.y, l.color.z ); }
		if ( l.brightness > 0.0f ) { f->Printf( " brightness %.3f", l.brightness ); }
		if ( l.hasRadius ) { f->Printf( " radius %.0f %.0f %.0f", l.radius.x, l.radius.y, l.radius.z ); }
		f->Printf( "\n" );
	}
	for ( int i = 0; i < s_emMaterials.Num(); i++ ) {
		if ( s_emMaterials[i].emit ) { f->Printf( "material \"%s\" emit\n", s_emMaterials[i].name.c_str() ); }
	}
	for ( int i = 0; i < s_emFixtures.Num(); i++ ) {
		const emAuthFixture_t &x = s_emFixtures[i];
		if ( x.emit == 0 && x.scale <= 0.0f && x.reach <= 0.0f && x.follow.Length() == 0 ) { continue; }
		f->Printf( "fixture %016llx%s", x.id, x.emit > 0 ? " emit" : x.emit < 0 ? " off" : "" );
		if ( x.scale > 0.0f ) { f->Printf( " scale %.3f", x.scale ); }
		if ( x.reach > 0.0f ) { f->Printf( " reach %.0f", x.reach ); }
		if ( x.follow.Length() ) { f->Printf( " follow \"%s\"", x.follow.c_str() ); }
		f->Printf( "\n" );
	}
	fileSystem->CloseFile( f );
	return true;
}

static void EmAuth_Save_f( const idCmdArgs &args ) {
	if ( EmAuth_CmdWorld() == NULL ) { return; }
	const idStr path = EmAuth_FilePath( s_emMap );
	if ( !EmAuth_WriteFile( path, NULL ) ) { return; }
	s_emDirty = false;
	s_emFileFound = true;
	const idStr rec = EmAuth_RecoveryPath( s_emMap );
	const bool hadRecovery = EmAuth_FileExists( rec );
	if ( hadRecovery ) { fileSystem->RemoveFile( rec ); }
	common->Printf( "emissiveSave: wrote %s (save path): %d lights, %d materials, %d fixtures%s\n", path.c_str(), s_emLights.Num(), s_emMaterials.Num(), s_emFixtures.Num(),
		hadRecovery ? "; the recovery copy is out of date and was removed" : "" );
}

static void EmAuth_Recover_f( const idCmdArgs &args ) {
	if ( EmAuth_CmdWorld() == NULL ) { return; }
	const idStr rec = EmAuth_RecoveryPath( s_emMap );
	if ( !EmAuth_FileExists( rec ) ) { common->Printf( "emissiveRecover: no recovery copy for %s (%s)\n", s_emMap.c_str(), rec.c_str() ); return; }
	EmAuth_ReadFileFrom( rec );
	s_emDirty = true;											// recovered, not saved: emissiveSave keeps them (and removes the copy)
	common->Printf( "emissiveRecover: loaded %s - %d lights, %d materials, %d fixtures (UNSAVED: emissiveSave keeps them)\n", rec.c_str(),
		s_emLights.Num(), s_emMaterials.Num(), s_emFixtures.Num() );
}

static void EmAuth_DiscardRecovery_f( const idCmdArgs &args ) {
	if ( EmAuth_CmdWorld() == NULL ) { return; }
	const idStr rec = EmAuth_RecoveryPath( s_emMap );
	if ( !EmAuth_FileExists( rec ) ) { common->Printf( "emissiveDiscardRecovery: no recovery copy for %s\n", s_emMap.c_str() ); return; }
	fileSystem->RemoveFile( rec );
	common->Printf( "emissiveDiscardRecovery: deleted %s\n", rec.c_str() );
}

// the game quits (idRenderSystemLocal::Shutdown, before the file system goes): unsaved edits go to the recovery copy
void R_EmAuthShutdown( void ) {
	EmAuth_FlushRecovery();
}

static void EmAuth_Reload_f( const idCmdArgs &args ) {
	if ( EmAuth_CmdWorld() == NULL ) { return; }
	EmAuth_ReadFile();
	common->Printf( "emissiveReload: %s %s (%d lights, %d materials, %d fixtures)\n", EmAuth_FilePath( s_emMap ).c_str(),
		s_emFileFound ? "read" : "not found - nothing authored", s_emLights.Num(), s_emMaterials.Num(), s_emFixtures.Num() );
}

static void EmAuth_Info_f( const idCmdArgs &args ) {
	idRenderWorldLocal *world = EmAuth_CmdWorld();
	if ( world == NULL ) { return; }
	common->Printf( "emissive authoring for %s: %s%s%s; emissive mode %s; %d lights named in the .map\n", s_emMap.c_str(), EmAuth_FilePath( s_emMap ).c_str(),
		s_emFileFound ? "" : " (not saved yet)", s_emDirty ? ", UNSAVED edits" : "", r_rtEmissiveLighting.GetBool() ? "ON" : "off", s_emMapLights.Num() );
	for ( int i = 0; i < s_emLights.Num(); i++ ) {
		const int li = EmAuth_LightIndexByKey( world, s_emLights[i].key );
		common->Printf( "  light %s%s%s\n", s_emLights[i].key.c_str(), s_emLights[i].deleted ? " delete" : "", li < 0 ? "  (not in the map now)" : "" );
	}
	for ( int i = 0; i < s_emMaterials.Num(); i++ ) {
		common->Printf( "  material %s %s%s\n", s_emMaterials[i].name.c_str(), s_emMaterials[i].emit ? "emit" : "-", s_emMaterials[i].mat ? "" : "  (not loaded)" );
	}
	for ( int i = 0; i < s_emFixtures.Num(); i++ ) {
		const emAuthFixture_t &x = s_emFixtures[i];
		common->Printf( "  fixture %016llx %s%s%s%s\n", x.id, x.emit > 0 ? "emit" : x.emit < 0 ? "off" : "-",
			x.scale > 0.0f ? va( " scale %.2f", x.scale ) : "", x.reach > 0.0f ? va( " reach %.0f", x.reach ) : "",
			x.follow.Length() ? va( " follow %s", x.follow.c_str() ) : "" );
	}
}

static void EmAuth_EditLights_f( const idCmdArgs &args ) {
	if ( args.Argc() > 1 ) { s_emLook = atoi( args.Argv( 1 ) ) != 0; } else { s_emLook = !s_emLook; }
	if ( !s_emLook ) {
		s_emSelLight = -1;
		common->Printf( "editLights: look mode off\n" );
		return;
	}
	if ( rhi::GetActiveBackendType() != rhi::BT_VULKAN ) {
		s_emLook = false;
		common->Printf( "editLights: Vulkan only (r_backend vulkan)\n" );
		return;
	}
	if ( EmAuth_CmdWorld() == NULL ) { s_emLook = false; return; }
	common->Printf( "editLights: look mode ON - lights: white active, red deleted, yellow overridden, cyan under the crosshair (+ its volume); "
		"surfaces: green emits, grey can emit, orange off by exception, cyan under the crosshair (+ its reach). lightDelete / lightRestore / "
		"emitOn / emitOff / emitMaterial ... act on what is highlighted; lightPick keeps a light's volume drawn. editLights again to leave.\n" );
	if ( !r_rtEmissiveLighting.GetBool() ) {
		common->Printf( "  emissive mode is off (r_rtEmissiveLighting 0): edits apply when it is on; the surface tints need it (or r_rtEmissiveTable 1)\n" );
	}
}

void R_EmAuthRegisterCommands( void ) {
	cmdSystem->AddCommand( "editLights", EmAuth_EditLights_f, CMD_FL_RENDERER, "emissive authoring: toggle the Light Editor's look mode (editLights [0|1]; bind it to a key) - lights and glowing surfaces drawn by their authored state, the one under the crosshair named" );
	cmdSystem->AddCommand( "lightPick", EmAuth_LightPick_f, CMD_FL_RENDERER, "emissive authoring: name the placed light nearest the crosshair (its .map name or @x,y,z)" );
	cmdSystem->AddCommand( "lightDelete", EmAuth_LightDelete_f, CMD_FL_RENDERER, "emissive authoring: delete the placed light nearest the crosshair (or lightDelete <name>) in emissive mode" );
	cmdSystem->AddCommand( "lightRestore", EmAuth_LightRestore_f, CMD_FL_RENDERER, "emissive authoring: restore the placed light nearest the crosshair (or lightRestore <name>)" );
	cmdSystem->AddCommand( "emitOn", EmAuth_EmitOn_f, CMD_FL_RENDERER, "emissive authoring: the glowing surface under the crosshair emits light in emissive mode" );
	cmdSystem->AddCommand( "emitOff", EmAuth_EmitOff_f, CMD_FL_RENDERER, "emissive authoring: the surface under the crosshair does not emit (an exception to emitMaterial)" );
	cmdSystem->AddCommand( "emitClear", EmAuth_EmitClear_f, CMD_FL_RENDERER, "emissive authoring: forget the per-surface setting of the surface under the crosshair (back to its material)" );
	cmdSystem->AddCommand( "emitMaterial", EmAuth_EmitMaterial_f, CMD_FL_RENDERER, "emissive authoring: emitMaterial [on|off] - every surface of the material under the crosshair emits (no argument: toggle)" );
	cmdSystem->AddCommand( "emitScale", EmAuth_EmitScale_f, CMD_FL_RENDERER, "emissive authoring: emitScale <x> - brightness multiplier of the surface under the crosshair (0 = unset)" );
	cmdSystem->AddCommand( "emitReach", EmAuth_EmitReach_f, CMD_FL_RENDERER, "emissive authoring: emitReach <units> - how far the surface under the crosshair lights (0 = r_rtEmissiveReachDefault)" );
	cmdSystem->AddCommand( "emitFollow", EmAuth_EmitFollow_f, CMD_FL_RENDERER, "emissive authoring: emitFollow <light> - the surface under the crosshair flickers / pulses with that placed light (no argument: stop)" );
	cmdSystem->AddCommand( "emissiveSave", EmAuth_Save_f, CMD_FL_RENDERER, "emissive authoring: write this map's emissive/<map>.cfg to the save path" );
	cmdSystem->AddCommand( "emissiveReload", EmAuth_Reload_f, CMD_FL_RENDERER, "emissive authoring: re-read this map's emissive/<map>.cfg (unsaved edits are lost)" );
	cmdSystem->AddCommand( "emissiveInfo", EmAuth_Info_f, CMD_FL_RENDERER, "emissive authoring: print this map's authored lights, materials and fixtures" );
	cmdSystem->AddCommand( "emissiveRecover", EmAuth_Recover_f, CMD_FL_RENDERER, "emissive authoring: load the recovery copy of unsaved edits from an earlier session (then emissiveSave keeps them)" );
	cmdSystem->AddCommand( "emissiveDiscardRecovery", EmAuth_DiscardRecovery_f, CMD_FL_RENDERER, "emissive authoring: delete the recovery copy of unsaved edits" );
}
