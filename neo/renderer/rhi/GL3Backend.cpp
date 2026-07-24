/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.cpp for license header)
===========================================================================
*/

// DUDE GL 3.3 core backend — Phase 3.
// Chunk C state: full 2D pipeline — programs, pipeline state (GLS_* bits),
// per-draw UBO/vertex/index rings, VAOs, indexed draws. The command
// executor translating idTech4 backend commands to RHI calls lives in
// RhiBackend.cpp. 3D views are not drawn yet (Chunk E).
// Selected with r_graphicsAPI opengl3; the legacy ARB path (r_graphicsAPI
// opengl) is untouched and remains the default until parity.

#include "sys/platform.h"
#include "renderer/tr_local.h"
#include "renderer/rhi/RHI.h"
#include "renderer/rhi/GL3Local.h"
#include "renderer/Model.h"		// shadowCache_t
#include "renderer/rhi/RenderParams.h"	// immediate-mode UBO (Chunk G)

namespace rhi {

// ---- core GL 3.3 function pointers (declared in GL3Local.h) ----
#define GL3F( type, name ) type gl3##name = NULL;
GL3_CORE_FUNCS
#undef GL3F

bool GL3_LoadCoreFunctions( idStr &missing ) {
	missing.Clear();
#define GL3F( type, name ) \
	gl3##name = (type)GLimp_ExtensionPointer( "gl" #name ); \
	if ( !gl3##name ) { missing.Append( " gl" #name ); }
	GL3_CORE_FUNCS
#undef GL3F
	return missing.IsEmpty();
}

// ARB_buffer_storage (GL 4.4): immutable, persistently-mappable buffers. Optional
// — loaded separately so a missing entry point just disables the persistent-ring
// fast path instead of failing backend init. gl3BufferStorage stays NULL then.
static PFNGLBUFFERSTORAGEPROC gl3BufferStorage = NULL;

// ARB_timer_query (core GL 3.3): GPU-side frame timing for r_gl3GpuTime. Loaded
// optionally like buffer_storage — a driver missing these just leaves the cvar inert.
static PFNGLGENQUERIESPROC			gl3GenQueries = NULL;
static PFNGLDELETEQUERIESPROC		gl3DeleteQueries = NULL;
static PFNGLBEGINQUERYPROC			gl3BeginQuery = NULL;
static PFNGLENDQUERYPROC			gl3EndQuery = NULL;
static PFNGLGETQUERYOBJECTIVPROC	gl3GetQueryObjectiv = NULL;
static PFNGLGETQUERYOBJECTUI64VPROC	gl3GetQueryObjectui64v = NULL;

static idCVar r_gl3GpuTime( "r_gl3GpuTime", "0", CVAR_RENDERER | CVAR_BOOL,
                            "opengl3 backend: print GPU frame time (ms), averaged once per second" );

class GL3Backend : public RHI {
	// ring sizes. Geometry now lives in the vertexCache's static VBOs, so the
	// vertex/index rings only carry small dynamic bits (fullscreen quads, the
	// virtual-memory fallback) and can be modest. The UBO ring takes one
	// RenderParams per draw and is persistently mapped (advances continuously
	// across frames, wrapping rarely), so it is sized for many frames of draws.
	static const int UBO_RING_SIZE  = 16 << 20;
	static const int VERT_RING_SIZE = 4 << 20;
	static const int IDX_RING_SIZE  = 2 << 20;

	struct ring_t {
		GLuint	buffer;
		int		offset;
		int		size;
		GLenum	target;
		byte *	persist;	// non-NULL: persistently-mapped, written via this ptr
	};

	bool			initialized;
	ring_t			uboRing, vertRing, idxRing;
	GLint			uboAlign;
	int				streamGen;
	GLuint			vaos[VL_COUNT];

	// immediate-mode debug drawing (Chunk G): own VAO/VBO/UBO, generic-program
	// vertex layout (imVert_t = xyz[3] st[2] rgba[4], 24 B)
	GLuint			imVao, imVbo, imUbo;
	static const int IM_STRIDE = 24;

	PipelineDesc	currentPipeline;
	int				currentStateBits;
	int				currentCull;
	bool			forceState;

	// vertex layout rebind cache (GL 3.3 VAOs capture pointer+buffer at
	// VertexAttribPointer time, so offsets are respecified when they change)
	int				boundLayout;
	GLuint			boundVBO;
	int				boundBase;
	unsigned int	boundProgram;	// skip redundant glUseProgram across a chain

	// GPU frame timing (r_gl3GpuTime) — a ring of GL_TIME_ELAPSED queries. Each
	// frame's result is read back a full ring later, by when it has always
	// finished, so the read never blocks the GPU.
	static const int GPU_TIMER_RING = 4;
	GLuint			gpuTimers[GPU_TIMER_RING];
	bool			gpuTimerBusy[GPU_TIMER_RING];
	int				gpuTimerFrame;
	bool			gpuTimerActive;		// a query is open this frame
	double			gpuTimeAccum;		// ms summed since the last print
	int				gpuTimeSamples;
	unsigned int	gpuTimeLastPrint;

	// offscreen render targets (shadow maps now; Phase 11 post stack later).
	// Handle is a 1-based index into this array (handle 0 == the backbuffer);
	// a slot with fbo == 0 is free. FBO/draw/read-buffer state is per-FBO in
	// GL 3.0+, so binding one never disturbs the backbuffer's state.
	static const int MAX_RENDER_TARGETS = 16;
	struct renderTarget_t {
		GLuint	fbo;
		GLuint	tex;
		int		w, h;
	};
	renderTarget_t		renderTargets[MAX_RENDER_TARGETS];
	RenderTargetHandle	activeTarget;		// 0 = backbuffer; set by BeginTargetPass
	GLint				savedViewport[4];	// restored by EndPass after a target pass

public:
	GL3Backend() : initialized( false ), uboAlign( 256 ), streamGen( 0 ) {
		uboRing.buffer = vertRing.buffer = idxRing.buffer = 0;
		uboRing.persist = vertRing.persist = idxRing.persist = NULL;
		vaos[0] = vaos[1] = 0;
		imVao = imVbo = imUbo = 0;
		memset( gpuTimers, 0, sizeof( gpuTimers ) );
		memset( gpuTimerBusy, 0, sizeof( gpuTimerBusy ) );
		gpuTimerFrame = 0;
		gpuTimerActive = false;
		gpuTimeAccum = 0.0;
		gpuTimeSamples = 0;
		gpuTimeLastPrint = 0;
		memset( renderTargets, 0, sizeof( renderTargets ) );
		activeTarget = 0;
		memset( savedViewport, 0, sizeof( savedViewport ) );
		InvalidateCaches();
	}

	// ---- lifecycle ----
	virtual bool Init() {
		idStr missing;
		if ( !GL3_LoadCoreFunctions( missing ) ) {
			common->Warning( "GL3 backend: missing core GL functions:%s", missing.c_str() );
			return false;
		}

		qglGetIntegerv( GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &uboAlign );
		if ( uboAlign < 4 ) {
			uboAlign = 256;		// spec maximum-minimum, aligns everywhere
		}

		// ARB_buffer_storage lets us persistently map the per-draw UBO ring:
		// write straight through a CPU pointer with zero map/unmap/subdata calls
		// per draw (only the cheap glBindBufferRange remains). This is the big
		// win on drivers that stall when a bound buffer is re-uploaded per draw.
		gl3BufferStorage = (PFNGLBUFFERSTORAGEPROC)GLimp_ExtensionPointer( "glBufferStorage" );

		// GPU timer queries (r_gl3GpuTime). All-or-nothing: if any entry point is
		// absent, timing stays off (gpuTimers left 0). Regenerated here so it also
		// re-arms after vid_restart, where the old query names died with the context.
		gl3GenQueries          = (PFNGLGENQUERIESPROC)GLimp_ExtensionPointer( "glGenQueries" );
		gl3DeleteQueries       = (PFNGLDELETEQUERIESPROC)GLimp_ExtensionPointer( "glDeleteQueries" );
		gl3BeginQuery          = (PFNGLBEGINQUERYPROC)GLimp_ExtensionPointer( "glBeginQuery" );
		gl3EndQuery            = (PFNGLENDQUERYPROC)GLimp_ExtensionPointer( "glEndQuery" );
		gl3GetQueryObjectiv    = (PFNGLGETQUERYOBJECTIVPROC)GLimp_ExtensionPointer( "glGetQueryObjectiv" );
		gl3GetQueryObjectui64v = (PFNGLGETQUERYOBJECTUI64VPROC)GLimp_ExtensionPointer( "glGetQueryObjectui64v" );
		memset( gpuTimers, 0, sizeof( gpuTimers ) );
		memset( gpuTimerBusy, 0, sizeof( gpuTimerBusy ) );
		gpuTimerActive = false;
		if ( gl3GenQueries && gl3DeleteQueries && gl3BeginQuery && gl3EndQuery
		     && gl3GetQueryObjectiv && gl3GetQueryObjectui64v ) {
			gl3GenQueries( GPU_TIMER_RING, gpuTimers );
		}

		InitRing( uboRing,  GL_UNIFORM_BUFFER,       UBO_RING_SIZE,  gl3BufferStorage != NULL );
		InitRing( vertRing, GL_ARRAY_BUFFER,         VERT_RING_SIZE, false );
		InitRing( idxRing,  GL_ELEMENT_ARRAY_BUFFER, IDX_RING_SIZE,  false );
		common->Printf( "GL3 backend: UBO ring is %s\n",
		                uboRing.persist ? "persistently mapped (fast)" : "orphaned per draw" );

		gl3GenVertexArrays( VL_COUNT, vaos );

		// attribute enable is VAO state: set it once here so the per-draw
		// BindVertexLayout only has to (re)specify the pointers when the bound
		// buffer/offset changes, not re-enable every array every draw
		gl3BindVertexArray( vaos[VL_DRAWVERT] );
		for ( int i = 0; i <= 5; i++ ) {
			gl3EnableVertexAttribArray( i );
		}
		gl3BindVertexArray( vaos[VL_SHADOW] );
		gl3EnableVertexAttribArray( 0 );

		// immediate-mode VAO/VBO/UBO (Chunk G): generic-program attribute
		// layout — pos @0, texcoord @1, color @5 — matching imVert_t
		gl3GenVertexArrays( 1, &imVao );
		gl3GenBuffers( 1, &imVbo );
		gl3GenBuffers( 1, &imUbo );
		gl3BindVertexArray( imVao );
		gl3BindBuffer( GL_ARRAY_BUFFER, imVbo );
		gl3EnableVertexAttribArray( 0 );
		gl3EnableVertexAttribArray( 1 );
		gl3EnableVertexAttribArray( 5 );
		gl3VertexAttribPointer( 0, 3, GL_FLOAT, GL_FALSE, IM_STRIDE, (const GLbyte *)NULL + 0 );
		gl3VertexAttribPointer( 1, 2, GL_FLOAT, GL_FALSE, IM_STRIDE, (const GLbyte *)NULL + 12 );
		gl3VertexAttribPointer( 5, 4, GL_UNSIGNED_BYTE, GL_TRUE, IM_STRIDE, (const GLbyte *)NULL + 20 );

		InvalidateCaches();

		GL3_InitShaderCache();

		initialized = true;
		common->Printf( "GL3 backend: rings %d/%d/%d KB (ubo/vert/idx, ubo align %d)\n",
		                UBO_RING_SIZE / 1024, VERT_RING_SIZE / 1024, IDX_RING_SIZE / 1024, uboAlign );
		return true;
	}

	virtual void Shutdown() {
		if ( !initialized ) {
			return;
		}
		GL3_ShutdownShaderCache();
		if ( gpuTimers[0] && gl3DeleteQueries ) {
			gl3DeleteQueries( GPU_TIMER_RING, gpuTimers );
		}
		memset( gpuTimers, 0, sizeof( gpuTimers ) );
		memset( gpuTimerBusy, 0, sizeof( gpuTimerBusy ) );
		gpuTimerActive = false;
		if ( uboRing.persist ) {
			gl3BindBuffer( GL_ARRAY_BUFFER, uboRing.buffer );
			gl3UnmapBuffer( GL_ARRAY_BUFFER );
			uboRing.persist = NULL;
		}
		gl3DeleteBuffers( 1, &uboRing.buffer );
		gl3DeleteBuffers( 1, &vertRing.buffer );
		gl3DeleteBuffers( 1, &idxRing.buffer );
		gl3DeleteVertexArrays( VL_COUNT, vaos );
		gl3DeleteVertexArrays( 1, &imVao );
		gl3DeleteBuffers( 1, &imVbo );
		gl3DeleteBuffers( 1, &imUbo );
		imVao = imVbo = imUbo = 0;
		// tear down any live offscreen targets (their GL names die with the
		// context on vid_restart; callers recreate them after re-init)
		for ( int i = 0; i < MAX_RENDER_TARGETS; i++ ) {
			if ( renderTargets[i].fbo ) {
				gl3DeleteFramebuffers( 1, &renderTargets[i].fbo );
			}
			if ( renderTargets[i].tex ) {
				qglDeleteTextures( 1, &renderTargets[i].tex );
			}
		}
		memset( renderTargets, 0, sizeof( renderTargets ) );
		activeTarget = 0;
		uboRing.buffer = vertRing.buffer = idxRing.buffer = 0;
		vaos[0] = vaos[1] = 0;
		initialized = false;
	}

	// ---- frame ----
	virtual void BeginFrame( int windowWidth, int windowHeight ) {
		qglViewport( 0, 0, windowWidth, windowHeight );

		// baseline state the pipeline bits build on (mirrors RB_SetDefaultGLState)
		qglEnable( GL_BLEND );
		qglEnable( GL_DEPTH_TEST );
		qglEnable( GL_SCISSOR_TEST );
		qglScissor( 0, 0, windowWidth, windowHeight );
		qglDepthMask( GL_TRUE );
		qglColorMask( 1, 1, 1, 1 );
		qglDisable( GL_STENCIL_TEST );
		qglDisable( GL_POLYGON_OFFSET_FILL );
		forceState = true;
		InvalidateCaches();

		if ( initialized ) {
			// orphan the mutable rings: the driver keeps last frame's storage
			// alive for in-flight draws while we write into fresh memory. The
			// persistent UBO ring is immutable (can't be orphaned) and instead
			// advances continuously, wrapping with a sync in AllocFromRing.
			OrphanRing( uboRing );
			OrphanRing( vertRing );
			OrphanRing( idxRing );
		}

		BeginGpuTimer();
	}
	virtual void EndFrame() {
		EndGpuTimer();
	}

	// shared clear: a masked write channel makes glClear a no-op on that buffer,
	// so force the write masks on first (legacy does GL_State(GLS_DEFAULT) before
	// its clear for this reason). Without it a preceding subview whose last draw
	// set GLS_DEPTHMASK (interactions/shadows/fog) leaves depthMask FALSE and the
	// next view's depth clear silently does nothing — the "world goes black around
	// a mirror" bug.
	void DoClear( const ClearArgs *clear ) {
		if ( !clear ) {
			return;
		}
		GLbitfield bits = 0;
		if ( clear->color ) {
			qglColorMask( 1, 1, 1, 1 );
			qglClearColor( clear->rgba[0], clear->rgba[1], clear->rgba[2], clear->rgba[3] );
			bits |= GL_COLOR_BUFFER_BIT;
		}
		if ( clear->depth ) {
			qglDepthMask( GL_TRUE );
			bits |= GL_DEPTH_BUFFER_BIT;
		}
		if ( clear->stencil ) {
			qglStencilMask( 0xff );
			qglClearStencil( clear->stencilValue );
			bits |= GL_STENCIL_BUFFER_BIT;
		}
		if ( bits ) {
			qglClear( bits );
			// write masks were forced for the clear; re-apply the bound
			// pipeline's masks on the next draw
			forceState = true;
		}
	}

	virtual void BeginPass( const ClearArgs *clear ) {
		DoClear( clear );
	}

	virtual void BeginTargetPass( RenderTargetHandle rt, const ClearArgs *clear ) {
		if ( rt == 0 || rt >= (RenderTargetHandle)MAX_RENDER_TARGETS || !renderTargets[rt].fbo ) {
			return;
		}
		const renderTarget_t &t = renderTargets[rt];
		// remember the backbuffer viewport so EndPass can put it back
		qglGetIntegerv( GL_VIEWPORT, savedViewport );
		gl3BindFramebuffer( GL_FRAMEBUFFER, t.fbo );
		activeTarget = rt;
		qglViewport( 0, 0, t.w, t.h );
		qglScissor( 0, 0, t.w, t.h );
		DoClear( clear );
	}

	virtual void EndPass() {
		if ( activeTarget ) {
			// return to the backbuffer and restore the view it had
			gl3BindFramebuffer( GL_FRAMEBUFFER, 0 );
			activeTarget = 0;
			qglViewport( savedViewport[0], savedViewport[1], savedViewport[2], savedViewport[3] );
			qglScissor( savedViewport[0], savedViewport[1], savedViewport[2], savedViewport[3] );
			forceState = true;
		}
	}
	virtual void SetViewport( int x, int y, int w, int h )	{ qglViewport( x, y, w, h ); }
	virtual void SetScissor( int x, int y, int w, int h )	{ qglScissor( x, y, w, h ); }

	// ---- resources ----
	// handles are the GL object names themselves (0 = invalid, matching GL)

	virtual BufferHandle CreateBuffer( BufferUsage usage, int size, const void *data ) {
		if ( !initialized ) {
			return 0;
		}
		GLuint b = 0;
		gl3GenBuffers( 1, &b );
		// upload through ARRAY_BUFFER regardless of usage: GL buffers are
		// untyped, and ELEMENT_ARRAY_BUFFER binding is VAO state we must not
		// disturb; the real target is chosen at draw time
		gl3BindBuffer( GL_ARRAY_BUFFER, b );
		gl3BufferData( GL_ARRAY_BUFFER, size, data,
		               usage == BU_UNIFORM ? GL_DYNAMIC_DRAW : ( data ? GL_STATIC_DRAW : GL_STREAM_DRAW ) );
		boundVBO = 0;
		return b;
	}

	virtual void UpdateBuffer( BufferHandle b, int offset, int size, const void *data ) {
		gl3BindBuffer( GL_ARRAY_BUFFER, b );
		gl3BufferSubData( GL_ARRAY_BUFFER, offset, size, data );
		boundVBO = 0;
	}

	virtual void DestroyBuffer( BufferHandle b ) {
		if ( b ) {
			GLuint name = b;
			gl3DeleteBuffers( 1, &name );
		}
	}

	virtual int AllocUniforms( const void *data, int size, BufferHandle *buffer ) {
		return AllocFromRing( uboRing, data, size, uboAlign, buffer );
	}
	virtual int AllocVertices( const void *data, int size, BufferHandle *buffer ) {
		int offset = AllocFromRing( vertRing, data, size, 4, buffer );
		boundVBO = 0;	// ring bind disturbed ARRAY_BUFFER
		return offset;
	}
	virtual int AllocIndices( const void *data, int size, BufferHandle *buffer ) {
		// note: ELEMENT_ARRAY binding is VAO state; ring uploads bind through
		// ARRAY_BUFFER to stay VAO-neutral
		return AllocFromRing( idxRing, data, size, 4, buffer );
	}

	virtual int StreamGeneration() {
		return streamGen;
	}

	virtual ImageHandle CreateImage( ImageFormat, int, int, const void * )	{ return 0; }	// engine images bridge via idImage until Phase 4
	virtual void DestroyImage( ImageHandle )								{}

	virtual ShaderHandle LoadShader( const char *name ) {
		return GL3_FindProgram( name );
	}

	// ---- offscreen render targets ----
	virtual RenderTargetHandle CreateRenderTarget( ImageFormat fmt, int w, int h ) {
		if ( !initialized || w <= 0 || h <= 0 ) {
			return 0;
		}
		// only depth-only targets so far (shadow maps); color targets arrive with
		// the post stack. Anything else is unsupported here.
		if ( fmt != IF_DEPTH24 ) {
			common->Warning( "GL3 CreateRenderTarget: only IF_DEPTH24 supported so far" );
			return 0;
		}
		int slot = -1;
		for ( int i = 1; i < MAX_RENDER_TARGETS; i++ ) {	// slot 0 == the backbuffer handle
			if ( renderTargets[i].fbo == 0 && renderTargets[i].tex == 0 ) {
				slot = i;
				break;
			}
		}
		if ( slot < 0 ) {
			common->Warning( "GL3 CreateRenderTarget: out of render-target slots" );
			return 0;
		}

		GLuint tex = 0;
		qglGenTextures( 1, &tex );
		gl3ActiveTexture( GL_TEXTURE0 );
		qglBindTexture( GL_TEXTURE_2D, tex );
		qglTexImage2D( GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, w, h, 0,
		               GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL );
		qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
		qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
		qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER );
		qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER );
		// outside the shadow frustum reads as depth 1.0 (farthest) → never in
		// shadow, so unmapped areas stay fully lit rather than black
		const GLfloat borderLit[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
		qglTexParameterfv( GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, borderLit );
		// hardware depth comparison so a sampler2DShadow returns 0..1 with 2×2 PCF
		qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE );
		qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL );

		GLuint fbo = 0;
		gl3GenFramebuffers( 1, &fbo );
		gl3BindFramebuffer( GL_FRAMEBUFFER, fbo );
		gl3FramebufferTexture2D( GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, tex, 0 );
		qglDrawBuffer( GL_NONE );	// depth-only: no color buffer to draw/read
		qglReadBuffer( GL_NONE );
		GLenum status = gl3CheckFramebufferStatus( GL_FRAMEBUFFER );
		gl3BindFramebuffer( GL_FRAMEBUFFER, 0 );
		qglBindTexture( GL_TEXTURE_2D, 0 );

		if ( status != GL_FRAMEBUFFER_COMPLETE ) {
			common->Warning( "GL3 CreateRenderTarget: incomplete FBO (0x%x), %dx%d", status, w, h );
			gl3DeleteFramebuffers( 1, &fbo );
			qglDeleteTextures( 1, &tex );
			return 0;
		}

		renderTargets[slot].fbo = fbo;
		renderTargets[slot].tex = tex;
		renderTargets[slot].w = w;
		renderTargets[slot].h = h;
		boundVBO = 0;	// binding the FBO's texture disturbed unit-0 bind tracking
		common->Printf( "GL3: created %dx%d depth render target (handle %d)\n", w, h, slot );
		return (RenderTargetHandle)slot;
	}

	virtual void DestroyRenderTarget( RenderTargetHandle rt ) {
		if ( rt == 0 || rt >= (RenderTargetHandle)MAX_RENDER_TARGETS ) {
			return;
		}
		if ( renderTargets[rt].fbo ) {
			gl3DeleteFramebuffers( 1, &renderTargets[rt].fbo );
		}
		if ( renderTargets[rt].tex ) {
			qglDeleteTextures( 1, &renderTargets[rt].tex );
		}
		memset( &renderTargets[rt], 0, sizeof( renderTargets[rt] ) );
	}

	virtual ImageHandle GetRenderTargetImage( RenderTargetHandle rt ) {
		if ( rt == 0 || rt >= (RenderTargetHandle)MAX_RENDER_TARGETS ) {
			return 0;
		}
		return (ImageHandle)renderTargets[rt].tex;	// GL texture name doubles as the ImageHandle
	}

	// ---- drawing ----
	virtual void BindPipeline( const PipelineDesc &desc ) {
		unsigned int program = GL3_ProgramObject( desc.shader );
		if ( program && program != boundProgram ) {
			gl3UseProgram( program );
			boundProgram = program;
		}
		ApplyState( desc.stateBits );
		ApplyCull( desc.cullType );
		currentPipeline = desc;
	}

	virtual void Draw( const DrawArgs &args ) {
		BindVertexLayout( currentPipeline.vertexLayout, args.vertexBuffer, args.vertexOffset );
		gl3BindBuffer( GL_ELEMENT_ARRAY_BUFFER, args.indexBuffer );

		if ( args.uniformBuffer ) {
			gl3BindBufferRange( GL_UNIFORM_BUFFER, 0, args.uniformBuffer,
			                    args.uniformOffset, args.uniformSize );
		}

		// engine textures currently bind through idImage (RhiBackend.cpp);
		// handles here cover RHI-created images (assumed 2D until Phase 4)
		for ( int i = 0; i < 8; i++ ) {
			if ( args.textures[i] ) {
				gl3ActiveTexture( GL_TEXTURE0 + i );
				qglBindTexture( GL_TEXTURE_2D, args.textures[i] );
			}
		}

		qglDrawElements( GL_TRIANGLES, args.indexCount, GL_UNSIGNED_INT,
		                 (const GLvoid *)( (const GLbyte *)NULL + args.firstIndex * sizeof( unsigned int ) ) );
	}

	virtual void CopyFramebufferToImage( ImageHandle, int, int )			{}	// Chunk F

	virtual void DrawImmediate( const void *verts, int numVerts, unsigned int primMode,
	                            const float mvp[16], bool textured ) {
		if ( numVerts <= 0 || !imVao ) {
			return;
		}
		unsigned int prog = GL3_ProgramObject( GL3_FindProgram( "generic" ) );
		if ( !prog ) {
			return;
		}

		gl3BindVertexArray( imVao );
		gl3BindBuffer( GL_ARRAY_BUFFER, imVbo );
		gl3BufferData( GL_ARRAY_BUFFER, numVerts * IM_STRIDE, verts, GL_STREAM_DRAW );

		// generic-program uniforms: identity texture matrix, straight per-vertex
		// color (modulate 1 / add 0 / color 1), alpha test off
		RenderParams p;
		memset( &p, 0, sizeof( p ) );
		memcpy( p.mvpMatrix, mvp, sizeof( p.mvpMatrix ) );
		p.diffuseMatrixS[0] = 1.0f;
		p.diffuseMatrixT[1] = 1.0f;
		p.vertexColorModulate[0] = p.vertexColorModulate[1] = p.vertexColorModulate[2] = p.vertexColorModulate[3] = 1.0f;
		p.color[0] = p.color[1] = p.color[2] = p.color[3] = 1.0f;

		gl3BindBuffer( GL_UNIFORM_BUFFER, imUbo );
		gl3BufferData( GL_UNIFORM_BUFFER, sizeof( p ), &p, GL_STREAM_DRAW );
		gl3BindBufferRange( GL_UNIFORM_BUFFER, 0, imUbo, 0, sizeof( p ) );

		gl3UseProgram( prog );

		gl3ActiveTexture( GL_TEXTURE0 );
		backEnd.glState.currenttmu = 0;
		if ( !textured ) {
			globalImages->whiteImage->Bind();		// untextured lines/points/tris
		}

		qglDrawArrays( primMode, 0, numVerts );

		// bypassed the pipeline machinery (own VAO/program/UBO); force the next
		// real Draw to re-bind everything cleanly
		InvalidateCaches();
	}

private:
	// Opens this frame's GL_TIME_ELAPSED query and, before reusing the slot,
	// reads back the result it held from GPU_TIMER_RING frames ago (always
	// finished by now, so no GPU stall). No-op unless r_gl3GpuTime and the
	// query entry points are both available.
	void BeginGpuTimer() {
		gpuTimerActive = false;
		if ( !gpuTimers[0] || !r_gl3GpuTime.GetBool() ) {
			return;
		}
		int slot = gpuTimerFrame % GPU_TIMER_RING;
		if ( gpuTimerBusy[slot] ) {
			GLint available = 0;
			gl3GetQueryObjectiv( gpuTimers[slot], GL_QUERY_RESULT_AVAILABLE, &available );
			if ( available ) {
				GLuint64 ns = 0;
				gl3GetQueryObjectui64v( gpuTimers[slot], GL_QUERY_RESULT, &ns );
				AccumGpuTime( (double)ns / 1000000.0 );
			}
			gpuTimerBusy[slot] = false;
		}
		gl3BeginQuery( GL_TIME_ELAPSED, gpuTimers[slot] );
		gpuTimerActive = true;
	}

	void EndGpuTimer() {
		if ( !gpuTimerActive ) {
			return;
		}
		gl3EndQuery( GL_TIME_ELAPSED );
		gpuTimerBusy[gpuTimerFrame % GPU_TIMER_RING] = true;
		gpuTimerFrame++;
		gpuTimerActive = false;
	}

	void AccumGpuTime( double ms ) {
		gpuTimeAccum += ms;
		gpuTimeSamples++;
		unsigned int now = Sys_Milliseconds();
		if ( now - gpuTimeLastPrint >= 1000 ) {
			common->Printf( "GL3 GPU: %.2f ms (%d samples)\n",
			                gpuTimeAccum / gpuTimeSamples, gpuTimeSamples );
			gpuTimeAccum = 0.0;
			gpuTimeSamples = 0;
			gpuTimeLastPrint = now;
		}
	}

	void InitRing( ring_t &ring, GLenum target, int size, bool persistent ) {
		ring.target = target;
		ring.size = size;
		ring.offset = 0;
		ring.persist = NULL;
		gl3GenBuffers( 1, &ring.buffer );
		// allocate via ARRAY_BUFFER (VAO-neutral, buffers are untyped)
		gl3BindBuffer( GL_ARRAY_BUFFER, ring.buffer );

		if ( persistent && gl3BufferStorage ) {
			const GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
			gl3BufferStorage( GL_ARRAY_BUFFER, size, NULL, flags );
			ring.persist = (byte *)gl3MapBufferRange( GL_ARRAY_BUFFER, 0, size, flags );
			if ( ring.persist ) {
				return;		// mapped for the buffer's lifetime
			}
			// immutable store made but mapping failed: it can't be reused as a
			// mutable buffer, so replace it with a fresh mutable one
			gl3DeleteBuffers( 1, &ring.buffer );
			gl3GenBuffers( 1, &ring.buffer );
			gl3BindBuffer( GL_ARRAY_BUFFER, ring.buffer );
		}
		gl3BufferData( GL_ARRAY_BUFFER, size, NULL, GL_DYNAMIC_DRAW );
	}

	void OrphanRing( ring_t &ring ) {
		if ( ring.persist ) {
			return;		// immutable + persistently mapped: never orphaned
		}
		gl3BindBuffer( GL_ARRAY_BUFFER, ring.buffer );
		gl3BufferData( GL_ARRAY_BUFFER, ring.size, NULL, GL_DYNAMIC_DRAW );
		ring.offset = 0;
		streamGen++;
	}

	int AllocFromRing( ring_t &ring, const void *data, int size, int align, BufferHandle *buffer ) {
		int offset = ( ring.offset + align - 1 ) & ~( align - 1 );

		if ( ring.persist ) {
			// persistently mapped: write straight through the CPU pointer, no GL
			// calls at all (the coherent mapping needs no flush, and only the
			// draw's glBindBufferRange references it). Advances continuously
			// across frames; on wrap, sync once — rare, since the ring holds many
			// frames of draws — so we can't stomp data a draw is still reading.
			if ( offset + size > ring.size ) {
				qglFinish();
				offset = 0;
			}
			memcpy( ring.persist + offset, data, size );
			ring.offset = offset + size;
			*buffer = ring.buffer;
			return offset;
		}

		gl3BindBuffer( GL_ARRAY_BUFFER, ring.buffer );
		if ( offset + size > ring.size ) {
			// mid-frame wrap: orphan again, earlier draws keep the old storage
			gl3BufferData( GL_ARRAY_BUFFER, ring.size, NULL, GL_DYNAMIC_DRAW );
			offset = 0;
		}
		// Write with an UNSYNCHRONIZED map instead of glBufferSubData. The ring
		// only ever advances (and orphans each frame in BeginFrame), so we never
		// touch a region a draw is still reading — but the driver can't know
		// that, and glBufferSubData on a buffer that's bound for drawing makes
		// NVIDIA stall until the in-flight draw completes, serializing CPU/GPU
		// on every draw. INVALIDATE_RANGE|UNSYNCHRONIZED tells it not to wait.
		void *dst = gl3MapBufferRange( GL_ARRAY_BUFFER, offset, size,
		                               GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_UNSYNCHRONIZED_BIT );
		if ( dst ) {
			memcpy( dst, data, size );
			gl3UnmapBuffer( GL_ARRAY_BUFFER );
		} else {
			gl3BufferSubData( GL_ARRAY_BUFFER, offset, size, data );	// map failed; safe fallback
		}
		ring.offset = offset + size;
		*buffer = ring.buffer;
		return offset;
	}

	// GLS_* translation, modeled on GL_State() minus the fixed-function
	// alpha test (GLS_ATEST_* is handled in-shader via u_alphaTest)
	void ApplyState( int stateBits ) {
		int diff = forceState ? -1 : ( stateBits ^ currentStateBits );
		forceState = false;
		if ( !diff ) {
			return;
		}

		if ( diff & ( GLS_DEPTHFUNC_EQUAL | GLS_DEPTHFUNC_LESS | GLS_DEPTHFUNC_ALWAYS ) ) {
			if ( stateBits & GLS_DEPTHFUNC_EQUAL ) {
				qglDepthFunc( GL_EQUAL );
			} else if ( stateBits & GLS_DEPTHFUNC_ALWAYS ) {
				qglDepthFunc( GL_ALWAYS );
			} else {
				qglDepthFunc( GL_LEQUAL );
			}
		}

		if ( diff & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS ) ) {
			GLenum srcFactor, dstFactor;
			switch ( stateBits & GLS_SRCBLEND_BITS ) {
			case GLS_SRCBLEND_ZERO:					srcFactor = GL_ZERO; break;
			case GLS_SRCBLEND_ONE:					srcFactor = GL_ONE; break;
			case GLS_SRCBLEND_DST_COLOR:			srcFactor = GL_DST_COLOR; break;
			case GLS_SRCBLEND_ONE_MINUS_DST_COLOR:	srcFactor = GL_ONE_MINUS_DST_COLOR; break;
			case GLS_SRCBLEND_SRC_ALPHA:			srcFactor = GL_SRC_ALPHA; break;
			case GLS_SRCBLEND_ONE_MINUS_SRC_ALPHA:	srcFactor = GL_ONE_MINUS_SRC_ALPHA; break;
			case GLS_SRCBLEND_DST_ALPHA:			srcFactor = GL_DST_ALPHA; break;
			case GLS_SRCBLEND_ONE_MINUS_DST_ALPHA:	srcFactor = GL_ONE_MINUS_DST_ALPHA; break;
			case GLS_SRCBLEND_ALPHA_SATURATE:		srcFactor = GL_SRC_ALPHA_SATURATE; break;
			default:								srcFactor = GL_ONE; break;
			}
			switch ( stateBits & GLS_DSTBLEND_BITS ) {
			case GLS_DSTBLEND_ZERO:					dstFactor = GL_ZERO; break;
			case GLS_DSTBLEND_ONE:					dstFactor = GL_ONE; break;
			case GLS_DSTBLEND_SRC_COLOR:			dstFactor = GL_SRC_COLOR; break;
			case GLS_DSTBLEND_ONE_MINUS_SRC_COLOR:	dstFactor = GL_ONE_MINUS_SRC_COLOR; break;
			case GLS_DSTBLEND_SRC_ALPHA:			dstFactor = GL_SRC_ALPHA; break;
			case GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA:	dstFactor = GL_ONE_MINUS_SRC_ALPHA; break;
			case GLS_DSTBLEND_DST_ALPHA:			dstFactor = GL_DST_ALPHA; break;
			case GLS_DSTBLEND_ONE_MINUS_DST_ALPHA:	dstFactor = GL_ONE_MINUS_DST_ALPHA; break;
			default:								dstFactor = GL_ONE; break;
			}
			qglBlendFunc( srcFactor, dstFactor );
		}

		if ( diff & GLS_DEPTHMASK ) {
			qglDepthMask( ( stateBits & GLS_DEPTHMASK ) ? GL_FALSE : GL_TRUE );
		}

		if ( diff & ( GLS_REDMASK | GLS_GREENMASK | GLS_BLUEMASK | GLS_ALPHAMASK ) ) {
			qglColorMask( ( stateBits & GLS_REDMASK )   ? 0 : 1,
			              ( stateBits & GLS_GREENMASK ) ? 0 : 1,
			              ( stateBits & GLS_BLUEMASK )  ? 0 : 1,
			              ( stateBits & GLS_ALPHAMASK ) ? 0 : 1 );
		}

		if ( diff & GLS_POLYMODE_LINE ) {
			qglPolygonMode( GL_FRONT_AND_BACK,
			                ( stateBits & GLS_POLYMODE_LINE ) ? GL_LINE : GL_FILL );
		}

		currentStateBits = stateBits;
	}

	void ApplyCull( int cullType ) {
		if ( cullType == currentCull ) {
			return;
		}
		if ( cullType == CT_TWO_SIDED ) {
			qglDisable( GL_CULL_FACE );
		} else {
			if ( currentCull == CT_TWO_SIDED || currentCull == -1 ) {
				qglEnable( GL_CULL_FACE );
			}
			// idTech4 winds triangles clockwise, so front-sided culls GL_FRONT
			// (matches legacy GL_Cull exactly — getting this backwards culls
			// every GUI quad in the engine)
			// TODO Chunk E: mirror views flip this (backEnd.viewDef->isMirror)
			qglCullFace( cullType == CT_BACK_SIDED ? GL_BACK : GL_FRONT );
		}
		currentCull = cullType;
	}

	// binds the VAO for `layout` with attribute pointers into vbo at
	// baseOffset (cached to skip redundant respecifies)
	void BindVertexLayout( VertexLayout layout, GLuint vbo, int baseOffset ) {
		if ( (int)layout == boundLayout && vbo == boundVBO && baseOffset == boundBase ) {
			return;
		}
		gl3BindVertexArray( vaos[layout] );
		gl3BindBuffer( GL_ARRAY_BUFFER, vbo );
		const GLbyte *base = (const GLbyte *)NULL + baseOffset;

		// arrays are enabled once at VAO creation; only respecify pointers here
		if ( layout == VL_DRAWVERT ) {
			const GLsizei stride = sizeof( idDrawVert );
			gl3VertexAttribPointer( 0, 3, GL_FLOAT, GL_FALSE, stride, base + offsetof( idDrawVert, xyz ) );
			gl3VertexAttribPointer( 1, 2, GL_FLOAT, GL_FALSE, stride, base + offsetof( idDrawVert, st ) );
			gl3VertexAttribPointer( 2, 3, GL_FLOAT, GL_FALSE, stride, base + offsetof( idDrawVert, normal ) );
			gl3VertexAttribPointer( 3, 3, GL_FLOAT, GL_FALSE, stride, base + offsetof( idDrawVert, tangents ) );
			gl3VertexAttribPointer( 4, 3, GL_FLOAT, GL_FALSE, stride, base + offsetof( idDrawVert, tangents ) + sizeof( idVec3 ) );
			gl3VertexAttribPointer( 5, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, base + offsetof( idDrawVert, color ) );
		} else {
			gl3VertexAttribPointer( 0, 4, GL_FLOAT, GL_FALSE, sizeof( shadowCache_t ), base );
		}

		boundLayout = layout;
		boundVBO = vbo;
		boundBase = baseOffset;
	}

	void InvalidateCaches() {
		boundLayout = -1;
		boundVBO = 0;
		boundBase = -1;
		boundProgram = 0;
		currentStateBits = 0;
		currentCull = -1;
		forceState = true;
		currentPipeline.stateBits = 0;
		currentPipeline.shader = 0;
		currentPipeline.vertexLayout = VL_DRAWVERT;
		currentPipeline.cullType = 0;
	}
};

static GL3Backend gl3Backend;

RHI *GetGL3RHI() {
	return &gl3Backend;
}

} // namespace rhi
