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
	material "textures/base_light/sterlightdecal" emit radiance 20 profile lambert		physical brightness, a panel
	fixture 8b53a3ab858dba38 tint 1 0.8 0.6 twosided 1		its own appearance over the material's

A map with no file is unchanged: no surface emits, no light is deleted. Light names come from the map's .map file
(the renderer has no entity names and renderLight_t is frozen by the dhewm3 ABI): a light whose spawn origin matches a
"light" entity there takes its name, any other is keyed by position. Surfaces are the emitter table's fixtures (the
per-fixture split of each glow run; id = hash of the run's centroid) and their materials. Brightness: the authored
RADIANCE (physical: a multiplier on the glow's mean radiance), else a standalone power match (a light of the surface's
own colour at the fixture's reach, or r_rtEmissiveReachDefault), times the fixture's scale. The APPEARANCE - radiance,
tint, emission profile, two-sided - is a fixture's own value, else its material's, else the default, and rides on the
emitter row: every edit is live. (It was emitters.cfg, a global file baked into the table; folded in here and removed
2026-10-31.) "light ... color / brightness / radius" lines are kept but not applied yet (stage L4).

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
#ifndef IMGUI_DISABLE
#include "../libs/imgui/imgui_internal.h"	// ImGui::TeleportMousePos (the menu puts the cursor on itself)
#endif

struct emAuthLight_t {
	idStr	key;							// a .map name, or "@x,y,z"
	bool	deleted;
	bool	hasColor;	idVec3 color;		// L4: kept, not applied yet
	float	brightness;						// L4 (0 = unset)
	bool	hasRadius;	idVec3 radius;		// L4
};
// a surface's APPEARANCE (2026-10-31: folded in from emitters.cfg, applied live per emitter row - no table rebuild). Every
// field is optional: a fixture's own value, else its material's, else the default.
struct emAuthLook_t {
	float	radiance;						// > 0: PHYSICAL - this multiplier on the glow's mean radiance; < 0: power-matched (overrides a
											// physical material); 0 = unset (the default: power-matched to the reach)
	bool	hasTint;	idVec3 tint;
	int		profile;						// -1 unset; 0 sqrt(cos) (a bare tube), 1 Lambert (a diffuser), 2 uniform (a globe)
	int		twoSided;						// -1 unset; 0 / 1 (a material with back faces in its model emits both ways anyway)
};
struct emAuthFixture_t {
	unsigned long long id;
	int		emit;							// 1 = emits, -1 = off (an exception to a material-wide emit), 0 = unset
	float	scale;							// <= 0: unset (1)
	float	reach;							// <= 0: unset (r_rtEmissiveReachDefault)
	idStr	follow;							// a light key ("" = none): the emitter follows that light's animation
	int		followIndex;	unsigned int followGen;		// resolved lightDef index (-1 = not found), cached per data generation
	emAuthLook_t look;
};
struct emAuthMaterial_t {
	idStr				name;
	const idMaterial *	mat;
	bool				emit;
	emAuthLook_t		look;				// the default appearance of every surface of the material in this map
};
struct emMapLight_t { idStr name; idVec3 origin; };
struct emAuthLight_t;
static bool EmAuth_LightOverridden( const emAuthLight_t &l );
static void EmAuth_DefaultPhysical( bool material, unsigned long long fixture, const idMaterial *mat );
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
// SOLO (user, 2026-10-31: "we could try isolating a single light"): only this placed light or this emitting surface lights
// the scene - every other light leaves the scene-light buffer and the forward pass (RhiWorld.cpp RB_RHI_GatherSceneLights).
// Session only, never saved; cleared on leaving look mode and on a map change.
static int							s_emSoloLight = -1;			// lightDef index (-1 = none)
static unsigned int					s_emSoloSerial = 0u;
static unsigned long long			s_emSoloFixture = 0;		// else an emitter fixture (0 = none)

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
// One appearance word of a material / fixture line at w[k] (and its values): true = it was one (k advanced past it)
static bool EmAuth_ParseLookWord( const idStrList &w, int &k, emAuthLook_t &look ) {
	if ( w[k].Icmp( "radiance" ) == 0 && k + 1 < w.Num() ) {
		look.radiance = ( w[k+1].Icmp( "matched" ) == 0 ) ? -1.0f : Max( (float)atof( w[k+1] ), 0.0f );
		k += 1;
		return true;
	}
	if ( w[k].Icmp( "tint" ) == 0 && k + 3 < w.Num() ) {
		look.hasTint = true;
		look.tint.Set( Max( (float)atof( w[k+1] ), 0.0f ), Max( (float)atof( w[k+2] ), 0.0f ), Max( (float)atof( w[k+3] ), 0.0f ) );
		k += 3;
		return true;
	}
	if ( w[k].Icmp( "profile" ) == 0 && k + 1 < w.Num() ) {
		const char *v = w[k+1];
		look.profile = !idStr::Icmp( v, "tube" ) ? 0 : !idStr::Icmp( v, "lambert" ) ? 1 : !idStr::Icmp( v, "uniform" ) ? 2 : idMath::ClampInt( 0, 2, atoi( v ) );
		k += 1;
		return true;
	}
	if ( w[k].Icmp( "twosided" ) == 0 && k + 1 < w.Num() ) {
		look.twoSided = atoi( w[k+1] ) != 0 ? 1 : 0;
		k += 1;
		return true;
	}
	return false;
}

static void EmAuth_ClearLook( emAuthLook_t &look ) {
	look.radiance = 0.0f; look.hasTint = false; look.tint.Set( 1.0f, 1.0f, 1.0f ); look.profile = -1; look.twoSided = -1;
}

static bool EmAuth_LookSet( const emAuthLook_t &look ) {
	return look.radiance != 0.0f || look.hasTint || look.profile >= 0 || look.twoSided >= 0;
}

// the appearance words of a line (what EmAuth_ParseLookWord reads back)
static idStr EmAuth_LookWords( const emAuthLook_t &look ) {
	static const char *profileName[3] = { "tube", "lambert", "uniform" };
	idStr out;
	if ( look.radiance > 0.0f ) { out += va( " radiance %.3f", look.radiance ); }
	else if ( look.radiance < 0.0f ) { out += " radiance matched"; }
	if ( look.hasTint ) { out += va( " tint %.3f %.3f %.3f", look.tint.x, look.tint.y, look.tint.z ); }
	if ( look.profile >= 0 ) { out += va( " profile %s", profileName[ idMath::ClampInt( 0, 2, look.profile ) ] ); }
	if ( look.twoSided >= 0 ) { out += va( " twosided %d", look.twoSided ); }
	return out;
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
	EmAuth_ClearLook( f.look );
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
			em.emit = false;
			EmAuth_ClearLook( em.look );
			for ( int k = 2; k < w.Num(); k++ ) {
				if ( w[k].Icmp( "emit" ) == 0 ) { em.emit = true; }
				else if ( !EmAuth_ParseLookWord( w, k, em.look ) ) { common->Warning( "%s line %d: unknown material word '%s'", path.c_str(), lineNo, w[k].c_str() ); }
			}
		} else if ( w[0].Icmp( "fixture" ) == 0 && w.Num() >= 2 ) {
			emAuthFixture_t &f = s_emFixtures.Alloc();
			EmAuth_NewFixture( f, strtoull( w[1].c_str(), NULL, 16 ) );
			for ( int k = 2; k < w.Num(); k++ ) {
				if ( w[k].Icmp( "emit" ) == 0 ) { f.emit = 1; }
				else if ( w[k].Icmp( "off" ) == 0 ) { f.emit = -1; }
				else if ( w[k].Icmp( "scale" ) == 0 && k + 1 < w.Num() ) { f.scale = atof( w[k+1] ); k += 1; }
				else if ( w[k].Icmp( "reach" ) == 0 && k + 1 < w.Num() ) { f.reach = atof( w[k+1] ); k += 1; }
				else if ( w[k].Icmp( "follow" ) == 0 && k + 1 < w.Num() ) { f.follow = w[k+1]; k += 1; }
				else if ( EmAuth_ParseLookWord( w, k, f.look ) ) {}
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
	s_emSoloLight = -1; s_emSoloSerial = 0u; s_emSoloFixture = 0;	// a solo never outlives its map
	EmAuth_ScanMapLights( s_emMap );
	EmAuth_ReadFile();
	common->Printf( "emissive authoring: %s - %s (%d lights named in the .map)\n", s_emMap.c_str(),
		s_emFileFound ? va( "%s: %d lights, %d materials, %d fixtures", EmAuth_FilePath( s_emMap ).c_str(), s_emLights.Num(), s_emMaterials.Num(), s_emFixtures.Num() )
		              : va( "no %s: the map is unauthored (emissive mode changes nothing)", EmAuth_FilePath( s_emMap ).c_str() ),
		s_emMapLights.Num() );
}

// A placed light's key: its .map name when its spawn origin matches a light entity (within half a unit), else "@x,y,z"
static idStr EmAuth_KeyForOrigin( const idVec3 &o ) {
	int best = -1; float bestD = 0.5f;
	for ( int i = 0; i < s_emMapLights.Num(); i++ ) {
		const float d = ( s_emMapLights[i].origin - o ).LengthFast();
		if ( d <= bestD ) { bestD = d; best = i; }
	}
	if ( best >= 0 ) { return s_emMapLights[ best ].name; }
	return va( "@%d,%d,%d", idMath::FtoiFast( floorf( o.x + 0.5f ) ), idMath::FtoiFast( floorf( o.y + 0.5f ) ), idMath::FtoiFast( floorf( o.z + 0.5f ) ) );
}
// keyed by the GAME's origin (dudePristine): an authored override never changes a light's position, so its key holds
static idStr EmAuth_LightKey( const idRenderLightLocal *ld ) {
	return EmAuth_KeyForOrigin( ld->dudePristine.origin );
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

/*
===================
R_EmAuthOverrideLight

UpdateLightDef, for every light update: the map's authored colour / brightness / radius for this light (emissive mode
only) applied to a copy of the game's parms. false = no override (out untouched). Colour replaces shaderParms 0-2,
brightness multiplies them, radius replaces a point light's lightRadius.
===================
*/
static int EmAuth_NumLightOverrides( void ) {
	int n = 0;
	for ( int i = 0; i < s_emLights.Num(); i++ ) { n += ( s_emLights[i].hasColor || s_emLights[i].hasRadius || s_emLights[i].brightness > 0.0f ) ? 1 : 0; }
	return n;
}

bool R_EmAuthOverrideLight( const idRenderWorldLocal *world, int index, const renderLight_t &in, renderLight_t *out ) {
	if ( !r_rtEmissiveLighting.GetBool() || world == NULL || world->mapName.Length() == 0 ) { return false; }
	EmAuth_Sync( world );
	if ( EmAuth_NumLightOverrides() == 0 ) { return false; }
	const int a = EmAuth_FindLight( EmAuth_KeyForOrigin( in.origin ) );
	if ( a < 0 ) { return false; }
	const emAuthLight_t &l = s_emLights[a];
	if ( !l.hasColor && !l.hasRadius && !( l.brightness > 0.0f ) ) { return false; }
	*out = in;
	if ( l.hasColor ) { out->shaderParms[ SHADERPARM_RED ] = l.color.x; out->shaderParms[ SHADERPARM_GREEN ] = l.color.y; out->shaderParms[ SHADERPARM_BLUE ] = l.color.z; }
	if ( l.brightness > 0.0f ) { for ( int k = 0; k < 3; k++ ) { out->shaderParms[ SHADERPARM_RED + k ] *= l.brightness; } }
	if ( l.hasRadius && in.pointLight && !in.parallel ) {
		out->lightRadius.Set( Max( l.radius.x, 1.0f ), Max( l.radius.y, 1.0f ), Max( l.radius.z, 1.0f ) );
	}
	return true;
}

/*
===================
R_EmAuthRefreshLightOverrides

RenderScene, before the views: when the authored data or emissive mode changed, every light that is overridden - or
should be - re-derives from the game's own parms (UpdateLightDef applies the override, or none).
===================
*/
void R_EmAuthRefreshLightOverrides( idRenderWorldLocal *world ) {
	static unsigned int lastGen = 0;
	static bool lastMode = false;
	static const idRenderWorldLocal *lastWorld = NULL;
	if ( world == NULL || world->mapName.Length() == 0 ) { return; }			// a GUI's world: nothing authored
	const bool mode = r_rtEmissiveLighting.GetBool();
	if ( world == lastWorld && mode == lastMode && s_emGen == lastGen ) { return; }
	lastWorld = world; lastMode = mode; lastGen = s_emGen;
	EmAuth_Sync( world );
	lastGen = s_emGen;															// (a map change read its file just now)
	const bool any = mode && EmAuth_NumLightOverrides() > 0;
	for ( int i = 0; i < world->lightDefs.Num(); i++ ) {
		idRenderLightLocal *ld = world->lightDefs[i];
		if ( ld == NULL ) { continue; }
		bool want = false;
		if ( any ) {
			const emLightCache_t *c = EmAuth_LightCache( world, i );
			want = c != NULL && c->auth >= 0 && EmAuth_LightOverridden( s_emLights[ c->auth ] );
		}
		if ( want || ld->dudeOverridden ) {
			const renderLight_t p = ld->dudePristine;
			world->UpdateLightDef( i, &p );
		}
	}
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
static const emAuthMaterial_t *EmAuth_MaterialEntry( const idMaterial *mat ) {
	if ( mat == NULL ) { return NULL; }
	for ( int i = 0; i < s_emMaterials.Num(); i++ ) {
		if ( s_emMaterials[i].mat == mat || ( s_emMaterials[i].mat == NULL && s_emMaterials[i].name.Icmp( mat->GetName() ) == 0 ) ) { return &s_emMaterials[i]; }
	}
	return NULL;
}

// a fixture's EFFECTIVE appearance: its own fields, else its material's, else the defaults
static void EmAuth_ResolveLook( const emAuthLook_t *own, const emAuthLook_t *mat, rtEmAuthEmit_t *out ) {
	float rad = ( own != NULL && own->radiance != 0.0f ) ? own->radiance : ( mat != NULL ) ? mat->radiance : 0.0f;
	out->radiance = Max( rad, 0.0f );
	const emAuthLook_t *t = ( own != NULL && own->hasTint ) ? own : ( mat != NULL && mat->hasTint ) ? mat : NULL;
	out->tint[0] = t ? t->tint.x : 1.0f; out->tint[1] = t ? t->tint.y : 1.0f; out->tint[2] = t ? t->tint.z : 1.0f;
	out->profile = ( own != NULL && own->profile >= 0 ) ? own->profile : ( mat != NULL && mat->profile >= 0 ) ? mat->profile : 0;
	out->twoSided = ( ( own != NULL && own->twoSided >= 0 ) ? own->twoSided : ( mat != NULL && mat->twoSided >= 0 ) ? mat->twoSided : 0 ) != 0;
}

/*
===================
R_EmAuthFixtureEmits

Does this emitter-table fixture emit under the map's authored setup? Its own emit / off line wins, else its material's.
Out (always filled, also for a fixture that does not emit): the fixture's brightness scale (1 = unset), reach (0 = unset),
the lightDef index it follows (-1 = none) and its effective appearance - radiance (0 = power-matched), tint, emission
profile, two-sided.
===================
*/
bool R_EmAuthFixtureEmits( const idRenderWorldLocal *world, unsigned long long fixtureId, const idMaterial *mat, rtEmAuthEmit_t *out ) {
	out->scale = 1.0f; out->reach = 0.0f; out->followLight = -1;
	EmAuth_Sync( world );
	int emit = 0;
	const emAuthLook_t *ownLook = NULL;
	const int fi = EmAuth_FindFixture( fixtureId );
	if ( fi >= 0 ) {
		emAuthFixture_t &f = s_emFixtures[ fi ];
		emit = f.emit;
		ownLook = &f.look;
		if ( f.scale > 0.0f ) { out->scale = f.scale; }
		if ( f.reach > 0.0f ) { out->reach = f.reach; }
		if ( f.follow.Length() > 0 ) {
			if ( f.followGen != s_emGen ) { f.followIndex = EmAuth_LightIndexByKey( world, f.follow ); f.followGen = s_emGen; }
			out->followLight = f.followIndex;
		}
	}
	const emAuthMaterial_t *me = EmAuth_MaterialEntry( mat );
	if ( emit == 0 && me != NULL ) { emit = me->emit ? 1 : 0; }
	EmAuth_ResolveLook( ownLook, me != NULL ? &me->look : NULL, out );
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
// the right-click menu (RtEmissiveAuthor.cpp R_EmAuthRightClick / Com_DrawLightEditorMenu): its target, frozen at the click
enum { EM_MENU_NONE, EM_MENU_LIGHT, EM_MENU_SURFACE, EM_MENU_GENERAL };
struct emMenu_t {
	int					kind;
	int					light;		unsigned int serial;		// EM_MENU_LIGHT
	unsigned long long	fixture;	const idMaterial *mat;	idVec3 pos;	// EM_MENU_SURFACE
	int					openFrames;								// frames since it opened (the cursor is placed on the first ones)
	int					prevSel;	unsigned int prevSelSerial;	// the selection before the menu took it
	int					apScope;								// the surface menu's Appearance: 0 = the material, 1 = this surface
};
static emMenu_t						s_emMenu = { EM_MENU_NONE };
// a two-step "follow" link armed from the menu: the next right-click picks the other end
enum { EM_ARM_NONE, EM_ARM_PICK_LIGHT, EM_ARM_PICK_SURFACE, EM_ARM_MATCH_LIGHT };
static int							s_emArm = EM_ARM_NONE;
static unsigned long long			s_emArmFixture = 0;			// EM_ARM_PICK_LIGHT: the surface that will follow
static idStr						s_emArmLightKey;			// EM_ARM_PICK_SURFACE: the light it will follow
static bool							s_emSwallowRightUp = false;
static void EmAuth_CloseMenu( void );
bool R_EmAuthMouseButton( int button, bool down );

static const byte EM_COL_ACTIVE[4]   = { 235, 235, 235, 255 };
static const byte EM_COL_DELETED[4]  = { 255,  40,  40, 255 };
static const byte EM_COL_OVERRIDE[4] = { 255, 220,  40, 255 };
static const byte EM_COL_HOVER[4]    = {  40, 230, 255, 255 };
static const byte EM_COL_SOLO[4]     = { 255,  60, 255, 255 };
static const byte EM_COL_SCREEN[4]   = { 110, 160, 255, 255 };	// a DUDE fill light: a glowing screen (r_emissiveSurfaces) or an item glow
static const byte EM_COL_EMITS[4]    = {  40, 220,  60, 150 };
static const byte EM_COL_CANEMIT[4]  = { 150, 150, 150, 130 };
static const byte EM_COL_OFF[4]      = { 255, 140,  20, 150 };

bool R_EmAuthLookMode( void ) {
	return s_emLook;
}

// a fixture's authored state: 1 = emits, -1 = off by its own exception line, 0 = can emit (nothing says it does)
static int EmAuth_FixtureState( const idRenderWorldLocal *world, unsigned long long id, const idMaterial *mat, bool *own ) {
	rtEmAuthEmit_t ap;
	const int fi = EmAuth_FindFixture( id );
	*own = fi >= 0 && s_emFixtures[ fi ].emit != 0;
	if ( fi >= 0 && s_emFixtures[ fi ].emit < 0 ) { return -1; }
	return R_EmAuthFixtureEmits( world, id, mat, &ap ) ? 1 : 0;
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
R_EmAuthSolo

The gather, once per refill: is a solo on? Out: the soloed lightDef (-1 = a surface is soloed) and fixture (0 = a light is).
A soloed light that went away ends the solo.
===================
*/
bool R_EmAuthSolo( const idRenderWorldLocal *world, int *light, unsigned long long *fixture ) {
	*light = -1; *fixture = 0;
	if ( s_emSoloLight >= 0 ) {
		if ( world == NULL || s_emSoloLight >= world->lightDefs.Num() || world->lightDefs[ s_emSoloLight ] == NULL
				|| world->lightDefs[ s_emSoloLight ]->dudeSerial != s_emSoloSerial ) {
			s_emSoloLight = -1;
			return false;
		}
		*light = s_emSoloLight;
		return true;
	}
	if ( s_emSoloFixture != 0 ) { *fixture = s_emSoloFixture; return true; }
	return false;
}

static void EmAuth_ClearSolo( void ) {
	s_emSoloLight = -1; s_emSoloSerial = 0u; s_emSoloFixture = 0;
}

// solo the light (or surface) under the crosshair; again on the soloed one ends it
static void EmAuth_ToggleSoloHover( const idRenderWorldLocal *world ) {
	if ( s_emHoverLight >= 0 && world != NULL && s_emHoverLight < world->lightDefs.Num() && world->lightDefs[ s_emHoverLight ] != NULL ) {
		const bool same = s_emSoloLight == s_emHoverLight;
		EmAuth_ClearSolo();
		if ( !same ) { s_emSoloLight = s_emHoverLight; s_emSoloSerial = world->lightDefs[ s_emHoverLight ]->dudeSerial; }
	} else if ( s_emHoverFixture != 0 ) {
		const bool same = s_emSoloFixture == s_emHoverFixture;
		EmAuth_ClearSolo();
		if ( !same ) { s_emSoloFixture = s_emHoverFixture; }
	} else {
		EmAuth_ClearSolo();
	}
	common->Printf( s_emSoloLight >= 0 ? "Light Editor: SOLO light %d - only it lights the scene (Home / lightSolo again ends it)\n"
		: s_emSoloFixture ? "Light Editor: SOLO surface - only it lights the scene (Home / lightSolo again ends it)\n" : "Light Editor: solo off\n", s_emSoloLight );
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
		const byte *col = ( sph[k].light == s_emSoloLight ) ? EM_COL_SOLO : ( sph[k].light == s_emHoverLight || sph[k].light == s_emSelLight ) ? EM_COL_HOVER
			: world->lightDefs[ sph[k].light ]->dudeFillLight ? EM_COL_SCREEN
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
		const byte *col = ( s_emSoloFixture != 0 && row.fixtureId == s_emSoloFixture ) ? EM_COL_SOLO : hov ? EM_COL_HOVER : ( st > 0 ) ? EM_COL_EMITS : ( st < 0 ) ? EM_COL_OFF : EM_COL_CANEMIT;
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
			ld->dudeFillLight ? ( ld->parms.pointLight ? "DUDE item glow (r_itemGlow, made by the engine)" : "DUDE screen light (r_emissiveSurfaces, made by the engine)" )
			: ( a != NULL && a->deleted ) ? "DELETED in emissive mode" : ( a != NULL && EmAuth_LightOverridden( *a ) )
				? ( a->brightness > 0.0f ? va( "overridden, brightness x%.2f", a->brightness ) : "overridden" ) : "active",
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
		{
			rtEmAuthEmit_t ap;
			R_EmAuthFixtureEmits( world, s_emHoverFixture, fixMat, &ap );
			state += ( ap.radiance > 0.0f ) ? va( "  radiance x%.2f", ap.radiance ) : idStr( "  power-matched" );
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
	if ( !s_emLook || s_emLookFrame < tr.frameCount - 1 ) { return NULL; }	// (the console draws this frame's 2D after the view)
	if ( s_emSoloLight >= 0 || s_emSoloFixture != 0 ) {
		static char solo[320];
		idStr::snPrintf( solo, sizeof( solo ), "SOLO %s (Home ends it)%s%s", s_emSoloLight >= 0 ? "light" : "surface",
			s_emHoverText[0] ? "  |  " : "", s_emHoverText );
		if ( s_emArm == EM_ARM_NONE ) { return solo; }
	}
	if ( s_emArm != EM_ARM_NONE ) {
		static char armed[320];
		idStr::snPrintf( armed, sizeof( armed ), "%s (right-click nothing to cancel)%s%s",
			s_emArm == EM_ARM_PICK_LIGHT ? "RIGHT-CLICK THE LIGHT THIS SURFACE FOLLOWS" : s_emArm == EM_ARM_MATCH_LIGHT ? "RIGHT-CLICK THE LIGHT WHOSE POWER THIS SURFACE TAKES"
			: va( "RIGHT-CLICK A SURFACE TO FOLLOW \"%s\"", s_emArmLightKey.c_str() ),
			s_emHoverText[0] ? "  |  " : "", s_emHoverText );
		return armed;
	}
	return s_emHoverText[0] != '\0' ? s_emHoverText : NULL;
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

// delete / restore placed light `i` (false = nothing to do: restoring a light that is active)
static bool EmAuth_SetLightDeleted( const idRenderWorldLocal *world, int i, bool deleted ) {
	const emLightCache_t *c = EmAuth_LightCache( world, i );
	if ( c == NULL ) { return false; }
	int a = EmAuth_FindLight( c->key );
	if ( a < 0 ) {
		if ( !deleted ) { return false; }
		EmAuth_NewLight( s_emLights.Alloc(), c->key );
		a = s_emLights.Num() - 1;
	}
	s_emLights[a].deleted = deleted;
	if ( !deleted && !EmAuth_LightOverridden( s_emLights[a] ) ) { s_emLights.RemoveIndex( a ); }
	EmAuth_Changed();
	return true;
}

static void EmAuth_SetDeleted( const idCmdArgs &args, bool deleted ) {
	idRenderWorldLocal *world = EmAuth_CmdWorld();
	if ( world == NULL ) { return; }
	const int i = EmAuth_PickLight( world, args );
	if ( i < 0 ) { return; }
	if ( !EmAuth_SetLightDeleted( world, i, deleted ) ) { EmAuth_PrintLight( world, i, "lightRestore: already active:" ); return; }
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
	if ( emit > 0 ) { EmAuth_DefaultPhysical( false, f->id, mat ); }
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

// every surface of `mat` in this map emits (on) or is back to its per-fixture lines (off)
static void EmAuth_SetMaterialEmit( const idMaterial *mat, bool on ) {
	int mi = EmAuth_FindMaterial( mat->GetName() );
	if ( mi < 0 ) {
		if ( !on ) { return; }
		emAuthMaterial_t &em = s_emMaterials.Alloc();
		em.name = mat->GetName(); em.mat = mat; em.emit = false;
		EmAuth_ClearLook( em.look );
		mi = s_emMaterials.Num() - 1;
	}
	s_emMaterials[ mi ].emit = on;
	if ( !on && !EmAuth_LookSet( s_emMaterials[ mi ].look ) ) { s_emMaterials.RemoveIndex( mi ); }
	EmAuth_Changed();
}

// the material's entry, created on demand (emit off)
static emAuthMaterial_t &EmAuth_MaterialRef( const idMaterial *mat ) {
	int mi = EmAuth_FindMaterial( mat->GetName() );
	if ( mi < 0 ) {
		emAuthMaterial_t &em = s_emMaterials.Alloc();
		em.name = mat->GetName(); em.mat = mat; em.emit = false;
		EmAuth_ClearLook( em.look );
		mi = s_emMaterials.Num() - 1;
	}
	return s_emMaterials[ mi ];
}

static bool EmAuth_MaterialEmits( const idMaterial *mat ) {
	const int mi = ( mat != NULL ) ? EmAuth_FindMaterial( mat->GetName() ) : -1;
	return mi >= 0 && s_emMaterials[ mi ].emit;
}

// a fixture's entry, created on demand
static emAuthFixture_t &EmAuth_FixtureRef( unsigned long long id ) {
	int fi = EmAuth_FindFixture( id );
	if ( fi < 0 ) {
		EmAuth_NewFixture( s_emFixtures.Alloc(), id );
		fi = s_emFixtures.Num() - 1;
	}
	return s_emFixtures[ fi ];
}

// the power match's multiplier under the AUTHORED setup (the gather's authoredRowScale without an entry): a row at its
// authored reach, its own colour, no inherited light - where a switch to physical radiance starts, so the look holds
static float EmAuth_AuthoredMatched( const rtEmissiveRow_t &row ) {
	const int fi = EmAuth_FindFixture( row.fixtureId );
	return R_RtEmissiveMatchedScale( row, ( fi >= 0 && s_emFixtures[ fi ].reach > 0.0f ) ? s_emFixtures[ fi ].reach : 0.0f, 0.0f );
}

// the power match under the authored setup for one fixture (material = false) or the area-weighted one over a material's
// rows: where a switch to physical radiance starts, so the look does not jump. -1 = no rows (no table built)
static float EmAuth_MatchedFor( bool material, unsigned long long fixture, const idMaterial *mat ) {
	const rtEmissiveRow_t *rows = NULL;
	const int n = R_RtEmissiveRows( &rows );
	double w = 0.0, sum = 0.0;
	for ( int i = 0; i < n; i++ ) {
		if ( material ? rows[i].mat != mat : rows[i].fixtureId != fixture ) { continue; }
		if ( rows[i].dark || !( rows[i].area > 0.0f ) ) { continue; }
		w += rows[i].area; sum += rows[i].area * EmAuth_AuthoredMatched( rows[i] );
	}
	return ( w > 0.0 ) ? (float)( sum / w ) : -1.0f;
}

// NEW emitters are PHYSICAL (user, 2026-10-31): a fixture / material just made to emit, with no radiance in effect (none
// authored on it or, for a fixture, on its material - "radiance matched" counts as authored), gets the radiance its power
// match gives now: the same look, from then on its own glow's radiance x that multiplier
static void EmAuth_DefaultPhysical( bool material, unsigned long long fixture, const idMaterial *mat ) {
	const int mi = ( mat != NULL ) ? EmAuth_FindMaterial( mat->GetName() ) : -1;
	if ( mi >= 0 && s_emMaterials[ mi ].look.radiance != 0.0f ) { return; }
	if ( !material ) {
		const int fi = EmAuth_FindFixture( fixture );
		if ( fi >= 0 && s_emFixtures[ fi ].look.radiance != 0.0f ) { return; }
	}
	const float v = EmAuth_MatchedFor( material, fixture, mat );
	if ( !( v > 0.0f ) ) { return; }
	if ( material ) { EmAuth_MaterialRef( mat ).look.radiance = v; } else { EmAuth_FixtureRef( fixture ).look.radiance = v; }
	EmAuth_Changed();
}

static void EmAuth_EmitMaterial_f( const idCmdArgs &args ) {
	if ( EmAuth_CmdWorld() == NULL ) { return; }
	const idMaterial *mat = NULL; idVec3 pos;
	if ( R_RtEmissivePickFixture( &mat, &pos ) == 0 || mat == NULL ) { common->Printf( "emitMaterial: no glowing surface under the crosshair\n" ); return; }
	const int mi = EmAuth_FindMaterial( mat->GetName() );
	bool on;
	if ( args.Argc() > 1 ) { on = idStr::Icmp( args.Argv( 1 ), "off" ) != 0 && idStr::Icmp( args.Argv( 1 ), "0" ) != 0; }
	else { on = !( mi >= 0 && s_emMaterials[ mi ].emit ); }		// no argument: toggle
	EmAuth_SetMaterialEmit( mat, on );
	if ( on ) { EmAuth_DefaultPhysical( true, 0, mat ); }
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
	f->Printf( "// light <\"name\" | @x,y,z> delete    material \"<name>\" [emit] [look]    fixture <idhex> [emit|off] [scale <x>] [reach <units>] [follow \"<light>\"] [look]\n" );
	f->Printf( "// look = [radiance <x> | radiance matched] [tint <r g b>] [profile tube|lambert|uniform] [twosided 0|1]  (a fixture's own, else its material's)\n" );
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
		const emAuthMaterial_t &m = s_emMaterials[i];
		if ( !m.emit && !EmAuth_LookSet( m.look ) ) { continue; }
		f->Printf( "material \"%s\"%s%s\n", m.name.c_str(), m.emit ? " emit" : "", EmAuth_LookWords( m.look ).c_str() );
	}
	for ( int i = 0; i < s_emFixtures.Num(); i++ ) {
		const emAuthFixture_t &x = s_emFixtures[i];
		if ( x.emit == 0 && x.scale <= 0.0f && x.reach <= 0.0f && x.follow.Length() == 0 && !EmAuth_LookSet( x.look ) ) { continue; }
		f->Printf( "fixture %016llx%s", x.id, x.emit > 0 ? " emit" : x.emit < 0 ? " off" : "" );
		if ( x.scale > 0.0f ) { f->Printf( " scale %.3f", x.scale ); }
		if ( x.reach > 0.0f ) { f->Printf( " reach %.0f", x.reach ); }
		if ( x.follow.Length() ) { f->Printf( " follow \"%s\"", x.follow.c_str() ); }
		f->Printf( "%s\n", EmAuth_LookWords( x.look ).c_str() );
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
		common->Printf( "  material %s %s%s%s\n", s_emMaterials[i].name.c_str(), s_emMaterials[i].emit ? "emit" : "-", EmAuth_LookWords( s_emMaterials[i].look ).c_str(), s_emMaterials[i].mat ? "" : "  (not loaded)" );
	}
	for ( int i = 0; i < s_emFixtures.Num(); i++ ) {
		const emAuthFixture_t &x = s_emFixtures[i];
		common->Printf( "  fixture %016llx %s%s%s%s%s\n", x.id, x.emit > 0 ? "emit" : x.emit < 0 ? "off" : "-",
			x.scale > 0.0f ? va( " scale %.2f", x.scale ) : "", x.reach > 0.0f ? va( " reach %.0f", x.reach ) : "",
			x.follow.Length() ? va( " follow %s", x.follow.c_str() ) : "", EmAuth_LookWords( x.look ).c_str() );
	}
}

static void EmAuth_LightSolo_f( const idCmdArgs &args ) {
	idRenderWorldLocal *world = EmAuth_CmdWorld();
	if ( world == NULL ) { return; }
	if ( args.Argc() > 1 && atoi( args.Argv( 1 ) ) == 0 && idStr::Cmp( args.Argv( 1 ), "0" ) == 0 ) { EmAuth_ClearSolo(); common->Printf( "lightSolo: off\n" ); return; }
	if ( !s_emLook ) {														// outside look mode: the light nearest the crosshair (or named)
		const int i = EmAuth_PickLight( world, args );
		if ( i < 0 ) { return; }
		const bool same = s_emSoloLight == i;
		EmAuth_ClearSolo();
		if ( !same ) { s_emSoloLight = i; s_emSoloSerial = world->lightDefs[i]->dudeSerial; }
		EmAuth_PrintLight( world, i, same ? "lightSolo: off -" : "lightSolo: SOLO" );
		return;
	}
	EmAuth_ToggleSoloHover( world );
}

static void EmAuth_EditLights_f( const idCmdArgs &args ) {
	if ( args.Argc() > 1 ) { s_emLook = atoi( args.Argv( 1 ) ) != 0; } else { s_emLook = !s_emLook; }
	if ( !s_emLook ) {
		if ( s_emMenu.kind != EM_MENU_NONE ) { EmAuth_CloseMenu(); }
		s_emSelLight = -1;
		s_emArm = EM_ARM_NONE;
		EmAuth_ClearSolo();
		common->Printf( "editLights: look mode off\n" );
		return;
	}
	if ( rhi::GetActiveBackendType() != rhi::BT_VULKAN ) {
		s_emLook = false;
		common->Printf( "editLights: Vulkan only (r_backend vulkan)\n" );
		return;
	}
	if ( EmAuth_CmdWorld() == NULL ) { s_emLook = false; return; }
	common->Printf( "editLights: look mode ON - lights: white active, red deleted, yellow overridden, light blue a glowing screen's / item's engine light, cyan under the crosshair (+ its volume); "
		"surfaces: green emits, grey can emit, orange off by exception, cyan under the crosshair (+ its reach). lightDelete / lightRestore / "
		"emitOn / emitOff / emitMaterial ... act on what is highlighted; lightPick keeps a light's volume drawn. RIGHT-CLICK what you point at for its menu; Del deletes / restores the light (a surface: stops it emitting), Ins makes a surface emit, Home SOLOS it (only it lights the scene); the WHEEL changes a surface's radiance (Ctrl: its material's) or a light's brightness. editLights again to leave.\n" );
	if ( !r_rtEmissiveLighting.GetBool() ) {
		common->Printf( "  emissive mode is off (r_rtEmissiveLighting 0): edits apply when it is on; the surface tints need it (or r_rtEmissiveTable 1)\n" );
	}
}

/*
==============================================================================

The right-click menu (L3, user 2026-10-31: "pointing at a light and right clicking should open a menu with all the
available options to that light"): in look mode, a right-click freezes what is under the crosshair and opens an ImGui
window at the crosshair with everything that applies to it - a light, a glowing surface, or (nothing under the
crosshair) the map's file. The right-click never reaches the game. Edits are live (the next gather reads them);
Save writes the per-map file. Esc or a click outside closes it.

==============================================================================
*/

// a placed light's luminance as authored by the game: its time-average when it animates, else its colour parms
static float EmAuth_LightLum( const idRenderWorldLocal *world, int index ) {
	const float avg = R_EmAuthLightAnimAverage( world, index );
	if ( avg > 0.0f ) { return avg; }
	const float *c = world->lightDefs[ index ]->dudePristine.shaderParms;
	return Max( c[0], 0.0f ) * 0.2126f + Max( c[1], 0.0f ) * 0.7152f + Max( c[2], 0.0f ) * 0.0722f;
}

// MATCH POWER (L4, one-shot): the surface takes a placed light's power - its reach = the light's radius, its radiance the
// power match against that light's luminance (L = C R^2 / 16 A, as the old census did for a light it replaced), physical
// from then on; Brightness reset. Typically: delete the light, then match its fixture to it.
static void EmAuth_MatchLightPower( idRenderWorldLocal *world, unsigned long long fixture, int light ) {
	const idRenderLightLocal *ld = world->lightDefs[ light ];
	const renderLight_t &p = ld->dudePristine;
	const float R = p.pointLight ? Max( p.lightRadius.x, Max( p.lightRadius.y, p.lightRadius.z ) ) : p.target.Length();
	const rtEmissiveRow_t *rows = NULL;
	const int n = R_RtEmissiveRows( &rows );
	const rtEmissiveRow_t *row = NULL;
	for ( int i = 0; i < n && row == NULL; i++ ) { if ( rows[i].fixtureId == fixture ) { row = &rows[i]; } }
	const emLightCache_t *c = EmAuth_LightCache( world, light );
	if ( row == NULL || !( R > 0.0f ) ) { common->Printf( "Light Editor: match power - no emitter row for that surface / no radius\n" ); return; }
	const float radiance = Max( R_RtEmissiveMatchedScale( *row, R, Max( EmAuth_LightLum( world, light ), 1e-3f ) ), 0.01f );
	emAuthFixture_t &f = EmAuth_FixtureRef( fixture );
	f.reach = R;
	f.scale = 0.0f;
	f.look.radiance = radiance;
	EmAuth_Changed();
	common->Printf( "Light Editor: fixture %016llx matched to light \"%s\": reach %.0f, radiance x%.2f\n", fixture, c ? c->key.c_str() : "?", R, f.look.radiance );
}

// the highlighted surface's material (from its emitter row)
static const idMaterial *EmAuth_HoverMaterial( void ) {
	const rtEmissiveRow_t *rows = NULL;
	const int n = R_RtEmissiveRows( &rows );
	for ( int i = 0; i < n; i++ ) { if ( rows[i].fixtureId == s_emHoverFixture ) { return rows[i].mat; } }
	return NULL;
}

/*
===================
R_EmAuthKey

events.cpp, Del (1) / Ins (2) in game while look mode is on: Del deletes the highlighted light (again: restores it) or
stops the highlighted surface emitting; Ins makes the highlighted surface emit. true = taken (never reaches the game).
===================
*/
bool R_EmAuthKey( int key, bool down ) {
	if ( !s_emLook || tr.primaryWorld == NULL || s_emMenu.kind != EM_MENU_NONE ) { return false; }
	if ( !down ) { return true; }
	idRenderWorldLocal *world = tr.primaryWorld;
	if ( key == 3 ) { EmAuth_ToggleSoloHover( world ); return true; }	// Home: solo what is highlighted
	if ( s_emHoverLight >= 0 && s_emHoverLight < world->lightDefs.Num() && world->lightDefs[ s_emHoverLight ] != NULL ) {
		if ( key != 1 ) { return true; }
		const emLightCache_t *c = EmAuth_LightCache( world, s_emHoverLight );
		const bool deleted = c != NULL && c->auth >= 0 && s_emLights[ c->auth ].deleted;
		EmAuth_SetLightDeleted( world, s_emHoverLight, !deleted );
		EmAuth_PrintLight( world, s_emHoverLight, deleted ? "Del: restored" : "Del: deleted" );
		return true;
	}
	if ( s_emHoverFixture != 0 ) {
		const idMaterial *mat = EmAuth_HoverMaterial();
		emAuthFixture_t &f = EmAuth_FixtureRef( s_emHoverFixture );
		if ( key == 2 ) {
			f.emit = 1;
			EmAuth_Changed();
			EmAuth_DefaultPhysical( false, s_emHoverFixture, mat );
		} else {
			f.emit = EmAuth_MaterialEmits( mat ) ? -1 : 0;			// off: an exception when its material emits, else just unset
			EmAuth_Changed();
		}
		common->Printf( "%s: fixture %016llx '%s' %s\n", key == 2 ? "Ins" : "Del", s_emHoverFixture, EmAuth_ShortMaterial( mat ), key == 2 ? "emits" : "does not emit" );
	}
	return true;
}

/*
===================
R_EmAuthWheel

events.cpp, a mouse-wheel step in game while look mode is on: the highlighted surface's radiance (this surface; with Ctrl
its material) or the highlighted light's brightness, 10 % a step. false = nothing highlighted (the wheel stays the game's).
===================
*/
bool R_EmAuthWheel( int dir, bool material ) {
	if ( !s_emLook || tr.primaryWorld == NULL || s_emMenu.kind != EM_MENU_NONE || dir == 0 ) { return false; }
	idRenderWorldLocal *world = tr.primaryWorld;
	const float step = ( dir > 0 ) ? 1.1f : 1.0f / 1.1f;
	if ( s_emHoverLight >= 0 && s_emHoverLight < world->lightDefs.Num() && world->lightDefs[ s_emHoverLight ] != NULL ) {
		const emLightCache_t *c = EmAuth_LightCache( world, s_emHoverLight );
		if ( c == NULL ) { return true; }
		int a = EmAuth_FindLight( c->key );
		if ( a < 0 ) { EmAuth_NewLight( s_emLights.Alloc(), c->key ); a = s_emLights.Num() - 1; }
		emAuthLight_t &l = s_emLights[a];
		const float b = idMath::ClampFloat( 0.01f, 64.0f, ( ( l.brightness > 0.0f ) ? l.brightness : 1.0f ) * step );
		l.brightness = ( idMath::Fabs( b - 1.0f ) < 0.02f ) ? 0.0f : b;		// back at x1: unset
		if ( !l.deleted && !EmAuth_LightOverridden( l ) ) { s_emLights.RemoveIndex( a ); }
		EmAuth_Changed();
		return true;
	}
	if ( s_emHoverFixture == 0 ) { return false; }
	const idMaterial *mat = EmAuth_HoverMaterial();
	if ( material && mat == NULL ) { return true; }
	rtEmAuthEmit_t eff;
	const int fi = EmAuth_FindFixture( s_emHoverFixture );
	const int mi = ( mat != NULL ) ? EmAuth_FindMaterial( mat->GetName() ) : -1;
	if ( material ) { EmAuth_ResolveLook( NULL, mi >= 0 ? &s_emMaterials[ mi ].look : NULL, &eff ); }
	else { EmAuth_ResolveLook( fi >= 0 ? &s_emFixtures[ fi ].look : NULL, mi >= 0 ? &s_emMaterials[ mi ].look : NULL, &eff ); }
	float v = eff.radiance;
	if ( !( v > 0.0f ) ) { v = EmAuth_MatchedFor( material, s_emHoverFixture, mat ); }	// matched so far: physical from its matched look
	if ( !( v > 0.0f ) ) { v = 1.0f; }
	v = idMath::ClampFloat( 0.01f, 1024.0f, v * step );
	if ( material ) { EmAuth_MaterialRef( mat ).look.radiance = v; } else { EmAuth_FixtureRef( s_emHoverFixture ).look.radiance = v; }
	EmAuth_Changed();
	return true;
}

static void EmAuth_CloseMenu( void ) {
	if ( s_emMenu.kind == EM_MENU_LIGHT ) { s_emSelLight = s_emMenu.prevSel; s_emSelSerial = s_emMenu.prevSelSerial; }
	s_emMenu.kind = EM_MENU_NONE;
#ifndef IMGUI_DISABLE
	D3::ImGuiHooks::CloseWindow( D3::ImGuiHooks::D3_ImGuiWin_LightEditor );
#endif
}

/*
===================
R_EmAuthRightClick

events.cpp, for every right mouse button event in game (no menu, no console): true = the Light Editor took it (look
mode on). Opens the menu on what is under the crosshair, or completes an armed follow pick.
===================
*/
bool R_EmAuthRightClick( bool down ) {
	return R_EmAuthMouseButton( 2, down );
}

// any mouse button in game (1 = left, 2 = right): the right one opens the menu; a left click outside an open menu (ImGui
// did not take it) closes it instead of firing the weapon
bool R_EmAuthMouseButton( int button, bool down ) {
	static bool swallowLeftUp = false;
	if ( button == 1 ) {
		if ( !down ) { const bool took = swallowLeftUp; swallowLeftUp = false; return took; }
		if ( s_emMenu.kind == EM_MENU_NONE ) { return false; }
		EmAuth_CloseMenu();
		swallowLeftUp = true;
		return true;
	}
	if ( button != 2 ) { return false; }
	if ( !down ) {
		const bool took = s_emSwallowRightUp;
		s_emSwallowRightUp = false;
		return took;
	}
	if ( !s_emLook || tr.primaryWorld == NULL ) { return false; }
	s_emSwallowRightUp = true;
	idRenderWorldLocal *world = tr.primaryWorld;
	if ( s_emArm == EM_ARM_PICK_LIGHT ) {
		const emLightCache_t *c = ( s_emHoverLight >= 0 ) ? EmAuth_LightCache( world, s_emHoverLight ) : NULL;
		if ( c != NULL ) {
			emAuthFixture_t &f = EmAuth_FixtureRef( s_emArmFixture );
			f.follow = c->key; f.followGen = 0;
			EmAuth_Changed();
			common->Printf( "Light Editor: fixture %016llx now follows light \"%s\"\n", s_emArmFixture, c->key.c_str() );
		} else {
			common->Printf( "Light Editor: follow pick cancelled\n" );
		}
		s_emArm = EM_ARM_NONE;
		return true;
	}
	if ( s_emArm == EM_ARM_MATCH_LIGHT ) {
		if ( s_emHoverLight >= 0 ) { EmAuth_MatchLightPower( world, s_emArmFixture, s_emHoverLight ); }
		else { common->Printf( "Light Editor: match pick cancelled\n" ); }
		s_emArm = EM_ARM_NONE;
		return true;
	}
	if ( s_emArm == EM_ARM_PICK_SURFACE ) {
		if ( s_emHoverFixture != 0 ) {
			emAuthFixture_t &f = EmAuth_FixtureRef( s_emHoverFixture );
			f.follow = s_emArmLightKey; f.followGen = 0;
			EmAuth_Changed();
			common->Printf( "Light Editor: fixture %016llx now follows light \"%s\"\n", s_emHoverFixture, s_emArmLightKey.c_str() );
		} else {
			common->Printf( "Light Editor: follow pick cancelled\n" );
		}
		s_emArm = EM_ARM_NONE;
		return true;
	}
#ifndef IMGUI_DISABLE
	if ( !D3::ImGuiHooks::IsImguiEnabled() ) {
		common->Printf( "Light Editor: ImGui is not available - use the console commands\n" );
		return true;
	}
	if ( s_emMenu.kind != EM_MENU_NONE ) { EmAuth_CloseMenu(); }
	emMenu_t m;
	memset( &m, 0, sizeof( m ) );
	m.prevSel = s_emSelLight; m.prevSelSerial = s_emSelSerial;
	if ( s_emHoverLight >= 0 && s_emHoverLight < world->lightDefs.Num() && world->lightDefs[ s_emHoverLight ] != NULL ) {
		m.kind = EM_MENU_LIGHT;
		m.light = s_emHoverLight;
		m.serial = world->lightDefs[ s_emHoverLight ]->dudeSerial;
		s_emSelLight = m.light; s_emSelSerial = m.serial;		// its volume stays drawn while the menu is open
	} else if ( s_emHoverFixture != 0 ) {
		m.kind = EM_MENU_SURFACE;
		m.fixture = R_RtEmissivePickFixture( &m.mat, &m.pos );
		if ( m.fixture == 0 ) { m.fixture = s_emHoverFixture; }
	} else {
		m.kind = EM_MENU_GENERAL;
	}
	s_emMenu = m;
	D3::ImGuiHooks::OpenWindow( D3::ImGuiHooks::D3_ImGuiWin_LightEditor );
#endif
	return true;
}

#ifndef IMGUI_DISABLE

static void EmAuth_MenuFileButtons( void ) {
	idCmdArgs none;
	ImGui::Separator();
	if ( ImGui::Button( s_emDirty ? "Save*" : "Save" ) ) { EmAuth_Save_f( none ); }
	if ( ImGui::IsItemHovered() ) { ImGui::SetTooltip( "Write %s to the save path", EmAuth_FilePath( s_emMap ).c_str() ); }
	ImGui::SameLine();
	if ( ImGui::Button( "Reload" ) ) { EmAuth_Reload_f( none ); }
	if ( ImGui::IsItemHovered() ) { ImGui::SetTooltip( "Re-read the saved file (unsaved edits are lost)" ); }
	ImGui::SameLine();
	if ( ImGui::Button( "Close" ) ) { EmAuth_CloseMenu(); }
	if ( !r_rtEmissiveLighting.GetBool() ) {
		ImGui::TextColored( ImVec4( 1.0f, 0.75f, 0.3f, 1.0f ), "Emissive mode is off: edits apply when it is on." );
		ImGui::SameLine();
		if ( ImGui::SmallButton( "Turn it on" ) ) { r_rtEmissiveLighting.SetBool( true ); }
	}
}

// L4: the light's colour / brightness / radius in emissive mode (live: RenderScene re-derives it from the game's parms)
static void EmAuth_MenuLightOverrides( idRenderWorldLocal *world, const idStr &key ) {
	ImGui::SeparatorText( "Colour, brightness, radius (emissive mode)" );
	const idRenderLightLocal *ld = world->lightDefs[ s_emMenu.light ];
	const renderLight_t &p = ld->dudePristine;									// the game's own values
	int a = EmAuth_FindLight( key );
	emAuthLight_t cur;
	if ( a >= 0 ) { cur = s_emLights[a]; } else { EmAuth_NewLight( cur, key ); }
	bool changed = false;
	float col[3] = { cur.hasColor ? cur.color.x : p.shaderParms[0], cur.hasColor ? cur.color.y : p.shaderParms[1], cur.hasColor ? cur.color.z : p.shaderParms[2] };
	if ( ImGui::ColorEdit3( "Colour", col, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR ) ) { cur.hasColor = true; cur.color.Set( col[0], col[1], col[2] ); changed = true; }
	if ( cur.hasColor ) { ImGui::SameLine(); if ( ImGui::SmallButton( "Reset##col" ) ) { cur.hasColor = false; changed = true; } }
	float b = ( cur.brightness > 0.0f ) ? cur.brightness : 1.0f;
	if ( ImGui::SliderFloat( "Brightness##light", &b, 0.05f, 16.0f, "%.2f x", ImGuiSliderFlags_Logarithmic ) ) { cur.brightness = b; changed = true; }
	if ( cur.brightness > 0.0f ) { ImGui::SameLine(); if ( ImGui::SmallButton( "Reset##bri" ) ) { cur.brightness = 0.0f; changed = true; } }
	if ( p.pointLight && !p.parallel ) {
		float r[3] = { cur.hasRadius ? cur.radius.x : p.lightRadius.x, cur.hasRadius ? cur.radius.y : p.lightRadius.y, cur.hasRadius ? cur.radius.z : p.lightRadius.z };
		if ( ImGui::DragFloat3( "Radius", r, 1.0f, 1.0f, 4096.0f, "%.0f" ) ) { cur.hasRadius = true; cur.radius.Set( r[0], r[1], r[2] ); changed = true; }
		if ( cur.hasRadius ) { ImGui::SameLine(); if ( ImGui::SmallButton( "Reset##rad" ) ) { cur.hasRadius = false; changed = true; } }
	} else {
		ImGui::TextDisabled( "(a projected light: no radius override)" );
	}
	if ( !r_rtEmissiveLighting.GetBool() ) { ImGui::TextDisabled( "(applies while emissive mode is on)" ); }
	if ( changed ) {
		if ( a < 0 ) { s_emLights.Append( cur ); }
		else if ( !cur.deleted && !EmAuth_LightOverridden( cur ) ) { s_emLights.RemoveIndex( a ); }
		else { s_emLights[a] = cur; }
		EmAuth_Changed();
	}
}

static void EmAuth_MenuLight( idRenderWorldLocal *world ) {
	const idRenderLightLocal *ld = world->lightDefs[ s_emMenu.light ];
	const emLightCache_t *c = EmAuth_LightCache( world, s_emMenu.light );
	if ( c == NULL ) { EmAuth_CloseMenu(); return; }
	const idStr key = c->key;
	const emAuthLight_t *a = ( c->auth >= 0 ) ? &s_emLights[ c->auth ] : NULL;
	const bool deleted = a != NULL && a->deleted;
	ImGui::Text( "Light \"%s\"", key.c_str() );
	ImGui::TextDisabled( "%s", ld->lightShader ? ld->lightShader->GetName() : "?" );
	ImGui::TextDisabled( "at %.0f %.0f %.0f   %s", ld->dudePristine.origin.x, ld->dudePristine.origin.y, ld->dudePristine.origin.z,
		ld->dudePristine.pointLight ? va( "radius %.0f %.0f %.0f", ld->dudePristine.lightRadius.x, ld->dudePristine.lightRadius.y, ld->dudePristine.lightRadius.z ) : "projected" );
	ImGui::TextDisabled( "game colour %.2f %.2f %.2f%s%s", ld->dudePristine.shaderParms[0], ld->dudePristine.shaderParms[1], ld->dudePristine.shaderParms[2],
		ld->parms.noShadows ? ", no shadows" : "", key[0] == '@' ? ", keyed by position (no .map name)" : "" );
	if ( a != NULL && EmAuth_LightOverridden( *a ) ) {
		idStr o;
		if ( a->brightness > 0.0f ) { o += va( "brightness x%.2f  ", a->brightness ); }
		if ( a->hasColor ) { o += va( "colour %.2f %.2f %.2f  ", a->color.x, a->color.y, a->color.z ); }
		if ( a->hasRadius ) { o += va( "radius %.0f %.0f %.0f", a->radius.x, a->radius.y, a->radius.z ); }
		ImGui::TextColored( ImVec4( 1.0f, 0.86f, 0.25f, 1.0f ), "Overridden: %s%s", o.c_str(), r_rtEmissiveLighting.GetBool() ? "" : " (emissive mode is off)" );
	}
	ImGui::Separator();
	if ( deleted ) {
		ImGui::TextColored( ImVec4( 1.0f, 0.35f, 0.3f, 1.0f ), "Deleted in emissive mode" );
		if ( ImGui::Button( "Restore" ) ) { EmAuth_SetLightDeleted( world, s_emMenu.light, false ); }
	} else {
		ImGui::Text( "Active" );
		if ( ImGui::Button( "Delete in emissive mode" ) ) { EmAuth_SetLightDeleted( world, s_emMenu.light, true ); }
		if ( ImGui::IsItemHovered() ) { ImGui::SetTooltip( "The light is gone while r_rtEmissiveLighting is on; normal mode keeps it" ); }
	}
	if ( ImGui::Button( "Make a surface follow this light..." ) ) {
		s_emArm = EM_ARM_PICK_SURFACE; s_emArmLightKey = key;
		EmAuth_CloseMenu();
		return;
	}
	if ( ImGui::IsItemHovered() ) { ImGui::SetTooltip( "Then right-click a glowing surface: it flickers / pulses with this light" ); }
	bool keep = s_emMenu.prevSel == s_emMenu.light && s_emMenu.prevSelSerial == s_emMenu.serial;
	if ( ImGui::Checkbox( "Keep its volume shown", &keep ) ) {
		s_emMenu.prevSel = keep ? s_emMenu.light : -1; s_emMenu.prevSelSerial = keep ? s_emMenu.serial : 0u;
	}
	bool solo = s_emSoloLight == s_emMenu.light;
	if ( ImGui::Checkbox( "Solo: only this light lights the scene", &solo ) ) {
		EmAuth_ClearSolo();
		if ( solo ) { s_emSoloLight = s_emMenu.light; s_emSoloSerial = s_emMenu.serial; }
	}
	if ( ImGui::SmallButton( "Copy name" ) ) { ImGui::SetClipboardText( key.c_str() ); }
	EmAuth_MenuLightOverrides( world, key );
	EmAuth_MenuFileButtons();
}

static float EmAuth_MenuMatched( int scope ) {
	const float v = EmAuth_MatchedFor( scope == 0, s_emMenu.fixture, s_emMenu.mat );
	return ( v > 0.0f ) ? v : 1.0f;
}

// The surface menu's Appearance - radiance, tint, emission profile, two-sided - for the material (every surface of it in
// this map) or this surface only (its own value wins over the material's). Live: each change is in the next frame's rows.
static void EmAuth_MenuAppearance( void ) {
	emMenu_t &m = s_emMenu;
	ImGui::SeparatorText( "Appearance" );
	ImGui::RadioButton( "This material", &m.apScope, 0 );
	ImGui::SameLine();
	ImGui::RadioButton( "This surface only", &m.apScope, 1 );
	if ( m.mat == NULL ) { m.apScope = 1; }
	const bool surf = m.apScope == 1;
	// the entry being edited (looked up every frame: the controls may create it) and what it inherits
	const int fi = EmAuth_FindFixture( m.fixture );
	const emAuthMaterial_t *me = EmAuth_MaterialEntry( m.mat );
	const emAuthLook_t *matLook = ( me != NULL ) ? &me->look : NULL;
	emAuthLook_t cur;
	EmAuth_ClearLook( cur );
	if ( surf ) { if ( fi >= 0 ) { cur = s_emFixtures[ fi ].look; } }
	else if ( matLook != NULL ) { cur = *matLook; }
	rtEmAuthEmit_t eff;															// what the controls show: the scope's own value, else what it inherits
	if ( surf ) { EmAuth_ResolveLook( &cur, matLook, &eff ); } else { EmAuth_ResolveLook( NULL, &cur, &eff ); }
	bool changed = false;
	auto inheritNote = [&]( bool own ) { if ( surf && !own ) { ImGui::SameLine(); ImGui::TextDisabled( "(material's)" ); } };

	// radiance: power-matched to the reach, or physical
	bool physical = eff.radiance > 0.0f;
	if ( ImGui::Checkbox( "Physical radiance", &physical ) ) {
		if ( physical ) { cur.radiance = EmAuth_MenuMatched( m.apScope ); }		// start from the matched look
		else { cur.radiance = ( surf && matLook != NULL && matLook->radiance > 0.0f ) ? -1.0f : 0.0f; }	// a surface under a physical material: explicitly matched
		changed = true;
	}
	if ( ImGui::IsItemHovered() ) { ImGui::SetTooltip( "Off: brightness set so the surface lights like a light of its own colour at its Reach.\n"
		"On: its glow's own radiance x the multiplier below - Reach then only limits how far it lights." ); }
	inheritNote( cur.radiance != 0.0f );
	if ( physical ) {
		float rad = ( cur.radiance > 0.0f ) ? cur.radiance : eff.radiance;
		if ( ImGui::SliderFloat( "Radiance x", &rad, 0.05f, 256.0f, "%.2f", ImGuiSliderFlags_Logarithmic ) ) { cur.radiance = Max( rad, 0.01f ); changed = true; }
		inheritNote( cur.radiance > 0.0f );
	}
	// tint
	float tint[3] = { eff.tint[0], eff.tint[1], eff.tint[2] };
	if ( ImGui::ColorEdit3( "Tint", tint, ImGuiColorEditFlags_Float ) ) { cur.hasTint = true; cur.tint.Set( tint[0], tint[1], tint[2] ); changed = true; }
	inheritNote( cur.hasTint );
	if ( cur.hasTint ) { ImGui::SameLine(); if ( ImGui::SmallButton( "Reset##tint" ) ) { cur.hasTint = false; changed = true; } }
	// emission profile
	int prof = cur.profile + 1;
	const char *profItems = surf ? "as its material\0sqrt(cos) - a bare tube\0Lambert - a diffuser / panel\0uniform - a globe / bulb\0"
		: "default (bare tube)\0sqrt(cos) - a bare tube\0Lambert - a diffuser / panel\0uniform - a globe / bulb\0";
	if ( ImGui::Combo( "Emission profile", &prof, profItems ) ) { cur.profile = prof - 1; changed = true; }
	// two-sided
	int two = cur.twoSided + 1;
	const char *twoItems = surf ? "as its material\0one-sided\0two-sided\0" : "default (one-sided)\0one-sided\0two-sided\0";
	if ( ImGui::Combo( "Faces", &two, twoItems ) ) { cur.twoSided = two - 1; changed = true; }
	if ( m.mat != NULL && m.mat->ShouldCreateBackSides() ) { ImGui::TextDisabled( "(this material has back faces in its model: both sides emit anyway)" ); }

	if ( changed ) {
		if ( surf ) {
			EmAuth_FixtureRef( m.fixture ).look = cur;
		} else if ( m.mat != NULL ) {
			emAuthMaterial_t &em = EmAuth_MaterialRef( m.mat );
			em.look = cur;
			if ( !em.emit && !EmAuth_LookSet( em.look ) ) { s_emMaterials.RemoveIndex( EmAuth_FindMaterial( m.mat->GetName() ) ); }
		}
		EmAuth_Changed();
	}
	if ( ImGui::SmallButton( surf ? "Clear this surface's appearance" : "Clear the material's appearance" ) ) {
		if ( surf ) { if ( fi >= 0 ) { EmAuth_ClearLook( s_emFixtures[ fi ].look ); } }
		else if ( m.mat != NULL ) {
			const int mi = EmAuth_FindMaterial( m.mat->GetName() );
			if ( mi >= 0 ) {
				EmAuth_ClearLook( s_emMaterials[ mi ].look );
				if ( !s_emMaterials[ mi ].emit ) { s_emMaterials.RemoveIndex( mi ); }
			}
		}
		EmAuth_Changed();
	}
}

static void EmAuth_MenuSurface( idRenderWorldLocal *world ) {
	const unsigned long long id = s_emMenu.fixture;
	const idMaterial *mat = s_emMenu.mat;
	const int fi = EmAuth_FindFixture( id );
	const emAuthFixture_t *f = ( fi >= 0 ) ? &s_emFixtures[ fi ] : NULL;
	bool own;
	const int st = EmAuth_FixtureState( world, id, mat, &own );
	ImGui::Text( "Surface %s", EmAuth_ShortMaterial( mat ) );
	ImGui::TextDisabled( "fixture %016llx at %.0f %.0f %.0f", id, s_emMenu.pos.x, s_emMenu.pos.y, s_emMenu.pos.z );
	ImGui::TextColored( st > 0 ? ImVec4( 0.3f, 0.95f, 0.4f, 1.0f ) : st < 0 ? ImVec4( 1.0f, 0.6f, 0.2f, 1.0f ) : ImVec4( 0.7f, 0.7f, 0.7f, 1.0f ), "%s",
		st > 0 ? ( own ? "Emits (this surface)" : "Emits (its material)" ) : st < 0 ? "Off (exception to its material)" : "Can emit" );
	ImGui::Separator();
	int emit = ( f != NULL ) ? f->emit : 0;
	ImGui::TextUnformatted( "This surface:" );
	ImGui::SameLine(); bool ch = ImGui::RadioButton( "as its material", &emit, 0 );
	ImGui::SameLine(); ch |= ImGui::RadioButton( "emits", &emit, 1 );
	ImGui::SameLine(); ch |= ImGui::RadioButton( "off", &emit, -1 );
	if ( ch ) { EmAuth_FixtureRef( id ).emit = emit; EmAuth_Changed(); if ( emit > 0 ) { EmAuth_DefaultPhysical( false, id, mat ); } }
	if ( mat != NULL ) {
		bool matOn = EmAuth_MaterialEmits( mat );
		if ( ImGui::Checkbox( "Every surface of this material in this map emits", &matOn ) ) { EmAuth_SetMaterialEmit( mat, matOn ); if ( matOn ) { EmAuth_DefaultPhysical( true, 0, mat ); } }
	}
	// brightness and reach (0 = unset: shown at their defaults)
	const float reachDef = idMath::ClampFloat( 0.0f, 4096.0f, r_rtEmissiveReachDefault.GetFloat() );
	float scale = ( f != NULL && f->scale > 0.0f ) ? f->scale : 1.0f;
	if ( ImGui::SliderFloat( "Brightness", &scale, 0.05f, 32.0f, "%.2f x", ImGuiSliderFlags_Logarithmic ) ) {
		EmAuth_FixtureRef( id ).scale = Max( scale, 0.01f ); EmAuth_Changed();
	}
	ImGui::SameLine();
	if ( ImGui::SmallButton( "Reset##scale" ) && f != NULL ) { EmAuth_FixtureRef( id ).scale = 0.0f; EmAuth_Changed(); }
	float reach = ( f != NULL && f->reach > 0.0f ) ? f->reach : reachDef;
	if ( ImGui::SliderFloat( "Reach", &reach, 8.0f, 2048.0f, "%.0f units", ImGuiSliderFlags_Logarithmic ) ) {
		EmAuth_FixtureRef( id ).reach = Max( reach, 1.0f ); EmAuth_Changed();
	}
	ImGui::SameLine();
	if ( ImGui::SmallButton( "Reset##reach" ) && f != NULL ) { EmAuth_FixtureRef( id ).reach = 0.0f; EmAuth_Changed(); }
	ImGui::TextDisabled( "(reset reach = r_rtEmissiveReachDefault, %.0f)", reachDef );
	// re-read: the controls above may have created / changed the entry
	const int fi2 = EmAuth_FindFixture( id );
	const emAuthFixture_t *f2 = ( fi2 >= 0 ) ? &s_emFixtures[ fi2 ] : NULL;
	if ( f2 != NULL && f2->follow.Length() ) {
		ImGui::Text( "Follows light \"%s\"%s", f2->follow.c_str(), EmAuth_LightIndexByKey( world, f2->follow ) < 0 ? " (not in the map now)" : "" );
		ImGui::SameLine();
		if ( ImGui::SmallButton( "Stop" ) ) { EmAuth_FixtureRef( id ).follow.Clear(); EmAuth_FixtureRef( id ).followGen = 0; EmAuth_Changed(); }
	}
	if ( ImGui::Button( "Follow a light..." ) ) {
		s_emArm = EM_ARM_PICK_LIGHT; s_emArmFixture = id;
		EmAuth_CloseMenu();
		return;
	}
	if ( ImGui::IsItemHovered() ) { ImGui::SetTooltip( "Then right-click the placed light whose flicker / pulse this surface copies" ); }
	ImGui::SameLine();
	if ( ImGui::Button( "Match a light's power..." ) ) {
		s_emArm = EM_ARM_MATCH_LIGHT; s_emArmFixture = id;
		EmAuth_CloseMenu();
		return;
	}
	if ( ImGui::IsItemHovered() ) { ImGui::SetTooltip( "Then right-click a placed light (usually the one you deleted for this fixture): this surface\n"
		"takes its reach and lights like it (physical radiance from the power match), once" ); }
	if ( ImGui::Button( "Clear this surface's settings" ) && fi2 >= 0 ) { s_emFixtures.RemoveIndex( fi2 ); EmAuth_Changed(); }
	bool solo = s_emSoloFixture == id;
	if ( ImGui::Checkbox( "Solo: only this surface lights the scene", &solo ) ) {
		EmAuth_ClearSolo();
		if ( solo ) { s_emSoloFixture = id; }
	}
	if ( solo && st <= 0 ) { ImGui::SameLine(); ImGui::TextDisabled( "(it does not emit: the scene goes dark)" ); }
	if ( ImGui::SmallButton( "Copy fixture id" ) ) { ImGui::SetClipboardText( va( "%016llx", id ) ); }
	EmAuth_MenuAppearance();
	EmAuth_MenuFileButtons();
}

static void EmAuth_MenuGeneral( void ) {
	idCmdArgs none;
	int deleted = 0;
	for ( int i = 0; i < s_emLights.Num(); i++ ) { deleted += s_emLights[i].deleted ? 1 : 0; }
	int emitting = 0;
	for ( int i = 0; i < s_emFixtures.Num(); i++ ) { emitting += ( s_emFixtures[i].emit > 0 ) ? 1 : 0; }
	ImGui::Text( "%s", s_emMap.c_str() );
	ImGui::TextDisabled( "%s%s", EmAuth_FilePath( s_emMap ).c_str(), s_emFileFound ? "" : " (not saved yet)" );
	ImGui::Text( "%d lights deleted, %d materials emit, %d surfaces emit on their own", deleted, s_emMaterials.Num(), emitting );
	ImGui::TextDisabled( "Point at a light or a glowing surface and right-click it to edit it." );
	if ( EmAuth_FileExists( EmAuth_RecoveryPath( s_emMap ) ) ) {
		ImGui::TextColored( ImVec4( 1.0f, 0.75f, 0.3f, 1.0f ), "Unsaved edits from an earlier session were kept:" );
		if ( ImGui::Button( "Recover them" ) ) { EmAuth_Recover_f( none ); }
		ImGui::SameLine();
		if ( ImGui::Button( "Discard them" ) ) { EmAuth_DiscardRecovery_f( none ); }
	}
	if ( s_emSoloLight >= 0 || s_emSoloFixture != 0 ) {
		if ( ImGui::Button( "End solo" ) ) { EmAuth_ClearSolo(); }
		ImGui::SameLine();
	}
	if ( ImGui::Button( "Leave look mode" ) ) {
		EmAuth_CloseMenu();
		s_emLook = false; s_emSelLight = -1; s_emArm = EM_ARM_NONE; EmAuth_ClearSolo();
		return;
	}
	EmAuth_MenuFileButtons();
}

/*
===================
Com_DrawLightEditorMenu

sys_imgui.cpp, every ImGui frame while the window is open.
===================
*/
void Com_DrawLightEditorMenu( void ) {
	idRenderWorldLocal *world = tr.primaryWorld;
	if ( s_emMenu.kind == EM_MENU_NONE || !s_emLook || world == NULL || world->mapName.Icmp( s_emMap ) != 0 ) {
		EmAuth_CloseMenu();
		return;
	}
	if ( s_emMenu.kind == EM_MENU_LIGHT && ( s_emMenu.light >= world->lightDefs.Num() || world->lightDefs[ s_emMenu.light ] == NULL
			|| world->lightDefs[ s_emMenu.light ]->dudeSerial != s_emMenu.serial ) ) {
		EmAuth_CloseMenu();											// the light went away under the menu
		return;
	}
	const ImVec2 centre( ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f );
	if ( s_emMenu.openFrames == 0 ) {
		ImGui::SetNextWindowPos( ImVec2( centre.x + 24.0f, centre.y + 24.0f ), ImGuiCond_Always );
		ImGui::SetNextWindowFocus();
	}
	if ( s_emMenu.openFrames < 3 ) {
		ImGui::TeleportMousePos( ImVec2( centre.x + 48.0f, centre.y + 40.0f ) );	// the cursor onto the menu (the game had it hidden, relative)
	}
	bool open = true;
	const char *title = s_emMenu.kind == EM_MENU_LIGHT ? "Light Editor - light###LightEditorMenu"
		: s_emMenu.kind == EM_MENU_SURFACE ? "Light Editor - surface###LightEditorMenu" : "Light Editor###LightEditorMenu";
	if ( ImGui::Begin( title, &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings ) ) {
		if ( s_emMenu.kind == EM_MENU_LIGHT ) { EmAuth_MenuLight( world ); }
		else if ( s_emMenu.kind == EM_MENU_SURFACE ) { EmAuth_MenuSurface( world ); }
		else { EmAuth_MenuGeneral(); }
		// Esc, or a click outside (focus lost once it had it), closes it
		const bool focused = ImGui::IsWindowFocused( ImGuiFocusedFlags_RootAndChildWindows );
		if ( focused && ImGui::IsKeyPressed( ImGuiKey_Escape ) ) { open = false; }
		if ( !focused && s_emMenu.openFrames > 2 ) { open = false; }
	}
	ImGui::End();
	if ( s_emMenu.kind != EM_MENU_NONE ) { s_emMenu.openFrames++; }
	if ( !open && s_emMenu.kind != EM_MENU_NONE ) { EmAuth_CloseMenu(); }
}

#endif // IMGUI_DISABLE

void R_EmAuthRegisterCommands( void ) {
	cmdSystem->AddCommand( "lightSolo", EmAuth_LightSolo_f, CMD_FL_RENDERER, "emissive authoring: SOLO the highlighted light or surface (look mode; else the light nearest the crosshair, or lightSolo <name>) - only it lights the scene; again (or lightSolo 0) ends it. Session only" );
	cmdSystem->AddCommand( "editEmitter", EmAuth_EditLights_f, CMD_FL_RENDERER, "the old Emitter Editor's command (2026-10-31): now an alias of editLights - the Light Editor's look mode; right-click a surface for its emitter settings" );
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
