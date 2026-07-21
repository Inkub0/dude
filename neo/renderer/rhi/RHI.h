/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.h for license header)
===========================================================================
*/

#ifndef __RHI_H__
#define __RHI_H__

// DUDE render hardware interface (docs/vulkan-port.md Phase 3).
//
// Minimal abstraction shaped by what idTech4 actually needs, implemented by
// the GL 3.3 core backend (rhi/GL3Backend.cpp) and later the Vulkan backend.
// Design guardrails (for the vulkan-rt profile, see plan): passes are
// begin/end-scoped (not VkRenderPass-shaped), geometry stays identifiable,
// descriptor-ish layout decisions live in the backends, sync is high-level.
//
// C++11, engine-independent header (no GL/VK includes here).

namespace rhi {

// opaque handles; 0 = invalid
typedef unsigned int BufferHandle;
typedef unsigned int ImageHandle;
typedef unsigned int SamplerHandle;
typedef unsigned int ShaderHandle;		// linked vert+frag program pair

enum BufferUsage {
	BU_VERTEX,
	BU_INDEX,
	BU_UNIFORM		// per-draw ring slices (RenderParams / ArbParams)
};

enum ImageFormat {
	IF_RGBA8,
	IF_DEPTH24_STENCIL8,	// backend may substitute D32S8
	IF_RGBA16F				// Tier-3 HDR target (post stack)
};

enum VertexLayout {
	VL_DRAWVERT,	// idDrawVert: pos3/st2/normal3/tangent3/bitangent3/color4ub, locations 0-5
	VL_SHADOW,		// shadowCache_t: pos4 only, location 0
	VL_COUNT
};

// GLS_* state bits from Material.h travel through unchanged; a pipeline is
// (stateBits, shader, vertexLayout) and backends cache whatever object that
// maps to (GL: program+state apply, VK: VkPipeline keyed the same way).
struct PipelineDesc {
	int				stateBits;		// GLS_* blend/depth/stencil/mask bits
	ShaderHandle	shader;
	VertexLayout	vertexLayout;
	int				cullType;		// CT_* from Material.h
};

struct DrawArgs {
	BufferHandle	vertexBuffer;
	int				vertexOffset;	// bytes into the buffer
	BufferHandle	indexBuffer;
	int				firstIndex;
	int				indexCount;
	BufferHandle	uniformBuffer;	// UBO ring slice
	int				uniformOffset;	// aligned to backend's requirement
	int				uniformSize;
	ImageHandle		textures[8];	// by unit, 0 = unbound
	SamplerHandle	samplers[8];
};

struct ClearArgs {
	bool	color, depth, stencil;
	float	rgba[4];
	unsigned char stencilValue;
};

class RHI {
public:
	virtual			~RHI() {}

	// ---- lifecycle ----
	// Init is called with a live context/device (and again after vid_restart,
	// where all previous handles are already dead with the old context).
	virtual bool	Init() = 0;
	virtual void	Shutdown() = 0;

	// ---- frame ----
	virtual void	BeginFrame( int windowWidth, int windowHeight ) = 0;
	virtual void	EndFrame() = 0;						// present handled by glimp/swapchain

	// ---- passes (begin/end-scoped; backend decides render-pass objects) ----
	virtual void	BeginPass( const ClearArgs *clear ) = 0;	// NULL = load existing
	virtual void	EndPass() = 0;
	virtual void	SetViewport( int x, int y, int w, int h ) = 0;
	virtual void	SetScissor( int x, int y, int w, int h ) = 0;

	// ---- resources (Chunk B+) ----
	virtual BufferHandle	CreateBuffer( BufferUsage usage, int size, const void *data ) = 0;
	virtual void			UpdateBuffer( BufferHandle b, int offset, int size, const void *data ) = 0;
	virtual void			DestroyBuffer( BufferHandle b ) = 0;
	virtual ImageHandle		CreateImage( ImageFormat fmt, int w, int h, const void *pixels ) = 0;
	virtual void			DestroyImage( ImageHandle i ) = 0;
	virtual ShaderHandle	LoadShader( const char *name ) = 0;	// loads name.vert/.frag via VFS

	// per-draw uniform ring: writes `size` bytes and returns the aligned
	// offset (+ the ring's buffer in *buffer) for DrawArgs::uniformBuffer/
	// uniformOffset. Contents live at least until the frame is presented.
	virtual int				AllocUniforms( const void *data, int size, BufferHandle *buffer ) = 0;

	// frame-temporary geometry rings, same lifetime rules (core profiles have
	// no client-side arrays, so dynamic geometry streams through these).
	// Returned offsets are in bytes.
	virtual int				AllocVertices( const void *data, int size, BufferHandle *buffer ) = 0;
	virtual int				AllocIndices( const void *data, int size, BufferHandle *buffer ) = 0;

	// bumped whenever a ring orphans its storage (new frame or mid-frame
	// wrap); previously returned offsets are invalid for NEW draws once this
	// changes — callers caching stream results must revalidate against it
	virtual int				StreamGeneration() = 0;

	// ---- drawing (Chunk C+) ----
	virtual void	BindPipeline( const PipelineDesc &desc ) = 0;
	virtual void	Draw( const DrawArgs &args ) = 0;

	// ---- screen copies (_currentRender / _currentDepth points) ----
	virtual void	CopyFramebufferToImage( ImageHandle dst, int w, int h ) = 0;
};

// factory: created by the active backend at renderer init
RHI *		GetGL3RHI();

} // namespace rhi

#endif /* !__RHI_H__ */
