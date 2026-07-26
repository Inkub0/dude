/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.cpp for license header)
===========================================================================
*/

// DUDE GL 3.3 backend — GLSL program loader and cache (Phase 3 Chunk B/D).
//
// Source layout (see neo/shaders/README.md): each program is a
// shaders/<name>.vert + shaders/<name>.frag pair carrying no #version line;
// the loader prepends shaders/prelude.gl.glsl and expands #include "file"
// textually (GL drivers have no include support). Files come from the VFS
// first (moddable: base/shaders/, pk4s), falling back to the source tree
// (DUDE_SHADER_SOURCE_DIR) so editing + `reloadShaders` works in dev runs
// without an install step.
//
// Chunk D adds source-built programs: the Material IR feeds transpiled ARB
// shaders in as in-memory GLSL bodies (GL3_FindProgramFromSource), cached
// under "arb/<vp>+<fp>" names alongside the file-based ones.

#include "sys/platform.h"
#include "renderer/tr_local.h"
#include "framework/FileSystem.h"
#include "framework/CmdSystem.h"
#include "renderer/rhi/RHI.h"
#include "renderer/rhi/GL3Local.h"
#include "renderer/rhi/MaterialIR.h"

namespace rhi {

struct gl3Program_t {
	idStr		name;
	GLuint		object;		// linked GL program, 0 = failed to build (degraded)
	bool		fromSource;	// built from in-memory text (transpiled ARB)
	idStr		vertSrc;	// stored bodies for fromSource entries (rebuild/compare)
	idStr		fragSrc;
};

static idList<gl3Program_t>	gl3Programs;
static bool					gl3CmdRegistered = false;

// hand-translated boot set (neo/shaders/README.md "Translated" table);
// transpiled ARB programs register lazily under their own names later
static const char *gl3BootPrograms[] = {
	"ambientlight",
	"blendlight",
	"bloodorb",
	"bumpyenvironment",
	"colorprocess",
	"environment",
	"fog",
	"gammabrightness",
	"generic",
	"heathaze",
	"heathaze_mask",
	"heathaze_maskvertex",
	"interaction",
	"portalsky",
	"postprocess",
	"shadow",
	"softparticle",
	"ssao",
	"ssao_blur",
	"ssao_debug",
	"zfill",
};
static const int GL3_NUM_BOOT_PROGRAMS = sizeof( gl3BootPrograms ) / sizeof( gl3BootPrograms[0] );

/*
=============
GL3_ReadShaderFile

VFS first (mods can override), then the source tree for dev runs.
=============
*/
static bool GL3_ReadShaderFile( const char *fileName, idStr &out ) {
	void *buf = NULL;
	int len = fileSystem->ReadFile( va( "shaders/%s", fileName ), &buf, NULL );
	if ( len >= 0 && buf ) {
		out.Clear();
		out.Append( (const char *)buf, len );
		fileSystem->FreeFile( buf );
		return true;
	}

#ifdef DUDE_SHADER_SOURCE_DIR
	FILE *f = fopen( va( "%s/%s", DUDE_SHADER_SOURCE_DIR, fileName ), "rb" );
	if ( f ) {
		fseek( f, 0, SEEK_END );
		long size = ftell( f );
		fseek( f, 0, SEEK_SET );
		if ( size > 0 ) {
			char *text = (char *)malloc( size );
			if ( fread( text, 1, size, f ) == (size_t)size ) {
				out.Clear();
				out.Append( text, size );
				free( text );
				fclose( f );
				return true;
			}
			free( text );
		}
		fclose( f );
	}
#endif
	return false;
}

static bool GL3_ExpandFile( const char *fileName, idStr &out, idList<idStr> &files, int depth );

/*
=============
GL3_ExpandText

Expands #include "file" recursively in an in-memory body, emitting #line
directives so driver compile errors report (fileIndex:line) mappable through
the legend printed on failure.
=============
*/
static bool GL3_ExpandText( const idStr &text, const char *displayName, idStr &out, idList<idStr> &files, int depth ) {
	if ( depth > 8 ) {
		common->Warning( "GL3 shaders: include depth > 8 at '%s' (cycle?)", displayName );
		return false;
	}

	int fileIndex = files.Append( idStr( displayName ) );
	out += va( "#line 1 %d\n", fileIndex );

	int lineNum = 1;
	const char *p = text.c_str();
	while ( *p ) {
		// isolate this line
		const char *lineStart = p;
		while ( *p && *p != '\n' ) {
			p++;
		}
		int lineLen = p - lineStart;
		if ( *p == '\n' ) {
			p++;
		}

		// #include "file" ?
		const char *s = lineStart;
		while ( *s == ' ' || *s == '\t' ) {
			s++;
		}
		if ( idStr::Cmpn( s, "#include", 8 ) == 0 ) {
			const char *q1 = (const char *)memchr( s, '"', lineLen - ( s - lineStart ) );
			const char *q2 = q1 ? (const char *)memchr( q1 + 1, '"', lineLen - ( q1 + 1 - lineStart ) ) : NULL;
			if ( !q2 ) {
				common->Warning( "GL3 shaders: malformed #include in %s:%d", displayName, lineNum );
				return false;
			}
			idStr incName( q1 + 1, 0, q2 - q1 - 1 );
			if ( !GL3_ExpandFile( incName.c_str(), out, files, depth + 1 ) ) {
				return false;
			}
			// resume numbering in this file
			out += va( "#line %d %d\n", lineNum + 1, fileIndex );
		} else {
			out.Append( lineStart, lineLen );
			out.Append( '\n' );
		}
		lineNum++;
	}
	return true;
}

/*
=============
GL3_ExpandFile
=============
*/
static bool GL3_ExpandFile( const char *fileName, idStr &out, idList<idStr> &files, int depth ) {
	idStr text;
	if ( !GL3_ReadShaderFile( fileName, text ) ) {
		common->Warning( "GL3 shaders: couldn't read shaders/%s (VFS or source tree)", fileName );
		return false;
	}
	return GL3_ExpandText( text, fileName, out, files, depth );
}

/*
=============
GL3_PrintFileLegend

Driver logs reference sources as "fileIndex(line)" / "fileIndex:line".
=============
*/
static void GL3_PrintFileLegend( const idList<idStr> &files ) {
	for ( int i = 0; i < files.Num(); i++ ) {
		common->Printf( "    source %d = %s\n", i, files[i].c_str() );
	}
}

/*
=============
GL3_CompileStage
=============
*/
static GLuint GL3_CompileStage( GLenum type, const char *source, const char *stageFile, const idList<idStr> &files ) {
	GLuint shader = gl3CreateShader( type );
	gl3ShaderSource( shader, 1, &source, NULL );
	gl3CompileShader( shader );

	GLint status = 0;
	gl3GetShaderiv( shader, GL_COMPILE_STATUS, &status );
	if ( !status ) {
		GLint logLen = 0;
		gl3GetShaderiv( shader, GL_INFO_LOG_LENGTH, &logLen );
		idStr log;
		if ( logLen > 1 ) {
			log.Fill( ' ', logLen );
			gl3GetShaderInfoLog( shader, logLen, NULL, &log[0] );
		}
		common->Warning( "GL3 shaders: compile failed: %s\n%s", stageFile, log.c_str() );
		GL3_PrintFileLegend( files );
		gl3DeleteShader( shader );
		return 0;
	}
	return shader;
}

/*
=============
GL3_AssignSamplerUnits

The binding macro is a no-op for GL (see prelude.gl.glsl), so texture units
are assigned here by scanning the source for the declaration pattern
"SAMPLER_BINDING(n) uniform <type> <name>;" — single source of truth with
the Vulkan layout(binding=n) path.
=============
*/
static void GL3_AssignSamplerUnits( GLuint program, const char *source ) {
	const char *p = source;
	while ( ( p = strstr( p, "SAMPLER_BINDING(" ) ) != NULL ) {
		p += 16;	// strlen( "SAMPLER_BINDING(" )
		int unit = atoi( p );
		const char *close = strchr( p, ')' );
		if ( !close ) {
			return;
		}
		char type[64], name[64];
		if ( sscanf( close + 1, " uniform %63s %63s", type, name ) == 2 ) {
			// strip trailing ';'
			char *semi = strchr( name, ';' );
			if ( semi ) {
				*semi = '\0';
			}
			GLint loc = gl3GetUniformLocation( program, name );
			if ( loc >= 0 ) {
				gl3Uniform1i( loc, unit );
			}
		}
		p = close;
	}
}

/*
=============
GL3_BuildProgramObject

Full build: prelude + include expansion + compile + link + binding setup.
Stage bodies come from shaders/<name>.vert/.frag when vertBody/fragBody are
NULL, or from the given in-memory text (transpiled ARB) otherwise.
Returns the linked GL program object, 0 on any failure.
=============
*/
static GLuint GL3_BuildProgramObject( const char *name, const char *vertBody = NULL, const char *fragBody = NULL ) {
	idStr prelude;
	if ( !GL3_ReadShaderFile( "prelude.gl.glsl", prelude ) ) {
		common->Warning( "GL3 shaders: missing shaders/prelude.gl.glsl" );
		return 0;
	}

	// assemble and compile both stages
	GLuint stages[2] = { 0, 0 };
	static const GLenum stageType[2] = { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER };
	static const char *stageExt[2] = { "vert", "frag" };
	const char *stageBody[2] = { vertBody, fragBody };
	idStr stageSource[2];

	for ( int i = 0; i < 2; i++ ) {
		idStr stageFile = va( "%s.%s", name, stageExt[i] );
		idList<idStr> files;
		stageSource[i] = prelude;
		if ( i == 0 ) {
			// multi-pass rendering depends on every program producing bit-equal
			// depth (zfill prepass vs interaction passes use depthFunc EQUAL);
			// unlike ARB_position_invariant this isn't implicit in GLSL
			stageSource[i] += "invariant gl_Position;\n";
		}
		bool ok;
		if ( stageBody[i] ) {
			ok = GL3_ExpandText( idStr( stageBody[i] ), stageFile.c_str(), stageSource[i], files, 0 );
		} else {
			ok = GL3_ExpandFile( va( "%s.%s", name, stageExt[i] ), stageSource[i], files, 0 );
		}
		if ( ok ) {
			stages[i] = GL3_CompileStage( stageType[i], stageSource[i].c_str(), stageFile.c_str(), files );
		}
		if ( !stages[i] ) {
			if ( stages[0] && i == 1 ) {
				gl3DeleteShader( stages[0] );
			}
			return 0;
		}
	}

	GLuint program = gl3CreateProgram();
	gl3AttachShader( program, stages[0] );
	gl3AttachShader( program, stages[1] );
	gl3LinkProgram( program );

	// shaders are owned by the program now either way
	gl3DetachShader( program, stages[0] );
	gl3DetachShader( program, stages[1] );
	gl3DeleteShader( stages[0] );
	gl3DeleteShader( stages[1] );

	GLint status = 0;
	gl3GetProgramiv( program, GL_LINK_STATUS, &status );
	if ( !status ) {
		GLint logLen = 0;
		gl3GetProgramiv( program, GL_INFO_LOG_LENGTH, &logLen );
		idStr log;
		if ( logLen > 1 ) {
			log.Fill( ' ', logLen );
			gl3GetProgramInfoLog( program, logLen, NULL, &log[0] );
		}
		common->Warning( "GL3 shaders: link failed: %s\n%s", name, log.c_str() );
		gl3DeleteProgram( program );
		return 0;
	}

	// per-draw UBO: a program uses RenderParams (hand shaders) or ArbParams
	// (transpiled), never both — either binds to binding point 0
	GLuint blockIndex = gl3GetUniformBlockIndex( program, "RenderParams" );
	if ( blockIndex == GL_INVALID_INDEX ) {
		blockIndex = gl3GetUniformBlockIndex( program, "ArbParams" );
	}
	if ( blockIndex != GL_INVALID_INDEX ) {
		gl3UniformBlockBinding( program, blockIndex, 0 );
	}

	// sampler uniforms -> texture units
	gl3UseProgram( program );
	GL3_AssignSamplerUnits( program, stageSource[0].c_str() );
	GL3_AssignSamplerUnits( program, stageSource[1].c_str() );
	gl3UseProgram( 0 );

	return program;
}

/*
=============
GL3_FindProgram

Cache lookup, loading on demand. A failed build stays cached with object 0
so a broken shader warns once instead of once per frame; `reloadShaders`
retries it.
=============
*/
unsigned int GL3_FindProgram( const char *name ) {
	for ( int i = 0; i < gl3Programs.Num(); i++ ) {
		if ( gl3Programs[i].name.Icmp( name ) == 0 ) {
			return gl3Programs[i].object ? i + 1 : 0;
		}
	}

	gl3Program_t entry;
	entry.name = name;
	entry.fromSource = false;
	entry.object = GL3_BuildProgramObject( name );
	int index = gl3Programs.Append( entry );
	return gl3Programs[index].object ? index + 1 : 0;
}

/*
=============
GL3_FindProgramFromSource

Cache entry built from in-memory stage bodies (transpiled ARB programs).
If the entry exists but the bodies changed (reloadShaders re-transpiled a
modified .vfp), it is rebuilt in place — handles stay valid.
=============
*/
unsigned int GL3_FindProgramFromSource( const char *name, const char *vertBody, const char *fragBody ) {
	for ( int i = 0; i < gl3Programs.Num(); i++ ) {
		gl3Program_t &e = gl3Programs[i];
		if ( e.name.Icmp( name ) != 0 ) {
			continue;
		}
		if ( e.object && e.vertSrc.Cmp( vertBody ) == 0 && e.fragSrc.Cmp( fragBody ) == 0 ) {
			return i + 1;
		}
		// changed (or previously failed): rebuild in place
		GLuint object = GL3_BuildProgramObject( name, vertBody, fragBody );
		if ( object ) {
			if ( e.object ) {
				gl3DeleteProgram( e.object );
			}
			e.object = object;
			e.vertSrc = vertBody;
			e.fragSrc = fragBody;
		}
		return e.object ? i + 1 : 0;
	}

	gl3Program_t entry;
	entry.name = name;
	entry.fromSource = true;
	entry.vertSrc = vertBody;
	entry.fragSrc = fragBody;
	entry.object = GL3_BuildProgramObject( name, vertBody, fragBody );
	int index = gl3Programs.Append( entry );
	return gl3Programs[index].object ? index + 1 : 0;
}

/*
=============
GL3_ProgramObject
=============
*/
unsigned int GL3_ProgramObject( unsigned int handle ) {
	if ( handle < 1 || (int)handle > gl3Programs.Num() ) {
		return 0;
	}
	return gl3Programs[handle - 1].object;
}

/*
=============
R_ReloadGLSLPrograms_f

Rebuilds every cached program. File-based programs re-read from disk; a
program that no longer compiles keeps its previous object (degrade, don't
crash). Source-built (transpiled) programs are re-resolved lazily: purging
the Material IR makes the next frame re-transpile and rebuild any that
changed.
=============
*/
static void R_ReloadGLSLPrograms_f( const idCmdArgs &args ) {
	if ( !glConfig.coreProfile ) {
		common->Printf( "reloadShaders: GL3 backend not active (r_graphicsAPI is not opengl3)\n" );
		return;
	}

	int ok = 0, total = 0;
	for ( int i = 0; i < gl3Programs.Num(); i++ ) {
		if ( gl3Programs[i].fromSource ) {
			continue;	// re-resolved via IR purge below
		}
		total++;
		GLuint object = GL3_BuildProgramObject( gl3Programs[i].name.c_str() );
		if ( object ) {
			if ( gl3Programs[i].object ) {
				gl3DeleteProgram( gl3Programs[i].object );
			}
			gl3Programs[i].object = object;
			ok++;
		} else if ( gl3Programs[i].object ) {
			common->Printf( "  %s: rebuild failed, keeping previous program\n", gl3Programs[i].name.c_str() );
		}
	}
	IR_Purge();
	common->Printf( "reloadShaders: %d/%d programs rebuilt (transpiled ARB re-resolves lazily)\n", ok, total );
}

/*
=============
GL3_InitShaderCache

Called from GL3Backend::Init — including again after vid_restart, where the
old GL objects died with the old context (cleared, not deleted).
=============
*/
void GL3_InitShaderCache() {
	gl3Programs.Clear();
	IR_Purge();

	int ok = 0;
	for ( int i = 0; i < GL3_NUM_BOOT_PROGRAMS; i++ ) {
		if ( GL3_FindProgram( gl3BootPrograms[i] ) ) {
			ok++;
		}
	}
	common->Printf( "GL3 backend: %d/%d GLSL programs loaded\n", ok, GL3_NUM_BOOT_PROGRAMS );

	if ( !gl3CmdRegistered ) {
		gl3CmdRegistered = true;
		cmdSystem->AddCommand( "reloadShaders", R_ReloadGLSLPrograms_f, CMD_FL_RENDERER,
		                       "reloads GLSL shader programs (opengl3 backend)" );
	}
}

/*
=============
GL3_ShutdownShaderCache
=============
*/
void GL3_ShutdownShaderCache() {
	IR_Purge();
	for ( int i = 0; i < gl3Programs.Num(); i++ ) {
		if ( gl3Programs[i].object ) {
			gl3DeleteProgram( gl3Programs[i].object );
		}
	}
	gl3Programs.Clear();
}

} // namespace rhi
