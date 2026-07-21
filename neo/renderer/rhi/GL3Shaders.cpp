/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.cpp for license header)
===========================================================================
*/

// DUDE GL 3.3 backend — GLSL program loader and cache (Phase 3 Chunk B).
//
// Source layout (see neo/shaders/README.md): each program is a
// shaders/<name>.vert + shaders/<name>.frag pair carrying no #version line;
// the loader prepends shaders/prelude.gl.glsl and expands #include "file"
// textually (GL drivers have no include support). Files come from the VFS
// first (moddable: base/shaders/, pk4s), falling back to the source tree
// (DUDE_SHADER_SOURCE_DIR) so editing + `reloadShaders` works in dev runs
// without an install step.

#include "sys/platform.h"
#include "renderer/tr_local.h"
#include "framework/FileSystem.h"
#include "framework/CmdSystem.h"
#include "renderer/rhi/RHI.h"
#include "renderer/rhi/GL3Local.h"

namespace rhi {

struct gl3Program_t {
	idStr		name;
	GLuint		object;		// linked GL program, 0 = failed to build (degraded)
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
	"generic",
	"heathaze",
	"heathaze_mask",
	"heathaze_maskvertex",
	"interaction",
	"portalsky",
	"postprocess",
	"shadow",
	"softparticle",
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

/*
=============
GL3_ExpandSource

Expands #include "file" recursively, emitting #line directives so driver
compile errors report (fileIndex:line) mappable through the legend printed
on failure. `files` doubles as the include stack guard.
=============
*/
static bool GL3_ExpandSource( const char *fileName, idStr &out, idList<idStr> &files, int depth ) {
	if ( depth > 8 ) {
		common->Warning( "GL3 shaders: include depth > 8 at '%s' (cycle?)", fileName );
		return false;
	}

	idStr text;
	if ( !GL3_ReadShaderFile( fileName, text ) ) {
		common->Warning( "GL3 shaders: couldn't read shaders/%s (VFS or source tree)", fileName );
		return false;
	}

	int fileIndex = files.Append( idStr( fileName ) );
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
				common->Warning( "GL3 shaders: malformed #include in %s:%d", fileName, lineNum );
				return false;
			}
			idStr incName( q1 + 1, 0, q2 - q1 - 1 );
			if ( !GL3_ExpandSource( incName.c_str(), out, files, depth + 1 ) ) {
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
GL3_PrintFileLegend

Driver logs reference sources as "fileIndex(line)" / "fileIndex:line".
=============
*/
static void GL3_PrintFileLegend( const idList<idStr> &files ) {
	for ( int i = 0; i < files.Num(); i++ ) {
		common->Printf( "    source %d = shaders/%s\n", i, files[i].c_str() );
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
		common->Warning( "GL3 shaders: compile failed: shaders/%s\n%s", stageFile, log.c_str() );
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
Returns the linked GL program object, 0 on any failure.
=============
*/
static GLuint GL3_BuildProgramObject( const char *name ) {
	idStr prelude;
	if ( !GL3_ReadShaderFile( "prelude.gl.glsl", prelude ) ) {
		common->Warning( "GL3 shaders: missing shaders/prelude.gl.glsl" );
		return 0;
	}

	// assemble and compile both stages
	GLuint stages[2] = { 0, 0 };
	static const GLenum stageType[2] = { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER };
	static const char *stageExt[2] = { "vert", "frag" };
	idStr stageSource[2];

	for ( int i = 0; i < 2; i++ ) {
		idStr stageFile = va( "%s.%s", name, stageExt[i] );
		idList<idStr> files;
		stageSource[i] = prelude;
		if ( !GL3_ExpandSource( stageFile.c_str(), stageSource[i], files, 0 ) ) {
			if ( stages[0] ) {
				gl3DeleteShader( stages[0] );
			}
			return 0;
		}
		stages[i] = GL3_CompileStage( stageType[i], stageSource[i].c_str(), stageFile.c_str(), files );
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
	entry.object = GL3_BuildProgramObject( name );
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

Rebuilds every cached program from disk. A program that no longer compiles
keeps its previous object (degrade, don't crash — matches the ARB fallback
policy in neo/shaders/README.md).
=============
*/
static void R_ReloadGLSLPrograms_f( const idCmdArgs &args ) {
	if ( !glConfig.coreProfile ) {
		common->Printf( "reloadShaders: GL3 backend not active (r_graphicsAPI is not opengl3)\n" );
		return;
	}

	int ok = 0;
	for ( int i = 0; i < gl3Programs.Num(); i++ ) {
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
	common->Printf( "reloadShaders: %d/%d programs rebuilt\n", ok, gl3Programs.Num() );
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
	for ( int i = 0; i < gl3Programs.Num(); i++ ) {
		if ( gl3Programs[i].object ) {
			gl3DeleteProgram( gl3Programs[i].object );
		}
	}
	gl3Programs.Clear();
}

} // namespace rhi
