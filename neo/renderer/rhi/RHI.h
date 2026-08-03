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
typedef unsigned int RenderTargetHandle;	// offscreen FBO; 0 = the backbuffer

enum BufferUsage {
	BU_VERTEX,
	BU_INDEX,
	BU_UNIFORM		// per-draw ring slices (RenderParams / ArbParams)
};

enum ImageFormat {
	IF_RGBA8,
	IF_DEPTH24_STENCIL8,	// backend may substitute D32S8
	IF_RGBA16F,				// Tier-3 HDR target (post stack)
	IF_DEPTH24				// depth-only; shadow-map target, sampler2DShadow-ready
};

enum VertexLayout {
	VL_DRAWVERT,	// idDrawVert: pos3/st2/normal3/tangent3/bitangent3/color4ub, locations 0-5
	VL_SHADOW,		// shadowCache_t: pos4 only, location 0
	VL_IMMEDIATE,	// imVert_t: pos3/st2/color4ub (24 B), locations 0/1/5 — debug drawing
	VL_COUNT
};

// Stencil configurations the world renderer actually uses (Phase 4 M4).
// GL keeps stencil as free-floating state (RhiWorld drives qglStencilFunc/
// OpSeparate directly and the GL3 backend ignores this field); Vulkan bakes
// it into the pipeline, so the same choices travel as a compact enum in
// PipelineDesc. Ops are wrap variants; masks are 255. "front" below means
// GL front-facing — the VK backend's CCW front-face setup preserves GL's
// facing exactly (verified with the M2 winding work).
enum StencilState {
	SS_DISABLED = 0,		// stencil test off (all 2D / non-world paths)
	SS_ALWAYS,				// test enabled, ALWAYS pass, KEEP — depth fill / unshadowed
	SS_SHADOW_TEST,			// GEQUAL ref 128 — interactions test against the volumes
	SS_VOLUME_PRELOAD,		// caps preload: front INCR on zfail+zpass, back DECR
	SS_VOLUME_ZPASS,		// depth-pass volumes: front DECR on zpass, back INCR
	SS_VOLUME_ZFAIL,		// Carmack's reverse: front INCR on zfail, back DECR
	SS_VOLUME_PRELOAD_MIRROR,	// mirror views swap the faces
	SS_VOLUME_ZPASS_MIRROR,
	SS_VOLUME_ZFAIL_MIRROR,
	SS_COUNT
};

// GLS_* state bits from Material.h travel through unchanged; a pipeline is
// (stateBits, shader, vertexLayout) and backends cache whatever object that
// maps to (GL: program+state apply, VK: VkPipeline keyed the same way).
struct PipelineDesc {
	int				stateBits = 0;		// GLS_* blend/depth/stencil/mask bits
	ShaderHandle	shader = 0;
	VertexLayout	vertexLayout = VL_DRAWVERT;
	int				cullType = 0;		// CT_* from Material.h
	int				stencilState = SS_DISABLED;	// StencilState (GL3 backend ignores it)
	// primitive topology as a GL primMode (GL_LINES/POINTS/TRIANGLES/…); -1 =
	// triangle list (the default every non-immediate draw uses). Only the
	// Vulkan backend reads it (bakes topology into the pipeline); GL3 passes
	// primMode straight to glDrawArrays and ignores this.
	int				topology = -1;
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
	ImageHandle		shadowCube;		// point-light cube depth map; bound to unit 8
									// as a GL_TEXTURE_CUBE_MAP (0 = unbound)
	ImageHandle		ssao;			// SSAO/GTAO buffer; bound to unit 9 (0 = unbound)
	ImageHandle		occlusion;		// baked occlusion map; bound to unit 10 (0 = unbound)
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
	// begin a pass rendering INTO an offscreen target instead of the backbuffer.
	// Sets the viewport to the whole target; the matching EndPass restores the
	// backbuffer and the previous viewport. Nests one level under the main pass
	// (a plain FBO bind/unbind in GL terms), keeping the begin/end-scoped model.
	virtual void	BeginTargetPass( RenderTargetHandle rt, const ClearArgs *clear ) = 0;
	virtual void	EndPass() = 0;
	virtual void	SetViewport( int x, int y, int w, int h ) = 0;
	virtual void	SetScissor( int x, int y, int w, int h ) = 0;
	// depth-range window (the weapon/model depth hacks): GL maps it to
	// glDepthRange, Vulkan to the viewport's min/max depth. Reset to 0..1 by
	// BeginFrame.
	virtual void	SetDepthRange( float minDepth, float maxDepth ) {}
	// polygon offset (shadow volumes, polygonOffset materials): Vulkan dynamic
	// depth bias; the GL3 backend keeps its literal qglPolygonOffset path in
	// RhiWorld and ignores this (default no-op). Reset (disabled) by BeginFrame.
	virtual void	SetPolygonOffset( bool enable, float factor, float units ) {}
	// clear the stencil buffer to `value` within the current scissor rect,
	// mid-pass (the per-light stencil clear). GL path uses qglClear directly;
	// Vulkan implements this as vkCmdClearAttachments.
	virtual void	ClearStencilBuffer( int value ) {}

	// ---- resources (Chunk B+) ----
	virtual BufferHandle	CreateBuffer( BufferUsage usage, int size, const void *data ) = 0;
	virtual void			UpdateBuffer( BufferHandle b, int offset, int size, const void *data ) = 0;
	virtual void			DestroyBuffer( BufferHandle b ) = 0;
	virtual ImageHandle		CreateImage( ImageFormat fmt, int w, int h, const void *pixels ) = 0;
	virtual void			DestroyImage( ImageHandle i ) = 0;

	// Phase 4 M2 image ownership: engine texture upload. pixels is RGBA8;
	// textureFilter/textureRepeat carry the engine's textureFilter_t /
	// textureRepeat_t values so the backend derives the sampler; allowMips
	// builds a full CPU mip chain. Backends without an image path return 0
	// (the GL3 backend keeps binding through idImage; the default below keeps
	// it source-compatible).
	virtual ImageHandle		CreateTexture2D( int w, int h, const void *pixels,
	                                         int textureFilter, int textureRepeat,
	                                         bool allowMips ) { return 0; }

	// Phase 4 M4: cube-map upload (normalization/ambient cube maps, env maps).
	// pics are six size×size RGBA8 faces in GL_TEXTURE_CUBE_MAP_POSITIVE_X..
	// order; sampler is always clamp-to-edge (the only mode that makes sense
	// on a cube). Same default-0 contract as CreateTexture2D.
	virtual ImageHandle		CreateTextureCube( int size, const void * const pics[6],
	                                           int textureFilter, bool allowMips ) { return 0; }

	virtual ShaderHandle	LoadShader( const char *name ) = 0;	// loads name.vert/.frag via VFS

	// ---- offscreen render targets (Phase 3.5 shadow maps; Phase 11 post stack) ----
	// Create an offscreen target and its backing texture. A depth format makes a
	// depth-only target (no color attachment) suitable for shadow maps, sampled
	// via GetRenderTargetImage() as a sampler2DShadow-ready depth texture. Returns
	// 0 on failure (e.g. incomplete FBO); callers must cope with an absent target.
	virtual RenderTargetHandle	CreateRenderTarget( ImageFormat fmt, int w, int h ) = 0;
	// Cube depth target for point-light (omni) shadow maps: six square depth faces
	// sampled as a samplerCubeShadow. Render each face with BeginCubeFacePass();
	// DestroyRenderTarget / GetRenderTargetImage work the same as the 2D target.
	virtual RenderTargetHandle	CreateRenderTargetCube( ImageFormat fmt, int size ) = 0;
	// Color target WITH a depth attachment, for a depth-tested offscreen geometry pass
	// (the SSAO normal G-buffer). Color is a sampleable RGBA8 texture (GetRenderTargetImage);
	// the depth attachment is written/tested but not sampled. colorCount 2 adds a second
	// RGBA8 attachment (MRT, GetRenderTargetImage2) — the SSR roughness/metalness buffer
	// riding the same geometry pass (docs/ssr.md). 0 on failure.
	virtual RenderTargetHandle	CreateRenderTargetColorDepth( ImageFormat fmt, int w, int h, int colorCount = 1 ) = 0;
	// Color target (IF_RGBA8 or IF_RGBA16F) with a combined DEPTH24_STENCIL8 attachment,
	// for the HDR scene buffer: an offscreen geometry pass that needs stencil (stencil
	// shadows) and a sampleable float color. GetRenderTargetImage returns the color. 0 on failure.
	virtual RenderTargetHandle	CreateRenderTargetColorDepthStencil( ImageFormat fmt, int w, int h ) = 0;
	virtual void				DestroyRenderTarget( RenderTargetHandle rt ) = 0;
	// Route the whole frame into an offscreen target (the HDR scene buffer): BeginPass
	// clears into it and EndPass returns to it after nested target passes (shadow maps,
	// SSAO). rt == 0 restores the backbuffer. BeginFrame resets this to 0.
	virtual void				SetFrameTarget( RenderTargetHandle rt ) = 0;
	// begin a pass into one face (0..5 = +X,-X,+Y,-Y,+Z,-Z) of a cube target.
	// EndPass restores the backbuffer + viewport exactly like BeginTargetPass.
	virtual void				BeginCubeFacePass( RenderTargetHandle rt, int face, const ClearArgs *clear ) = 0;
	// the target's texture as a sampleable image handle — the same ImageHandle
	// abstraction future material textures will use (Phase 4 image ownership).
	virtual ImageHandle			GetRenderTargetImage( RenderTargetHandle rt ) = 0;
	// second color attachment of a colorCount-2 color+depth target (0 if absent)
	virtual ImageHandle			GetRenderTargetImage2( RenderTargetHandle rt ) = 0;

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

	// ---- screen copies (_currentRender / _currentDepth / _scratch, Phase 4 M5) ----
	// The GL3 backend keeps the literal qglCopyTexSubImage2D path in idImage
	// (default no-ops here); the Vulkan backend implements captures as copies
	// out of the offscreen scene image.
	//
	// CreateCaptureImage allocates a sampleable copy target: RGBA8 color
	// (linear, clamp-to-edge — the capture filtering GL sets) or the scene's
	// depth format (nearest). Contents are undefined until the first copy.
	virtual ImageHandle	CreateCaptureImage( int w, int h, bool depth ) { return 0; }
	// Copy a framebuffer rect into a capture image. src rect is in GL window
	// coordinates (origin bottom-left, like qglCopyTexSubImage2D); dst is in
	// texel rows from the start of the image. Color copies convert to GL's
	// bottom-up memory layout so explicit-texcoord consumers (the player-view
	// _scratch warps) sample identically to GL; depth copies stay in native
	// orientation (their consumers address by gl_FragCoord, which is native
	// per backend). Callable mid-pass — the backend suspends/resumes.
	virtual void	CopyFramebufferToImage( ImageHandle dst, int dstX, int dstY,
	                                        int srcX, int srcY, int w, int h, bool depth ) {}
	// Destroy that never stalls mid-frame: queued until no in-flight frame can
	// reference the image (capture/cinematic reallocation).
	virtual void	RetireImage( ImageHandle img ) { DestroyImage( img ); }
	// Full re-upload of an existing same-size RGBA8 texture, mid-frame safe
	// (cinematic frames): staged through a per-frame ring inside the frame's
	// command stream, ordered against this frame's earlier samples.
	virtual void	UpdateTexture2D( ImageHandle dst, int w, int h, const void *pixels ) {}

	// ---- readback (Phase 4 M6: screenshots / capture-to-file) ----
	// Read an RGB rect of the last completed frame into dest, matching the
	// glReadPixels(GL_RGB) contract the callers were written against: GL
	// window coordinates (origin bottom-left), rows bottom-up and padded to
	// 4-byte boundaries. Synchronous (screenshot-grade stall is fine).
	// Default false = unsupported; the GL backends keep their literal
	// glReadPixels paths.
	virtual bool	ReadPixelsRGB( unsigned char *dest, int x, int y, int w, int h ) { return false; }

	// ---- immediate-mode debug drawing (Chunk G) ----
	// Draws a batch of interleaved verts { float xyz[3]; float st[2];
	// byte rgba[4]; } (24 B, = imVert_t) as `primMode` (GL_LINES/POINTS/
	// TRIANGLES/TRIANGLE_FAN) through the generic program with the given MVP.
	// `textured`: sample the currently-bound unit-0 image; else a white texel.
	// State (blend/depth/cull/polygon-mode) is whatever the caller set; this
	// invalidates the pipeline cache so the next Draw re-binds cleanly.
	virtual void	DrawImmediate( const void *verts, int numVerts, unsigned int primMode,
	                               const float mvp[16], bool textured ) = 0;
};

// backend factories
RHI *		GetGL3RHI();
RHI *		GetVulkanRHI();		// NULL until Phase 4 M1 (rhi/vk/, DHEWM3_VULKAN builds)

// active-backend selection (Phase 4 M0). R_InitOpenGL records the backend it
// brought up; everything else asks GetRHI() instead of naming a backend.
// Callers must still gate on glConfig.rhiBackend — on the legacy GL path no
// RHI backend is initialized and GetRHI() must not be used to draw.
enum BackendType {
	BT_GL3,
	BT_VULKAN
};
void		SetActiveBackend( BackendType type );
BackendType	GetActiveBackendType();
RHI *		GetRHI();

} // namespace rhi

#endif /* !__RHI_H__ */
