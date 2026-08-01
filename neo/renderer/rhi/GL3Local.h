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
#ifndef GL_TEXTURE0
#define GL_TEXTURE0						0x84C0
#endif
// framebuffer objects + depth-texture / shadow-sampler state (shadow maps)
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER					0x8D40
#define GL_DEPTH_ATTACHMENT				0x8D00
#define GL_COLOR_ATTACHMENT0			0x8CE0
#define GL_FRAMEBUFFER_COMPLETE			0x8CD5
#endif
#ifndef GL_RGBA8
#define GL_RGBA8						0x8058
#endif
#ifndef GL_DEPTH_COMPONENT24
#define GL_DEPTH_COMPONENT24			0x81A6
#endif
// HDR scene target (RGBA16F color) + combined depth-stencil attachment
#ifndef GL_RGBA16F
#define GL_RGBA16F						0x881A
#endif
#ifndef GL_HALF_FLOAT
#define GL_HALF_FLOAT					0x140B
#endif
#ifndef GL_DEPTH24_STENCIL8
#define GL_DEPTH24_STENCIL8				0x88F0
#endif
#ifndef GL_DEPTH_STENCIL
#define GL_DEPTH_STENCIL				0x84F9
#endif
#ifndef GL_UNSIGNED_INT_24_8
#define GL_UNSIGNED_INT_24_8			0x84FA
#endif
#ifndef GL_DEPTH_STENCIL_ATTACHMENT
#define GL_DEPTH_STENCIL_ATTACHMENT		0x821A
#endif
#ifndef GL_TEXTURE_COMPARE_MODE
#define GL_TEXTURE_COMPARE_MODE			0x884C
#define GL_TEXTURE_COMPARE_FUNC			0x884D
#define GL_COMPARE_REF_TO_TEXTURE		0x884E
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE				0x812F
#endif
// cube-map depth targets (point-light shadow maps)
#ifndef GL_TEXTURE_CUBE_MAP
#define GL_TEXTURE_CUBE_MAP				0x8513
#define GL_TEXTURE_CUBE_MAP_POSITIVE_X	0x8515
#endif
#ifndef GL_TEXTURE_WRAP_R
#define GL_TEXTURE_WRAP_R				0x8072
#endif
#ifndef GL_TEXTURE_CUBE_MAP_SEAMLESS
#define GL_TEXTURE_CUBE_MAP_SEAMLESS	0x884F
#endif
#ifndef GL_CLAMP_TO_BORDER
#define GL_CLAMP_TO_BORDER				0x812D
#define GL_TEXTURE_BORDER_COLOR			0x1004
#endif
// vendor VRAM-size queries (used to auto-size the shadow cache budget)
#ifndef GL_GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX
#define GL_GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX			0x9047	// total dedicated VRAM, KB
#endif
#ifndef GL_TEXTURE_FREE_MEMORY_ATI
#define GL_TEXTURE_FREE_MEMORY_ATI						0x87FC	// [0] = total texture pool, KB
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
	GL3F( PFNGLMAPBUFFERRANGEPROC,			MapBufferRange ) \
	GL3F( PFNGLUNMAPBUFFERPROC,				UnmapBuffer ) \
	GL3F( PFNGLBINDBUFFERRANGEPROC,			BindBufferRange ) \
	/* textures */ \
	GL3F( PFNGLACTIVETEXTUREPROC,			ActiveTexture ) \
	/* vertex arrays */ \
	GL3F( PFNGLGENVERTEXARRAYSPROC,			GenVertexArrays ) \
	GL3F( PFNGLDELETEVERTEXARRAYSPROC,		DeleteVertexArrays ) \
	GL3F( PFNGLBINDVERTEXARRAYPROC,			BindVertexArray ) \
	GL3F( PFNGLENABLEVERTEXATTRIBARRAYPROC,	EnableVertexAttribArray ) \
	GL3F( PFNGLVERTEXATTRIBPOINTERPROC,		VertexAttribPointer ) \
	/* framebuffer objects (shadow maps / offscreen targets) */ \
	GL3F( PFNGLGENFRAMEBUFFERSPROC,			GenFramebuffers ) \
	GL3F( PFNGLDELETEFRAMEBUFFERSPROC,		DeleteFramebuffers ) \
	GL3F( PFNGLBINDFRAMEBUFFERPROC,			BindFramebuffer ) \
	GL3F( PFNGLFRAMEBUFFERTEXTURE2DPROC,	FramebufferTexture2D ) \
	GL3F( PFNGLCHECKFRAMEBUFFERSTATUSPROC,	CheckFramebufferStatus ) \
	GL3F( PFNGLDRAWBUFFERSPROC,				DrawBuffers )

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
unsigned int	GL3_FindProgramFromSource( const char *name, const char *vertBody, const char *fragBody );	// transpiled ARB (Material IR)
unsigned int	GL3_ProgramObject( unsigned int handle );	// GL program object for a handle, 0 if failed

} // namespace rhi

// ---- RHI command executor internals (RhiBackend.cpp / RhiWorld.cpp) ----
struct viewDef_s;
struct drawSurf_s;
struct srfTriangles_s;

void RB_RHI_LogOnce( const char *what );

// per-frame geometry streaming with dedup: the same surface streamed once
// per frame no matter how many passes reference it (depth fill, per-light
// interactions, shader passes)
void RB_RHI_StreamAmbient( rhi::RHI *r, const srfTriangles_s *tri, rhi::BufferHandle &vb, int &vertOfs, rhi::BufferHandle &ib, int &idxOfs );
void RB_RHI_StreamShadow( rhi::RHI *r, const srfTriangles_s *tri, rhi::BufferHandle &vb, int &vertOfs, rhi::BufferHandle &ib, int &idxOfs );

// MVP for a model space, including the weapon/model depth hack projection
// tweaks (the depth range part stays in RB_Enter/LeaveDepthHack)
void RB_RHI_SpaceMvp( const viewDef_s *viewDef, const struct viewEntity_s *space, float mvp[16] );

// material cull type adjusted for mirror views
int RB_RHI_CullFor( const viewDef_s *viewDef, int cullType );

// 3D world: depth prepass + stencil shadows + light interactions (RhiWorld.cpp)
void RB_RHI_DrawWorld( rhi::RHI *r, viewDef_s *viewDef );

// fog + blend lights: the last fixed-function passes, added after the ambient
// shader passes (RhiWorld.cpp), mirroring RB_STD_FogAllLights
void RB_RHI_FogAllLights( rhi::RHI *r, viewDef_s *viewDef );

// GTAO debug overlay (r_ssaoDebug): blit the AO buffer over the finished 3D view.
// Called at end-of-view so the scene passes don't overwrite it (RhiWorld.cpp).
void RB_RHI_SSAODebugOverlay( rhi::RHI *r, const viewDef_s *viewDef );

// DUDE screen-space reflections (docs/ssr.md): additive composite over the lit
// opaque scene. Called at the shader-pass translucent split point (RhiWorld.cpp);
// no-op unless r_ssr produced this view's MRT G-buffer.
void RB_RHI_ScreenSpaceReflections( rhi::RHI *r, const viewDef_s *viewDef );

// glass SSR (docs/ssr.md): true while _currentRender holds THIS view's
// split-point opaque scene snapshot (i.e. the SSR pass above completed for it),
// so cube-reflection stages may swap to the marching shader variants; plus the
// normal G-buffer image the march samples for its weapon-mask/backface rejects
bool RB_RHI_SsrSceneValid( const viewDef_s *viewDef );
rhi::ImageHandle RB_RHI_SsrNormalImage( rhi::RHI *r );

// screenshot support: composited desktops return garbage for front-buffer
// reads, so R_ReadTiledPixels registers a destination and the executor
// captures GL_BACK right before the next swap (GL_RGB, pack alignment 4)
void RB_RHI_CaptureNextSwap( unsigned char *dest );

// vid_restart / renderer teardown: forget the world backend's cached render-target
// handles (shadow/SSAO/SSR/normal) so they rebuild against the fresh GL context.
// Part of the strong shutdown orchestrated by RB_RHI_Shutdown (RhiBackend.cpp).
void RB_RHI_ResetWorldTargets( void );

#endif /* !__GL3LOCAL_H__ */
