/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.cpp for license header)
===========================================================================
*/

// DUDE GL 3.3 core backend — Phase 3.
// Chunk A state: context bring-up + frame clear only. Selected with
// r_graphicsAPI opengl3; the legacy ARB path (r_graphicsAPI opengl) is
// untouched and remains the default until parity.

#include "sys/platform.h"
#include "renderer/tr_local.h"
#include "renderer/rhi/RHI.h"

namespace rhi {

class GL3Backend : public RHI {
public:
	virtual void BeginFrame( int windowWidth, int windowHeight ) {
		qglViewport( 0, 0, windowWidth, windowHeight );
		qglDisable( GL_SCISSOR_TEST );
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

	// ---- Chunk B+ ----
	virtual BufferHandle CreateBuffer( BufferUsage, int, const void * )		{ return 0; }
	virtual void UpdateBuffer( BufferHandle, int, int, const void * )		{}
	virtual void DestroyBuffer( BufferHandle )								{}
	virtual ImageHandle CreateImage( ImageFormat, int, int, const void * )	{ return 0; }
	virtual void DestroyImage( ImageHandle )								{}
	virtual ShaderHandle LoadShader( const char * )							{ return 0; }

	// ---- Chunk C+ ----
	virtual void BindPipeline( const PipelineDesc & )						{}
	virtual void Draw( const DrawArgs & )									{}
	virtual void CopyFramebufferToImage( ImageHandle, int, int )			{}
};

static GL3Backend gl3Backend;

RHI *GetGL3RHI() {
	return &gl3Backend;
}

} // namespace rhi

/*
=============
RB_GL3_ExecuteBackEndCommands

Chunk A command executor: clears the frame to a recognizable color and swaps.
All draw commands are consumed without rendering (the frontend still runs, so
vertexCache/session behave normally). Replaced chunk by chunk.
=============
*/
void RB_GL3_ExecuteBackEndCommands( const emptyCommand_t *cmds ) {
	static bool announced = false;
	if ( !announced ) {
		announced = true;
		common->Printf( "GL3 backend: Chunk A - clearing frames only, no drawing yet\n" );
	}

	rhi::RHI *r = rhi::GetGL3RHI();
	r->BeginFrame( glConfig.vidWidth, glConfig.vidHeight );

	// recognizable "new backend alive" clear (dark teal, clearly not legacy black)
	rhi::ClearArgs clear;
	clear.color = true;
	clear.depth = true;
	clear.stencil = true;
	clear.rgba[0] = 0.05f; clear.rgba[1] = 0.10f; clear.rgba[2] = 0.12f; clear.rgba[3] = 1.0f;
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
