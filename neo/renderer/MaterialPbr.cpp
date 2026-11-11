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
#include "idlib/containers/HashTable.h"	// DUDE PBR per-material table
#include "idlib/math/Interpolate.h"
#include "renderer/Cinematic.h"
#include "renderer/tr_local.h"
#include "ui/Window.h"
#include "ui/UserInterface.h"
#include "sound/sound.h"
#include "framework/GameTitle.h"

#include "renderer/Material.h"

/*
===========================================================================

DUDE: the PBR per-material table (docs/pbr-materials.md Phase B), moved out of Material.cpp as
one block (the structural pass): the category vocabulary and its defaults, the table files'
parse / load / write, and idMaterial::ApplyPbrTable. The parser calls ApplyPbrTable; the
editor and the reloadPbrTable command use the R_Pbr* entry points (Material.h, tr_local.h).

===========================================================================
*/
/*
===============================================================================

	DUDE PBR per-material table (docs/pbr-materials.md Phase B)

	Two files, loaded lazily on the first material parse (both via the VFS, so a
	writable fs_savepath copy outranks the shipped fs_basepath one):
	  pbr/pbr_materials.cfg  — the main authored config (bootstrapped by
	                           tools/pbr_classify.py, then hand-tuned in-game and
	                           baked in); loaded first
	  pbr/pbr_overrides.cfg  — per-game deltas, loaded second so they win on
	                           collision. The editor (R_PbrWriteOverrideLine) writes
	                           these to fs_savepath (the "dude folder"), so edits are
	                           deltas on the base baseline and win on the next load
	                           regardless of launch method; bake folds them down into
	                           pbr_materials.cfg for release.
	Both share one column layout (the override file is a superset of the generated
	one):

	  <material>  <metalness>  <roughness>  [category]  [wetness]  [env]

	'#' starts a comment. Any numeric column may be '*' (also '-' / 'auto') meaning
	*inherit*: metalness/roughness fall back to the per-category slider or the r_pbr*
	global, wetness falls back to the per-category wetness cvar (1.0 off-category),
	env falls back to 1.0. [category] is a class token (pbrCategoryInfos below:
	Quake 4's materialType names - solidmetal, hollowmetal, concrete, rock, flesh,
	glass... - plus DUDE's skin, eyes, ceramic, rust, wood, cloth, cardboard) or
	"none"; a real category makes the entry track its live preset row ("@cat"), and
	"none"/absent pins the explicit numbers. The pre-unification Doom 3 names
	(metal, metal_painted, ceramic_sheen, metal_rust, stone, liquid) still load as
	aliases, so older override files keep working. Keys match case-insensitively. reloadPbrTable
	(RenderSystem_init.cpp) invalidates the cache and re-applies to parsed materials,
	so edits tune live in-game.

===============================================================================
*/

struct pbrTableEntry_t {
	float	metalness;		// -1 = inherit
	float	roughness;		// -1 = inherit
	float	wetness;		// -1 = inherit
	float	env;			// -1 = inherit
	int		category;		// pbrCategory_t; PBR_CAT_NONE when category "none"/absent
};

static idHashTable<pbrTableEntry_t>	pbrTable;
static bool							pbrTableLoaded = false;

// parse a numeric table column: '*', '-' and 'auto' mean inherit (returns -1);
// otherwise the float value (caller clamps).
static float PBR_ParseNum( const char *tok ) {
	if ( !tok[0] || tok[0] == '*' || tok[0] == '-' || !idStr::Icmp( tok, "auto" ) ) {
		return -1.0f;
	}
	return atof( tok );
}

// DUDE: the class vocabulary, in UI display order (docs/pbr-materials.md). Tokens are
// Quake 4's materialType names where Raven defined one, so a Quake 4 material's declared
// type and its PBR class read the same.
static const pbrCategoryInfo_t pbrCategoryInfos[] = {
	{ PBR_CAT_SOLIDMETAL,      "solidmetal",      "Solid Metal",          "Metal" },
	{ PBR_CAT_HOLLOWMETAL,     "hollowmetal",     "Hollow Metal (panels)", "Metal" },
	{ PBR_CAT_MONSTERMETAL,    "monstermetal",    "Monster Metal",        "Metal" },
	{ PBR_CAT_RUST,            "rust",            "Rusted Metal",         "Metal" },
	{ PBR_CAT_CONCRETE,        "concrete",        "Concrete / Brick",     "Mineral" },
	{ PBR_CAT_CERAMIC,         "ceramic",         "Ceramic Sheen",        "Mineral" },
	{ PBR_CAT_ROCK,            "rock",            "Rock",                 "Mineral" },
	{ PBR_CAT_DIRT,            "dirt",            "Dirt",                 "Mineral" },
	{ PBR_CAT_SAND,            "sand",            "Sand",                 "Mineral" },
	{ PBR_CAT_GRAVEL,          "gravel",          "Gravel",               "Mineral" },
	{ PBR_CAT_SKIN,            "skin",            "Skin (faces)",         "Organic" },
	{ PBR_CAT_EYES,            "eyes",            "Eyes / Teeth",         "Organic" },
	{ PBR_CAT_FLESH,           "flesh",           "Flesh / Gore",         "Organic" },
	{ PBR_CAT_WOOD,            "wood",            "Wood",                 "Organic" },
	{ PBR_CAT_CLOTH,           "cloth",           "Cloth",                "Organic" },
	{ PBR_CAT_CARDBOARD,       "cardboard",       "Cardboard / Paper",    "Organic" },
	{ PBR_CAT_ARMOR,           "armor",           "Armor",                "Synthetic" },
	{ PBR_CAT_ELECTRONICS,     "electronics",     "Electronics",          "Synthetic" },
	{ PBR_CAT_PLASTIC,         "plastic",         "Plastic",              "Synthetic" },
	{ PBR_CAT_RUBBER,          "rubber",          "Rubber",               "Synthetic" },
	{ PBR_CAT_GLASS,           "glass",           "Glass",                "Glass / Liquid" },
	{ PBR_CAT_BPGLASS,         "bpglass",         "Bulletproof Glass",    "Glass / Liquid" },
	{ PBR_CAT_WATER,           "water",           "Water",                "Glass / Liquid" },
	{ PBR_CAT_OIL,             "oil",             "Oil",                  "Glass / Liquid" },
	{ PBR_CAT_GELATINOUSFLUID, "gelatinousfluid", "Gelatinous Fluid",     "Glass / Liquid" },
};
static_assert( sizeof( pbrCategoryInfos ) / sizeof( pbrCategoryInfos[0] ) == PBR_CAT_COUNT - 1,
               "pbrCategoryInfos needs one row per class" );

// the pre-unification Doom 3 names, still accepted on load (old override files)
static const struct { const char *name; int cat; } pbrCategoryAliases[] = {
	{ "metal",         PBR_CAT_SOLIDMETAL },
	{ "metal_painted", PBR_CAT_HOLLOWMETAL },
	{ "ceramic_sheen", PBR_CAT_CERAMIC },
	{ "metal_rust",    PBR_CAT_RUST },
	{ "stone",         PBR_CAT_CONCRETE },	// an "@cat stone" row seeds all five mineral classes it was split into
	{ "liquid",        PBR_CAT_WATER },
};

int R_PbrNumCategoryInfos( void ) {
	return PBR_CAT_COUNT - 1;
}

const pbrCategoryInfo_t &R_PbrCategoryInfo( int i ) {
	return pbrCategoryInfos[ idMath::ClampInt( 0, PBR_CAT_COUNT - 2, i ) ];
}

// table category column -> class; "none", absent and unknown tokens -> PBR_CAT_NONE
// (the entry's explicit numbers stand)
static int PBR_CategoryForName( const char *cat ) {
	if ( !cat[0] || cat[0] == '*' || cat[0] == '-' ) {
		return PBR_CAT_NONE;
	}
	for ( int i = 0; i < PBR_CAT_COUNT - 1; i++ ) {
		if ( !idStr::Icmp( cat, pbrCategoryInfos[i].token ) ) {
			return pbrCategoryInfos[i].cat;
		}
	}
	for ( int i = 0; i < (int)( sizeof( pbrCategoryAliases ) / sizeof( pbrCategoryAliases[0] ) ); i++ ) {
		if ( !idStr::Icmp( cat, pbrCategoryAliases[i].name ) ) {
			return pbrCategoryAliases[i].cat;
		}
	}
	return PBR_CAT_NONE;
}

// class -> table token, for the material editor's write-back (inverse of the above)
const char *R_PbrCategoryToken( int cat ) {
	for ( int i = 0; i < PBR_CAT_COUNT - 1; i++ ) {
		if ( pbrCategoryInfos[i].cat == cat ) {
			return pbrCategoryInfos[i].token;
		}
	}
	return "none";
}

// DUDE per-category PBR defaults (docs/pbr-materials.md). Each category is a preset of
// {metalness, roughness, wetness, env}; a tagged material tracks its row live, a pinned
// (NONE) material carries its own explicit values. Seeded with the historical per-category
// cvar defaults so the look is unchanged until a slider moves; "@cat <name> m r w e" rows
// in the pbr config override these at load, and the Categories UI writes them back.
// wetness/env are multipliers (1 = neutral); env rides on top of the global r_pbrEnvScale.
static pbrCatDefaults_t pbrCatDefaults[PBR_CAT_COUNT] = {
	/* PBR_CAT_NONE            */ { 0.0f, 0.58f, 1.0f, 1.0f },	// unused (pinned materials carry their own)
	/* PBR_CAT_SKIN            */ { 0.0f, 0.40f, 1.0f, 1.0f },
	/* PBR_CAT_EYES            */ { 0.0f, 0.15f, 1.0f, 1.0f },
	/* PBR_CAT_FLESH           */ { 0.0f, 0.55f, 1.0f, 1.0f },
	/* PBR_CAT_SOLIDMETAL      */ { 0.8f, 0.32f, 1.0f, 1.0f },
	/* PBR_CAT_HOLLOWMETAL     */ { 0.2f, 0.55f, 1.0f, 1.0f },
	/* PBR_CAT_CERAMIC         */ { 0.2f, 0.45f, 1.0f, 1.0f },
	/* PBR_CAT_RUST            */ { 0.4f, 0.78f, 1.0f, 1.0f },
	/* PBR_CAT_CONCRETE        */ { 0.0f, 0.90f, 1.0f, 1.0f },
	/* PBR_CAT_ROCK            */ { 0.0f, 0.90f, 1.0f, 1.0f },
	/* PBR_CAT_DIRT            */ { 0.0f, 0.90f, 1.0f, 1.0f },
	/* PBR_CAT_SAND            */ { 0.0f, 0.90f, 1.0f, 1.0f },
	/* PBR_CAT_GRAVEL          */ { 0.0f, 0.90f, 1.0f, 1.0f },
	/* PBR_CAT_MONSTERMETAL    */ { 0.8f, 0.32f, 1.0f, 1.0f },
	/* PBR_CAT_ARMOR           */ { 0.1f, 0.50f, 1.0f, 1.0f },
	/* PBR_CAT_ELECTRONICS     */ { 0.2f, 0.40f, 1.0f, 1.0f },
	/* PBR_CAT_PLASTIC         */ { 0.0f, 0.45f, 1.0f, 1.0f },
	/* PBR_CAT_RUBBER          */ { 0.0f, 0.70f, 1.0f, 1.0f },
	/* PBR_CAT_GLASS           */ { 0.0f, 0.05f, 1.0f, 1.0f },
	/* PBR_CAT_BPGLASS         */ { 0.0f, 0.05f, 1.0f, 1.0f },
	/* PBR_CAT_WATER           */ { 0.0f, 0.15f, 1.0f, 1.0f },
	/* PBR_CAT_OIL             */ { 0.0f, 0.10f, 1.0f, 1.0f },
	/* PBR_CAT_GELATINOUSFLUID */ { 0.0f, 0.20f, 1.0f, 1.0f },
	/* PBR_CAT_WOOD            */ { 0.0f, 0.75f, 1.0f, 1.0f },
	/* PBR_CAT_CLOTH           */ { 0.0f, 0.85f, 1.0f, 1.0f },
	/* PBR_CAT_CARDBOARD       */ { 0.0f, 0.85f, 1.0f, 1.0f },
};

void R_PbrCategoryDefaults( int cat, float &metal, float &rough, float &wet, float &env ) {
	if ( cat <= PBR_CAT_NONE || cat >= PBR_CAT_COUNT ) {
		metal = 0.0f; rough = 0.58f; wet = 1.0f; env = 1.0f;
		return;
	}
	const pbrCatDefaults_t &d = pbrCatDefaults[cat];
	metal = d.metalness; rough = d.roughness; wet = d.wetness; env = d.env;
}

void R_PbrSetCategoryDefault( int cat, float metal, float rough, float wet, float env ) {
	if ( cat <= PBR_CAT_NONE || cat >= PBR_CAT_COUNT ) {
		return;
	}
	pbrCatDefaults_t &d = pbrCatDefaults[cat];
	d.metalness = idMath::ClampFloat( 0.0f, 1.0f, metal );
	d.roughness = idMath::ClampFloat( 0.03f, 1.0f, rough );
	d.wetness   = idMath::ClampFloat( 0.0f, 8.0f, wet );
	d.env       = idMath::ClampFloat( 0.0f, 4.0f, env );
}

// parse an already-loaded, NUL-terminated table buffer into pbrTable. 'label' is
// only used for the summary print.
static void PBR_ParseTableBuffer( const char *buf, const char *label ) {
	int count = 0;
	const char *p = buf;
	while ( *p ) {
		const char *lineStart = p;
		while ( *p && *p != '\n' && *p != '\r' ) {
			p++;
		}
		idStr line( lineStart, 0, (int)( p - lineStart ) );
		while ( *p == '\n' || *p == '\r' ) {
			p++;
		}
		int c = line.Find( '#' );
		if ( c >= 0 ) {
			line = line.Left( c );
		}
		// per-category defaults row: "@cat <name> <metal> <rough> <wet> <env>" — the
		// preset every material tagged with that category tracks live. Parsed into
		// pbrCatDefaults; later files win (dude-folder deltas over the shipped baseline).
		{
			char kw[16] = "", cname[64] = "";
			float cm = 0, cr = 0, cw = 0, ce = 0;
			if ( sscanf( line.c_str(), "%15s %63s %f %f %f %f", kw, cname, &cm, &cr, &cw, &ce ) == 6
			     && !idStr::Icmp( kw, "@cat" ) ) {
				if ( !idStr::Icmp( cname, "stone" ) ) {
					// pre-unification row: Doom 3's "stone" was split into these five, which
					// all start from its preset
					static const int split[] = { PBR_CAT_CONCRETE, PBR_CAT_ROCK, PBR_CAT_DIRT, PBR_CAT_SAND, PBR_CAT_GRAVEL };
					for ( int i = 0; i < (int)( sizeof( split ) / sizeof( split[0] ) ); i++ ) {
						R_PbrSetCategoryDefault( split[i], cm, cr, cw, ce );
					}
					continue;
				}
				const int cat = PBR_CategoryForName( cname );
				if ( cat != PBR_CAT_NONE ) {
					R_PbrSetCategoryDefault( cat, cm, cr, cw, ce );
				}
				continue;
			}
		}
		// name + up to 5 string columns: metal, rough, category, wet, env. Each is
		// tokenized as a string first so a '*' sentinel (inherit) parses uniformly.
		char name[256];
		char mTok[32] = "", rTok[32] = "", cTok[32] = "", wTok[32] = "", eTok[32] = "";
		int fields = sscanf( line.c_str(), "%255s %31s %31s %31s %31s %31s",
			name, mTok, rTok, cTok, wTok, eTok );
		if ( fields >= 3 ) {
			pbrTableEntry_t e;
			float m = PBR_ParseNum( mTok );
			float r = PBR_ParseNum( rTok );
			float w = PBR_ParseNum( wTok );
			float en = PBR_ParseNum( eTok );
			e.metalness = m < 0.0f ? -1.0f : idMath::ClampFloat( 0.0f, 1.0f, m );
			e.roughness = r < 0.0f ? -1.0f : idMath::ClampFloat( 0.03f, 1.0f, r );
			e.wetness   = w < 0.0f ? -1.0f : idMath::ClampFloat( 0.0f, 8.0f, w );
			e.env       = en < 0.0f ? -1.0f : idMath::ClampFloat( 0.0f, 4.0f, en );
			// a category (in either file) tracks the live per-category sliders;
			// "none"/absent leaves the explicit numbers to stand.
			e.category = PBR_CategoryForName( cTok );
			idStr key( name );
			key.ToLower();
			pbrTable.Set( key, e );		// Set overwrites, so later files win
			count++;
		}
	}
	common->Printf( "PBR table: %d entries from %s\n", count, label );
}

// loads a table file through the VFS (search-path order, pk4-aware). fs_savepath
// (the writable "dude folder") outranks fs_basepath, so a user's edited copy wins
// over the shipped baseline — which is exactly the divergence model: base ships the
// read-only defaults, the dude folder holds the user's deltas on top.
static void PBR_LoadTableFile( const char *path ) {
	char *buf = NULL;
	int len = fileSystem->ReadFile( path, (void **)&buf, NULL );
	if ( len <= 0 || !buf ) {
		return;
	}
	PBR_ParseTableBuffer( buf, path );
	fileSystem->FreeFile( buf );
}

static void PBR_EnsureTableLoaded( void ) {
	if ( pbrTableLoaded ) {
		return;
	}
	pbrTableLoaded = true;
	pbrTable.Clear();
	PBR_LoadTableFile( "pbr/pbr_materials.cfg" );	// shipped table
	PBR_LoadTableFile( "pbr/pbr_overrides.cfg" );	// baseline + dude-folder deltas (later wins)
}

// drops the cached table so the next lookup re-reads the files (reloadPbrTable)
void R_PbrTableInvalidate( void ) {
	pbrTableLoaded = false;
}

// invalidate the cache and re-apply the lookup to every parsed material; shared by
// the reloadPbrTable command and the material editor's save. Returns the count.
int R_PbrTableReloadApply( void ) {
	R_PbrTableInvalidate();
	int applied = 0;
	const int n = declManager->GetNumDecls( DECL_MATERIAL );
	for ( int i = 0; i < n; i++ ) {
		const idMaterial *m = declManager->MaterialByIndex( i, false );
		if ( m && m->IsValid() ) {
			const_cast<idMaterial *>( m )->ApplyPbrTable();
			applied++;
		}
	}
	return applied;
}

// first whitespace-delimited token of a table line, skipping a leading '#'
// (so commented UNCLASSIFIED entries match too and get promoted to active)
static idStr PBR_LineKey( const char *s ) {
	while ( *s == ' ' || *s == '\t' ) s++;
	if ( *s == '#' ) { s++; while ( *s == ' ' || *s == '\t' ) s++; }
	idStr key;
	while ( *s && *s != ' ' && *s != '\t' && *s != '\r' && *s != '\n' ) {
		key += *s++;
	}
	return key;
}

/*
=============
R_PbrWriteOverrideLine

Material-editor save (docs/pbr-materials.md): rewrite this material's line in
pbr/pbr_overrides.cfg in place — replacing an existing active OR commented entry,
or appending to a live-edits section if new — then re-apply the table live. Reads
and writes the fs_savepath copy (the writable "dude folder") explicitly, so the
edit lands as a delta on top of the shipped fs_basepath baseline. fs_savepath
outranks fs_basepath in the VFS, so the delta wins on the next load — the edit is
launch-method-independent. Under run.sh (fs_savepath == fs_basepath == the repo)
this resolves to the same file the developer ships. Fold accumulated deltas back
into the baseline with tools/pbr_make_overrides.py --bake. -1 in any numeric arg
writes '*' (inherit); a real category writes '*' for metalness/roughness so the
line tracks the sliders.
=============
*/
bool R_PbrWriteOverrideLine( const idMaterial *mat, float metal, float rough,
                             float wet, float env, int category ) {
	if ( !mat ) {
		return false;
	}
	idStr name = mat->GetName();

	idStr mTok, rTok, wTok, eTok;
	const bool pinned = ( category == PBR_CAT_NONE );
	mTok = ( pinned && metal >= 0.0f ) ? idStr( va( "%.2f", metal ) ) : idStr( "*" );
	rTok = ( pinned && rough >= 0.0f ) ? idStr( va( "%.2f", rough ) ) : idStr( "*" );
	wTok = ( wet  >= 0.0f ) ? idStr( va( "%.2f", wet ) )  : idStr( "*" );
	eTok = ( env  >= 0.0f ) ? idStr( va( "%.2f", env ) )  : idStr( "*" );
	idStr line = va( "%s %s %s %s %s %s", name.c_str(), mTok.c_str(), rTok.c_str(),
	                 R_PbrCategoryToken( category ), wTok.c_str(), eTok.c_str() );

	// copy out of the rotating static buffer RelativePathToOSPath returns
	idStr osPath = fileSystem->RelativePathToOSPath( "pbr/pbr_overrides.cfg", "fs_savepath" );

	// read current contents (explicit savepath, matching the write below)
	idStr content;
	idFile *rf = fileSystem->OpenExplicitFileRead( osPath.c_str() );
	if ( rf ) {
		int l = rf->Length();
		if ( l > 0 ) {
			char *tmp = (char *)Mem_Alloc( l + 1 );
			rf->Read( tmp, l );
			tmp[l] = '\0';
			content = tmp;
			Mem_Free( tmp );
		}
		fileSystem->CloseFile( rf );
	}

	// walk lines, replacing the material's entry in place (active or commented)
	idStr out;
	bool replaced = false;
	const char *p = content.c_str();
	while ( *p ) {
		const char *ls = p;
		while ( *p && *p != '\n' ) {
			p++;
		}
		idStr raw( ls, 0, (int)( p - ls ) );		// one line, no '\n'
		if ( *p == '\n' ) {
			p++;
		}
		if ( !replaced && PBR_LineKey( raw.c_str() ).Icmp( name ) == 0 ) {
			out += line;
			replaced = true;
		} else {
			out += raw;
		}
		out += "\n";
	}

	if ( !replaced ) {
		const char *hdr = "\n# ---- live edits (PBR material editor) ----\n";
		if ( content.Find( "live edits (PBR material editor)" ) < 0 ) {
			out += hdr;
		}
		out += line;
		out += "\n";
	}

	idFile *wf = fileSystem->OpenExplicitFileWrite( osPath.c_str() );
	if ( !wf ) {
		common->Warning( "PBR editor: could not open %s for writing", osPath.c_str() );
		return false;
	}
	wf->Write( out.c_str(), out.Length() );
	fileSystem->CloseFile( wf );
	common->Printf( "PBR editor: %s '%s' in %s\n",
	                replaced ? "updated" : "added", name.c_str(), osPath.c_str() );

	R_PbrTableReloadApply();
	return true;
}

/*
=============
R_PbrWriteCategoryDefaults

Persist the per-category defaults (the 8 "@cat <name> m r w e" rows) to the
fs_savepath copy of pbr/pbr_overrides.cfg — the same dude-folder delta file the
material editor writes. Drops any existing @cat rows and rewrites a fresh block,
so the file always holds exactly one row per category. Called by the Categories UI.
=============
*/
bool R_PbrWriteCategoryDefaults( void ) {
	idStr osPath = fileSystem->RelativePathToOSPath( "pbr/pbr_overrides.cfg", "fs_savepath" );

	idStr content;
	idFile *rf = fileSystem->OpenExplicitFileRead( osPath.c_str() );
	if ( rf ) {
		int l = rf->Length();
		if ( l > 0 ) {
			char *tmp = (char *)Mem_Alloc( l + 1 );
			rf->Read( tmp, l );
			tmp[l] = '\0';
			content = tmp;
			Mem_Free( tmp );
		}
		fileSystem->CloseFile( rf );
	}

	// copy through, dropping existing @cat rows and the old section header
	idStr out;
	const char *p = content.c_str();
	while ( *p ) {
		const char *ls = p;
		while ( *p && *p != '\n' ) {
			p++;
		}
		idStr raw( ls, 0, (int)( p - ls ) );
		if ( *p == '\n' ) {
			p++;
		}
		idStr trimmed = raw;
		trimmed.StripLeading( ' ' );
		trimmed.StripLeading( '\t' );
		if ( trimmed.Icmpn( "@cat ", 5 ) == 0 ||
		     trimmed.Find( "category defaults (PBR)" ) >= 0 ) {
			continue;		// drop; rewritten below
		}
		out += raw;
		out += "\n";
	}

	out += "\n# ---- category defaults (PBR) ----\n";
	for ( int cat = PBR_CAT_NONE + 1; cat < PBR_CAT_COUNT; cat++ ) {
		float m, r, w, e;
		R_PbrCategoryDefaults( cat, m, r, w, e );
		out += va( "@cat %s %.3f %.3f %.3f %.3f\n", R_PbrCategoryToken( cat ), m, r, w, e );
	}

	idFile *wf = fileSystem->OpenExplicitFileWrite( osPath.c_str() );
	if ( !wf ) {
		common->Warning( "PBR editor: could not open %s for writing", osPath.c_str() );
		return false;
	}
	wf->Write( out.c_str(), out.Length() );
	fileSystem->CloseFile( wf );
	common->Printf( "PBR editor: wrote category defaults to %s\n", osPath.c_str() );
	return true;
}

/*
=============
idMaterial::ApplyPbrTable
=============
*/
void idMaterial::ApplyPbrTable( void ) {
	PBR_EnsureTableLoaded();
	pbrMetalness = -1.0f;
	pbrRoughness = -1.0f;
	pbrWetness = -1.0f;
	pbrEnv = -1.0f;
	pbrCategory = PBR_CAT_NONE;
	pbrTableEntry_t *e;
	idStr key( GetName() );
	key.ToLower();
	if ( pbrTable.Get( key, &e ) ) {
		pbrMetalness = e->metalness;
		pbrRoughness = e->roughness;
		pbrWetness = e->wetness;
		pbrEnv = e->env;
		pbrCategory = e->category;
	}
}
