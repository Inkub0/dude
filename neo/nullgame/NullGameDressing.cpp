/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.h for license header)
===========================================================================
*/

// DUDE: null game map dressing - see NullGameDressing.h.

#include "sys/platform.h"
#include "idlib/Lexer.h"
#include "idlib/MapFile.h"
#include "idlib/geometry/JointTransform.h"
#include "framework/Common.h"
#include "framework/CVarSystem.h"
#include "framework/FileSystem.h"
#include "framework/DeclManager.h"
#include "framework/DeclEntityDef.h"
#include "renderer/Model.h"
#include "renderer/ModelManager.h"
#include "renderer/RenderWorld.h"
#include "ui/UserInterface.h"
#include "nullgame/NullGameDressing.h"

static idCVar nullgame_dressing( "nullgame_dressing", "1", CVAR_GAME | CVAR_BOOL,
	"null game: when a map loads, add its lights, models, MD5 models (frozen on their start "
	"animation) and entity GUIs, render-only; 0 = the world geometry alone" );

namespace {

const int MAX_INHERIT_DEPTH = 16;

// the parts of a model def ("model <name> { mesh ... anim ... }") the dressing needs
struct modelDef_t {
	idStr		mesh;
	idStr		skin;
	idVec3		offset;
	idDict		anims;			// anim name -> its first .md5anim file
};

struct counts_t {
	int			lights;
	int			models;
	int			posed;			// MD5 models on frame 0 of an animation
	int			bindPose;		// MD5 models with no usable animation
	int			heads;
	int			guis;
	int			missing;		// a model key that names nothing loadable
};

idList<qhandle_t>		entityHandles;
idList<qhandle_t>		lightHandles;
idList<idJointMat *>	jointFrames;	// the renderer reads them every frame they are drawn

/*
================
ParseModelDef

Mirrors the keys idDeclModelDef::Parse (game/anim/Anim_Blend.cpp) acts on; "inherit"
copies the parent at that point, later keys override, as there.
================
*/
bool ParseModelDef( const char *name, modelDef_t &out, int depth ) {
	if ( depth > MAX_INHERIT_DEPTH ) {
		common->Warning( "null game: model def '%s' inherits too deep", name );
		return false;
	}
	const idDecl *decl = declManager->FindType( DECL_MODELDEF, name, false );
	if ( decl == NULL ) {
		return false;
	}
	const int length = decl->GetTextLength();
	idList<char> text;
	text.SetNum( length + 1 );
	decl->GetText( text.Ptr() );

	idLexer src;
	src.LoadMemory( text.Ptr(), length, decl->GetFileName(), decl->GetLineNum() );
	src.SetFlags( DECL_LEXER_FLAGS | LEXFL_NOFATALERRORS );
	if ( !src.SkipUntilString( "{" ) ) {
		return false;
	}

	idToken token, value;
	while ( src.ReadToken( &token ) && token != "}" ) {
		if ( !token.Icmp( "inherit" ) ) {
			if ( src.ReadToken( &value ) ) {
				ParseModelDef( value, out, depth + 1 );
			}
		} else if ( !token.Icmp( "mesh" ) ) {
			if ( src.ReadToken( &value ) ) {
				out.mesh = value;
			}
		} else if ( !token.Icmp( "skin" ) ) {
			if ( src.ReadToken( &value ) ) {
				out.skin = value;
			}
		} else if ( !token.Icmp( "offset" ) ) {
			src.Parse1DMatrix( 3, out.offset.ToFloatPtr() );
		} else if ( !token.Icmp( "channel" ) ) {
			src.ReadToken( &value );					// channel name, then ( joint list )
			if ( src.CheckTokenString( "(" ) ) {
				src.SkipUntilString( ")" );
			}
		} else if ( !token.Icmp( "anim" ) ) {
			idToken animName;
			if ( !src.ReadToken( &animName ) || !src.ReadToken( &value ) ) {
				break;
			}
			out.anims.Set( animName, value );
			while ( src.CheckTokenString( "," ) ) {		// alternates: the first one is enough here
				src.ReadToken( &value );
			}
			if ( src.PeekTokenString( "{" ) ) {			// frame commands, flags
				src.SkipBracedSection();
			}
		} else if ( token == "{" ) {
			src.UnreadToken( &token );
			src.SkipBracedSection();
		}
		// anything else (Quake 4 additions included) is not needed to draw the model
	}
	return true;
}

/*
================
ParseAnimFrame0

Frame 0 of an .md5anim (the format idMD5Anim::LoadAnim reads), written over pose in the
model's joint order. pose holds the model's default pose on entry, parent-relative like
idRenderModel::GetDefaultPose; joints are matched by name, so a joint the animation does
not have keeps it.
================
*/
bool ParseAnimFrame0( idLexer &src, const idRenderModel *model, idJointQuat *pose, idBounds &bounds ) {
	idToken token;
	if ( !src.ExpectTokenString( "MD5Version" ) || src.ParseInt() != 10 ) {
		return false;
	}
	if ( !src.ExpectTokenString( "commandline" ) || !src.ReadToken( &token ) ) {
		return false;
	}
	if ( !src.ExpectTokenString( "numFrames" ) ) {
		return false;
	}
	const int numFrames = src.ParseInt();
	if ( !src.ExpectTokenString( "numJoints" ) ) {
		return false;
	}
	const int numJoints = src.ParseInt();
	if ( !src.ExpectTokenString( "frameRate" ) ) {
		return false;
	}
	src.ParseInt();
	if ( !src.ExpectTokenString( "numAnimatedComponents" ) ) {
		return false;
	}
	const int numComponents = src.ParseInt();
	if ( numFrames < 1 || numJoints < 1 || numComponents < 0 ) {
		return false;
	}

	struct animJoint_t {
		int			modelJoint;
		int			flags;
		int			firstComponent;
	};
	idList<animJoint_t> joints;
	joints.SetNum( numJoints );
	if ( !src.ExpectTokenString( "hierarchy" ) || !src.ExpectTokenString( "{" ) ) {
		return false;
	}
	for ( int i = 0; i < numJoints; i++ ) {
		if ( !src.ReadToken( &token ) ) {
			return false;
		}
		joints[i].modelJoint = model->GetJointHandle( token );
		src.ParseInt();								// parent: the model's hierarchy is used
		joints[i].flags = src.ParseInt();
		joints[i].firstComponent = src.ParseInt();
	}
	if ( !src.ExpectTokenString( "}" ) ) {
		return false;
	}

	if ( !src.ExpectTokenString( "bounds" ) || !src.ExpectTokenString( "{" ) ) {
		return false;
	}
	src.Parse1DMatrix( 3, bounds[0].ToFloatPtr() );
	src.Parse1DMatrix( 3, bounds[1].ToFloatPtr() );
	src.SkipUntilString( "}" );						// the other frames' bounds

	idList<idJointQuat> base;
	base.SetNum( numJoints );
	if ( !src.ExpectTokenString( "baseframe" ) || !src.ExpectTokenString( "{" ) ) {
		return false;
	}
	for ( int i = 0; i < numJoints; i++ ) {
		src.Parse1DMatrix( 3, base[i].t.ToFloatPtr() );
		src.Parse1DMatrix( 3, base[i].q.ToFloatPtr() );
	}
	if ( !src.ExpectTokenString( "}" ) ) {
		return false;
	}

	idList<float> components;
	components.SetNum( numComponents );
	if ( !src.ExpectTokenString( "frame" ) || src.ParseInt() != 0 || !src.ExpectTokenString( "{" ) ) {
		return false;
	}
	for ( int i = 0; i < numComponents; i++ ) {
		components[i] = src.ParseFloat();
	}
	if ( src.HadError() ) {
		return false;
	}

	const int numModelJoints = model->NumJoints();
	for ( int i = 0; i < numJoints; i++ ) {
		const animJoint_t &j = joints[i];
		if ( j.modelJoint < 0 || j.modelJoint >= numModelJoints ) {
			continue;
		}
		idJointQuat q = base[i];
		int c = j.firstComponent;
		float *values[6] = { &q.t.x, &q.t.y, &q.t.z, &q.q.x, &q.q.y, &q.q.z };
		for ( int k = 0; k < 6; k++ ) {
			if ( j.flags & ( 1 << k ) ) {
				if ( c < 0 || c >= numComponents ) {
					return false;
				}
				*values[k] = components[c++];
			}
		}
		q.q.w = q.q.CalcW();
		pose[j.modelJoint] = q;
	}
	return true;
}

bool LoadAnimFrame0( const char *fileName, const idRenderModel *model, idJointQuat *pose, idBounds &bounds ) {
	char *buffer = NULL;
	const int length = fileSystem->ReadFile( fileName, (void **)&buffer );
	if ( length <= 0 || buffer == NULL ) {
		return false;
	}
	idLexer src( LEXFL_ALLOWPATHNAMES | LEXFL_NOSTRINGESCAPECHARS | LEXFL_NOSTRINGCONCAT | LEXFL_NOFATALERRORS );
	src.LoadMemory( buffer, length, fileName );
	const bool ok = ParseAnimFrame0( src, model, pose, bounds );
	fileSystem->FreeFile( buffer );
	return ok;
}

/*
================
PoseModel

Model-space joint matrices for an MD5 model on frame 0 of animFile (or its bind pose when
animFile is empty or unusable), the way idGameEdit::ANIM_CreateAnimFrame builds them with
remove_origin_offset: the root sits at the model def's offset, children follow their
parents. Fills the render entity's joints and bounds.
================
*/
bool PoseModel( idRenderModel *model, const char *animFile, const idVec3 &offset, renderEntity_t &re ) {
	const int numJoints = model->NumJoints();
	const idMD5Joint *md5Joints = model->GetJoints();
	const idJointQuat *defaultPose = model->GetDefaultPose();
	if ( numJoints <= 0 || md5Joints == NULL || defaultPose == NULL ) {
		return false;
	}

	idList<idJointQuat> pose;
	pose.SetNum( numJoints );
	for ( int i = 0; i < numJoints; i++ ) {
		pose[i] = defaultPose[i];
	}
	idBounds animBounds;
	animBounds.Clear();
	bool posed = false;
	if ( animFile != NULL && animFile[0] != '\0' ) {
		posed = LoadAnimFrame0( animFile, model, pose.Ptr(), animBounds );
		if ( !posed ) {
			for ( int i = 0; i < numJoints; i++ ) {
				pose[i] = defaultPose[i];
			}
			animBounds.Clear();
		}
	}
	const idVec3 rootFrom = pose[0].t;

	idJointMat *mats = (idJointMat *)Mem_Alloc16( numJoints * sizeof( idJointMat ) );
	for ( int i = 0; i < numJoints; i++ ) {
		mats[i].SetRotation( pose[i].q.ToMat3() );
		mats[i].SetTranslation( pose[i].t );
	}
	mats[0].SetTranslation( offset );
	for ( int i = 1; i < numJoints; i++ ) {
		if ( md5Joints[i].parent != NULL ) {
			const int parent = md5Joints[i].parent - md5Joints;
			if ( parent >= 0 && parent < i ) {
				mats[i] *= mats[parent];
			}
		}
	}
	jointFrames.Append( mats );

	re.numJoints = numJoints;
	re.joints = mats;
	// the reference pose's bounds, grown by the frame's (moved with the root), with a margin:
	// a generous box only costs a little culling
	idBounds bounds = model->Bounds( NULL );
	if ( !animBounds.IsCleared() ) {
		animBounds.TranslateSelf( offset - rootFrom );
		bounds.AddBounds( animBounds );
	}
	bounds.ExpandSelf( 8.0f );
	re.bounds = bounds;
	return posed;
}

/*
================
AnimFileFor

The animation an entity shows when it spawns: its "anim" key (func_animate), else idle,
else the articulated figure's pose.
================
*/
const char *AnimFileFor( const idDict &args, const modelDef_t &def ) {
	const char *names[] = { args.GetString( "anim" ), "idle", "af_pose" };
	for ( int i = 0; i < 3; i++ ) {
		if ( names[i][0] != '\0' ) {
			const char *file = def.anims.GetString( names[i], "" );
			if ( file[0] != '\0' ) {
				return file;
			}
		}
	}
	return "";
}

/*
================
UpdateGuiParms / AddRenderGui

As in game/Entity.cpp.
================
*/
void UpdateGuiParms( idUserInterface *gui, const idDict &args, int gameTime ) {
	if ( gui == NULL ) {
		return;
	}
	const idKeyValue *kv = args.MatchPrefix( "gui_parm", NULL );
	while ( kv ) {
		gui->SetStateString( kv->GetKey(), kv->GetValue() );
		kv = args.MatchPrefix( "gui_parm", kv );
	}
	gui->SetStateBool( "noninteractive", args.GetBool( "gui_noninteractive" ) );
	gui->StateChanged( gameTime );
}

void AddRenderGui( const char *name, idUserInterface **gui, const idDict &args, int gameTime ) {
	const idKeyValue *kv = args.MatchPrefix( "gui_parm", NULL );
	*gui = uiManager->FindGui( name, true, ( kv != NULL ) );
	UpdateGuiParms( *gui, args, gameTime );
}

/*
================
ParseRenderEntityParms

The non-model half of idGameEdit::ParseSpawnArgsToRenderEntity: placement, skin, shader,
shader parms, shadow flags and GUIs.
================
*/
void ParseRenderEntityParms( const idDict &args, const modelDef_t *def, renderEntity_t &re, counts_t &counts, int gameTime ) {
	const char *temp = args.GetString( "skin" );
	if ( temp[0] != '\0' ) {
		re.customSkin = declManager->FindSkin( temp );
	} else if ( def != NULL && def->skin.Length() ) {
		re.customSkin = declManager->FindSkin( def->skin );
	}
	temp = args.GetString( "shader" );
	if ( temp[0] != '\0' ) {
		re.customShader = declManager->FindMaterial( temp );
	}

	args.GetVector( "origin", "0 0 0", re.origin );
	if ( !args.GetMatrix( "rotation", "1 0 0 0 1 0 0 0 1", re.axis ) ) {
		const float angle = args.GetFloat( "angle" );
		if ( angle != 0.0f ) {
			re.axis = idAngles( 0.0f, angle, 0.0f ).ToMat3();
		} else {
			re.axis.Identity();
		}
	}

	idVec3 color;
	args.GetVector( "_color", "1 1 1", color );
	re.shaderParms[ SHADERPARM_RED ]	= color[0];
	re.shaderParms[ SHADERPARM_GREEN ]	= color[1];
	re.shaderParms[ SHADERPARM_BLUE ]	= color[2];
	re.shaderParms[ 3 ] = args.GetFloat( "shaderParm3", "1" );
	for ( int i = 4; i < MAX_ENTITY_SHADER_PARMS; i++ ) {
		re.shaderParms[ i ] = args.GetFloat( va( "shaderParm%d", i ), "0" );
	}
	re.noDynamicInteractions = args.GetBool( "noDynamicInteractions" );
	re.noShadow = args.GetBool( "noshadows" );
	re.noSelfShadow = args.GetBool( "noselfshadows" );

	for ( int i = 0; i < MAX_RENDERENTITY_GUI; i++ ) {
		temp = args.GetString( i == 0 ? "gui" : va( "gui%d", i + 1 ) );
		if ( temp[0] != '\0' ) {
			AddRenderGui( temp, &re.gui[i], args, gameTime );
			if ( re.gui[i] != NULL ) {
				counts.guis++;
			}
		}
	}
}

/*
================
FindModel

What idGameEdit::ParseSpawnArgsToRenderEntity resolves a model key to: a model def's mesh,
else a model file or a .proc brush model of that name. NULL when nothing loads (instead of
the default model, which would draw a red box).
================
*/
idRenderModel *FindModel( const char *modelName, modelDef_t &def, bool &haveDef ) {
	haveDef = ParseModelDef( modelName, def, 0 );
	idRenderModel *model = NULL;
	if ( haveDef && def.mesh.Length() ) {
		model = renderModelManager->CheckModel( def.mesh );
	}
	if ( model == NULL ) {
		model = renderModelManager->CheckModel( modelName );
	}
	if ( model != NULL && model->IsDefaultModel() ) {
		model = NULL;
	}
	return model;
}

/*
================
AddModel

One render entity for an entity's model key; for an actor with "def_head", the head too,
placed on the body's head joint as idActor::SetupHead does.
================
*/
void AddModel( idRenderWorld *rw, int entityNum, const idDict &args, const idMaterial *referenceShader, counts_t &counts, int gameTime ) {
	const char *modelName = args.GetString( "model" );
	modelDef_t def;
	bool haveDef = false;
	idRenderModel *model = FindModel( modelName, def, haveDef );
	if ( model == NULL ) {
		counts.missing++;
		return;
	}

	renderEntity_t re;
	memset( &re, 0, sizeof( re ) );
	re.hModel = model;
	ParseRenderEntityParms( args, haveDef ? &def : NULL, re, counts, gameTime );
	re.referenceShader = referenceShader;
	re.entityNum = entityNum;

	if ( model->NumJoints() > 0 ) {
		// an actor's visual model sits at its "offsetModel" from the entity origin
		const idVec3 modelOffset = args.GetVector( "offsetModel" );
		re.origin += modelOffset * re.axis;
		if ( PoseModel( model, AnimFileFor( args, def ), def.offset, re ) ) {
			counts.posed++;
		} else if ( re.joints != NULL ) {
			counts.bindPose++;
		} else {
			counts.missing++;
			return;
		}
	} else {
		re.bounds = model->Bounds( &re );
	}
	entityHandles.Append( rw->AddEntityDef( &re ) );
	counts.models++;

	// the head: "def_head" names its model (idAFAttachment::SetBody -> SetModel), frozen
	// the same way and placed on the body's head joint at the body's axis
	const char *headModelName = args.GetString( "def_head" );
	if ( headModelName[0] == '\0' || re.joints == NULL ) {
		return;
	}
	const jointHandle_t joint = model->GetJointHandle( args.GetString( "head_joint" ) );
	if ( joint < 0 || joint >= re.numJoints ) {
		return;
	}
	modelDef_t headModelDef;
	bool haveHeadDef = false;
	idRenderModel *headModel = FindModel( headModelName, headModelDef, haveHeadDef );
	if ( headModel == NULL || headModel->NumJoints() <= 0 ) {
		counts.missing++;
		return;
	}
	// a fresh entity with only sound keys: default shader parms and shadows, the model
	// def's skin, its idle animation
	renderEntity_t head;
	memset( &head, 0, sizeof( head ) );
	head.hModel = headModel;
	head.entityNum = entityNum;
	if ( haveHeadDef && headModelDef.skin.Length() ) {
		head.customSkin = declManager->FindSkin( headModelDef.skin );
	}
	head.shaderParms[ SHADERPARM_RED ] = head.shaderParms[ SHADERPARM_GREEN ] = head.shaderParms[ SHADERPARM_BLUE ] = 1.0f;
	head.shaderParms[ 3 ] = 1.0f;
	// re.origin already carries offsetModel; the joints are model-space
	head.origin = re.origin + re.joints[joint].ToVec3() * re.axis;
	head.axis = re.axis;
	const idDict noArgs;
	PoseModel( headModel, AnimFileFor( noArgs, headModelDef ), headModelDef.offset, head );
	entityHandles.Append( rw->AddEntityDef( &head ) );
	counts.heads++;
}

/*
================
AddLight

idGameEdit::ParseSpawnArgsToRenderLight plus what idLight::Spawn adds for drawing (the
precomputed shadow model); returns the light's material for its model's referenceShader.
================
*/
const idMaterial *AddLight( idRenderWorld *rw, const idDict &args, counts_t &counts, int gameTime ) {
	renderLight_t rl;
	memset( &rl, 0, sizeof( rl ) );

	if ( !args.GetVector( "light_origin", "", rl.origin ) ) {
		args.GetVector( "origin", "", rl.origin );
	}
	const bool gotTarget = args.GetVector( "light_target", "", rl.target );
	const bool gotUp = args.GetVector( "light_up", "", rl.up );
	const bool gotRight = args.GetVector( "light_right", "", rl.right );
	args.GetVector( "light_start", "0 0 0", rl.start );
	if ( !args.GetVector( "light_end", "", rl.end ) ) {
		rl.end = rl.target;
	}
	if ( ( gotTarget || gotUp || gotRight ) != ( gotTarget && gotUp && gotRight ) ) {
		common->Printf( "null game: light at (%s) has bad target info\n", rl.origin.ToString( 0 ) );
		return NULL;
	}
	if ( !gotTarget ) {
		rl.pointLight = true;
		args.GetVector( "light_center", "0 0 0", rl.lightCenter );
		if ( !args.GetVector( "light_radius", "300 300 300", rl.lightRadius ) ) {
			float radius;
			args.GetFloat( "light", "300", radius );
			rl.lightRadius[0] = rl.lightRadius[1] = rl.lightRadius[2] = radius;
		}
	}

	idMat3 mat;
	if ( !args.GetMatrix( "light_rotation", "1 0 0 0 1 0 0 0 1", mat ) ) {
		if ( !args.GetMatrix( "rotation", "1 0 0 0 1 0 0 0 1", mat ) ) {
			idAngles angles( 0.0f, idMath::AngleNormalize360( args.GetFloat( "angle", "0" ) ), 0.0f );
			mat = angles.ToMat3();
		}
	}
	mat[0].FixDegenerateNormal();
	mat[1].FixDegenerateNormal();
	mat[2].FixDegenerateNormal();
	rl.axis = mat;

	idVec3 color;
	args.GetVector( "_color", "1 1 1", color );
	rl.shaderParms[ SHADERPARM_RED ]	= color[0];
	rl.shaderParms[ SHADERPARM_GREEN ]	= color[1];
	rl.shaderParms[ SHADERPARM_BLUE ]	= color[2];
	args.GetFloat( "shaderParm3", "1", rl.shaderParms[ SHADERPARM_TIMESCALE ] );
	if ( !args.GetFloat( "shaderParm4", "0", rl.shaderParms[ SHADERPARM_TIMEOFFSET ] ) ) {
		rl.shaderParms[ SHADERPARM_TIMEOFFSET ] = -MS2SEC( gameTime );
	}
	args.GetFloat( "shaderParm5", "0", rl.shaderParms[5] );
	args.GetFloat( "shaderParm6", "0", rl.shaderParms[6] );
	args.GetFloat( "shaderParm7", "0", rl.shaderParms[ SHADERPARM_MODE ] );
	args.GetBool( "noshadows", "0", rl.noShadows );
	args.GetBool( "nospecular", "0", rl.noSpecular );
	args.GetBool( "parallel", "0", rl.parallel );
	rl.shader = declManager->FindMaterial( args.GetString( "texture", "lights/squarelight1" ), false );

	const char *name = args.GetString( "name" );
	if ( name[0] != '\0' ) {
		rl.prelightModel = renderModelManager->CheckModel( va( "_prelight_%s", name ) );
	}

	lightHandles.Append( rw->AddLightDef( &rl ) );
	counts.lights++;
	return rl.shader;
}

}	// anonymous namespace

/*
================
NullGameDressing::Spawn
================
*/
void NullGameDressing::Spawn( const idMapFile &mapFile, idRenderWorld *rw, int gameTime ) {
	Clear( NULL );
	if ( rw == NULL || !nullgame_dressing.GetBool() || mapFile.GetNumEntities() == 0 ) {
		return;
	}

	counts_t counts;
	memset( &counts, 0, sizeof( counts ) );
	const bool inlineAllStatics = mapFile.GetEntity( 0 )->epairs.GetBool( "inlineAllStatics" );

	for ( int i = 1; i < mapFile.GetNumEntities(); i++ ) {		// 0 is worldspawn
		// the spawn args the game sees: the map's keys over the entity def's
		idDict args = mapFile.GetEntity( i )->epairs;
		const char *classname = args.GetString( "classname" );
		const idDeclEntityDef *def = static_cast<const idDeclEntityDef *>( declManager->FindType( DECL_ENTITYDEF, classname, false ) );
		if ( def != NULL ) {
			args.SetDefaults( &def->dict );
		}
		const char *spawnclass = args.GetString( "spawnclass" );

		// what does not show when it spawns: hidden until triggered (idEntity::Spawn), AI
		// waiting for a cinematic, a trigger or a teleport (idAI::Spawn), switched off,
		// triggers, statics dmap merged into the world (idStaticEntity::Spawn)
		if ( args.GetBool( "hide" ) || args.GetBool( "teleport" ) ) {
			continue;
		}
		if ( idStr::Icmpn( spawnclass, "idAI", 4 ) == 0 && ( args.GetInt( "num_cinematics" ) || args.GetBool( "trigger_anim" ) ) ) {
			continue;
		}
		if ( idStr::Icmpn( spawnclass, "idTrigger", 9 ) == 0 || idStr::Icmpn( classname, "trigger_", 8 ) == 0 ) {
			continue;
		}
		if ( idStr::Icmp( spawnclass, "idPlayerStart" ) == 0 || idStr::Icmpn( classname, "info_player_", 12 ) == 0 ) {
			continue;
		}
		if ( idStr::Icmp( spawnclass, "idStaticEntity" ) == 0 && ( args.GetBool( "inline" ) || inlineAllStatics ) ) {
			continue;
		}

		const idMaterial *lightShader = NULL;
		const bool isLight = idStr::Icmp( spawnclass, "idLight" ) == 0 || idStr::Icmp( classname, "light" ) == 0;
		if ( isLight && !args.GetBool( "start_off" ) ) {
			lightShader = AddLight( rw, args, counts, gameTime );
		}
		if ( !isLight && args.GetBool( "start_off" ) ) {		// emitters, smoke
			continue;
		}
		if ( args.GetString( "model" )[0] != '\0' ) {
			AddModel( rw, i, args, lightShader, counts, gameTime );
		}
	}

	common->Printf( "null game: dressed the map with %d lights, %d models (%d MD5 on an animation frame, %d in bind pose, %d heads), %d GUIs; %d model keys found nothing to load\n",
		counts.lights, counts.models + counts.heads, counts.posed, counts.bindPose, counts.heads, counts.guis, counts.missing );
}

/*
================
NullGameDressing::Clear
================
*/
void NullGameDressing::Clear( idRenderWorld *rw ) {
	if ( rw != NULL ) {
		for ( int i = 0; i < entityHandles.Num(); i++ ) {
			rw->FreeEntityDef( entityHandles[i] );
		}
		for ( int i = 0; i < lightHandles.Num(); i++ ) {
			rw->FreeLightDef( lightHandles[i] );
		}
	}
	entityHandles.Clear();
	lightHandles.Clear();
	for ( int i = 0; i < jointFrames.Num(); i++ ) {
		Mem_Free16( jointFrames[i] );
	}
	jointFrames.Clear();
}
