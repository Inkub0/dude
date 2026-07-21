/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.cpp for license header)
===========================================================================
*/

// DUDE GL 3.3 core backend — Phase 3.
// Chunk B state: core function loading, GLSL program cache (GL3Shaders.cpp),
// per-draw UBO ring, VAOs, buffer objects. Still clears magenta and draws
// nothing — drawing starts in Chunk C (2D/GUI/console milestone).
// Selected with r_graphicsAPI opengl3; the legacy ARB path (r_graphicsAPI
// opengl) is untouched and remains the default until parity.

#include "sys/platform.h"
#include "renderer/tr_local.h"
#include "renderer/rhi/RHI.h"
#include "renderer/rhi/GL3Local.h"
#include "renderer/Model.h"		// shadowCache_t

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

class GL3Backend : public RHI {
	static const int UBO_RING_SIZE = 1 << 20;	// 1 MB = ~1400 aligned RenderParams slices per wrap

	bool			initialized;
	GLuint			uboRing;
	int				uboRingOffset;
	GLint			uboAlign;
	GLuint			vaos[VL_COUNT];

	// vertex layout rebind cache (GL 3.3 VAOs capture pointer+buffer at
	// VertexAttribPointer time, so offsets are respecified when they change)
	int				boundLayout;
	GLuint			boundVBO;
	int				boundBase;

public:
	GL3Backend() : initialized( false ), uboRing( 0 ), uboRingOffset( 0 ), uboAlign( 256 ) {
		vaos[0] = vaos[1] = 0;
		InvalidateVertexLayout();
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

		gl3GenBuffers( 1, &uboRing );
		gl3BindBuffer( GL_UNIFORM_BUFFER, uboRing );
		gl3BufferData( GL_UNIFORM_BUFFER, UBO_RING_SIZE, NULL, GL_DYNAMIC_DRAW );
		uboRingOffset = 0;

		gl3GenVertexArrays( VL_COUNT, vaos );
		InvalidateVertexLayout();

		GL3_InitShaderCache();

		initialized = true;
		common->Printf( "GL3 backend: %d KB uniform ring (align %d), %d vertex layouts\n",
		                UBO_RING_SIZE / 1024, uboAlign, (int)VL_COUNT );
		return true;
	}

	virtual void Shutdown() {
		if ( !initialized ) {
			return;
		}
		GL3_ShutdownShaderCache();
		gl3DeleteBuffers( 1, &uboRing );
		gl3DeleteVertexArrays( VL_COUNT, vaos );
		uboRing = 0;
		vaos[0] = vaos[1] = 0;
		initialized = false;
	}

	// ---- frame ----
	virtual void BeginFrame( int windowWidth, int windowHeight ) {
		qglViewport( 0, 0, windowWidth, windowHeight );
		qglDisable( GL_SCISSOR_TEST );

		if ( initialized ) {
			// orphan the uniform ring: the driver keeps last frame's storage
			// alive for in-flight draws while we write into fresh memory
			gl3BindBuffer( GL_UNIFORM_BUFFER, uboRing );
			gl3BufferData( GL_UNIFORM_BUFFER, UBO_RING_SIZE, NULL, GL_DYNAMIC_DRAW );
			uboRingOffset = 0;
			InvalidateVertexLayout();
		}
	}
	virtual void EndFrame() {}

	virtual void BeginPass( const ClearArgs *clear ) {
		if ( !clear ) {
			return;
		}
		GLbitfield bits = 0;
		if ( clear->color ) {
			qglClearColor( clear->rgba[0], clear->rgba[1], clear->rgba[2], clear->rgba[3] );
			bits |= GL_COLOR_BUFFER_BIT;
		}
		if ( clear->depth )		bits |= GL_DEPTH_BUFFER_BIT;
		if ( clear->stencil ) {
			qglClearStencil( clear->stencilValue );
			bits |= GL_STENCIL_BUFFER_BIT;
		}
		if ( bits ) {
			qglClear( bits );
		}
	}
	virtual void EndPass() {}
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
		return b;
	}

	virtual void UpdateBuffer( BufferHandle b, int offset, int size, const void *data ) {
		gl3BindBuffer( GL_ARRAY_BUFFER, b );
		gl3BufferSubData( GL_ARRAY_BUFFER, offset, size, data );
	}

	virtual void DestroyBuffer( BufferHandle b ) {
		if ( b ) {
			GLuint name = b;
			gl3DeleteBuffers( 1, &name );
		}
	}

	virtual int AllocUniforms( const void *data, int size, BufferHandle *buffer ) {
		int offset = ( uboRingOffset + uboAlign - 1 ) & ~( uboAlign - 1 );
		gl3BindBuffer( GL_UNIFORM_BUFFER, uboRing );
		if ( offset + size > UBO_RING_SIZE ) {
			// mid-frame wrap: orphan again, earlier draws keep the old storage
			gl3BufferData( GL_UNIFORM_BUFFER, UBO_RING_SIZE, NULL, GL_DYNAMIC_DRAW );
			offset = 0;
		}
		gl3BufferSubData( GL_UNIFORM_BUFFER, offset, size, data );
		uboRingOffset = offset + size;
		*buffer = uboRing;
		return offset;
	}

	virtual ImageHandle CreateImage( ImageFormat, int, int, const void * )	{ return 0; }	// Chunk C
	virtual void DestroyImage( ImageHandle )								{}

	virtual ShaderHandle LoadShader( const char *name ) {
		return GL3_FindProgram( name );
	}

	// ---- drawing (Chunk C) ----
	virtual void BindPipeline( const PipelineDesc & )						{}
	virtual void Draw( const DrawArgs & )									{}
	virtual void CopyFramebufferToImage( ImageHandle, int, int )			{}

	// binds the VAO for `layout` with attribute pointers into vbo at
	// baseOffset (Chunk C draw path; cached to skip redundant respecifies)
	void BindVertexLayout( VertexLayout layout, GLuint vbo, int baseOffset ) {
		if ( (int)layout == boundLayout && vbo == boundVBO && baseOffset == boundBase ) {
			return;
		}
		gl3BindVertexArray( vaos[layout] );
		gl3BindBuffer( GL_ARRAY_BUFFER, vbo );
		const GLbyte *base = (const GLbyte *)NULL + baseOffset;

		if ( layout == VL_DRAWVERT ) {
			const GLsizei stride = sizeof( idDrawVert );
			for ( int i = 0; i <= 5; i++ ) {
				gl3EnableVertexAttribArray( i );
			}
			gl3VertexAttribPointer( 0, 3, GL_FLOAT, GL_FALSE, stride, base + offsetof( idDrawVert, xyz ) );
			gl3VertexAttribPointer( 1, 2, GL_FLOAT, GL_FALSE, stride, base + offsetof( idDrawVert, st ) );
			gl3VertexAttribPointer( 2, 3, GL_FLOAT, GL_FALSE, stride, base + offsetof( idDrawVert, normal ) );
			gl3VertexAttribPointer( 3, 3, GL_FLOAT, GL_FALSE, stride, base + offsetof( idDrawVert, tangents ) );
			gl3VertexAttribPointer( 4, 3, GL_FLOAT, GL_FALSE, stride, base + offsetof( idDrawVert, tangents ) + sizeof( idVec3 ) );
			gl3VertexAttribPointer( 5, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, base + offsetof( idDrawVert, color ) );
		} else {
			gl3EnableVertexAttribArray( 0 );
			gl3VertexAttribPointer( 0, 4, GL_FLOAT, GL_FALSE, sizeof( shadowCache_t ), base );
		}

		boundLayout = layout;
		boundVBO = vbo;
		boundBase = baseOffset;
	}

private:
	void InvalidateVertexLayout() {
		boundLayout = -1;
		boundVBO = 0;
		boundBase = -1;
	}
};

static GL3Backend gl3Backend;

RHI *GetGL3RHI() {
	return &gl3Backend;
}

} // namespace rhi

/*
=============
RB_GL3_ExecuteBackEndCommands

Chunk B command executor: clears the frame to a recognizable color and swaps.
All draw commands are consumed without rendering (the frontend still runs, so
vertexCache/session behave normally). Replaced chunk by chunk.
=============
*/
void RB_GL3_ExecuteBackEndCommands( const emptyCommand_t *cmds ) {
	static bool announced = false;
	if ( !announced ) {
		announced = true;
		common->Printf( "GL3 backend: Chunk B - resources online, no drawing yet\n" );
	}

	rhi::RHI *r = rhi::GetGL3RHI();
	r->BeginFrame( glConfig.vidWidth, glConfig.vidHeight );

	// unmistakable "new backend alive" clear — bright magenta, the universal
	// placeholder colour (draws nothing yet; this proves the context clears)
	rhi::ClearArgs clear;
	clear.color = true;
	clear.depth = true;
	clear.stencil = true;
	clear.rgba[0] = 0.7f; clear.rgba[1] = 0.0f; clear.rgba[2] = 0.7f; clear.rgba[3] = 1.0f;
	clear.stencilValue = 0;
	r->BeginPass( &clear );
	r->EndPass();

	for ( ; cmds; cmds = (const emptyCommand_t *)cmds->next ) {
		if ( cmds->commandId == RC_SWAP_BUFFERS ) {
			GLimp_SwapBuffers();
		}
	}

	r->EndFrame();
}
