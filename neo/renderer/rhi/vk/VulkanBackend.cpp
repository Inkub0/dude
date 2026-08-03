/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.h for license header)
===========================================================================
*/

// DUDE Vulkan backend — Phase 4 (docs/vulkan-backend.md).
//
// M1: instance / device / swapchain bring-up. The backend can clear the
// offscreen scene image and present it (blit → swapchain); the executor runs
// its frames as begin/clear/present only (RhiBackend.cpp vkClearOnly). Every
// draw-path entry point is a safe no-op until M2+.
//
// Architecture (decided 2026-08-02, see the milestone doc):
// - the scene always renders into an offscreen color+depth/stencil image and
//   is blitted to the swapchain at EndFrame; swapchain images stay
//   single-purpose
// - 2 frames in flight; per-slot command pool/fence/acquire semaphore,
//   per-swapchain-image release semaphore
// - r_swapInterval maps to the present mode (1+ FIFO, 0 IMMEDIATE, <0
//   FIFO_RELAXED), swapchain recreates on change/resize/out-of-date
// - Vulkan 1.1 baseline, classic render passes
//
// Compiled only with DHEWM3_VULKAN=ON (CMake source list + this #ifdef).

#ifdef DHEWM3_VULKAN

#include "sys/sys_sdl.h"
#if SDL_VERSION_ATLEAST(3, 0, 0)
  #include <SDL3/SDL_vulkan.h>
#elif SDL_VERSION_ATLEAST(2, 0, 0)
  #include <SDL_vulkan.h>
#endif

#include <vulkan/vulkan.h>

// VMA implementation lives in this TU (header-only, vendored at libs/vma/).
// We link the real loader and compile with prototypes, so VMA can call the
// API statically. The pragmas keep its (vendored) warnings out of our build.
#define VMA_STATIC_VULKAN_FUNCTIONS 1
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#define VMA_IMPLEMENTATION
#if defined(__GNUC__) || defined(__clang__)
  #pragma GCC diagnostic push
  #pragma GCC diagnostic ignored "-Wunused-parameter"
  #pragma GCC diagnostic ignored "-Wunused-variable"
  #pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include <vk_mem_alloc.h>
#if defined(__GNUC__) || defined(__clang__)
  #pragma GCC diagnostic pop
#endif

#include <vector>

#include <unordered_map>

#include "framework/FileSystem.h"
#include "renderer/tr_local.h"
#include "renderer/rhi/RHI.h"
#include "renderer/rhi/RenderParams.h"	// M6: DrawImmediate fills the generic UBO
#include "renderer/rhi/MaterialIR.h"		// IR_Purge on shader-cache lifecycle
#include "renderer/rhi/vk/VulkanImGui.h"	// M6: ImGui glue (impl at the end of this TU)

#ifndef IMGUI_DISABLE
  #include "../../../libs/imgui/imgui.h"
  #include "../../../libs/imgui/backends/imgui_impl_vulkan.h"
#endif

// dev-time validation layer (VK_LAYER_KHRONOS_validation). Default OFF: it deep-
// checks every API call and its per-descriptor-set bookkeeping scales with draw
// count, so it costs a large, draw-count-dependent chunk of frame time — a dev
// tool, not something to ship on. Enable with +set r_vkValidation 1 when chasing
// correctness (every milestone's exit bar is still "validation clean" with it on).
// Not archived: a missing layer must never stick.
static idCVar r_vkValidation( "r_vkValidation", "0", CVAR_RENDERER | CVAR_BOOL,
	"Vulkan: enable the Khronos validation layer if installed (dev; costs FPS)" );
// explicit adapter pick; -1 = auto (first discrete GPU, else first usable)
static idCVar r_vkDevice( "r_vkDevice", "-1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER,
	"Vulkan: physical device index to use (-1 = auto-select)" );
// dev bring-up aid (seeds the M6 capture path): dump the next presented scene
// image to vkdump.tga in fs_savepath, then self-reset. Blocks one frame.
static idCVar r_vkDumpNextFrame( "r_vkDumpNextFrame", "0", CVAR_RENDERER | CVAR_BOOL,
	"Vulkan: write the next frame's scene image to vkdump.tga (dev)" );

namespace rhi {

static const int FRAMES_IN_FLIGHT = 2;

/*
===============================================================================

	VulkanBackend

===============================================================================
*/

class VulkanBackend final : public RHI {
public:
	VulkanBackend() = default;		// members carry their own initializers

	// ---- lifecycle ----
	virtual bool	Init();
	virtual void	Shutdown();

	// ---- frame ----
	virtual void	BeginFrame( int windowWidth, int windowHeight );
	virtual void	EndFrame();

	// ---- passes ----
	virtual void	BeginPass( const ClearArgs *clear );
	virtual void	BeginTargetPass( RenderTargetHandle rt, const ClearArgs *clear );
	virtual void	EndPass();
	virtual void	SetViewport( int x, int y, int w, int h );
	virtual void	SetScissor( int x, int y, int w, int h );
	virtual void	SetDepthRange( float minDepth, float maxDepth );
	virtual void	SetPolygonOffset( bool enable, float factor, float units );
	virtual void	ClearStencilBuffer( int value );

	// ---- resources ----
	virtual BufferHandle	CreateBuffer( BufferUsage, int, const void * ) { return 0; }	// static VBOs: unused (CPU vertexCache); M3+
	virtual void			UpdateBuffer( BufferHandle, int, int, const void * ) {}
	virtual void			DestroyBuffer( BufferHandle ) {}
	virtual ImageHandle		CreateImage( ImageFormat, int, int, const void * ) { return 0; }	// render-target era API; M7
	virtual void			DestroyImage( ImageHandle h );
	virtual ImageHandle		CreateTexture2D( int w, int h, const void *pixels,
	                                         int textureFilter, int textureRepeat, bool allowMips );
	virtual ImageHandle		CreateTextureCube( int size, const void * const pics[6],
	                                           int textureFilter, bool allowMips );
	virtual ShaderHandle	LoadShader( const char *name );

	// M7 render-target family. Live: depth targets (2D + cube shadow maps),
	// color-only + color+depth-stencil targets (the HDR RGBA16F scene buffer and
	// its FXAA ping). Still stubbed: color+depth for the SSAO normal G-buffer /
	// SSR material buffer (return 0 → those features stay off until the SSAO slice).
	virtual RenderTargetHandle	CreateRenderTarget( ImageFormat fmt, int w, int h );
	virtual RenderTargetHandle	CreateRenderTargetCube( ImageFormat fmt, int size );
	virtual RenderTargetHandle	CreateRenderTargetColorDepth( ImageFormat, int, int, int ) { return 0; }
	virtual RenderTargetHandle	CreateRenderTargetColorDepthStencil( ImageFormat fmt, int w, int h );
	virtual void				DestroyRenderTarget( RenderTargetHandle rt );
	virtual void				SetFrameTarget( RenderTargetHandle rt );
	virtual void				BeginCubeFacePass( RenderTargetHandle rt, int face, const ClearArgs *clear );
	virtual ImageHandle			GetRenderTargetImage( RenderTargetHandle rt );
	virtual ImageHandle			GetRenderTargetImage2( RenderTargetHandle rt );

	virtual int		AllocUniforms( const void *data, int size, BufferHandle *buffer );
	virtual int		AllocVertices( const void *data, int size, BufferHandle *buffer );
	virtual int		AllocIndices( const void *data, int size, BufferHandle *buffer );
	virtual int		StreamGeneration() { return streamGen; }

	// ---- drawing ----
	virtual void	BindPipeline( const PipelineDesc &desc );
	virtual void	Draw( const DrawArgs &args );
	virtual ImageHandle	CreateCaptureImage( int w, int h, bool depth );
	virtual void	CopyFramebufferToImage( ImageHandle dst, int dstX, int dstY,
	                                        int srcX, int srcY, int w, int h, bool depth );
	virtual void	RetireImage( ImageHandle img );
	virtual void	UpdateTexture2D( ImageHandle dst, int w, int h, const void *pixels );
	virtual bool	ReadPixelsRGB( unsigned char *dest, int x, int y, int w, int h );
	virtual void	DrawImmediate( const void *verts, int numVerts, unsigned int primMode,
	                               const float mvp[16], bool textured );

private:
	bool			CreateInstance();
	bool			PickPhysicalDevice();
	bool			CreateDeviceAndVma();
	bool			CreateSwapchain();
	void			DestroySwapchain( bool destroyHandle );
	bool			CreateSceneTargets();
	void			DestroySceneTargets();
	bool			CreateFrameSlots();
	void			DestroyFrameSlots();
	bool			RecreateSwapchain();
	void			QueryDrawableSize( int &w, int &h );
	VkPresentModeKHR	PickPresentMode();
	void			FillGlConfig();

	// core objects
	VkInstance					instance = VK_NULL_HANDLE;
	VkDebugUtilsMessengerEXT	messenger = VK_NULL_HANDLE;
	VkSurfaceKHR				surface = VK_NULL_HANDLE;
	VkPhysicalDevice			physical = VK_NULL_HANDLE;
	VkPhysicalDeviceProperties	physProps = {};
	uint32_t					gfxFamily = 0;
	uint32_t					presentFamily = 0;
	VkDevice					device = VK_NULL_HANDLE;
	VkQueue						gfxQueue = VK_NULL_HANDLE;
	VkQueue						presentQueue = VK_NULL_HANDLE;
	VmaAllocator				vma = NULL;

	// swapchain
	VkSwapchainKHR				swapchain = VK_NULL_HANDLE;
	VkFormat					swapFormat = VK_FORMAT_UNDEFINED;
	VkExtent2D					swapExtent = {};
	std::vector<VkImage>		swapImages;
	std::vector<VkSemaphore>	releaseSems;	// per swapchain image
	bool						swapchainDirty = false;

	// offscreen scene target (color + depth/stencil) + its passes
	VkImage						sceneColor = VK_NULL_HANDLE;
	VmaAllocation				sceneColorAlloc = NULL;
	VkImageView					sceneColorView = VK_NULL_HANDLE;
	VkFormat					sceneDepthFormat = VK_FORMAT_UNDEFINED;
	VkImage						sceneDepth = VK_NULL_HANDLE;
	VmaAllocation				sceneDepthAlloc = NULL;
	VkImageView					sceneDepthView = VK_NULL_HANDLE;
	VkRenderPass				passClear = VK_NULL_HANDLE;	// clears color + depth/stencil
	VkRenderPass				passLoad = VK_NULL_HANDLE;	// loads everything
	VkRenderPass				passClearDS = VK_NULL_HANDLE;	// keeps color, clears depth/stencil (world-view begin)
	VkFramebuffer				sceneFb = VK_NULL_HANDLE;
	VkExtent2D					sceneExtent = {};
	bool						sceneEverWritten = false;	// false until the first clear pass after (re)create

	// frames in flight
	struct FrameSlot {
		VkCommandPool	pool = VK_NULL_HANDLE;
		VkCommandBuffer	cb = VK_NULL_HANDLE;
		VkFence			fence = VK_NULL_HANDLE;
		VkSemaphore		acquireSem = VK_NULL_HANDLE;
	};
	FrameSlot					frames[FRAMES_IN_FLIGHT];
	int							frameIndex = 0;
	uint32_t					imageIndex = 0;

	// per-frame state
	bool						frameOpen = false;
	bool						skipFrame = false;			// minimized / acquire failed
	bool						insideScenePass = false;
	bool						sceneWritten = false;
	int							fakePassDepth = 0;			// balanced no-op target passes
	int							presentedFrames = 0;		// session diagnostic (shutdown summary)

	// ================= M2: rings / shaders / pipelines / images =================

	// M2 helpers
	struct RingBuf;					// defined below (member struct, not ::rhi::RingBuf)
	bool			CreateM2Resources();
	void			DestroyM2Resources();
	void			EnsureScenePass();
	int				AllocFromRing( RingBuf &ring, const void *data, int size, int align,
	                               int wrapReserve, BufferHandle *buffer );
	VkSampler		GetSampler( int textureFilter, int textureRepeat, bool hasMips );
	VkPipeline		GetPipeline( const PipelineDesc &desc );
	VkBuffer		LookupBuffer( BufferHandle h ) const {
		return ( h >= 1 && h <= (BufferHandle)bufferTable.size() ) ? bufferTable[h - 1] : VK_NULL_HANDLE;
	}

	// per-draw uniform slice ceiling: ArbParams (1536 B) is the largest block;
	// the dynamic-UBO descriptor's fixed range must cover whatever a shader
	// reads past the dynamic offset
	static const int MAX_UNIFORM_SLICE = 2048;
	static const int UBO_RING_SIZE  = 16 << 20;		// per frame slot (GL3 sizes)
	static const int VERT_RING_SIZE = 4 << 20;
	static const int IDX_RING_SIZE  = 2 << 20;
	static const int STAGING_RING_SIZE = 2 << 20;	// mid-frame texture updates (cinematics)
	static const int MAX_FRAME_SETS = 4096;			// per-draw texture sets per frame

	struct RingBuf {
		VkBuffer		buffer = VK_NULL_HANDLE;
		VmaAllocation	alloc = NULL;
		byte *			mapped = NULL;
		int				size = 0;
		int				offset = 0;
		BufferHandle	handle = 0;
		VkBufferUsageFlags	usage = 0;	// growth (GrowRing) recreates with the same usage
	};
	RingBuf						uboRing[FRAMES_IN_FLIGHT];
	RingBuf						vertRing[FRAMES_IN_FLIGHT];
	RingBuf						idxRing[FRAMES_IN_FLIGHT];
	// M5: transfer-source ring for mid-frame texture updates (cinematic
	// frames); grows like the geometry rings. A 512x512 RGB video frame is
	// 1 MB, so 2 MB covers the common case without growth.
	RingBuf						stagingRing[FRAMES_IN_FLIGHT];
	int							streamGen = 0;
	int							uboAlign = 256;
	bool						ringOverflowWarned = false;
	// geometry rings grow on mid-frame overflow instead of wrapping (a wrap
	// stomps data in-flight draws still read — M4's "flying triangles"). The
	// old buffer must outlive this slot's frame: destroyed after the slot's
	// next fence wait in BeginFrame.
	struct RetiredRing {
		BufferHandle	handle;
		VkBuffer		buffer;
		VmaAllocation	alloc;
	};
	std::vector<RetiredRing>	retiredRings[FRAMES_IN_FLIGHT];
	bool						GrowRing( RingBuf &ring, int minSize );
	void						DrainRetiredRings( int slot );

	std::vector<VkBuffer>		bufferTable;			// handle = index + 1

	struct ImageRec {
		VkImage			image = VK_NULL_HANDLE;
		VmaAllocation	alloc = NULL;
		VkImageView		view = VK_NULL_HANDLE;
		VkSampler		sampler = VK_NULL_HANDLE;		// borrowed from the sampler cache
		bool			live = false;
		// M5 capture/cinematic images transition between transfer-dst and
		// shader-read mid-frame; regular textures stay SHADER_READ_ONLY.
		// UNDEFINED means never written (first transition discards).
		VkImageLayout	layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		bool			isDepth = false;				// capture: scene depth format
		// a color render target (CreateColorTarget) is stored top-down like the
		// scene, unlike the bottom-up M5 captures — so a fullscreen pass sampling it
		// with the shared GL quad needs the negative-height flip cancelled (Draw).
		bool			isColorTarget = false;
		int				width = 0, height = 0;
	};
	std::vector<ImageRec>		imageTable;				// handle = index + 1

	// M5: mid-frame image destruction (capture/cinematic reallocation). The
	// handle's slot is cleared immediately; the Vulkan objects live until this
	// frame slot's next fence wait (same lifetime rule as retiredRings).
	struct RetiredImage {
		VkImage			image;
		VmaAllocation	alloc;
		VkImageView		view;
	};
	std::vector<RetiredImage>	retiredImages[FRAMES_IN_FLIGHT];
	void						DrainRetiredImages( int slot );

	struct ShaderRec {
		idStr			name;
		VkShaderModule	vert = VK_NULL_HANDLE;
		VkShaderModule	frag = VK_NULL_HANDLE;
		bool			failed = false;
	};
	std::vector<ShaderRec>		shaderTable;			// handle = index + 1

	std::vector<std::pair<unsigned int, VkSampler> >	samplerCache;	// key = filter|repeat|mips

	// descriptor model (prelude.vk.glsl): set 0 = per-draw UBO (dynamic
	// offset), set 1 = combined image samplers, units 0-7 + cube at 8
	VkDescriptorSetLayout		setLayoutUbo = VK_NULL_HANDLE;
	VkDescriptorSetLayout		setLayoutTex = VK_NULL_HANDLE;
	VkPipelineLayout			pipeLayout = VK_NULL_HANDLE;
	VkDescriptorPool			persistentPool = VK_NULL_HANDLE;	// holds the per-slot set-0s
	VkDescriptorSet				uboSet[FRAMES_IN_FLIGHT] = {};
	VkDescriptorPool			framePool[FRAMES_IN_FLIGHT] = {};	// per-draw set-1s, reset per frame
	VkDescriptorPool			texturePool = VK_NULL_HANDLE;	// persistent pool for repeated texture combos
	// Cross-frame texture-set cache, keyed on the referenced ImageHandles. Those
	// handles are recycled (shadow-map eviction frees + reuses imageTable slots,
	// RetireImage the same for captures/cinematics), so a set cached in an earlier
	// frame can end up referencing a VkImageView that was destroyed and its handle
	// reassigned. InvalidateTextureSets() drops the whole cache whenever any handle
	// is retired; the sets themselves are deferred-freed a frame slot later (they
	// may still be bound in in-flight command buffers, same lifetime rule as
	// retiredImages/retiredTargets), so no live command buffer ever loses its set.
	std::unordered_map<uint64_t, VkDescriptorSet> textureSetCache;
	std::vector<VkDescriptorSet> retiredTexSets[FRAMES_IN_FLIGHT];
	void						InvalidateTextureSets();
	bool						framePoolWarned = false;
	ImageHandle					dummyImage = 0;			// 1x1 white for unused sampler slots
	ImageHandle					dummyCube = 0;			// 1x1 white cube (samplerCube slots)

	// M4: shaders statically use sampler2DShadow / samplerCubeShadow (unit 7/8
	// of interaction.frag) even when the runtime path never samples them, so
	// empty slots need depth-format views with compare-enabled samplers.
	struct ShadowDummy {
		VkImage			image = VK_NULL_HANDLE;
		VmaAllocation	alloc = NULL;
		VkImageView		view = VK_NULL_HANDLE;
		VkSampler		sampler = VK_NULL_HANDLE;	// owned (compare sampler, not in the cache)
	};
	ShadowDummy					dummyShadow2D;
	ShadowDummy					dummyShadowCube;
	bool						CreateShadowDummy( ShadowDummy &d, bool cube );
	void						DestroyShadowDummy( ShadowDummy &d );

	// ================= M7: offscreen render targets =================
	// Depth-only targets for shadow maps: a 2D depth image (projected/spot
	// lights) or a 6-face cube depth image (point lights). Rendered into with
	// BeginTargetPass / BeginCubeFacePass and sampled through the ordinary
	// DrawArgs path — GetRenderTargetImage registers a sampleable ImageRec with
	// a depth-compare sampler (unit 7 = 2D map, unit 8 = cube map), so the
	// descriptor writer needs no shadow-specific code. The color / color+depth
	// (+stencil) variants that SSAO, SSR and the HDR frame target need are not
	// built yet (their Create* methods still return 0).
	struct RenderTarget {
		bool			live = false;
		bool			cube = false;
		int				w = 0, h = 0;
		VkFormat		depthFormat = VK_FORMAT_UNDEFINED;
		VkImage			depthImage = VK_NULL_HANDLE;
		VmaAllocation	depthAlloc = NULL;
		VkImageView		sampleView = VK_NULL_HANDLE;	// 2D depth / cube view (sampled)
		VkImageView		faceView[6] = {};				// per-face single-layer views (rendered into)
		VkRenderPass	pass = VK_NULL_HANDLE;			// depth clear → shader-read
		VkFramebuffer	fb[6] = {};						// [0] = 2D; [0..5] = cube faces
		ImageHandle		sampleImage = 0;				// imageTable handle GetRenderTargetImage returns

		// ---- M7 color targets (HDR scene buffer, later SSAO/SSR) ----
		// A color target carries 1-2 sampleable color attachments (colorImage[]),
		// each registered as a SHADER_READ_ONLY ImageRec (colorSampleImage[]), plus
		// an optional depth(-stencil) attachment. Two usage shapes share the fields:
		//   * frame target (SetFrameTarget): the whole scene renders into it across
		//     several BeginPass/EndPass passes, so it needs clear/load/clearDS pass
		//     variants + everWritten tracking, exactly like the swapchain sceneColor.
		//   * nested target (BeginTargetPass): one begin→draw→EndPass fullscreen pass,
		//     served by colorClearPass alone.
		bool			colorTarget = false;
		int				colorCount = 0;					// 1 or 2 color attachments
		VkFormat		colorFormat = VK_FORMAT_UNDEFINED;
		VkImage			colorImage[2] = {};
		VmaAllocation	colorAlloc[2] = {};
		VkImageView		colorView[2] = {};				// attachment + sample view (same view)
		ImageHandle		colorSampleImage[2] = { 0, 0 };	// handles GetRenderTargetImage / 2 return
		bool			hasDepth = false;				// depth or depth-stencil attachment present
		VkImage			dsImage = VK_NULL_HANDLE;
		VmaAllocation	dsAlloc = NULL;
		VkImageView		dsView = VK_NULL_HANDLE;
		VkRenderPass	colorClearPass = VK_NULL_HANDLE;	// clear color(+ds) → shader-read
		VkRenderPass	colorLoadPass = VK_NULL_HANDLE;		// load color(+ds) → shader-read (frame target only)
		VkRenderPass	colorClearDSPass = VK_NULL_HANDLE;	// keep color, clear depth-stencil (world-view begin)
		VkFramebuffer	colorFb = VK_NULL_HANDLE;
		bool			everWritten = false;			// false until this target's first clear pass this session
		uint8_t			passClass = 0;					// pipeline-key discriminator (format signature)
	};
	std::vector<RenderTarget>	targetTable;			// handle = index + 1
	VkSampler					shadowCompareSampler = VK_NULL_HANDLE;	// shared LINEAR + LEQUAL compare
	// one shared depth-only render pass (D32_SFLOAT, clear→shader-read); every
	// depth target's framebuffer and every shadow-map pipeline is built against
	// it (2D + cube faces share the structure, so all are render-pass compatible)
	VkRenderPass				shadowPass = VK_NULL_HANDLE;
	bool						EnsureShadowPass();
	// Shadow-cache eviction/reallocation destroys targets mid-frame (the cube/2D
	// caches, RhiWorld.cpp), so target destruction is deferred to this slot's next
	// fence wait — same lifetime rule as retiredImages/retiredRings.
	std::vector<RenderTarget>	retiredTargets[FRAMES_IN_FLIGHT];
	RenderTarget *				LookupTarget( RenderTargetHandle h ) {
		return ( h >= 1 && h <= (RenderTargetHandle)targetTable.size() && targetTable[h - 1].live )
			? &targetTable[h - 1] : NULL;
	}
	int				AllocTargetSlot();			// index of a free targetTable slot (grows the table if needed)
	bool			CreateDepthTarget( RenderTarget &t, int w, int h, bool cube );
	// color target: colorCount 1-2 sampleable color attachments (colorFmt), plus a
	// depth-stencil attachment when wantDepthStencil. frameCapable builds the extra
	// load/clearDS pass variants a SetFrameTarget scene buffer needs (HDR).
	bool			CreateColorTarget( RenderTarget &t, int w, int h, VkFormat colorFmt,
	                                   int colorCount, bool wantDepthStencil, bool frameCapable );
	bool			BuildColorPasses( RenderTarget &t, bool frameCapable );
	void			FreeTargetObjects( RenderTarget &t );	// frees VK objects only (not the imageTable slot)
	void			ReleaseTargetSampleSlots( RenderTarget &t );	// frees the imageTable slots a target lent out
	// shared tail of BeginTargetPass / BeginCubeFacePass: retarget viewport + pipeline pass
	void			EnterTargetPass( int w, int h, bool flipY, VkRenderPass pipePass, uint8_t passClass, int colorAtt );
	void			DrainRetiredTargets( int slot );
	void			DestroyAllTargets();
	VkSampler		ShadowCompareSampler();
	// classify a color target's format signature into the pipeline-key pass class,
	// so scene pipelines built for the RGBA8 swapchain path and the RGBA16F HDR
	// path stay distinct cache entries (render-pass-incompatible attachment formats)
	uint8_t			PassClassFor( VkFormat colorFmt, bool hasDepth, int colorCount ) const;

	// SetFrameTarget: 0 = the swapchain sceneColor path (default); otherwise the
	// scene renders into this color target (the HDR RGBA16F scene buffer). BeginPass
	// and the shadow/AA nested passes' EndPass resume route through it.
	RenderTargetHandle			frameTarget = 0;
	// render pass + key-class the next pipeline is built for. Updated whenever the
	// active draw destination changes (scene target, frame target, nested target).
	VkRenderPass				curPipelinePass = VK_NULL_HANDLE;	// canonical pass to build against
	uint8_t						curPassClass = 0;
	int							curColorAtt = 1;	// color attachments of the active pass (0 = depth-only shadow, 2 = MRT)
	bool						lastEffFlipY = true;	// effective Y-flip last applied to the viewport (post passes cancel it)
	// the color/depth images the current frame target resolves to, for the M5
	// _currentRender / _currentDepth captures (sceneColor by default, the HDR
	// buffer while a frame target is set). Layout differs: sceneColor sits in
	// TRANSFER_SRC between passes, a sampled color target in SHADER_READ_ONLY.
	VkImage			FrameColorImage() const;
	VkImageLayout	FrameColorBetweenLayout() const;
	VkImage			FrameDepthImage() const;

	// device features actually enabled (queried before device creation)
	bool						haveAnisotropy = false;
	bool						haveFillModeNonSolid = false;

	std::unordered_map<unsigned long long, VkPipeline>	pipelineCache;
	PipelineDesc				currentDesc;
	VkPipeline					boundPipeline = VK_NULL_HANDLE;
	uint64_t					boundTexKey = 0;
	VkDescriptorSet				boundTexSet = VK_NULL_HANDLE;
	bool						dynStateDirty = true;	// (re)emit viewport+scissor before next draw
	float						depthRangeMin = 0.0f;	// SetDepthRange window (weapon/model depth hacks)
	float						depthRangeMax = 1.0f;
	float						polyOfsFactor = 0.0f;	// SetPolygonOffset (shadow volumes); dynamic depth bias
	float						polyOfsUnits = 0.0f;
	int							vpRect[4] = { 0, 0, 0, 0 };	// GL-convention viewport (origin bottom-left)
	int							scRect[4] = { 0, 0, 0, 0 };
	// M7: while rendering into an offscreen target (shadow map) the viewport is
	// NOT Y-flipped (unlike the scene pass) and uses the target's height, so the
	// projective shadow write/read stays self-consistent — matching GL exactly.
	bool						insideTargetPass = false;
	int							curRenderH = 0;			// viewport/scissor height of the active pass
	bool						curFlipY = true;		// scene = flipped; offscreen target = not
	int							savedVpRect[4] = { 0, 0, 0, 0 };	// restored by the target's EndPass
	int							savedScRect[4] = { 0, 0, 0, 0 };

	// synchronous upload plumbing (image staging at load time)
	VkCommandPool				uploadPool = VK_NULL_HANDLE;
	VkCommandBuffer				uploadCb = VK_NULL_HANDLE;
	VkFence						uploadFence = VK_NULL_HANDLE;

public:
	// ================= M6: ImGui on Vulkan (VulkanImGui.h glue) =================
	// Renders into the swapchain image between the scene blit and present, so
	// ImGui stays out of screenshots (they read the scene image), like GL.
	bool						ImGuiInit();
	void						ImGuiShutdown();
	bool						ImGuiUp() const { return imguiUp; }
	void						ImGuiSetDrawData( void *dd ) { imguiDrawData = dd; }
private:
	bool						CreateImGuiTargets();	// pass + per-swap-image views/framebuffers
	void						DestroyImGuiTargets();
	VkRenderPass				imguiPass = VK_NULL_HANDLE;
	VkFormat					imguiPassFormat = VK_FORMAT_UNDEFINED;
	std::vector<VkImageView>	imguiViews;
	std::vector<VkFramebuffer>	imguiFbs;
	bool						imguiUp = false;
	void *						imguiDrawData = NULL;	// ImDrawData* for this frame
};

static VulkanBackend vkBackend;

RHI *GetVulkanRHI() {
	return &vkBackend;
}

// identity strings glConfig points at (glConfig keeps const char* only)
static char vkVendorStr[64];
static char vkRendererStr[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE + 16];
static char vkVersionStr[128];

static int vkValidationErrors = 0;

/*
====================
vkCheck
====================
*/
static bool vkCheck( VkResult res, const char *what ) {
	if ( res == VK_SUCCESS ) {
		return true;
	}
	common->Warning( "VK: %s failed (VkResult %d)", what, (int)res );
	return false;
}

/*
====================
VulkanDebugCallback
====================
*/
static VKAPI_ATTR VkBool32 VKAPI_CALL VulkanDebugCallback(
		VkDebugUtilsMessageSeverityFlagBitsEXT severity,
		VkDebugUtilsMessageTypeFlagsEXT types,
		const VkDebugUtilsMessengerCallbackDataEXT *data,
		void *userData ) {
	// loader chatter (e.g. a broken third-party implicit layer being skipped)
	// is not our API usage — report it, but don't count it against the
	// "validation clean" bar that gates every milestone
	const bool loaderMsg = ( data->pMessageIdName != NULL
		&& idStr::Icmp( data->pMessageIdName, "Loader Message" ) == 0 );
	if ( ( severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ) && !loaderMsg ) {
		vkValidationErrors++;
		common->Warning( "VK validation ERROR: %s", data->pMessage );
	} else {
		common->Warning( "VK %s: %s", loaderMsg ? "loader" : "validation", data->pMessage );
	}
	return VK_FALSE;
}

/*
====================
VulkanBackend::CreateInstance
====================
*/
bool VulkanBackend::CreateInstance() {
	SDL_Window *win = (SDL_Window *)GLimp_GetSDLWindow();
	if ( win == NULL ) {
		common->Warning( "VK: no SDL window - GLimp_Init must run first" );
		return false;
	}

	// instance-level Vulkan version — the loader must give us 1.4. If it can't,
	// we bail and R_InitOpenGL falls back to a GL backend (no crash); the whole
	// backend targets a single 1.4 baseline rather than juggling older versions.
	uint32_t instVersion = VK_API_VERSION_1_0;
	vkEnumerateInstanceVersion( &instVersion );
	if ( instVersion < VK_API_VERSION_1_4 ) {
		common->Warning( "VK: loader only supports Vulkan %u.%u (baseline is 1.4) - update your Vulkan runtime/drivers",
			VK_API_VERSION_MAJOR( instVersion ), VK_API_VERSION_MINOR( instVersion ) );
		return false;
	}

	// surface extensions from SDL
	std::vector<const char *> exts;
#if SDL_VERSION_ATLEAST(3, 0, 0)
	{
		Uint32 extCount = 0;
		const char * const * sdlExts = SDL_Vulkan_GetInstanceExtensions( &extCount );
		if ( sdlExts == NULL ) {
			common->Warning( "VK: SDL_Vulkan_GetInstanceExtensions failed: %s", SDL_GetError() );
			return false;
		}
		for ( Uint32 i = 0; i < extCount; i++ ) {
			exts.push_back( sdlExts[i] );
		}
	}
#else
	{
		unsigned int extCount = 0;
		if ( !SDL_Vulkan_GetInstanceExtensions( win, &extCount, NULL ) ) {
			common->Warning( "VK: SDL_Vulkan_GetInstanceExtensions failed: %s", SDL_GetError() );
			return false;
		}
		exts.resize( extCount );
		SDL_Vulkan_GetInstanceExtensions( win, &extCount, exts.data() );
	}
#endif

	// validation layer, if requested and installed
	bool wantValidation = r_vkValidation.GetBool();
	bool haveValidation = false;
	const char *validationLayer = "VK_LAYER_KHRONOS_validation";
	if ( wantValidation ) {
		uint32_t layerCount = 0;
		vkEnumerateInstanceLayerProperties( &layerCount, NULL );
		std::vector<VkLayerProperties> layers( layerCount );
		vkEnumerateInstanceLayerProperties( &layerCount, layers.data() );
		for ( uint32_t i = 0; i < layerCount; i++ ) {
			if ( idStr::Cmp( layers[i].layerName, validationLayer ) == 0 ) {
				haveValidation = true;
				break;
			}
		}
		if ( haveValidation ) {
			exts.push_back( VK_EXT_DEBUG_UTILS_EXTENSION_NAME );
		} else {
			common->Printf( "VK: validation requested (r_vkValidation) but %s is not installed\n", validationLayer );
		}
	}

	VkApplicationInfo app = {};
	app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app.pApplicationName = "DUDE";
	app.pEngineName = "DUDE";
	app.apiVersion = VK_API_VERSION_1_4;

	VkInstanceCreateInfo ici = {};
	ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	ici.pApplicationInfo = &app;
	ici.enabledExtensionCount = (uint32_t)exts.size();
	ici.ppEnabledExtensionNames = exts.data();
	if ( haveValidation ) {
		ici.enabledLayerCount = 1;
		ici.ppEnabledLayerNames = &validationLayer;
	}

	if ( !vkCheck( vkCreateInstance( &ici, NULL, &instance ), "vkCreateInstance" ) ) {
		return false;
	}

	if ( haveValidation ) {
		PFN_vkCreateDebugUtilsMessengerEXT createMessenger =
			(PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr( instance, "vkCreateDebugUtilsMessengerEXT" );
		if ( createMessenger ) {
			VkDebugUtilsMessengerCreateInfoEXT mci = {};
			mci.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
			mci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
			                    | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
			mci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
			                | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
			                | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
			mci.pfnUserCallback = VulkanDebugCallback;
			createMessenger( instance, &mci, NULL, &messenger );
		}
	}

	common->Printf( "VK: instance created (loader Vulkan %u.%u.%u, validation %s)\n",
		VK_API_VERSION_MAJOR( instVersion ), VK_API_VERSION_MINOR( instVersion ),
		VK_API_VERSION_PATCH( instVersion ),
		haveValidation ? "ON" : "off" );

	// surface
#if SDL_VERSION_ATLEAST(3, 0, 0)
	if ( !SDL_Vulkan_CreateSurface( win, instance, NULL, &surface ) ) {
#else
	if ( !SDL_Vulkan_CreateSurface( win, instance, &surface ) ) {
#endif
		common->Warning( "VK: SDL_Vulkan_CreateSurface failed: %s", SDL_GetError() );
		return false;
	}

	return true;
}

/*
====================
VulkanBackend::PickPhysicalDevice

Auto-pick prefers the first discrete GPU that can render to our surface;
r_vkDevice pins an enumeration index explicitly.
====================
*/
bool VulkanBackend::PickPhysicalDevice() {
	uint32_t count = 0;
	vkEnumeratePhysicalDevices( instance, &count, NULL );
	if ( count == 0 ) {
		common->Warning( "VK: no Vulkan physical devices" );
		return false;
	}
	std::vector<VkPhysicalDevice> devices( count );
	vkEnumeratePhysicalDevices( instance, &count, devices.data() );

	int best = -1;
	int bestScore = -1;
	uint32_t bestGfx = 0, bestPresent = 0;

	for ( uint32_t i = 0; i < count; i++ ) {
		VkPhysicalDeviceProperties props;
		vkGetPhysicalDeviceProperties( devices[i], &props );

		const char *type =
			props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? "discrete" :
			props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? "integrated" :
			props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ? "cpu" : "other";
		common->Printf( "VK: device %u: %s (%s, api %u.%u.%u)\n", i, props.deviceName, type,
			VK_API_VERSION_MAJOR( props.apiVersion ), VK_API_VERSION_MINOR( props.apiVersion ),
			VK_API_VERSION_PATCH( props.apiVersion ) );

		if ( props.apiVersion < VK_API_VERSION_1_4 ) {
			common->Printf( "VK:   skipped (needs Vulkan 1.4)\n" );
			continue;
		}

		// needs the swapchain extension
		uint32_t extCount = 0;
		vkEnumerateDeviceExtensionProperties( devices[i], NULL, &extCount, NULL );
		std::vector<VkExtensionProperties> exts( extCount );
		vkEnumerateDeviceExtensionProperties( devices[i], NULL, &extCount, exts.data() );
		bool hasSwapchain = false;
		for ( uint32_t e = 0; e < extCount; e++ ) {
			if ( idStr::Cmp( exts[e].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME ) == 0 ) {
				hasSwapchain = true;
				break;
			}
		}
		if ( !hasSwapchain ) {
			common->Printf( "VK:   skipped (no %s)\n", VK_KHR_SWAPCHAIN_EXTENSION_NAME );
			continue;
		}

		// queue families: graphics + present (prefer one family doing both)
		uint32_t famCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties( devices[i], &famCount, NULL );
		std::vector<VkQueueFamilyProperties> fams( famCount );
		vkGetPhysicalDeviceQueueFamilyProperties( devices[i], &famCount, fams.data() );

		int gfx = -1, present = -1;
		for ( uint32_t f = 0; f < famCount; f++ ) {
			VkBool32 canPresent = VK_FALSE;
			vkGetPhysicalDeviceSurfaceSupportKHR( devices[i], f, surface, &canPresent );
			bool isGfx = ( fams[f].queueFlags & VK_QUEUE_GRAPHICS_BIT ) != 0;
			if ( isGfx && canPresent ) {
				gfx = present = (int)f;		// combined family — done
				break;
			}
			if ( isGfx && gfx < 0 ) {
				gfx = (int)f;
			}
			if ( canPresent && present < 0 ) {
				present = (int)f;
			}
		}
		if ( gfx < 0 || present < 0 ) {
			common->Printf( "VK:   skipped (no graphics+present queues for this surface)\n" );
			continue;
		}

		int score = 1;
		if ( props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ) {
			score = 3;
		} else if ( props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ) {
			score = 2;
		}

		if ( r_vkDevice.GetInteger() == (int)i ) {
			score = 1000;	// explicit user pick wins
		}

		if ( score > bestScore ) {
			bestScore = score;
			best = (int)i;
			bestGfx = (uint32_t)gfx;
			bestPresent = (uint32_t)present;
		}
	}

	if ( best < 0 ) {
		common->Warning( "VK: no usable Vulkan device (need 1.1 + swapchain + surface support)" );
		return false;
	}
	if ( r_vkDevice.GetInteger() >= 0 && r_vkDevice.GetInteger() != best ) {
		common->Warning( "VK: r_vkDevice %d is not usable; using device %d", r_vkDevice.GetInteger(), best );
	}

	physical = devices[best];
	gfxFamily = bestGfx;
	presentFamily = bestPresent;
	vkGetPhysicalDeviceProperties( physical, &physProps );
	common->Printf( "VK: using device %d: %s (queue families: gfx %u, present %u)\n",
		best, physProps.deviceName, gfxFamily, presentFamily );
	return true;
}

/*
====================
VulkanBackend::CreateDeviceAndVma
====================
*/
bool VulkanBackend::CreateDeviceAndVma() {
	float prio = 1.0f;
	VkDeviceQueueCreateInfo queues[2] = {};
	uint32_t queueCount = 0;

	queues[queueCount].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queues[queueCount].queueFamilyIndex = gfxFamily;
	queues[queueCount].queueCount = 1;
	queues[queueCount].pQueuePriorities = &prio;
	queueCount++;
	if ( presentFamily != gfxFamily ) {
		queues[queueCount] = queues[0];
		queues[queueCount].queueFamilyIndex = presentFamily;
		queueCount++;
	}

	const char *devExts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

	// optional features the M2+ paths use when present: sampler anisotropy
	// (TF_DEFAULT textures) and line-fill mode (GLS_POLYMODE_LINE debug draws)
	VkPhysicalDeviceFeatures supported = {};
	vkGetPhysicalDeviceFeatures( physical, &supported );
	VkPhysicalDeviceFeatures enabled = {};
	haveAnisotropy = supported.samplerAnisotropy == VK_TRUE;
	haveFillModeNonSolid = supported.fillModeNonSolid == VK_TRUE;
	enabled.samplerAnisotropy = haveAnisotropy ? VK_TRUE : VK_FALSE;
	enabled.fillModeNonSolid = haveFillModeNonSolid ? VK_TRUE : VK_FALSE;
	// zfill.vert always writes gl_ClipDistance[0] (subview near clip; zero
	// plane when unused) — universal on desktop
	if ( supported.shaderClipDistance ) {
		enabled.shaderClipDistance = VK_TRUE;
	} else {
		common->Warning( "VK: device lacks shaderClipDistance - the depth prepass shader may fail" );
	}

	// discard in fragment shaders compiles to OpDemoteToHelperInvocation under
	// the vulkan1.4 SPIR-V target (modern helper-invocation semantics rather
	// than the old OpKill); the capability needs shaderDemoteToHelperInvocation,
	// a core + required feature since Vulkan 1.3, so guaranteed on our 1.4 floor.
	VkPhysicalDeviceVulkan13Features supported13 = {};
	supported13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
	VkPhysicalDeviceFeatures2 supported2 = {};
	supported2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	supported2.pNext = &supported13;
	vkGetPhysicalDeviceFeatures2( physical, &supported2 );

	VkPhysicalDeviceVulkan13Features enabled13 = {};
	enabled13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
	if ( supported13.shaderDemoteToHelperInvocation ) {
		enabled13.shaderDemoteToHelperInvocation = VK_TRUE;
	} else {
		common->Warning( "VK: device lacks shaderDemoteToHelperInvocation - alpha-tested (discard) shaders may fail" );
	}

	VkDeviceCreateInfo dci = {};
	dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	dci.pNext = &enabled13;
	dci.queueCreateInfoCount = queueCount;
	dci.pQueueCreateInfos = queues;
	dci.enabledExtensionCount = 1;
	dci.ppEnabledExtensionNames = devExts;
	dci.pEnabledFeatures = &enabled;

	if ( !vkCheck( vkCreateDevice( physical, &dci, NULL, &device ), "vkCreateDevice" ) ) {
		return false;
	}
	vkGetDeviceQueue( device, gfxFamily, 0, &gfxQueue );
	vkGetDeviceQueue( device, presentFamily, 0, &presentQueue );

	VmaAllocatorCreateInfo aci = {};
	aci.physicalDevice = physical;
	aci.device = device;
	aci.instance = instance;
	aci.vulkanApiVersion = VK_API_VERSION_1_4;
	if ( !vkCheck( vmaCreateAllocator( &aci, &vma ), "vmaCreateAllocator" ) ) {
		return false;
	}
	return true;
}

/*
====================
VulkanBackend::QueryDrawableSize
====================
*/
void VulkanBackend::QueryDrawableSize( int &w, int &h ) {
	SDL_Window *win = (SDL_Window *)GLimp_GetSDLWindow();
	w = h = 0;
	if ( win == NULL ) {
		return;
	}
#if SDL_VERSION_ATLEAST(3, 0, 0)
	SDL_GetWindowSizeInPixels( win, &w, &h );
#else
	SDL_Vulkan_GetDrawableSize( win, &w, &h );
#endif
}

/*
====================
VulkanBackend::PickPresentMode

r_swapInterval semantics carried over from GL: 1+ = vsync (FIFO, always
available), 0 = off (IMMEDIATE, else MAILBOX, else FIFO), negative = adaptive
(FIFO_RELAXED, else FIFO).
====================
*/
VkPresentModeKHR VulkanBackend::PickPresentMode() {
	uint32_t count = 0;
	vkGetPhysicalDeviceSurfacePresentModesKHR( physical, surface, &count, NULL );
	std::vector<VkPresentModeKHR> modes( count );
	vkGetPhysicalDeviceSurfacePresentModesKHR( physical, surface, &count, modes.data() );

	const int interval = r_swapInterval.GetInteger();
	VkPresentModeKHR want[3];
	int wantCount = 0;
	if ( interval == 0 ) {
		want[wantCount++] = VK_PRESENT_MODE_IMMEDIATE_KHR;
		want[wantCount++] = VK_PRESENT_MODE_MAILBOX_KHR;
	} else if ( interval < 0 ) {
		want[wantCount++] = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
	}
	want[wantCount++] = VK_PRESENT_MODE_FIFO_KHR;		// guaranteed

	for ( int i = 0; i < wantCount; i++ ) {
		for ( uint32_t m = 0; m < count; m++ ) {
			if ( modes[m] == want[i] ) {
				return want[i];
			}
		}
	}
	return VK_PRESENT_MODE_FIFO_KHR;
}

/*
====================
VulkanBackend::CreateSwapchain
====================
*/
bool VulkanBackend::CreateSwapchain() {
	VkSurfaceCapabilitiesKHR caps;
	if ( !vkCheck( vkGetPhysicalDeviceSurfaceCapabilitiesKHR( physical, surface, &caps ),
	               "vkGetPhysicalDeviceSurfaceCapabilitiesKHR" ) ) {
		return false;
	}

	// extent: surface-decided, else the drawable size clamped to the caps
	VkExtent2D extent = caps.currentExtent;
	if ( extent.width == 0xFFFFFFFFu ) {
		int w, h;
		QueryDrawableSize( w, h );
		uint32_t uw = w > 0 ? (uint32_t)w : 0;
		uint32_t uh = h > 0 ? (uint32_t)h : 0;
		extent.width  = uw < caps.minImageExtent.width  ? caps.minImageExtent.width
		              : ( uw > caps.maxImageExtent.width  ? caps.maxImageExtent.width  : uw );
		extent.height = uh < caps.minImageExtent.height ? caps.minImageExtent.height
		              : ( uh > caps.maxImageExtent.height ? caps.maxImageExtent.height : uh );
	}
	if ( extent.width == 0 || extent.height == 0 ) {
		// minimized: nothing to create; BeginFrame skips until we have area
		swapExtent = extent;
		return true;
	}

	// format: prefer BGRA8 UNORM + sRGB colorspace (the LDR pipeline stays
	// non-sRGB-encoded, matching GL's default framebuffer)
	uint32_t fmtCount = 0;
	vkGetPhysicalDeviceSurfaceFormatsKHR( physical, surface, &fmtCount, NULL );
	std::vector<VkSurfaceFormatKHR> formats( fmtCount );
	vkGetPhysicalDeviceSurfaceFormatsKHR( physical, surface, &fmtCount, formats.data() );
	VkSurfaceFormatKHR pick = formats[0];
	for ( uint32_t i = 0; i < fmtCount; i++ ) {
		if ( ( formats[i].format == VK_FORMAT_B8G8R8A8_UNORM || formats[i].format == VK_FORMAT_R8G8B8A8_UNORM )
			&& formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR ) {
			pick = formats[i];
			break;
		}
	}

	if ( ( caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT ) == 0 ) {
		common->Warning( "VK: surface doesn't support TRANSFER_DST (blit presentation)" );
		return false;
	}

	uint32_t imageCount = caps.minImageCount + 1;
	if ( caps.maxImageCount > 0 && imageCount > caps.maxImageCount ) {
		imageCount = caps.maxImageCount;
	}

	VkPresentModeKHR presentMode = PickPresentMode();

	VkSwapchainCreateInfoKHR sci = {};
	sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	sci.surface = surface;
	sci.minImageCount = imageCount;
	sci.imageFormat = pick.format;
	sci.imageColorSpace = pick.colorSpace;
	sci.imageExtent = extent;
	sci.imageArrayLayers = 1;
	sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	uint32_t families[2] = { gfxFamily, presentFamily };
	if ( gfxFamily != presentFamily ) {
		sci.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
		sci.queueFamilyIndexCount = 2;
		sci.pQueueFamilyIndices = families;
	} else {
		sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	}
	sci.preTransform = caps.currentTransform;
	sci.compositeAlpha = ( caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR )
		? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
	sci.presentMode = presentMode;
	sci.clipped = VK_TRUE;
	VkSwapchainKHR oldChain = swapchain;
	sci.oldSwapchain = oldChain;

	VkSwapchainKHR newChain = VK_NULL_HANDLE;
	if ( !vkCheck( vkCreateSwapchainKHR( device, &sci, NULL, &newChain ), "vkCreateSwapchainKHR" ) ) {
		return false;
	}
	// drop the per-image semaphores of the old chain, then the retired chain
	// itself (retiring via oldSwapchain does not free the handle). The callers
	// have the device idle here (Init / RecreateSwapchain), so this is safe.
	DestroySwapchain( false );
	if ( oldChain != VK_NULL_HANDLE ) {
		vkDestroySwapchainKHR( device, oldChain, NULL );
	}
	swapchain = newChain;
	swapFormat = pick.format;
	swapExtent = extent;

	uint32_t actualCount = 0;
	vkGetSwapchainImagesKHR( device, swapchain, &actualCount, NULL );
	swapImages.resize( actualCount );
	vkGetSwapchainImagesKHR( device, swapchain, &actualCount, swapImages.data() );

	releaseSems.resize( actualCount );
	for ( uint32_t i = 0; i < actualCount; i++ ) {
		VkSemaphoreCreateInfo si = {};
		si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		if ( !vkCheck( vkCreateSemaphore( device, &si, NULL, &releaseSems[i] ), "vkCreateSemaphore" ) ) {
			return false;
		}
	}

	const char *modeStr =
		presentMode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "IMMEDIATE" :
		presentMode == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX" :
		presentMode == VK_PRESENT_MODE_FIFO_RELAXED_KHR ? "FIFO_RELAXED" : "FIFO";
	common->Printf( "VK: swapchain %ux%u, %u images, %s (r_swapInterval %d)\n",
		extent.width, extent.height, actualCount, modeStr, r_swapInterval.GetInteger() );

	// M6: rebuild the ImGui swapchain targets when ImGui is (or was) up
	if ( imguiPass != VK_NULL_HANDLE && !CreateImGuiTargets() ) {
		return false;
	}
	return true;
}

/*
====================
VulkanBackend::DestroySwapchain
====================
*/
void VulkanBackend::DestroySwapchain( bool destroyHandle ) {
	DestroyImGuiTargets();		// per-swap-image views/framebuffers (pass survives)
	for ( size_t i = 0; i < releaseSems.size(); i++ ) {
		vkDestroySemaphore( device, releaseSems[i], NULL );
	}
	releaseSems.clear();
	swapImages.clear();
	if ( destroyHandle && swapchain != VK_NULL_HANDLE ) {
		vkDestroySwapchainKHR( device, swapchain, NULL );
		swapchain = VK_NULL_HANDLE;
	}
}

/*
====================
VulkanBackend::CreateSceneTargets

The offscreen scene image pair (RGBA8 color + D24/32S8 depth-stencil) at the
swapchain extent, plus the clear/load render pass variants and the framebuffer
(compatible with both variants).
====================
*/
bool VulkanBackend::CreateSceneTargets() {
	DestroySceneTargets();

	if ( swapExtent.width == 0 || swapExtent.height == 0 ) {
		return true;	// minimized; created on first real frame
	}

	// depth-stencil format: D24S8 where supported (NVIDIA), else D32S8 (AMD)
	sceneDepthFormat = VK_FORMAT_D24_UNORM_S8_UINT;
	VkFormatProperties fp;
	vkGetPhysicalDeviceFormatProperties( physical, sceneDepthFormat, &fp );
	if ( ( fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT ) == 0 ) {
		sceneDepthFormat = VK_FORMAT_D32_SFLOAT_S8_UINT;
	}

	// color image
	VkImageCreateInfo ici = {};
	ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = VK_FORMAT_R8G8B8A8_UNORM;
	ici.extent.width = swapExtent.width;
	ici.extent.height = swapExtent.height;
	ici.extent.depth = 1;
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	VmaAllocationCreateInfo vci = {};
	vci.usage = VMA_MEMORY_USAGE_AUTO;
	if ( !vkCheck( vmaCreateImage( vma, &ici, &vci, &sceneColor, &sceneColorAlloc, NULL ), "vmaCreateImage(scene color)" ) ) {
		return false;
	}

	VkImageViewCreateInfo vwi = {};
	vwi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	vwi.image = sceneColor;
	vwi.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vwi.format = ici.format;
	vwi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	vwi.subresourceRange.levelCount = 1;
	vwi.subresourceRange.layerCount = 1;
	if ( !vkCheck( vkCreateImageView( device, &vwi, NULL, &sceneColorView ), "vkCreateImageView(scene color)" ) ) {
		return false;
	}

	// depth-stencil image (TRANSFER_SRC: the M5 _currentDepth capture copy)
	ici.format = sceneDepthFormat;
	ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	if ( !vkCheck( vmaCreateImage( vma, &ici, &vci, &sceneDepth, &sceneDepthAlloc, NULL ), "vmaCreateImage(scene depth)" ) ) {
		return false;
	}
	vwi.image = sceneDepth;
	vwi.format = sceneDepthFormat;
	vwi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
	if ( !vkCheck( vkCreateImageView( device, &vwi, NULL, &sceneDepthView ), "vkCreateImageView(scene depth)" ) ) {
		return false;
	}

	// render passes: clear-all / load-all / clear-DS-keep-color variants,
	// color always ends TRANSFER_SRC for the present blit
	for ( int variant = 0; variant < 3; variant++ ) {
		const bool isClear = ( variant == 0 );
		const bool clearDS = ( variant == 2 );

		VkAttachmentDescription atts[2] = {};
		atts[0].format = VK_FORMAT_R8G8B8A8_UNORM;
		atts[0].samples = VK_SAMPLE_COUNT_1_BIT;
		atts[0].loadOp = isClear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
		atts[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		atts[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		atts[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		atts[0].initialLayout = isClear ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		atts[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

		const bool dsClears = isClear || clearDS;
		atts[1].format = sceneDepthFormat;
		atts[1].samples = VK_SAMPLE_COUNT_1_BIT;
		atts[1].loadOp = dsClears ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
		atts[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		atts[1].stencilLoadOp = dsClears ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
		atts[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
		atts[1].initialLayout = dsClears ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		atts[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

		VkAttachmentReference colorRef = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
		VkAttachmentReference depthRef = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };

		VkSubpassDescription sub = {};
		sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		sub.colorAttachmentCount = 1;
		sub.pColorAttachments = &colorRef;
		sub.pDepthStencilAttachment = &depthRef;

		// prior blit read → attachment writes; attachment writes → next blit read
		VkSubpassDependency deps[2] = {};
		deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
		deps[0].dstSubpass = 0;
		deps[0].srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
		deps[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
		                     | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		deps[1].srcSubpass = 0;
		deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
		deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		deps[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
		deps[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

		VkRenderPassCreateInfo rpi = {};
		rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
		rpi.attachmentCount = 2;
		rpi.pAttachments = atts;
		rpi.subpassCount = 1;
		rpi.pSubpasses = &sub;
		rpi.dependencyCount = 2;
		rpi.pDependencies = deps;

		VkRenderPass *dst = isClear ? &passClear : ( clearDS ? &passClearDS : &passLoad );
		if ( !vkCheck( vkCreateRenderPass( device, &rpi, NULL, dst ), "vkCreateRenderPass" ) ) {
			return false;
		}
	}

	VkImageView views[2] = { sceneColorView, sceneDepthView };
	VkFramebufferCreateInfo fbi = {};
	fbi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	fbi.renderPass = passClear;
	fbi.attachmentCount = 2;
	fbi.pAttachments = views;
	fbi.width = swapExtent.width;
	fbi.height = swapExtent.height;
	fbi.layers = 1;
	if ( !vkCheck( vkCreateFramebuffer( device, &fbi, NULL, &sceneFb ), "vkCreateFramebuffer" ) ) {
		return false;
	}

	sceneExtent = swapExtent;
	sceneEverWritten = false;
	common->Printf( "VK: scene target %ux%u RGBA8 + %s\n", swapExtent.width, swapExtent.height,
		sceneDepthFormat == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32S8" );
	return true;
}

/*
====================
VulkanBackend::DestroySceneTargets
====================
*/
void VulkanBackend::DestroySceneTargets() {
	if ( sceneFb )         { vkDestroyFramebuffer( device, sceneFb, NULL ); sceneFb = VK_NULL_HANDLE; }
	if ( passClear )       { vkDestroyRenderPass( device, passClear, NULL ); passClear = VK_NULL_HANDLE; }
	if ( passLoad )        { vkDestroyRenderPass( device, passLoad, NULL ); passLoad = VK_NULL_HANDLE; }
	if ( passClearDS )     { vkDestroyRenderPass( device, passClearDS, NULL ); passClearDS = VK_NULL_HANDLE; }
	if ( sceneColorView )  { vkDestroyImageView( device, sceneColorView, NULL ); sceneColorView = VK_NULL_HANDLE; }
	if ( sceneDepthView )  { vkDestroyImageView( device, sceneDepthView, NULL ); sceneDepthView = VK_NULL_HANDLE; }
	if ( sceneColor )      { vmaDestroyImage( vma, sceneColor, sceneColorAlloc ); sceneColor = VK_NULL_HANDLE; sceneColorAlloc = NULL; }
	if ( sceneDepth )      { vmaDestroyImage( vma, sceneDepth, sceneDepthAlloc ); sceneDepth = VK_NULL_HANDLE; sceneDepthAlloc = NULL; }
	sceneExtent.width = sceneExtent.height = 0;
}

/*
====================
VulkanBackend::CreateFrameSlots
====================
*/
bool VulkanBackend::CreateFrameSlots() {
	for ( int i = 0; i < FRAMES_IN_FLIGHT; i++ ) {
		VkCommandPoolCreateInfo pci = {};
		pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
		pci.queueFamilyIndex = gfxFamily;
		if ( !vkCheck( vkCreateCommandPool( device, &pci, NULL, &frames[i].pool ), "vkCreateCommandPool" ) ) {
			return false;
		}
		VkCommandBufferAllocateInfo cai = {};
		cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		cai.commandPool = frames[i].pool;
		cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cai.commandBufferCount = 1;
		if ( !vkCheck( vkAllocateCommandBuffers( device, &cai, &frames[i].cb ), "vkAllocateCommandBuffers" ) ) {
			return false;
		}
		VkFenceCreateInfo fci = {};
		fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
		if ( !vkCheck( vkCreateFence( device, &fci, NULL, &frames[i].fence ), "vkCreateFence" ) ) {
			return false;
		}
		VkSemaphoreCreateInfo si = {};
		si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		if ( !vkCheck( vkCreateSemaphore( device, &si, NULL, &frames[i].acquireSem ), "vkCreateSemaphore" ) ) {
			return false;
		}
	}
	return true;
}

/*
====================
VulkanBackend::DestroyFrameSlots
====================
*/
void VulkanBackend::DestroyFrameSlots() {
	for ( int i = 0; i < FRAMES_IN_FLIGHT; i++ ) {
		if ( frames[i].pool )       { vkDestroyCommandPool( device, frames[i].pool, NULL ); }
		if ( frames[i].fence )      { vkDestroyFence( device, frames[i].fence, NULL ); }
		if ( frames[i].acquireSem ) { vkDestroySemaphore( device, frames[i].acquireSem, NULL ); }
		memset( &frames[i], 0, sizeof( frames[i] ) );
	}
}

/*
====================
VulkanBackend::RecreateSwapchain

Resize / present-mode change / out-of-date. Full stop (device idle) is fine
here: this is a rare event and M-milestone code favors obviously-correct.
====================
*/
bool VulkanBackend::RecreateSwapchain() {
	vkDeviceWaitIdle( device );
	if ( !CreateSwapchain() ) {
		return false;
	}
	if ( swapExtent.width != sceneExtent.width || swapExtent.height != sceneExtent.height ) {
		if ( !CreateSceneTargets() ) {
			return false;
		}
	}
	swapchainDirty = false;
	return true;
}

/*
====================
VulkanBackend::FillGlConfig
====================
*/
void VulkanBackend::FillGlConfig() {
	const char *vendor = "unknown";
	switch ( physProps.vendorID ) {
		case 0x10DE: vendor = "NVIDIA"; break;
		case 0x1002: vendor = "AMD"; break;
		case 0x8086: vendor = "Intel"; break;
		case 0x13B5: vendor = "ARM"; break;
		case 0x5143: vendor = "Qualcomm"; break;
		case 0x10005: vendor = "Mesa (lavapipe)"; break;
	}
	idStr::snPrintf( vkVendorStr, sizeof( vkVendorStr ), "%s (0x%04x)", vendor, physProps.vendorID );
	idStr::snPrintf( vkRendererStr, sizeof( vkRendererStr ), "%s", physProps.deviceName );
	idStr::snPrintf( vkVersionStr, sizeof( vkVersionStr ), "Vulkan %u.%u.%u (driver %u.%u.%u)",
		VK_API_VERSION_MAJOR( physProps.apiVersion ), VK_API_VERSION_MINOR( physProps.apiVersion ),
		VK_API_VERSION_PATCH( physProps.apiVersion ),
		VK_API_VERSION_MAJOR( physProps.driverVersion ), VK_API_VERSION_MINOR( physProps.driverVersion ),
		VK_API_VERSION_PATCH( physProps.driverVersion ) );

	glConfig.vendor_string = vkVendorStr;
	glConfig.renderer_string = vkRendererStr;
	glConfig.version_string = vkVersionStr;
	glConfig.maxTextureSize = (int)physProps.limits.maxImageDimension2D;
	glConfig.maxCubeMapSize = (int)physProps.limits.maxImageDimensionCube;

	VkPhysicalDeviceMemoryProperties mem;
	vkGetPhysicalDeviceMemoryProperties( physical, &mem );
	VkDeviceSize local = 0;
	for ( uint32_t i = 0; i < mem.memoryHeapCount; i++ ) {
		if ( mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ) {
			local += mem.memoryHeaps[i].size;
		}
	}
	glConfig.vidMemMB = (int)( local / ( 1024 * 1024 ) );
}

/*
====================
VulkanBackend::Init

Runs with the SDL Vulkan window live (R_InitOpenGL), and again after every
vid_restart from a clean Shutdown().
====================
*/
bool VulkanBackend::Init() {
	if ( instance != VK_NULL_HANDLE ) {
		common->Warning( "VK: Init called while already initialized" );
		return true;
	}
	vkValidationErrors = 0;

	if ( !CreateInstance() || !PickPhysicalDevice() || !CreateDeviceAndVma() ) {
		Shutdown();
		return false;
	}
	if ( !CreateSwapchain() || !CreateSceneTargets() || !CreateFrameSlots() || !CreateM2Resources() ) {
		Shutdown();
		return false;
	}

	FillGlConfig();

	frameIndex = 0;
	frameOpen = skipFrame = insideScenePass = sceneWritten = false;
	fakePassDepth = 0;
	swapchainDirty = false;

	common->Printf( "VK: backend up - %s, %d MB VRAM, max tex %d\n",
		glConfig.renderer_string, glConfig.vidMemMB, glConfig.maxTextureSize );
	return true;
}

/*
====================
VulkanBackend::Shutdown

Called by RB_RHI_Shutdown() before GLimp destroys the window (vid_restart and
full shutdown), so the surface never outlives the window. Safe on a partial
init (failed bring-up) and when never initialized.
====================
*/
void VulkanBackend::Shutdown() {
	if ( instance == VK_NULL_HANDLE ) {
		return;
	}
	if ( device != VK_NULL_HANDLE ) {
		vkDeviceWaitIdle( device );
	}

	// M6: ImGui device objects must die before the device. Normally sys_imgui
	// shuts down first (it calls ImGuiShutdown through the glue); this is the
	// safety net for partial-teardown orders.
	ImGuiShutdown();
	if ( imguiPass != VK_NULL_HANDLE ) {
		vkDestroyRenderPass( device, imguiPass, NULL );
		imguiPass = VK_NULL_HANDLE;
		imguiPassFormat = VK_FORMAT_UNDEFINED;
	}

	if ( device != VK_NULL_HANDLE ) {
		DestroyM2Resources();
	}
	DestroyFrameSlots();
	DestroySceneTargets();
	DestroySwapchain( true );
	if ( vma )     { vmaDestroyAllocator( vma ); vma = NULL; }
	if ( device )  { vkDestroyDevice( device, NULL ); device = VK_NULL_HANDLE; }
	if ( messenger ) {
		PFN_vkDestroyDebugUtilsMessengerEXT destroyMessenger =
			(PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr( instance, "vkDestroyDebugUtilsMessengerEXT" );
		if ( destroyMessenger ) {
			destroyMessenger( instance, messenger, NULL );
		}
		messenger = VK_NULL_HANDLE;
	}
	if ( surface ) { vkDestroySurfaceKHR( instance, surface, NULL ); surface = VK_NULL_HANDLE; }
	vkDestroyInstance( instance, NULL );
	instance = VK_NULL_HANDLE;

	gfxQueue = presentQueue = VK_NULL_HANDLE;
	physical = VK_NULL_HANDLE;
	frameOpen = skipFrame = insideScenePass = sceneWritten = false;

	if ( vkValidationErrors ) {
		common->Warning( "VK: session had %d validation errors (%d frames presented)", vkValidationErrors, presentedFrames );
	} else {
		common->Printf( "VK: backend down (validation clean, %d frames presented)\n", presentedFrames );
	}
	presentedFrames = 0;
}

/*
====================
VulkanBackend::BeginFrame
====================
*/
void VulkanBackend::BeginFrame( int windowWidth, int windowHeight ) {
	if ( device == VK_NULL_HANDLE || frameOpen ) {
		return;
	}
	FrameSlot &f = frames[frameIndex];
	vkWaitForFences( device, 1, &f.fence, VK_TRUE, UINT64_MAX );

	// vsync toggle → new present mode; window size change → new extent
	if ( r_swapInterval.IsModified() ) {
		r_swapInterval.ClearModified();
		swapchainDirty = true;
	}
	int dw, dh;
	QueryDrawableSize( dw, dh );
	if ( (uint32_t)dw != swapExtent.width || (uint32_t)dh != swapExtent.height ) {
		swapchainDirty = true;
	}
	if ( swapchainDirty && !RecreateSwapchain() ) {
		skipFrame = true;
		frameOpen = true;
		return;
	}
	if ( swapExtent.width == 0 || swapExtent.height == 0 || swapchain == VK_NULL_HANDLE ) {
		skipFrame = true;		// minimized
		frameOpen = true;
		return;
	}

	VkResult ar = vkAcquireNextImageKHR( device, swapchain, UINT64_MAX, f.acquireSem, VK_NULL_HANDLE, &imageIndex );
	if ( ar == VK_ERROR_OUT_OF_DATE_KHR ) {
		if ( !RecreateSwapchain() ) {
			skipFrame = true;
			frameOpen = true;
			return;
		}
		ar = vkAcquireNextImageKHR( device, swapchain, UINT64_MAX, f.acquireSem, VK_NULL_HANDLE, &imageIndex );
	}
	if ( ar == VK_SUBOPTIMAL_KHR ) {
		swapchainDirty = true;			// present this frame, rebuild next
	} else if ( ar != VK_SUCCESS ) {
		vkCheck( ar, "vkAcquireNextImageKHR" );
		skipFrame = true;
		frameOpen = true;
		return;
	}

	vkResetFences( device, 1, &f.fence );
	vkResetCommandPool( device, f.pool, 0 );
	VkCommandBufferBeginInfo bi = {};
	bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer( f.cb, &bi );

	// M2: this slot's GPU work is fenced off — reset its rings (the
	// StreamGeneration contract) and its per-draw descriptor pool
	DrainRetiredRings( frameIndex );	// buffers replaced by GrowRing last time this slot ran
	DrainRetiredImages( frameIndex );	// capture/cinematic images replaced by RetireImage
	DrainRetiredTargets( frameIndex );	// shadow-map targets evicted by the shadow caches
	if ( texturePool && !retiredTexSets[frameIndex].empty() ) {
		// texture sets invalidated ~FRAMES_IN_FLIGHT frames ago: this slot's fence
		// has passed, so nothing in flight still references them — free for reuse
		vkFreeDescriptorSets( device, texturePool, (uint32_t)retiredTexSets[frameIndex].size(),
			retiredTexSets[frameIndex].data() );
		retiredTexSets[frameIndex].clear();
	}
	uboRing[frameIndex].offset = 0;
	vertRing[frameIndex].offset = 0;
	idxRing[frameIndex].offset = 0;
	stagingRing[frameIndex].offset = 0;
	streamGen++;
	if ( framePool[frameIndex] ) {
		vkResetDescriptorPool( device, framePool[frameIndex], 0 );
	}
	boundPipeline = VK_NULL_HANDLE;
	boundTexKey = 0;
	boundTexSet = VK_NULL_HANDLE;
	dynStateDirty = true;
	depthRangeMin = 0.0f;
	depthRangeMax = 1.0f;
	polyOfsFactor = 0.0f;
	polyOfsUnits = 0.0f;
	vpRect[0] = 0; vpRect[1] = 0; vpRect[2] = (int)swapExtent.width;  vpRect[3] = (int)swapExtent.height;
	scRect[0] = 0; scRect[1] = 0; scRect[2] = (int)swapExtent.width;  scRect[3] = (int)swapExtent.height;

	frameOpen = true;
	skipFrame = false;
	insideScenePass = false;
	sceneWritten = false;
	fakePassDepth = 0;
	insideTargetPass = false;
	curRenderH = (int)swapExtent.height;	// scene extent; target passes override
	curFlipY = true;
	frameTarget = 0;						// SetFrameTarget re-routes each HDR frame from scratch
	curPipelinePass = passClear;
	curPassClass = 0;
	curColorAtt = 1;
}

/*
====================
VulkanBackend::BeginPass

NULL clear = load the existing scene content (except the very first use of a
fresh scene image, where there is nothing to load yet).
====================
*/
void VulkanBackend::BeginPass( const ClearArgs *clear ) {
	if ( !frameOpen || skipFrame || insideScenePass ) {
		return;
	}
	// the scene renders into the swapchain sceneColor by default, or into the
	// active frame target (the HDR RGBA16F buffer) when SetFrameTarget set one.
	RenderTarget *ft = ( frameTarget != 0 ) ? LookupTarget( frameTarget ) : NULL;
	if ( frameTarget != 0 && ( ft == NULL || !ft->colorTarget ) ) {
		ft = NULL;			// stale/invalid handle → fall back to sceneColor
	}
	bool &everWritten = ft ? ft->everWritten : sceneEverWritten;

	// pick the pass variant by which channels the caller wants cleared: a
	// depth/stencil-only clear (world-view begin) must keep the frame's color
	const bool wantColorClear = ( clear != NULL && clear->color ) || !everWritten;
	const bool wantDsClear = ( clear != NULL && ( clear->depth || clear->stencil ) ) || !everWritten;

	VkClearValue cv[3] = {};
	const int nColor = ft ? ft->colorCount : 1;
	const bool hasDS = ft ? ft->hasDepth : true;	// sceneColor always carries depth-stencil
	if ( clear != NULL && clear->color ) {
		for ( int c = 0; c < nColor; c++ ) {
			cv[c].color.float32[0] = clear->rgba[0];
			cv[c].color.float32[1] = clear->rgba[1];
			cv[c].color.float32[2] = clear->rgba[2];
			cv[c].color.float32[3] = clear->rgba[3];
		}
	}
	if ( hasDS ) {
		cv[nColor].depthStencil.depth = 1.0f;
		cv[nColor].depthStencil.stencil = clear != NULL ? clear->stencilValue : 0;
	}

	// a frame target without the load/clearDS variants (a color-only target used
	// as a frame target — the frontend doesn't, but stay robust) falls back to clear
	VkRenderPass rpClear = ft ? ft->colorClearPass   : passClear;
	VkRenderPass rpLoad  = ( ft ? ft->colorLoadPass    : passLoad )    ? ( ft ? ft->colorLoadPass : passLoad ) : rpClear;
	VkRenderPass rpClrDS = ( ft ? ft->colorClearDSPass : passClearDS ) ? ( ft ? ft->colorClearDSPass : passClearDS ) : rpClear;

	VkRenderPassBeginInfo rbi = {};
	rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	rbi.renderPass = wantColorClear ? rpClear : ( wantDsClear ? rpClrDS : rpLoad );
	rbi.framebuffer = ft ? ft->colorFb : sceneFb;
	rbi.renderArea.extent = ft ? VkExtent2D{ (uint32_t)ft->w, (uint32_t)ft->h } : sceneExtent;
	rbi.clearValueCount = (uint32_t)( nColor + ( hasDS ? 1 : 0 ) );
	rbi.pClearValues = cv;
	vkCmdBeginRenderPass( frames[frameIndex].cb, &rbi, VK_SUBPASS_CONTENTS_INLINE );

	// the pipelines drawn in this pass target its render pass / format class
	curRenderH = ft ? ft->h : (int)sceneExtent.height;
	curFlipY = true;
	curPipelinePass = rpClear;
	curPassClass = ft ? ft->passClass : 0;
	curColorAtt = nColor;

	insideScenePass = true;
	sceneWritten = true;
	everWritten = true;
}

/*
====================
VulkanBackend::EndPass
====================
*/
void VulkanBackend::EndPass() {
	if ( insideTargetPass ) {
		// close the offscreen nested pass (shadow map / AA ping). The scene pass
		// resumes on the next Draw (EnsureScenePass → load variant) against the
		// frame target it interrupted; restore that destination's viewport class,
		// exactly like GL3's savedViewport restore.
		vkCmdEndRenderPass( frames[frameIndex].cb );
		insideTargetPass = false;
		RenderTarget *ft = ( frameTarget != 0 ) ? LookupTarget( frameTarget ) : NULL;
		if ( ft && ft->colorTarget ) {
			curRenderH = ft->h;
			curPipelinePass = ft->colorClearPass;
			curPassClass = ft->passClass;
			curColorAtt = ft->colorCount;
		} else {
			curRenderH = (int)sceneExtent.height;
			curPipelinePass = passClear;
			curPassClass = 0;
			curColorAtt = 1;
		}
		curFlipY = true;
		memcpy( vpRect, savedVpRect, sizeof( vpRect ) );
		memcpy( scRect, savedScRect, sizeof( scRect ) );
		dynStateDirty = true;
		return;
	}
	if ( fakePassDepth > 0 ) {
		fakePassDepth--;		// balanced a no-op target pass
		return;
	}
	if ( !insideScenePass ) {
		return;
	}
	vkCmdEndRenderPass( frames[frameIndex].cb );
	insideScenePass = false;
}

/*
====================
VulkanBackend::EndFrame

Blit the scene image onto the acquired swapchain image, submit, present.
====================
*/
void VulkanBackend::EndFrame() {
	if ( !frameOpen ) {
		return;
	}
	frameOpen = false;
	if ( skipFrame ) {
		skipFrame = false;
		return;			// nothing recorded; fence still signaled
	}

	FrameSlot &f = frames[frameIndex];

	// every presented frame must be defined, even a draw-less one
	if ( !sceneWritten ) {
		ClearArgs black = {};
		black.color = true;
		black.rgba[3] = 1.0f;
		frameOpen = true;		// BeginPass guards on it
		BeginPass( &black );
		frameOpen = false;
	}
	if ( insideScenePass ) {
		vkCmdEndRenderPass( f.cb );
		insideScenePass = false;
	}

	// swapchain image UNDEFINED → TRANSFER_DST
	VkImageMemoryBarrier toDst = {};
	toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	toDst.srcAccessMask = 0;
	toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toDst.image = swapImages[imageIndex];
	toDst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	toDst.subresourceRange.levelCount = 1;
	toDst.subresourceRange.layerCount = 1;
	vkCmdPipelineBarrier( f.cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		0, 0, NULL, 0, NULL, 1, &toDst );

	VkImageBlit blit = {};
	blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	blit.srcSubresource.layerCount = 1;
	blit.srcOffsets[1].x = (int32_t)sceneExtent.width;
	blit.srcOffsets[1].y = (int32_t)sceneExtent.height;
	blit.srcOffsets[1].z = 1;
	blit.dstSubresource = blit.srcSubresource;
	blit.dstOffsets[1].x = (int32_t)swapExtent.width;
	blit.dstOffsets[1].y = (int32_t)swapExtent.height;
	blit.dstOffsets[1].z = 1;
	vkCmdBlitImage( f.cb, sceneColor, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		swapImages[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
		sceneExtent.width == swapExtent.width && sceneExtent.height == swapExtent.height
			? VK_FILTER_NEAREST : VK_FILTER_LINEAR );

	// M6: ImGui draws into the swapchain image after the blit, through a
	// LOAD render pass whose finalLayout is PRESENT (screenshots read the
	// scene image, so the menus stay out of them like on GL). Without draw
	// data, the plain barrier transition to PRESENT stands.
#ifndef IMGUI_DISABLE
	if ( imguiUp && imguiDrawData != NULL && imageIndex < imguiFbs.size()
	     && imguiFbs[imageIndex] != VK_NULL_HANDLE
	     && ((ImDrawData *)imguiDrawData)->TotalVtxCount >= 0 ) {
		VkRenderPassBeginInfo rbi = {};
		rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
		rbi.renderPass = imguiPass;
		rbi.framebuffer = imguiFbs[imageIndex];
		rbi.renderArea.extent = swapExtent;
		vkCmdBeginRenderPass( f.cb, &rbi, VK_SUBPASS_CONTENTS_INLINE );
		ImGui_ImplVulkan_RenderDrawData( (ImDrawData *)imguiDrawData, f.cb );
		vkCmdEndRenderPass( f.cb );
		imguiDrawData = NULL;
	} else
#endif
	{
		// swapchain image → PRESENT
		VkImageMemoryBarrier toPresent = toDst;
		toPresent.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		toPresent.dstAccessMask = 0;
		toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		vkCmdPipelineBarrier( f.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
			0, 0, NULL, 0, NULL, 1, &toPresent );
	}
	imguiDrawData = NULL;

	// dev frame dump (r_vkDumpNextFrame): copy the scene image (still
	// TRANSFER_SRC) into a host buffer alongside the present blit
	VkBuffer dumpBuf = VK_NULL_HANDLE;
	VmaAllocation dumpAlloc = NULL;
	byte *dumpMapped = NULL;
	if ( r_vkDumpNextFrame.GetBool() ) {
		VkBufferCreateInfo bci = {};
		bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bci.size = (VkDeviceSize)sceneExtent.width * sceneExtent.height * 4;
		bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		VmaAllocationCreateInfo aci = {};
		aci.usage = VMA_MEMORY_USAGE_AUTO;
		aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
		aci.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
		VmaAllocationInfo info = {};
		if ( vmaCreateBuffer( vma, &bci, &aci, &dumpBuf, &dumpAlloc, &info ) == VK_SUCCESS ) {
			dumpMapped = (byte *)info.pMappedData;
			VkBufferImageCopy c = {};
			c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			c.imageSubresource.layerCount = 1;
			c.imageExtent = { sceneExtent.width, sceneExtent.height, 1 };
			vkCmdCopyImageToBuffer( f.cb, sceneColor, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dumpBuf, 1, &c );
		}
	}

	vkEndCommandBuffer( f.cb );

	VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	VkSubmitInfo si = {};
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.waitSemaphoreCount = 1;
	si.pWaitSemaphores = &f.acquireSem;
	si.pWaitDstStageMask = &waitStage;
	si.commandBufferCount = 1;
	si.pCommandBuffers = &f.cb;
	si.signalSemaphoreCount = 1;
	si.pSignalSemaphores = &releaseSems[imageIndex];
	vkCheck( vkQueueSubmit( gfxQueue, 1, &si, f.fence ), "vkQueueSubmit" );

	VkPresentInfoKHR pi = {};
	pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	pi.waitSemaphoreCount = 1;
	pi.pWaitSemaphores = &releaseSems[imageIndex];
	pi.swapchainCount = 1;
	pi.pSwapchains = &swapchain;
	pi.pImageIndices = &imageIndex;
	VkResult pr = vkQueuePresentKHR( presentQueue, &pi );
	if ( pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR ) {
		swapchainDirty = true;
	} else {
		vkCheck( pr, "vkQueuePresentKHR" );
	}
	presentedFrames++;

	if ( dumpBuf != VK_NULL_HANDLE ) {
		r_vkDumpNextFrame.SetBool( false );
		vkWaitForFences( device, 1, &f.fence, VK_TRUE, UINT64_MAX );
		if ( dumpMapped != NULL ) {
			// a Vulkan image is top-to-bottom; flipVertical=false sets the TGA
			// top-down flag (true is for GL's bottom-up readbacks)
			R_WriteTGA( "vkdump.tga", dumpMapped, (int)sceneExtent.width, (int)sceneExtent.height, false );
			common->Printf( "VK: wrote vkdump.tga (%ux%u)\n", sceneExtent.width, sceneExtent.height );
		}
		vmaDestroyBuffer( vma, dumpBuf, dumpAlloc );
	}

	frameIndex = ( frameIndex + 1 ) % FRAMES_IN_FLIGHT;
}

/*
===============================================================================

	M2 — rings, shaders, pipelines, images (docs/vulkan-backend.md)

===============================================================================
*/

/*
====================
VulkanBackend::CreateM2Resources
====================
*/
bool VulkanBackend::CreateM2Resources() {
	uboAlign = (int)physProps.limits.minUniformBufferOffsetAlignment;
	if ( uboAlign < 4 ) {
		uboAlign = 256;
	}

	// host-visible persistently-mapped rings, one set per frame slot — the
	// slot's fence wait in BeginFrame guarantees the GPU is done with them
	// before offsets reset (the StreamGeneration contract)
	struct ringSetup_t {
		RingBuf *ring;
		int size;
		VkBufferUsageFlags usage;
	};
	for ( int slot = 0; slot < FRAMES_IN_FLIGHT; slot++ ) {
		const ringSetup_t setups[4] = {
			{ &uboRing[slot],  UBO_RING_SIZE,  VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT },
			{ &vertRing[slot], VERT_RING_SIZE, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT },
			{ &idxRing[slot],  IDX_RING_SIZE,  VK_BUFFER_USAGE_INDEX_BUFFER_BIT },
			{ &stagingRing[slot], STAGING_RING_SIZE, VK_BUFFER_USAGE_TRANSFER_SRC_BIT },
		};
		for ( int i = 0; i < 4; i++ ) {
			VkBufferCreateInfo bci = {};
			bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
			bci.size = (VkDeviceSize)setups[i].size;
			bci.usage = setups[i].usage;
			bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
			VmaAllocationCreateInfo aci = {};
			aci.usage = VMA_MEMORY_USAGE_AUTO;
			aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
			aci.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
			VmaAllocationInfo info = {};
			if ( !vkCheck( vmaCreateBuffer( vma, &bci, &aci, &setups[i].ring->buffer,
			                                &setups[i].ring->alloc, &info ), "vmaCreateBuffer(ring)" ) ) {
				return false;
			}
			setups[i].ring->mapped = (byte *)info.pMappedData;
			setups[i].ring->size = setups[i].size;
			setups[i].ring->offset = 0;
			setups[i].ring->usage = setups[i].usage;
			bufferTable.push_back( setups[i].ring->buffer );
			setups[i].ring->handle = (BufferHandle)bufferTable.size();
		}
	}

	// set 0: one dynamic-offset UBO reused for every draw
	{
		VkDescriptorSetLayoutBinding b = {};
		b.binding = 0;
		b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
		b.descriptorCount = 1;
		b.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
		VkDescriptorSetLayoutCreateInfo li = {};
		li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		li.bindingCount = 1;
		li.pBindings = &b;
		if ( !vkCheck( vkCreateDescriptorSetLayout( device, &li, NULL, &setLayoutUbo ), "vkCreateDescriptorSetLayout(ubo)" ) ) {
			return false;
		}
	}
	// set 1: combined image samplers, units 0-7, shadow cube at 8, SSAO at 9,
	// occlusion map at 10 (interaction/ambientlight declare 9/10; dummies until M7)
	{
		VkDescriptorSetLayoutBinding b[11] = {};
		for ( int i = 0; i < 11; i++ ) {
			b[i].binding = (uint32_t)i;
			b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			b[i].descriptorCount = 1;
			b[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
		}
		VkDescriptorSetLayoutCreateInfo li = {};
		li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		li.bindingCount = 11;
		li.pBindings = b;
		if ( !vkCheck( vkCreateDescriptorSetLayout( device, &li, NULL, &setLayoutTex ), "vkCreateDescriptorSetLayout(tex)" ) ) {
			return false;
		}
	}
	{
		VkDescriptorSetLayout sets[2] = { setLayoutUbo, setLayoutTex };
		VkPipelineLayoutCreateInfo pli = {};
		pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		pli.setLayoutCount = 2;
		pli.pSetLayouts = sets;
		if ( !vkCheck( vkCreatePipelineLayout( device, &pli, NULL, &pipeLayout ), "vkCreatePipelineLayout" ) ) {
			return false;
		}
	}

	// per-slot set 0, pointing at that slot's UBO ring (persistent pool)
	{
		VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, FRAMES_IN_FLIGHT };
		VkDescriptorPoolCreateInfo pci = {};
		pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		pci.maxSets = FRAMES_IN_FLIGHT;
		pci.poolSizeCount = 1;
		pci.pPoolSizes = &ps;
		if ( !vkCheck( vkCreateDescriptorPool( device, &pci, NULL, &persistentPool ), "vkCreateDescriptorPool(persistent)" ) ) {
			return false;
		}
		for ( int i = 0; i < FRAMES_IN_FLIGHT; i++ ) {
			VkDescriptorSetAllocateInfo ai = {};
			ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
			ai.descriptorPool = persistentPool;
			ai.descriptorSetCount = 1;
			ai.pSetLayouts = &setLayoutUbo;
			if ( !vkCheck( vkAllocateDescriptorSets( device, &ai, &uboSet[i] ), "vkAllocateDescriptorSets(ubo)" ) ) {
				return false;
			}
			VkDescriptorBufferInfo bi = {};
			bi.buffer = uboRing[i].buffer;
			bi.offset = 0;
			bi.range = MAX_UNIFORM_SLICE;
			VkWriteDescriptorSet w = {};
			w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			w.dstSet = uboSet[i];
			w.dstBinding = 0;
			w.descriptorCount = 1;
			w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
			w.pBufferInfo = &bi;
			vkUpdateDescriptorSets( device, 1, &w, 0, NULL );
		}
	}
	// persistent texture-set pool: reuse the same descriptor sets across frames
	// for repeated texture combinations, which is the common case and avoids
	// the per-frame allocation churn that otherwise stresses the pool.
	{
		VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_FRAME_SETS * 11 * 4 };
		VkDescriptorPoolCreateInfo pci = {};
		pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		pci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
		pci.maxSets = MAX_FRAME_SETS * 4;
		pci.poolSizeCount = 1;
		pci.pPoolSizes = &ps;
		if ( !vkCheck( vkCreateDescriptorPool( device, &pci, NULL, &texturePool ), "vkCreateDescriptorPool(texture)" ) ) {
			return false;
		}
	}
	// per-frame texture-set pools (reset wholesale each BeginFrame) for fallback
	// allocations if the persistent pool is exhausted.
	for ( int i = 0; i < FRAMES_IN_FLIGHT; i++ ) {
		VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_FRAME_SETS * 11 };
		VkDescriptorPoolCreateInfo pci = {};
		pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		pci.maxSets = MAX_FRAME_SETS;
		pci.poolSizeCount = 1;
		pci.pPoolSizes = &ps;
		if ( !vkCheck( vkCreateDescriptorPool( device, &pci, NULL, &framePool[i] ), "vkCreateDescriptorPool(frame)" ) ) {
			return false;
		}
	}

	// synchronous image-upload plumbing (level/menu load time)
	{
		VkCommandPoolCreateInfo pci = {};
		pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		pci.queueFamilyIndex = gfxFamily;
		if ( !vkCheck( vkCreateCommandPool( device, &pci, NULL, &uploadPool ), "vkCreateCommandPool(upload)" ) ) {
			return false;
		}
		VkCommandBufferAllocateInfo cai = {};
		cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		cai.commandPool = uploadPool;
		cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cai.commandBufferCount = 1;
		vkAllocateCommandBuffers( device, &cai, &uploadCb );
		VkFenceCreateInfo fci = {};
		fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		vkCreateFence( device, &fci, NULL, &uploadFence );
	}

	// 1x1 white dummy for descriptor slots no draw populates
	{
		const byte white[4] = { 255, 255, 255, 255 };
		dummyImage = CreateTexture2D( 1, 1, white, TF_NEAREST, TR_REPEAT, false );
		if ( !dummyImage ) {
			common->Warning( "VK: couldn't create the dummy texture" );
			return false;
		}
		// M4: typed dummies for slots the interaction shaders statically use —
		// a white cube (samplerCube fallback) and depth-compare dummies for the
		// sampler2DShadow / samplerCubeShadow units (7/8) until M7 shadow maps
		const void *whiteFaces[6] = { white, white, white, white, white, white };
		dummyCube = CreateTextureCube( 1, whiteFaces, TF_NEAREST, false );
		if ( !dummyCube
		     || !CreateShadowDummy( dummyShadow2D, false )
		     || !CreateShadowDummy( dummyShadowCube, true ) ) {
			common->Warning( "VK: couldn't create the M4 dummy images" );
			return false;
		}
	}

	// shader handles are per-backend: drop any Material IR built against
	// another backend's cache (rebuilt lazily)
	IR_Purge();

	common->Printf( "VK: M2 resources up - rings %d/%d/%d KB x%d slots (ubo align %d), set0 dynamic-UBO + set1 9-sampler layout\n",
	                UBO_RING_SIZE >> 10, VERT_RING_SIZE >> 10, IDX_RING_SIZE >> 10, FRAMES_IN_FLIGHT, uboAlign );
	return true;
}

/*
====================
VulkanBackend::DestroyM2Resources
====================
*/
void VulkanBackend::DestroyM2Resources() {
	// M7 render targets first: this frees the shadow-map images/views and clears
	// their borrowed imageTable slots (live=false) so the imageTable sweep below
	// doesn't double-free them. Device is idle here.
	DestroyAllTargets();

	for ( auto &kv : pipelineCache ) {
		vkDestroyPipeline( device, kv.second, NULL );
	}
	pipelineCache.clear();
	boundPipeline = VK_NULL_HANDLE;

	for ( size_t i = 0; i < shaderTable.size(); i++ ) {
		if ( shaderTable[i].vert ) { vkDestroyShaderModule( device, shaderTable[i].vert, NULL ); }
		if ( shaderTable[i].frag ) { vkDestroyShaderModule( device, shaderTable[i].frag, NULL ); }
	}
	shaderTable.clear();

	for ( size_t i = 0; i < imageTable.size(); i++ ) {
		if ( imageTable[i].live ) {
			if ( imageTable[i].view )  { vkDestroyImageView( device, imageTable[i].view, NULL ); }
			if ( imageTable[i].image ) { vmaDestroyImage( vma, imageTable[i].image, imageTable[i].alloc ); }
		}
	}
	imageTable.clear();
	dummyImage = 0;
	dummyCube = 0;
	DestroyShadowDummy( dummyShadow2D );
	DestroyShadowDummy( dummyShadowCube );

	for ( size_t i = 0; i < samplerCache.size(); i++ ) {
		vkDestroySampler( device, samplerCache[i].second, NULL );
	}
	samplerCache.clear();

	for ( int i = 0; i < FRAMES_IN_FLIGHT; i++ ) {
		if ( framePool[i] ) { vkDestroyDescriptorPool( device, framePool[i], NULL ); framePool[i] = VK_NULL_HANDLE; }
		uboSet[i] = VK_NULL_HANDLE;
	}
	textureSetCache.clear();
	// device is idle here; destroying the pool frees every set, so the pending
	// retire lists just need their now-dangling handles dropped
	for ( int i = 0; i < FRAMES_IN_FLIGHT; i++ ) { retiredTexSets[i].clear(); }
	if ( texturePool ) { vkDestroyDescriptorPool( device, texturePool, NULL ); texturePool = VK_NULL_HANDLE; }
	if ( persistentPool ) { vkDestroyDescriptorPool( device, persistentPool, NULL ); persistentPool = VK_NULL_HANDLE; }
	if ( pipeLayout )     { vkDestroyPipelineLayout( device, pipeLayout, NULL ); pipeLayout = VK_NULL_HANDLE; }
	if ( setLayoutUbo )   { vkDestroyDescriptorSetLayout( device, setLayoutUbo, NULL ); setLayoutUbo = VK_NULL_HANDLE; }
	if ( setLayoutTex )   { vkDestroyDescriptorSetLayout( device, setLayoutTex, NULL ); setLayoutTex = VK_NULL_HANDLE; }

	if ( uploadFence ) { vkDestroyFence( device, uploadFence, NULL ); uploadFence = VK_NULL_HANDLE; }
	if ( uploadPool )  { vkDestroyCommandPool( device, uploadPool, NULL ); uploadPool = VK_NULL_HANDLE; uploadCb = VK_NULL_HANDLE; }

	for ( int slot = 0; slot < FRAMES_IN_FLIGHT; slot++ ) {
		DrainRetiredRings( slot );		// device is idle here
		DrainRetiredImages( slot );
		RingBuf *rings[4] = { &uboRing[slot], &vertRing[slot], &idxRing[slot], &stagingRing[slot] };
		for ( int i = 0; i < 4; i++ ) {
			if ( rings[i]->buffer ) {
				vmaDestroyBuffer( vma, rings[i]->buffer, rings[i]->alloc );
			}
			*rings[i] = RingBuf();
		}
	}
	bufferTable.clear();
	ringOverflowWarned = false;
	framePoolWarned = false;

	IR_Purge();
}

/*
====================
VulkanBackend::AllocFromRing
====================
*/
/*
====================
VulkanBackend::GrowRing / DrainRetiredRings

A mid-frame wrap would overwrite data draws recorded earlier this frame still
read on the GPU (observed as scattered "flying triangle" geometry once M4's
shadow volumes pushed a busy view past the vertex ring). Instead the ring
doubles: the live buffer is retired — in-flight draws keep their handle to it —
and destroyed only after this slot's next fence wait. StreamGeneration bumps so
cached stream offsets into the retired buffer aren't reused next frame.
====================
*/
bool VulkanBackend::GrowRing( RingBuf &ring, int minSize ) {
	int newSize = ring.size * 2;
	while ( newSize < minSize ) {
		newSize *= 2;
	}

	VkBufferCreateInfo bci = {};
	bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bci.size = (VkDeviceSize)newSize;
	bci.usage = ring.usage;
	bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VmaAllocationCreateInfo aci = {};
	aci.usage = VMA_MEMORY_USAGE_AUTO;
	aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
	aci.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	VkBuffer newBuf = VK_NULL_HANDLE;
	VmaAllocation newAlloc = NULL;
	VmaAllocationInfo info = {};
	if ( !vkCheck( vmaCreateBuffer( vma, &bci, &aci, &newBuf, &newAlloc, &info ), "vmaCreateBuffer(ring grow)" ) ) {
		return false;
	}

	retiredRings[frameIndex].push_back( { ring.handle, ring.buffer, ring.alloc } );

	ring.buffer = newBuf;
	ring.alloc = newAlloc;
	ring.mapped = (byte *)info.pMappedData;
	ring.size = newSize;
	ring.offset = 0;
	bufferTable.push_back( newBuf );
	ring.handle = (BufferHandle)bufferTable.size();
	streamGen++;
	common->Printf( "VK: geometry ring grew to %d KB (mid-frame overflow)\n", newSize >> 10 );
	return true;
}

void VulkanBackend::DrainRetiredRings( int slot ) {
	for ( size_t i = 0; i < retiredRings[slot].size(); i++ ) {
		const RetiredRing &r = retiredRings[slot][i];
		if ( r.handle >= 1 && r.handle <= (BufferHandle)bufferTable.size() ) {
			bufferTable[r.handle - 1] = VK_NULL_HANDLE;
		}
		vmaDestroyBuffer( vma, r.buffer, r.alloc );
	}
	retiredRings[slot].clear();
}

int VulkanBackend::AllocFromRing( RingBuf &ring, const void *data, int size, int align,
                                  int wrapReserve, BufferHandle *buffer ) {
	if ( ring.mapped == NULL || size <= 0 ) {
		if ( buffer ) { *buffer = 0; }
		return 0;
	}
	int offset = ( ring.offset + align - 1 ) & ~( align - 1 );
	if ( offset + size > ring.size - wrapReserve ) {
		// geometry rings grow in place (see GrowRing). The UBO ring can't — its
		// dynamic-offset descriptor set is already bound in the recording
		// command buffer — but at 16MB (~16k draws) it has never overflowed;
		// wrap + warn remains the fallback for it and for a failed grow.
		if ( ring.usage != VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT && GrowRing( ring, size ) ) {
			offset = 0;
		} else {
			if ( !ringOverflowWarned ) {
				ringOverflowWarned = true;
				common->Warning( "VK: ring overflow (%d KB frame) - draws may corrupt until the ring grows", ring.size >> 10 );
			}
			offset = 0;
		}
	}
	memcpy( ring.mapped + offset, data, size );
	ring.offset = offset + size;
	*buffer = ring.handle;
	return offset;
}

int VulkanBackend::AllocUniforms( const void *data, int size, BufferHandle *buffer ) {
	return AllocFromRing( uboRing[frameIndex], data, size, uboAlign, MAX_UNIFORM_SLICE, buffer );
}
int VulkanBackend::AllocVertices( const void *data, int size, BufferHandle *buffer ) {
	return AllocFromRing( vertRing[frameIndex], data, size, 4, 0, buffer );
}
int VulkanBackend::AllocIndices( const void *data, int size, BufferHandle *buffer ) {
	return AllocFromRing( idxRing[frameIndex], data, size, 4, 0, buffer );
}

/*
====================
VulkanBackend::LoadShader

<name> -> shaders/spv/<name>.vert.spv + .frag.spv, via the VFS with the same
build-tree dev fallback the GLSL loader uses (DUDE_SHADER_SPV_DIR).
====================
*/
static bool VK_ReadSpv( const char *fileName, std::vector<byte> &out ) {
	void *buf = NULL;
	int len = fileSystem->ReadFile( va( "shaders/spv/%s", fileName ), &buf, NULL );
	if ( len > 0 && buf != NULL ) {
		out.assign( (byte *)buf, (byte *)buf + len );
		fileSystem->FreeFile( buf );
		return true;
	}
#ifdef DUDE_SHADER_SPV_DIR
	FILE *f = fopen( va( "%s/%s", DUDE_SHADER_SPV_DIR, fileName ), "rb" );
	if ( f != NULL ) {
		fseek( f, 0, SEEK_END );
		long sz = ftell( f );
		fseek( f, 0, SEEK_SET );
		if ( sz > 0 ) {
			out.resize( (size_t)sz );
			size_t rd = fread( out.data(), 1, (size_t)sz, f );
			fclose( f );
			return rd == (size_t)sz;
		}
		fclose( f );
	}
#endif
	return false;
}

ShaderHandle VulkanBackend::LoadShader( const char *name ) {
	if ( device == VK_NULL_HANDLE || name == NULL || name[0] == '\0' ) {
		return 0;
	}
	for ( size_t i = 0; i < shaderTable.size(); i++ ) {
		if ( shaderTable[i].name.Icmp( name ) == 0 ) {
			return shaderTable[i].failed ? 0 : (ShaderHandle)( i + 1 );
		}
	}

	ShaderRec rec;
	rec.name = name;

	std::vector<byte> vertSpv, fragSpv;
	if ( !VK_ReadSpv( va( "%s.vert.spv", name ), vertSpv )
	  || !VK_ReadSpv( va( "%s.frag.spv", name ), fragSpv ) ) {
		common->Warning( "VK shaders: missing SPIR-V pair for '%s' (shaders/spv)", name );
		rec.failed = true;
		shaderTable.push_back( rec );
		return 0;
	}

	VkShaderModuleCreateInfo mi = {};
	mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	mi.codeSize = vertSpv.size();
	mi.pCode = (const uint32_t *)vertSpv.data();
	bool ok = vkCheck( vkCreateShaderModule( device, &mi, NULL, &rec.vert ), va( "vkCreateShaderModule(%s.vert)", name ) );
	mi.codeSize = fragSpv.size();
	mi.pCode = (const uint32_t *)fragSpv.data();
	ok = ok && vkCheck( vkCreateShaderModule( device, &mi, NULL, &rec.frag ), va( "vkCreateShaderModule(%s.frag)", name ) );
	if ( !ok ) {
		if ( rec.vert ) { vkDestroyShaderModule( device, rec.vert, NULL ); rec.vert = VK_NULL_HANDLE; }
		if ( rec.frag ) { vkDestroyShaderModule( device, rec.frag, NULL ); rec.frag = VK_NULL_HANDLE; }
		rec.failed = true;
	}
	shaderTable.push_back( rec );
	return rec.failed ? 0 : (ShaderHandle)shaderTable.size();
}

/*
====================
VulkanBackend::GetSampler

Sampler from the engine's textureFilter_t/textureRepeat_t. The GL path's
per-texture filter knobs (image_filter etc.) collapse to their defaults here.
====================
*/
VkSampler VulkanBackend::GetSampler( int textureFilter, int textureRepeat, bool hasMips ) {
	const unsigned int key = ( (unsigned int)textureFilter << 8 )
	                       | ( (unsigned int)textureRepeat << 1 )
	                       | ( hasMips ? 1u : 0u );
	for ( size_t i = 0; i < samplerCache.size(); i++ ) {
		if ( samplerCache[i].first == key ) {
			return samplerCache[i].second;
		}
	}

	VkSamplerCreateInfo si = {};
	si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	switch ( textureFilter ) {
	case TF_NEAREST:
		si.magFilter = si.minFilter = VK_FILTER_NEAREST;
		si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
		si.maxLod = 0.0f;
		break;
	case TF_LINEAR:
		si.magFilter = si.minFilter = VK_FILTER_LINEAR;
		si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
		si.maxLod = 0.0f;
		break;
	default:	// TF_DEFAULT: trilinear + anisotropy when the device has it
		si.magFilter = si.minFilter = VK_FILTER_LINEAR;
		si.mipmapMode = hasMips ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
		si.maxLod = hasMips ? VK_LOD_CLAMP_NONE : 0.0f;
		if ( haveAnisotropy && hasMips ) {
			si.anisotropyEnable = VK_TRUE;
			float aniso = (float)globalImages->textureAnisotropy;
			if ( aniso < 1.0f ) { aniso = 1.0f; }
			if ( aniso > physProps.limits.maxSamplerAnisotropy ) { aniso = physProps.limits.maxSamplerAnisotropy; }
			si.maxAnisotropy = aniso;
		}
		break;
	}
	switch ( textureRepeat ) {
	case TR_CLAMP:
		si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		break;
	case TR_CLAMP_TO_BORDER:
	case TR_CLAMP_TO_ZERO:
	case TR_CLAMP_TO_ZERO_ALPHA:
		si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
		si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
		break;
	default:	// TR_REPEAT
		si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		break;
	}

	VkSampler sampler = VK_NULL_HANDLE;
	if ( !vkCheck( vkCreateSampler( device, &si, NULL, &sampler ), "vkCreateSampler" ) ) {
		return VK_NULL_HANDLE;
	}
	samplerCache.push_back( std::make_pair( key, sampler ) );
	return sampler;
}

/*
====================
VulkanBackend::CreateTexture2D

RGBA8 upload with a CPU-built mip chain (R_MipMap, the same filter the GL
path uses), through a staging buffer and a blocking submit — image loads
happen at init/level load, not mid-frame.
====================
*/
ImageHandle VulkanBackend::CreateTexture2D( int w, int h, const void *pixels,
                                            int textureFilter, int textureRepeat, bool allowMips ) {
	if ( device == VK_NULL_HANDLE || w <= 0 || h <= 0 || pixels == NULL || uploadCb == VK_NULL_HANDLE ) {
		return 0;
	}

	// build the level list (level 0 borrows the caller's pixels)
	struct level_t { const byte *data; int w, h; };
	std::vector<level_t> levels;
	std::vector<byte *> owned;		// R_MipMap results to free
	levels.push_back( { (const byte *)pixels, w, h } );
	if ( allowMips ) {
		const bool preserveBorder = ( textureRepeat == TR_CLAMP_TO_ZERO );
		const byte *src = (const byte *)pixels;
		int lw = w, lh = h;
		while ( lw > 1 || lh > 1 ) {
			byte *shrunk = R_MipMap( src, lw, lh, preserveBorder );
			lw = lw > 1 ? lw >> 1 : 1;
			lh = lh > 1 ? lh >> 1 : 1;
			owned.push_back( shrunk );
			levels.push_back( { shrunk, lw, lh } );
			src = shrunk;
		}
	}

	VkDeviceSize total = 0;
	for ( size_t i = 0; i < levels.size(); i++ ) {
		total += (VkDeviceSize)levels[i].w * levels[i].h * 4;
	}

	// staging buffer
	VkBuffer staging = VK_NULL_HANDLE;
	VmaAllocation stagingAlloc = NULL;
	{
		VkBufferCreateInfo bci = {};
		bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bci.size = total;
		bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		VmaAllocationCreateInfo aci = {};
		aci.usage = VMA_MEMORY_USAGE_AUTO;
		aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
		aci.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
		VmaAllocationInfo info = {};
		if ( !vkCheck( vmaCreateBuffer( vma, &bci, &aci, &staging, &stagingAlloc, &info ), "vmaCreateBuffer(staging)" ) ) {
			for ( size_t i = 0; i < owned.size(); i++ ) { R_StaticFree( owned[i] ); }
			return 0;
		}
		byte *dst = (byte *)info.pMappedData;
		for ( size_t i = 0; i < levels.size(); i++ ) {
			const size_t bytes = (size_t)levels[i].w * levels[i].h * 4;
			memcpy( dst, levels[i].data, bytes );
			dst += bytes;
		}
	}
	for ( size_t i = 0; i < owned.size(); i++ ) { R_StaticFree( owned[i] ); }

	// the image
	VkImage image = VK_NULL_HANDLE;
	VmaAllocation alloc = NULL;
	{
		VkImageCreateInfo ici = {};
		ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		ici.imageType = VK_IMAGE_TYPE_2D;
		ici.format = VK_FORMAT_R8G8B8A8_UNORM;
		ici.extent = { (uint32_t)w, (uint32_t)h, 1 };
		ici.mipLevels = (uint32_t)levels.size();
		ici.arrayLayers = 1;
		ici.samples = VK_SAMPLE_COUNT_1_BIT;
		ici.tiling = VK_IMAGE_TILING_OPTIMAL;
		ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		VmaAllocationCreateInfo aci = {};
		aci.usage = VMA_MEMORY_USAGE_AUTO;
		if ( !vkCheck( vmaCreateImage( vma, &ici, &aci, &image, &alloc, NULL ), "vmaCreateImage(texture)" ) ) {
			vmaDestroyBuffer( vma, staging, stagingAlloc );
			return 0;
		}
	}

	// record + submit the upload, wait for it (load-time only)
	{
		vkResetCommandBuffer( uploadCb, 0 );
		VkCommandBufferBeginInfo bi = {};
		bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer( uploadCb, &bi );

		VkImageMemoryBarrier toDst = {};
		toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		toDst.srcAccessMask = 0;
		toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toDst.image = image;
		toDst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		toDst.subresourceRange.levelCount = (uint32_t)levels.size();
		toDst.subresourceRange.layerCount = 1;
		vkCmdPipelineBarrier( uploadCb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			0, 0, NULL, 0, NULL, 1, &toDst );

		VkDeviceSize bufOfs = 0;
		for ( size_t i = 0; i < levels.size(); i++ ) {
			VkBufferImageCopy c = {};
			c.bufferOffset = bufOfs;
			c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			c.imageSubresource.mipLevel = (uint32_t)i;
			c.imageSubresource.layerCount = 1;
			c.imageExtent = { (uint32_t)levels[i].w, (uint32_t)levels[i].h, 1 };
			vkCmdCopyBufferToImage( uploadCb, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c );
			bufOfs += (VkDeviceSize)levels[i].w * levels[i].h * 4;
		}

		VkImageMemoryBarrier toRead = toDst;
		toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		vkCmdPipelineBarrier( uploadCb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			0, 0, NULL, 0, NULL, 1, &toRead );

		vkEndCommandBuffer( uploadCb );
		VkSubmitInfo si = {};
		si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		si.commandBufferCount = 1;
		si.pCommandBuffers = &uploadCb;
		vkResetFences( device, 1, &uploadFence );
		vkQueueSubmit( gfxQueue, 1, &si, uploadFence );
		vkWaitForFences( device, 1, &uploadFence, VK_TRUE, UINT64_MAX );
	}
	vmaDestroyBuffer( vma, staging, stagingAlloc );

	ImageRec rec;
	rec.image = image;
	rec.alloc = alloc;
	{
		VkImageViewCreateInfo vi = {};
		vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		vi.image = image;
		vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vi.format = VK_FORMAT_R8G8B8A8_UNORM;
		vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		vi.subresourceRange.levelCount = (uint32_t)levels.size();
		vi.subresourceRange.layerCount = 1;
		if ( !vkCheck( vkCreateImageView( device, &vi, NULL, &rec.view ), "vkCreateImageView(texture)" ) ) {
			vmaDestroyImage( vma, image, alloc );
			return 0;
		}
	}
	rec.sampler = GetSampler( textureFilter, textureRepeat, levels.size() > 1 );
	rec.live = true;
	rec.width = w;
	rec.height = h;

	// reuse a freed slot when one exists
	for ( size_t i = 0; i < imageTable.size(); i++ ) {
		if ( !imageTable[i].live ) {
			imageTable[i] = rec;
			return (ImageHandle)( i + 1 );
		}
	}
	imageTable.push_back( rec );
	return (ImageHandle)imageTable.size();
}

/*
====================
VulkanBackend::DestroyImage
====================
*/
void VulkanBackend::DestroyImage( ImageHandle h ) {
	if ( device == VK_NULL_HANDLE || h < 1 || h > (ImageHandle)imageTable.size() || !imageTable[h - 1].live ) {
		return;
	}
	// frames may still reference the view via this frame's descriptor sets;
	// image churn happens at load boundaries, where a full stop is acceptable
	vkDeviceWaitIdle( device );
	ImageRec &rec = imageTable[h - 1];
	if ( rec.view )  { vkDestroyImageView( device, rec.view, NULL ); }
	if ( rec.image ) { vmaDestroyImage( vma, rec.image, rec.alloc ); }
	rec = ImageRec();
	// handle h is now free for reuse (CreateTexture2D reuses freed slots). The
	// cross-frame textureSetCache keys on ImageHandles, so any cached set that
	// referenced h now points at the destroyed view — and once a new image
	// takes the slot, that stale set would sample the wrong texture. Drop the
	// cache, same as RetireImage. (This path is common: GenerateImage destroys
	// then recreates dynamically-updated 2D images — loading bars, scratch GUIs.)
	InvalidateTextureSets();
}

/*
====================
VulkanBackend::RetireImage / DrainRetiredImages

Mid-frame image destruction (capture/cinematic size changes): the slot is
freed immediately, the Vulkan objects wait for this frame slot's fence —
descriptor sets recorded earlier this frame still reference the old view.
====================
*/
void VulkanBackend::RetireImage( ImageHandle h ) {
	if ( device == VK_NULL_HANDLE || h < 1 || h > (ImageHandle)imageTable.size() || !imageTable[h - 1].live ) {
		return;
	}
	ImageRec &rec = imageTable[h - 1];
	retiredImages[frameIndex].push_back( { rec.image, rec.alloc, rec.view } );
	rec = ImageRec();
	InvalidateTextureSets();	// handle h is now free for reuse; cached sets referencing it are stale
}

void VulkanBackend::DrainRetiredImages( int slot ) {
	for ( size_t i = 0; i < retiredImages[slot].size(); i++ ) {
		const RetiredImage &r = retiredImages[slot][i];
		if ( r.view )  { vkDestroyImageView( device, r.view, NULL ); }
		if ( r.image ) { vmaDestroyImage( vma, r.image, r.alloc ); }
	}
	retiredImages[slot].clear();
}

/*
====================
VulkanBackend::InvalidateTextureSets

Drop the whole cross-frame texture-set cache. Called whenever an ImageHandle is
retired (RetireImage) or a render target is destroyed (DestroyRenderTarget) —
either recycles an imageTable slot, so any cached set that references it is now
stale. The sets can't be freed here (this runs mid-frame; they may still be
bound in this and the other in-flight command buffers), so they are queued on
this slot's retire list and freed once its fence has passed, in BeginFrame.

Coarse but correct, and never worse than the shipped baseline (which rebuilt
every set every frame): on frames with no retirement the cache survives intact.
====================
*/
void VulkanBackend::InvalidateTextureSets() {
	if ( textureSetCache.empty() ) {
		return;
	}
	for ( auto &kv : textureSetCache ) {
		retiredTexSets[frameIndex].push_back( kv.second );
	}
	textureSetCache.clear();
	// a retired set may be the one the last-bound fast path is holding; drop it
	boundTexKey = 0;
	boundTexSet = VK_NULL_HANDLE;
}

/*
====================
VulkanBackend::CreateCaptureImage

Sampleable copy target for the M5 screen captures: RGBA8 (linear/clamp — the
filtering GL sets after every capture) or the scene's depth-stencil format
(nearest, sampled through a depth-aspect view). Contents are undefined until
the first CopyFramebufferToImage.
====================
*/
ImageHandle VulkanBackend::CreateCaptureImage( int w, int h, bool depth ) {
	if ( device == VK_NULL_HANDLE || w <= 0 || h <= 0 ) {
		return 0;
	}
	if ( depth && sceneDepthFormat == VK_FORMAT_UNDEFINED ) {
		return 0;
	}

	VkImageCreateInfo ici = {};
	ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = depth ? sceneDepthFormat : VK_FORMAT_R8G8B8A8_UNORM;
	ici.extent = { (uint32_t)w, (uint32_t)h, 1 };
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VmaAllocationCreateInfo aci = {};
	aci.usage = VMA_MEMORY_USAGE_AUTO;
	VkImage image = VK_NULL_HANDLE;
	VmaAllocation alloc = NULL;
	if ( !vkCheck( vmaCreateImage( vma, &ici, &aci, &image, &alloc, NULL ), "vmaCreateImage(capture)" ) ) {
		return 0;
	}

	ImageRec rec;
	rec.image = image;
	rec.alloc = alloc;
	{
		VkImageViewCreateInfo vi = {};
		vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		vi.image = image;
		vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vi.format = ici.format;
		vi.subresourceRange.aspectMask = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
		vi.subresourceRange.levelCount = 1;
		vi.subresourceRange.layerCount = 1;
		if ( !vkCheck( vkCreateImageView( device, &vi, NULL, &rec.view ), "vkCreateImageView(capture)" ) ) {
			vmaDestroyImage( vma, image, alloc );
			return 0;
		}
	}
	rec.sampler = GetSampler( depth ? TF_NEAREST : TF_LINEAR, TR_CLAMP, false );
	rec.live = true;
	rec.layout = VK_IMAGE_LAYOUT_UNDEFINED;
	rec.isDepth = depth;
	rec.width = w;
	rec.height = h;

	for ( size_t i = 0; i < imageTable.size(); i++ ) {
		if ( !imageTable[i].live ) {
			imageTable[i] = rec;
			return (ImageHandle)( i + 1 );
		}
	}
	imageTable.push_back( rec );
	return (ImageHandle)imageTable.size();
}

/*
====================
VulkanBackend::CopyFramebufferToImage

Copy a scene-image rect into a capture image, mid-pass: the render pass is
suspended (every pass variant leaves the color image TRANSFER_SRC_OPTIMAL)
and the next draw's EnsureScenePass resumes with the load variant.

src rect arrives in GL window coordinates (origin bottom-left). Color copies
blit with a vertical flip so the capture matches GL's bottom-up texture
memory — explicit-texcoord consumers (the player-view _scratch warps) then
sample identically to GL. Depth copies stay native top-down: their consumers
address by gl_FragCoord, which is top-down here, so native orientation is the
self-consistent one (and depth blits can't flip anyway).
====================
*/
void VulkanBackend::CopyFramebufferToImage( ImageHandle dst, int dstX, int dstY,
                                            int srcX, int srcY, int w, int h, bool depth ) {
	if ( device == VK_NULL_HANDLE || !frameOpen || skipFrame || !sceneEverWritten ) {
		return;
	}
	if ( dst < 1 || dst > (ImageHandle)imageTable.size() || !imageTable[dst - 1].live ) {
		return;
	}
	ImageRec &rec = imageTable[dst - 1];
	if ( rec.isDepth != depth ) {
		return;
	}

	const int sceneW = (int)sceneExtent.width;
	const int sceneH = (int)sceneExtent.height;
	// clip against the scene image (GL coords) and the destination
	if ( srcX < 0 ) { w += srcX; dstX -= srcX; srcX = 0; }
	if ( srcY < 0 ) { h += srcY; dstY -= srcY; srcY = 0; }
	if ( srcX + w > sceneW ) { w = sceneW - srcX; }
	if ( srcY + h > sceneH ) { h = sceneH - srcY; }
	if ( dstX + w > rec.width )  { w = rec.width - dstX; }
	if ( dstY + h > rec.height ) { h = rec.height - dstY; }
	if ( w <= 0 || h <= 0 || dstX < 0 || dstY < 0 ) {
		return;
	}
	const int vkTop = sceneH - srcY - h;	// GL bottom-left rect -> VK top-left row

	VkCommandBuffer cb = frames[frameIndex].cb;
	if ( insideScenePass ) {
		vkCmdEndRenderPass( cb );
		insideScenePass = false;
	}

	// destination -> TRANSFER_DST (orders after every prior read, this frame's
	// recorded draws and earlier submitted frames alike)
	VkImageMemoryBarrier toDst = {};
	toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	toDst.srcAccessMask = rec.layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_SHADER_READ_BIT;
	toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	toDst.oldLayout = rec.layout;
	toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toDst.image = rec.image;
	// layout-transition barriers must cover every aspect of a combined
	// depth/stencil format, even when the copy itself only touches depth
	toDst.subresourceRange.aspectMask = depth
		? ( VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT ) : VK_IMAGE_ASPECT_COLOR_BIT;
	toDst.subresourceRange.levelCount = 1;
	toDst.subresourceRange.layerCount = 1;
	vkCmdPipelineBarrier( cb,
		rec.layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &toDst );

	if ( !depth ) {
		// source = the active frame color (sceneColor, or the HDR buffer when a
		// frame target is set). sceneColor rests in TRANSFER_SRC between passes; a
		// sampled color target rests in SHADER_READ_ONLY, so round-trip it here.
		VkImage       srcImg = FrameColorImage();
		VkImageLayout between = FrameColorBetweenLayout();
		const bool    needRT = ( between != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL );
		if ( needRT ) {
			VkImageMemoryBarrier b = {};
			b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
			b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			b.oldLayout = between;
			b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			b.image = srcImg;
			b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			b.subresourceRange.levelCount = 1;
			b.subresourceRange.layerCount = 1;
			vkCmdPipelineBarrier( cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
				0, 0, NULL, 0, NULL, 1, &b );
		}
		// flip while blitting (reversed src y corners) to reach GL's bottom-up layout
		VkImageBlit blit = {};
		blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blit.srcSubresource.layerCount = 1;
		blit.srcOffsets[0] = { srcX, vkTop + h, 0 };
		blit.srcOffsets[1] = { srcX + w, vkTop, 1 };
		blit.dstSubresource = blit.srcSubresource;
		blit.dstOffsets[0] = { dstX, dstY, 0 };
		blit.dstOffsets[1] = { dstX + w, dstY + h, 1 };
		vkCmdBlitImage( cb, srcImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			rec.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST );
		if ( needRT ) {
			VkImageMemoryBarrier b = {};
			b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			b.newLayout = between;
			b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			b.image = srcImg;
			b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			b.subresourceRange.levelCount = 1;
			b.subresourceRange.layerCount = 1;
			vkCmdPipelineBarrier( cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
				0, 0, NULL, 0, NULL, 1, &b );
		}
	} else {
		// depth (sceneDepth, or the HDR buffer's depth-stencil when a frame target
		// is set) stays DEPTH_STENCIL_ATTACHMENT_OPTIMAL between passes; round-trip
		// it through TRANSFER_SRC for the copy
		VkImage srcDepth = FrameDepthImage();
		VkImageMemoryBarrier srcBar = {};
		srcBar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		srcBar.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		srcBar.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		srcBar.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		srcBar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		srcBar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		srcBar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		srcBar.image = srcDepth;
		srcBar.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
		srcBar.subresourceRange.levelCount = 1;
		srcBar.subresourceRange.layerCount = 1;
		vkCmdPipelineBarrier( cb, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			0, 0, NULL, 0, NULL, 1, &srcBar );

		VkImageCopy c = {};
		c.srcSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
		c.srcSubresource.layerCount = 1;
		c.srcOffset = { srcX, vkTop, 0 };
		c.dstSubresource = c.srcSubresource;
		c.dstOffset = { dstX, dstY, 0 };
		c.extent = { (uint32_t)w, (uint32_t)h, 1 };
		vkCmdCopyImage( cb, srcDepth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			rec.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c );

		VkImageMemoryBarrier srcBack = srcBar;
		srcBack.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		srcBack.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		srcBack.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		srcBack.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		vkCmdPipelineBarrier( cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
			0, 0, NULL, 0, NULL, 1, &srcBack );
	}

	// destination -> SHADER_READ for the stages that sample it
	VkImageMemoryBarrier toRead = toDst;
	toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	vkCmdPipelineBarrier( cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		0, 0, NULL, 0, NULL, 1, &toRead );
	rec.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

/*
====================
VulkanBackend::UpdateTexture2D

Full same-size re-upload of an RGBA8 texture inside the frame's command
stream (cinematic frames). Staged through the per-slot transfer ring; the
in-cb barriers order the write against this frame's earlier samples and any
still-executing prior frame.
====================
*/
void VulkanBackend::UpdateTexture2D( ImageHandle dst, int w, int h, const void *pixels ) {
	if ( device == VK_NULL_HANDLE || !frameOpen || skipFrame || pixels == NULL ) {
		return;
	}
	if ( dst < 1 || dst > (ImageHandle)imageTable.size() || !imageTable[dst - 1].live ) {
		return;
	}
	ImageRec &rec = imageTable[dst - 1];
	if ( rec.width != w || rec.height != h ) {
		return;		// size changes recreate the image (caller's contract)
	}

	const int bytes = w * h * 4;
	BufferHandle stagingHandle = 0;
	int stagingOfs = AllocFromRing( stagingRing[frameIndex], pixels, bytes, 16, 0, &stagingHandle );
	VkBuffer staging = LookupBuffer( stagingHandle );
	if ( staging == VK_NULL_HANDLE ) {
		return;
	}

	VkCommandBuffer cb = frames[frameIndex].cb;
	if ( insideScenePass ) {
		vkCmdEndRenderPass( cb );
		insideScenePass = false;
	}

	VkImageMemoryBarrier toDst = {};
	toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	toDst.srcAccessMask = rec.layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_SHADER_READ_BIT;
	toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	toDst.oldLayout = rec.layout;
	toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toDst.image = rec.image;
	toDst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	toDst.subresourceRange.levelCount = 1;
	toDst.subresourceRange.layerCount = 1;
	vkCmdPipelineBarrier( cb,
		rec.layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &toDst );

	VkBufferImageCopy c = {};
	c.bufferOffset = (VkDeviceSize)stagingOfs;
	c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	c.imageSubresource.layerCount = 1;
	c.imageExtent = { (uint32_t)w, (uint32_t)h, 1 };
	vkCmdCopyBufferToImage( cb, staging, rec.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c );

	VkImageMemoryBarrier toRead = toDst;
	toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	vkCmdPipelineBarrier( cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		0, 0, NULL, 0, NULL, 1, &toRead );
	rec.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

/*
====================
VulkanBackend::ReadPixelsRGB

M6 screenshots: read a rect of the last completed frame out of the scene
image (it keeps the frame between passes/frames, layout TRANSFER_SRC) into
the glReadPixels(GL_RGB) layout the callers expect — GL window coords, rows
bottom-up, padded to 4-byte boundaries. Synchronous by design: waits the
queue idle, one-off copy through the upload command buffer, fence wait.
====================
*/
bool VulkanBackend::ReadPixelsRGB( unsigned char *dest, int x, int y, int w, int h ) {
	if ( device == VK_NULL_HANDLE || sceneColor == VK_NULL_HANDLE || !sceneEverWritten
	     || dest == NULL || uploadCb == VK_NULL_HANDLE ) {
		return false;
	}
	const int sceneW = (int)sceneExtent.width;
	const int sceneH = (int)sceneExtent.height;
	if ( x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > sceneW || y + h > sceneH ) {
		// clamp like glReadPixels would; out-of-range rows stay untouched
		if ( x < 0 ) { w += x; x = 0; }
		if ( y < 0 ) { h += y; y = 0; }
		if ( x + w > sceneW ) { w = sceneW - x; }
		if ( y + h > sceneH ) { h = sceneH - y; }
		if ( w <= 0 || h <= 0 ) {
			return false;
		}
	}

	// all rendering that wrote the image must be complete before we copy
	vkQueueWaitIdle( gfxQueue );

	VkBuffer buf = VK_NULL_HANDLE;
	VmaAllocation alloc = NULL;
	byte *mapped = NULL;
	{
		VkBufferCreateInfo bci = {};
		bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bci.size = (VkDeviceSize)w * h * 4;
		bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		VmaAllocationCreateInfo aci = {};
		aci.usage = VMA_MEMORY_USAGE_AUTO;
		aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
		aci.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
		VmaAllocationInfo info = {};
		if ( !vkCheck( vmaCreateBuffer( vma, &bci, &aci, &buf, &alloc, &info ), "vmaCreateBuffer(readback)" ) ) {
			return false;
		}
		mapped = (byte *)info.pMappedData;
	}

	vkResetCommandBuffer( uploadCb, 0 );
	VkCommandBufferBeginInfo bi = {};
	bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer( uploadCb, &bi );

	// make the color writes available to transfer reads (queue is idle, so
	// this is a memory barrier, not an execution race)
	VkMemoryBarrier mb = {};
	mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
	mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	vkCmdPipelineBarrier( uploadCb,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL );

	// GL bottom-left rect -> VK top-left row
	VkBufferImageCopy c = {};
	c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	c.imageSubresource.layerCount = 1;
	c.imageOffset = { x, sceneH - y - h, 0 };
	c.imageExtent = { (uint32_t)w, (uint32_t)h, 1 };
	vkCmdCopyImageToBuffer( uploadCb, sceneColor, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &c );

	vkEndCommandBuffer( uploadCb );
	VkSubmitInfo si = {};
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.commandBufferCount = 1;
	si.pCommandBuffers = &uploadCb;
	vkResetFences( device, 1, &uploadFence );
	vkQueueSubmit( gfxQueue, 1, &si, uploadFence );
	vkWaitForFences( device, 1, &uploadFence, VK_TRUE, UINT64_MAX );

	// RGBA top-down -> RGB bottom-up with 4-byte row padding (the GL contract)
	const int dstRow = ( w * 3 + 3 ) & ~3;
	for ( int r = 0; r < h; r++ ) {
		const byte *src = mapped + (size_t)( h - 1 - r ) * w * 4;
		byte *dst = dest + (size_t)r * dstRow;
		for ( int i = 0; i < w; i++ ) {
			dst[i * 3 + 0] = src[i * 4 + 0];
			dst[i * 3 + 1] = src[i * 4 + 1];
			dst[i * 3 + 2] = src[i * 4 + 2];
		}
	}

	vmaDestroyBuffer( vma, buf, alloc );
	return true;
}

/*
====================
VulkanBackend::CreateTextureCube

Six RGBA8 faces in GL_TEXTURE_CUBE_MAP_POSITIVE_X.. order, with a CPU mip
chain per face (the same R_MipMap the GL path uses in GenerateCubeImage).
Sampler is clamp-to-edge — the only mode that makes sense on a cube.
====================
*/
ImageHandle VulkanBackend::CreateTextureCube( int size, const void * const pics[6],
                                              int textureFilter, bool allowMips ) {
	if ( device == VK_NULL_HANDLE || size <= 0 || pics == NULL || uploadCb == VK_NULL_HANDLE ) {
		return 0;
	}
	for ( int f = 0; f < 6; f++ ) {
		if ( pics[f] == NULL ) {
			return 0;
		}
	}

	// per-face level lists (level 0 borrows the caller's pixels)
	struct level_t { const byte *data; int size; };
	std::vector<level_t> levels[6];
	std::vector<byte *> owned;
	uint32_t numLevels = 1;
	for ( int f = 0; f < 6; f++ ) {
		levels[f].push_back( { (const byte *)pics[f], size } );
		if ( allowMips ) {
			const byte *src = (const byte *)pics[f];
			int ls = size;
			while ( ls > 1 ) {
				byte *shrunk = R_MipMap( src, ls, ls, false );
				ls >>= 1;
				owned.push_back( shrunk );
				levels[f].push_back( { shrunk, ls } );
				src = shrunk;
			}
		}
		numLevels = (uint32_t)levels[f].size();
	}

	VkDeviceSize faceBytes = 0;
	for ( uint32_t l = 0; l < numLevels; l++ ) {
		faceBytes += (VkDeviceSize)levels[0][l].size * levels[0][l].size * 4;
	}
	const VkDeviceSize total = faceBytes * 6;

	// staging buffer: all levels of face 0, then face 1, ...
	VkBuffer staging = VK_NULL_HANDLE;
	VmaAllocation stagingAlloc = NULL;
	{
		VkBufferCreateInfo bci = {};
		bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bci.size = total;
		bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		VmaAllocationCreateInfo aci = {};
		aci.usage = VMA_MEMORY_USAGE_AUTO;
		aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
		aci.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
		VmaAllocationInfo info = {};
		if ( !vkCheck( vmaCreateBuffer( vma, &bci, &aci, &staging, &stagingAlloc, &info ), "vmaCreateBuffer(cube staging)" ) ) {
			for ( size_t i = 0; i < owned.size(); i++ ) { R_StaticFree( owned[i] ); }
			return 0;
		}
		byte *dst = (byte *)info.pMappedData;
		for ( int f = 0; f < 6; f++ ) {
			for ( uint32_t l = 0; l < numLevels; l++ ) {
				const size_t bytes = (size_t)levels[f][l].size * levels[f][l].size * 4;
				memcpy( dst, levels[f][l].data, bytes );
				dst += bytes;
			}
		}
	}
	for ( size_t i = 0; i < owned.size(); i++ ) { R_StaticFree( owned[i] ); }

	VkImage image = VK_NULL_HANDLE;
	VmaAllocation alloc = NULL;
	{
		VkImageCreateInfo ici = {};
		ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		ici.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
		ici.imageType = VK_IMAGE_TYPE_2D;
		ici.format = VK_FORMAT_R8G8B8A8_UNORM;
		ici.extent = { (uint32_t)size, (uint32_t)size, 1 };
		ici.mipLevels = numLevels;
		ici.arrayLayers = 6;
		ici.samples = VK_SAMPLE_COUNT_1_BIT;
		ici.tiling = VK_IMAGE_TILING_OPTIMAL;
		ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		VmaAllocationCreateInfo aci = {};
		aci.usage = VMA_MEMORY_USAGE_AUTO;
		if ( !vkCheck( vmaCreateImage( vma, &ici, &aci, &image, &alloc, NULL ), "vmaCreateImage(cube)" ) ) {
			vmaDestroyBuffer( vma, staging, stagingAlloc );
			return 0;
		}
	}

	{
		vkResetCommandBuffer( uploadCb, 0 );
		VkCommandBufferBeginInfo bi = {};
		bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer( uploadCb, &bi );

		VkImageMemoryBarrier toDst = {};
		toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		toDst.srcAccessMask = 0;
		toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toDst.image = image;
		toDst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		toDst.subresourceRange.levelCount = numLevels;
		toDst.subresourceRange.layerCount = 6;
		vkCmdPipelineBarrier( uploadCb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			0, 0, NULL, 0, NULL, 1, &toDst );

		VkDeviceSize bufOfs = 0;
		for ( int f = 0; f < 6; f++ ) {
			for ( uint32_t l = 0; l < numLevels; l++ ) {
				VkBufferImageCopy c = {};
				c.bufferOffset = bufOfs;
				c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				c.imageSubresource.mipLevel = l;
				c.imageSubresource.baseArrayLayer = (uint32_t)f;
				c.imageSubresource.layerCount = 1;
				c.imageExtent = { (uint32_t)levels[f][l].size, (uint32_t)levels[f][l].size, 1 };
				vkCmdCopyBufferToImage( uploadCb, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c );
				bufOfs += (VkDeviceSize)levels[f][l].size * levels[f][l].size * 4;
			}
		}

		VkImageMemoryBarrier toRead = toDst;
		toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		vkCmdPipelineBarrier( uploadCb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			0, 0, NULL, 0, NULL, 1, &toRead );

		vkEndCommandBuffer( uploadCb );
		VkSubmitInfo si = {};
		si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		si.commandBufferCount = 1;
		si.pCommandBuffers = &uploadCb;
		vkResetFences( device, 1, &uploadFence );
		vkQueueSubmit( gfxQueue, 1, &si, uploadFence );
		vkWaitForFences( device, 1, &uploadFence, VK_TRUE, UINT64_MAX );
	}
	vmaDestroyBuffer( vma, staging, stagingAlloc );

	ImageRec rec;
	rec.image = image;
	rec.alloc = alloc;
	{
		VkImageViewCreateInfo vi = {};
		vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		vi.image = image;
		vi.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
		vi.format = VK_FORMAT_R8G8B8A8_UNORM;
		vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		vi.subresourceRange.levelCount = numLevels;
		vi.subresourceRange.layerCount = 6;
		if ( !vkCheck( vkCreateImageView( device, &vi, NULL, &rec.view ), "vkCreateImageView(cube)" ) ) {
			vmaDestroyImage( vma, image, alloc );
			return 0;
		}
	}
	rec.sampler = GetSampler( textureFilter, TR_CLAMP, numLevels > 1 );
	rec.live = true;

	for ( size_t i = 0; i < imageTable.size(); i++ ) {
		if ( !imageTable[i].live ) {
			imageTable[i] = rec;
			return (ImageHandle)( i + 1 );
		}
	}
	imageTable.push_back( rec );
	return (ImageHandle)imageTable.size();
}

/*
====================
VulkanBackend::CreateShadowDummy / DestroyShadowDummy

1x1 D32 image (single face or cube) in SHADER_READ_ONLY layout plus a
compare-enabled sampler, so sampler2DShadow / samplerCubeShadow descriptor
slots are always valid. Content is never actually sampled — the shaders gate
the shadow lookups on u_shadowParms.x, which stays 0 until M7 shadow maps.
====================
*/
bool VulkanBackend::CreateShadowDummy( ShadowDummy &d, bool cube ) {
	VkImageCreateInfo ici = {};
	ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ici.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = VK_FORMAT_D32_SFLOAT;
	ici.extent = { 1, 1, 1 };
	ici.mipLevels = 1;
	ici.arrayLayers = cube ? 6 : 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VmaAllocationCreateInfo aci = {};
	aci.usage = VMA_MEMORY_USAGE_AUTO;
	if ( !vkCheck( vmaCreateImage( vma, &ici, &aci, &d.image, &d.alloc, NULL ), "vmaCreateImage(shadow dummy)" ) ) {
		return false;
	}

	// transition to SHADER_READ_ONLY (content undefined, never read)
	vkResetCommandBuffer( uploadCb, 0 );
	VkCommandBufferBeginInfo bi = {};
	bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer( uploadCb, &bi );
	VkImageMemoryBarrier b = {};
	b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	b.srcAccessMask = 0;
	b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = d.image;
	b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	b.subresourceRange.levelCount = 1;
	b.subresourceRange.layerCount = cube ? 6 : 1;
	vkCmdPipelineBarrier( uploadCb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		0, 0, NULL, 0, NULL, 1, &b );
	vkEndCommandBuffer( uploadCb );
	VkSubmitInfo si = {};
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.commandBufferCount = 1;
	si.pCommandBuffers = &uploadCb;
	vkResetFences( device, 1, &uploadFence );
	vkQueueSubmit( gfxQueue, 1, &si, uploadFence );
	vkWaitForFences( device, 1, &uploadFence, VK_TRUE, UINT64_MAX );

	VkImageViewCreateInfo vi = {};
	vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	vi.image = d.image;
	vi.viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
	vi.format = VK_FORMAT_D32_SFLOAT;
	vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	vi.subresourceRange.levelCount = 1;
	vi.subresourceRange.layerCount = cube ? 6 : 1;
	if ( !vkCheck( vkCreateImageView( device, &vi, NULL, &d.view ), "vkCreateImageView(shadow dummy)" ) ) {
		return false;
	}

	VkSamplerCreateInfo sci = {};
	sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
	sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sci.compareEnable = VK_TRUE;
	sci.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
	return vkCheck( vkCreateSampler( device, &sci, NULL, &d.sampler ), "vkCreateSampler(shadow dummy)" );
}

void VulkanBackend::DestroyShadowDummy( ShadowDummy &d ) {
	if ( d.sampler ) { vkDestroySampler( device, d.sampler, NULL ); }
	if ( d.view )    { vkDestroyImageView( device, d.view, NULL ); }
	if ( d.image )   { vmaDestroyImage( vma, d.image, d.alloc ); }
	d = ShadowDummy();
}

/*
===============================================================================

	M7 render targets — depth shadow maps (docs/vulkan-backend.md M7)

	Depth-only offscreen targets, 2D (projected/spot lights) or 6-face cube
	(point lights). shadow_sm/shadow_sm_cube write the light's linear falloff to
	gl_FragDepth, which interaction.frag compares against — an explicit [0,1]
	value, so the GL/VK NDC-depth difference is irrelevant here. The one VK
	nuance: the pass renders with a plain (non-Y-flipped) viewport, so the
	projective (s/q, t/q) write reads back self-consistently, matching GL.

===============================================================================
*/

// Shared LINEAR + clamp + LEQUAL-compare sampler for every shadow map (matches
// GL3's per-target GL_COMPARE_REF_TO_TEXTURE / GL_LEQUAL / GL_LINEAR setup).
VkSampler VulkanBackend::ShadowCompareSampler() {
	if ( shadowCompareSampler == VK_NULL_HANDLE ) {
		VkSamplerCreateInfo sci = {};
		sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
		sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
		sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		sci.compareEnable = VK_TRUE;
		sci.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
		sci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;	// out-of-map = depth 1 = unshadowed
		vkCheck( vkCreateSampler( device, &sci, NULL, &shadowCompareSampler ), "vkCreateSampler(shadow compare)" );
	}
	return shadowCompareSampler;
}

// The single depth-only render pass every shadow target shares: clear depth →
// draw occluders → leave the image SHADER_READ_ONLY for sampling. Cube faces
// are single-layer depth attachments, structurally identical to the 2D map.
bool VulkanBackend::EnsureShadowPass() {
	if ( shadowPass != VK_NULL_HANDLE ) {
		return true;
	}
	VkAttachmentDescription att = {};
	att.format = VK_FORMAT_D32_SFLOAT;
	att.samples = VK_SAMPLE_COUNT_1_BIT;
	att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	att.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	VkAttachmentReference depthRef = { 0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
	VkSubpassDescription sub = {};
	sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	sub.pDepthStencilAttachment = &depthRef;

	// prior sampling of last frame's map must finish before we overwrite it;
	// our depth writes must be visible to the interaction pass that samples it
	VkSubpassDependency deps[2] = {};
	deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
	deps[0].dstSubpass = 0;
	deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
	deps[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	deps[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
	deps[1].srcSubpass = 0;
	deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
	deps[1].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	deps[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

	VkRenderPassCreateInfo rpi = {};
	rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	rpi.attachmentCount = 1;
	rpi.pAttachments = &att;
	rpi.subpassCount = 1;
	rpi.pSubpasses = &sub;
	rpi.dependencyCount = 2;
	rpi.pDependencies = deps;
	return vkCheck( vkCreateRenderPass( device, &rpi, NULL, &shadowPass ), "vkCreateRenderPass(shadow)" );
}

// Allocate a depth image (2D or cube), its sample view + per-face render views,
// framebuffer(s), and register the sampleable ImageRec. Returns false (and
// leaves t clean) on failure.
bool VulkanBackend::CreateDepthTarget( RenderTarget &t, int w, int h, bool cube ) {
	if ( device == VK_NULL_HANDLE || !EnsureShadowPass() ) {
		return false;
	}
	const int layers = cube ? 6 : 1;
	t.cube = cube;
	t.w = w;
	t.h = h;
	t.depthFormat = VK_FORMAT_D32_SFLOAT;

	VkImageCreateInfo ici = {};
	ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ici.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = t.depthFormat;
	ici.extent = { (uint32_t)w, (uint32_t)h, 1 };
	ici.mipLevels = 1;
	ici.arrayLayers = (uint32_t)layers;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VmaAllocationCreateInfo aci = {};
	aci.usage = VMA_MEMORY_USAGE_AUTO;
	if ( !vkCheck( vmaCreateImage( vma, &ici, &aci, &t.depthImage, &t.depthAlloc, NULL ), "vmaCreateImage(shadow map)" ) ) {
		t = RenderTarget();
		return false;
	}

	// sampled view: cube (all 6 layers) or plain 2D
	VkImageViewCreateInfo vwi = {};
	vwi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	vwi.image = t.depthImage;
	vwi.viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
	vwi.format = t.depthFormat;
	vwi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	vwi.subresourceRange.levelCount = 1;
	vwi.subresourceRange.layerCount = (uint32_t)layers;
	if ( !vkCheck( vkCreateImageView( device, &vwi, NULL, &t.sampleView ), "vkCreateImageView(shadow sample)" ) ) {
		FreeTargetObjects( t );
		t = RenderTarget();
		return false;
	}

	// render view(s) + framebuffer(s). 2D reuses the sample view as its
	// attachment; the cube needs one single-layer 2D view per face.
	for ( int f = 0; f < layers; f++ ) {
		VkImageView renderView = t.sampleView;
		if ( cube ) {
			VkImageViewCreateInfo fvi = vwi;
			fvi.viewType = VK_IMAGE_VIEW_TYPE_2D;
			fvi.subresourceRange.baseArrayLayer = (uint32_t)f;
			fvi.subresourceRange.layerCount = 1;
			if ( !vkCheck( vkCreateImageView( device, &fvi, NULL, &t.faceView[f] ), "vkCreateImageView(shadow face)" ) ) {
				FreeTargetObjects( t );
				t = RenderTarget();
				return false;
			}
			renderView = t.faceView[f];
		}
		VkFramebufferCreateInfo fbi = {};
		fbi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		fbi.renderPass = shadowPass;
		fbi.attachmentCount = 1;
		fbi.pAttachments = &renderView;
		fbi.width = (uint32_t)w;
		fbi.height = (uint32_t)h;
		fbi.layers = 1;
		if ( !vkCheck( vkCreateFramebuffer( device, &fbi, NULL, &t.fb[f] ), "vkCreateFramebuffer(shadow)" ) ) {
			FreeTargetObjects( t );
			t = RenderTarget();
			return false;
		}
	}

	// register the sampleable ImageRec (borrowed image/view — freed by the RT,
	// not the generic image path). The descriptor writer binds it at unit 7/8.
	ImageRec rec;
	rec.image = t.depthImage;
	rec.alloc = NULL;					// owned by the RenderTarget
	rec.view = t.sampleView;			// owned by the RenderTarget
	rec.sampler = ShadowCompareSampler();
	rec.live = true;
	rec.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	rec.isDepth = true;
	rec.width = w;
	rec.height = h;
	t.sampleImage = 0;
	for ( size_t i = 0; i < imageTable.size(); i++ ) {
		if ( !imageTable[i].live ) {
			imageTable[i] = rec;
			t.sampleImage = (ImageHandle)( i + 1 );
			break;
		}
	}
	if ( t.sampleImage == 0 ) {
		imageTable.push_back( rec );
		t.sampleImage = (ImageHandle)imageTable.size();
	}

	t.live = true;
	return true;
}

RenderTargetHandle VulkanBackend::CreateRenderTarget( ImageFormat fmt, int w, int h ) {
	if ( device == VK_NULL_HANDLE || w <= 0 || h <= 0 ) {
		return 0;
	}
	// IF_DEPTH24 = a 2D shadow map (depth-only). IF_RGBA16F/IF_RGBA8 = a color-only
	// target sampled as an ordinary texture: the HDR FXAA ping, and later the SSR
	// march/history buffers. IF_DEPTH24_STENCIL8 (SSAO color+depth) still stubbed.
	if ( fmt == IF_DEPTH24 ) {
		int slot = AllocTargetSlot();
		if ( !CreateDepthTarget( targetTable[slot], w, h, false ) ) {
			targetTable[slot] = RenderTarget();
			return 0;
		}
		return (RenderTargetHandle)( slot + 1 );
	}
	VkFormat cf = ( fmt == IF_RGBA16F ) ? VK_FORMAT_R16G16B16A16_SFLOAT
	            : ( fmt == IF_RGBA8 ) ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_UNDEFINED;
	if ( cf == VK_FORMAT_UNDEFINED ) {
		return 0;
	}
	int slot = AllocTargetSlot();
	if ( !CreateColorTarget( targetTable[slot], w, h, cf, 1, /*ds*/false, /*frameCapable*/false ) ) {
		targetTable[slot] = RenderTarget();
		return 0;
	}
	return (RenderTargetHandle)( slot + 1 );
}

RenderTargetHandle VulkanBackend::CreateRenderTargetCube( ImageFormat fmt, int size ) {
	if ( device == VK_NULL_HANDLE || size <= 0 ) {
		return 0;
	}
	if ( fmt != IF_DEPTH24 ) {
		return 0;
	}
	int slot = -1;
	for ( size_t i = 0; i < targetTable.size(); i++ ) {
		if ( !targetTable[i].live ) { slot = (int)i; break; }
	}
	if ( slot < 0 ) {
		targetTable.push_back( RenderTarget() );
		slot = (int)targetTable.size() - 1;
	}
	if ( !CreateDepthTarget( targetTable[slot], size, size, true ) ) {
		return 0;
	}
	return (RenderTargetHandle)( slot + 1 );
}

ImageHandle VulkanBackend::GetRenderTargetImage( RenderTargetHandle rt ) {
	RenderTarget *t = LookupTarget( rt );
	if ( t == NULL ) {
		return 0;
	}
	return t->colorTarget ? t->colorSampleImage[0] : t->sampleImage;
}

// stash the interrupted scene viewport, point the dynamic viewport/scissor at
// the target, and mark the target pass open. Shared tail of every BeginTargetPass
// / BeginCubeFacePass. flipY: shadow maps render un-flipped (projective read/write
// self-consistency); color targets render flipped like the scene (their fullscreen
// resolve samples them 1:1 back onto sceneColor).
void VulkanBackend::EnterTargetPass( int w, int h, bool flipY, VkRenderPass pipePass,
                                     uint8_t passClass, int colorAtt ) {
	memcpy( savedVpRect, vpRect, sizeof( vpRect ) );
	memcpy( savedScRect, scRect, sizeof( scRect ) );
	vpRect[0] = 0; vpRect[1] = 0; vpRect[2] = w; vpRect[3] = h;
	scRect[0] = 0; scRect[1] = 0; scRect[2] = w; scRect[3] = h;
	curRenderH = h;
	curFlipY = flipY;
	curPipelinePass = pipePass;
	curPassClass = passClass;
	curColorAtt = colorAtt;
	insideTargetPass = true;
	dynStateDirty = true;
}

// begin an offscreen pass into a target. 2D depth (shadow map) and color (the HDR
// FXAA ping, later SSAO/SSR fullscreen buffers) both route here; cube depth faces
// go through BeginCubeFacePass. Suspends the scene pass; the scene resumes on the
// next Draw's EnsureScenePass (or this target's EndPass).
void VulkanBackend::BeginTargetPass( RenderTargetHandle rt, const ClearArgs *clear ) {
	if ( !frameOpen || skipFrame ) {
		return;
	}
	RenderTarget *t = LookupTarget( rt );
	if ( t == NULL || t->cube ) {
		if ( frameOpen ) { fakePassDepth++; }	// invalid / wrong entry point → balance the EndPass
		return;
	}
	VkCommandBuffer cb = frames[frameIndex].cb;
	if ( insideScenePass ) {
		vkCmdEndRenderPass( cb );				// suspend the scene pass
		insideScenePass = false;
	}

	VkRenderPassBeginInfo rbi = {};
	rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	rbi.renderArea.extent = { (uint32_t)t->w, (uint32_t)t->h };

	if ( t->colorTarget ) {
		// clear each color attachment (fullscreen draws overwrite it anyway) + ds
		VkClearValue cv[3] = {};
		if ( clear != NULL && clear->color ) {
			for ( int c = 0; c < t->colorCount; c++ ) {
				cv[c].color.float32[0] = clear->rgba[0]; cv[c].color.float32[1] = clear->rgba[1];
				cv[c].color.float32[2] = clear->rgba[2]; cv[c].color.float32[3] = clear->rgba[3];
			}
		}
		if ( t->hasDepth ) { cv[t->colorCount].depthStencil.depth = 1.0f; }
		rbi.renderPass = t->colorClearPass;
		rbi.framebuffer = t->colorFb;
		rbi.clearValueCount = (uint32_t)( t->colorCount + ( t->hasDepth ? 1 : 0 ) );
		rbi.pClearValues = cv;
		vkCmdBeginRenderPass( cb, &rbi, VK_SUBPASS_CONTENTS_INLINE );
		EnterTargetPass( t->w, t->h, /*flipY*/true, t->colorClearPass, t->passClass, t->colorCount );
		t->everWritten = true;
	} else {
		VkClearValue cv = {};
		cv.depthStencil.depth = 1.0f;
		rbi.renderPass = shadowPass;
		rbi.framebuffer = t->fb[0];
		rbi.clearValueCount = 1;
		rbi.pClearValues = &cv;
		vkCmdBeginRenderPass( cb, &rbi, VK_SUBPASS_CONTENTS_INLINE );
		EnterTargetPass( t->w, t->h, /*flipY*/false, shadowPass, /*PC_SHADOW*/1, /*colorAtt*/0 );
	}
}

void VulkanBackend::BeginCubeFacePass( RenderTargetHandle rt, int face, const ClearArgs *clear ) {
	if ( !frameOpen || skipFrame ) {
		return;
	}
	RenderTarget *t = LookupTarget( rt );
	if ( t == NULL || !t->cube || face < 0 || face > 5 ) {
		if ( frameOpen ) { fakePassDepth++; }
		return;
	}
	VkCommandBuffer cb = frames[frameIndex].cb;
	if ( insideScenePass ) {
		vkCmdEndRenderPass( cb );
		insideScenePass = false;
	}
	VkClearValue cv = {};
	cv.depthStencil.depth = 1.0f;
	VkRenderPassBeginInfo rbi = {};
	rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	rbi.renderPass = shadowPass;
	rbi.framebuffer = t->fb[face];
	rbi.renderArea.extent = { (uint32_t)t->w, (uint32_t)t->h };
	rbi.clearValueCount = 1;
	rbi.pClearValues = &cv;
	vkCmdBeginRenderPass( cb, &rbi, VK_SUBPASS_CONTENTS_INLINE );

	EnterTargetPass( t->w, t->h, /*flipY*/false, shadowPass, /*PC_SHADOW*/1, /*colorAtt*/0 );
}

// free a target's Vulkan objects (framebuffers, views, image). Never touches
// the imageTable slot — that is cleared eagerly by DestroyRenderTarget so no
// new draw samples a target being torn down.
void VulkanBackend::FreeTargetObjects( RenderTarget &t ) {
	for ( int f = 0; f < 6; f++ ) {
		if ( t.fb[f] )       { vkDestroyFramebuffer( device, t.fb[f], NULL ); t.fb[f] = VK_NULL_HANDLE; }
	}
	for ( int f = 0; f < 6; f++ ) {
		if ( t.faceView[f] ) { vkDestroyImageView( device, t.faceView[f], NULL ); t.faceView[f] = VK_NULL_HANDLE; }
	}
	if ( t.sampleView )      { vkDestroyImageView( device, t.sampleView, NULL ); t.sampleView = VK_NULL_HANDLE; }
	if ( t.depthImage )      { vmaDestroyImage( vma, t.depthImage, t.depthAlloc ); t.depthImage = VK_NULL_HANDLE; t.depthAlloc = NULL; }

	// color-target resources
	if ( t.colorFb )         { vkDestroyFramebuffer( device, t.colorFb, NULL ); t.colorFb = VK_NULL_HANDLE; }
	if ( t.colorClearPass )  { vkDestroyRenderPass( device, t.colorClearPass, NULL ); t.colorClearPass = VK_NULL_HANDLE; }
	if ( t.colorLoadPass )   { vkDestroyRenderPass( device, t.colorLoadPass, NULL ); t.colorLoadPass = VK_NULL_HANDLE; }
	if ( t.colorClearDSPass ){ vkDestroyRenderPass( device, t.colorClearDSPass, NULL ); t.colorClearDSPass = VK_NULL_HANDLE; }
	for ( int c = 0; c < 2; c++ ) {
		if ( t.colorView[c] )  { vkDestroyImageView( device, t.colorView[c], NULL ); t.colorView[c] = VK_NULL_HANDLE; }
		if ( t.colorImage[c] ) { vmaDestroyImage( vma, t.colorImage[c], t.colorAlloc[c] ); t.colorImage[c] = VK_NULL_HANDLE; t.colorAlloc[c] = NULL; }
	}
	if ( t.dsView )          { vkDestroyImageView( device, t.dsView, NULL ); t.dsView = VK_NULL_HANDLE; }
	if ( t.dsImage )         { vmaDestroyImage( vma, t.dsImage, t.dsAlloc ); t.dsImage = VK_NULL_HANDLE; t.dsAlloc = NULL; }
}

// free every imageTable slot a target lent out (depth sample + color samples)
void VulkanBackend::ReleaseTargetSampleSlots( RenderTarget &t ) {
	ImageHandle handles[3] = { t.sampleImage, t.colorSampleImage[0], t.colorSampleImage[1] };
	for ( int i = 0; i < 3; i++ ) {
		ImageHandle h = handles[i];
		if ( h >= 1 && h <= (ImageHandle)imageTable.size() ) {
			imageTable[h - 1] = ImageRec();
			imageTable[h - 1].live = false;
		}
	}
	t.sampleImage = 0;
	t.colorSampleImage[0] = t.colorSampleImage[1] = 0;
}

void VulkanBackend::DestroyRenderTarget( RenderTargetHandle rt ) {
	RenderTarget *t = LookupTarget( rt );
	if ( t == NULL ) {
		return;
	}
	// stop new draws from sampling it immediately (the ImageRec slots are freed
	// for reuse), but defer the Vulkan-object destruction until this frame
	// slot's GPU work is fenced off — the shadow caches evict mid-frame.
	ReleaseTargetSampleSlots( *t );
	retiredTargets[frameIndex].push_back( *t );
	*t = RenderTarget();		// free the target-table slot
	InvalidateTextureSets();	// a freed sample handle may be cached in a texture set (shadow unit 7/8, HDR unit 0)
}

void VulkanBackend::DrainRetiredTargets( int slot ) {
	for ( size_t i = 0; i < retiredTargets[slot].size(); i++ ) {
		FreeTargetObjects( retiredTargets[slot][i] );
	}
	retiredTargets[slot].clear();
}

// teardown: device is idle. Free every retired + live target and the shared
// pass/sampler, and clear their imageTable slots.
void VulkanBackend::DestroyAllTargets() {
	for ( int slot = 0; slot < FRAMES_IN_FLIGHT; slot++ ) {
		DrainRetiredTargets( slot );
	}
	for ( size_t i = 0; i < targetTable.size(); i++ ) {
		RenderTarget &t = targetTable[i];
		if ( !t.live ) {
			continue;
		}
		ReleaseTargetSampleSlots( t );
		FreeTargetObjects( t );
		t = RenderTarget();
	}
	targetTable.clear();
	if ( shadowPass )           { vkDestroyRenderPass( device, shadowPass, NULL ); shadowPass = VK_NULL_HANDLE; }
	if ( shadowCompareSampler ) { vkDestroySampler( device, shadowCompareSampler, NULL ); shadowCompareSampler = VK_NULL_HANDLE; }
}

/*
===============================================================================

	M7 color render targets — HDR scene buffer (docs/hdr-pipeline.md, r_hdr)

	A color target carries 1-2 sampleable color attachments (+ optional depth-
	stencil) and, unlike the depth shadow maps, is *sampled* as an ordinary
	texture after being rendered into. Its render passes therefore end the color
	attachment in SHADER_READ_ONLY so the descriptor path (which assumes that
	layout) binds it with no special case; the load-pass initialLayout is the same
	SHADER_READ_ONLY, so the between-pass round-trip is render-pass-managed.

	The HDR scene buffer is a *frame* target (SetFrameTarget): the whole scene
	renders into an RGBA16F color + depth-stencil target across the usual
	clear / clearDS / load pass sequence, then hdrresolve.frag samples it back
	onto the swapchain sceneColor. Colour is float, so fog/gradient banding is
	gone; matches the GL3 path exactly.

===============================================================================
*/

// format signature -> pipeline-key pass class. Scene pipelines rendered into the
// RGBA8 swapchain path (class 0) and the RGBA16F HDR buffer (class 2) are render-
// pass-incompatible (different color format), so they must be distinct cache keys.
uint8_t VulkanBackend::PassClassFor( VkFormat colorFmt, bool hasDepth, int colorCount ) const {
	if ( colorFmt == VK_FORMAT_R16G16B16A16_SFLOAT ) {
		return hasDepth ? 2 : 3;			// HDR scene buffer / RGBA16F color-only (AA ping, SSR)
	}
	// RGBA8 family (SSAO buffers later)
	if ( !hasDepth )      { return 4; }		// color-only RGBA8
	return colorCount >= 2 ? 6 : 5;			// color+depth (+MRT)
}

// The per-target render passes. colorClearPass always; the load/clearDS variants
// only for a frame target (a nested one-shot target never re-loads its content).
bool VulkanBackend::BuildColorPasses( RenderTarget &t, bool frameCapable ) {
	const int nColor = t.colorCount;
	const int variants = frameCapable ? 3 : 1;		// 0=clear, 1=load, 2=clearDS
	for ( int v = 0; v < variants; v++ ) {
		const bool clearColor = ( v == 0 );			// clear resets color; load/clearDS keep it
		const bool clearDS    = ( v == 0 || v == 2 );

		VkAttachmentDescription atts[3] = {};
		VkAttachmentReference   colorRefs[2] = {};
		for ( int c = 0; c < nColor; c++ ) {
			atts[c].format = t.colorFormat;
			atts[c].samples = VK_SAMPLE_COUNT_1_BIT;
			atts[c].loadOp = clearColor ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
			atts[c].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			atts[c].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			atts[c].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			atts[c].initialLayout = clearColor ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			atts[c].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			colorRefs[c].attachment = (uint32_t)c;
			colorRefs[c].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		}
		VkAttachmentReference depthRef = { (uint32_t)nColor, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
		if ( t.hasDepth ) {
			VkAttachmentDescription &d = atts[nColor];
			d.format = sceneDepthFormat;
			d.samples = VK_SAMPLE_COUNT_1_BIT;
			d.loadOp = clearDS ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
			d.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			d.stencilLoadOp = clearDS ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
			d.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
			d.initialLayout = clearDS ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			d.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		}

		VkSubpassDescription sub = {};
		sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		sub.colorAttachmentCount = (uint32_t)nColor;
		sub.pColorAttachments = colorRefs;
		sub.pDepthStencilAttachment = t.hasDepth ? &depthRef : NULL;

		// prior sample of this target (last pass / last frame) must finish before we
		// overwrite it; our writes must be visible to whoever samples it next
		VkSubpassDependency deps[2] = {};
		deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
		deps[0].dstSubpass = 0;
		deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
		                     | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
		                     | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
		                      | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
		deps[1].srcSubpass = 0;
		deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
		deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
		deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;

		VkRenderPassCreateInfo rpi = {};
		rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
		rpi.attachmentCount = (uint32_t)( nColor + ( t.hasDepth ? 1 : 0 ) );
		rpi.pAttachments = atts;
		rpi.subpassCount = 1;
		rpi.pSubpasses = &sub;
		rpi.dependencyCount = 2;
		rpi.pDependencies = deps;

		VkRenderPass *dst = ( v == 0 ) ? &t.colorClearPass : ( v == 1 ? &t.colorLoadPass : &t.colorClearDSPass );
		if ( !vkCheck( vkCreateRenderPass( device, &rpi, NULL, dst ), "vkCreateRenderPass(color target)" ) ) {
			return false;
		}
	}
	return true;
}

bool VulkanBackend::CreateColorTarget( RenderTarget &t, int w, int h, VkFormat colorFmt,
                                       int colorCount, bool wantDepthStencil, bool frameCapable ) {
	if ( device == VK_NULL_HANDLE || colorCount < 1 || colorCount > 2 ) {
		return false;
	}
	if ( wantDepthStencil && sceneDepthFormat == VK_FORMAT_UNDEFINED ) {
		return false;
	}
	t.colorTarget = true;
	t.colorCount = colorCount;
	t.colorFormat = colorFmt;
	t.hasDepth = wantDepthStencil;
	t.w = w;
	t.h = h;
	t.passClass = PassClassFor( colorFmt, wantDepthStencil, colorCount );

	VmaAllocationCreateInfo aci = {};
	aci.usage = VMA_MEMORY_USAGE_AUTO;

	for ( int c = 0; c < colorCount; c++ ) {
		VkImageCreateInfo ici = {};
		ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		ici.imageType = VK_IMAGE_TYPE_2D;
		ici.format = colorFmt;
		ici.extent = { (uint32_t)w, (uint32_t)h, 1 };
		ici.mipLevels = 1;
		ici.arrayLayers = 1;
		ici.samples = VK_SAMPLE_COUNT_1_BIT;
		ici.tiling = VK_IMAGE_TILING_OPTIMAL;
		// SAMPLED: read back by the resolve/next feature; TRANSFER_SRC: the M5
		// _currentRender capture blits from it while this is the frame target
		ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
		          | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		if ( !vkCheck( vmaCreateImage( vma, &ici, &aci, &t.colorImage[c], &t.colorAlloc[c], NULL ),
		               "vmaCreateImage(color target)" ) ) {
			FreeTargetObjects( t );
			return false;
		}
		VkImageViewCreateInfo vwi = {};
		vwi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		vwi.image = t.colorImage[c];
		vwi.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vwi.format = colorFmt;
		vwi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		vwi.subresourceRange.levelCount = 1;
		vwi.subresourceRange.layerCount = 1;
		if ( !vkCheck( vkCreateImageView( device, &vwi, NULL, &t.colorView[c] ), "vkCreateImageView(color target)" ) ) {
			FreeTargetObjects( t );
			return false;
		}
	}

	if ( wantDepthStencil ) {
		VkImageCreateInfo ici = {};
		ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		ici.imageType = VK_IMAGE_TYPE_2D;
		ici.format = sceneDepthFormat;
		ici.extent = { (uint32_t)w, (uint32_t)h, 1 };
		ici.mipLevels = 1;
		ici.arrayLayers = 1;
		ici.samples = VK_SAMPLE_COUNT_1_BIT;
		ici.tiling = VK_IMAGE_TILING_OPTIMAL;
		ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		if ( !vkCheck( vmaCreateImage( vma, &ici, &aci, &t.dsImage, &t.dsAlloc, NULL ), "vmaCreateImage(target ds)" ) ) {
			FreeTargetObjects( t );
			return false;
		}
		VkImageViewCreateInfo vwi = {};
		vwi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		vwi.image = t.dsImage;
		vwi.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vwi.format = sceneDepthFormat;
		vwi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
		vwi.subresourceRange.levelCount = 1;
		vwi.subresourceRange.layerCount = 1;
		if ( !vkCheck( vkCreateImageView( device, &vwi, NULL, &t.dsView ), "vkCreateImageView(target ds)" ) ) {
			FreeTargetObjects( t );
			return false;
		}
	}

	if ( !BuildColorPasses( t, frameCapable ) ) {
		FreeTargetObjects( t );
		return false;
	}

	VkImageView views[3] = {};
	int nv = 0;
	for ( int c = 0; c < colorCount; c++ ) { views[nv++] = t.colorView[c]; }
	if ( wantDepthStencil ) { views[nv++] = t.dsView; }
	VkFramebufferCreateInfo fbi = {};
	fbi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	fbi.renderPass = t.colorClearPass;		// compatible with the load/clearDS variants
	fbi.attachmentCount = (uint32_t)nv;
	fbi.pAttachments = views;
	fbi.width = (uint32_t)w;
	fbi.height = (uint32_t)h;
	fbi.layers = 1;
	if ( !vkCheck( vkCreateFramebuffer( device, &fbi, NULL, &t.colorFb ), "vkCreateFramebuffer(color target)" ) ) {
		FreeTargetObjects( t );
		return false;
	}

	// register each color attachment as a sampleable ImageRec (SHADER_READ_ONLY,
	// linear/clamp — the resolve samples 1:1). Views/images owned by the target.
	for ( int c = 0; c < colorCount; c++ ) {
		ImageRec rec;
		rec.image = t.colorImage[c];
		rec.alloc = NULL;
		rec.view = t.colorView[c];
		rec.sampler = GetSampler( TF_LINEAR, TR_CLAMP, false );
		rec.live = true;
		rec.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		rec.isDepth = false;
		rec.isColorTarget = true;
		rec.width = w;
		rec.height = h;
		ImageHandle handle = 0;
		for ( size_t i = 0; i < imageTable.size(); i++ ) {
			if ( !imageTable[i].live ) { imageTable[i] = rec; handle = (ImageHandle)( i + 1 ); break; }
		}
		if ( handle == 0 ) {
			imageTable.push_back( rec );
			handle = (ImageHandle)imageTable.size();
		}
		t.colorSampleImage[c] = handle;
	}

	t.everWritten = false;
	t.live = true;
	return true;
}

// slot allocation shared by every Create* entry point
int VulkanBackend::AllocTargetSlot() {
	for ( size_t i = 0; i < targetTable.size(); i++ ) {
		if ( !targetTable[i].live ) { return (int)i; }
	}
	targetTable.push_back( RenderTarget() );
	return (int)targetTable.size() - 1;
}

RenderTargetHandle VulkanBackend::CreateRenderTargetColorDepthStencil( ImageFormat fmt, int w, int h ) {
	if ( device == VK_NULL_HANDLE || w <= 0 || h <= 0 ) {
		return 0;
	}
	VkFormat cf = ( fmt == IF_RGBA16F ) ? VK_FORMAT_R16G16B16A16_SFLOAT
	            : ( fmt == IF_RGBA8 ) ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_UNDEFINED;
	if ( cf == VK_FORMAT_UNDEFINED ) {
		common->Warning( "VK CreateRenderTargetColorDepthStencil: only IF_RGBA8/IF_RGBA16F supported" );
		return 0;
	}
	int slot = AllocTargetSlot();
	if ( !CreateColorTarget( targetTable[slot], w, h, cf, 1, /*ds*/true, /*frameCapable*/true ) ) {
		targetTable[slot] = RenderTarget();
		return 0;
	}
	common->DPrintf( "VK: created %dx%d %s color+depth-stencil frame target (handle %d)\n",
		w, h, fmt == IF_RGBA16F ? "RGBA16F" : "RGBA8", slot + 1 );
	return (RenderTargetHandle)( slot + 1 );
}

ImageHandle VulkanBackend::GetRenderTargetImage2( RenderTargetHandle rt ) {
	RenderTarget *t = LookupTarget( rt );
	return ( t && t->colorCount >= 2 ) ? t->colorSampleImage[1] : 0;
}

// the color/depth image the current frame target resolves to (for M5 captures)
VkImage VulkanBackend::FrameColorImage() const {
	if ( frameTarget != 0 ) {
		const RenderTarget &t = targetTable[frameTarget - 1];
		if ( t.live && t.colorTarget ) { return t.colorImage[0]; }
	}
	return sceneColor;
}
VkImageLayout VulkanBackend::FrameColorBetweenLayout() const {
	// a sampled color target rests in SHADER_READ_ONLY between passes; the
	// swapchain sceneColor rests in TRANSFER_SRC (its render passes' finalLayout)
	if ( frameTarget != 0 ) {
		const RenderTarget &t = targetTable[frameTarget - 1];
		if ( t.live && t.colorTarget ) { return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; }
	}
	return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
}
VkImage VulkanBackend::FrameDepthImage() const {
	if ( frameTarget != 0 ) {
		const RenderTarget &t = targetTable[frameTarget - 1];
		if ( t.live && t.hasDepth ) { return t.dsImage; }
	}
	return sceneDepth;
}

// SetFrameTarget: route the whole scene into an offscreen color target (HDR), or
// back to the swapchain sceneColor path (rt == 0). Any open scene pass is closed
// so the next Draw re-opens against the new destination.
void VulkanBackend::SetFrameTarget( RenderTargetHandle rt ) {
	if ( !frameOpen || skipFrame ) {
		frameTarget = rt;		// remembered; takes effect when a frame is open
		return;
	}
	if ( rt != 0 ) {
		RenderTarget *t = LookupTarget( rt );
		if ( t == NULL || !t->colorTarget ) {
			return;				// invalid handle — stay where we are
		}
	}
	if ( insideScenePass ) {
		vkCmdEndRenderPass( frames[frameIndex].cb );
		insideScenePass = false;
	}
	frameTarget = rt;
	// the resume viewport/extent tracks the new destination
	if ( rt != 0 ) {
		RenderTarget *t = &targetTable[rt - 1];
		curRenderH = t->h;
		curPipelinePass = t->colorClearPass;
		curPassClass = t->passClass;
		curColorAtt = t->colorCount;
	} else {
		curRenderH = (int)sceneExtent.height;
		curPipelinePass = passClear;
		curPassClass = 0;
		curColorAtt = 1;
	}
	curFlipY = true;
	dynStateDirty = true;
}

/*
====================
VulkanBackend::GetPipeline

(stateBits, shader, vertexLayout, cullType) -> VkPipeline, created on first
use against the scene render pass. The GLS_* decode mirrors the GL3 backend's
ApplyState table exactly.
====================
*/
VkPipeline VulkanBackend::GetPipeline( const PipelineDesc &desc ) {
	const int RELEVANT = GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS | GLS_DEPTHMASK
	                   | GLS_REDMASK | GLS_GREENMASK | GLS_BLUEMASK | GLS_ALPHAMASK
	                   | GLS_POLYMODE_LINE | GLS_DEPTHFUNC_EQUAL | GLS_DEPTHFUNC_LESS | GLS_DEPTHFUNC_ALWAYS;
	const unsigned int bits = (unsigned int)( desc.stateBits & RELEVANT );
	// curPassClass (bits 24-31, free above RELEVANT's bit 17) keeps pipelines for
	// render-pass-incompatible destinations apart: the same shader+state built for
	// the RGBA8 swapchain scene (class 0) and the RGBA16F HDR buffer (class 2) are
	// distinct cache entries built against distinct render passes.
	const unsigned long long key = (unsigned long long)bits
		| ( (unsigned long long)( curPassClass & 0xff ) << 24 )
		| ( (unsigned long long)( desc.shader & 0xffff ) << 32 )
		| ( (unsigned long long)( desc.vertexLayout & 0xf ) << 48 )
		| ( (unsigned long long)( desc.cullType & 0xf ) << 52 )
		| ( (unsigned long long)( desc.stencilState & 0xf ) << 56 )
		| ( (unsigned long long)( desc.topology & 0xf ) << 60 );

	auto it = pipelineCache.find( key );
	if ( it != pipelineCache.end() ) {
		return it->second;
	}

	if ( desc.shader < 1 || desc.shader > (ShaderHandle)shaderTable.size()
	     || shaderTable[desc.shader - 1].failed ) {
		pipelineCache[key] = VK_NULL_HANDLE;
		return VK_NULL_HANDLE;
	}
	const ShaderRec &sh = shaderTable[desc.shader - 1];

	VkPipelineShaderStageCreateInfo stages[2] = {};
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = sh.vert;
	stages[0].pName = "main";
	stages[1] = stages[0];
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = sh.frag;

	// vertex layouts (must match the GL3 VAO setups / shader locations)
	VkVertexInputBindingDescription binding = {};
	VkVertexInputAttributeDescription attrs[6] = {};
	uint32_t attrCount = 0;
	if ( desc.vertexLayout == VL_DRAWVERT ) {
		binding.stride = sizeof( idDrawVert );
		attrs[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, (uint32_t)offsetof( idDrawVert, xyz ) };
		attrs[1] = { 1, 0, VK_FORMAT_R32G32_SFLOAT,    (uint32_t)offsetof( idDrawVert, st ) };
		attrs[2] = { 2, 0, VK_FORMAT_R32G32B32_SFLOAT, (uint32_t)offsetof( idDrawVert, normal ) };
		attrs[3] = { 3, 0, VK_FORMAT_R32G32B32_SFLOAT, (uint32_t)offsetof( idDrawVert, tangents ) };
		attrs[4] = { 4, 0, VK_FORMAT_R32G32B32_SFLOAT, (uint32_t)( offsetof( idDrawVert, tangents ) + sizeof( idVec3 ) ) };
		attrs[5] = { 5, 0, VK_FORMAT_R8G8B8A8_UNORM,   (uint32_t)offsetof( idDrawVert, color ) };
		attrCount = 6;
	} else if ( desc.vertexLayout == VL_IMMEDIATE ) {
		// imVert_t: float xyz[3] @0, float st[2] @12, byte color[4] @20 (24 B).
		// Same locations generic.vert reads (0/1/5); the missing 2/3/4 are
		// unconsumed (the benign "attribute not consumed" pipeline warning).
		binding.stride = 24;
		attrs[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 };
		attrs[1] = { 1, 0, VK_FORMAT_R32G32_SFLOAT,    12 };
		attrs[2] = { 5, 0, VK_FORMAT_R8G8B8A8_UNORM,   20 };
		attrCount = 3;
	} else {
		binding.stride = sizeof( shadowCache_t );
		attrs[0] = { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0 };
		attrCount = 1;
	}
	binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

	VkPipelineVertexInputStateCreateInfo vin = {};
	vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vin.vertexBindingDescriptionCount = 1;
	vin.pVertexBindingDescriptions = &binding;
	vin.vertexAttributeDescriptionCount = attrCount;
	vin.pVertexAttributeDescriptions = attrs;

	VkPipelineInputAssemblyStateCreateInfo ia = {};
	ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	// desc.topology carries a GL primMode for immediate-mode debug draws; -1
	// (every normal draw) stays triangle list. GL_LINE_LOOP has no VK analogue —
	// degrade to a line strip (leaves the loop open; only debug wireframes).
	switch ( desc.topology ) {
	case GL_POINTS:         ia.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
	case GL_LINES:          ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
	case GL_LINE_LOOP:
	case GL_LINE_STRIP:     ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
	case GL_TRIANGLE_STRIP: ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
	case GL_TRIANGLE_FAN:   ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; break;
	default:                ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
	}

	VkPipelineViewportStateCreateInfo vp = {};
	vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	vp.viewportCount = 1;
	vp.scissorCount = 1;

	// Winding (verified empirically with the M2 frame dump, 2026-08-02): with
	// the negative-height viewport, VK's framebuffer-space winding for a GUI
	// quad comes out such that COUNTER_CLOCKWISE-as-front + the legacy cull
	// mapping (front-sided culls FRONT, as GL3Backend::ApplyCull) keeps
	// idTech4's CW-wound triangles — CLOCKWISE-as-front culled every GUI quad
	// in the engine (all-black menu).
	VkPipelineRasterizationStateCreateInfo rs = {};
	rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rs.polygonMode = ( ( bits & GLS_POLYMODE_LINE ) && haveFillModeNonSolid )
		? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
	rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rs.cullMode = desc.cullType == CT_TWO_SIDED ? VK_CULL_MODE_NONE
	            : ( desc.cullType == CT_BACK_SIDED ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_FRONT_BIT );
	rs.lineWidth = 1.0f;
	// depth bias always enabled + dynamic (SetPolygonOffset): (0, 0) — the
	// BeginFrame default — is exactly "disabled"
	rs.depthBiasEnable = VK_TRUE;

	VkPipelineMultisampleStateCreateInfo ms = {};
	ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineDepthStencilStateCreateInfo ds = {};
	ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	ds.depthTestEnable = VK_TRUE;
	ds.depthWriteEnable = ( bits & GLS_DEPTHMASK ) ? VK_FALSE : VK_TRUE;
	ds.depthCompareOp = ( bits & GLS_DEPTHFUNC_EQUAL ) ? VK_COMPARE_OP_EQUAL
	                  : ( ( bits & GLS_DEPTHFUNC_ALWAYS ) ? VK_COMPARE_OP_ALWAYS : VK_COMPARE_OP_LESS_OR_EQUAL );

	// M4 stencil: the compact StencilState enum bakes the handful of configs
	// RB_RHI_StencilShadowPass / the light loop drive through qglStencil* on
	// GL. "front" is GL front-facing; the CCW front-face below preserves GL's
	// facing exactly, so the ops translate literally (mirror variants swap
	// front/back like the firstFace/secondFace swap in the GL path).
	if ( desc.stencilState != SS_DISABLED ) {
		ds.stencilTestEnable = VK_TRUE;
		VkStencilOpState front = {};
		front.failOp = front.passOp = front.depthFailOp = VK_STENCIL_OP_KEEP;
		front.compareOp = VK_COMPARE_OP_ALWAYS;
		front.compareMask = 0xff;
		front.writeMask = 0xff;
		front.reference = 128;
		VkStencilOpState back = front;

		int vol = desc.stencilState;
		bool mirror = false;
		switch ( vol ) {
		case SS_VOLUME_PRELOAD_MIRROR:	vol = SS_VOLUME_PRELOAD; mirror = true; break;
		case SS_VOLUME_ZPASS_MIRROR:	vol = SS_VOLUME_ZPASS;   mirror = true; break;
		case SS_VOLUME_ZFAIL_MIRROR:	vol = SS_VOLUME_ZFAIL;   mirror = true; break;
		default: break;
		}
		switch ( vol ) {
		case SS_ALWAYS:
			break;			// ALWAYS + KEEP, ref 128 — the defaults above
		case SS_SHADOW_TEST:
			// qglStencilFunc( GL_GEQUAL, 128, 255 ): GL tests ref >= stored,
			// Vulkan compareOp compares (ref & mask) OP (stored & mask)
			front.compareOp = back.compareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
			break;
		case SS_VOLUME_PRELOAD:
			// qglStencilOpSeparate( BACK, KEEP, DECR, DECR ) / ( FRONT, KEEP, INCR, INCR )
			front.reference = back.reference = 1;
			front.depthFailOp = front.passOp = VK_STENCIL_OP_INCREMENT_AND_WRAP;
			back.depthFailOp = back.passOp = VK_STENCIL_OP_DECREMENT_AND_WRAP;
			break;
		case SS_VOLUME_ZPASS:
			// qglStencilOpSeparate( BACK, KEEP, KEEP, INCR ) / ( FRONT, KEEP, KEEP, DECR )
			front.reference = back.reference = 1;
			front.passOp = VK_STENCIL_OP_DECREMENT_AND_WRAP;
			back.passOp = VK_STENCIL_OP_INCREMENT_AND_WRAP;
			break;
		case SS_VOLUME_ZFAIL:
			// qglStencilOpSeparate( BACK, KEEP, DECR, KEEP ) / ( FRONT, KEEP, INCR, KEEP )
			front.reference = back.reference = 1;
			front.depthFailOp = VK_STENCIL_OP_INCREMENT_AND_WRAP;
			back.depthFailOp = VK_STENCIL_OP_DECREMENT_AND_WRAP;
			break;
		}
		if ( mirror ) {
			VkStencilOpState tmp = front; front = back; back = tmp;
		}
		ds.front = front;
		ds.back = back;
	}

	auto mapBlend = []( unsigned int b, bool isSrc ) -> VkBlendFactor {
		if ( isSrc ) {
			switch ( b ) {
			case GLS_SRCBLEND_ZERO:					return VK_BLEND_FACTOR_ZERO;
			case GLS_SRCBLEND_ONE:					return VK_BLEND_FACTOR_ONE;
			case GLS_SRCBLEND_DST_COLOR:			return VK_BLEND_FACTOR_DST_COLOR;
			case GLS_SRCBLEND_ONE_MINUS_DST_COLOR:	return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
			case GLS_SRCBLEND_SRC_ALPHA:			return VK_BLEND_FACTOR_SRC_ALPHA;
			case GLS_SRCBLEND_ONE_MINUS_SRC_ALPHA:	return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
			case GLS_SRCBLEND_DST_ALPHA:			return VK_BLEND_FACTOR_DST_ALPHA;
			case GLS_SRCBLEND_ONE_MINUS_DST_ALPHA:	return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
			case GLS_SRCBLEND_ALPHA_SATURATE:		return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
			default:								return VK_BLEND_FACTOR_ONE;
			}
		}
		switch ( b ) {
		case GLS_DSTBLEND_ZERO:					return VK_BLEND_FACTOR_ZERO;
		case GLS_DSTBLEND_ONE:					return VK_BLEND_FACTOR_ONE;
		case GLS_DSTBLEND_SRC_COLOR:			return VK_BLEND_FACTOR_SRC_COLOR;
		case GLS_DSTBLEND_ONE_MINUS_SRC_COLOR:	return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
		case GLS_DSTBLEND_SRC_ALPHA:			return VK_BLEND_FACTOR_SRC_ALPHA;
		case GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA:	return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		case GLS_DSTBLEND_DST_ALPHA:			return VK_BLEND_FACTOR_DST_ALPHA;
		case GLS_DSTBLEND_ONE_MINUS_DST_ALPHA:	return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
		default:								return VK_BLEND_FACTOR_ONE;
		}
	};

	VkPipelineColorBlendAttachmentState att = {};
	const unsigned int srcBits = bits & GLS_SRCBLEND_BITS;
	const unsigned int dstBits = bits & GLS_DSTBLEND_BITS;
	att.blendEnable = !( srcBits == GLS_SRCBLEND_ONE && dstBits == GLS_DSTBLEND_ZERO );
	att.srcColorBlendFactor = att.srcAlphaBlendFactor = mapBlend( srcBits, true );
	att.dstColorBlendFactor = att.dstAlphaBlendFactor = mapBlend( dstBits, false );
	att.colorBlendOp = att.alphaBlendOp = VK_BLEND_OP_ADD;
	att.colorWriteMask =
		( ( bits & GLS_REDMASK )   ? 0 : VK_COLOR_COMPONENT_R_BIT )
	  | ( ( bits & GLS_GREENMASK ) ? 0 : VK_COLOR_COMPONENT_G_BIT )
	  | ( ( bits & GLS_BLUEMASK )  ? 0 : VK_COLOR_COMPONENT_B_BIT )
	  | ( ( bits & GLS_ALPHAMASK ) ? 0 : VK_COLOR_COMPONENT_A_BIT );

	VkPipelineColorBlendStateCreateInfo cb = {};
	cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	// blend-attachment count must match the active subpass: 0 for the depth-only
	// shadow pass, 1 for the scene / HDR / color targets, 2 for an MRT target. The
	// MRT attachments share one blend config (the SSR material buffer isn't blended).
	VkPipelineColorBlendAttachmentState attArr[2] = { att, att };
	cb.attachmentCount = (uint32_t)curColorAtt;
	cb.pAttachments = ( curColorAtt > 0 ) ? attArr : NULL;

	const VkDynamicState dyn[3] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
	                                VK_DYNAMIC_STATE_DEPTH_BIAS };
	VkPipelineDynamicStateCreateInfo dsi = {};
	dsi.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dsi.dynamicStateCount = 3;
	dsi.pDynamicStates = dyn;

	VkGraphicsPipelineCreateInfo pci = {};
	pci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pci.stageCount = 2;
	pci.pStages = stages;
	pci.pVertexInputState = &vin;
	pci.pInputAssemblyState = &ia;
	pci.pViewportState = &vp;
	pci.pRasterizationState = &rs;
	pci.pMultisampleState = &ms;
	pci.pDepthStencilState = &ds;
	pci.pColorBlendState = &cb;
	pci.pDynamicState = &dsi;
	pci.layout = pipeLayout;
	// curPipelinePass is the render pass of the active destination (scene passClear,
	// the HDR buffer's colorClearPass, a shadow/AA nested pass, ...). It's compatible
	// with that destination's load/clearDS variants (same formats), and curPassClass
	// in the cache key keeps render-pass-incompatible destinations on separate keys.
	pci.renderPass = curPipelinePass ? curPipelinePass : passClear;
	pci.subpass = 0;

	VkPipeline pipeline = VK_NULL_HANDLE;
	if ( !vkCheck( vkCreateGraphicsPipelines( device, VK_NULL_HANDLE, 1, &pci, NULL, &pipeline ),
	               va( "vkCreateGraphicsPipelines(%s)", sh.name.c_str() ) ) ) {
		pipeline = VK_NULL_HANDLE;
	}
	pipelineCache[key] = pipeline;
	return pipeline;
}

/*
====================
VulkanBackend::EnsureScenePass

2D draws arrive without an explicit BeginPass (the GL executor draws straight
onto the current framebuffer); Vulkan draws must sit inside a render pass.
====================
*/
void VulkanBackend::EnsureScenePass() {
	if ( !insideScenePass && frameOpen && !skipFrame ) {
		BeginPass( NULL );
	}
}

/*
====================
VulkanBackend::SetViewport / SetScissor

GL-convention rects (origin bottom-left); converted to Vulkan's top-left
origin here. The viewport uses the negative-height trick (core in 1.1) so
NDC stays y-up and the matrices remain GL-identical.
====================
*/
void VulkanBackend::SetViewport( int x, int y, int w, int h ) {
	vpRect[0] = x; vpRect[1] = y; vpRect[2] = w; vpRect[3] = h;
	dynStateDirty = true;
}

void VulkanBackend::SetScissor( int x, int y, int w, int h ) {
	scRect[0] = x; scRect[1] = y; scRect[2] = w; scRect[3] = h;
	dynStateDirty = true;
}

void VulkanBackend::SetDepthRange( float minDepth, float maxDepth ) {
	depthRangeMin = minDepth;
	depthRangeMax = maxDepth;
	dynStateDirty = true;
}

// GL polygon offset -> dynamic depth bias: units maps to the constant factor
// (minimum-resolvable-depth steps, same definition as GL), factor to the
// slope factor. Every pipeline enables depth bias; (0, 0) is a no-op.
void VulkanBackend::SetPolygonOffset( bool enable, float factor, float units ) {
	polyOfsFactor = enable ? factor : 0.0f;
	polyOfsUnits = enable ? units : 0.0f;
	dynStateDirty = true;
}

// the per-light stencil clear (GL: scissored qglClear(GL_STENCIL_BUFFER_BIT))
void VulkanBackend::ClearStencilBuffer( int value ) {
	if ( !frameOpen || skipFrame ) {
		return;
	}
	EnsureScenePass();
	if ( !insideScenePass ) {
		return;
	}
	VkClearAttachment att = {};
	att.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
	att.clearValue.depthStencil.stencil = (uint32_t)value;
	// GL's clear respects the scissor rect; replicate with the current scissor
	// (converted from GL's bottom-left origin, clamped to the framebuffer)
	VkClearRect rect = {};
	int sx = scRect[0], sy = scRect[1], sw = scRect[2], sh = scRect[3];
	if ( sw < 0 ) { sw = 0; }
	if ( sh < 0 ) { sh = 0; }
	int top = (int)sceneExtent.height - ( sy + sh );
	if ( sx < 0 ) { sw += sx; sx = 0; }
	if ( top < 0 ) { sh += top; top = 0; }
	if ( sx + sw > (int)sceneExtent.width )  { sw = (int)sceneExtent.width - sx; }
	if ( top + sh > (int)sceneExtent.height ) { sh = (int)sceneExtent.height - top; }
	if ( sw <= 0 || sh <= 0 ) {
		return;
	}
	rect.rect.offset = { sx, top };
	rect.rect.extent = { (uint32_t)sw, (uint32_t)sh };
	rect.layerCount = 1;
	vkCmdClearAttachments( frames[frameIndex].cb, 1, &att, 1, &rect );
}

/*
====================
VulkanBackend::BindPipeline
====================
*/
void VulkanBackend::BindPipeline( const PipelineDesc &desc ) {
	currentDesc = desc;
}

/*
====================
VulkanBackend::Draw
====================
*/
void VulkanBackend::Draw( const DrawArgs &args ) {
	if ( !frameOpen || skipFrame || args.indexCount <= 0 ) {
		return;
	}
	VkPipeline pipeline = GetPipeline( currentDesc );
	if ( pipeline == VK_NULL_HANDLE ) {
		return;		// missing shader — degrade by not drawing
	}
	VkBuffer vb = LookupBuffer( args.vertexBuffer );
	VkBuffer ib = LookupBuffer( args.indexBuffer );
	if ( vb == VK_NULL_HANDLE || ib == VK_NULL_HANDLE ) {
		return;
	}

	if ( insideTargetPass ) {
		// rendering into an offscreen shadow-map target: its render pass is
		// already open (BeginTargetPass/BeginCubeFacePass); don't touch the scene pass
	} else {
		EnsureScenePass();
		if ( !insideScenePass ) {
			return;
		}
	}
	VkCommandBuffer cb = frames[frameIndex].cb;

	// A fullscreen pass sampling a color render target (HDR resolve/FXAA/SMAA) must
	// cancel the scene's negative-height flip: those targets are stored top-down,
	// so the shared GL fullscreen quad would otherwise sample them upside down. Only
	// post passes ever sample a color target as unit 0, so this is precise.
	bool effFlipY = curFlipY;
	if ( curFlipY && args.textures[0] >= 1 && args.textures[0] <= (ImageHandle)imageTable.size()
	     && imageTable[args.textures[0] - 1].live && imageTable[args.textures[0] - 1].isColorTarget ) {
		effFlipY = false;
	}
	if ( effFlipY != lastEffFlipY ) {
		lastEffFlipY = effFlipY;
		dynStateDirty = true;
	}

	if ( dynStateDirty ) {
		// scene pass: negative-height viewport (y-up NDC like GL) with the GL
		// bottom-left rect converted to Vulkan's top-left. Offscreen target
		// pass: a plain top-left viewport at the target's height — the shadow
		// map's projective write then reads back self-consistently (matches GL).
		const int   renderH = curRenderH;
		VkViewport v = {};
		v.x = (float)vpRect[0];
		v.width = (float)vpRect[2];
		if ( effFlipY ) {
			v.y = (float)renderH - (float)vpRect[1];
			v.height = -(float)vpRect[3];
		} else {
			v.y = (float)vpRect[1];
			v.height = (float)vpRect[3];
		}
		// shadow-map passes always want the full [0,1] depth range: the weapon/
		// model depth hack (SetDepthRange) is a scene-only concern
		v.minDepth = insideTargetPass ? 0.0f : depthRangeMin;
		v.maxDepth = insideTargetPass ? 1.0f : depthRangeMax;
		vkCmdSetViewport( cb, 0, 1, &v );

		VkRect2D sc = {};
		int sx = scRect[0], sy = scRect[1], sw = scRect[2], sh = scRect[3];
		if ( sw < 0 ) { sw = 0; }
		if ( sh < 0 ) { sh = 0; }
		int top = effFlipY ? ( renderH - ( sy + sh ) ) : sy;
		if ( sx < 0 ) { sw += sx; sx = 0; }
		if ( top < 0 ) { sh += top; top = 0; }
		if ( sw < 0 ) { sw = 0; }
		if ( sh < 0 ) { sh = 0; }
		sc.offset = { sx, top };
		sc.extent = { (uint32_t)sw, (uint32_t)sh };
		vkCmdSetScissor( cb, 0, 1, &sc );

		vkCmdSetDepthBias( cb, polyOfsUnits, 0.0f, polyOfsFactor );
		dynStateDirty = false;
	}

	if ( pipeline != boundPipeline ) {
		vkCmdBindPipeline( cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline );
		boundPipeline = pipeline;
	}

	// set 0: the slot's dynamic-offset UBO
	uint32_t dynOfs = (uint32_t)args.uniformOffset;
	vkCmdBindDescriptorSets( cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeLayout,
		0, 1, &uboSet[frameIndex], 1, &dynOfs );

	// set 1: per-draw texture set from the frame pool (dummy in empty slots)
	{
		VkDescriptorSet texSet = VK_NULL_HANDLE;
		uint64_t key = 0xcbf29ce484222325ull;
		auto mixKey = []( uint64_t &seed, uint32_t value ) {
			seed ^= (uint64_t)value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
		};
		for ( int i = 0; i < 8; i++ ) {
			mixKey( key, (uint32_t)args.textures[i] );
		}
		mixKey( key, (uint32_t)args.shadowCube );
		bool needsTexBind = true;
		if ( boundTexSet != VK_NULL_HANDLE && boundTexKey == key ) {
			texSet = boundTexSet;
			needsTexBind = false;
		} else {
			auto it = textureSetCache.find( key );
			if ( it != textureSetCache.end() ) {
				texSet = it->second;
			} else {
				VkDescriptorSetAllocateInfo ai = {};
				ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
				ai.descriptorPool = texturePool ? texturePool : framePool[frameIndex];
				ai.descriptorSetCount = 1;
				ai.pSetLayouts = &setLayoutTex;
				if ( vkAllocateDescriptorSets( device, &ai, &texSet ) != VK_SUCCESS ) {
					if ( !framePoolWarned ) {
						framePoolWarned = true;
						common->Warning( "VK: descriptor pool exhausted while allocating texture set" );
					}
					return;
				}
				// bindings 0-7 = units, 8 = shadow cube, 9 = SSAO, 10 = occlusion map.
				// Empty slots take a dummy typed for what the shaders statically
				// declare: unit 7 is interaction.frag's sampler2DShadow and 8 its
				// samplerCubeShadow (depth-compare dummies until M7 shadow maps);
				// everything else is sampler2D (white). Real handles always win.
				VkDescriptorImageInfo infos[11];
				VkWriteDescriptorSet writes[11];
				const ImageRec &dummy = imageTable[dummyImage - 1];
				for ( int i = 0; i < 11; i++ ) {
					VkSampler sampler = dummy.sampler;
					VkImageView view = dummy.view;
					if ( i < 8 && args.textures[i] >= 1 && args.textures[i] <= (ImageHandle)imageTable.size()
					     && imageTable[args.textures[i] - 1].live ) {
						const ImageRec &rec = imageTable[args.textures[i] - 1];
						sampler = rec.sampler;
						view = rec.view;
					} else if ( i == 7 ) {
						sampler = dummyShadow2D.sampler;
						view = dummyShadow2D.view;
					} else if ( i == 8 ) {
						if ( args.shadowCube >= 1 && args.shadowCube <= (ImageHandle)imageTable.size()
						     && imageTable[args.shadowCube - 1].live ) {
							const ImageRec &rec = imageTable[args.shadowCube - 1];
							sampler = rec.sampler;
							view = rec.view;
						} else {
							sampler = dummyShadowCube.sampler;
							view = dummyShadowCube.view;
						}
					}
					infos[i] = {};
					infos[i].sampler = sampler;
					infos[i].imageView = view;
					infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
					writes[i] = {};
					writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
					writes[i].dstSet = texSet;
					writes[i].dstBinding = (uint32_t)i;
					writes[i].descriptorCount = 1;
					writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
					writes[i].pImageInfo = &infos[i];
				}
				vkUpdateDescriptorSets( device, 11, writes, 0, NULL );
				textureSetCache[key] = texSet;
			}
			boundTexSet = texSet;
			boundTexKey = key;
		}
		if ( needsTexBind ) {
			vkCmdBindDescriptorSets( cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeLayout,
				1, 1, &texSet, 0, NULL );
		}
	}

	VkDeviceSize vbOfs = (VkDeviceSize)args.vertexOffset;
	vkCmdBindVertexBuffers( cb, 0, 1, &vb, &vbOfs );
	vkCmdBindIndexBuffer( cb, ib, 0, VK_INDEX_TYPE_UINT32 );
	vkCmdDrawIndexed( cb, (uint32_t)args.indexCount, 1, (uint32_t)args.firstIndex, 0, 0 );
}

/*
====================
VulkanBackend::DrawImmediate

M6 debug drawing (idImmediateMode → the generic program). A non-indexed
batch of imVert_t (24 B) as `primMode`, streamed through the vertex + UBO
rings. Fixed pipeline state: alpha blend, depth-test LEQUAL, no depth write —
debug lines/polygons/portals read over the scene. Unlike GL (which inherits
the caller's GL_State), the state is baked here, so "depth test off" debug
lines are still depth-tested; acceptable for the dev overlays.
====================
*/
void VulkanBackend::DrawImmediate( const void *verts, int numVerts, unsigned int primMode,
                                   const float mvp[16], bool textured ) {
	if ( device == VK_NULL_HANDLE || !frameOpen || skipFrame || numVerts <= 0 || verts == NULL ) {
		return;
	}

	BufferHandle vbh = 0;
	int vertOfs = AllocFromRing( vertRing[frameIndex], verts, numVerts * 24, 4, 0, &vbh );
	VkBuffer vb = LookupBuffer( vbh );
	if ( vb == VK_NULL_HANDLE ) {
		return;
	}

	// generic-program uniforms: identity texture matrix, straight per-vertex
	// color (modulate 1 / add 0 / color 1), alpha test off — matches the GL3 path
	RenderParams p;
	memset( &p, 0, sizeof( p ) );
	memcpy( p.mvpMatrix, mvp, sizeof( p.mvpMatrix ) );
	p.diffuseMatrixS[0] = 1.0f;
	p.diffuseMatrixT[1] = 1.0f;
	p.vertexColorModulate[0] = p.vertexColorModulate[1] = p.vertexColorModulate[2] = p.vertexColorModulate[3] = 1.0f;
	p.color[0] = p.color[1] = p.color[2] = p.color[3] = 1.0f;
	BufferHandle ubh = 0;
	int uniOfs = AllocFromRing( uboRing[frameIndex], &p, sizeof( p ), uboAlign, MAX_UNIFORM_SLICE, &ubh );

	PipelineDesc pd;
	pd.stateBits = GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA | GLS_DEPTHMASK;
	pd.shader = LoadShader( "generic" );
	pd.vertexLayout = VL_IMMEDIATE;
	pd.cullType = CT_TWO_SIDED;
	pd.topology = (int)primMode;
	VkPipeline pipeline = GetPipeline( pd );
	if ( pipeline == VK_NULL_HANDLE ) {
		return;
	}
	// untextured lines/points/tris: the generic program samples unit 0 and
	// multiplies by vertex color, so the white dummy gives pure vertex color.
	// textured debug immediate draws aren't plumbed (no bound-image state on
	// VK) — they also fall back to the white texel.
	if ( textured ) {
		static bool warnedTex = false;
		if ( !warnedTex ) {
			warnedTex = true;
			common->Printf( "VK: textured DrawImmediate falls back to white (no bound-image state)\n" );
		}
	}

	EnsureScenePass();
	if ( !insideScenePass ) {
		return;
	}
	VkCommandBuffer cb = frames[frameIndex].cb;

	if ( dynStateDirty ) {
		const float fbH = (float)sceneExtent.height;
		VkViewport v = {};
		v.x = (float)vpRect[0];
		v.y = fbH - (float)vpRect[1];
		v.width = (float)vpRect[2];
		v.height = -(float)vpRect[3];
		v.minDepth = depthRangeMin;
		v.maxDepth = depthRangeMax;
		vkCmdSetViewport( cb, 0, 1, &v );
		VkRect2D sc = {};
		int sx = scRect[0], sy = scRect[1], sw = scRect[2], sh = scRect[3];
		int top = (int)sceneExtent.height - ( sy + sh );
		if ( sx < 0 ) { sw += sx; sx = 0; }
		if ( top < 0 ) { sh += top; top = 0; }
		if ( sw < 0 ) { sw = 0; }
		if ( sh < 0 ) { sh = 0; }
		sc.offset = { sx, top };
		sc.extent = { (uint32_t)sw, (uint32_t)sh };
		vkCmdSetScissor( cb, 0, 1, &sc );
		vkCmdSetDepthBias( cb, polyOfsUnits, 0.0f, polyOfsFactor );
		dynStateDirty = false;
	}

	vkCmdBindPipeline( cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline );
	boundPipeline = VK_NULL_HANDLE;		// bypassed the normal Draw path; force a rebind next

	uint32_t dynOfs = (uint32_t)uniOfs;
	vkCmdBindDescriptorSets( cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeLayout, 0, 1, &uboSet[frameIndex], 1, &dynOfs );

	// set 1: all-dummy texture set (white on every unit) — debug draws are
	// untextured; a white texel yields pure vertex color through generic.frag
	{
		VkDescriptorSet texSet = VK_NULL_HANDLE;
		const uint64_t key = 0;
		auto it = textureSetCache.find( key );
		if ( it != textureSetCache.end() ) {
			texSet = it->second;
		} else {
			VkDescriptorSetAllocateInfo ai = {};
			ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
			ai.descriptorPool = texturePool ? texturePool : framePool[frameIndex];
			ai.descriptorSetCount = 1;
			ai.pSetLayouts = &setLayoutTex;
			if ( vkAllocateDescriptorSets( device, &ai, &texSet ) != VK_SUCCESS ) {
				return;
			}
			VkDescriptorImageInfo infos[11];
			VkWriteDescriptorSet writes[11];
			const ImageRec &dummy = imageTable[dummyImage - 1];
			for ( int i = 0; i < 11; i++ ) {
				infos[i] = {};
				infos[i].sampler = ( i == 7 ) ? dummyShadow2D.sampler : ( i == 8 ? dummyShadowCube.sampler : dummy.sampler );
				infos[i].imageView = ( i == 7 ) ? dummyShadow2D.view : ( i == 8 ? dummyShadowCube.view : dummy.view );
				infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				writes[i] = {};
				writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
				writes[i].dstSet = texSet;
				writes[i].dstBinding = (uint32_t)i;
				writes[i].descriptorCount = 1;
				writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
				writes[i].pImageInfo = &infos[i];
			}
			vkUpdateDescriptorSets( device, 11, writes, 0, NULL );
			textureSetCache[key] = texSet;
		}
		vkCmdBindDescriptorSets( cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeLayout, 1, 1, &texSet, 0, NULL );
	}

	VkDeviceSize vbOfs = (VkDeviceSize)vertOfs;
	vkCmdBindVertexBuffers( cb, 0, 1, &vb, &vbOfs );
	vkCmdDraw( cb, (uint32_t)numVerts, 1, 0, 0 );
}

/*
====================
M6: ImGui on Vulkan — swapchain targets + init/shutdown + glue

The pass loads the blitted frame (initialLayout TRANSFER_DST) and hands the
image to present (finalLayout PRESENT_SRC); EndFrame begins it only when
draw data is pending. The pass object survives swapchain recreation (only
views/framebuffers rebuild); a surface-format change recreates it and asks
the ImGui backend for a new pipeline.
====================
*/
bool VulkanBackend::CreateImGuiTargets() {
#ifndef IMGUI_DISABLE
	if ( device == VK_NULL_HANDLE || swapImages.empty() || swapFormat == VK_FORMAT_UNDEFINED ) {
		return false;
	}

	const bool formatChanged = imguiPass != VK_NULL_HANDLE && imguiPassFormat != swapFormat;
	if ( formatChanged ) {
		vkDestroyRenderPass( device, imguiPass, NULL );
		imguiPass = VK_NULL_HANDLE;
	}
	if ( imguiPass == VK_NULL_HANDLE ) {
		VkAttachmentDescription att = {};
		att.format = swapFormat;
		att.samples = VK_SAMPLE_COUNT_1_BIT;
		att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		att.initialLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;	// after the scene blit
		att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

		VkAttachmentReference colorRef = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
		VkSubpassDescription sub = {};
		sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		sub.colorAttachmentCount = 1;
		sub.pColorAttachments = &colorRef;

		VkSubpassDependency dep = {};
		dep.srcSubpass = VK_SUBPASS_EXTERNAL;
		dep.dstSubpass = 0;
		dep.srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
		dep.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

		VkRenderPassCreateInfo rpi = {};
		rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
		rpi.attachmentCount = 1;
		rpi.pAttachments = &att;
		rpi.subpassCount = 1;
		rpi.pSubpasses = &sub;
		rpi.dependencyCount = 1;
		rpi.pDependencies = &dep;
		if ( !vkCheck( vkCreateRenderPass( device, &rpi, NULL, &imguiPass ), "vkCreateRenderPass(imgui)" ) ) {
			return false;
		}
		imguiPassFormat = swapFormat;

		if ( formatChanged && imguiUp ) {
			ImGui_ImplVulkan_PipelineInfo pi = {};
			pi.RenderPass = imguiPass;
			ImGui_ImplVulkan_CreateMainPipeline( &pi );
		}
	}

	DestroyImGuiTargets();
	imguiViews.resize( swapImages.size(), VK_NULL_HANDLE );
	imguiFbs.resize( swapImages.size(), VK_NULL_HANDLE );
	for ( size_t i = 0; i < swapImages.size(); i++ ) {
		VkImageViewCreateInfo vi = {};
		vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		vi.image = swapImages[i];
		vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vi.format = swapFormat;
		vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		vi.subresourceRange.levelCount = 1;
		vi.subresourceRange.layerCount = 1;
		if ( !vkCheck( vkCreateImageView( device, &vi, NULL, &imguiViews[i] ), "vkCreateImageView(imgui)" ) ) {
			return false;
		}
		VkFramebufferCreateInfo fbi = {};
		fbi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		fbi.renderPass = imguiPass;
		fbi.attachmentCount = 1;
		fbi.pAttachments = &imguiViews[i];
		fbi.width = swapExtent.width;
		fbi.height = swapExtent.height;
		fbi.layers = 1;
		if ( !vkCheck( vkCreateFramebuffer( device, &fbi, NULL, &imguiFbs[i] ), "vkCreateFramebuffer(imgui)" ) ) {
			return false;
		}
	}
	return true;
#else
	return false;
#endif
}

void VulkanBackend::DestroyImGuiTargets() {
	for ( size_t i = 0; i < imguiFbs.size(); i++ ) {
		if ( imguiFbs[i] ) { vkDestroyFramebuffer( device, imguiFbs[i], NULL ); }
	}
	imguiFbs.clear();
	for ( size_t i = 0; i < imguiViews.size(); i++ ) {
		if ( imguiViews[i] ) { vkDestroyImageView( device, imguiViews[i], NULL ); }
	}
	imguiViews.clear();
}

bool VulkanBackend::ImGuiInit() {
#ifndef IMGUI_DISABLE
	if ( device == VK_NULL_HANDLE ) {
		return false;
	}
	if ( imguiUp ) {
		return true;
	}
	if ( !CreateImGuiTargets() ) {
		common->Warning( "VK ImGui: couldn't create the swapchain render targets" );
		return false;
	}

	ImGui_ImplVulkan_InitInfo ii = {};
	ii.ApiVersion = VK_API_VERSION_1_4;
	ii.Instance = instance;
	ii.PhysicalDevice = physical;
	ii.Device = device;
	ii.QueueFamily = gfxFamily;
	ii.Queue = gfxQueue;
	ii.DescriptorPoolSize = 2 * IMGUI_IMPL_VULKAN_MINIMUM_IMAGE_SAMPLER_POOL_SIZE;
	ii.MinImageCount = 2;
	ii.ImageCount = (uint32_t)swapImages.size();
	ii.PipelineInfoMain.RenderPass = imguiPass;
	if ( !ImGui_ImplVulkan_Init( &ii ) ) {
		common->Warning( "VK ImGui: ImGui_ImplVulkan_Init failed" );
		return false;
	}
	imguiUp = true;
	return true;
#else
	return false;
#endif
}

void VulkanBackend::ImGuiShutdown() {
#ifndef IMGUI_DISABLE
	if ( !imguiUp ) {
		return;
	}
	if ( device != VK_NULL_HANDLE ) {
		vkDeviceWaitIdle( device );
	}
	ImGui_ImplVulkan_Shutdown();
	imguiUp = false;
	imguiDrawData = NULL;
#endif
}

// ---- VulkanImGui.h glue (called from sys_imgui.cpp / the executor) ----
bool VK_ImGuiInit() {
	return vkBackend.ImGuiInit();
}
void VK_ImGuiShutdown() {
	vkBackend.ImGuiShutdown();
}
void VK_ImGuiNewFrame() {
#ifndef IMGUI_DISABLE
	if ( vkBackend.ImGuiUp() ) {
		ImGui_ImplVulkan_NewFrame();
	}
#endif
}
void VK_ImGuiSetDrawData( void *imDrawData ) {
	vkBackend.ImGuiSetDrawData( imDrawData );
}

} // namespace rhi

#endif // DHEWM3_VULKAN
