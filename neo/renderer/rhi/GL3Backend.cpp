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
	// ring sizes; wraps orphan mid-frame, so these are throughput hints.
	// vertex/index rings carry the whole visible world per frame until
	// vertexCache gets real VBOs (Chunk G), so they are sized generously.
	static const int UBO_RING_SIZE  = 4 << 20;
	static const int VERT_RING_SIZE = 32 << 20;
	static const int IDX_RING_SIZE  = 8 << 20;

	struct ring_t {
		GLuint	buffer;
		int		offset;
		int		size;
		GLenum	target;
	};

	bool			initialized;
	ring_t			uboRing, vertRing, idxRing;
	GLint			uboAlign;
	int				streamGen;
	GLuint			vaos[VL_COUNT];

	PipelineDesc	currentPipeline;
	int				currentStateBits;
	int				currentCull;
	bool			forceState;

	// vertex layout rebind cache (GL 3.3 VAOs capture pointer+buffer at
	// VertexAttribPointer time, so offsets are respecified when they change)
	int				boundLayout;
	GLuint			boundVBO;
	int				boundBase;

public:
	GL3Backend() : initialized( false ), uboAlign( 256 ), streamGen( 0 ) {
		uboRing.buffer = vertRing.buffer = idxRing.buffer = 0;
		vaos[0] = vaos[1] = 0;
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

		InitRing( uboRing,  GL_UNIFORM_BUFFER,       UBO_RING_SIZE );
		InitRing( vertRing, GL_ARRAY_BUFFER,         VERT_RING_SIZE );
		InitRing( idxRing,  GL_ELEMENT_ARRAY_BUFFER, IDX_RING_SIZE );

		gl3GenVertexArrays( VL_COUNT, vaos );
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
		gl3DeleteBuffers( 1, &uboRing.buffer );
		gl3DeleteBuffers( 1, &vertRing.buffer );
		gl3DeleteBuffers( 1, &idxRing.buffer );
		gl3DeleteVertexArrays( VL_COUNT, vaos );
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
			// orphan the rings: the driver keeps last frame's storage alive
			// for in-flight draws while we write into fresh memory
			OrphanRing( uboRing );
			OrphanRing( vertRing );
			OrphanRing( idxRing );
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
			qglStencilMask( 0xff );
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

	// ---- drawing ----
	virtual void BindPipeline( const PipelineDesc &desc ) {
		unsigned int program = GL3_ProgramObject( desc.shader );
		if ( program ) {
			gl3UseProgram( program );
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

private:
	void InitRing( ring_t &ring, GLenum target, int size ) {
		ring.target = target;
		ring.size = size;
		ring.offset = 0;
		gl3GenBuffers( 1, &ring.buffer );
		// allocate via ARRAY_BUFFER (VAO-neutral, buffers are untyped)
		gl3BindBuffer( GL_ARRAY_BUFFER, ring.buffer );
		gl3BufferData( GL_ARRAY_BUFFER, size, NULL, GL_DYNAMIC_DRAW );
	}

	void OrphanRing( ring_t &ring ) {
		gl3BindBuffer( GL_ARRAY_BUFFER, ring.buffer );
		gl3BufferData( GL_ARRAY_BUFFER, ring.size, NULL, GL_DYNAMIC_DRAW );
		ring.offset = 0;
		streamGen++;
	}

	int AllocFromRing( ring_t &ring, const void *data, int size, int align, BufferHandle *buffer ) {
		int offset = ( ring.offset + align - 1 ) & ~( align - 1 );
		gl3BindBuffer( GL_ARRAY_BUFFER, ring.buffer );
		if ( offset + size > ring.size ) {
			// mid-frame wrap: orphan again, earlier draws keep the old storage
			gl3BufferData( GL_ARRAY_BUFFER, ring.size, NULL, GL_DYNAMIC_DRAW );
			offset = 0;
		}
		gl3BufferSubData( GL_ARRAY_BUFFER, offset, size, data );
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

	void InvalidateCaches() {
		boundLayout = -1;
		boundVBO = 0;
		boundBase = -1;
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
