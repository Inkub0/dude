/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.h for license header)
===========================================================================
*/

#ifndef __GL3LOCAL_H__
#define __GL3LOCAL_H__

// DUDE GL 3.3 core backend internals — shared between GL3Backend.cpp and
// GL3Shaders.cpp only. Not part of the public RHI.
//
// The legacy path loads its GL entry points in R_CheckPortableExtensions,
// which is skipped entirely on a core profile; the GL3 backend loads its own
// core-function pointers here (gl3* prefix, never touching the qgl* set).

#include "renderer/qgl.h"
#include "idlib/Str.h"

// constants that very old bundled GL headers may lack
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER					0x8892
#define GL_ELEMENT_ARRAY_BUFFER			0x8893
#endif
#ifndef GL_STREAM_DRAW
#define GL_STREAM_DRAW					0x88E0
#define GL_STATIC_DRAW					0x88E4
#define GL_DYNAMIC_DRAW					0x88E8
#endif
#ifndef GL_UNIFORM_BUFFER
#define GL_UNIFORM_BUFFER				0x8A11
#define GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT	0x8A34
#endif
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER				0x8B30
#define GL_VERTEX_SHADER				0x8B31
#endif
#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS				0x8B81
#define GL_LINK_STATUS					0x8B82
#define GL_INFO_LOG_LENGTH				0x8B84
#endif
#ifndef GL_INVALID_INDEX
#define GL_INVALID_INDEX				0xFFFFFFFFu
#endif

// every core entry point the backend uses beyond fixed GL 1.1 (qgl covers those).
// X-macro: GL3F( pointer-typedef, NameWithoutGlPrefix )
#define GL3_CORE_FUNCS \
	/* shaders / programs */ \
	GL3F( PFNGLCREATESHADERPROC,			CreateShader ) \
	GL3F( PFNGLSHADERSOURCEPROC,			ShaderSource ) \
	GL3F( PFNGLCOMPILESHADERPROC,			CompileShader ) \
	GL3F( PFNGLGETSHADERIVPROC,				GetShaderiv ) \
	GL3F( PFNGLGETSHADERINFOLOGPROC,		GetShaderInfoLog ) \
	GL3F( PFNGLDELETESHADERPROC,			DeleteShader ) \
	GL3F( PFNGLCREATEPROGRAMPROC,			CreateProgram ) \
	GL3F( PFNGLATTACHSHADERPROC,			AttachShader ) \
	GL3F( PFNGLDETACHSHADERPROC,			DetachShader ) \
	GL3F( PFNGLLINKPROGRAMPROC,				LinkProgram ) \
	GL3F( PFNGLGETPROGRAMIVPROC,			GetProgramiv ) \
	GL3F( PFNGLGETPROGRAMINFOLOGPROC,		GetProgramInfoLog ) \
	GL3F( PFNGLUSEPROGRAMPROC,				UseProgram ) \
	GL3F( PFNGLDELETEPROGRAMPROC,			DeleteProgram ) \
	GL3F( PFNGLGETUNIFORMLOCATIONPROC,		GetUniformLocation ) \
	GL3F( PFNGLUNIFORM1IPROC,				Uniform1i ) \
	GL3F( PFNGLGETUNIFORMBLOCKINDEXPROC,	GetUniformBlockIndex ) \
	GL3F( PFNGLUNIFORMBLOCKBINDINGPROC,		UniformBlockBinding ) \
	/* buffers */ \
	GL3F( PFNGLGENBUFFERSPROC,				GenBuffers ) \
	GL3F( PFNGLDELETEBUFFERSPROC,			DeleteBuffers ) \
	GL3F( PFNGLBINDBUFFERPROC,				BindBuffer ) \
	GL3F( PFNGLBUFFERDATAPROC,				BufferData ) \
	GL3F( PFNGLBUFFERSUBDATAPROC,			BufferSubData ) \
	GL3F( PFNGLBINDBUFFERRANGEPROC,			BindBufferRange ) \
	/* vertex arrays */ \
	GL3F( PFNGLGENVERTEXARRAYSPROC,			GenVertexArrays ) \
	GL3F( PFNGLDELETEVERTEXARRAYSPROC,		DeleteVertexArrays ) \
	GL3F( PFNGLBINDVERTEXARRAYPROC,			BindVertexArray ) \
	GL3F( PFNGLENABLEVERTEXATTRIBARRAYPROC,	EnableVertexAttribArray ) \
	GL3F( PFNGLVERTEXATTRIBPOINTERPROC,		VertexAttribPointer )

namespace rhi {

// pointer declarations (defined + loaded in GL3Backend.cpp)
#define GL3F( type, name ) extern type gl3##name;
GL3_CORE_FUNCS
#undef GL3F

// loads all GL3_CORE_FUNCS via GLimp_ExtensionPointer; on failure returns
// false with the missing names appended to `missing`
bool GL3_LoadCoreFunctions( idStr &missing );

// ---- GLSL program cache (GL3Shaders.cpp) ----
// Programs load shaders/<name>.vert + .frag as an inseparable pair, so
// hand-written and transpiled shaders (different varying conventions, see
// neo/shaders/README.md) can never cross-link by construction.
void			GL3_InitShaderCache();		// (re)load boot set; called from Init, incl. after vid_restart
void			GL3_ShutdownShaderCache();
unsigned int	GL3_FindProgram( const char *name );	// ShaderHandle; loads on demand, 0 = failed (degrade, don't crash)
unsigned int	GL3_ProgramObject( unsigned int handle );	// GL program object for a handle, 0 if failed

} // namespace rhi

#endif /* !__GL3LOCAL_H__ */
