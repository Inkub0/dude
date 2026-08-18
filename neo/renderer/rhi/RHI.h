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
typedef unsigned int BlasHandle;		// bottom-level acceleration structure (R2 ray query); 0 = invalid

enum BufferUsage {
	BU_VERTEX,		// must stay 0 (the VK backend's switch default)
	BU_INDEX,
	BU_UNIFORM,		// per-draw ring slices (RenderParams / ArbParams)
	BU_STORAGE,		// GPU compute storage buffer (SSBO). VK only; GL 3.3 has no compute.
					// Phase 1: host-visible (BAR), read back via ReadBuffer; a device-local
					// staged variant is a Phase-2 follow-up (docs/gpu-offload-plan.md).
					// Also carries INDIRECT usage so a compute pass can write a
					// VkDrawIndexedIndirectCommand[] here for DrawIndexedIndirect (Phase 3 seed).
	BU_SKIN			// dual-usage STORAGE|VERTEX buffer: written by the skinning compute
					// kernel, then bound as a vertex buffer by the draw passes (Phase 2
					// GPU MD5 skinning). VK only; the single hard prereq tessellation
					// composition needs (the tesc/tese read it as ordinary vertex input).
					// Host-visible+mapped for now (lets CreateBuffer pre-fill the static
					// st/color fields the kernel leaves alone); device-local is a follow-up.
};

enum ImageFormat {
	IF_RGBA8,
	IF_DEPTH24_STENCIL8,	// backend may substitute D32S8
	IF_RGBA16F,				// Tier-3 HDR target (post stack)
	IF_DEPTH24,				// depth-only; shadow-map target, sampler2DShadow-ready
	IF_R16F,				// single-channel half-float; SSAO linear-depth mip (Phase 2)
	IF_RG16F				// two-channel half-float; motion-vector / velocity MRT (R1/A0)
};

enum VertexLayout {
	VL_DRAWVERT,	// idDrawVert: pos3/st2/normal3/tangent3/bitangent3/color4ub, locations 0-5
	VL_SHADOW,		// shadowCache_t: pos4 only, location 0
	VL_IMMEDIATE,	// imVert_t: pos3/st2/color4ub (24 B), locations 0/1/5 — debug drawing
	VL_NONE,		// no vertex input: the shader fetches vertices itself (BDA manual fetch,
					// Phase 3.2b batched zfill). No bound vertex buffer required.
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
	// DUDE tessellation (docs/tessellation.md): route this draw through the
	// bound shader's tessellation-control/eval stages (patch-list topology, PN
	// smoothing of enemy/prop meshes). Requires the shader to have a tess variant
	// loaded and the device to support tessellation. Ignored by the GL3 backend
	// (its GL 3.3 core context has no tessellation stages).
	bool			tessellate = false;
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
	ImageHandle		parallax;		// parallax height map; bound to unit 11 (0 = unbound)
	ImageHandle		shadowCubeDyn;	// static/dynamic split (lever B): movers-only cube depth; bound to unit 12 (0 = unbound)
};

struct ClearArgs {
	bool	color, depth, stencil;
	float	rgba[4];
	unsigned char stencilValue;
};

// A GPU compute dispatch (docs/gpu-offload-plan.md Phase 1). The foundational
// primitive later phases build GPU skinning / culling on. Vulkan only — the GL3
// backend (GL 3.3 core, no compute) no-ops it. storage[] binds BU_STORAGE buffers
// to std430 bindings 0..7; a small pushConstants blob carries params (element
// counts etc.); groups* are the work-group counts. Recorded outside any render pass.
struct ComputeArgs {
	ShaderHandle	shader;				// from CreateComputeShader (0 = skip, no-op)
	BufferHandle	storage[8];			// storage-buffer bindings 0..7 (0 = unbound)
	const void *	pushConstants;		// params bound at push-constant offset 0 (NULL = none)
	int				pushConstantSize;	// bytes, <= 128
	unsigned int	groupsX, groupsY, groupsZ;
};

// FSR2 Native-AA dispatch (docs/fsr-temporal-pipeline.md R1/C2). Vulkan only; the
// GL3 backend returns false and the frame is unchanged. The backend reads sceneRT's
// color[0] (pre-tonemap RGBA16F scene) + depth attachment and velocityRT's 3rd
// attachment (RG16F per-object motion vectors, R1/A2), runs the FSR2 temporal
// resolve, and copies the result back over sceneRT color[0] — so everything after
// (eye adaptation, bloom, HUD composite, tonemap/grain at the resolve) consumes the
// temporally-stabilised scene with no reorder of the frame. jitter[XY] is the applied
// projection jitter in pixels, engine convention (+Y-up); the backend owns the sign
// flips into FSR2's top-left convention (see RunFsr2's derivation comment).
struct Fsr2DispatchArgs {
	RenderTargetHandle	sceneRT;		// RGBA16F color + depth-stencil frame target (rhiHdrRT)
	RenderTargetHandle	velocityRT;		// 3-MRT velocity gbuffer (attachment 2 = RG16F)
	float	jitterX, jitterY;			// viewDef->jitter, pixels, +Y-up
	float	frameTimeMs;				// wall-clock delta since the previous dispatch
	float	fovYRadians;				// vertical field of view
	float	zNear;						// near plane (Doom units)
	bool	reset;						// camera cut / teleport / first frame: drop FSR2 history
	float	sharpness;					// RCAS [0,1]; < 0 disables the sharpening pass
	// R1/D auto-reactive: strength of the generated reactive mask (additive particles,
	// muzzle flashes, GUI screens get less history = no ghost trails). < 0 disables; also
	// inert unless Fsr2CaptureOpaque ran this frame (the mask needs the opaque-only copy).
	float	reactiveScale;
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
	// Synchronous read-back of a buffer's contents into dst (GPU compute output;
	// docs/gpu-offload-plan.md Phase 1). Stalls, so a dev/validation path — not a
	// per-frame call. Backends without a compute lane return false. VK: memcpy of the
	// host-visible BU_STORAGE mapping after a queue idle (device-local staging is a
	// Phase-2 extension).
	virtual bool			ReadBuffer( BufferHandle b, void *dst, int size ) { return false; }
	// GPU virtual address of a buffer (Vulkan buffer_device_address / BDA). Lets a shader
	// dereference the buffer through a raw pointer (GL_EXT_buffer_reference) instead of a bound
	// vertex buffer or descriptor — the Phase 3.2b path to per-draw geometry without a single
	// unified vertex buffer (docs/gpu-offload-plan.md §3.2b). 0 = unsupported (GL 3.3 has no
	// equivalent, device lacks the feature) or invalid handle.
	virtual unsigned long long	GetBufferDeviceAddress( BufferHandle b ) { return 0; }
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

	// DUDE: parallel image load. Like CreateTexture2D, but the caller supplies the
	// full RGBA8 mip chain already built on a worker thread (levels[0] = base level
	// w×h, each following level half-size down to 1×1), so the CPU-heavy R_MipMap
	// work happens off the main thread. Only the GPU staging + submit runs here.
	// numLevels >= 1; if 1, the texture has no mips. Same default-0 contract.
	struct PrebuiltMip { const void *data; int w, h; };
	virtual ImageHandle		CreateTexture2DPrebuilt( int w, int h, const PrebuiltMip *levels,
	                                                 int numLevels, int textureFilter,
	                                                 int textureRepeat ) { return 0; }

	// Phase 4 M4: cube-map upload (normalization/ambient cube maps, env maps).
	// pics are six size×size RGBA8 faces in GL_TEXTURE_CUBE_MAP_POSITIVE_X..
	// order; sampler is always clamp-to-edge (the only mode that makes sense
	// on a cube). Same default-0 contract as CreateTexture2D.
	virtual ImageHandle		CreateTextureCube( int size, const void * const pics[6],
	                                           int textureFilter, bool allowMips ) { return 0; }

	virtual ShaderHandle	LoadShader( const char *name ) = 0;	// loads name.vert/.frag via VFS

	// Compile a transpiled vertex+fragment GLSL body pair (as emitted by
	// arb::ToGlsl — no #version/prelude line) into a linked program, cached by
	// name. GL3 compiles through the driver (GL3_FindProgramFromSource); Vulkan
	// through shaderc when built with DUDE_HAVE_SHADERC. Returns 0 when the
	// backend has no runtime compiler (Vulkan without shaderc), so the caller
	// degrades the stage. Used for custom (mod) ARB material stages the offline
	// builtin table doesn't cover.
	virtual ShaderHandle	CreateShaderFromGlsl( const char *name, const char *vertSrc, const char *fragSrc ) { return 0; }

	// Compile a standalone compute-shader GLSL source (its own #version, no graphics
	// prelude) into a compute pipeline, cached by name. Returns a ShaderHandle usable
	// as ComputeArgs::shader. Backends without a compute lane (GL 3.3) return 0.
	// docs/gpu-offload-plan.md Phase 1.
	virtual ShaderHandle	CreateComputeShader( const char *name, const char *glslSrc ) { return 0; }

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
	// Color target with a render-generated mip chain (SSAO Phase 1 prefiltered depth,
	// docs/ssao-perf-optimization.md): a single mipLevels-deep color image whose level 0
	// is rendered by a fullscreen pass (BeginTargetPass renders into level 0) and whose
	// coarser levels are filled by a custom downsample shader — BeginTargetMipPass(level)
	// renders into a chosen level while GetRenderTargetMipImage(level-1) supplies the
	// source. The whole chain is sampleable through GetRenderTargetImage() with an
	// explicit textureLod (a mip-spanning sampler). Default returns 0 so a backend
	// without the capability degrades to the non-mipped path.
	virtual RenderTargetHandle	CreateRenderTargetMipped( ImageFormat fmt, int w, int h, int mipLevels ) { return 0; }
	// Begin a fullscreen pass rendering into a specific mip LEVEL of a mipped target
	// (level 1..mipLevels-1; level 0 uses BeginTargetPass). Paired with EndPass.
	virtual void				BeginTargetMipPass( RenderTargetHandle rt, int mipLevel, const ClearArgs *clear ) {}
	// A sampleable ImageHandle for one mip level, to feed as the downsample input. On
	// Vulkan this is a single-level view (sample it with texelFetch lod 0); on GL3 it is
	// the whole texture (sample level `mipLevel` with texelFetch). 0 if not mipped.
	virtual ImageHandle			GetRenderTargetMipImage( RenderTargetHandle rt, int mipLevel ) { return 0; }
	virtual void				DestroyRenderTarget( RenderTargetHandle rt ) = 0;
	// Route the whole frame into an offscreen target (the HDR scene buffer): BeginPass
	// clears into it and EndPass returns to it after nested target passes (shadow maps,
	// SSAO). rt == 0 restores the backbuffer. BeginFrame resets this to 0.
	virtual void				SetFrameTarget( RenderTargetHandle rt ) = 0;
	// begin a pass into one face (0..5 = +X,-X,+Y,-Y,+Z,-Z) of a cube target.
	// EndPass restores the backbuffer + viewport exactly like BeginTargetPass.
	virtual void				BeginCubeFacePass( RenderTargetHandle rt, int face, const ClearArgs *clear ) = 0;
	// SSAO normal-pass merge (docs/ssao-normal-merge.md): begin a prepass that renders
	// the normal G-buffer into a dedicated color image while sharing the *scene* depth
	// (FrameDepthImage), so the depth prepass geometry produces the normal in one pass
	// instead of a second opaque submission. Returns a RenderTargetHandle whose
	// GetRenderTargetImage is the normal (bind it like the standalone rhiNormalRT), or 0
	// where unsupported (GL3) so the caller runs the standalone pass. wantMrt adds a
	// second color attachment (SSR roughness/metalness, GetRenderTargetImage2) so the
	// merge also serves SSR; the pass then carries {normal, mat} + shared scene depth.
	// End with EndPass. Vulkan-only; the default is a no-op returning 0.
	virtual RenderTargetHandle	BeginNormalPrepass( int w, int h, const ClearArgs *clear, bool wantMrt = false ) { return 0; }
	// the target's texture as a sampleable image handle — the same ImageHandle
	// abstraction future material textures will use (Phase 4 image ownership).
	virtual ImageHandle			GetRenderTargetImage( RenderTargetHandle rt ) = 0;
	// second color attachment of a colorCount-2 color+depth target (0 if absent)
	virtual ImageHandle			GetRenderTargetImage2( RenderTargetHandle rt ) = 0;
	// third color attachment (RG16F velocity MRT, R1/A2). Non-pure: only the VK
	// 3-MRT velocity gbuffer has one; every other target and the GL3 backend return 0.
	virtual ImageHandle			GetRenderTargetImage3( RenderTargetHandle rt ) { return 0; }

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

	// GPU-driven indirect indexed draw (docs/gpu-offload-plan.md Phase 3, the seed
	// primitive). Binds the SAME pipeline / vertex+index / uniform / textures as
	// Draw(args) — args.indexCount/firstIndex are ignored — then issues the draw(s)
	// from a GPU-resident VkDrawIndexedIndirectCommand[] living in `argsBuffer` (a
	// BU_STORAGE buffer, which also carries INDIRECT usage) at byte `argsOffset`:
	// `drawCount` commands, `stride` bytes apart. When `countBuffer` is nonzero the
	// live draw count is read from it at `countOffset` (a GPU-written count, clamped
	// to drawCount) via vkCmdDrawIndexedIndirectCount — the form GPU culling will use
	// once a compute pass compacts the visible commands. Vulkan only; GL3 (GL 3.3, no
	// indirect draw) no-ops. All commands share the one bound vertex/index buffer, so
	// multi-draw (drawCount>1) needs a single GPU-resident geometry buffer (Phase 3).
	virtual void	DrawIndexedIndirect( const DrawArgs &args, BufferHandle argsBuffer, int argsOffset,
	                                     int drawCount, int stride,
	                                     BufferHandle countBuffer = 0, int countOffset = 0 ) {}

	// GPU virtual address of a buffer created with an address-capable usage (BDA). Declared
	// above as GetBufferDeviceAddress; the frontend uses it to build a ZfillBatch (below).

	// One draw for a whole bucket of solid-opaque depth-prepass surfaces (Phase 3.2b consume,
	// Increment 2). Each item carries its geometry as DEVICE ADDRESSES (vbAddr = base+vertexOffset,
	// ibAddr = base+firstIndex-bytes) plus the surface MVP; the backend uploads them to a per-object
	// SSBO, builds one VkDrawIndirectCommand per item (firstInstance = item index), and issues a
	// single non-indexed vkCmdDrawIndirect whose shader fetches indices+vertices through the
	// pointers (no bound vb/ib). Pixel-identical to per-surface zfill for the same surfaces. Vulkan
	// only; GL3 no-ops (the frontend never populates a batch there — GetBufferDeviceAddress is 0).
	struct ZfillBatchItem {
		unsigned long long	vbAddr;			// device address of idDrawVert[0] (+vertexOffset)
		unsigned long long	ibAddr;			// device address of the surface's first index
		int					indexCount;		// indices to draw (== vertexCount of the non-indexed draw)
		float				mvp[16];		// RB_RHI_SpaceMvp for the surface's space
	};
	// Surfaces sharing one scissor form a group; each becomes a separate indirect draw (one bound
	// scissor per draw) over a sub-range of the one uploaded item array. Items must be ordered so a
	// group's items are contiguous ([firstItem, firstItem+itemCount)). Scissor is a GL-convention
	// rect (same x/y/w/h as SetScissor); a zero-size or negative rect means "no scissor" (full view).
	struct ZfillBatchGroup {
		int	firstItem, itemCount;
		int	scissorX, scissorY, scissorW, scissorH;
	};
	// Upload all `count` items to one per-frame SSBO ONCE, then issue `groupCount` indirect draws
	// (one per scissor group) over the shared buffer. One upload avoids the multi-draw hazard of
	// re-writing a shared buffer between draws that only execute at submit.
	virtual void	DrawZfillBatch( const ZfillBatchItem *items, int count,
	                                const ZfillBatchGroup *groups, int groupCount ) {}
	// True when the frontend should collect a ZfillBatch this view (Vulkan, r_vkBdaZfill 2,
	// BDA supported, the batch shader loaded). GL3 / other modes return false → per-surface draw.
	virtual bool	ZfillBatchEnabled() { return false; }

	// ---- compute (docs/gpu-offload-plan.md Phase 1) ----
	// Record a GPU compute dispatch (see ComputeArgs). Vulkan records it on the frame
	// command buffer outside any render pass; the GL3 backend (no compute) no-ops.
	// The foundational primitive for CPU->GPU offload (skinning / culling).
	virtual void	Dispatch( const ComputeArgs &args ) {}
	// Like Dispatch but on a dedicated command buffer, submitted and WAITED ON (synchronous),
	// so the result is ready for a ReadBuffer immediately. For dev/validation and load-time
	// GPU work; stalls the GPU, so never per-frame. GL3 no-ops.
	virtual void	DispatchSync( const ComputeArgs &args ) {}
	// FSR2 Native-AA temporal resolve over the scene target (R1/C2, Vulkan only; see
	// Fsr2DispatchArgs). Records compute on the frame command buffer after closing any
	// open render pass; returns true when the dispatch ran and sceneRT now holds the
	// resolved image. GL3 returns false (frame unchanged).
	virtual bool	RunFsr2( const Fsr2DispatchArgs &args ) { return false; }
	// R1/D: snapshot the scene color as the OPAQUE-ONLY input for FSR2's auto-reactive
	// mask. Called at the opaque/translucent split of the primary view (after the SSR
	// composite, before particles/blends draw); a straight same-orientation copy, unlike
	// the y-flipped _currentRender capture. Valid for this frame's RunFsr2 only. GL3 no-op.
	virtual void	Fsr2CaptureOpaque( RenderTargetHandle sceneRT ) {}

	// ---- ray-query acceleration structures (R2, docs/rtx-shadow-roadmap.md) ----
	// Vulkan-only, and only on RT-capable hardware (KHR_acceleration_structure +
	// KHR_ray_query); GL3 and non-RT devices no-op. Gate all feature work on this.
	virtual bool	SupportsRayQuery() { return false; }
	// Build one BLAS over an indexed triangle soup, SYNCHRONOUSLY (load-time / validation;
	// the per-frame refit lane comes with animated geometry). positions is a float3 array
	// read with posStride bytes between vertices (pass sizeof(idDrawVert) to feed idDrawVert
	// arrays in place); indexes is 3 ints per triangle. The input buffers are staged
	// internally and freed after the build - the caller keeps ownership of its arrays.
	virtual BlasHandle	CreateBlas( const float *positions, int numVerts, int posStride,
	                                const int *indexes, int numIndexes ) { return 0; }
	virtual void	DestroyBlas( BlasHandle blas ) {}
	struct RtInstance {
		float			transform[12];		// row-major 3x4 (VkTransformMatrixKHR layout)
		BlasHandle		blas;
		unsigned int	mask;				// 8-bit ray visibility mask (0xFF = all rays)
	};
	// (Re)build the single scene TLAS over `count` instances, synchronously for now (the
	// per-frame rebuild moves onto the frame command buffer when R3 consumes it). Returns
	// the TLAS device address - what accelerationStructureEXT(uvec2) wants in a shader -
	// or 0 on failure. Replaces any previous TLAS.
	virtual unsigned long long	BuildTlas( const RtInstance *instances, int count ) { return 0; }
	// Device address of the current scene TLAS (0 = none). Reads 0 after a backend restart
	// dropped the scene - callers treat that as "rebuild needed".
	virtual unsigned long long	GetTlasAddress() { return 0; }
	// Free the TLAS and every live BLAS (level transition / shutdown).
	virtual void	DestroyRtScene() {}

	// ---- screen copies (_currentRender / _currentDepth / _scratch, Phase 4 M5) ----
	// The GL3 backend keeps the literal qglCopyTexSubImage2D path in idImage
	// (default no-ops here); the Vulkan backend implements captures as copies
	// out of the offscreen scene image.
	//
	// CreateCaptureImage allocates a sampleable copy target: RGBA8 color
	// (linear, clamp-to-edge — the capture filtering GL sets) or the scene's
	// depth format (nearest). hdrFloat makes the color target RGBA16F so an HDR
	// frame's capture (glass refraction / heat haze) keeps the un-clamped scene
	// instead of an 8-bit copy — mirrors GL3's GL_RGBA16F _currentRender in HDR.
	// Contents are undefined until the first copy.
	virtual ImageHandle	CreateCaptureImage( int w, int h, bool depth, bool hdrFloat = false ) { return 0; }
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
