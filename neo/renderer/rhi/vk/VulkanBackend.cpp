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

#ifdef DUDE_HAVE_SHADERC
// runtime GLSL->SPIR-V for custom (mod) ARB material stages (CreateShaderFromGlsl)
#include <shaderc/shaderc.h>
#endif

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
#include <mutex>
#include <ctime>

#include <unordered_map>

#include "framework/FileSystem.h"
#include "renderer/tr_local.h"
#include "renderer/rhi/RHI.h"
#include "renderer/rhi/RenderParams.h"	// M6: DrawImmediate fills the generic UBO
#include "renderer/rhi/MaterialIR.h"		// IR_Purge on shader-cache lifecycle
#include "renderer/rhi/vk/VulkanImGui.h"	// M6: ImGui glue (impl at the end of this TU)
#include "ffx_fsr2.h"						// vendored FidelityFX FSR2 (docs/fsr-temporal-pipeline.md, R1)
#include "vk/ffx_fsr2_vk.h"					// FSR2 Vulkan backend init (against our VkDevice)
#include "framework/CmdSystem.h"              // cmdSystem for registering console commands
#include "sys/sys_public.h"                     // Sys_DLL_Load (portable NVML load), Sys_Milliseconds

// auto-reactive mask flags (R1/D): CPU/shader ABI values from the vendored
// shaders/ffx_fsr2_resources.h (a GLSL-shared header the C API doesn't re-export)
#ifndef FFX_FSR2_AUTOREACTIVEFLAGS_APPLY_TONEMAP
#define FFX_FSR2_AUTOREACTIVEFLAGS_APPLY_TONEMAP		1
#define FFX_FSR2_AUTOREACTIVEFLAGS_APPLY_INVERSETONEMAP	2
#define FFX_FSR2_AUTOREACTIVEFLAGS_APPLY_THRESHOLD		4
#define FFX_FSR2_AUTOREACTIVEFLAGS_USE_COMPONENTS_MAX	8
#endif

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

static idCVar r_vkGpuTime( "r_vkGpuTime", "0", CVAR_RENDERER | CVAR_BOOL,
	"Vulkan backend: print GPU frame time (ms), averaged once per second" );

// Enable writing on-disk crash logs when a fatal Vulkan event (eg. VK_ERROR_DEVICE_LOST)
// is observed. Default OFF; opt-in dev tool that writes a dudelog-crash-YYYYMMDD-HHMMSS.txt
// file into the save path when a device-lost occurs.
static idCVar r_vkCrashLogging( "r_vkCrashLogging", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL,
	"Vulkan: enable on-disk crash logging when device lost occurs (dev tool)" );

// Deep-dive debug HUD overlay: GPU clock/temp/power (NVML), VRAM (VMA), validation errors +
// recent messages. Archived so it survives a vid_restart while hunting a device-loss.
static idCVar r_vkDebugHud( "r_vkDebugHud", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL,
	"Vulkan: deep-dive debug HUD overlay (GPU clock/temp/power, VRAM, validation errors)" );

static idCVar r_vkComputeTest( "r_vkComputeTest", "0", CVAR_RENDERER | CVAR_BOOL,
	"Vulkan backend: run the compute-lane self-test (dispatch a kernel doubling a storage buffer, read back, verify) and print PASS/FAIL. Set to 1 to trigger (docs/gpu-offload-plan.md Phase 1)" );
static idCVar r_fsr2Test( "r_fsr2Test", "0", CVAR_RENDERER | CVAR_BOOL,
	"Vulkan backend: FSR2 bring-up self-test - size the scratch, build the FSR2 VK interface, create a Native-AA FSR2 context (no dispatch), destroy it, and print PASS/FAIL. Set to 1 to trigger (docs/fsr-temporal-pipeline.md, R1/C0)" );
static idCVar r_mrt3Test( "r_mrt3Test", "0", CVAR_RENDERER | CVAR_BOOL,
	"Vulkan backend: MRT plumbing self-test - create + destroy a 3-attachment velocity gbuffer (RGBA8 normal + RGBA8 SSR + RG16F velocity + depth, a distinct pass class) and print PASS/FAIL. Proves RG16F color attachments work on this driver. Set to 1 to trigger (docs/fsr-temporal-pipeline.md, R1/A0)" );

static idCVar r_vkIndirectTest( "r_vkIndirectTest", "0", CVAR_RENDERER | CVAR_BOOL,
	"Vulkan backend: route every indexed draw through the DrawIndexedIndirect primitive (a per-draw 1-command indirect ring) instead of vkCmdDrawIndexed. A pixel-identical A/B validating the Phase-3 indirect-draw seed (docs/gpu-offload-plan.md)" );

static idCVar r_vkBdaTest( "r_vkBdaTest", "0", CVAR_RENDERER | CVAR_BOOL,
	"Vulkan backend: run the buffer-device-address self-test (a compute kernel sums a storage buffer read through its raw GPU pointer, not a bound buffer) and print PASS/FAIL. Set to 1 to trigger. Validates the Phase-3.2b BDA primitive (docs/gpu-offload-plan.md)" );

static idCVar r_rayQueryTest( "r_rayQueryTest", "0", CVAR_RENDERER | CVAR_BOOL,
	"Vulkan backend: run the ray-query self-test - build a synthetic BLAS/TLAS (two known triangles), trace a 16x16 ray grid from a compute shader via GL_EXT_ray_query, and diff every hit's t + primitive index against a CPU reference. Prints PASS/FAIL. Set to 1 to trigger. Validates the R2 acceleration-structure foundation (docs/rtx-shadow-roadmap.md)" );

static idCVar r_vkBdaZfill( "r_vkBdaZfill", "0", CVAR_RENDERER | CVAR_INTEGER,
	"Vulkan backend: route the world-static depth prepass (zfill) through buffer-device-address geometry fetch. 0 = off (bound attributes); 1 = per-draw BDA vertex fetch; 2 = batched indirect (one vkCmdDrawIndirect over the solid-opaque bucket, indices+verts via BDA). Pixel-identical A/B; the Phase 3.2b consume (docs/gpu-offload-plan.md). Addressable persistent geometry only; animated/streamed/tessellated/perforated surfaces fall back." );

static idCVar r_vkBdaVerbose( "r_vkBdaVerbose", "0", CVAR_RENDERER | CVAR_BOOL,
	"Vulkan backend: print the r_vkBdaZfill firing counter once/sec (per-draw / batched / fell-back) to confirm the BDA z-fill path is live. Diagnostic only, off by default so enabling the offload doesn't chatter to the console." );

namespace rhi {

// Forward declarations for the crash-dump helpers implemented later in this TU.
static void WriteVulkanCrashDump( const char *what, VkResult res );
static void WriteVulkanCrashDumpForced( const char *what, VkResult res );
static void VkSampleTelemetry();		// advance the NVML telemetry ring (impl near the debug HUD)

// Console command: force a CPU-only crash dump to disk. Useful to manually capture engine
// state without waiting for an actual device-lost. Usage: vk_dump_state [label]
static void Vk_ForceCrashDump_f( const idCmdArgs &args ) {
	const char *label = ( args.Argc() > 1 ) ? args.Argv(1) : "manual";
	char what[256];
	idStr::snPrintf( what, sizeof(what), "Console-forced dump: %s", label );
	WriteVulkanCrashDumpForced( what, VK_SUCCESS );
}


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
	// persistent static vertex/index buffers: host-visible + mapped, so the
	// vertexCache uploads level/model geometry once instead of re-streaming every
	// surface through the ring each frame (the old CPU-cache "SLOW" path).
	virtual BufferHandle	CreateBuffer( BufferUsage usage, int size, const void *data );
	virtual void			UpdateBuffer( BufferHandle b, int offset, int size, const void *data );
	virtual void			DestroyBuffer( BufferHandle b );
	virtual bool			ReadBuffer( BufferHandle b, void *dst, int size );
	virtual unsigned long long	GetBufferDeviceAddress( BufferHandle b );
	virtual ImageHandle		CreateImage( ImageFormat, int, int, const void * ) { return 0; }	// render-target era API; M7
	virtual void			DestroyImage( ImageHandle h );
	virtual ImageHandle		CreateTexture2D( int w, int h, const void *pixels,
	                                         int textureFilter, int textureRepeat, bool allowMips );
	virtual ImageHandle		CreateTexture2DPrebuilt( int w, int h, const PrebuiltMip *levels,
	                                                 int numLevels, int textureFilter,
	                                                 int textureRepeat );
	ImageHandle				UploadTexture2DLevels( int w, int h, const PrebuiltMip *levels,
	                                               int numLevels, int textureFilter,
	                                               int textureRepeat );	// shared by the two above
	virtual ImageHandle		CreateTextureCube( int size, const void * const pics[6],
	                                           int textureFilter, bool allowMips );
	virtual ShaderHandle	LoadShader( const char *name );
	virtual ShaderHandle	CreateShaderFromGlsl( const char *name, const char *vertSrc, const char *fragSrc );
	virtual ShaderHandle	CreateComputeShader( const char *name, const char *glslSrc );

	// M7 render-target family. Live: depth targets (2D + cube shadow maps),
	// color-only + color+depth-stencil targets (the HDR RGBA16F scene buffer and
	// its FXAA ping). Still stubbed: color+depth for the SSAO normal G-buffer /
	// SSR material buffer (return 0 → those features stay off until the SSAO slice).
	virtual RenderTargetHandle	CreateRenderTarget( ImageFormat fmt, int w, int h );
	virtual RenderTargetHandle	CreateRenderTargetCube( ImageFormat fmt, int size );
	virtual RenderTargetHandle	CreateRenderTargetColorDepth( ImageFormat fmt, int w, int h, int colorCount );
	virtual RenderTargetHandle	CreateRenderTargetColorDepthStencil( ImageFormat fmt, int w, int h );
	virtual RenderTargetHandle	CreateRenderTargetMipped( ImageFormat fmt, int w, int h, int mipLevels );
	virtual void				BeginTargetMipPass( RenderTargetHandle rt, int mipLevel, const ClearArgs *clear );
	virtual ImageHandle			GetRenderTargetMipImage( RenderTargetHandle rt, int mipLevel );
	virtual void				DestroyRenderTarget( RenderTargetHandle rt );
	virtual void				SetFrameTarget( RenderTargetHandle rt );
	virtual void				BeginCubeFacePass( RenderTargetHandle rt, int face, const ClearArgs *clear );
	virtual RenderTargetHandle	BeginNormalPrepass( int w, int h, const ClearArgs *clear, bool wantMrt );
	virtual ImageHandle			GetRenderTargetImage( RenderTargetHandle rt );
	virtual ImageHandle			GetRenderTargetImage2( RenderTargetHandle rt );
	virtual ImageHandle			GetRenderTargetImage3( RenderTargetHandle rt );

	virtual int		AllocUniforms( const void *data, int size, BufferHandle *buffer );
	virtual int		AllocVertices( const void *data, int size, BufferHandle *buffer );
	virtual int		AllocIndices( const void *data, int size, BufferHandle *buffer );
	virtual int		StreamGeneration() { return streamGen; }

	// ---- drawing ----
	virtual void	BindPipeline( const PipelineDesc &desc );
	virtual void	Draw( const DrawArgs &args );
	virtual void	DrawIndexedIndirect( const DrawArgs &args, BufferHandle argsBuffer, int argsOffset,
	                                     int drawCount, int stride, BufferHandle countBuffer, int countOffset );
	virtual void	DrawZfillBatch( const ZfillBatchItem *items, int count,
	                                const ZfillBatchGroup *groups, int groupCount );
	virtual bool	ZfillBatchEnabled() {
		return r_vkBdaZfill.GetInteger() >= 2 && haveBufferDeviceAddress && zfillBatchShaderHandle != 0
		       && haveDrawIndirectFirstInstance && haveMultiDrawIndirect;
	}
	virtual void	Dispatch( const ComputeArgs &args );
	virtual void	PostComputeBarrier();
	virtual void	DispatchSync( const ComputeArgs &args );
	virtual bool	RunFsr2( const Fsr2DispatchArgs &args );
	virtual void	Fsr2CaptureOpaque( RenderTargetHandle sceneRT );
	virtual ImageHandle	CreateCaptureImage( int w, int h, bool depth, bool hdrFloat );
	virtual void	CopyFramebufferToImage( ImageHandle dst, int dstX, int dstY,
	                                        int srcX, int srcY, int w, int h, bool depth );
	virtual void	RetireImage( ImageHandle img );
	virtual void	UpdateTexture2D( ImageHandle dst, int w, int h, const void *pixels );
	virtual bool	ReadPixelsRGB( unsigned char *dest, int x, int y, int w, int h );
	virtual void	DrawImmediate( const void *verts, int numVerts, unsigned int primMode,
	                               const float mvp[16], bool textured );

private:
	// compute lane (docs/gpu-offload-plan.md Phase 1)
	VkPipeline		GetComputePipeline( ShaderHandle shader );	// build/cache a VkPipeline for a compute shader
	VkDescriptorSet	RecordDispatch( VkCommandBuffer cb, const ComputeArgs &args );	// bind+dispatch; returns the set to reclaim
	void			ComputeSelfTest();							// r_vkComputeTest: dispatch a trivial kernel, read back, verify
	void			BdaSelfTest();								// r_vkBdaTest: sum a buffer through its device-address pointer (Phase 3.2b BDA primitive)
	void			Fsr2SelfTest();								// r_fsr2Test: init the vendored FSR2 VK backend + create/destroy a context
	void			Mrt3SelfTest();								// r_mrt3Test: create/destroy a 3-MRT RG16F velocity gbuffer target
	void			RayQuerySelfTest();							// r_rayQueryTest: BLAS/TLAS build + compute ray trace vs CPU reference (R2 foundation)

	// ---- ray-query acceleration structures (R2, docs/rtx-shadow-roadmap.md) ----
	bool			SupportsRayQuery() override;
	BlasHandle		CreateBlas( const float *positions, int numVerts, int posStride,
	                            const int *indexes, int numIndexes ) override;
	BlasHandle		CreateBlasFromBuffers( const BlasGeometry *geoms, int count, bool allowUpdate ) override;
	void			RefitBlas( BlasHandle blas, const BlasGeometry *geoms, int count ) override;
	void			DestroyBlas( BlasHandle blas ) override;
	unsigned long long	BuildTlas( const RtInstance *instances, int count ) override;
	unsigned long long	BuildStandaloneTlas( const RtInstance *instances, int count ) override;
	void			DestroyStandaloneTlas() override;
	unsigned long long	GetTlasAddress() override;
	unsigned long long	GetStaticTlasAddress() override;
	void			UpdateTlas( const RtInstance *instances, int count ) override;
	void			UpdateDynamicGeometry( const float *worldPositions, int numVerts,
	                                       const int *indexes, int numIndexes ) override;
	void			DestroyRtScene() override;
	// FSR2 runtime (R1/C2): persistent context + output image, sized to the scene target.
	bool			Fsr2EnsureContext( int w, int h );			// (re)create the FSR2 context + output image on size change
	void			Fsr2DestroyContext( bool deviceIdle );		// tear down context/scratch/output (vid_restart, resize, shutdown)

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

	// GPU frame timing (r_vkGpuTime) — a VK_QUERY_TYPE_TIMESTAMP pool holding two
	// stamps (begin+end) per frame slot. Each slot's pair is read back at that
	// slot's next BeginFrame, after its fence proves the GPU is done, so the read
	// never stalls. Mirrors the GL3 backend's r_gl3GpuTime.
	VkQueryPool					gpuTimerPool = VK_NULL_HANDLE;
	bool						gpuTimerSupported = false;	// device+queue can timestamp
	bool						gpuTimerBusy[FRAMES_IN_FLIGHT] = {};	// slot holds an unread pair
	bool						gpuTimerActive = false;		// begin stamp written this frame
	double						gpuTimerPeriodNs = 1.0;		// ns per timestamp tick
	uint64_t					gpuTimerMask = ~0ULL;		// valid-bit mask (queue timestampValidBits)
	double						gpuTimeAccumMs = 0.0;		// summed since last print
	int							gpuTimeSamples = 0;
	unsigned int				gpuTimeLastPrint = 0;
	void						GpuTimerReadback( int slot );

	// ================= M2: rings / shaders / pipelines / images =================

	// M2 helpers
	struct RingBuf;					// defined below (member struct, not ::rhi::RingBuf)
	bool			CreateM2Resources();
	void			DestroyM2Resources();
	void			CreatePipelineCache();		// seed the driver pipeline cache from disk
	void			SavePipelineCache();		// write the driver pipeline cache back to disk
	void			EnsureScenePass();
	// Shared per-draw binding (viewport/scissor/bias, pipeline, descriptor sets, vertex+index
	// buffers) for both Draw and DrawIndexedIndirect. Returns false + the caller draws nothing
	// when the frame/pass/pipeline/buffers aren't ready. Does NOT gate on args.indexCount (the
	// indirect path's count lives in the args buffer).
	bool			BindForDraw( const DrawArgs &args, VkCommandBuffer &cbOut );
	void			ApplyDynState( VkCommandBuffer cb, bool effFlipY );	// viewport/scissor/depth-bias (shared by BindForDraw + DrawZfillBatch)
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
	static const int UBO_RING_SIZE  = 16 << 20;		// per frame slot
	// geometry rings: sized so complex D3 scenes (M7 streams a lot of shadow-volume
	// + multi-pass geometry) fit without the mid-frame doubling in GrowRing. The
	// GL3 4MB/2MB defaults grew to 16MB on busy views; start there so the grow is a
	// rare safety net, not a per-scene warm-up cost.
	static const int VERT_RING_SIZE = 16 << 20;
	static const int IDX_RING_SIZE  = 8 << 20;
	static const int STAGING_RING_SIZE = 2 << 20;	// mid-frame texture updates (cinematics)
	static const int INDIRECT_RING_SIZE = 1 << 20;	// r_vkIndirectTest 1-command-per-draw ring (~52k draws; grows)
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
	// r_vkIndirectTest: per-frame ring of VkDrawIndexedIndirectCommand written one-per-draw
	// (host-visible+INDIRECT). A per-frame-in-flight partition (like the geometry rings) so a
	// draw's command survives until its frame's fence, unread by the next frame's writes.
	RingBuf						indirectRing[FRAMES_IN_FLIGHT];
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
	// parallel to bufferTable, kept in lockstep at every push site. A non-NULL
	// alloc marks a *persistent* buffer (CreateBuffer) that this backend owns and
	// must vmaDestroy; ring slots leave these NULL (the RingBuf owns their alloc)
	// so teardown never double-frees. bufferMapped is the persistent buffer's
	// host pointer (host-visible storage) for UpdateBuffer.
	std::vector<VmaAllocation>	bufferAllocs;
	std::vector<byte *>			bufferMapped;
	// parallel to bufferTable: cached GPU device address (BDA), computed once at
	// creation for buffers with SHADER_DEVICE_ADDRESS usage; 0 = not addressable
	// (ring slots, non-BDA usages, or the feature is unsupported). Cached so
	// GetBufferDeviceAddress never calls vkGetBufferDeviceAddress on a buffer that
	// lacks the usage bit (which would be invalid). (Phase 3.2b.)
	std::vector<uint64_t>		bufferAddr;
	std::vector<uint32_t>		freeBufferSlots;		// DestroyBuffer'd slots, reused by CreateBuffer
	// A DestroyBuffer'd persistent buffer can still be referenced by up to
	// FRAMES_IN_FLIGHT in-flight command buffers (a dynamic shadow/interaction
	// rebuilt this frame, or the whole level's geometry freed at map unload).
	// Destroying it immediately makes the GPU read freed memory → blinking shadows
	// / unload-time UAF. So retire it with a TTL and vmaDestroy only after that many
	// BeginFrame fence waits: DestroyBuffer runs at arbitrary points in the frame
	// cycle (often *between* frames, after frameIndex advanced), so a per-slot list
	// can drain too early — a TTL counted down once per BeginFrame is correct
	// regardless of when the free happens. TTL = FRAMES_IN_FLIGHT+1 guarantees every
	// slot's fence has been waited at least once (all referencing frames done).
	struct RetiredBuffer {
		VkBuffer		buffer;
		VmaAllocation	alloc;
		int				ttl;
	};
	std::vector<RetiredBuffer>	retiredBuffers;
	void						DrainRetiredBuffers( bool force );

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
		// optional tessellation stages (DUDE tessellation, docs/tessellation.md).
		// Loaded when shaders/spv/<name>.tesc.spv + .tese.spv exist; a pipeline
		// built with PipelineDesc::tessellate appends them and switches to a
		// patch-list topology. NULL for every shader without a tess variant.
		VkShaderModule	tesc = VK_NULL_HANDLE;
		VkShaderModule	tese = VK_NULL_HANDLE;
		// compute stage (docs/gpu-offload-plan.md Phase 1). Set by CreateComputeShader;
		// vert/frag stay NULL for a compute rec, and GetComputePipeline reads this.
		VkShaderModule	comp = VK_NULL_HANDLE;
		bool			failed = false;
	};
	std::vector<ShaderRec>		shaderTable;			// handle = index + 1

	std::vector<std::pair<unsigned int, VkSampler> >	samplerCache;	// key = filter|repeat|mips

	// descriptor model (prelude.vk.glsl): set 0 = per-draw UBO (dynamic
	// offset), set 1 = combined image samplers, units 0-7 + cube at 8
	VkDescriptorSetLayout		setLayoutUbo = VK_NULL_HANDLE;
	VkDescriptorSetLayout		setLayoutTex = VK_NULL_HANDLE;
	VkPipelineLayout			pipeLayout = VK_NULL_HANDLE;
	// RR4 bindless materials (docs/rtx-reflections.md): set 2 = a variable-count array of every
	// resident texture, indexed by ImageHandle-1, sampled by the RT reflection shader at a ray hit.
	// Present only when the device reports the 1.2 descriptor-indexing features (haveDescriptorIndexing).
	bool						haveDescriptorIndexing = false;
	VkDescriptorSetLayout		setLayoutBindless = VK_NULL_HANDLE;
	VkDescriptorPool			bindlessPool = VK_NULL_HANDLE;
	VkDescriptorSet				bindlessSet = VK_NULL_HANDLE;
	uint32_t					bindlessCapacity = 0;
	void						SyncBindlessSlot( ImageHandle h );	// RR4: point slot h-1 at the image's view+sampler (or dummy)
	// compute lane (docs/gpu-offload-plan.md Phase 1): one set of 8 storage-buffer
	// bindings + a 128-byte push-constant range, its own pipeline layout, a pool for
	// per-dispatch sets, and a shader-handle-keyed compute pipeline cache. Kept fully
	// separate from the graphics layouts/pipelines above.
	VkDescriptorSetLayout		setLayoutCompute = VK_NULL_HANDLE;
	VkPipelineLayout			computePipeLayout = VK_NULL_HANDLE;
	VkDescriptorPool			computePool = VK_NULL_HANDLE;
	std::unordered_map<ShaderHandle, VkPipeline>	computePipelineCache;
	std::vector<VkDescriptorSet>	retiredComputeSets[FRAMES_IN_FLIGHT];	// freed behind the frame fence
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
		int				colorCount = 0;					// 1-3 color attachments (3 = velocity gbuffer, R1/A0)
		VkFormat		colorFormat[3] = {};			// per-attachment format (mixed for the 3-MRT velocity target)
		VkImage			colorImage[3] = {};
		VmaAllocation	colorAlloc[3] = {};
		VkImageView		colorView[3] = {};				// level-0 attachment view (framebuffer)
		// SSAO Phase 1 mip chain (colorMipLevels > 1, colorImage[0] only). colorView[0]
		// stays level-0-only for the linearize framebuffer; colorSampleView[0] spans every
		// level so the sampleable ImageRec can textureLod into coarser mips. The chain is
		// built by a per-level max-downsample: colorLevelView[L]/colorLevelFb[L] render
		// into level L, colorLevelInput[L] is a single-level ImageRec feeding level L as
		// the next level's source. 1 = plain single-mip (none of the mip fields used).
		int				colorMipLevels = 1;
		static const int MAX_MIP = 8;
		VkImageView		colorSampleView[3] = {};		// all-levels sample view (= colorView[] when 1 mip)
		VkImageView		colorLevelView[MAX_MIP] = {};	// per-level single-level attachment/sample views (L>=1)
		VkFramebuffer	colorLevelFb[MAX_MIP] = {};		// per-level downsample framebuffers (L>=1)
		ImageHandle		colorLevelInput[MAX_MIP] = {};	// per-level single-level sampleable handles (source binds)
		ImageHandle		colorSampleImage[3] = { 0, 0, 0 };	// handles GetRenderTargetImage / 2 return
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
	int			ClampTargetSize( int size, const char *kind, bool cube = false ) const;
	bool		CreateDepthTarget( RenderTarget &t, int w, int h, bool cube );
	// color target: colorCount 1-3 sampleable color attachments (colorFmt, or per-attachment
	// via mrtFormats for the mixed 3-MRT velocity gbuffer), plus a depth-stencil attachment
	// when wantDepthStencil. frameCapable builds the extra load/clearDS pass variants a
	// SetFrameTarget scene buffer needs (HDR).
	bool			CreateColorTarget( RenderTarget &t, int w, int h, VkFormat colorFmt,
	                                   int colorCount, bool wantDepthStencil, bool frameCapable,
	                                   int mipLevels = 1, const VkFormat *mrtFormats = NULL );
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
	VkImageView		FrameDepthView() const;		// view backing FrameDepthImage (for the merged normal fb)

	// SSAO normal-pass merge (docs/ssao-normal-merge.md): a dedicated normal color image
	// rendered alongside the *scene* depth (FrameDepthImage) in the depth prepass. The
	// framebuffer is rebuilt when the scene depth image changes (HDR toggle / resize).
	// B1 uses depth storeOp DONT_CARE — it shares the depth attachment but the scene pass
	// still clears + reseals it via zfill, so nothing here changes the scene depth.
	VkImage			mergeNormalImage = VK_NULL_HANDLE;
	VmaAllocation	mergeNormalAlloc = NULL;
	VkImageView		mergeNormalView = VK_NULL_HANDLE;
	ImageHandle		mergeNormalSampleImage = 0;
	int				mergeNormalW = 0, mergeNormalH = 0;
	VkRenderPass	mergeNormalPass = VK_NULL_HANDLE;	// {normal clear->shader-read, depth clear->dontcare}
	VkFramebuffer	mergeNormalFb = VK_NULL_HANDLE;
	VkImage			mergeNormalFbDepth = VK_NULL_HANDLE;	// depth image the fb was built against
	RenderTargetHandle mergeNormalTarget = 0;			// wraps mergeNormalImage for GetRenderTargetImage
	// optional 2nd color attachment (SSR roughness/metalness MRT) so the merge serves SSR too
	VkImage			mergeMatImage = VK_NULL_HANDLE;
	VmaAllocation	mergeMatAlloc = NULL;
	VkImageView		mergeMatView = VK_NULL_HANDLE;
	ImageHandle		mergeMatSampleImage = 0;			// GetRenderTargetImage2 of the merged handle
	bool			mergeNormalMrt = false;				// the merged image/pass/fb currently carry the MRT
	bool			EnsureMergeNormal( int w, int h, bool wantMrt );
	void			DestroyMergeNormal();
	ImageHandle		RegisterMergeSampleImage( VkImage img, VkImageView view, int w, int h );

	// device features actually enabled (queried before device creation)
	bool						haveAnisotropy = false;
	bool						haveFillModeNonSolid = false;
	// tessellationShader (optional; universal on desktop). Gates the whole DUDE
	// tessellation feature: without it no tess pipeline is ever built and the
	// tess shader modules are skipped. maxTessGenLevel bounds the slider.
	bool						haveTessellation = false;
	uint32_t					maxTessGenLevel = 64;
	// GPU-driven indirect draw (docs/gpu-offload-plan.md Phase 3). The 1.4 floor guarantees
	// the vkCmdDrawIndexedIndirect[Count] *commands* exist, but the FEATURES are still explicit
	// opt-in at device creation: drawIndirectCount gates the count-buffer form, multiDrawIndirect
	// gates any drawCount>1. Universal on desktop; enabled when present, gated when not.
	bool						haveDrawIndirectCount = false;
	bool						haveBufferDeviceAddress = false;	// VK_KHR_buffer_device_address (core 1.2); gates BDA usage + the VMA flag
	// FSR2 Native-AA runtime (docs/fsr-temporal-pipeline.md R1/C2). The context and its
	// scratch persist across frames (they hold the temporal history); recreated when the
	// scene-target size changes. fsr2Out is the display-res UAV FSR2 writes, copied back
	// over the scene color each dispatch. fsr2DepthView is a depth-only-aspect view of the
	// scene target's combined depth-stencil image (a sampled view may carry only one
	// aspect), keyed on fsr2DepthSrc so a target realloc rebuilds it.
	bool						haveSeparateDepthStencilLayouts = false;	// core 1.2; FSR2's depth barriers need it

	// R2 ray-query foundation (docs/rtx-shadow-roadmap.md): VK_KHR_acceleration_structure +
	// VK_KHR_ray_query (+ their required VK_KHR_deferred_host_operations). Deliberately NOT the
	// RT-pipeline/SBT extension - shadows trace inline from existing shaders. No fallback
	// in-spec: pre-Turing/pre-RDNA2 hardware lacks the extensions and every RT feature stays
	// off (capability gate, not emulation). Extension entry points are not exported by the
	// loader, so they resolve via vkGetDeviceProcAddr after device creation.
	bool						haveRayQuery = false;
	uint32_t					asScratchAlignment = 256;	// minAccelerationStructureScratchOffsetAlignment
	PFN_vkGetAccelerationStructureBuildSizesKHR		pfnGetAsBuildSizes = NULL;
	PFN_vkCreateAccelerationStructureKHR			pfnCreateAs = NULL;
	PFN_vkDestroyAccelerationStructureKHR			pfnDestroyAs = NULL;
	PFN_vkCmdBuildAccelerationStructuresKHR			pfnCmdBuildAs = NULL;
	PFN_vkGetAccelerationStructureDeviceAddressKHR	pfnGetAsDeviceAddress = NULL;

	// R2 AS plumbing: raw VMA buffers with the AS-specific usage bits BufferHandle doesn't
	// carry (build-input / AS-storage / scratch), host-visible + BDA. Tiny per-AS metadata;
	// device-local staging is a later perf pass, mirroring the static-vertex-buffer history.
	struct RtBuf {
		VkBuffer			buf = VK_NULL_HANDLE;
		VmaAllocation		alloc = NULL;
		VkDeviceAddress		addr = 0;
		void *				map = NULL;			// persistent host mapping (all RtBufs are host-visible)
	};
	struct RtBlas {
		VkAccelerationStructureKHR	as = VK_NULL_HANDLE;	// VK_NULL_HANDLE = freed slot
		RtBuf						buf;
		VkDeviceAddress				addr = 0;				// AS device address (TLAS instances consume this)
		// RR5: the per-geometry descriptors this BLAS was built from (device-buffer-fed BLASes only).
		// UpdateTlas copies them into the geometry table so a reflection ray can fetch st + material at a
		// world hit. Empty for CPU-fed CreateBlas (positions only) -> those instances get a zero defer row.
		std::vector<BlasGeometry>	geoms;
		// R3.5 animated (device-buffer-fed) BLAS: built with ALLOW_UPDATE and refit in place from
		// gpuSkinVB each pose change. updateScratch is the persistent scratch RefitBlas reuses; both
		// stay unset (0/false) for the synchronous CPU-fed CreateBlas path.
		RtBuf						updateScratch;
		bool						updatable = false;
	};
	std::vector<RtBlas>			rtBlases;					// BlasHandle = index + 1
	VkAccelerationStructureKHR	rtTlas = VK_NULL_HANDLE;
	RtBuf						rtTlasBuf;
	VkDeviceAddress				rtTlasAddr = 0;				// synchronous-scene TLAS address (fallback)
	// R3.5 validator (r_rtAnimBlasTest): a throwaway TLAS built independently of the scene above, so a
	// validator can trace test geometry without clobbering the live rtTlas / per-frame slots.
	VkAccelerationStructureKHR	rtTestTlas = VK_NULL_HANDLE;
	RtBuf						rtTestTlasBuf;
	VkDeviceAddress				rtTestTlasAddr = 0;
	// translate RtInstance[] -> VK instances + build one TLAS synchronously into the caller's target
	// objects (shared by BuildTlas + BuildStandaloneTlas; the caller clears the target first)
	unsigned long long	BuildTlasInto( const RtInstance *instances, int count,
	                                   VkAccelerationStructureKHR &tlas, RtBuf &tlasBuf, VkDeviceAddress &tlasAddr );

	// R3.5 S3: per-entity animated BLAS cache. Each visible GPU-skinned monster gets one refit-capable
	// BLAS built from its gpuSkinVB, keyed by a stable entity id + a topology signature. Built/refit on
	// the frame cb in RefreshAnimBlas (after the skin flush), so it captures the current-frame pose.
	struct AnimBlas {
		uint32_t					key = 0;			// entity id (0 = free slot)
		unsigned long long			topoSig = 0;		// model + per-surface counts + gpuSkinVB handles; change => rebuild
		VkAccelerationStructureKHR	as = VK_NULL_HANDLE;
		RtBuf						buf;				// AS storage
		RtBuf						scratch;			// persistent scratch (sized to max(build,update); reused by refits)
		VkDeviceAddress				addr = 0;
		uint32_t					maxPrims = 0;		// triangles the AS was built for (refit must match)
		int							lastSeenFrame = -1;
	};
	std::vector<AnimBlas>		animBlasCache;
	struct AnimCasterStaged {						// a copy of one RHI::AnimCaster (backend-owned)
		uint32_t					key;
		unsigned long long			topoSig;
		float						transform[12];
		uint32_t					mask;
		int							geomFirst, geomCount;	// slice into animStagedGeoms
	};
	std::vector<AnimCasterStaged>	animStaged;			// this frame's staged casters (UpdateAnimCasters)
	std::vector<BlasGeometry>		animStagedGeoms;	// flattened per-caster geometry
	std::vector<VkAccelerationStructureInstanceKHR>	animPendInst;	// S4: this frame's animated TLAS instances (appended in RefreshAnimBlas)
	std::vector<uint32_t>		animPendGeoFirst;	// RR0: parallel to animPendInst — geomFirst into animStagedGeoms
	std::vector<uint32_t>		animPendGeoCount;	// RR0: parallel to animPendInst — geometry (surface) count
	struct RetiredAs { VkAccelerationStructureKHR as; RtBuf buf; RtBuf scratch; int ttl; };
	std::vector<RetiredAs>		retiredAnimAs;			// fence-safe deferred AS destroys (TTL = FRAMES_IN_FLIGHT)
	int							animBlasFrameCounter = 0;
	int							animStatBuilds = 0, animStatRefits = 0, animStatRetires = 0;	// cumulative
	AnimBlas *	FindOrAllocAnimBlas( uint32_t key );
	bool		RecordAnimBlasBuild( AnimBlas &e, const BlasGeometry *geoms, int count, VkCommandBuffer cb );
	void		RecordAnimBlasRefit( AnimBlas &e, const BlasGeometry *geoms, int count, VkCommandBuffer cb );
	void		RetireAnimBlas( AnimBlas &e );				// move to retiredAnimAs (fence-safe destroy later)
	void		DrainRetiredAnimAs( bool force );			// destroy retired ASes whose TTL elapsed
	void			UpdateAnimCasters( const AnimCaster *casters, int count ) override;
	void			RefreshAnimBlas() override;
	void			AnimBlasStats( int &builds, int &refits, int &retires, int &live ) override;
	void			RtReflStats( int &geoRows, int &geoMonsterRows ) override;
	unsigned long long	GetRtGeoTableAddress() override { return (unsigned long long)rtCurrentGeoAddr; }

	// R3 per-frame TLAS lane (movers): a TLAS slot per frame-in-flight, fully rebuilt each
	// frame from CURRENT instance transforms. UpdateTlas (frontend, between frames) waits the
	// slot's fence (its last use was FRAMES_IN_FLIGHT frames ago), uploads the instances and
	// arms the slot; BeginFrame records the build on the frame cb AHEAD of every draw, then
	// barriers AS-build writes against fragment-shader ray reads. The synchronous rtTlas
	// stays as the liveness anchor + address fallback when no per-frame build is live.
	VkAccelerationStructureKHR	rtFrameTlas[FRAMES_IN_FLIGHT] = {};
	RtBuf						rtFrameTlasBuf[FRAMES_IN_FLIGHT];
	RtBuf						rtFrameInstBuf[FRAMES_IN_FLIGHT];
	RtBuf						rtFrameScratch[FRAMES_IN_FLIGHT];
	uint32_t					rtFrameCapacity[FRAMES_IN_FLIGHT] = {};	// instances each slot is sized for
	uint32_t					rtFrameCount[FRAMES_IN_FLIGHT] = {};	// instances armed for the pending build
	// RT reflections RR0 (docs/rtx-reflections.md): a per-slot geometry table parallel to the instance
	// buffer. One RtGeoDesc row per (instance, geometry); each instance's instanceCustomIndex is its base
	// row. Monster rows carry the surface's gpuSkinVB/index device addresses so the reflection shader can
	// fetch + shade the hit; static/mover rows are zero (vtxAddr 0 = "no attributes, defer to SSR").
	struct RtGeoDesc {			// std430 / GL_EXT_buffer_reference layout (32 B, 8-aligned)
		uint64_t				vtxAddr;
		uint64_t				idxAddr;
		uint32_t				vtxStride;
		uint32_t				flags;			// bit0 = has attributes (monster)
		uint32_t				baseColor;		// RR3: material average colour, packed RGBA8 (unpackUnorm4x8)
		uint32_t				texIndex;		// RR4: bindless texture slot+1 (0 = none -> shader uses baseColor)
	};
	static const uint32_t		RT_GEO_MONSTER = 1u;
	RtBuf						rtFrameGeoTable[FRAMES_IN_FLIGHT];		// device-addressable RtGeoDesc[]
	uint32_t					rtFrameGeoCap[FRAMES_IN_FLIGHT] = {};	// rows each slot is sized for
	uint32_t					rtFrameGeoRows[FRAMES_IN_FLIGHT] = {};	// rows filled this frame (running)
	int							rtReflStatRows = 0, rtReflStatMonsterRows = 0;	// RR0 debug readout (last frame)
	VkDeviceAddress				rtFrameAddr[FRAMES_IN_FLIGHT] = {};
	bool						rtFrameBuilt[FRAMES_IN_FLIGHT] = {};	// slot holds a completed build (safe to traverse)
	int							rtPendingSlot = -1;						// slot awaiting its BeginFrame build
	// R3.5 S4: when animated casters are staged, UpdateTlas defers this slot's TLAS build to
	// RefreshAnimBlas (after the skin flush), which appends one model-space instance per monster and
	// records the build on the frame cb — capturing the current-frame pose. -1 when not deferred.
	int							rtAnimTlasDeferredSlot = -1;
	VkDeviceAddress				rtCurrentAddr = 0;						// per-frame TLAS address (0 = use rtTlasAddr)
	VkDeviceAddress				rtCurrentGeoAddr = 0;					// RR2: geo table for the slot rtCurrentAddr names (0 = none)
	void	RecordFrameTlasBuild( VkCommandBuffer cb, int slot );		// build + AS-write -> frag-read barrier
	bool	EnsureFrameGeoTable( int slot, uint32_t rows );				// RR0: (re)create the per-slot RtGeoDesc table
	void	DestroyRtFrameSlots();

	// R3 animated casters (monsters): one combined WORLD-space triangle-soup BLAS per frame
	// slot, rebuilt each frame the frontend supplies geometry. The slot's dyn-BLAS build is
	// recorded on the frame cb AHEAD of the TLAS build (dyn-BLAS-write -> TLAS-read barrier),
	// and UpdateTlas appends one identity instance for it. Fresh slots prime synchronously so
	// a fresh TLAS prime that references the dyn-BLAS finds it built.
	VkAccelerationStructureKHR	rtFrameDynBlas[FRAMES_IN_FLIGHT] = {};
	RtBuf						rtFrameDynBlasBuf[FRAMES_IN_FLIGHT];
	RtBuf						rtFrameDynVb[FRAMES_IN_FLIGHT];
	RtBuf						rtFrameDynIb[FRAMES_IN_FLIGHT];
	RtBuf						rtFrameDynScratch[FRAMES_IN_FLIGHT];
	uint32_t					rtFrameDynVertCap[FRAMES_IN_FLIGHT] = {};	// verts each slot's dyn-BLAS is sized for
	uint32_t					rtFrameDynTriCap[FRAMES_IN_FLIGHT] = {};	// triangles ditto
	uint32_t					rtFrameDynVerts[FRAMES_IN_FLIGHT] = {};		// verts armed for the pending build (maxVertex)
	uint32_t					rtFrameDynTris[FRAMES_IN_FLIGHT] = {};		// triangles armed for the pending build
	VkDeviceAddress				rtFrameDynAddr[FRAMES_IN_FLIGHT] = {};
	bool						rtFrameDynBuilt[FRAMES_IN_FLIGHT] = {};
	int							rtDynArmedSlot = -1;						// slot whose dyn-BLAS UpdateTlas must append (one-shot)
	int							rtDynPendingSlot = -1;						// slot whose dyn-BLAS BeginFrame must rebuild (steady state)
	void	RecordFrameDynBlasBuild( VkCommandBuffer cb, int slot );		// rebuild + AS-write -> AS-read barrier
	bool	CreateRtBuffer( VkBufferUsageFlags usage, VkDeviceSize size, const void *data, RtBuf &rb );
	void	DestroyRtBuffer( RtBuf &rb );
	// size + create + build one AS on the upload cb, synchronously (scratch is transient)
	bool	BuildAsSync( VkAccelerationStructureTypeKHR type, const VkAccelerationStructureGeometryKHR &geom,
	                     uint32_t primCount, RtBuf &asBuf, VkAccelerationStructureKHR &as );
	FfxFsr2Context *			fsr2Ctx = NULL;
	void *						fsr2Scratch = NULL;
	int							fsr2W = 0, fsr2H = 0;
	VkImage						fsr2Out = VK_NULL_HANDLE;
	VmaAllocation				fsr2OutAlloc = NULL;
	VkImageView					fsr2OutView = VK_NULL_HANDLE;
	bool						fsr2OutWritten = false;		// false until first dispatch (layout UNDEFINED)
	VkImage						fsr2DepthSrc = VK_NULL_HANDLE;
	VkImageView					fsr2DepthView = VK_NULL_HANDLE;
	bool						fsr2WarnedNoFeature = false;
	bool						fsr2FirstDispatch = true;	// force reset on the first dispatch of a context
	// R1/D auto-reactive: opaque-only scene snapshot (Fsr2CaptureOpaque, straight copy at
	// the translucent split) + the R8 reactive mask FSR2 generates from opaque-vs-final.
	// fsr2OpaqueValid is per-frame (cleared in BeginFrame): the mask is only meaningful
	// when the snapshot came from THIS frame's opaque scene.
	VkImage						fsr2Opaque = VK_NULL_HANDLE;
	VmaAllocation				fsr2OpaqueAlloc = NULL;
	VkImageView					fsr2OpaqueView = VK_NULL_HANDLE;
	int							fsr2OpaqueW = 0, fsr2OpaqueH = 0;
	bool						fsr2OpaqueWritten = false;	// layout: false = UNDEFINED, true = SHADER_READ_ONLY
	bool						fsr2OpaqueValid = false;	// captured this frame (cleared each BeginFrame)
	VkImage						fsr2Reactive = VK_NULL_HANDLE;
	VmaAllocation				fsr2ReactiveAlloc = NULL;
	VkImageView					fsr2ReactiveView = VK_NULL_HANDLE;
	bool						fsr2ReactiveWritten = false;	// false = UNDEFINED, true = SHADER_READ_ONLY (post-dispatch)
	// r_vkBdaZfill (Phase 3.2b): cached handles for the depth-prepass BDA consume.
	// Loaded on the cvar's first enable; the flat zfill draw is identified by
	// currentDesc.shader == zfillShaderHandle. zfillBdaShaderHandle 0 = variant absent.
	ShaderHandle				zfillShaderHandle = 0;
	ShaderHandle				zfillBdaShaderHandle = 0;	// per-draw BDA vertex fetch (mode 1)
	ShaderHandle				zfillBatchShaderHandle = 0;	// batched indirect, verts+indices via BDA (mode 2)
	// r_vkBdaZfill firing counters (confirmation the consume path is active, not
	// silently falling back — a pixel-identical A/B looks the same either way).
	int							bdaZfillDraws = 0;		// zfill draws routed via device address last frame
	int							bdaZfillFallback = 0;	// candidate zfill draws that fell back (buffer not addressable)
	int							bdaZfillBatched = 0;	// surfaces folded into batched indirect draws last frame
	int							bdaZfillBatchDraws = 0;	// number of indirect draws (one per distinct scissor group)
	unsigned int				bdaZfillLastPrint = 0;
	// mode-2 batch: per-frame double-buffered SSBO of ObjRec + the VkDrawIndirectCommand[]. Both
	// BU_STORAGE (addressable + INDIRECT usage); grown on demand and updated each frame (host-visible).
	BufferHandle				batchObjBuf[FRAMES_IN_FLIGHT] = { 0 };
	BufferHandle				batchCmdBuf[FRAMES_IN_FLIGHT] = { 0 };
	int							batchCapacity[FRAMES_IN_FLIGHT] = { 0 };	// objects each buffer pair holds
	bool						haveMultiDrawIndirect = false;
	bool						haveDrawIndirectFirstInstance = false;	// non-zero firstInstance in indirect cmds (Phase 3.2b batch)
	bool						indirectFeatureWarned = false;

	std::unordered_map<unsigned long long, VkPipeline>	pipelineCache;
	// disk-persisted DRIVER pipeline cache (distinct from the map above, which is our
	// CPU permutation->VkPipeline table): seeded from a blob at Init, fed to every
	// vkCreateGraphicsPipelines, written back at Shutdown so first-use compile cost is
	// paid once across runs, not every session.
	VkPipelineCache				diskPipelineCache = VK_NULL_HANDLE;
	PipelineDesc				currentDesc;
	VkPipeline					boundPipeline = VK_NULL_HANDLE;
	uint64_t					boundTexKey = 0;
	VkDescriptorSet				boundTexSet = VK_NULL_HANDLE;
	bool						bindlessBoundThisCb = false;	// RR4: set 2 (bindless array) bound once per frame cb
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
	void						DrawDebugHud();			// r_vkDebugHud overlay (telemetry / VRAM / errors)
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

// Set the moment vkCheck sees VK_ERROR_DEVICE_LOST (independent of crash logging) so the
// per-frame dead-device guards engage by default; cleared in Init() so vid_restart recovers.
static bool vkDeviceDead = false;

// Once-only guard for the on-disk device-lost dump. Decoupled from vkDeviceDead so the
// mitigation (guards) and the diagnostic (dump) never gate each other. Reset in Init().
static bool vkCrashDumpWritten = false;

// Short ring of recent validation / debug messages for the crash dump and the debug HUD.
// Guarded by vkDebugMsgMutex: the debug-utils callback can run off the main thread while
// the HUD / dump reads it.
static const int VK_DEBUG_MSG_HISTORY = 64;
static std::vector<idStr> vkDebugMsgHistory;
static std::mutex vkDebugMsgMutex;

// One GPU telemetry sample. The ring of these is the crash dump's most useful forensic signal:
// it shows the P-state / clock / power trajectory in the seconds before a device loss, which is
// exactly what a boost-transition (di/dt) hypothesis needs to confirm. Sampled ~10 Hz in
// BeginFrame via NVML (implementation lives near the debug HUD at the end of this TU).
struct VkTelemetrySample {
	unsigned int       timeMs = 0;      // Sys_Milliseconds() when sampled
	unsigned           smClockMHz = 0;
	unsigned           memClockMHz = 0;
	unsigned           tempC = 0;
	unsigned           powerW = 0;      // instantaneous board draw (rounded)
	unsigned           pstate = 32;     // NVML perf state P0..P15 (0 = max); 32 = unknown
};
static const int          VK_TELEMETRY_RING = 256;   // ~25 s of history at 10 Hz
static VkTelemetrySample  vkTelemetryRing[VK_TELEMETRY_RING];
static int                vkTelemetryHead = 0;        // next write slot
static int                vkTelemetryCount = 0;
static unsigned int       vkTelemetryLastMs = 0;
// slow-changing extras for the HUD's power-limit / VRAM lines (refreshed ~1 Hz)
static unsigned           vkTelemetryPowerLimitW = 0;
static unsigned long long vkTelemetryMemUsedMB = 0, vkTelemetryMemTotalMB = 0;
static unsigned int       vkTelemetryExtraMs = 0;

/*
====================
WriteVulkanCrashDumpBody

Best-effort, CPU-only crash dump written into fs_savepath. Avoids every Vulkan call (the
device may be lost) and uses a portable save-path lookup (no Posix_* — this TU is also
compiled for the Windows cross-build). `suffix` distinguishes the forced dump's filename.
====================
*/
static void WriteVulkanCrashDumpBody( const char *what, VkResult res, const char *suffix ) {
	const char *savePath = cvarSystem->GetCVarString( "fs_savepath" );
	if ( savePath == NULL || savePath[0] == '\0' ) {
		return;
	}

	time_t now = time( NULL );
	// one-shot crash path: plain localtime()'s static buffer is fine here, and it sidesteps
	// the localtime_r (POSIX) vs localtime_s (Win) signature split for the cross-build.
	struct tm *tmnow = localtime( &now );
	char timestr[64];
	if ( tmnow != NULL ) {
		idStr::snPrintf( timestr, sizeof(timestr), "%04d%02d%02d-%02d%02d%02d",
				tmnow->tm_year + 1900, tmnow->tm_mon + 1, tmnow->tm_mday,
				tmnow->tm_hour, tmnow->tm_min, tmnow->tm_sec );
	} else {
		idStr::Copynz( timestr, "unknown-time", sizeof( timestr ) );
	}

	char fname[1024];
	idStr::snPrintf( fname, sizeof(fname), "%s/dudelog-crash-%s%s.txt", savePath, timestr, suffix ? suffix : "" );

	FILE *f = fopen( fname, "w" );
	if ( !f ) {
		common->Warning( "VK: failed to open crash dump file '%s' for writing", fname );
		return;
	}

	fprintf( f, "DUDE Vulkan crash dump\n" );
	fprintf( f, "Timestamp: %s\n", timestr );
	fprintf( f, "Event: %s\n", what ? what : "(null)" );
	fprintf( f, "VkResult: %d\n\n", (int)res );

	// Static / global runtime state we can safely gather without calling Vulkan.
#ifdef ID_DEDICATED
	fprintf( f, "Engine build: dedicated\n" );
#else
	fprintf( f, "Engine build: client\n" );
#endif
	fprintf( f, "glConfig vendor: %s\n", glConfig.vendor_string );
	fprintf( f, "glConfig renderer: %s\n", glConfig.renderer_string );
	fprintf( f, "glConfig version: %s\n", glConfig.version_string );
	fprintf( f, "glConfig maxTextureSize: %d\n", glConfig.maxTextureSize );
	fprintf( f, "Estimated VRAM (glConfig.vidMemMB): %d MB\n\n", glConfig.vidMemMB );

	// CVar summary useful for reproduction
	fprintf( f, "CVar snapshot:\n" );
	fprintf( f, "  r_vkValidation = %d\n", r_vkValidation.GetInteger() );
	fprintf( f, "  r_vkCrashLogging = %d\n", r_vkCrashLogging.GetInteger() );
	fprintf( f, "  r_hdr = %d\n", r_hdr.GetInteger() );
	fprintf( f, "  r_shadows = %d\n", r_shadows.GetInteger() );
	fprintf( f, "  r_ssr = %d\n", cvarSystem->GetCVarInteger( "r_ssr" ) );
	fprintf( f, "  r_ssao = %d\n", cvarSystem->GetCVarInteger( "r_ssao" ) );
	fprintf( f, "  r_fsr2 = %d\n", cvarSystem->GetCVarInteger( "r_fsr2" ) );
	fprintf( f, "\n" );

	fprintf( f, "Validation errors this session: %d\n\n", vkValidationErrors );

	// Include the last N validation/debug messages collected by the debug callback.
	fprintf( f, "Recent Vulkan validation/debug messages (newest last):\n" );
	{
		std::lock_guard<std::mutex> lock( vkDebugMsgMutex );
		for ( size_t i = 0; i < vkDebugMsgHistory.size(); ++i ) {
			fprintf( f, "  %s\n", vkDebugMsgHistory[i].c_str() );
		}
	}
	if ( r_vkValidation.GetInteger() == 0 ) {
		fprintf( f, "  (validation layer was OFF — set r_vkValidation 1 to capture messages here next time)\n" );
	}
	fprintf( f, "\n" );

	// Telemetry trajectory: the clock / P-state / power history right before the event. For a
	// suspected boost-transition (di/dt) crash this is the key signal — watch the P-state and SM
	// clock in the newest rows (t-ms counts down to ~0 at the dump). A large P8->P0 jump / clock
	// ramp immediately before the loss supports "a current spike on the way back up killed it".
	fprintf( f, "GPU telemetry ring (~10 Hz; t-ms = ms before this dump, newest last):\n" );
	if ( vkTelemetryCount == 0 ) {
		fprintf( f, "  (none — NVML unavailable, or neither r_vkCrashLogging nor r_vkDebugHud was on before the crash)\n" );
	} else {
		fprintf( f, "  %8s  %-6s  %8s  %6s  %7s  %8s\n", "t-ms", "Pstate", "SMclkMHz", "TempC", "PowerW", "MEMclk" );
		const int          n = vkTelemetryCount;
		const int          newestIdx = ( vkTelemetryHead - 1 + VK_TELEMETRY_RING ) % VK_TELEMETRY_RING;
		const unsigned int newestMs = vkTelemetryRing[newestIdx].timeMs;
		const int          start = ( vkTelemetryHead - n + VK_TELEMETRY_RING * 2 ) % VK_TELEMETRY_RING;
		for ( int k = 0; k < n; k++ ) {
			const VkTelemetrySample &s = vkTelemetryRing[ ( start + k ) % VK_TELEMETRY_RING ];
			char pst[8];
			if ( s.pstate <= 15 ) { idStr::snPrintf( pst, sizeof( pst ), "P%u", s.pstate ); }
			else                  { idStr::Copynz( pst, "P?", sizeof( pst ) ); }
			fprintf( f, "  %8u  %-6s  %8u  %6u  %7u  %8u\n",
				(unsigned)( newestMs - s.timeMs ), pst, s.smClockMHz, s.tempC, s.powerW, s.memClockMHz );
		}
	}
	fprintf( f, "\n" );

	fprintf( f, "Notes:\n" );
	fprintf( f, "  - This is a CPU-only best-effort dump. Vulkan objects are not queried because the device may be lost.\n" );
	fprintf( f, "  - For vendor-specific details (Xid on NVIDIA / GPU hang reason) check kernel logs: dmesg or journalctl -k.\n" );
	fprintf( f, "  - Re-run with r_vkValidation=1 and r_vkCrashLogging=1 to capture more validation messages next time.\n" );

	fclose( f );
	common->Printf( "VK: crash dump written to %s\n", fname );
}

// device-lost path: honors the r_vkCrashLogging opt-in and writes at most once per session.
static void WriteVulkanCrashDump( const char *what, VkResult res ) {
	if ( !r_vkCrashLogging.GetBool() || vkCrashDumpWritten ) {
		return;
	}
	vkCrashDumpWritten = true;
	WriteVulkanCrashDumpBody( what, res, NULL );
}

// forced (vk_dump_state console cmd): always writes a snapshot, regardless of the opt-in.
static void WriteVulkanCrashDumpForced( const char *what, VkResult res ) {
	WriteVulkanCrashDumpBody( what, res, "-forced" );
}


static bool vkCheck( VkResult res, const char *what ) {
	if ( vkDeviceDead ) {
		// once the device is considered dead, avoid further Vulkan API usage and warn once
		static bool warned = false;
		if ( !warned ) {
			common->Warning( "VK: device is dead, skipping Vulkan call check for %s", what );
			warned = true;
		}
		return false;
	}
	if ( res == VK_SUCCESS ) {
		return true;
	}
	common->Warning( "VK: %s failed (VkResult %d)", what, (int)res );

	// A device-lost is terminal: mark the device dead so the per-frame guards stop issuing
	// Vulkan calls (this IS the mitigation, and it must engage regardless of crash logging),
	// then emit a best-effort dump if the user opted in.
	if ( res == VK_ERROR_DEVICE_LOST ) {
		WriteVulkanCrashDump( what, res );
		vkDeviceDead = true;
	}
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

	// Append to the in-memory history for the crash dump / debug HUD. Keep the ring bounded.
	// Locked: this callback can run off the main thread while the HUD reads the ring.
	if ( data->pMessage ) {
		std::lock_guard<std::mutex> lock( vkDebugMsgMutex );
		if ( vkDebugMsgHistory.size() >= VK_DEBUG_MSG_HISTORY ) {
			vkDebugMsgHistory.erase( vkDebugMsgHistory.begin() );
		}
		vkDebugMsgHistory.emplace_back( data->pMessage );
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

	const char *devExts[4] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
	uint32_t devExtCount = 1;

	// R2 ray-query foundation: probe for the RT extension trio. deferred_host_operations is a
	// hard dependency of acceleration_structure even though we never use host builds. All other
	// dependencies (BDA, descriptor indexing) are core on our 1.4 floor.
	bool haveAccelExt = false, haveRayQueryExt = false, haveDeferredOpsExt = false;
	{
		uint32_t extCount = 0;
		vkEnumerateDeviceExtensionProperties( physical, NULL, &extCount, NULL );
		std::vector<VkExtensionProperties> extProps( extCount );
		if ( extCount > 0 ) {
			vkEnumerateDeviceExtensionProperties( physical, NULL, &extCount, extProps.data() );
		}
		for ( uint32_t i = 0; i < extCount; i++ ) {
			if ( !strcmp( extProps[i].extensionName, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME ) ) {
				haveAccelExt = true;
			} else if ( !strcmp( extProps[i].extensionName, VK_KHR_RAY_QUERY_EXTENSION_NAME ) ) {
				haveRayQueryExt = true;
			} else if ( !strcmp( extProps[i].extensionName, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME ) ) {
				haveDeferredOpsExt = true;
			}
		}
	}

	// optional features the M2+ paths use when present: sampler anisotropy
	// (TF_DEFAULT textures) and line-fill mode (GLS_POLYMODE_LINE debug draws)
	VkPhysicalDeviceFeatures supported = {};
	vkGetPhysicalDeviceFeatures( physical, &supported );
	VkPhysicalDeviceFeatures enabled = {};
	haveAnisotropy = supported.samplerAnisotropy == VK_TRUE;
	haveFillModeNonSolid = supported.fillModeNonSolid == VK_TRUE;
	enabled.samplerAnisotropy = haveAnisotropy ? VK_TRUE : VK_FALSE;
	enabled.fillModeNonSolid = haveFillModeNonSolid ? VK_TRUE : VK_FALSE;
	// DUDE tessellation (docs/tessellation.md): PN-triangle smoothing of enemy/
	// prop meshes. Optional feature, but present on every desktop GPU we target.
	haveTessellation = supported.tessellationShader == VK_TRUE;
	enabled.tessellationShader = haveTessellation ? VK_TRUE : VK_FALSE;
	if ( haveTessellation ) {
		maxTessGenLevel = physProps.limits.maxTessellationGenerationLevel;
	}
	// zfill.vert always writes gl_ClipDistance[0] (subview near clip; zero
	// plane when unused) — universal on desktop
	if ( supported.shaderClipDistance ) {
		enabled.shaderClipDistance = VK_TRUE;
	} else {
		common->Warning( "VK: device lacks shaderClipDistance - the depth prepass shader may fail" );
	}

	// GPU-offload hardening (docs/gpu-offload-plan.md): with robustBufferAccess an out-of-bounds
	// vertex/index/storage fetch returns 0 / is clamped instead of faulting the device. The compute
	// offload paths (skinning, tessellate-once) hand the rasterizer buffers produced this frame, so a
	// bug there must degrade to a visible glitch — never a GPU page fault that device-losts and hangs
	// the machine. Universally supported on desktop and near-zero cost; enable whenever present.
	if ( supported.robustBufferAccess ) {
		enabled.robustBufferAccess = VK_TRUE;
	}

	// GPU-driven indirect draw (Phase 3): multiDrawIndirect gates a >1 drawCount in a single
	// vkCmdDrawIndexedIndirect; the count-buffer form (drawIndirectCount) is a Vulkan 1.2 feature
	// enabled below via the pNext chain. Both universal on desktop; enable when present.
	haveMultiDrawIndirect = supported.multiDrawIndirect == VK_TRUE;
	enabled.multiDrawIndirect = haveMultiDrawIndirect ? VK_TRUE : VK_FALSE;
	// Phase 3.2b batched zfill: each VkDrawIndirectCommand carries a non-zero firstInstance to
	// index the per-object SSBO (gl_InstanceIndex). A non-zero indirect firstInstance needs this
	// feature; without it the batch is disabled (ZfillBatchEnabled) and mode 2 degrades to per-draw.
	haveDrawIndirectFirstInstance = supported.drawIndirectFirstInstance == VK_TRUE;
	enabled.drawIndirectFirstInstance = haveDrawIndirectFirstInstance ? VK_TRUE : VK_FALSE;

	// discard in fragment shaders compiles to OpDemoteToHelperInvocation under
	// the vulkan1.4 SPIR-V target (modern helper-invocation semantics rather
	// than the old OpKill); the capability needs shaderDemoteToHelperInvocation,
	// a core + required feature since Vulkan 1.3, so guaranteed on our 1.4 floor.
	VkPhysicalDeviceVulkan13Features supported13 = {};
	supported13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
	// Vulkan 1.2 feature struct: drawIndirectCount (the vkCmdDraw*IndirectCount form) lives here.
	VkPhysicalDeviceVulkan12Features supported12 = {};
	supported12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
	supported13.pNext = &supported12;
	// Vulkan 1.1 feature struct: 16-bit storage, needed by FSR2's fp16 shader permutations.
	VkPhysicalDeviceVulkan11Features supported11 = {};
	supported11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
	supported12.pNext = &supported11;
	// R2: acceleration-structure + ray-query feature structs, chained into the query only when
	// the extensions actually exist (an unrecognized pNext struct is not valid otherwise).
	VkPhysicalDeviceAccelerationStructureFeaturesKHR supportedAccel = {};
	supportedAccel.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
	VkPhysicalDeviceRayQueryFeaturesKHR supportedRq = {};
	supportedRq.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
	if ( haveAccelExt && haveRayQueryExt && haveDeferredOpsExt ) {
		supported11.pNext = &supportedAccel;
		supportedAccel.pNext = &supportedRq;
	}
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

	// GPU-driven indirect draw (Phase 3): the count-buffer form needs drawIndirectCount enabled
	// explicitly (the 1.4 command floor does not imply the feature). Chained after enabled13.
	VkPhysicalDeviceVulkan12Features enabled12 = {};
	enabled12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
	haveDrawIndirectCount = supported12.drawIndirectCount == VK_TRUE;
	enabled12.drawIndirectCount = haveDrawIndirectCount ? VK_TRUE : VK_FALSE;
	// Buffer device address (Phase 3.2b): lets a shader dereference a buffer via a raw 64-bit
	// pointer (GL_EXT_buffer_reference) instead of a bound vertex buffer — the modern path to
	// per-draw geometry without one unified vb. Core in 1.2; enable only if reported, and gate
	// the VMA allocator flag + buffer usage bit on the same flag (all three must agree).
	haveBufferDeviceAddress = supported12.bufferDeviceAddress == VK_TRUE;
	enabled12.bufferDeviceAddress = haveBufferDeviceAddress ? VK_TRUE : VK_FALSE;
	// FSR2 (docs/fsr-temporal-pipeline.md) auto-selects fp16 shader permutations on GPUs that
	// support half precision; enable shaderFloat16 + 16-bit storage when present so those
	// pipelines create. Enabled only when supported, so device creation is unchanged on GPUs
	// that lack them, and the features are inert unless FSR2 actually runs.
	enabled12.shaderFloat16 = supported12.shaderFloat16;
	// FSR2 C2: the vendored VK backend emits depth-ONLY-aspect barriers on the sampled scene
	// depth, which a combined D24S8/D32S8 image only permits with separateDepthStencilLayouts
	// (core 1.2). Enable when supported; RunFsr2 refuses (once, with a warning) without it.
	haveSeparateDepthStencilLayouts = supported12.separateDepthStencilLayouts == VK_TRUE;
	enabled12.separateDepthStencilLayouts = haveSeparateDepthStencilLayouts ? VK_TRUE : VK_FALSE;
	// RR4 bindless materials (docs/rtx-reflections.md): descriptor indexing lets the RT passes sample
	// any resident texture from one variable-count set-2 array. All five bits are core Vulkan 1.2;
	// enable when the device reports them (guaranteed on our 1.4 floor). haveDescriptorIndexing gates
	// the set-2 layout + the pipeLayout's third set built later.
	haveDescriptorIndexing = supported12.runtimeDescriptorArray
		&& supported12.shaderSampledImageArrayNonUniformIndexing
		&& supported12.descriptorBindingPartiallyBound
		&& supported12.descriptorBindingSampledImageUpdateAfterBind
		&& supported12.descriptorBindingVariableDescriptorCount;
	if ( haveDescriptorIndexing ) {
		enabled12.runtimeDescriptorArray = VK_TRUE;
		enabled12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
		enabled12.descriptorBindingPartiallyBound = VK_TRUE;
		enabled12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
		enabled12.descriptorBindingVariableDescriptorCount = VK_TRUE;
	}
	enabled13.pNext = &enabled12;

	VkPhysicalDeviceVulkan11Features enabled11 = {};
	enabled11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
	enabled11.storageBuffer16BitAccess = supported11.storageBuffer16BitAccess;
	enabled11.uniformAndStorageBuffer16BitAccess = supported11.uniformAndStorageBuffer16BitAccess;
	enabled12.pNext = &enabled11;

	// R2 ray-query foundation: enable the RT trio when the extensions AND both features AND BDA
	// (AS builds consume raw device addresses) are all present. One flag gates every RT feature
	// downstream; device creation is byte-identical on hardware that lacks any piece.
	haveRayQuery = haveAccelExt && haveRayQueryExt && haveDeferredOpsExt
		&& supportedAccel.accelerationStructure == VK_TRUE
		&& supportedRq.rayQuery == VK_TRUE
		&& haveBufferDeviceAddress;
	VkPhysicalDeviceAccelerationStructureFeaturesKHR enabledAccel = {};
	enabledAccel.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
	VkPhysicalDeviceRayQueryFeaturesKHR enabledRq = {};
	enabledRq.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
	if ( haveRayQuery ) {
		enabledAccel.accelerationStructure = VK_TRUE;
		enabledRq.rayQuery = VK_TRUE;
		enabled11.pNext = &enabledAccel;
		enabledAccel.pNext = &enabledRq;
		devExts[devExtCount++] = VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME;
		devExts[devExtCount++] = VK_KHR_RAY_QUERY_EXTENSION_NAME;
		devExts[devExtCount++] = VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME;
	}

	VkDeviceCreateInfo dci = {};
	dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	dci.pNext = &enabled13;
	dci.queueCreateInfoCount = queueCount;
	dci.pQueueCreateInfos = queues;
	dci.enabledExtensionCount = devExtCount;
	dci.ppEnabledExtensionNames = devExts;
	dci.pEnabledFeatures = &enabled;

	if ( !vkCheck( vkCreateDevice( physical, &dci, NULL, &device ), "vkCreateDevice" ) ) {
		return false;
	}
	vkGetDeviceQueue( device, gfxFamily, 0, &gfxQueue );
	vkGetDeviceQueue( device, presentFamily, 0, &presentQueue );

	// R2: resolve the acceleration-structure entry points and the scratch-alignment limit.
	// If any entry point fails to resolve (broken loader/driver), drop the capability whole -
	// partial RT support is worse than none.
	if ( haveRayQuery ) {
		pfnGetAsBuildSizes = (PFN_vkGetAccelerationStructureBuildSizesKHR)
			vkGetDeviceProcAddr( device, "vkGetAccelerationStructureBuildSizesKHR" );
		pfnCreateAs = (PFN_vkCreateAccelerationStructureKHR)
			vkGetDeviceProcAddr( device, "vkCreateAccelerationStructureKHR" );
		pfnDestroyAs = (PFN_vkDestroyAccelerationStructureKHR)
			vkGetDeviceProcAddr( device, "vkDestroyAccelerationStructureKHR" );
		pfnCmdBuildAs = (PFN_vkCmdBuildAccelerationStructuresKHR)
			vkGetDeviceProcAddr( device, "vkCmdBuildAccelerationStructuresKHR" );
		pfnGetAsDeviceAddress = (PFN_vkGetAccelerationStructureDeviceAddressKHR)
			vkGetDeviceProcAddr( device, "vkGetAccelerationStructureDeviceAddressKHR" );
		if ( !pfnGetAsBuildSizes || !pfnCreateAs || !pfnDestroyAs || !pfnCmdBuildAs || !pfnGetAsDeviceAddress ) {
			common->Warning( "VK: acceleration-structure entry points failed to resolve - ray query disabled" );
			haveRayQuery = false;
		} else {
			VkPhysicalDeviceAccelerationStructurePropertiesKHR asProps = {};
			asProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
			VkPhysicalDeviceProperties2 props2 = {};
			props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
			props2.pNext = &asProps;
			vkGetPhysicalDeviceProperties2( physical, &props2 );
			if ( asProps.minAccelerationStructureScratchOffsetAlignment > 0 ) {
				asScratchAlignment = asProps.minAccelerationStructureScratchOffsetAlignment;
			}
		}
	}
	common->Printf( "VK: ray query %s\n", haveRayQuery
		? "available (KHR_acceleration_structure + KHR_ray_query)"
		: "not available - RT features disabled" );

	VmaAllocatorCreateInfo aci = {};
	aci.physicalDevice = physical;
	aci.device = device;
	aci.instance = instance;
	aci.vulkanApiVersion = VK_API_VERSION_1_4;
	// must match the enabled feature + the buffer usage bit (Phase 3.2b BDA); VMA needs this to
	// pass VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT / the alloc flag through to buffer creation.
	if ( haveBufferDeviceAddress ) {
		aci.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
	}
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
	// Clear any device-lost latch from a previous session so a vid_restart after a device
	// loss actually recovers (BeginFrame/EndFrame/etc. bail while vkDeviceDead is set).
	vkDeviceDead = false;
	vkCrashDumpWritten = false;
	{
		std::lock_guard<std::mutex> lock( vkDebugMsgMutex );
		vkDebugMsgHistory.clear();
	}

	if ( !CreateInstance() || !PickPhysicalDevice() || !CreateDeviceAndVma() ) {
		Shutdown();
		return false;
	}
	CreatePipelineCache();		// non-fatal: pipelines still build cold if this fails
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

	// Register developer console command to force a Vulkan crash-state dump into the save path.
	// Registered here (after successful backend bring-up) so cmdSystem is available.
	cmdSystem->AddCommand( "vk_dump_state", Vk_ForceCrashDump_f, CMD_FL_RENDERER, "Force a Vulkan crash-state dump to dudelog-crash-*.txt (dev)" );

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
		SavePipelineCache();		// persist this session's compiles before teardown
	}

	// FSR2 (R1/C2): the context owns device pipelines/images; free while the device lives
	Fsr2DestroyContext( true );

	// R2: acceleration structures + their backing buffers must die before VMA/device
	DestroyRtScene();

	// M6: ImGui device objects must die before the device. Normally sys_imgui
	// shuts down first (it calls ImGuiShutdown through the glue); this is the
	// safety net for partial-teardown orders.
	ImGuiShutdown();
	if ( imguiPass != VK_NULL_HANDLE ) {
		vkDestroyRenderPass( device, imguiPass, NULL );
		imguiPass = VK_NULL_HANDLE;
		imguiPassFormat = VK_FORMAT_UNDEFINED;
	}

	// Free the frame command buffers (via their pools) BEFORE the buffer/image
	// sweeps. After vkDeviceWaitIdle their GPU work is done, but they're still in
	// the recorded state, holding references to this level's persistent static
	// buffers; the validation layer only releases those on command-buffer *free*
	// (not reset), so destroying the buffers first intermittently trips
	// VUID-vkDestroyBuffer-buffer-00922 on a clean quit. DestroyM2Resources never
	// touches the frame slots, so this reorder is safe.
	DestroyFrameSlots();
	if ( device != VK_NULL_HANDLE ) {
		DestroyM2Resources();
	}
	DestroySceneTargets();
	DestroySwapchain( true );
	if ( diskPipelineCache != VK_NULL_HANDLE ) {
		vkDestroyPipelineCache( device, diskPipelineCache, NULL );
		diskPipelineCache = VK_NULL_HANDLE;
	}
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
VulkanBackend::GpuTimerReadback

Reads back slot's begin/end timestamp pair (recorded the last time this slot ran,
now guaranteed done by BeginFrame's fence wait — so no WAIT bit, no stall),
converts the tick delta to ms, and prints a running average once per second.
====================
*/
void VulkanBackend::GpuTimerReadback( int slot ) {
	if ( !gpuTimerBusy[slot] ) {
		return;
	}
	gpuTimerBusy[slot] = false;
	uint64_t stamps[2] = { 0, 0 };
	if ( vkGetQueryPoolResults( device, gpuTimerPool, 2 * slot, 2,
	         sizeof( stamps ), stamps, sizeof( uint64_t ), VK_QUERY_RESULT_64_BIT ) != VK_SUCCESS ) {
		return;		// VK_NOT_READY not expected post-fence; skip this sample
	}
	uint64_t delta = ( stamps[1] & gpuTimerMask ) - ( stamps[0] & gpuTimerMask );
	gpuTimeAccumMs += ( (double)delta * gpuTimerPeriodNs ) / 1000000.0;
	gpuTimeSamples++;
	unsigned int now = Sys_Milliseconds();
	if ( now - gpuTimeLastPrint >= 1000 ) {
		common->Printf( "VK GPU: %.2f ms (%d samples)\n",
		                gpuTimeAccumMs / gpuTimeSamples, gpuTimeSamples );
		gpuTimeAccumMs = 0.0;
		gpuTimeSamples = 0;
		gpuTimeLastPrint = now;
	}
}

/*
====================
VulkanBackend::BeginFrame
====================
*/
void VulkanBackend::BeginFrame( int windowWidth, int windowHeight ) {
	if ( vkDeviceDead ) {
		common->Warning( "VK: device is dead; skipping BeginFrame (use vid_restart)" );
		return;
	}
	if ( device == VK_NULL_HANDLE || frameOpen ) {
		return;
	}

	// Advance the NVML telemetry ring while the device is alive, so a crash dump can show the
	// clock / P-state / power trajectory leading into a device loss. Cheap and throttled to ~10 Hz;
	// only runs when the HUD or crash logging is on.
	if ( r_vkCrashLogging.GetBool() || r_vkDebugHud.GetBool() ) {
		VkSampleTelemetry();
	}
	FrameSlot &f = frames[frameIndex];
	vkWaitForFences( device, 1, &f.fence, VK_TRUE, UINT64_MAX );

	// FSR2 auto-reactive (R1/D): the opaque snapshot is one-frame data — a new frame
	// invalidates it until Fsr2CaptureOpaque runs again at this frame's translucent split
	fsr2OpaqueValid = false;

	// compute-lane self-test (Phase 1): one-shot on the cvar toggle. Runs on the synchronous
	// upload cb (waits idle) — a dev validation path, so gate it to the modified edge.
	if ( r_vkComputeTest.IsModified() ) {
		r_vkComputeTest.ClearModified();
		if ( r_vkComputeTest.GetBool() ) {
			ComputeSelfTest();
		}
	}
	// BDA self-test (Phase 3.2b): same one-shot-on-toggle idiom as the compute test above.
	if ( r_vkBdaTest.IsModified() ) {
		r_vkBdaTest.ClearModified();
		if ( r_vkBdaTest.GetBool() ) {
			BdaSelfTest();
		}
	}
	// ray-query self-test (R2): same one-shot-on-toggle idiom.
	if ( r_rayQueryTest.IsModified() ) {
		r_rayQueryTest.ClearModified();
		if ( r_rayQueryTest.GetBool() ) {
			RayQuerySelfTest();
		}
	}
	// BDA zfill consume (Phase 3.2b): load the manual-vertex-fetch zfill variant on first enable,
	// caching both the flat zfill handle (to identify the draw) and its BDA sibling. If the SPIR-V
	// pair is missing the variant stays 0 and the Draw path falls back to normal zfill.
	if ( r_vkBdaZfill.IsModified() ) {
		r_vkBdaZfill.ClearModified();
		if ( r_vkBdaZfill.GetInteger() != 0 && zfillBdaShaderHandle == 0 ) {
			zfillShaderHandle = LoadShader( "zfill" );
			zfillBdaShaderHandle = LoadShader( "zfill_bda" );		// mode 1
			zfillBatchShaderHandle = LoadShader( "zfill_batch" );	// mode 2
			if ( zfillBdaShaderHandle == 0 || zfillBatchShaderHandle == 0 ) {
				common->Warning( "VK: r_vkBdaZfill needs zfill_bda + zfill_batch .spv (shaders/spv) - some modes disabled" );
			}
		}
	}
	// r_vkBdaZfill firing report (once/sec, gated behind r_vkBdaVerbose so enabling the offload
	// stays silent): confirms the consume path is active. The counters hold the frame that just
	// ended; a non-zero "via device address"/"batched" means live.
	if ( r_vkBdaZfill.GetInteger() != 0 && r_vkBdaVerbose.GetBool() ) {
		unsigned int now = Sys_Milliseconds();
		if ( now - bdaZfillLastPrint >= 1000 ) {
			common->Printf( "VK BDA zfill: %d per-draw via device address, %d batched in %d indirect draws, %d fell back (last frame)\n",
			                bdaZfillDraws, bdaZfillBatched, bdaZfillBatchDraws, bdaZfillFallback );
			bdaZfillLastPrint = now;
		}
	}
	bdaZfillDraws = 0;
	bdaZfillFallback = 0;
	bdaZfillBatched = 0;
	bdaZfillBatchDraws = 0;

	// FSR2 bring-up self-test (R1/C0): one-shot on the cvar toggle.
	if ( r_fsr2Test.IsModified() ) {
		r_fsr2Test.ClearModified();
		if ( r_fsr2Test.GetBool() ) {
			Fsr2SelfTest();
		}
	}

	// MRT/format plumbing self-test (R1/A0): one-shot on the cvar toggle.
	if ( r_mrt3Test.IsModified() ) {
		r_mrt3Test.ClearModified();
		if ( r_mrt3Test.GetBool() ) {
			Mrt3SelfTest();
		}
	}

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

	// GPU timer (r_vkGpuTime): read back this slot's previous pair before the
	// reset wipes it, then — while active — reset the two queries (must be outside
	// a render pass; the cb has none open yet) and stamp the frame's start.
	gpuTimerActive = false;
	if ( gpuTimerSupported ) {
		GpuTimerReadback( frameIndex );
		if ( r_vkGpuTime.GetBool() ) {
			vkCmdResetQueryPool( f.cb, gpuTimerPool, 2 * frameIndex, 2 );
			vkCmdWriteTimestamp( f.cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, gpuTimerPool, 2 * frameIndex );
			gpuTimerActive = true;
		}
	}

	// M2: this slot's GPU work is fenced off — reset its rings (the
	// StreamGeneration contract) and its per-draw descriptor pool
	DrainRetiredRings( frameIndex );	// buffers replaced by GrowRing last time this slot ran
	DrainRetiredBuffers( false );		// age out static vertex/index blocks freed by the vertexCache
	DrainRetiredImages( frameIndex );	// capture/cinematic images replaced by RetireImage
	DrainRetiredTargets( frameIndex );	// shadow-map targets evicted by the shadow caches
	DrainRetiredAnimAs( false );		// R3.5: age out retired animated-caster BLASes (fence-safe destroy)
	if ( texturePool && !retiredTexSets[frameIndex].empty() ) {
		// texture sets invalidated ~FRAMES_IN_FLIGHT frames ago: this slot's fence
		// has passed, so nothing in flight still references them — free for reuse
		vkFreeDescriptorSets( device, texturePool, (uint32_t)retiredTexSets[frameIndex].size(),
			retiredTexSets[frameIndex].data() );
		retiredTexSets[frameIndex].clear();
	}
	if ( computePool && !retiredComputeSets[frameIndex].empty() ) {
		// per-dispatch compute sets from this slot's previous frame; the fence above
		// guarantees that frame's work is done, so they're safe to free (Phase 1).
		vkFreeDescriptorSets( device, computePool, (uint32_t)retiredComputeSets[frameIndex].size(),
			retiredComputeSets[frameIndex].data() );
		retiredComputeSets[frameIndex].clear();
	}
	uboRing[frameIndex].offset = 0;
	vertRing[frameIndex].offset = 0;
	idxRing[frameIndex].offset = 0;
	stagingRing[frameIndex].offset = 0;
	indirectRing[frameIndex].offset = 0;
	streamGen++;
	if ( framePool[frameIndex] ) {
		vkResetDescriptorPool( device, framePool[frameIndex], 0 );
	}

	// R3 animated casters (monsters): rebuild the slot's combined monster BLAS FIRST, so the
	// TLAS build below reads the current-pose geometry. RecordFrameDynBlasBuild ends with an
	// AS-write->AS-read barrier ordering it against that TLAS build. Fresh slots were primed
	// synchronously in UpdateDynamicGeometry (rtDynPendingSlot -1 there); only steady slots
	// rebuild here. A stale arm for a skipped frame is dropped.
	if ( rtDynPendingSlot == frameIndex && rtFrameDynBlas[frameIndex] != VK_NULL_HANDLE ) {
		RecordFrameDynBlasBuild( f.cb, frameIndex );
		rtDynPendingSlot = -1;
	} else if ( rtDynPendingSlot >= 0 ) {
		rtDynPendingSlot = -1;
	}

	// R3 per-frame TLAS (movers): record the armed instance rebuild AHEAD of every draw
	// (cb open, no render pass yet - the same pre-scene window the skin jobs use), then
	// barrier the AS-build write against the fragment-shader ray reads of the mode-4
	// interaction draws. An armed slot that is not this frame's (a skipped/lost frame)
	// is dropped: its data was never built, so the address falls back to the static TLAS.
	if ( rtPendingSlot == frameIndex && rtFrameTlas[frameIndex] != VK_NULL_HANDLE ) {
		RecordFrameTlasBuild( f.cb, frameIndex );
		rtPendingSlot = -1;
	} else if ( rtPendingSlot >= 0 ) {
		rtPendingSlot = -1;
		rtCurrentAddr = 0;
		rtCurrentGeoAddr = 0;
	}

	boundPipeline = VK_NULL_HANDLE;
	boundTexKey = 0;
	boundTexSet = VK_NULL_HANDLE;
	bindlessBoundThisCb = false;	// RR4: rebind the bindless set at the first draw of this frame's cb
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
	if ( vkDeviceDead ) {
		common->Warning( "VK: device is dead; skipping BeginPass" );
		return;
	}
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
	if ( vkDeviceDead ) {
		common->Warning( "VK: device is dead; skipping EndPass" );
		return;
	}
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
	if ( vkDeviceDead ) {
		// If the device is dead, gracefully abort EndFrame without issuing any Vulkan calls.
		common->Warning( "VK: device is dead; skipping EndFrame (use vid_restart)" );
		// ensure consistent state for the rest of the engine
		frameOpen = false;
		skipFrame = false;
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

	// GPU timer (r_vkGpuTime): stamp the frame's end just before closing the cb;
	// the pair reads back at this slot's next BeginFrame. BOTTOM_OF_PIPE so the
	// stamp waits for all prior GPU work this frame to finish.
	if ( gpuTimerActive ) {
		vkCmdWriteTimestamp( f.cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, gpuTimerPool, 2 * frameIndex + 1 );
		gpuTimerBusy[frameIndex] = true;
		gpuTimerActive = false;
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

	// GPU timer query pool (r_vkGpuTime). Gated on the graphics queue's
	// timestampValidBits; timestampPeriod is ns/tick. If unsupported the cvar just
	// stays inert (like the GL3 timer without the query entry points). Recreated
	// here so it re-arms after vid_restart too.
	uint32_t validBits = 0;
	{
		uint32_t qfCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties( physical, &qfCount, NULL );
		std::vector<VkQueueFamilyProperties> qfp( qfCount );
		vkGetPhysicalDeviceQueueFamilyProperties( physical, &qfCount, qfp.data() );
		if ( gfxFamily < qfCount ) { validBits = qfp[gfxFamily].timestampValidBits; }
	}
	gpuTimerSupported = physProps.limits.timestampPeriod > 0.0f && validBits > 0;
	if ( gpuTimerSupported ) {
		gpuTimerPeriodNs = (double)physProps.limits.timestampPeriod;
		gpuTimerMask = ( validBits >= 64 ) ? ~0ULL : ( ( 1ULL << validBits ) - 1 );
		VkQueryPoolCreateInfo qpi = {};
		qpi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
		qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
		qpi.queryCount = 2 * FRAMES_IN_FLIGHT;		// begin+end per slot
		if ( !vkCheck( vkCreateQueryPool( device, &qpi, NULL, &gpuTimerPool ), "vkCreateQueryPool(timer)" ) ) {
			gpuTimerPool = VK_NULL_HANDLE;
			gpuTimerSupported = false;
		}
	}
	for ( int i = 0; i < FRAMES_IN_FLIGHT; i++ ) { gpuTimerBusy[i] = false; }
	gpuTimerActive = false;

	// host-visible persistently-mapped rings, one set per frame slot — the
	// slot's fence wait in BeginFrame guarantees the GPU is done with them
	// before offsets reset (the StreamGeneration contract)
	struct ringSetup_t {
		RingBuf *ring;
		int size;
		VkBufferUsageFlags usage;
	};
	for ( int slot = 0; slot < FRAMES_IN_FLIGHT; slot++ ) {
		const ringSetup_t setups[5] = {
			{ &uboRing[slot],  UBO_RING_SIZE,  VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT },
			{ &vertRing[slot], VERT_RING_SIZE, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT },
			{ &idxRing[slot],  IDX_RING_SIZE,  VK_BUFFER_USAGE_INDEX_BUFFER_BIT },
			{ &stagingRing[slot], STAGING_RING_SIZE, VK_BUFFER_USAGE_TRANSFER_SRC_BIT },
			{ &indirectRing[slot], INDIRECT_RING_SIZE, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT },
		};
		for ( int i = 0; i < 5; i++ ) {
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
			bufferAllocs.push_back( NULL );		// ring-owned; not a persistent buffer
			bufferMapped.push_back( NULL );
			bufferAddr.push_back( 0 );			// ring buffers aren't BDA-addressable
			setups[i].ring->handle = (BufferHandle)bufferTable.size();
		}
	}

	// tess stages read the same UBO (matrices, tess params, light origin) and,
	// for displacement, the same texture set; fold the bits in only when the
	// device supports tessellation (else the flags reference an unusable stage).
	const VkShaderStageFlags tessStages = haveTessellation
		? ( VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT )
		: 0;

	// set 0: one dynamic-offset UBO reused for every draw
	{
		VkDescriptorSetLayoutBinding b = {};
		b.binding = 0;
		b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
		b.descriptorCount = 1;
		b.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | tessStages;
		VkDescriptorSetLayoutCreateInfo li = {};
		li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		li.bindingCount = 1;
		li.pBindings = &b;
		if ( !vkCheck( vkCreateDescriptorSetLayout( device, &li, NULL, &setLayoutUbo ), "vkCreateDescriptorSetLayout(ubo)" ) ) {
			return false;
		}
	}
	// set 1: combined image samplers, units 0-7, shadow cube at 8, SSAO at 9,
	// occlusion map at 10, parallax height map at 11 (interaction declares 9/10/11;
	// dummies bound where a shader doesn't sample them)
	{
		VkDescriptorSetLayoutBinding b[13] = {};
		for ( int i = 0; i < 13; i++ ) {
			b[i].binding = (uint32_t)i;
			b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			b[i].descriptorCount = 1;
			b[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | tessStages;
		}
		VkDescriptorSetLayoutCreateInfo li = {};
		li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		li.bindingCount = 13;
		li.pBindings = b;
		if ( !vkCheck( vkCreateDescriptorSetLayout( device, &li, NULL, &setLayoutTex ), "vkCreateDescriptorSetLayout(tex)" ) ) {
			return false;
		}
	}
	// set 2 (RR4 bindless materials, docs/rtx-reflections.md): a variable-count array of every
	// resident texture (COMBINED_IMAGE_SAMPLER), indexed directly by ImageHandle-1. UPDATE_AFTER_BIND
	// lets CreateTexture/DestroyImage rewrite a slot while earlier frames are still in flight;
	// PARTIALLY_BOUND means only the slots we actually register need hold a valid descriptor. Built
	// only when the device reports descriptor indexing (guaranteed on our 1.4 RT floor); otherwise the
	// pipeLayout stays 2-set and the RT reflection shader keeps its RR3 per-material average colour.
	if ( haveDescriptorIndexing ) {
		bindlessCapacity = 8192;			// >> the ~2k textures a Doom 3 level resides; unwritten slots are free
		VkDescriptorSetLayoutBinding bb = {};
		bb.binding = 0;
		bb.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		bb.descriptorCount = bindlessCapacity;
		bb.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
		VkDescriptorBindingFlags bfl = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT
			| VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT
			| VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;
		VkDescriptorSetLayoutBindingFlagsCreateInfo bfi = {};
		bfi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
		bfi.bindingCount = 1;
		bfi.pBindingFlags = &bfl;
		VkDescriptorSetLayoutCreateInfo li = {};
		li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		li.pNext = &bfi;
		li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
		li.bindingCount = 1;
		li.pBindings = &bb;
		if ( !vkCheck( vkCreateDescriptorSetLayout( device, &li, NULL, &setLayoutBindless ), "vkCreateDescriptorSetLayout(bindless)" ) ) {
			return false;
		}
		VkDescriptorPoolSize bps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, bindlessCapacity };
		VkDescriptorPoolCreateInfo bpci = {};
		bpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		bpci.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
		bpci.maxSets = 1;
		bpci.poolSizeCount = 1;
		bpci.pPoolSizes = &bps;
		if ( !vkCheck( vkCreateDescriptorPool( device, &bpci, NULL, &bindlessPool ), "vkCreateDescriptorPool(bindless)" ) ) {
			return false;
		}
		uint32_t varCount = bindlessCapacity;
		VkDescriptorSetVariableDescriptorCountAllocateInfo vci = {};
		vci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO;
		vci.descriptorSetCount = 1;
		vci.pDescriptorCounts = &varCount;
		VkDescriptorSetAllocateInfo bai = {};
		bai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		bai.pNext = &vci;
		bai.descriptorPool = bindlessPool;
		bai.descriptorSetCount = 1;
		bai.pSetLayouts = &setLayoutBindless;
		if ( !vkCheck( vkAllocateDescriptorSets( device, &bai, &bindlessSet ), "vkAllocateDescriptorSets(bindless)" ) ) {
			return false;
		}
	}
	{
		VkDescriptorSetLayout sets[3] = { setLayoutUbo, setLayoutTex, setLayoutBindless };
		VkPipelineLayoutCreateInfo pli = {};
		pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		pli.setLayoutCount = haveDescriptorIndexing ? 3 : 2;	// RR4: set 2 = bindless textures when present
		pli.pSetLayouts = sets;
		// Phase 3.2b: a small vertex-stage push-constant range carries a buffer_device_address
		// (idDrawVert*) for the BDA manual-vertex-fetch zfill variant (r_vkBdaZfill). Backward-
		// compatible — shaders that declare no push_constant simply never read it (128B floor).
		VkPushConstantRange pcr = {};
		pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
		pcr.offset = 0;
		pcr.size = 16;
		pli.pushConstantRangeCount = 1;
		pli.pPushConstantRanges = &pcr;
		if ( !vkCheck( vkCreatePipelineLayout( device, &pli, NULL, &pipeLayout ), "vkCreatePipelineLayout" ) ) {
			return false;
		}
	}

	// compute lane (docs/gpu-offload-plan.md Phase 1): one set of 8 STORAGE_BUFFER bindings
	// (ComputeArgs::storage[]) + a 128-byte push-constant range for params, a dedicated
	// pipeline layout, and a FREE-able pool for per-dispatch sets. All COMPUTE-stage, kept
	// fully independent of the 2-set graphics layout above.
	{
		VkDescriptorSetLayoutBinding sb[8] = {};
		for ( int i = 0; i < 8; i++ ) {
			sb[i].binding = (uint32_t)i;
			sb[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			sb[i].descriptorCount = 1;
			sb[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		}
		VkDescriptorSetLayoutCreateInfo li = {};
		li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		li.bindingCount = 8;
		li.pBindings = sb;
		if ( !vkCheck( vkCreateDescriptorSetLayout( device, &li, NULL, &setLayoutCompute ), "vkCreateDescriptorSetLayout(compute)" ) ) {
			return false;
		}
		VkPushConstantRange pcr = {};
		pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		pcr.offset = 0;
		pcr.size = 128;			// >= the guaranteed maxPushConstantsSize floor
		VkPipelineLayoutCreateInfo pli = {};
		pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		pli.setLayoutCount = 1;
		pli.pSetLayouts = &setLayoutCompute;
		pli.pushConstantRangeCount = 1;
		pli.pPushConstantRanges = &pcr;
		if ( !vkCheck( vkCreatePipelineLayout( device, &pli, NULL, &computePipeLayout ), "vkCreatePipelineLayout(compute)" ) ) {
			return false;
		}
		VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, MAX_FRAME_SETS * 8 };
		VkDescriptorPoolCreateInfo pci = {};
		pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		pci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
		pci.maxSets = MAX_FRAME_SETS;
		pci.poolSizeCount = 1;
		pci.pPoolSizes = &ps;
		if ( !vkCheck( vkCreateDescriptorPool( device, &pci, NULL, &computePool ), "vkCreateDescriptorPool(compute)" ) ) {
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
		VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_FRAME_SETS * 13 * 4 };
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

	if ( gpuTimerPool != VK_NULL_HANDLE ) {
		vkDestroyQueryPool( device, gpuTimerPool, NULL );
		gpuTimerPool = VK_NULL_HANDLE;
	}

	DestroyMergeNormal();		// SSAO normal-merge image/pass/fb (before the imageTable sweep)

	for ( auto &kv : pipelineCache ) {
		vkDestroyPipeline( device, kv.second, NULL );
	}
	pipelineCache.clear();
	boundPipeline = VK_NULL_HANDLE;

	// compute pipelines (Phase 1); the map also holds cached VK_NULL_HANDLEs for
	// failed shaders — skip those.
	for ( auto &kv : computePipelineCache ) {
		if ( kv.second ) { vkDestroyPipeline( device, kv.second, NULL ); }
	}
	computePipelineCache.clear();

	for ( size_t i = 0; i < shaderTable.size(); i++ ) {
		if ( shaderTable[i].vert ) { vkDestroyShaderModule( device, shaderTable[i].vert, NULL ); }
		if ( shaderTable[i].frag ) { vkDestroyShaderModule( device, shaderTable[i].frag, NULL ); }
		if ( shaderTable[i].tesc ) { vkDestroyShaderModule( device, shaderTable[i].tesc, NULL ); }
		if ( shaderTable[i].tese ) { vkDestroyShaderModule( device, shaderTable[i].tese, NULL ); }
		if ( shaderTable[i].comp ) { vkDestroyShaderModule( device, shaderTable[i].comp, NULL ); }
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
	for ( int i = 0; i < FRAMES_IN_FLIGHT; i++ ) { retiredTexSets[i].clear(); retiredComputeSets[i].clear(); }
	if ( texturePool ) { vkDestroyDescriptorPool( device, texturePool, NULL ); texturePool = VK_NULL_HANDLE; }
	if ( persistentPool ) { vkDestroyDescriptorPool( device, persistentPool, NULL ); persistentPool = VK_NULL_HANDLE; }
	if ( pipeLayout )     { vkDestroyPipelineLayout( device, pipeLayout, NULL ); pipeLayout = VK_NULL_HANDLE; }
	if ( setLayoutUbo )   { vkDestroyDescriptorSetLayout( device, setLayoutUbo, NULL ); setLayoutUbo = VK_NULL_HANDLE; }
	if ( setLayoutTex )   { vkDestroyDescriptorSetLayout( device, setLayoutTex, NULL ); setLayoutTex = VK_NULL_HANDLE; }
	// RR4 bindless materials: the pool owns bindlessSet, so destroying it frees the set
	if ( bindlessPool )      { vkDestroyDescriptorPool( device, bindlessPool, NULL ); bindlessPool = VK_NULL_HANDLE; bindlessSet = VK_NULL_HANDLE; }
	if ( setLayoutBindless ) { vkDestroyDescriptorSetLayout( device, setLayoutBindless, NULL ); setLayoutBindless = VK_NULL_HANDLE; }
	// compute lane (Phase 1)
	if ( computePool )       { vkDestroyDescriptorPool( device, computePool, NULL ); computePool = VK_NULL_HANDLE; }
	if ( computePipeLayout ) { vkDestroyPipelineLayout( device, computePipeLayout, NULL ); computePipeLayout = VK_NULL_HANDLE; }
	if ( setLayoutCompute )  { vkDestroyDescriptorSetLayout( device, setLayoutCompute, NULL ); setLayoutCompute = VK_NULL_HANDLE; }

	if ( uploadFence ) { vkDestroyFence( device, uploadFence, NULL ); uploadFence = VK_NULL_HANDLE; }
	if ( uploadPool )  { vkDestroyCommandPool( device, uploadPool, NULL ); uploadPool = VK_NULL_HANDLE; uploadCb = VK_NULL_HANDLE; }

	DrainRetiredBuffers( true );			// device idle: destroy every retired block now
	for ( int slot = 0; slot < FRAMES_IN_FLIGHT; slot++ ) {
		DrainRetiredRings( slot );		// device is idle here
		DrainRetiredImages( slot );
		RingBuf *rings[5] = { &uboRing[slot], &vertRing[slot], &idxRing[slot], &stagingRing[slot], &indirectRing[slot] };
		for ( int i = 0; i < 5; i++ ) {
			if ( rings[i]->buffer ) {
				vmaDestroyBuffer( vma, rings[i]->buffer, rings[i]->alloc );
			}
			*rings[i] = RingBuf();
		}
	}
	// free any persistent buffers the vertexCache didn't explicitly DestroyBuffer
	// (e.g. engine shutdown, where blocks are abandoned rather than purged). Ring
	// slots have a NULL alloc here — already destroyed above — so they're skipped.
	for ( size_t i = 0; i < bufferAllocs.size(); i++ ) {
		if ( bufferAllocs[i] ) {
			vmaDestroyBuffer( vma, bufferTable[i], bufferAllocs[i] );
		}
	}
	bufferTable.clear();
	bufferAllocs.clear();
	bufferMapped.clear();
	bufferAddr.clear();
	// the mode-2 batch buffers lived in the table just cleared; drop the stale handles so
	// DrawZfillBatch reallocates rather than resolving a reused slot (Phase 3.2b).
	for ( int i = 0; i < FRAMES_IN_FLIGHT; i++ ) {
		batchObjBuf[i] = 0;
		batchCmdBuf[i] = 0;
		batchCapacity[i] = 0;
	}
	freeBufferSlots.clear();
	ringOverflowWarned = false;
	framePoolWarned = false;

	IR_Purge();
}

/*
====================
VulkanBackend::CreatePipelineCache

Create the driver's VkPipelineCache, seeded from the blob written at the last
Shutdown so first-use pipeline compiles are reused across runs. Non-fatal: on
any failure diskPipelineCache stays NULL and pipelines simply build cold (the
VK_NULL_HANDLE path). The Vulkan spec guarantees safe handling of incompatible
initial data, but we header-check vendor/device/UUID first so a GPU or driver
swap starts fresh (and logs it) instead of handing the driver stale bytes.
====================
*/
void VulkanBackend::CreatePipelineCache() {
	diskPipelineCache = VK_NULL_HANDLE;
	if ( device == VK_NULL_HANDLE ) {
		return;
	}

	void *blob = NULL;
	const int blobLen = fileSystem->ReadFile( "vkpipelinecache.bin", &blob, NULL );
	const void *initData = NULL;
	size_t      initSize = 0;
	// VkPipelineCacheHeaderVersionOne = uint32 headerSize, uint32 version,
	// uint32 vendorID, uint32 deviceID, uint8 uuid[VK_UUID_SIZE]
	if ( blob && blobLen >= (int)( 16 + VK_UUID_SIZE ) ) {
		const uint8_t *h = (const uint8_t *)blob;
		uint32_t hdrVer = 0, vendorID = 0, deviceID = 0;
		memcpy( &hdrVer,   h + 4,  4 );
		memcpy( &vendorID, h + 8,  4 );
		memcpy( &deviceID, h + 12, 4 );
		const bool match = hdrVer == VK_PIPELINE_CACHE_HEADER_VERSION_ONE
			&& vendorID == physProps.vendorID
			&& deviceID == physProps.deviceID
			&& memcmp( h + 16, physProps.pipelineCacheUUID, VK_UUID_SIZE ) == 0;
		if ( match ) {
			initData = blob;
			initSize = (size_t)blobLen;
		} else {
			common->Printf( "VK: on-disk pipeline cache is for a different device/driver - starting fresh\n" );
		}
	}

	VkPipelineCacheCreateInfo pci = {};
	pci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
	pci.initialDataSize = initSize;
	pci.pInitialData = initData;
	if ( !vkCheck( vkCreatePipelineCache( device, &pci, NULL, &diskPipelineCache ), "vkCreatePipelineCache" ) ) {
		diskPipelineCache = VK_NULL_HANDLE;
	} else if ( initSize ) {
		common->Printf( "VK: pipeline cache seeded from disk (%d KB)\n", blobLen / 1024 );
	}
	if ( blob ) {
		fileSystem->FreeFile( blob );		// vkCreatePipelineCache copied the initial data
	}
}

/*
====================
VulkanBackend::SavePipelineCache

Write the driver pipeline cache back to fs_savepath so the next run (and each
vid_restart) reuses this session's compiles. Called with the device idle.
====================
*/
void VulkanBackend::SavePipelineCache() {
	if ( device == VK_NULL_HANDLE || diskPipelineCache == VK_NULL_HANDLE ) {
		return;
	}
	size_t size = 0;
	if ( vkGetPipelineCacheData( device, diskPipelineCache, &size, NULL ) != VK_SUCCESS || size == 0 ) {
		return;
	}
	void *data = R_StaticAlloc( (int)size );
	if ( !data ) {
		return;
	}
	if ( vkGetPipelineCacheData( device, diskPipelineCache, &size, data ) == VK_SUCCESS && size > 0 ) {
		fileSystem->WriteFile( "vkpipelinecache.bin", data, (int)size );
	}
	R_StaticFree( data );
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
	bufferAllocs.push_back( NULL );		// ring-owned; not a persistent buffer
	bufferMapped.push_back( NULL );
	bufferAddr.push_back( 0 );			// ring buffers aren't BDA-addressable
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
VulkanBackend::CreateBuffer

Persistent static geometry buffer (the vertexCache's per-surface vertex/index
blocks). Host-visible + persistently mapped, so the upload is a plain memcpy
with no staging copy or queue submit — thousands of these are created at level
load, and a submit+fence-wait per block would serialize the whole load. On
discrete GPUs this lands in system RAM (or the ReBAR window), which is fine for
D3-sized geometry and still a large win over re-streaming every visible surface
into the ring every frame. Returns 0 on failure; callers fall back to the ring.
====================
*/
BufferHandle VulkanBackend::CreateBuffer( BufferUsage usage, int size, const void *data ) {
	if ( size <= 0 ) {
		return 0;
	}
	VkBufferUsageFlags usageBits;
	switch ( usage ) {
		case BU_INDEX:   usageBits = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;  break;
		case BU_UNIFORM: usageBits = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT; break;
		// compute storage buffer (Phase 1). TRANSFER_SRC/DST let a later device-local
		// variant stage seed/readback; harmless on the host-visible buffer below.
		// INDIRECT lets a compute pass write a VkDrawIndexedIndirectCommand[] here that
		// DrawIndexedIndirect consumes (Phase 3 seed); harmless on the host-visible buffer.
		case BU_STORAGE: usageBits = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
		                           | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT
		                           | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
		                           | VK_BUFFER_USAGE_TRANSFER_DST_BIT; break;
		// skinning output: compute writes it (STORAGE), the draw passes fetch it as a
		// vertex buffer (VERTEX). TRANSFER bits let CreateBuffer seed the static
		// st/color fields and a later device-local variant stage. (Phase 2.)
		case BU_SKIN:    usageBits = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
		                           | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
		                           | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
		                           | VK_BUFFER_USAGE_TRANSFER_DST_BIT; break;
		default:         usageBits = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT; break;
	}
	// BDA (Phase 3.2b): storage buffers (compute) and persistent vertex/index buffers (the world-static
	// geometry the depth-prepass consume manually fetches, r_vkBdaZfill — the batched path fetches BOTH
	// vertices and indices via device-address pointers) may be dereferenced by a shader pointer. Only
	// legal when the allocator carries the BDA flag (else vmaCreateBuffer fails validation), so gate on
	// the same capability. Ring buffers aren't created here, so they stay address-less and the BDA zfill
	// path falls back for streamed/skinned surfaces.
	if ( ( usage == BU_STORAGE || usage == BU_VERTEX || usage == BU_INDEX ) && haveBufferDeviceAddress ) {
		usageBits |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
	}
	// R3.5 animated BLAS: the GPU-skinned vertex output (BU_SKIN) and each surface's persistent
	// static index buffer (BU_INDEX) feed a per-entity ray-tracing BLAS by device address, so they
	// need SHADER_DEVICE_ADDRESS (BU_SKIN lacks it above) plus ACCELERATION_STRUCTURE_BUILD_INPUT.
	// RR5 (RT world reflections): the static world's ambient VERTEX cache (BU_VERTEX) also feeds a
	// per-surface world BLAS by device address, so it needs the AS-input bit too. The AS-input usage
	// bit is only legal when VK_KHR_acceleration_structure is enabled, so gate on haveRayQuery (which
	// implies both that extension and buffer_device_address). Inert on non-RT devices; on RT hardware
	// these are extra usage bits with no behavior change until a BLAS actually consumes the buffers.
	if ( haveRayQuery && ( usage == BU_SKIN || usage == BU_INDEX || usage == BU_VERTEX ) ) {
		usageBits |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
		           | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
	}

	VkBufferCreateInfo bci = {};
	bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bci.size = (VkDeviceSize)size;
	bci.usage = usageBits;
	bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VmaAllocationCreateInfo aci = {};
	aci.usage = VMA_MEMORY_USAGE_AUTO;
	// storage buffers are read back on the host (ReadBuffer), so hint RANDOM access to keep
	// them in cached memory; everything else is write-only-from-CPU (SEQUENTIAL_WRITE lets the
	// allocator pick write-combined). A wrong hint doesn't break correctness on coherent memory
	// but would make storage read-back pathologically slow on a write-combined heap.
	aci.flags = ( usage == BU_STORAGE ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT
	                                  : VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT )
	          | VMA_ALLOCATION_CREATE_MAPPED_BIT;
	aci.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	VkBuffer buf = VK_NULL_HANDLE;
	VmaAllocation alloc = NULL;
	VmaAllocationInfo info = {};
	if ( !vkCheck( vmaCreateBuffer( vma, &bci, &aci, &buf, &alloc, &info ), "vmaCreateBuffer(static)" ) ) {
		return 0;
	}
	if ( data ) {
		memcpy( info.pMappedData, data, (size_t)size );
	}

	// cache the device address now (BDA, Phase 3.2b): a buffer's address is fixed for its
	// lifetime, and only buffers created with the usage bit above may be queried.
	uint64_t addr = 0;
	if ( usageBits & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT ) {
		VkBufferDeviceAddressInfo bai = {};
		bai.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
		bai.buffer = buf;
		addr = (uint64_t)vkGetBufferDeviceAddress( device, &bai );
	}

	// reuse a slot freed by DestroyBuffer, else grow the table (keeping all
	// parallel vectors in lockstep with bufferTable)
	uint32_t idx;
	if ( !freeBufferSlots.empty() ) {
		idx = freeBufferSlots.back();
		freeBufferSlots.pop_back();
		bufferTable[idx] = buf;
		bufferAllocs[idx] = alloc;
		bufferMapped[idx] = (byte *)info.pMappedData;
		bufferAddr[idx] = addr;
	} else {
		idx = (uint32_t)bufferTable.size();
		bufferTable.push_back( buf );
		bufferAllocs.push_back( alloc );
		bufferMapped.push_back( (byte *)info.pMappedData );
		bufferAddr.push_back( addr );
	}
	return (BufferHandle)( idx + 1 );
}

void VulkanBackend::UpdateBuffer( BufferHandle b, int offset, int size, const void *data ) {
	if ( b < 1 || b > (BufferHandle)bufferMapped.size() || !data || size <= 0 ) {
		return;
	}
	byte *mapped = bufferMapped[b - 1];
	if ( mapped ) {		// persistent (host-visible) buffer; coherent, no flush needed
		memcpy( mapped + offset, data, (size_t)size );
	}
}

void VulkanBackend::DestroyBuffer( BufferHandle b ) {
	if ( b < 1 || b > (BufferHandle)bufferTable.size() ) {
		return;
	}
	uint32_t idx = (uint32_t)( b - 1 );
	if ( bufferAllocs[idx] ) {		// only persistent buffers are ours to destroy
		// defer the vmaDestroy until in-flight frames that may reference it retire
		retiredBuffers.push_back( { bufferTable[idx], bufferAllocs[idx], FRAMES_IN_FLIGHT + 1 } );
		// the slot/handle is free to reuse now: already-recorded command buffers
		// hold the VkBuffer by value, and no new draw references this handle (the
		// vertexCache cleared block->vbo alongside this call).
		bufferTable[idx] = VK_NULL_HANDLE;
		bufferAllocs[idx] = NULL;
		bufferMapped[idx] = NULL;
		bufferAddr[idx] = 0;
		freeBufferSlots.push_back( idx );
	}
}

// Synchronous readback of a buffer (docs/gpu-offload-plan.md Phase 1). Phase-1 storage
// buffers are host-visible+coherent+mapped, so once the GPU is idle the compute writes are
// visible to the mapped pointer and this is a plain memcpy. A device-local storage buffer
// (bufferMapped == NULL) needs a TRANSFER_SRC -> staging -> map copy — that's the Phase-2
// extension; return false here so callers know the fast path wasn't taken. Stalls the GPU
// (vkQueueWaitIdle) — a dev/validation path, never a per-frame call.
bool VulkanBackend::ReadBuffer( BufferHandle b, void *dst, int size ) {
	if ( device == VK_NULL_HANDLE || b < 1 || b > (BufferHandle)bufferMapped.size() || !dst || size <= 0 ) {
		return false;
	}
	byte *mapped = bufferMapped[b - 1];
	if ( !mapped ) {
		return false;		// device-local (no host mapping) — Phase-2 staging readback
	}
	vkQueueWaitIdle( gfxQueue );	// all GPU writes complete + (coherent) visible to the host
	memcpy( dst, mapped, (size_t)size );
	return true;
}

// Called once per BeginFrame (after the slot's fence wait): age every retired
// buffer and destroy those whose TTL has elapsed. `force` (device idle at
// teardown) destroys them all regardless of TTL.
void VulkanBackend::DrainRetiredBuffers( bool force ) {
	for ( size_t i = 0; i < retiredBuffers.size(); ) {
		if ( force || --retiredBuffers[i].ttl <= 0 ) {
			vmaDestroyBuffer( vma, retiredBuffers[i].buffer, retiredBuffers[i].alloc );
			retiredBuffers[i] = retiredBuffers.back();
			retiredBuffers.pop_back();
		} else {
			i++;
		}
	}
}

/*
====================
VulkanBackend::LoadShader

<name> -> shaders/spv/<name>.vert.spv + .frag.spv, via the VFS with the same
build-tree dev fallback the GLSL loader uses (DUDE_SHADER_SPV_DIR).
====================
*/
// generated at build time by shaders/embed_shaders.py (shaders_embedded.cpp)
extern "C" bool Dude_GetEmbeddedShader( const char *name, const unsigned char **data, int *len );

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

	// embedded table: the always-present fallback so a bare binary renders
	const unsigned char *edata;
	int elen;
	if ( Dude_GetEmbeddedShader( va( "spv/%s", fileName ), &edata, &elen ) ) {
		out.assign( edata, edata + elen );
		return true;
	}
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

	// optional tessellation pair (DUDE tessellation): a shader gets a tess
	// variant only if BOTH .tesc.spv and .tese.spv exist and the device supports
	// tessellation. Absent = normal vert+frag shader (not an error). If one half
	// is present without the other, treat it as no tess variant.
	if ( ok && haveTessellation ) {
		std::vector<byte> tescSpv, teseSpv;
		if ( VK_ReadSpv( va( "%s.tesc.spv", name ), tescSpv )
		  && VK_ReadSpv( va( "%s.tese.spv", name ), teseSpv ) ) {
			mi.codeSize = tescSpv.size();
			mi.pCode = (const uint32_t *)tescSpv.data();
			bool tok = vkCheck( vkCreateShaderModule( device, &mi, NULL, &rec.tesc ), va( "vkCreateShaderModule(%s.tesc)", name ) );
			mi.codeSize = teseSpv.size();
			mi.pCode = (const uint32_t *)teseSpv.data();
			tok = tok && vkCheck( vkCreateShaderModule( device, &mi, NULL, &rec.tese ), va( "vkCreateShaderModule(%s.tese)", name ) );
			if ( !tok ) {
				if ( rec.tesc ) { vkDestroyShaderModule( device, rec.tesc, NULL ); rec.tesc = VK_NULL_HANDLE; }
				if ( rec.tese ) { vkDestroyShaderModule( device, rec.tese, NULL ); rec.tese = VK_NULL_HANDLE; }
			}
		}
	}

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
VulkanBackend::CreateShaderFromGlsl

Runtime GLSL->SPIR-V for custom (mod) ARB material stages: arb::ToGlsl emits
backend-neutral GLSL (SAMPLER_BINDING/VARY/UBO_BINDING macros, #include
"arbparams.glsl"), which this compiles for Vulkan exactly the way the offline
compile_spv.py builds the builtins — prepend prelude.vk.glsl, inject
`invariant gl_Position;` for the vertex stage, textually resolve #include,
compile with shaderc (target vulkan1.4). Cached in shaderTable by name so a
material's program is compiled once. Without DUDE_HAVE_SHADERC this returns 0
and the caller degrades the stage (SK_SKIP) — the pre-shaderc behaviour.
====================
*/
#ifdef DUDE_HAVE_SHADERC
// Read a shader source file (prelude / #include target) the same way the GL3
// loader does: VFS "shaders/<file>" first (moddable), then the source tree.
static bool VK_ReadShaderSource( const char *fileName, idStr &out ) {
	void *buf = NULL;
	int len = fileSystem->ReadFile( va( "shaders/%s", fileName ), &buf, NULL );
	if ( len >= 0 && buf ) {
		out.Clear();
		out.Append( (const char *)buf, len );
		fileSystem->FreeFile( buf );
		return true;
	}
#ifdef DUDE_SHADER_SOURCE_DIR
	FILE *f = fopen( va( "%s/%s", DUDE_SHADER_SOURCE_DIR, fileName ), "rb" );
	if ( f ) {
		fseek( f, 0, SEEK_END );
		long size = ftell( f );
		fseek( f, 0, SEEK_SET );
		if ( size > 0 ) {
			char *text = (char *)malloc( size );
			if ( text && fread( text, 1, size, f ) == (size_t)size ) {
				out.Clear();
				out.Append( text, size );
				free( text );
				fclose( f );
				return true;
			}
			free( text );
		}
		fclose( f );
	}
#endif
	return false;
}

// Textually resolve #include "file" in an in-memory body, recursively —
// mirrors compile_spv.py's expand_includes and the GL3 loader, so a
// runtime-compiled transpiled program sees the same arbparams.glsl the offline
// builtins do (the driver/shaderc never sees the #include directive itself).
static bool VK_ExpandIncludes( const char *body, idStr &out, int depth ) {
	if ( depth > 8 ) {
		common->Warning( "VK shaderc: #include depth > 8 (cycle?)" );
		return false;
	}
	const char *p = body;
	while ( *p ) {
		const char *nl = strchr( p, '\n' );
		const char *lineEnd = nl ? nl : p + strlen( p );
		const char *s = p;
		while ( s < lineEnd && ( *s == ' ' || *s == '\t' ) ) {
			s++;
		}
		if ( lineEnd - s >= 8 && idStr::Cmpn( s, "#include", 8 ) == 0 ) {
			const char *q1 = (const char *)memchr( s, '"', lineEnd - s );
			const char *q2 = q1 ? (const char *)memchr( q1 + 1, '"', lineEnd - ( q1 + 1 ) ) : NULL;
			if ( !q1 || !q2 ) {
				common->Warning( "VK shaderc: malformed #include" );
				return false;
			}
			idStr inc;
			inc.Append( q1 + 1, (int)( q2 - q1 - 1 ) );
			idStr incText;
			if ( !VK_ReadShaderSource( inc.c_str(), incText ) ) {
				common->Warning( "VK shaderc: couldn't read include shaders/%s", inc.c_str() );
				return false;
			}
			if ( !VK_ExpandIncludes( incText.c_str(), out, depth + 1 ) ) {
				return false;
			}
			out.Append( "\n", 1 );
		} else {
			out.Append( p, (int)( lineEnd - p ) );
			if ( nl ) {
				out.Append( "\n", 1 );
			}
		}
		if ( !nl ) {
			break;
		}
		p = nl + 1;
	}
	return true;
}

static bool VK_CompileGlslToSpv( const char *name, const char *body, shaderc_shader_kind kind, std::vector<uint32_t> &out ) {
	idStr full;
	if ( kind == shaderc_compute_shader ) {
		// Compute units are authored standalone (their own #version, no graphics prelude):
		// prelude.vk.glsl declares vertex/fragment layout bindings + VARY/SAMPLER_BINDING
		// macros and the invariant gl_Position that don't belong in — and won't compile in —
		// a compute stage. Just expand includes on the body directly.
		if ( !VK_ExpandIncludes( body, full, 0 ) ) {
			return false;
		}
	} else {
		idStr prelude;
		if ( !VK_ReadShaderSource( "prelude.vk.glsl", prelude ) ) {
			common->Warning( "VK shaderc: missing shaders/prelude.vk.glsl" );
			return false;
		}
		full = prelude;
		full.Append( "\n", 1 );
		if ( kind == shaderc_vertex_shader ) {
			// multi-pass depth invariance, exactly as compile_spv.py injects it
			full += "invariant gl_Position;\n";
		}
		if ( !VK_ExpandIncludes( body, full, 0 ) ) {
			return false;
		}
	}

	shaderc_compiler_t comp = shaderc_compiler_initialize();
	shaderc_compile_options_t opts = shaderc_compile_options_initialize();
	shaderc_compile_options_set_target_env( opts, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_4 );
	shaderc_compile_options_set_source_language( opts, shaderc_source_language_glsl );
	shaderc_compilation_result_t res = shaderc_compile_into_spv(
	    comp, full.c_str(), (size_t)full.Length(), kind, name, "main", opts );
	bool ok = shaderc_result_get_compilation_status( res ) == shaderc_compilation_status_success;
	if ( !ok ) {
		common->Warning( "VK shaderc: %s failed:\n%s", name, shaderc_result_get_error_message( res ) );
	} else {
		const uint32_t *code = (const uint32_t *)shaderc_result_get_bytes( res );
		size_t words = shaderc_result_get_length( res ) / sizeof( uint32_t );
		out.assign( code, code + words );
	}
	shaderc_result_release( res );
	shaderc_compile_options_release( opts );
	shaderc_compiler_release( comp );
	return ok;
}
#endif	// DUDE_HAVE_SHADERC

ShaderHandle VulkanBackend::CreateShaderFromGlsl( const char *name, const char *vertSrc, const char *fragSrc ) {
#ifdef DUDE_HAVE_SHADERC
	if ( device == VK_NULL_HANDLE || name == NULL || name[0] == '\0' || vertSrc == NULL || fragSrc == NULL ) {
		return 0;
	}
	for ( size_t i = 0; i < shaderTable.size(); i++ ) {
		if ( shaderTable[i].name.Icmp( name ) == 0 ) {
			return shaderTable[i].failed ? 0 : (ShaderHandle)( i + 1 );
		}
	}

	ShaderRec rec;
	rec.name = name;

	std::vector<uint32_t> vertSpv, fragSpv;
	if ( !VK_CompileGlslToSpv( name, vertSrc, shaderc_vertex_shader, vertSpv )
	  || !VK_CompileGlslToSpv( name, fragSrc, shaderc_fragment_shader, fragSpv ) ) {
		rec.failed = true;
		shaderTable.push_back( rec );
		return 0;
	}

	VkShaderModuleCreateInfo mi = {};
	mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	mi.codeSize = vertSpv.size() * sizeof( uint32_t );
	mi.pCode = vertSpv.data();
	bool ok = vkCheck( vkCreateShaderModule( device, &mi, NULL, &rec.vert ), va( "vkCreateShaderModule(%s.vert)", name ) );
	mi.codeSize = fragSpv.size() * sizeof( uint32_t );
	mi.pCode = fragSpv.data();
	ok = ok && vkCheck( vkCreateShaderModule( device, &mi, NULL, &rec.frag ), va( "vkCreateShaderModule(%s.frag)", name ) );
	if ( !ok ) {
		if ( rec.vert ) { vkDestroyShaderModule( device, rec.vert, NULL ); rec.vert = VK_NULL_HANDLE; }
		if ( rec.frag ) { vkDestroyShaderModule( device, rec.frag, NULL ); rec.frag = VK_NULL_HANDLE; }
		rec.failed = true;
	}
	shaderTable.push_back( rec );
	return rec.failed ? 0 : (ShaderHandle)shaderTable.size();
#else
	(void)name; (void)vertSrc; (void)fragSrc;
	return 0;
#endif
}

// Compile a standalone compute GLSL source into a compute shader module (Phase 1).
// Mirrors CreateShaderFromGlsl but one stage (shaderc_compute_shader) into ShaderRec.comp.
// Cached by name in the same shaderTable (handle = index+1); a compute rec leaves vert/frag
// NULL. Namespace compute names (e.g. "cs_*") so they never collide with a graphics shader.
ShaderHandle VulkanBackend::CreateComputeShader( const char *name, const char *glslSrc ) {
#ifdef DUDE_HAVE_SHADERC
	if ( device == VK_NULL_HANDLE || name == NULL || name[0] == '\0' || glslSrc == NULL ) {
		return 0;
	}
	for ( size_t i = 0; i < shaderTable.size(); i++ ) {
		if ( shaderTable[i].name.Icmp( name ) == 0 ) {
			return shaderTable[i].failed ? 0 : (ShaderHandle)( i + 1 );
		}
	}

	ShaderRec rec;
	rec.name = name;

	std::vector<uint32_t> compSpv;
	if ( !VK_CompileGlslToSpv( name, glslSrc, shaderc_compute_shader, compSpv ) ) {
		rec.failed = true;
		shaderTable.push_back( rec );
		return 0;
	}

	VkShaderModuleCreateInfo mi = {};
	mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	mi.codeSize = compSpv.size() * sizeof( uint32_t );
	mi.pCode = compSpv.data();
	if ( !vkCheck( vkCreateShaderModule( device, &mi, NULL, &rec.comp ), va( "vkCreateShaderModule(%s.comp)", name ) ) ) {
		rec.comp = VK_NULL_HANDLE;
		rec.failed = true;
	}
	shaderTable.push_back( rec );
	return rec.failed ? 0 : (ShaderHandle)shaderTable.size();
#else
	(void)name; (void)glslSrc;
	return 0;
#endif
}

// Build (or fetch from cache) a compute VkPipeline for a compute shader handle. Keyed by
// ShaderHandle in a map separate from the graphics pipelineCache (whose key encodes graphics
// state compute has none of). Caches VK_NULL_HANDLE for a missing/failed compute module so a
// failed shaderc compile isn't retried every dispatch. Reuses diskPipelineCache. (Phase 1.)
VkPipeline VulkanBackend::GetComputePipeline( ShaderHandle shader ) {
	auto it = computePipelineCache.find( shader );
	if ( it != computePipelineCache.end() ) {
		return it->second;
	}
	VkPipeline pipeline = VK_NULL_HANDLE;
	if ( shader >= 1 && shader <= (ShaderHandle)shaderTable.size()
	     && !shaderTable[shader - 1].failed && shaderTable[shader - 1].comp != VK_NULL_HANDLE ) {
		VkPipelineShaderStageCreateInfo stage = {};
		stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
		stage.module = shaderTable[shader - 1].comp;
		stage.pName = "main";
		VkComputePipelineCreateInfo cpci = {};
		cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
		cpci.stage = stage;
		cpci.layout = computePipeLayout;
		if ( !vkCheck( vkCreateComputePipelines( device, diskPipelineCache, 1, &cpci, NULL, &pipeline ), "vkCreateComputePipelines" ) ) {
			pipeline = VK_NULL_HANDLE;
		}
	}
	computePipelineCache[shader] = pipeline;		// cache success AND null (don't retry)
	return pipeline;
}

// Record a compute dispatch onto cb: bind the pipeline, allocate + write + bind a storage
// descriptor set, push params, vkCmdDispatch. Returns the allocated set so the CALLER reclaims
// it (free after idle for the synchronous self-test; retire behind the frame fence mid-frame).
// VK_NULL_HANDLE = nothing recorded. Barriers are the caller's job (they know the consumer:
// HOST for readback, VERTEX_INPUT for skinning). Must be recorded outside a render pass. (Phase 1.)
VkDescriptorSet VulkanBackend::RecordDispatch( VkCommandBuffer cb, const ComputeArgs &args ) {
	VkPipeline pipeline = GetComputePipeline( args.shader );
	if ( pipeline == VK_NULL_HANDLE ) {
		return VK_NULL_HANDLE;
	}
	VkDescriptorSetAllocateInfo ai = {};
	ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	ai.descriptorPool = computePool;
	ai.descriptorSetCount = 1;
	ai.pSetLayouts = &setLayoutCompute;
	VkDescriptorSet set = VK_NULL_HANDLE;
	if ( vkAllocateDescriptorSets( device, &ai, &set ) != VK_SUCCESS ) {
		common->Warning( "VK: compute descriptor pool exhausted" );
		return VK_NULL_HANDLE;
	}
	// write only the bound storage buffers; a compute shader statically uses only what it
	// binds, so unbound bindings need no descriptor.
	VkDescriptorBufferInfo bi[8] = {};
	VkWriteDescriptorSet w[8] = {};
	int n = 0;
	for ( int i = 0; i < 8; i++ ) {
		if ( args.storage[i] < 1 || args.storage[i] > (BufferHandle)bufferTable.size() ) {
			continue;
		}
		bi[n].buffer = bufferTable[args.storage[i] - 1];
		bi[n].offset = 0;
		bi[n].range = VK_WHOLE_SIZE;
		w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		w[n].dstSet = set;
		w[n].dstBinding = (uint32_t)i;
		w[n].descriptorCount = 1;
		w[n].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		w[n].pBufferInfo = &bi[n];
		n++;
	}
	if ( n > 0 ) {
		vkUpdateDescriptorSets( device, (uint32_t)n, w, 0, NULL );
	}
	vkCmdBindPipeline( cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline );
	vkCmdBindDescriptorSets( cb, VK_PIPELINE_BIND_POINT_COMPUTE, computePipeLayout, 0, 1, &set, 0, NULL );
	if ( args.pushConstants && args.pushConstantSize > 0 ) {
		uint32_t pcSize = (uint32_t)( args.pushConstantSize > 128 ? 128 : args.pushConstantSize );
		vkCmdPushConstants( cb, computePipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, pcSize, args.pushConstants );
	}
	vkCmdDispatch( cb, args.groupsX ? args.groupsX : 1, args.groupsY ? args.groupsY : 1, args.groupsZ ? args.groupsZ : 1 );
	return set;
}

void VulkanBackend::Dispatch( const ComputeArgs &args ) {
	if ( device == VK_NULL_HANDLE || computePipeLayout == VK_NULL_HANDLE || !frameOpen || skipFrame ) {
		return;
	}
	// a vkCmdDispatch is illegal inside a render pass; the compute lane records in the
	// pre-scene window on the frame cb. If a scene/target pass is already open, skip
	// (Phase 2 sequences dispatches before the scene pass opens).
	if ( insideScenePass || insideTargetPass ) {
		static bool warned = false;
		if ( !warned ) { warned = true; common->Warning( "VK: Dispatch() inside a render pass — skipped (record it pre-scene)" ); }
		return;
	}
	VkCommandBuffer cb = frames[frameIndex].cb;
	VkDescriptorSet set = RecordDispatch( cb, args );
	if ( set == VK_NULL_HANDLE ) {
		return;
	}
	// make compute writes available to subsequent vertex fetch / shader reads this frame.
	// COMPUTE_SHADER is in the destination set so a following dispatch can consume this one's
	// output (the MD5 skin runs as position pass -> tangent-derive pass over the same buffer).
	// deferBarrier: a batch of independent dispatches (skin/deform flush) writes disjoint
	// buffers, so no barrier is needed between them — the caller emits one PostComputeBarrier()
	// after the batch. Standalone dispatches (default) self-barrier here.
	if ( !args.deferBarrier ) {
		PostComputeBarrier();
	}
	retiredComputeSets[frameIndex].push_back( set );
}

// One compute->consumer barrier for a batch of deferBarrier dispatches (see ComputeArgs).
// Identical masks to the per-dispatch barrier above: SHADER_WRITE -> vertex-attribute /
// index / shader read+write, so the skinned/deformed vertex buffers are visible to the
// draws (and to a following deform dispatch that reads a skin output).
void VulkanBackend::PostComputeBarrier() {
	if ( device == VK_NULL_HANDLE || !frameOpen || skipFrame || insideScenePass || insideTargetPass ) {
		return;
	}
	VkCommandBuffer cb = frames[frameIndex].cb;
	VkMemoryBarrier mb = {};
	mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	mb.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
	VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT
		| VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
	// R3.5 animated BLAS: also make the skinned vertex writes visible to an acceleration-structure
	// build/refit that consumes gpuSkinVB as geometry this frame (skin compute -> AS build). The
	// AS-build stage/access flags are only legal when the extension is enabled, so add them solely on
	// RT-capable devices. When no AS build actually reads the skin this frame the widened dst scope is
	// a harmless no-op (nothing waits in that stage). The existing AS-build -> fragment ray-read
	// barrier then carries the skinned geometry into the shadow trace.
	if ( haveRayQuery ) {
		mb.dstAccessMask |= VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
		dstStage |= VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
	}
	vkCmdPipelineBarrier( cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, dstStage,
		0, 1, &mb, 0, NULL, 0, NULL );
}

// Synchronous compute dispatch on the dedicated upload cb: record, barrier to HOST, submit,
// wait. Leaves the result visible to a subsequent ReadBuffer. Stalls the GPU — dev/validation
// and load-time only, never per-frame. (Phase 1/2.)
void VulkanBackend::DispatchSync( const ComputeArgs &args ) {
	if ( device == VK_NULL_HANDLE || computePipeLayout == VK_NULL_HANDLE || uploadCb == VK_NULL_HANDLE ) {
		return;
	}
	vkQueueWaitIdle( gfxQueue );
	vkResetCommandBuffer( uploadCb, 0 );
	VkCommandBufferBeginInfo bbi = {};
	bbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	bbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer( uploadCb, &bbi );
	VkDescriptorSet set = RecordDispatch( uploadCb, args );
	if ( set != VK_NULL_HANDLE ) {
		VkMemoryBarrier mb = {};
		mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
		vkCmdPipelineBarrier( uploadCb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
			0, 1, &mb, 0, NULL, 0, NULL );
	}
	vkEndCommandBuffer( uploadCb );
	VkSubmitInfo si = {};
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.commandBufferCount = 1;
	si.pCommandBuffers = &uploadCb;
	vkResetFences( device, 1, &uploadFence );
	vkQueueSubmit( gfxQueue, 1, &si, uploadFence );
	vkWaitForFences( device, 1, &uploadFence, VK_TRUE, UINT64_MAX );
	if ( set != VK_NULL_HANDLE ) {
		vkFreeDescriptorSets( device, computePool, 1, &set );		// cb done -> safe
	}
}

// r_vkComputeTest: validate the whole compute lane end to end. Seeds a host-visible storage
// buffer with 0..N-1, dispatches a kernel that doubles each element, reads it back and checks
// data[i] == 2*i. Two staged checks isolate a failure: the raw seed round-trip (buffer create +
// ReadBuffer, no compute) then the dispatched result. Synchronous one-shot (dev tool) — records
// on the upload cb and waits idle, so never call it per-frame. (Phase 1.)
void VulkanBackend::ComputeSelfTest() {
	if ( device == VK_NULL_HANDLE || computePipeLayout == VK_NULL_HANDLE || uploadCb == VK_NULL_HANDLE ) {
		common->Printf( "VK compute self-test: unavailable (no compute lane)\n" );
		return;
	}
	static const char *kSrc =
		"#version 450\n"
		"layout(local_size_x = 64) in;\n"
		"layout(std430, binding = 0) buffer B { uint v[]; };\n"
		"layout(push_constant) uniform PC { uint count; } pc;\n"
		"void main() {\n"
		"    uint i = gl_GlobalInvocationID.x;\n"
		"    if ( i < pc.count ) { v[i] = v[i] * 2u; }\n"
		"}\n";
	ShaderHandle sh = CreateComputeShader( "cs_selftest", kSrc );
	if ( sh == 0 ) {
		common->Printf( "VK compute self-test: FAIL (compute shader did not compile)\n" );
		return;
	}
	const int N = 256;
	uint32_t seed[N];
	for ( int i = 0; i < N; i++ ) { seed[i] = (uint32_t)i; }
	BufferHandle buf = CreateBuffer( BU_STORAGE, N * (int)sizeof( uint32_t ), seed );
	if ( buf == 0 ) {
		common->Printf( "VK compute self-test: FAIL (storage buffer alloc)\n" );
		return;
	}

	// check 1: raw seed survives buffer-create + readback (isolates the buffer path from compute)
	uint32_t back[N];
	bool ok = ReadBuffer( buf, back, N * (int)sizeof( uint32_t ) );
	for ( int i = 0; ok && i < N; i++ ) {
		if ( back[i] != (uint32_t)i ) {
			common->Printf( "VK compute self-test: FAIL (seed round-trip at i=%d: %u != %d)\n", i, back[i], i );
			ok = false;
		}
	}
	if ( !ok ) { DestroyBuffer( buf ); return; }

	// check 2: dispatch the doubling kernel, read back, verify data[i] == 2*i
	uint32_t count = (uint32_t)N;
	ComputeArgs ca = {};
	ca.shader = sh;
	ca.storage[0] = buf;
	ca.pushConstants = &count;
	ca.pushConstantSize = (int)sizeof( count );
	ca.groupsX = ( N + 63 ) / 64; ca.groupsY = 1; ca.groupsZ = 1;

	DispatchSync( ca );
	const bool readOk = ReadBuffer( buf, back, N * (int)sizeof( uint32_t ) );
	int firstBad = -1;
	for ( int i = 0; readOk && i < N; i++ ) {
		if ( back[i] != (uint32_t)( 2 * i ) ) { firstBad = i; break; }
	}
	if ( !readOk ) {
		common->Printf( "VK compute self-test: FAIL (readback failed)\n" );
	} else if ( firstBad >= 0 ) {
		// a no-op dispatch (null pipeline) leaves the seed untouched, so it also lands here
		common->Printf( "VK compute self-test: FAIL (result at i=%d: %u != %d)\n", firstBad, back[firstBad], 2 * firstBad );
	} else {
		common->Printf( "VK compute self-test: PASS (%d elements doubled on the GPU)\n", N );
	}
	DestroyBuffer( buf );
}

/*
====================
VulkanBackend::GetBufferDeviceAddress

GPU virtual address of a buffer (Vulkan buffer_device_address). Returns the value
cached at creation (a buffer's address is fixed for its lifetime), or 0 for a buffer
that lacks SHADER_DEVICE_ADDRESS usage (ring slots, non-BDA usages, feature absent) or
an invalid handle. Cached rather than re-queried so this never calls
vkGetBufferDeviceAddress on a buffer without the usage bit (which is invalid).
(Phase 3.2b BDA primitive.)
====================
*/
unsigned long long VulkanBackend::GetBufferDeviceAddress( BufferHandle b ) {
	if ( b < 1 || b > (BufferHandle)bufferAddr.size() ) {
		return 0;
	}
	return (unsigned long long)bufferAddr[b - 1];
}

// r_vkBdaTest: validate the buffer-device-address plumbing that Phase 3.2b needs to draw
// per-surface geometry without one unified vertex buffer. Seeds a storage buffer with 0..N-1,
// queries its GPU address, and dispatches a kernel that reads N uints THROUGH THAT RAW POINTER
// (GL_EXT_buffer_reference, no bound buffer at that binding) and writes the sum to a separate
// bound SSBO. Verifies the sum == N*(N-1)/2. Proves: feature enabled, address query works, and a
// shader can dereference the pointer. Synchronous one-shot (dev tool) — never call per-frame.
void VulkanBackend::BdaSelfTest() {
	if ( device == VK_NULL_HANDLE || computePipeLayout == VK_NULL_HANDLE || uploadCb == VK_NULL_HANDLE ) {
		common->Printf( "VK BDA self-test: unavailable (no compute lane)\n" );
		return;
	}
	if ( !haveBufferDeviceAddress ) {
		common->Printf( "VK BDA self-test: unavailable (device lacks bufferDeviceAddress)\n" );
		return;
	}
	// The source is read via a buffer_reference pointer carried in the push constant (NOT bound to
	// a descriptor); only the output sum is a bound SSBO at binding 0. buffer_reference is itself a
	// 64-bit handle, so the push-constant address needs no separate int64 extension.
	static const char *kSrc =
		"#version 450\n"
		"#extension GL_EXT_buffer_reference : require\n"
		"layout(local_size_x = 64) in;\n"
		"layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer SrcRef { uint v[]; };\n"
		"layout(std430, binding = 0) buffer Out { uint sum; } outb;\n"
		"layout(push_constant) uniform PC { SrcRef src; uint count; } pc;\n"
		"void main() {\n"
		"    if ( gl_GlobalInvocationID.x != 0u ) { return; }\n"
		"    uint s = 0u;\n"
		"    for ( uint i = 0u; i < pc.count; i++ ) { s += pc.src.v[i]; }\n"
		"    outb.sum = s;\n"
		"}\n";
	ShaderHandle sh = CreateComputeShader( "cs_bdatest", kSrc );
	if ( sh == 0 ) {
		common->Printf( "VK BDA self-test: FAIL (compute shader did not compile)\n" );
		return;
	}

	const int N = 256;
	uint32_t seed[N];
	uint32_t expect = 0;
	for ( int i = 0; i < N; i++ ) { seed[i] = (uint32_t)i; expect += (uint32_t)i; }
	BufferHandle src = CreateBuffer( BU_STORAGE, N * (int)sizeof( uint32_t ), seed );
	BufferHandle out = CreateBuffer( BU_STORAGE, (int)sizeof( uint32_t ), NULL );
	if ( src == 0 || out == 0 ) {
		common->Printf( "VK BDA self-test: FAIL (storage buffer alloc)\n" );
		if ( src ) { DestroyBuffer( src ); }
		if ( out ) { DestroyBuffer( out ); }
		return;
	}

	// check 1: the address query returns non-zero (feature + usage bit + query all wired)
	unsigned long long addr = GetBufferDeviceAddress( src );
	if ( addr == 0 ) {
		common->Printf( "VK BDA self-test: FAIL (GetBufferDeviceAddress returned 0)\n" );
		DestroyBuffer( src );
		DestroyBuffer( out );
		return;
	}

	// check 2: the shader dereferences that pointer, sums, and writes it to the bound out buffer
	struct { unsigned long long addr; uint32_t count; } pc;
	pc.addr = addr;
	pc.count = (uint32_t)N;
	ComputeArgs ca = {};
	ca.shader = sh;
	ca.storage[0] = out;			// binding 0 = the output sum (the source comes via the pointer)
	ca.pushConstants = &pc;
	ca.pushConstantSize = (int)sizeof( pc );
	ca.groupsX = 1; ca.groupsY = 1; ca.groupsZ = 1;
	DispatchSync( ca );

	uint32_t got = 0;
	const bool readOk = ReadBuffer( out, &got, (int)sizeof( got ) );
	if ( !readOk ) {
		common->Printf( "VK BDA self-test: FAIL (readback failed)\n" );
	} else if ( got != expect ) {
		common->Printf( "VK BDA self-test: FAIL (sum via pointer = %u, expected %u)\n", got, expect );
	} else {
		common->Printf( "VK BDA self-test: PASS (GPU summed %d uints through a device-address pointer 0x%llx = %u)\n",
			N, addr, got );
	}
	DestroyBuffer( src );
	DestroyBuffer( out );
}

/*
================================================================================
R2 ray-query acceleration structures (docs/rtx-shadow-roadmap.md)

Synchronous builds on the upload cb (DispatchSync's submit idiom) — load-time and
validation use; the per-frame TLAS rebuild + BLAS refit lanes move onto the frame
command buffer when R3 consumes them. All buffers host-visible (D3-scale AS data
is small); device-local staging is a later perf pass, mirroring the
static-vertex-buffer history.
================================================================================
*/

bool VulkanBackend::SupportsRayQuery() {
	return haveRayQuery && device != VK_NULL_HANDLE && uploadCb != VK_NULL_HANDLE;
}

bool VulkanBackend::CreateRtBuffer( VkBufferUsageFlags usage, VkDeviceSize size, const void *data, RtBuf &rb ) {
	VkBufferCreateInfo bci = {};
	bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bci.size = size;
	bci.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
	bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VmaAllocationCreateInfo aci = {};
	aci.usage = VMA_MEMORY_USAGE_AUTO;
	aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
	aci.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	VmaAllocationInfo info = {};
	if ( !vkCheck( vmaCreateBuffer( vma, &bci, &aci, &rb.buf, &rb.alloc, &info ), "vmaCreateBuffer(rt)" ) ) {
		rb.buf = VK_NULL_HANDLE;
		rb.alloc = NULL;
		return false;
	}
	if ( data ) {
		memcpy( info.pMappedData, data, (size_t)size );
	}
	rb.map = info.pMappedData;
	VkBufferDeviceAddressInfo bai = {};
	bai.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
	bai.buffer = rb.buf;
	rb.addr = (VkDeviceAddress)vkGetBufferDeviceAddress( device, &bai );
	return true;
}

void VulkanBackend::DestroyRtBuffer( RtBuf &rb ) {
	if ( rb.buf != VK_NULL_HANDLE ) {
		vmaDestroyBuffer( vma, rb.buf, rb.alloc );
	}
	rb.buf = VK_NULL_HANDLE;
	rb.alloc = NULL;
	rb.addr = 0;
	rb.map = NULL;
}

// Size, create and build one AS synchronously; the fence wait doubles as the build->consume
// dependency barrier (a TLAS build reading a BLAS built the same way is already ordered).
bool VulkanBackend::BuildAsSync( VkAccelerationStructureTypeKHR type, const VkAccelerationStructureGeometryKHR &geom,
		uint32_t primCount, RtBuf &asBuf, VkAccelerationStructureKHR &as ) {
	VkAccelerationStructureBuildGeometryInfoKHR bgi = {};
	bgi.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
	bgi.type = type;
	bgi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
	bgi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	bgi.geometryCount = 1;
	bgi.pGeometries = &geom;
	VkAccelerationStructureBuildSizesInfoKHR sizes = {};
	sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
	pfnGetAsBuildSizes( device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bgi, &primCount, &sizes );
	if ( !CreateRtBuffer( VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, sizes.accelerationStructureSize, NULL, asBuf ) ) {
		return false;
	}
	RtBuf scratch;
	if ( !CreateRtBuffer( VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, sizes.buildScratchSize + asScratchAlignment, NULL, scratch ) ) {
		DestroyRtBuffer( asBuf );
		return false;
	}
	VkAccelerationStructureCreateInfoKHR asci = {};
	asci.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
	asci.buffer = asBuf.buf;
	asci.size = sizes.accelerationStructureSize;
	asci.type = type;
	if ( !vkCheck( pfnCreateAs( device, &asci, NULL, &as ), "vkCreateAccelerationStructureKHR" ) ) {
		as = VK_NULL_HANDLE;
		DestroyRtBuffer( asBuf );
		DestroyRtBuffer( scratch );
		return false;
	}
	bgi.dstAccelerationStructure = as;
	bgi.scratchData.deviceAddress = ( scratch.addr + asScratchAlignment - 1 ) & ~(VkDeviceAddress)( asScratchAlignment - 1 );
	VkAccelerationStructureBuildRangeInfoKHR range = {};
	range.primitiveCount = primCount;
	const VkAccelerationStructureBuildRangeInfoKHR *pRange = &range;
	vkQueueWaitIdle( gfxQueue );
	vkResetCommandBuffer( uploadCb, 0 );
	VkCommandBufferBeginInfo bbi = {};
	bbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	bbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer( uploadCb, &bbi );
	pfnCmdBuildAs( uploadCb, 1, &bgi, &pRange );
	vkEndCommandBuffer( uploadCb );
	VkSubmitInfo si = {};
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.commandBufferCount = 1;
	si.pCommandBuffers = &uploadCb;
	vkResetFences( device, 1, &uploadFence );
	vkQueueSubmit( gfxQueue, 1, &si, uploadFence );
	vkWaitForFences( device, 1, &uploadFence, VK_TRUE, UINT64_MAX );
	DestroyRtBuffer( scratch );
	return true;
}

BlasHandle VulkanBackend::CreateBlas( const float *positions, int numVerts, int posStride,
		const int *indexes, int numIndexes ) {
	if ( !SupportsRayQuery() || positions == NULL || indexes == NULL || numVerts <= 0 || numIndexes < 3 ) {
		return 0;
	}
	// stage the inputs; freed right after the synchronous build (the AS holds no reference)
	RtBuf vb, ib;
	if ( !CreateRtBuffer( VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
			(VkDeviceSize)numVerts * (VkDeviceSize)posStride, positions, vb ) ) {
		return 0;
	}
	if ( !CreateRtBuffer( VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
			(VkDeviceSize)numIndexes * sizeof( int ), indexes, ib ) ) {
		DestroyRtBuffer( vb );
		return 0;
	}
	VkAccelerationStructureGeometryKHR geom = {};
	geom.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
	geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
	geom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
	geom.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
	geom.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
	geom.geometry.triangles.vertexData.deviceAddress = vb.addr;
	geom.geometry.triangles.vertexStride = (VkDeviceSize)posStride;
	geom.geometry.triangles.maxVertex = (uint32_t)( numVerts - 1 );
	geom.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
	geom.geometry.triangles.indexData.deviceAddress = ib.addr;

	RtBlas blas;
	const bool ok = BuildAsSync( VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, geom,
		(uint32_t)( numIndexes / 3 ), blas.buf, blas.as );
	DestroyRtBuffer( vb );
	DestroyRtBuffer( ib );
	if ( !ok ) {
		return 0;
	}
	VkAccelerationStructureDeviceAddressInfoKHR dai = {};
	dai.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
	dai.accelerationStructure = blas.as;
	blas.addr = pfnGetAsDeviceAddress( device, &dai );
	// reuse a freed slot if one exists (level-scoped churn), else append
	for ( size_t i = 0; i < rtBlases.size(); i++ ) {
		if ( rtBlases[i].as == VK_NULL_HANDLE ) {
			rtBlases[i] = blas;
			return (BlasHandle)( i + 1 );
		}
	}
	rtBlases.push_back( blas );
	return (BlasHandle)rtBlases.size();
}

// R3.5: an animated BLAS takes one geometry per surface; a Doom 3 character is ~6-10 srfTriangles,
// so a fixed stack cap avoids per-frame heap churn in RefitBlas (the hot path). Well above any real
// model's surface count; CreateBlasFromBuffers/RefitBlas reject a larger set rather than truncate.
static const int MAX_BLAS_GEOMS = 64;

// Fill the Vk triangle-geometry + build-range arrays for a device-buffer-fed BLAS from BlasGeometry[].
// Shared by CreateBlasFromBuffers (BUILD) and RefitBlas (UPDATE) — only the mode/scratch/AS handles
// differ between them, the per-surface geometry description is identical. Caller guarantees
// count <= MAX_BLAS_GEOMS. xyz is the first 3 floats at vertexAddress; indices are 32-bit, surface-local.
static void R_VkFillBlasGeoms( const RHI::BlasGeometry *geoms, int count,
		VkAccelerationStructureGeometryKHR *vkGeoms, VkAccelerationStructureBuildRangeInfoKHR *ranges,
		uint32_t *primCounts ) {
	for ( int i = 0; i < count; i++ ) {
		const RHI::BlasGeometry &g = geoms[i];
		VkAccelerationStructureGeometryKHR &vg = vkGeoms[i];
		memset( &vg, 0, sizeof( vg ) );
		vg.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
		vg.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
		vg.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
		vg.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
		vg.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
		vg.geometry.triangles.vertexData.deviceAddress = (VkDeviceAddress)g.vertexAddress;
		vg.geometry.triangles.vertexStride = (VkDeviceSize)g.vertexStride;
		vg.geometry.triangles.maxVertex = g.vertexCount > 0 ? g.vertexCount - 1 : 0;
		vg.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
		vg.geometry.triangles.indexData.deviceAddress = (VkDeviceAddress)g.indexAddress;
		memset( &ranges[i], 0, sizeof( ranges[i] ) );
		ranges[i].primitiveCount = g.indexCount / 3;
		primCounts[i] = g.indexCount / 3;
	}
}

// Build one multi-geometry BLAS directly from GPU device buffers (gpuSkinVB + static index buffers),
// synchronously on the upload cb. allowUpdate makes it refit-capable and allocates the persistent
// update-scratch RefitBlas reuses. Rare (first sight / topology change); the per-frame path is RefitBlas.
BlasHandle VulkanBackend::CreateBlasFromBuffers( const BlasGeometry *geoms, int count, bool allowUpdate ) {
	if ( !SupportsRayQuery() || geoms == NULL || count <= 0 || count > MAX_BLAS_GEOMS ) {
		return 0;
	}
	VkAccelerationStructureGeometryKHR vkGeoms[MAX_BLAS_GEOMS];
	VkAccelerationStructureBuildRangeInfoKHR ranges[MAX_BLAS_GEOMS];
	uint32_t primCounts[MAX_BLAS_GEOMS];
	R_VkFillBlasGeoms( geoms, count, vkGeoms, ranges, primCounts );

	VkBuildAccelerationStructureFlagsKHR flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
	if ( allowUpdate ) {
		flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
	}
	VkAccelerationStructureBuildGeometryInfoKHR bgi = {};
	bgi.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
	bgi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
	bgi.flags = flags;
	bgi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	bgi.geometryCount = (uint32_t)count;
	bgi.pGeometries = vkGeoms;
	VkAccelerationStructureBuildSizesInfoKHR sizes = {};
	sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
	pfnGetAsBuildSizes( device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bgi, primCounts, &sizes );

	RtBlas blas;
	if ( !CreateRtBuffer( VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, sizes.accelerationStructureSize, NULL, blas.buf ) ) {
		return 0;
	}
	RtBuf scratch;
	if ( !CreateRtBuffer( VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, sizes.buildScratchSize + asScratchAlignment, NULL, scratch ) ) {
		DestroyRtBuffer( blas.buf );
		return 0;
	}
	if ( allowUpdate ) {
		// persistent scratch sized for future in-place UPDATE builds (smaller than the build scratch)
		if ( !CreateRtBuffer( VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, sizes.updateScratchSize + asScratchAlignment, NULL, blas.updateScratch ) ) {
			DestroyRtBuffer( blas.buf );
			DestroyRtBuffer( scratch );
			return 0;
		}
		blas.updatable = true;
	}
	VkAccelerationStructureCreateInfoKHR asci = {};
	asci.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
	asci.buffer = blas.buf.buf;
	asci.size = sizes.accelerationStructureSize;
	asci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
	if ( !vkCheck( pfnCreateAs( device, &asci, NULL, &blas.as ), "vkCreateAccelerationStructureKHR(blasFromBuffers)" ) ) {
		blas.as = VK_NULL_HANDLE;
		DestroyRtBuffer( blas.buf );
		DestroyRtBuffer( scratch );
		DestroyRtBuffer( blas.updateScratch );
		return 0;
	}
	bgi.dstAccelerationStructure = blas.as;
	bgi.scratchData.deviceAddress = ( scratch.addr + asScratchAlignment - 1 ) & ~(VkDeviceAddress)( asScratchAlignment - 1 );
	const VkAccelerationStructureBuildRangeInfoKHR *pRange = ranges;
	vkQueueWaitIdle( gfxQueue );
	vkResetCommandBuffer( uploadCb, 0 );
	VkCommandBufferBeginInfo bbi = {};
	bbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	bbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer( uploadCb, &bbi );
	pfnCmdBuildAs( uploadCb, 1, &bgi, &pRange );
	vkEndCommandBuffer( uploadCb );
	VkSubmitInfo si = {};
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.commandBufferCount = 1;
	si.pCommandBuffers = &uploadCb;
	vkResetFences( device, 1, &uploadFence );
	vkQueueSubmit( gfxQueue, 1, &si, uploadFence );
	vkWaitForFences( device, 1, &uploadFence, VK_TRUE, UINT64_MAX );
	DestroyRtBuffer( scratch );

	VkAccelerationStructureDeviceAddressInfoKHR dai = {};
	dai.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
	dai.accelerationStructure = blas.as;
	blas.addr = pfnGetAsDeviceAddress( device, &dai );
	blas.geoms.assign( geoms, geoms + count );		// RR5: keep descriptors for the geo-table fill in UpdateTlas
	for ( size_t i = 0; i < rtBlases.size(); i++ ) {
		if ( rtBlases[i].as == VK_NULL_HANDLE ) {
			rtBlases[i] = blas;
			return (BlasHandle)( i + 1 );
		}
	}
	rtBlases.push_back( blas );
	return (BlasHandle)rtBlases.size();
}

// Refit an ALLOW_UPDATE BLAS in place from the same topology's current device buffers (skinning moved
// the vertices; triangle count / index layout fixed). In-frame it records on the frame cb + an
// AS-write -> AS-read barrier so the following TLAS build sees the refit; out of frame (validation) it
// runs synchronously on the upload cb.
void VulkanBackend::RefitBlas( BlasHandle blas, const BlasGeometry *geoms, int count ) {
	if ( !SupportsRayQuery() || blas == 0 || (size_t)blas > rtBlases.size()
		|| geoms == NULL || count <= 0 || count > MAX_BLAS_GEOMS ) {
		return;
	}
	RtBlas &b = rtBlases[blas - 1];
	if ( b.as == VK_NULL_HANDLE || !b.updatable ) {
		return;
	}
	VkAccelerationStructureGeometryKHR vkGeoms[MAX_BLAS_GEOMS];
	VkAccelerationStructureBuildRangeInfoKHR ranges[MAX_BLAS_GEOMS];
	uint32_t primCounts[MAX_BLAS_GEOMS];
	R_VkFillBlasGeoms( geoms, count, vkGeoms, ranges, primCounts );

	VkAccelerationStructureBuildGeometryInfoKHR bgi = {};
	bgi.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
	bgi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
	// UPDATE build flags must match the original build's flags exactly.
	bgi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
	bgi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
	bgi.srcAccelerationStructure = b.as;
	bgi.dstAccelerationStructure = b.as;			// in-place refit
	bgi.geometryCount = (uint32_t)count;
	bgi.pGeometries = vkGeoms;
	bgi.scratchData.deviceAddress = ( b.updateScratch.addr + asScratchAlignment - 1 ) & ~(VkDeviceAddress)( asScratchAlignment - 1 );
	const VkAccelerationStructureBuildRangeInfoKHR *pRange = ranges;

	if ( frameOpen && !skipFrame && !insideScenePass && !insideTargetPass ) {
		// hot path: record the refit on the frame cb, ahead of the TLAS build that reads this BLAS.
		VkCommandBuffer cb = frames[frameIndex].cb;
		pfnCmdBuildAs( cb, 1, &bgi, &pRange );
		VkMemoryBarrier mb = {};
		mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		mb.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
		mb.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
		vkCmdPipelineBarrier( cb, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
			VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &mb, 0, NULL, 0, NULL );
		return;
	}
	// out of frame (validation): synchronous refit on the upload cb. Stalls; never per-frame.
	vkQueueWaitIdle( gfxQueue );
	vkResetCommandBuffer( uploadCb, 0 );
	VkCommandBufferBeginInfo bbi = {};
	bbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	bbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer( uploadCb, &bbi );
	pfnCmdBuildAs( uploadCb, 1, &bgi, &pRange );
	vkEndCommandBuffer( uploadCb );
	VkSubmitInfo si = {};
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.commandBufferCount = 1;
	si.pCommandBuffers = &uploadCb;
	vkResetFences( device, 1, &uploadFence );
	vkQueueSubmit( gfxQueue, 1, &si, uploadFence );
	vkWaitForFences( device, 1, &uploadFence, VK_TRUE, UINT64_MAX );
}

void VulkanBackend::DestroyBlas( BlasHandle blas ) {
	if ( blas == 0 || (size_t)blas > rtBlases.size() ) {
		return;
	}
	RtBlas &b = rtBlases[blas - 1];
	if ( b.as != VK_NULL_HANDLE ) {
		vkQueueWaitIdle( gfxQueue );
		pfnDestroyAs( device, b.as, NULL );
		b.as = VK_NULL_HANDLE;
	}
	DestroyRtBuffer( b.buf );
	DestroyRtBuffer( b.updateScratch );
	b.updatable = false;
	b.addr = 0;
}

// ---- R3.5 S3: per-entity animated BLAS cache -----------------------------------------------------

VulkanBackend::AnimBlas *VulkanBackend::FindOrAllocAnimBlas( uint32_t key ) {
	for ( size_t i = 0; i < animBlasCache.size(); i++ ) {
		if ( animBlasCache[i].key == key && animBlasCache[i].as != VK_NULL_HANDLE ) {
			return &animBlasCache[i];		// live entry for this entity
		}
	}
	for ( size_t i = 0; i < animBlasCache.size(); i++ ) {
		if ( animBlasCache[i].key == 0 && animBlasCache[i].as == VK_NULL_HANDLE ) {
			animBlasCache[i] = AnimBlas();	// reuse a retired slot
			animBlasCache[i].key = key;
			return &animBlasCache[i];
		}
	}
	animBlasCache.push_back( AnimBlas() );
	animBlasCache.back().key = key;
	return &animBlasCache.back();
}

// Build one refit-capable BLAS for an entity on the frame cb (first sight / topology change). Allocates
// the AS + a persistent scratch sized to max(build,update) so subsequent refits reuse it. Records the
// BUILD on cb (no wait) — it reads this frame's gpuSkinVB, so it must run after the skin flush.
bool VulkanBackend::RecordAnimBlasBuild( AnimBlas &e, const BlasGeometry *geoms, int count, VkCommandBuffer cb ) {
	VkAccelerationStructureGeometryKHR vkGeoms[MAX_BLAS_GEOMS];
	VkAccelerationStructureBuildRangeInfoKHR ranges[MAX_BLAS_GEOMS];
	uint32_t primCounts[MAX_BLAS_GEOMS];
	R_VkFillBlasGeoms( geoms, count, vkGeoms, ranges, primCounts );
	uint32_t totalPrims = 0;
	for ( int i = 0; i < count; i++ ) { totalPrims += primCounts[i]; }

	VkAccelerationStructureBuildGeometryInfoKHR bgi = {};
	bgi.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
	bgi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
	bgi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
	bgi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	bgi.geometryCount = (uint32_t)count;
	bgi.pGeometries = vkGeoms;
	VkAccelerationStructureBuildSizesInfoKHR sizes = {};
	sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
	pfnGetAsBuildSizes( device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bgi, primCounts, &sizes );

	if ( !CreateRtBuffer( VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, sizes.accelerationStructureSize, NULL, e.buf ) ) {
		return false;
	}
	VkDeviceSize scratchSz = ( sizes.buildScratchSize > sizes.updateScratchSize ? sizes.buildScratchSize : sizes.updateScratchSize )
		+ asScratchAlignment;
	if ( !CreateRtBuffer( VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, scratchSz, NULL, e.scratch ) ) {
		DestroyRtBuffer( e.buf );
		return false;
	}
	VkAccelerationStructureCreateInfoKHR asci = {};
	asci.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
	asci.buffer = e.buf.buf;
	asci.size = sizes.accelerationStructureSize;
	asci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
	if ( !vkCheck( pfnCreateAs( device, &asci, NULL, &e.as ), "vkCreateAccelerationStructureKHR(animBlas)" ) ) {
		e.as = VK_NULL_HANDLE;
		DestroyRtBuffer( e.buf );
		DestroyRtBuffer( e.scratch );
		return false;
	}
	bgi.dstAccelerationStructure = e.as;
	bgi.scratchData.deviceAddress = ( e.scratch.addr + asScratchAlignment - 1 ) & ~(VkDeviceAddress)( asScratchAlignment - 1 );
	const VkAccelerationStructureBuildRangeInfoKHR *pRange = ranges;
	pfnCmdBuildAs( cb, 1, &bgi, &pRange );		// on the frame cb; RefreshAnimBlas emits the batched barrier
	VkAccelerationStructureDeviceAddressInfoKHR dai = {};
	dai.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
	dai.accelerationStructure = e.as;
	e.addr = pfnGetAsDeviceAddress( device, &dai );
	e.maxPrims = totalPrims;
	return true;
}

// In-place UPDATE (refit) of an entity's BLAS on the frame cb from its current gpuSkinVB. No per-refit
// barrier — RefreshAnimBlas emits one batched AS-write->AS-read barrier after all builds/refits.
void VulkanBackend::RecordAnimBlasRefit( AnimBlas &e, const BlasGeometry *geoms, int count, VkCommandBuffer cb ) {
	VkAccelerationStructureGeometryKHR vkGeoms[MAX_BLAS_GEOMS];
	VkAccelerationStructureBuildRangeInfoKHR ranges[MAX_BLAS_GEOMS];
	uint32_t primCounts[MAX_BLAS_GEOMS];
	R_VkFillBlasGeoms( geoms, count, vkGeoms, ranges, primCounts );
	VkAccelerationStructureBuildGeometryInfoKHR bgi = {};
	bgi.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
	bgi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
	bgi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
	bgi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
	bgi.srcAccelerationStructure = e.as;
	bgi.dstAccelerationStructure = e.as;
	bgi.geometryCount = (uint32_t)count;
	bgi.pGeometries = vkGeoms;
	bgi.scratchData.deviceAddress = ( e.scratch.addr + asScratchAlignment - 1 ) & ~(VkDeviceAddress)( asScratchAlignment - 1 );
	const VkAccelerationStructureBuildRangeInfoKHR *pRange = ranges;
	pfnCmdBuildAs( cb, 1, &bgi, &pRange );
}

// Move an entity's AS + buffers onto the fence-safe deferred-destroy list (a prior in-flight frame may
// still reference it). Drained FRAMES_IN_FLIGHT frames later, once no submitted frame can touch it.
void VulkanBackend::RetireAnimBlas( AnimBlas &e ) {
	if ( e.as != VK_NULL_HANDLE ) {
		RetiredAs r;
		r.as = e.as;
		r.buf = e.buf;
		r.scratch = e.scratch;
		r.ttl = FRAMES_IN_FLIGHT + 1;
		retiredAnimAs.push_back( r );
	}
	e.as = VK_NULL_HANDLE;
	e.addr = 0;
	e.key = 0;
	e.topoSig = 0;
	e.maxPrims = 0;
	e.lastSeenFrame = -1;
	e.buf = RtBuf();			// ownership transferred to the retired entry
	e.scratch = RtBuf();
}

void VulkanBackend::DrainRetiredAnimAs( bool force ) {
	for ( size_t i = 0; i < retiredAnimAs.size(); ) {
		if ( force || --retiredAnimAs[i].ttl <= 0 ) {
			if ( retiredAnimAs[i].as != VK_NULL_HANDLE ) {
				pfnDestroyAs( device, retiredAnimAs[i].as, NULL );
			}
			DestroyRtBuffer( retiredAnimAs[i].buf );
			DestroyRtBuffer( retiredAnimAs[i].scratch );
			retiredAnimAs[i] = retiredAnimAs.back();
			retiredAnimAs.pop_back();
		} else {
			i++;
		}
	}
}

void VulkanBackend::UpdateAnimCasters( const AnimCaster *casters, int count ) {
	animStaged.clear();
	animStagedGeoms.clear();
	if ( !SupportsRayQuery() || casters == NULL || count <= 0 ) {
		return;
	}
	for ( int i = 0; i < count; i++ ) {
		const AnimCaster &c = casters[i];
		if ( c.geoms == NULL || c.geomCount <= 0 || c.geomCount > MAX_BLAS_GEOMS || c.key == 0 ) {
			continue;
		}
		AnimCasterStaged s;
		s.key = c.key;
		s.topoSig = c.topoSig;
		memcpy( s.transform, c.transform, sizeof( s.transform ) );
		s.mask = c.mask;
		s.geomFirst = (int)animStagedGeoms.size();
		s.geomCount = c.geomCount;
		for ( int g = 0; g < c.geomCount; g++ ) {
			animStagedGeoms.push_back( c.geoms[g] );
		}
		animStaged.push_back( s );
	}
}

// grace window before a caster gone from view is retired: covers brief off-screen dips / peeking so a
// re-appearing monster refits its kept BLAS instead of paying a rebuild. Ample; retires stay rare.
static const int ANIM_RETIRE_GRACE = 60;

void VulkanBackend::RefreshAnimBlas() {
	if ( !SupportsRayQuery() || !frameOpen || skipFrame || insideScenePass || insideTargetPass ) {
		return;
	}
	if ( animStaged.empty() && animBlasCache.empty() ) {
		return;
	}
	animBlasFrameCounter++;
	animPendInst.clear();
	animPendGeoFirst.clear();
	animPendGeoCount.clear();
	VkCommandBuffer cb = frames[frameIndex].cb;
	bool anyWrite = false;
	for ( size_t i = 0; i < animStaged.size(); i++ ) {
		const AnimCasterStaged &s = animStaged[i];
		const BlasGeometry *geoms = animStagedGeoms.data() + s.geomFirst;
		AnimBlas *e = FindOrAllocAnimBlas( s.key );
		if ( e->as == VK_NULL_HANDLE || e->topoSig != s.topoSig ) {
			// first sight or topology change (model swap / LOD / gpuSkinVB realloc): (re)build
			if ( e->as != VK_NULL_HANDLE ) {
				RetireAnimBlas( *e );
				e->key = s.key;		// RetireAnimBlas cleared it; this slot is still this entity's
			}
			if ( RecordAnimBlasBuild( *e, geoms, s.geomCount, cb ) ) {
				e->topoSig = s.topoSig;
				animStatBuilds++;
				anyWrite = true;
			}
		} else {
			RecordAnimBlasRefit( *e, geoms, s.geomCount, cb );
			animStatRefits++;
			anyWrite = true;
		}
		e->lastSeenFrame = animBlasFrameCounter;
		// S4: collect this caster's TLAS instance (model-space BLAS + its model->world transform).
		// Copy by value now — e may dangle after the next FindOrAllocAnimBlas grows the cache vector.
		if ( e->as != VK_NULL_HANDLE && e->addr != 0 ) {
			VkAccelerationStructureInstanceKHR vi;
			memset( &vi, 0, sizeof( vi ) );
			memcpy( &vi.transform, s.transform, sizeof( s.transform ) );	// row-major 3x4
			vi.mask = s.mask & 0xFFu;
			vi.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
			vi.accelerationStructureReference = e->addr;
			// instanceCustomIndex (RR0 geo-table base row) is assigned in the append block below, where
			// the running row offset is known; record this caster's geometry slice for the row fill.
			animPendInst.push_back( vi );
			animPendGeoFirst.push_back( (uint32_t)s.geomFirst );
			animPendGeoCount.push_back( (uint32_t)s.geomCount );
		}
	}
	// one batched AS-write -> AS-read barrier for every build/refit above, ordering them before the
	// TLAS build below / any ray read this frame. Disjoint BLASes need no barriers between each other.
	if ( anyWrite ) {
		VkMemoryBarrier mb = {};
		mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		mb.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
		mb.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
		vkCmdPipelineBarrier( cb, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
			VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &mb, 0, NULL, 0, NULL );
	}
	// retire entries not seen this frame beyond the grace window (fence-safe deferred destroy)
	for ( size_t i = 0; i < animBlasCache.size(); i++ ) {
		AnimBlas &e = animBlasCache[i];
		if ( e.key != 0 && e.as != VK_NULL_HANDLE && animBlasFrameCounter - e.lastSeenFrame > ANIM_RETIRE_GRACE ) {
			RetireAnimBlas( e );
			animStatRetires++;
		}
	}
	// S4: UpdateTlas deferred this slot's TLAS build to us. Append one instance per animated caster
	// after the static+mover instances it already wrote, then record the build on the frame cb (reads
	// the just-built/refit BLASes past the barrier above; ends with the AS-build -> fragment ray-read
	// barrier). Always builds when deferred — even with 0 monsters appended — so the slot is never left
	// a frame unbuilt. Host writes here land before EndFrame's submit, so the build sees them.
	if ( rtAnimTlasDeferredSlot == frameIndex ) {
		const int slot = frameIndex;
		if ( rtFrameTlas[slot] != VK_NULL_HANDLE && rtFrameInstBuf[slot].map != NULL ) {
			VkAccelerationStructureInstanceKHR *dst = (VkAccelerationStructureInstanceKHR *)rtFrameInstBuf[slot].map;
			RtGeoDesc *grows = ( rtFrameGeoTable[slot].map != NULL ) ? (RtGeoDesc *)rtFrameGeoTable[slot].map : NULL;
			uint32_t nOut = rtFrameCount[slot];
			uint32_t gRow = rtFrameGeoRows[slot];		// = static rows; monster rows begin here (RR0)
			uint32_t monsterRows = 0;
			for ( size_t i = 0; i < animPendInst.size() && nOut < rtFrameCapacity[slot]; i++ ) {
				// RR0: fill this monster's geometry rows [gRow .. gRow+geomCount) with its gpuSkinVB/index
				// device addresses, and point its instanceCustomIndex at the base row so the reflection
				// shader resolves table[customIndex + geometryIndex] at a hit.
				const uint32_t base = gRow;
				const uint32_t gc = animPendGeoCount[i];
				if ( grows ) {
					for ( uint32_t g = 0; g < gc && base + g < rtFrameGeoCap[slot]; g++ ) {
						const BlasGeometry &bg = animStagedGeoms[animPendGeoFirst[i] + g];
						grows[base + g].vtxAddr = bg.vertexAddress;
						grows[base + g].idxAddr = bg.indexAddress;
						grows[base + g].vtxStride = bg.vertexStride;
						grows[base + g].flags = RT_GEO_MONSTER;
						grows[base + g].baseColor = bg.baseColor;	// RR3: material average colour
						grows[base + g].texIndex = bg.texIndex;		// RR4: diffuse bindless slot for this surface
					}
				}
				animPendInst[i].instanceCustomIndex = base;
				dst[nOut++] = animPendInst[i];
				gRow += gc;
				monsterRows += gc;
			}
			rtFrameCount[slot] = nOut;
			rtFrameGeoRows[slot] = gRow;
			rtReflStatRows = (int)gRow;
			rtReflStatMonsterRows = (int)monsterRows;
			RecordFrameTlasBuild( cb, slot );
			rtFrameBuilt[slot] = true;
		}
		rtAnimTlasDeferredSlot = -1;
	}
}

void VulkanBackend::AnimBlasStats( int &builds, int &refits, int &retires, int &live ) {
	builds = animStatBuilds;
	refits = animStatRefits;
	retires = animStatRetires;
	live = 0;
	for ( size_t i = 0; i < animBlasCache.size(); i++ ) {
		if ( animBlasCache[i].as != VK_NULL_HANDLE ) { live++; }
	}
}

unsigned long long VulkanBackend::BuildTlas( const RtInstance *instances, int count ) {
	if ( !SupportsRayQuery() || instances == NULL || count <= 0 ) {
		return 0;
	}
	// replace any previous TLAS (synchronous path, so the idle guarantees it isn't in flight)
	if ( rtTlas != VK_NULL_HANDLE ) {
		vkQueueWaitIdle( gfxQueue );
		pfnDestroyAs( device, rtTlas, NULL );
		rtTlas = VK_NULL_HANDLE;
	}
	DestroyRtBuffer( rtTlasBuf );
	rtTlasAddr = 0;
	return BuildTlasInto( instances, count, rtTlas, rtTlasBuf, rtTlasAddr );
}

// Translate RtInstance[] to VK instances (dead BLAS handles drop out), upload, and build ONE TLAS
// synchronously into the caller's target objects. Shared by BuildTlas (persistent scene) and
// BuildStandaloneTlas (throwaway validator TLAS). The caller must have cleared the target first.
unsigned long long VulkanBackend::BuildTlasInto( const RtInstance *instances, int count,
		VkAccelerationStructureKHR &tlas, RtBuf &tlasBuf, VkDeviceAddress &tlasAddr ) {
	std::vector<VkAccelerationStructureInstanceKHR> vkInst( (size_t)count );
	uint32_t live = 0;
	for ( int i = 0; i < count; i++ ) {
		const RtInstance &in = instances[i];
		if ( in.blas == 0 || (size_t)in.blas > rtBlases.size() || rtBlases[in.blas - 1].as == VK_NULL_HANDLE ) {
			continue;
		}
		VkAccelerationStructureInstanceKHR &out = vkInst[live++];
		memset( &out, 0, sizeof( out ) );
		memcpy( &out.transform, in.transform, sizeof( in.transform ) );	// both row-major 3x4
		out.mask = in.mask & 0xFFu;
		// facing cull off: shadow rays must block on geometry seen from either side
		out.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
		out.accelerationStructureReference = rtBlases[in.blas - 1].addr;
	}
	if ( live == 0 ) {
		return 0;
	}
	RtBuf instBuf;
	if ( !CreateRtBuffer( VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
			(VkDeviceSize)live * sizeof( VkAccelerationStructureInstanceKHR ), vkInst.data(), instBuf ) ) {
		return 0;
	}
	VkAccelerationStructureGeometryKHR geom = {};
	geom.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
	geom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
	geom.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
	geom.geometry.instances.arrayOfPointers = VK_FALSE;
	geom.geometry.instances.data.deviceAddress = instBuf.addr;
	const bool ok = BuildAsSync( VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, geom, live, tlasBuf, tlas );
	DestroyRtBuffer( instBuf );
	if ( !ok ) {
		tlas = VK_NULL_HANDLE;
		return 0;
	}
	VkAccelerationStructureDeviceAddressInfoKHR dai = {};
	dai.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
	dai.accelerationStructure = tlas;
	tlasAddr = pfnGetAsDeviceAddress( device, &dai );
	return (unsigned long long)tlasAddr;
}

// Build a throwaway TLAS into dedicated members, WITHOUT touching the persistent rtTlas / per-frame
// slots, so the r_rtAnimBlasTest validator can trace one monster's test BLAS while the live scene
// stays intact. Synchronous; released by DestroyStandaloneTlas.
unsigned long long VulkanBackend::BuildStandaloneTlas( const RtInstance *instances, int count ) {
	if ( !SupportsRayQuery() || instances == NULL || count <= 0 ) {
		return 0;
	}
	DestroyStandaloneTlas();
	return BuildTlasInto( instances, count, rtTestTlas, rtTestTlasBuf, rtTestTlasAddr );
}

void VulkanBackend::DestroyStandaloneTlas() {
	if ( rtTestTlas != VK_NULL_HANDLE ) {
		vkQueueWaitIdle( gfxQueue );
		pfnDestroyAs( device, rtTestTlas, NULL );
		rtTestTlas = VK_NULL_HANDLE;
	}
	DestroyRtBuffer( rtTestTlasBuf );
	rtTestTlasAddr = 0;
}

unsigned long long VulkanBackend::GetTlasAddress() {
	// the per-frame slot when its lane is live, else the synchronous scene
	return (unsigned long long)( rtCurrentAddr ? rtCurrentAddr : rtTlasAddr );
}

unsigned long long VulkanBackend::GetStaticTlasAddress() {
	return (unsigned long long)rtTlasAddr;		// synchronous scene only (no movers/monsters)
}

// Record the armed slot's dynamic (monster) BLAS rebuild + a barrier ordering its writes
// against the TLAS build that reads it (both in the AS-build stage). Shared by BeginFrame
// (async steady state) and the fresh-slot synchronous prime.
void VulkanBackend::RecordFrameDynBlasBuild( VkCommandBuffer cb, int slot ) {
	VkAccelerationStructureGeometryKHR geom = {};
	geom.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
	geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
	geom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
	geom.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
	geom.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
	geom.geometry.triangles.vertexData.deviceAddress = rtFrameDynVb[slot].addr;
	geom.geometry.triangles.vertexStride = 3 * sizeof( float );
	geom.geometry.triangles.maxVertex = ( rtFrameDynVerts[slot] > 0 ) ? rtFrameDynVerts[slot] - 1 : 0;
	geom.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
	geom.geometry.triangles.indexData.deviceAddress = rtFrameDynIb[slot].addr;
	VkAccelerationStructureBuildGeometryInfoKHR bgi = {};
	bgi.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
	bgi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
	bgi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
	bgi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	bgi.geometryCount = 1;
	bgi.pGeometries = &geom;
	bgi.dstAccelerationStructure = rtFrameDynBlas[slot];
	bgi.scratchData.deviceAddress = ( rtFrameDynScratch[slot].addr + asScratchAlignment - 1 )
		& ~(VkDeviceAddress)( asScratchAlignment - 1 );
	VkAccelerationStructureBuildRangeInfoKHR range = {};
	range.primitiveCount = rtFrameDynTris[slot];
	const VkAccelerationStructureBuildRangeInfoKHR *pRange = &range;
	pfnCmdBuildAs( cb, 1, &bgi, &pRange );
	// the BLAS write must complete before the TLAS build (later on this cb) reads it
	VkMemoryBarrier mb = {};
	mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	mb.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
	mb.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
	vkCmdPipelineBarrier( cb, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
		VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &mb, 0, NULL, 0, NULL );
}

// Per-frame animated casters (monsters, R3): rebuild ONE combined world-space triangle BLAS
// into the upcoming frame slot from the soup the frontend supplies (all visible monsters,
// verts already transformed to world space). Mirrors UpdateTlas's slot lifecycle: wait the
// slot fence, (re)create the slot's dyn-BLAS on first use / growth, upload the geometry, then
// prime synchronously on a fresh slot (so a fresh TLAS prime finds it built) or arm the
// frame-cb rebuild for steady state. UpdateTlas (called right after) appends the identity
// instance for it via the rtDynArmedSlot handshake.
void VulkanBackend::UpdateDynamicGeometry( const float *worldPositions, int numVerts,
		const int *indexes, int numIndexes ) {
	if ( !SupportsRayQuery() || worldPositions == NULL || indexes == NULL
		|| numVerts <= 0 || numIndexes < 3 || frameOpen ) {
		return;
	}
	const int slot = frameIndex;
	const uint32_t nv = (uint32_t)numVerts;
	const uint32_t nt = (uint32_t)( numIndexes / 3 );

	vkWaitForFences( device, 1, &frames[slot].fence, VK_TRUE, UINT64_MAX );

	// (re)create the slot's dyn-BLAS when absent or outgrown (growth queue-idles; rare)
	if ( rtFrameDynBlas[slot] == VK_NULL_HANDLE || nv > rtFrameDynVertCap[slot] || nt > rtFrameDynTriCap[slot] ) {
		vkQueueWaitIdle( gfxQueue );
		if ( rtFrameDynBlas[slot] != VK_NULL_HANDLE ) {
			pfnDestroyAs( device, rtFrameDynBlas[slot], NULL );
			rtFrameDynBlas[slot] = VK_NULL_HANDLE;
		}
		DestroyRtBuffer( rtFrameDynBlasBuf[slot] );
		DestroyRtBuffer( rtFrameDynVb[slot] );
		DestroyRtBuffer( rtFrameDynIb[slot] );
		DestroyRtBuffer( rtFrameDynScratch[slot] );
		rtFrameDynVertCap[slot] = rtFrameDynTriCap[slot] = 0;
		rtFrameDynAddr[slot] = 0;
		rtFrameDynBuilt[slot] = false;

		const uint32_t vcap = nv + nv / 2 + 64;
		const uint32_t tcap = nt + nt / 2 + 64;
		if ( !CreateRtBuffer( VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
				(VkDeviceSize)vcap * 3 * sizeof( float ), NULL, rtFrameDynVb[slot] )
			|| !CreateRtBuffer( VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
				(VkDeviceSize)tcap * 3 * sizeof( int ), NULL, rtFrameDynIb[slot] ) ) {
			DestroyRtBuffer( rtFrameDynVb[slot] );
			DestroyRtBuffer( rtFrameDynIb[slot] );
			return;
		}
		// size the AS + scratch for the worst-case tcap so a steady rebuild never needs to grow
		VkAccelerationStructureGeometryKHR sgeom = {};
		sgeom.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
		sgeom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
		sgeom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
		sgeom.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
		sgeom.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
		sgeom.geometry.triangles.vertexStride = 3 * sizeof( float );
		sgeom.geometry.triangles.maxVertex = vcap - 1;
		sgeom.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
		VkAccelerationStructureBuildGeometryInfoKHR sbgi = {};
		sbgi.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
		sbgi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
		sbgi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
		sbgi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
		sbgi.geometryCount = 1;
		sbgi.pGeometries = &sgeom;
		VkAccelerationStructureBuildSizesInfoKHR sizes = {};
		sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
		pfnGetAsBuildSizes( device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &sbgi, &tcap, &sizes );
		if ( !CreateRtBuffer( VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, sizes.accelerationStructureSize, NULL, rtFrameDynBlasBuf[slot] )
			|| !CreateRtBuffer( VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, sizes.buildScratchSize + asScratchAlignment, NULL, rtFrameDynScratch[slot] ) ) {
			DestroyRtBuffer( rtFrameDynBlasBuf[slot] );
			DestroyRtBuffer( rtFrameDynVb[slot] );
			DestroyRtBuffer( rtFrameDynIb[slot] );
			DestroyRtBuffer( rtFrameDynScratch[slot] );
			return;
		}
		VkAccelerationStructureCreateInfoKHR asci = {};
		asci.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
		asci.buffer = rtFrameDynBlasBuf[slot].buf;
		asci.size = sizes.accelerationStructureSize;
		asci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
		if ( !vkCheck( pfnCreateAs( device, &asci, NULL, &rtFrameDynBlas[slot] ), "vkCreateAccelerationStructureKHR(dynBlas)" ) ) {
			rtFrameDynBlas[slot] = VK_NULL_HANDLE;
			DestroyRtBuffer( rtFrameDynBlasBuf[slot] );
			DestroyRtBuffer( rtFrameDynVb[slot] );
			DestroyRtBuffer( rtFrameDynIb[slot] );
			DestroyRtBuffer( rtFrameDynScratch[slot] );
			return;
		}
		VkAccelerationStructureDeviceAddressInfoKHR dai = {};
		dai.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
		dai.accelerationStructure = rtFrameDynBlas[slot];
		rtFrameDynAddr[slot] = pfnGetAsDeviceAddress( device, &dai );
		rtFrameDynVertCap[slot] = vcap;
		rtFrameDynTriCap[slot] = tcap;
	}

	memcpy( rtFrameDynVb[slot].map, worldPositions, (size_t)nv * 3 * sizeof( float ) );
	memcpy( rtFrameDynIb[slot].map, indexes, (size_t)numIndexes * sizeof( int ) );
	rtFrameDynVerts[slot] = nv;
	rtFrameDynTris[slot] = nt;
	rtDynArmedSlot = slot;			// tell the following UpdateTlas to append the identity instance

	if ( !rtFrameDynBuilt[slot] ) {
		// fresh slot: prime synchronously so a fresh TLAS prime that references this BLAS
		// finds it built (steady-state frames rebuild it on the frame cb instead)
		vkResetCommandBuffer( uploadCb, 0 );
		VkCommandBufferBeginInfo bbi = {};
		bbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		bbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer( uploadCb, &bbi );
		RecordFrameDynBlasBuild( uploadCb, slot );
		vkEndCommandBuffer( uploadCb );
		VkSubmitInfo si = {};
		si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		si.commandBufferCount = 1;
		si.pCommandBuffers = &uploadCb;
		vkResetFences( device, 1, &uploadFence );
		vkQueueSubmit( gfxQueue, 1, &si, uploadFence );
		vkWaitForFences( device, 1, &uploadFence, VK_TRUE, UINT64_MAX );
		rtFrameDynBuilt[slot] = true;
		rtDynPendingSlot = -1;			// fresh slot already holds this frame's geometry
	} else {
		// steady slot: BeginFrame rebuilds this frame's geometry on the frame cb, AHEAD of the
		// TLAS build that references it (RecordFrameDynBlasBuild ends with the AS-write->read barrier)
		rtDynPendingSlot = slot;
	}
}

// Per-frame TLAS refresh (movers): translate + upload the instances into the upcoming
// frame slot's buffers and arm it; BeginFrame records the actual build. Called from the
// frontend BETWEEN frames (frameOpen false), when frameIndex already names the slot the
// next BeginFrame will use (Present advances it). The slot's fence is waited before its
// buffers are touched - its last GPU use was FRAMES_IN_FLIGHT frames ago, so this is the
// same (normally already-signaled) wait BeginFrame would do anyway, just earlier.
void VulkanBackend::UpdateTlas( const RtInstance *instances, int count ) {
	if ( !SupportsRayQuery() || instances == NULL || count <= 0 || frameOpen ) {
		return;			// mid-frame callers skip a beat; the next frame re-arms
	}
	const int slot = frameIndex;
	// one-shot consume of the dyn-caster arm (set by UpdateDynamicGeometry this frame)
	const bool appendDyn = ( rtDynArmedSlot == slot );
	rtDynArmedSlot = -1;

	// translate to VK instances (same rules as BuildTlas: dead BLAS handles drop out)
	std::vector<VkAccelerationStructureInstanceKHR> vkInst;
	std::vector<BlasHandle> instBlas;					// RR5: parallel to vkInst — the BLAS behind each instance (0 = none)
	vkInst.reserve( (size_t)count + 1 );
	instBlas.reserve( (size_t)count + 1 );
	uint32_t geoBase = 0;								// RR5: running geometry-table row (per-geometry, not per-instance)
	for ( int i = 0; i < count; i++ ) {
		const RtInstance &in = instances[i];
		if ( in.blas == 0 || (size_t)in.blas > rtBlases.size() || rtBlases[in.blas - 1].as == VK_NULL_HANDLE ) {
			continue;
		}
		VkAccelerationStructureInstanceKHR vi;
		memset( &vi, 0, sizeof( vi ) );
		memcpy( &vi.transform, in.transform, sizeof( in.transform ) );
		vi.mask = in.mask & 0xFFu;
		vi.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
		vi.accelerationStructureReference = rtBlases[in.blas - 1].addr;
		// RR0/RR5: instanceCustomIndex is this instance's BASE geometry-table row. A device-buffer-fed BLAS
		// (RR5 world surfaces) contributes one attributed row per geometry; a positions-only BLAS one defer row.
		const uint32_t rows = rtBlases[in.blas - 1].geoms.empty() ? 1u : (uint32_t)rtBlases[in.blas - 1].geoms.size();
		vi.instanceCustomIndex = geoBase;
		geoBase += rows;
		vkInst.push_back( vi );
		instBlas.push_back( in.blas );
	}
	// R3 animated casters: one identity instance for this slot's combined WORLD-space monster
	// BLAS (verts pre-transformed on the CPU, so no per-instance transform). Appended BEFORE the
	// empty check so a monster-only view still builds a TLAS.
	if ( appendDyn && rtFrameDynBlas[slot] != VK_NULL_HANDLE && rtFrameDynTris[slot] > 0 ) {
		VkAccelerationStructureInstanceKHR vi;
		memset( &vi, 0, sizeof( vi ) );
		vi.transform.matrix[0][0] = 1.0f;
		vi.transform.matrix[1][1] = 1.0f;
		vi.transform.matrix[2][2] = 1.0f;
		vi.mask = 0xFFu;
		vi.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
		vi.accelerationStructureReference = rtFrameDynAddr[slot];
		vi.instanceCustomIndex = geoBase;					// RR0: zero-attr defer row (CPU-soup positions only)
		geoBase += 1u;
		vkInst.push_back( vi );
		instBlas.push_back( 0 );							// RR5: no stored descriptors -> defer row
	}
	// R3.5 S4: RefreshAnimBlas will append one instance per staged animated caster to this slot after
	// the skin flush. Reserve capacity for them now and keep going even if the static+mover set is empty
	// (a monster-only view still needs a slot to append into).
	const bool willDefer = !animStaged.empty();
	if ( vkInst.empty() && !willDefer ) {
		return;
	}
	const uint32_t n = (uint32_t)vkInst.size();
	const uint32_t nCap = n + ( willDefer ? (uint32_t)animStaged.size() : 0u );
	// RR0: geometry-table rows = these n instances (1 row each) + every staged monster surface
	// (RefreshAnimBlas appends those rows). Size the table for both.
	uint32_t sumGeom = 0;
	for ( size_t i = 0; i < animStaged.size(); i++ ) { sumGeom += (uint32_t)animStaged[i].geomCount; }
	const uint32_t geoRowsNeeded = geoBase + sumGeom;	// RR5: static rows are now per-geometry, not per-instance

	vkWaitForFences( device, 1, &frames[slot].fence, VK_TRUE, UINT64_MAX );

	// (re)create the slot's resources when it has none or the (static+animated) count outgrew them.
	// Growth stalls the queue - rare (entity counts are near-constant within a map).
	if ( rtFrameTlas[slot] == VK_NULL_HANDLE || nCap > rtFrameCapacity[slot] ) {
		vkQueueWaitIdle( gfxQueue );
		if ( rtFrameTlas[slot] != VK_NULL_HANDLE ) {
			pfnDestroyAs( device, rtFrameTlas[slot], NULL );
			rtFrameTlas[slot] = VK_NULL_HANDLE;
		}
		DestroyRtBuffer( rtFrameTlasBuf[slot] );
		DestroyRtBuffer( rtFrameInstBuf[slot] );
		DestroyRtBuffer( rtFrameScratch[slot] );
		rtFrameCapacity[slot] = 0;
		rtFrameAddr[slot] = 0;
		rtFrameBuilt[slot] = false;			// fresh object: must prime before anything traverses it

		const uint32_t cap = nCap + nCap / 2 + 16;
		if ( !CreateRtBuffer( VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
				(VkDeviceSize)cap * sizeof( VkAccelerationStructureInstanceKHR ), NULL, rtFrameInstBuf[slot] ) ) {
			return;
		}
		VkAccelerationStructureGeometryKHR geom = {};
		geom.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
		geom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
		geom.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
		geom.geometry.instances.arrayOfPointers = VK_FALSE;
		VkAccelerationStructureBuildGeometryInfoKHR bgi = {};
		bgi.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
		bgi.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
		bgi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
		bgi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
		bgi.geometryCount = 1;
		bgi.pGeometries = &geom;
		VkAccelerationStructureBuildSizesInfoKHR sizes = {};
		sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
		pfnGetAsBuildSizes( device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bgi, &cap, &sizes );
		if ( !CreateRtBuffer( VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, sizes.accelerationStructureSize, NULL, rtFrameTlasBuf[slot] )
			|| !CreateRtBuffer( VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, sizes.buildScratchSize + asScratchAlignment, NULL, rtFrameScratch[slot] ) ) {
			DestroyRtBuffer( rtFrameTlasBuf[slot] );
			DestroyRtBuffer( rtFrameInstBuf[slot] );
			DestroyRtBuffer( rtFrameScratch[slot] );
			return;
		}
		VkAccelerationStructureCreateInfoKHR asci = {};
		asci.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
		asci.buffer = rtFrameTlasBuf[slot].buf;
		asci.size = sizes.accelerationStructureSize;
		asci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
		if ( !vkCheck( pfnCreateAs( device, &asci, NULL, &rtFrameTlas[slot] ), "vkCreateAccelerationStructureKHR(frame)" ) ) {
			rtFrameTlas[slot] = VK_NULL_HANDLE;
			DestroyRtBuffer( rtFrameTlasBuf[slot] );
			DestroyRtBuffer( rtFrameInstBuf[slot] );
			DestroyRtBuffer( rtFrameScratch[slot] );
			return;
		}
		VkAccelerationStructureDeviceAddressInfoKHR dai = {};
		dai.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
		dai.accelerationStructure = rtFrameTlas[slot];
		rtFrameAddr[slot] = pfnGetAsDeviceAddress( device, &dai );
		rtFrameCapacity[slot] = cap;
	}

	if ( n > 0 ) {
		memcpy( rtFrameInstBuf[slot].map, vkInst.data(), (size_t)n * sizeof( VkAccelerationStructureInstanceKHR ) );
	}
	rtFrameCount[slot] = n;
	// RR0: (re)size the geometry table + zero the static/mover/dyn-soup rows (no attributes — SSR and
	// env probes own those pixels). RefreshAnimBlas fills the monster rows [n .. n+sumGeom) after skin.
	EnsureFrameGeoTable( slot, geoRowsNeeded > 0 ? geoRowsNeeded : 1 );
	if ( rtFrameGeoTable[slot].map != NULL ) {
		RtGeoDesc *rows = (RtGeoDesc *)rtFrameGeoTable[slot].map;
		uint32_t base = 0;
		for ( size_t i = 0; i < vkInst.size() && base < geoBase; i++ ) {
			const BlasHandle bh = instBlas[i];
			const std::vector<BlasGeometry> *g = ( bh != 0 ) ? &rtBlases[bh - 1].geoms : NULL;
			if ( g != NULL && !g->empty() ) {
				// RR5: a device-buffer-fed BLAS (world surfaces) — one attributed row per geometry, so a
				// reflection ray resolves table[customIndex + geometryIndex] and fetches st + material.
				for ( size_t k = 0; k < g->size() && base < geoBase; k++ ) {
					const BlasGeometry &bgeo = (*g)[k];
					rows[base].vtxAddr = bgeo.vertexAddress;
					rows[base].idxAddr = bgeo.indexAddress;
					rows[base].vtxStride = bgeo.vertexStride;
					rows[base].flags = RT_GEO_MONSTER;			// bit0 = has attributes -> shade the hit (RR5b)
					rows[base].baseColor = bgeo.baseColor;
					rows[base].texIndex = bgeo.texIndex;
					base++;
				}
			} else {
				// positions-only (movers) / dyn-soup: one zero defer row — SSR + env probes own those pixels
				rows[base].vtxAddr = 0; rows[base].idxAddr = 0; rows[base].vtxStride = 0;
				rows[base].flags = 0; rows[base].baseColor = 0; rows[base].texIndex = 0;
				base++;
			}
		}
	}
	rtFrameGeoRows[slot] = geoBase;
	rtReflStatRows = (int)geoBase;		// static rows now; RefreshAnimBlas adds the monster rows + updates
	rtReflStatMonsterRows = 0;
	if ( willDefer ) {
		// R3.5 S4: hand this slot to RefreshAnimBlas (post skin flush) — it appends the animated
		// instances after rtFrameCount[slot] and records the TLAS build on the frame cb, so the TLAS
		// captures the current-frame pose. No sync prime, no BeginFrame arm; RefreshAnimBlas always
		// builds this slot (even with 0 monsters appended), so the slot never goes a frame unbuilt.
		rtAnimTlasDeferredSlot = slot;
		rtPendingSlot = -1;
	} else if ( !rtFrameBuilt[slot] ) {
		// prime a freshly created slot SYNCHRONOUSLY: out-of-band readers (the validator's
		// DispatchSync executes before this frame's cb) must never traverse a never-built
		// TLAS. Once per slot per (re)creation; steady-state frames take the async path.
		vkResetCommandBuffer( uploadCb, 0 );
		VkCommandBufferBeginInfo bbi = {};
		bbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		bbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer( uploadCb, &bbi );
		RecordFrameTlasBuild( uploadCb, slot );
		vkEndCommandBuffer( uploadCb );
		VkSubmitInfo si = {};
		si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		si.commandBufferCount = 1;
		si.pCommandBuffers = &uploadCb;
		vkResetFences( device, 1, &uploadFence );
		vkQueueSubmit( gfxQueue, 1, &si, uploadFence );
		vkWaitForFences( device, 1, &uploadFence, VK_TRUE, UINT64_MAX );
		rtFrameBuilt[slot] = true;
		rtPendingSlot = -1;					// the slot already holds this frame's poses
	} else {
		rtPendingSlot = slot;				// BeginFrame records the rebuild ahead of the draws
	}
	rtCurrentAddr = rtFrameAddr[slot];		// this frame's mode-4 parms read this slot
	rtCurrentGeoAddr = rtFrameGeoTable[slot].addr;	// RR2: geometry table matching this slot's TLAS
}

// RR0: (re)create the slot's RtGeoDesc table when absent or outgrown. Host-visible + device-addressable
// (CreateRtBuffer adds SHADER_DEVICE_ADDRESS) so the reflection shader reads it via buffer_reference.
// Called after the slot's fence wait, so the old table is not in flight when destroyed.
bool VulkanBackend::EnsureFrameGeoTable( int slot, uint32_t rows ) {
	if ( rtFrameGeoTable[slot].buf != VK_NULL_HANDLE && rows <= rtFrameGeoCap[slot] ) {
		return true;
	}
	DestroyRtBuffer( rtFrameGeoTable[slot] );
	rtFrameGeoCap[slot] = 0;
	const uint32_t cap = rows + rows / 2 + 16;
	if ( !CreateRtBuffer( VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, (VkDeviceSize)cap * sizeof( RtGeoDesc ), NULL, rtFrameGeoTable[slot] ) ) {
		return false;
	}
	rtFrameGeoCap[slot] = cap;
	return true;
}

void VulkanBackend::RtReflStats( int &geoRows, int &geoMonsterRows ) {
	geoRows = rtReflStatRows;
	geoMonsterRows = rtReflStatMonsterRows;
}

// Record the armed slot's TLAS rebuild + the barrier ordering it against fragment-shader
// ray reads. Shared by BeginFrame (async lane) and the synchronous slot-priming path.
void VulkanBackend::RecordFrameTlasBuild( VkCommandBuffer cb, int slot ) {
	VkAccelerationStructureGeometryKHR geom = {};
	geom.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
	geom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
	geom.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
	geom.geometry.instances.arrayOfPointers = VK_FALSE;
	geom.geometry.instances.data.deviceAddress = rtFrameInstBuf[slot].addr;
	VkAccelerationStructureBuildGeometryInfoKHR bgi = {};
	bgi.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
	bgi.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
	bgi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
	bgi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	bgi.geometryCount = 1;
	bgi.pGeometries = &geom;
	bgi.dstAccelerationStructure = rtFrameTlas[slot];
	bgi.scratchData.deviceAddress = ( rtFrameScratch[slot].addr + asScratchAlignment - 1 )
		& ~(VkDeviceAddress)( asScratchAlignment - 1 );
	VkAccelerationStructureBuildRangeInfoKHR range = {};
	range.primitiveCount = rtFrameCount[slot];
	const VkAccelerationStructureBuildRangeInfoKHR *pRange = &range;
	pfnCmdBuildAs( cb, 1, &bgi, &pRange );
	VkMemoryBarrier mb = {};
	mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	mb.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
	mb.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
	vkCmdPipelineBarrier( cb, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL );
}

void VulkanBackend::DestroyRtFrameSlots() {
	for ( int s = 0; s < FRAMES_IN_FLIGHT; s++ ) {
		if ( rtFrameTlas[s] != VK_NULL_HANDLE ) {
			pfnDestroyAs( device, rtFrameTlas[s], NULL );
			rtFrameTlas[s] = VK_NULL_HANDLE;
		}
		DestroyRtBuffer( rtFrameTlasBuf[s] );
		DestroyRtBuffer( rtFrameInstBuf[s] );
		DestroyRtBuffer( rtFrameScratch[s] );
		DestroyRtBuffer( rtFrameGeoTable[s] );		// RR0 per-instance geometry table
		rtFrameCapacity[s] = 0;
		rtFrameCount[s] = 0;
		rtFrameGeoCap[s] = 0;
		rtFrameGeoRows[s] = 0;
		rtFrameAddr[s] = 0;
		rtFrameBuilt[s] = false;
		// R3 animated-caster dyn-BLAS slot
		if ( rtFrameDynBlas[s] != VK_NULL_HANDLE ) {
			pfnDestroyAs( device, rtFrameDynBlas[s], NULL );
			rtFrameDynBlas[s] = VK_NULL_HANDLE;
		}
		DestroyRtBuffer( rtFrameDynBlasBuf[s] );
		DestroyRtBuffer( rtFrameDynVb[s] );
		DestroyRtBuffer( rtFrameDynIb[s] );
		DestroyRtBuffer( rtFrameDynScratch[s] );
		rtFrameDynVertCap[s] = rtFrameDynTriCap[s] = 0;
		rtFrameDynVerts[s] = rtFrameDynTris[s] = 0;
		rtFrameDynAddr[s] = 0;
		rtFrameDynBuilt[s] = false;
	}
	rtPendingSlot = -1;
	rtDynArmedSlot = -1;
	rtDynPendingSlot = -1;
	rtAnimTlasDeferredSlot = -1;
	rtCurrentAddr = 0;
	rtCurrentGeoAddr = 0;
}

void VulkanBackend::DestroyRtScene() {
	if ( device == VK_NULL_HANDLE ) {
		return;
	}
	if ( rtTlas != VK_NULL_HANDLE || !rtBlases.empty() ) {
		vkQueueWaitIdle( gfxQueue );
	}
	if ( rtTlas != VK_NULL_HANDLE ) {
		pfnDestroyAs( device, rtTlas, NULL );
		rtTlas = VK_NULL_HANDLE;
	}
	DestroyRtBuffer( rtTlasBuf );
	rtTlasAddr = 0;
	DestroyStandaloneTlas();		// R3.5 validator TLAS (no-op if none live)
	// R3.5 S3: tear down the per-entity animated BLAS cache (idle already waited above)
	for ( size_t i = 0; i < animBlasCache.size(); i++ ) {
		RetireAnimBlas( animBlasCache[i] );
	}
	animBlasCache.clear();
	animStaged.clear();
	animStagedGeoms.clear();
	DrainRetiredAnimAs( true );		// device is idle -> destroy them all now
	DestroyRtFrameSlots();
	for ( size_t i = 0; i < rtBlases.size(); i++ ) {
		if ( rtBlases[i].as != VK_NULL_HANDLE ) {
			pfnDestroyAs( device, rtBlases[i].as, NULL );
		}
		DestroyRtBuffer( rtBlases[i].buf );
		DestroyRtBuffer( rtBlases[i].updateScratch );	// R3.5 updatable BLAS scratch (no-op if unset)
	}
	rtBlases.clear();
}

// r_rayQueryTest: validate the R2 acceleration-structure foundation end-to-end BEFORE anything
// renders from it (docs/rtx-shadow-roadmap.md; the house validator-first pattern). Builds a BLAS
// over two known triangles (prim 0 spans the ray grid at z=5; prim 1 sits behind it at z=9,
// offset +x so some rays reach it alone), wraps it in a one-instance TLAS, then traces a 16x16
// grid of +Z rays from a compute shader (GL_EXT_ray_query). The TLAS rides into the shader as a
// raw device address in the push constant via the accelerationStructureEXT(uvec2) conversion
// constructor - the same no-descriptor-changes trick the BDA path uses. Every ray's committed
// t + primitive index is diffed against a CPU Moller-Trumbore reference; the grid deliberately
// covers misses, single-triangle hits, and both-triangle rays (closest-hit commit order), and no
// grid ray grazes a triangle edge (no watertightness ambiguity between GPU and CPU). Synchronous
// one-shot (dev tool) - never call per-frame.
void VulkanBackend::RayQuerySelfTest() {
	if ( device == VK_NULL_HANDLE || computePipeLayout == VK_NULL_HANDLE || uploadCb == VK_NULL_HANDLE ) {
		common->Printf( "VK ray-query self-test: unavailable (no compute lane)\n" );
		return;
	}
	if ( !haveRayQuery ) {
		common->Printf( "VK ray-query self-test: unavailable (device lacks KHR_acceleration_structure + KHR_ray_query)\n" );
		return;
	}

	// prim 0: z=5, spans x/y [-2,2]; prim 1: z=9, x shifted +2.5 so x in (2, 2.8125] hits it alone
	static const float kTris[2][3][3] = {
		{ { -2.0f, -2.0f, 5.0f }, { 2.0f, -2.0f, 5.0f }, { 0.0f, 2.0f, 5.0f } },
		{ {  0.5f, -2.0f, 9.0f }, { 4.5f, -2.0f, 9.0f }, { 2.5f, 2.0f, 9.0f } },
	};
	const int GRID = 16;
	const int N = GRID * GRID;

	// two triangles -> one BLAS -> one-instance TLAS, all through the PUBLIC API (CreateBlas /
	// BuildTlas) so this stays the permanent regression test of the exact path real geometry
	// uses. Identity instance transform; indexes 0..5 (two independent triangles).
	static const int kIdx[6] = { 0, 1, 2, 3, 4, 5 };
	const BlasHandle blasHandle = CreateBlas( &kTris[0][0][0], 6, 3 * (int)sizeof( float ), kIdx, 6 );
	if ( blasHandle == 0 ) {
		common->Printf( "VK ray-query self-test: FAIL (CreateBlas)\n" );
		return;
	}
	RtInstance rtInst = {};
	rtInst.transform[0] = 1.0f;		// row-major 3x4 identity
	rtInst.transform[5] = 1.0f;
	rtInst.transform[10] = 1.0f;
	rtInst.blas = blasHandle;
	rtInst.mask = 0xFF;
	const unsigned long long tlasAddr = BuildTlas( &rtInst, 1 );
	if ( tlasAddr == 0 ) {
		common->Printf( "VK ray-query self-test: FAIL (BuildTlas)\n" );
		DestroyRtScene();
		return;
	}

	// trace the grid from compute; results land in a bound SSBO seeded with sentinels so a
	// silently-dead dispatch reads as a mismatch, never a coincidental pass
	static const char *kSrc =
		"#version 460\n"
		"#extension GL_EXT_ray_query : require\n"
		"layout(local_size_x = 64) in;\n"
		"struct Hit { float t; uint prim; };\n"
		"layout(std430, binding = 0) writeonly buffer Out { Hit hits[]; } outb;\n"
		"layout(push_constant) uniform PC { uvec2 tlas; uint count; } pc;\n"
		"void main() {\n"
		"    uint i = gl_GlobalInvocationID.x;\n"
		"    if ( i >= pc.count ) { return; }\n"
		"    float x = -3.0 + 6.0 * ( float( i % 16u ) + 0.5 ) / 16.0;\n"
		"    float y = -3.0 + 6.0 * ( float( i / 16u ) + 0.5 ) / 16.0;\n"
		"    rayQueryEXT rq;\n"
		"    rayQueryInitializeEXT( rq, accelerationStructureEXT( pc.tlas ), gl_RayFlagsOpaqueEXT, 0xFFu,\n"
		"                           vec3( x, y, 0.0 ), 0.001, vec3( 0.0, 0.0, 1.0 ), 100.0 );\n"
		"    while ( rayQueryProceedEXT( rq ) ) { }\n"
		"    if ( rayQueryGetIntersectionTypeEXT( rq, true ) == gl_RayQueryCommittedIntersectionTriangleEXT ) {\n"
		"        outb.hits[i].t = rayQueryGetIntersectionTEXT( rq, true );\n"
		"        outb.hits[i].prim = uint( rayQueryGetIntersectionPrimitiveIndexEXT( rq, true ) );\n"
		"    } else {\n"
		"        outb.hits[i].t = -1.0;\n"
		"        outb.hits[i].prim = 0xFFFFFFFFu;\n"
		"    }\n"
		"}\n";
	ShaderHandle sh = CreateComputeShader( "cs_rayquerytest", kSrc );
	if ( sh == 0 ) {
		common->Printf( "VK ray-query self-test: FAIL (GL_EXT_ray_query compute shader did not compile)\n" );
		DestroyRtScene();
		return;
	}
	struct GpuHit { float t; uint32_t prim; };
	GpuHit seed[N];
	for ( int i = 0; i < N; i++ ) { seed[i].t = -2.0f; seed[i].prim = 0xDEADBEEFu; }
	BufferHandle out = CreateBuffer( BU_STORAGE, N * (int)sizeof( GpuHit ), seed );
	if ( out == 0 ) {
		common->Printf( "VK ray-query self-test: FAIL (result buffer alloc)\n" );
		DestroyRtScene();
		return;
	}
	struct { uint32_t tlasLo, tlasHi, count; } pc;
	pc.tlasLo = (uint32_t)( tlasAddr & 0xFFFFFFFFu );
	pc.tlasHi = (uint32_t)( tlasAddr >> 32 );
	pc.count = (uint32_t)N;
	ComputeArgs ca = {};
	ca.shader = sh;
	ca.storage[0] = out;
	ca.pushConstants = &pc;
	ca.pushConstantSize = (int)sizeof( pc );
	ca.groupsX = ( N + 63 ) / 64; ca.groupsY = 1; ca.groupsZ = 1;
	DispatchSync( ca );

	GpuHit got[N];
	if ( !ReadBuffer( out, got, N * (int)sizeof( GpuHit ) ) ) {
		common->Printf( "VK ray-query self-test: FAIL (readback failed)\n" );
		DestroyBuffer( out );
		DestroyRtScene();
		return;
	}

	// CPU reference: Moller-Trumbore closest-hit over the same grid, same [tmin,tmax] window
	int mismatches = 0, hit0 = 0, hit1 = 0, miss = 0, firstBad = -1;
	for ( int i = 0; i < N; i++ ) {
		const float ox = -3.0f + 6.0f * ( (float)( i % GRID ) + 0.5f ) / (float)GRID;
		const float oy = -3.0f + 6.0f * ( (float)( i / GRID ) + 0.5f ) / (float)GRID;
		float bestT = 1e30f;
		uint32_t bestPrim = 0xFFFFFFFFu;
		for ( int tri = 0; tri < 2; tri++ ) {
			const float *v0 = kTris[tri][0], *v1 = kTris[tri][1], *v2 = kTris[tri][2];
			const float e1[3] = { v1[0]-v0[0], v1[1]-v0[1], v1[2]-v0[2] };
			const float e2[3] = { v2[0]-v0[0], v2[1]-v0[1], v2[2]-v0[2] };
			// dir = (0,0,1): pvec = cross(dir, e2), det = dot(e1, pvec)
			const float pv[3] = { -e2[1], e2[0], 0.0f };
			const float det = e1[0]*pv[0] + e1[1]*pv[1];
			if ( det > -1e-8f && det < 1e-8f ) {
				continue;
			}
			const float inv = 1.0f / det;
			const float tv[3] = { ox - v0[0], oy - v0[1], 0.0f - v0[2] };
			const float u = ( tv[0]*pv[0] + tv[1]*pv[1] ) * inv;
			if ( u < 0.0f || u > 1.0f ) {
				continue;
			}
			const float qv[3] = { tv[1]*e1[2] - tv[2]*e1[1], tv[2]*e1[0] - tv[0]*e1[2], tv[0]*e1[1] - tv[1]*e1[0] };
			const float w = qv[2] * inv;		// dot(dir, qvec), dir = +Z
			if ( w < 0.0f || u + w > 1.0f ) {
				continue;
			}
			const float t = ( e2[0]*qv[0] + e2[1]*qv[1] + e2[2]*qv[2] ) * inv;
			if ( t > 0.001f && t < 100.0f && t < bestT ) {
				bestT = t;
				bestPrim = (uint32_t)tri;
			}
		}
		const bool refHit = bestPrim != 0xFFFFFFFFu;
		if ( refHit ) { if ( bestPrim == 0 ) { hit0++; } else { hit1++; } } else { miss++; }
		const bool gotHit = got[i].prim != 0xFFFFFFFFu && got[i].t >= 0.0f;
		const bool ok = ( refHit == gotHit )
			&& ( !refHit || ( got[i].prim == bestPrim && got[i].t > bestT - 1e-3f && got[i].t < bestT + 1e-3f ) );
		if ( !ok ) {
			mismatches++;
			if ( firstBad < 0 ) {
				firstBad = i;
			}
		}
	}
	if ( mismatches == 0 && hit0 > 0 && hit1 > 0 && miss > 0 ) {
		common->Printf( "VK ray-query self-test: PASS (256 rays match the CPU reference: %d hit prim 0, %d hit prim 1, %d miss)\n",
			hit0, hit1, miss );
	} else if ( mismatches == 0 ) {
		// all rays agree but a coverage class is empty - the scene setup regressed, not the GPU
		common->Printf( "VK ray-query self-test: FAIL (degenerate coverage: %d/%d/%d hit0/hit1/miss - test scene broken)\n",
			hit0, hit1, miss );
	} else {
		const int i = firstBad;
		common->Printf( "VK ray-query self-test: FAIL (%d/%d rays mismatch; first at ray %d: GPU t=%.4f prim=0x%x)\n",
			mismatches, N, i, got[i].t, got[i].prim );
	}
	DestroyBuffer( out );
	DestroyRtScene();
}

/*
====================
VulkanBackend::Fsr2SelfTest

r_fsr2Test: bring-up validation for the vendored FidelityFX FSR2 Vulkan backend (R1/C0,
docs/fsr-temporal-pipeline.md). Sizes the FSR2 scratch, builds its VK interface against our
real physical/logical device, creates an FSR2 context in Native-AA config (render == display,
no dispatch), then tears it down. Proves the vendored MIT library links and initialises its
compute pipelines on this GPU. No rendering effect; a dev/CI path gated to the cvar edge.
====================
*/
void VulkanBackend::Fsr2SelfTest() {
	if ( physical == VK_NULL_HANDLE || device == VK_NULL_HANDLE ) {
		common->Printf( "FSR2 self-test: unavailable (no VK device)\n" );
		return;
	}

	const size_t scratchSize = ffxFsr2GetScratchMemorySizeVK( physical );
	if ( scratchSize == 0 ) {
		common->Warning( "FSR2 self-test: ffxFsr2GetScratchMemorySizeVK returned 0" );
		return;
	}

	void *scratch = malloc( scratchSize );
	FfxFsr2Context *ctx = (FfxFsr2Context *)malloc( sizeof( FfxFsr2Context ) );
	if ( scratch == NULL || ctx == NULL ) {
		common->Warning( "FSR2 self-test: out of memory (scratch %zu B)", scratchSize );
		free( scratch );
		free( ctx );
		return;
	}

	FfxFsr2ContextDescription desc = {};
	FfxErrorCode err = ffxFsr2GetInterfaceVK( &desc.callbacks, scratch, scratchSize, physical, vkGetDeviceProcAddr );
	if ( err != FFX_OK ) {
		common->Warning( "FSR2 self-test: ffxFsr2GetInterfaceVK FAILED (code %d)", (int)err );
		free( scratch );
		free( ctx );
		return;
	}

	desc.device = ffxGetDeviceVK( device );
	desc.maxRenderSize.width  = desc.displaySize.width  = (uint32_t)glConfig.vidWidth;
	desc.maxRenderSize.height = desc.displaySize.height = (uint32_t)glConfig.vidHeight;
	// Native-AA config (matches the intended R1 usage): HDR pre-tonemap input, non-reversed-z
	// infinite-far depth, auto-exposure. No dispatch is issued here.
	desc.flags = FFX_FSR2_ENABLE_HIGH_DYNAMIC_RANGE | FFX_FSR2_ENABLE_DEPTH_INFINITE | FFX_FSR2_ENABLE_AUTO_EXPOSURE;
	desc.fpMessage = NULL;

	err = ffxFsr2ContextCreate( ctx, &desc );
	if ( err != FFX_OK ) {
		common->Warning( "FSR2 self-test: ffxFsr2ContextCreate FAILED (code %d)", (int)err );
		free( scratch );
		free( ctx );
		return;
	}

	common->Printf( "FSR2 self-test: PASS - context created (Native-AA %ux%u, scratch %zu KB)\n",
		desc.displaySize.width, desc.displaySize.height, scratchSize / 1024 );

	ffxFsr2ContextDestroy( ctx );		// scratch must outlive the context; free after destroy
	free( scratch );
	free( ctx );
}

/*
====================
VulkanBackend::Fsr2EnsureContext / Fsr2DestroyContext / RunFsr2

The FSR2 Native-AA runtime (R1/C2, docs/fsr-temporal-pipeline.md). The context holds
FSR2's temporal history (internal upscaled color / lock / depth pyramids), so it
persists across frames and is only recreated when the scene-target size changes.
The output image is a same-size RGBA16F storage image; after the dispatch it is
copied back over the scene target's color attachment 0, so the rest of the frame
(eye adaptation, bloom, HUD composite, tonemap resolve) consumes the resolved scene
with the frame structure unchanged (the render-scale reorder is deferred to the
upscaling phase — Native-AA makes copy-back exact).
====================
*/
void VulkanBackend::Fsr2DestroyContext( bool deviceIdle ) {
	if ( fsr2Ctx != NULL ) {
		if ( !deviceIdle && device != VK_NULL_HANDLE ) {
			vkDeviceWaitIdle( device );		// context teardown frees pipelines the GPU may still use
		}
		ffxFsr2ContextDestroy( fsr2Ctx );
		free( fsr2Ctx );
		fsr2Ctx = NULL;
	}
	if ( fsr2Scratch != NULL ) {
		free( fsr2Scratch );
		fsr2Scratch = NULL;
	}
	if ( fsr2OutView != VK_NULL_HANDLE ) { vkDestroyImageView( device, fsr2OutView, NULL ); fsr2OutView = VK_NULL_HANDLE; }
	if ( fsr2Out != VK_NULL_HANDLE )     { vmaDestroyImage( vma, fsr2Out, fsr2OutAlloc ); fsr2Out = VK_NULL_HANDLE; fsr2OutAlloc = NULL; }
	if ( fsr2DepthView != VK_NULL_HANDLE ) { vkDestroyImageView( device, fsr2DepthView, NULL ); fsr2DepthView = VK_NULL_HANDLE; }
	if ( fsr2OpaqueView != VK_NULL_HANDLE ) { vkDestroyImageView( device, fsr2OpaqueView, NULL ); fsr2OpaqueView = VK_NULL_HANDLE; }
	if ( fsr2Opaque != VK_NULL_HANDLE )     { vmaDestroyImage( vma, fsr2Opaque, fsr2OpaqueAlloc ); fsr2Opaque = VK_NULL_HANDLE; fsr2OpaqueAlloc = NULL; }
	if ( fsr2ReactiveView != VK_NULL_HANDLE ) { vkDestroyImageView( device, fsr2ReactiveView, NULL ); fsr2ReactiveView = VK_NULL_HANDLE; }
	if ( fsr2Reactive != VK_NULL_HANDLE )     { vmaDestroyImage( vma, fsr2Reactive, fsr2ReactiveAlloc ); fsr2Reactive = VK_NULL_HANDLE; fsr2ReactiveAlloc = NULL; }
	fsr2DepthSrc = VK_NULL_HANDLE;
	fsr2W = fsr2H = 0;
	fsr2OpaqueW = fsr2OpaqueH = 0;
	fsr2OutWritten = false;
	fsr2OpaqueWritten = false;
	fsr2OpaqueValid = false;
	fsr2ReactiveWritten = false;
	fsr2FirstDispatch = true;
}

bool VulkanBackend::Fsr2EnsureContext( int w, int h ) {
	if ( fsr2Ctx != NULL && fsr2W == w && fsr2H == h ) {
		return true;
	}
	Fsr2DestroyContext( false );		// size changed (or first use): full rebuild

	const size_t scratchSize = ffxFsr2GetScratchMemorySizeVK( physical );
	if ( scratchSize == 0 ) {
		return false;
	}
	fsr2Scratch = malloc( scratchSize );
	fsr2Ctx = (FfxFsr2Context *)malloc( sizeof( FfxFsr2Context ) );
	if ( fsr2Scratch == NULL || fsr2Ctx == NULL ) {
		Fsr2DestroyContext( true );
		return false;
	}

	FfxFsr2ContextDescription desc = {};
	FfxErrorCode err = ffxFsr2GetInterfaceVK( &desc.callbacks, fsr2Scratch, scratchSize, physical, vkGetDeviceProcAddr );
	if ( err != FFX_OK ) {
		common->Warning( "FSR2: ffxFsr2GetInterfaceVK failed (code %d)", (int)err );
		Fsr2DestroyContext( true );
		return false;
	}
	desc.device = ffxGetDeviceVK( device );
	desc.maxRenderSize.width = (uint32_t)w;
	desc.maxRenderSize.height = (uint32_t)h;
	desc.displaySize.width = (uint32_t)w;		// Native-AA: render == display
	desc.displaySize.height = (uint32_t)h;
	// Plan-verified flags: un-tonemapped linear HDR input, D3's far-plane-at-infinity
	// non-reversed depth, FSR2's own auto-exposure (the engine's eye adaptation runs
	// AFTER the resolve and is unaffected). NO jitter-cancellation flag: A2 already
	// subtracts the jitter from the velocity buffer.
	desc.flags = FFX_FSR2_ENABLE_HIGH_DYNAMIC_RANGE | FFX_FSR2_ENABLE_DEPTH_INFINITE | FFX_FSR2_ENABLE_AUTO_EXPOSURE;
	err = ffxFsr2ContextCreate( fsr2Ctx, &desc );
	if ( err != FFX_OK ) {
		common->Warning( "FSR2: ffxFsr2ContextCreate failed (code %d)", (int)err );
		Fsr2DestroyContext( true );
		return false;
	}

	// display-res output UAV (the one external write FSR2 makes) + its copy-back source role
	VkImageCreateInfo ici = {};
	ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = VK_FORMAT_R16G16B16A16_SFLOAT;
	ici.extent = { (uint32_t)w, (uint32_t)h, 1 };
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VmaAllocationCreateInfo aci = {};
	aci.usage = VMA_MEMORY_USAGE_AUTO;
	if ( !vkCheck( vmaCreateImage( vma, &ici, &aci, &fsr2Out, &fsr2OutAlloc, NULL ), "vmaCreateImage(FSR2 output)" ) ) {
		Fsr2DestroyContext( true );
		return false;
	}
	VkImageViewCreateInfo vwi = {};
	vwi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	vwi.image = fsr2Out;
	vwi.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vwi.format = VK_FORMAT_R16G16B16A16_SFLOAT;
	vwi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	vwi.subresourceRange.levelCount = 1;
	vwi.subresourceRange.layerCount = 1;
	if ( !vkCheck( vkCreateImageView( device, &vwi, NULL, &fsr2OutView ), "vkCreateImageView(FSR2 output)" ) ) {
		Fsr2DestroyContext( true );
		return false;
	}

	// R8 reactive mask (R1/D): written by FSR2's autogen pass (UAV), read by the dispatch.
	// Created with the context so it always matches the render size; its absence simply
	// disables the reactive path (RunFsr2 guards on it).
	ici.format = VK_FORMAT_R8_UNORM;
	ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	if ( !vkCheck( vmaCreateImage( vma, &ici, &aci, &fsr2Reactive, &fsr2ReactiveAlloc, NULL ), "vmaCreateImage(FSR2 reactive)" ) ) {
		Fsr2DestroyContext( true );
		return false;
	}
	vwi.image = fsr2Reactive;
	vwi.format = VK_FORMAT_R8_UNORM;
	if ( !vkCheck( vkCreateImageView( device, &vwi, NULL, &fsr2ReactiveView ), "vkCreateImageView(FSR2 reactive)" ) ) {
		Fsr2DestroyContext( true );
		return false;
	}
	fsr2ReactiveWritten = false;

	fsr2W = w;
	fsr2H = h;
	fsr2OutWritten = false;
	fsr2FirstDispatch = true;
	common->Printf( "FSR2: context created (Native-AA %dx%d, scratch %zu KB)\n", w, h, scratchSize / 1024 );
	return true;
}

/*
====================
VulkanBackend::Fsr2CaptureOpaque

R1/D: snapshot the scene color at the opaque/translucent split as the auto-reactive
mask's "opaque only" input. A straight vkCmdCopyImage in storage orientation — the
M5 CopyFramebufferToImage capture is y-FLIPPED (GL bottom-up) and would row-mismatch
the reactive comparison. Suspends the scene pass exactly like BeginTargetPass.
====================
*/
void VulkanBackend::Fsr2CaptureOpaque( RenderTargetHandle sceneRT ) {
	if ( device == VK_NULL_HANDLE || !frameOpen || skipFrame ) {
		return;
	}
	RenderTarget *scene = LookupTarget( sceneRT );
	if ( scene == NULL || !scene->colorTarget || scene->colorFormat[0] != VK_FORMAT_R16G16B16A16_SFLOAT ) {
		return;
	}

	// (re)create the snapshot image on first use / size change; sized to the scene target,
	// independent of the FSR2 context (the capture happens mid-view, before RunFsr2 may
	// have created the context on the first frame)
	if ( fsr2Opaque == VK_NULL_HANDLE || fsr2OpaqueW != scene->w || fsr2OpaqueH != scene->h ) {
		if ( fsr2OpaqueView != VK_NULL_HANDLE || fsr2Opaque != VK_NULL_HANDLE ) {
			retiredImages[frameIndex].push_back( { fsr2Opaque, fsr2OpaqueAlloc, fsr2OpaqueView } );
			fsr2Opaque = VK_NULL_HANDLE; fsr2OpaqueAlloc = NULL; fsr2OpaqueView = VK_NULL_HANDLE;
		}
		VkImageCreateInfo ici = {};
		ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		ici.imageType = VK_IMAGE_TYPE_2D;
		ici.format = VK_FORMAT_R16G16B16A16_SFLOAT;
		ici.extent = { (uint32_t)scene->w, (uint32_t)scene->h, 1 };
		ici.mipLevels = 1;
		ici.arrayLayers = 1;
		ici.samples = VK_SAMPLE_COUNT_1_BIT;
		ici.tiling = VK_IMAGE_TILING_OPTIMAL;
		ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		VmaAllocationCreateInfo aci = {};
		aci.usage = VMA_MEMORY_USAGE_AUTO;
		if ( !vkCheck( vmaCreateImage( vma, &ici, &aci, &fsr2Opaque, &fsr2OpaqueAlloc, NULL ), "vmaCreateImage(FSR2 opaque)" ) ) {
			fsr2Opaque = VK_NULL_HANDLE;
			return;
		}
		VkImageViewCreateInfo vwi = {};
		vwi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		vwi.image = fsr2Opaque;
		vwi.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vwi.format = VK_FORMAT_R16G16B16A16_SFLOAT;
		vwi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		vwi.subresourceRange.levelCount = 1;
		vwi.subresourceRange.layerCount = 1;
		if ( !vkCheck( vkCreateImageView( device, &vwi, NULL, &fsr2OpaqueView ), "vkCreateImageView(FSR2 opaque)" ) ) {
			vmaDestroyImage( vma, fsr2Opaque, fsr2OpaqueAlloc );
			fsr2Opaque = VK_NULL_HANDLE; fsr2OpaqueAlloc = NULL;
			return;
		}
		fsr2OpaqueW = scene->w;
		fsr2OpaqueH = scene->h;
		fsr2OpaqueWritten = false;
	}

	VkCommandBuffer cb = frames[frameIndex].cb;
	if ( insideScenePass ) {
		vkCmdEndRenderPass( cb );		// scene pass resumes on the next Draw (EnsureScenePass)
		insideScenePass = false;
	}
	if ( insideTargetPass ) {
		return;		// mid-target-pass capture would corrupt the pass; caller error
	}

	// scene color SHADER_READ_ONLY -> TRANSFER_SRC, snapshot -> TRANSFER_DST
	VkImageMemoryBarrier pre[2] = {};
	pre[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	pre[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	pre[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	pre[0].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	pre[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	pre[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	pre[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	pre[0].image = scene->colorImage[0];
	pre[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	pre[0].subresourceRange.levelCount = 1;
	pre[0].subresourceRange.layerCount = 1;
	pre[1] = pre[0];
	pre[1].srcAccessMask = fsr2OpaqueWritten ? VK_ACCESS_SHADER_READ_BIT : 0;
	pre[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	pre[1].oldLayout = fsr2OpaqueWritten ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
	pre[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	pre[1].image = fsr2Opaque;
	vkCmdPipelineBarrier( cb,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
			| VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, pre );

	VkImageCopy region = {};
	region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.srcSubresource.layerCount = 1;
	region.dstSubresource = region.srcSubresource;
	region.extent = { (uint32_t)scene->w, (uint32_t)scene->h, 1 };
	vkCmdCopyImage( cb, scene->colorImage[0], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		fsr2Opaque, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region );

	// both back to SHADER_READ_ONLY (the between-pass resting layout / the SRV state the
	// reactive autogen pass declares)
	VkImageMemoryBarrier post[2] = {};
	post[0] = pre[0];
	post[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	post[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	post[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	post[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	post[1] = pre[1];
	post[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	post[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	post[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	post[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	vkCmdPipelineBarrier( cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
			| VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		0, 0, NULL, 0, NULL, 2, post );

	fsr2OpaqueWritten = true;
	fsr2OpaqueValid = true;
}

bool VulkanBackend::RunFsr2( const Fsr2DispatchArgs &args ) {
	if ( device == VK_NULL_HANDLE || !frameOpen || skipFrame ) {
		return false;
	}
	if ( !haveSeparateDepthStencilLayouts ) {
		// the vendored FSR2 VK backend barriers the sampled depth with a depth-only
		// aspect, which a combined depth-stencil image only allows with this feature
		if ( !fsr2WarnedNoFeature ) {
			common->Warning( "FSR2: device lacks separateDepthStencilLayouts - r_fsr unavailable" );
			fsr2WarnedNoFeature = true;
		}
		return false;
	}
	RenderTarget *scene = LookupTarget( args.sceneRT );
	RenderTarget *vel = LookupTarget( args.velocityRT );
	if ( scene == NULL || !scene->colorTarget || !scene->hasDepth
	     || scene->colorFormat[0] != VK_FORMAT_R16G16B16A16_SFLOAT
	     || vel == NULL || vel->colorCount < 3 || vel->colorFormat[2] != VK_FORMAT_R16G16_SFLOAT
	     || vel->w != scene->w || vel->h != scene->h ) {
		return false;
	}
	if ( !Fsr2EnsureContext( scene->w, scene->h ) ) {
		return false;
	}

	// depth-only-aspect sampled view of the scene target's combined depth-stencil image,
	// rebuilt when the target reallocs (dsImage changes); the old view is retired on the
	// frame-slot fence like every mid-frame image teardown
	if ( fsr2DepthView == VK_NULL_HANDLE || fsr2DepthSrc != scene->dsImage ) {
		if ( fsr2DepthView != VK_NULL_HANDLE ) {
			retiredImages[frameIndex].push_back( { VK_NULL_HANDLE, NULL, fsr2DepthView } );
			fsr2DepthView = VK_NULL_HANDLE;
		}
		VkImageViewCreateInfo dvi = {};
		dvi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		dvi.image = scene->dsImage;
		dvi.viewType = VK_IMAGE_VIEW_TYPE_2D;
		dvi.format = sceneDepthFormat;
		dvi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
		dvi.subresourceRange.levelCount = 1;
		dvi.subresourceRange.layerCount = 1;
		if ( !vkCheck( vkCreateImageView( device, &dvi, NULL, &fsr2DepthView ), "vkCreateImageView(FSR2 depth)" ) ) {
			return false;
		}
		fsr2DepthSrc = scene->dsImage;
	}

	VkCommandBuffer cb = frames[frameIndex].cb;
	// FSR2 records compute; close any open render pass (the scene pass resumes on the
	// next Draw through EnsureScenePass, same contract as BeginTargetPass)
	if ( insideScenePass ) {
		vkCmdEndRenderPass( cb );
		insideScenePass = false;
	}
	if ( insideTargetPass ) {
		return false;		// caller error: mid-target-pass dispatch would corrupt the pass
	}

	const uint32_t w = (uint32_t)scene->w, h = (uint32_t)scene->h;

	// Pre-dispatch layouts. Color + velocity attachments already rest in
	// SHADER_READ_ONLY between passes (their render passes' finalLayout), which is
	// exactly FFX_RESOURCE_STATE_COMPUTE_READ's layout — declared as such below, so
	// FSR2's own barriers on them are same-layout no-ops. Depth sits in
	// DEPTH_STENCIL_ATTACHMENT_OPTIMAL and must round-trip; the output enters as a
	// discardable UAV (UNDEFINED on first use / TRANSFER_SRC after a copy-back).
	VkImageMemoryBarrier pre[2] = {};
	pre[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	pre[0].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	pre[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	pre[0].oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	pre[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	pre[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	pre[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	pre[0].image = scene->dsImage;
	pre[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
	pre[0].subresourceRange.levelCount = 1;
	pre[0].subresourceRange.layerCount = 1;
	pre[1] = pre[0];
	pre[1].srcAccessMask = fsr2OutWritten ? VK_ACCESS_TRANSFER_READ_BIT : 0;
	pre[1].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	pre[1].oldLayout = fsr2OutWritten ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
	pre[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
	pre[1].image = fsr2Out;
	pre[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	vkCmdPipelineBarrier( cb,
		VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 2, pre );

	FfxFsr2DispatchDescription dd = {};
	dd.commandList = ffxGetCommandListVK( cb );
	dd.color = ffxGetTextureResourceVK( fsr2Ctx, scene->colorImage[0], scene->colorSampleView[0] != VK_NULL_HANDLE ? scene->colorSampleView[0] : scene->colorView[0],
		w, h, scene->colorFormat[0], L"FSR2_Color", FFX_RESOURCE_STATE_COMPUTE_READ );
	dd.depth = ffxGetTextureResourceVK( fsr2Ctx, scene->dsImage, fsr2DepthView,
		w, h, sceneDepthFormat, L"FSR2_Depth", FFX_RESOURCE_STATE_COMPUTE_READ );
	dd.motionVectors = ffxGetTextureResourceVK( fsr2Ctx, vel->colorImage[2], vel->colorSampleView[2] != VK_NULL_HANDLE ? vel->colorSampleView[2] : vel->colorView[2],
		w, h, vel->colorFormat[2], L"FSR2_MotionVectors", FFX_RESOURCE_STATE_COMPUTE_READ );
	dd.output = ffxGetTextureResourceVK( fsr2Ctx, fsr2Out, fsr2OutView,
		w, h, VK_FORMAT_R16G16B16A16_SFLOAT, L"FSR2_Output", FFX_RESOURCE_STATE_UNORDERED_ACCESS );
	// exposure/transparencyAndComposition stay null (AUTO_EXPOSURE; hand-authored T&C mask
	// is the potential V2 of increment D)

	// R1/D auto-reactive: generate the reactive mask from opaque-vs-final before the main
	// dispatch. Where translucent/additive content diverges from the opaque snapshot
	// (particles, muzzle flashes, GUI screens), FSR2 trusts history less — killing ghost
	// trails at the cost of accumulation there. Skipped unless this frame captured the
	// opaque snapshot (Fsr2CaptureOpaque) and the caller enabled it (reactiveScale >= 0).
	if ( args.reactiveScale >= 0.0f && fsr2OpaqueValid
	     && fsr2Opaque != VK_NULL_HANDLE && fsr2Reactive != VK_NULL_HANDLE
	     && fsr2OpaqueW == (int)w && fsr2OpaqueH == (int)h ) {
		VkImageMemoryBarrier rb = {};
		rb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		rb.srcAccessMask = fsr2ReactiveWritten ? VK_ACCESS_SHADER_READ_BIT : 0;
		rb.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		rb.oldLayout = fsr2ReactiveWritten ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
		rb.newLayout = VK_IMAGE_LAYOUT_GENERAL;
		rb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		rb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		rb.image = fsr2Reactive;
		rb.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		rb.subresourceRange.levelCount = 1;
		rb.subresourceRange.layerCount = 1;
		vkCmdPipelineBarrier( cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &rb );

		FfxFsr2GenerateReactiveDescription gr = {};
		gr.commandList = dd.commandList;
		gr.colorOpaqueOnly = ffxGetTextureResourceVK( fsr2Ctx, fsr2Opaque, fsr2OpaqueView,
			w, h, VK_FORMAT_R16G16B16A16_SFLOAT, L"FSR2_OpaqueOnly", FFX_RESOURCE_STATE_COMPUTE_READ );
		gr.colorPreUpscale = dd.color;
		gr.outReactive = ffxGetTextureResourceVK( fsr2Ctx, fsr2Reactive, fsr2ReactiveView,
			w, h, VK_FORMAT_R8_UNORM, L"FSR2_Reactive", FFX_RESOURCE_STATE_UNORDERED_ACCESS );
		gr.renderSize.width = w;
		gr.renderSize.height = h;
		gr.scale = args.reactiveScale;
		// AMD reference defaults: binary mask (threshold 0.2 -> 0.9) over the per-channel
		// max delta, compared in tonemapped space so HDR fireballs don't saturate the test
		gr.cutoffThreshold = 0.2f;
		gr.binaryValue = 0.9f;
		gr.flags = FFX_FSR2_AUTOREACTIVEFLAGS_APPLY_TONEMAP
		         | FFX_FSR2_AUTOREACTIVEFLAGS_APPLY_THRESHOLD
		         | FFX_FSR2_AUTOREACTIVEFLAGS_USE_COMPONENTS_MAX;
		const FfxErrorCode grErr = ffxFsr2ContextGenerateReactiveMask( fsr2Ctx, &gr );
		if ( grErr == FFX_OK ) {
			// dispatch reads it as an SRV; declared UAV so FSR2's own barrier does the transition
			dd.reactive = ffxGetTextureResourceVK( fsr2Ctx, fsr2Reactive, fsr2ReactiveView,
				w, h, VK_FORMAT_R8_UNORM, L"FSR2_Reactive", FFX_RESOURCE_STATE_UNORDERED_ACCESS );
			fsr2ReactiveWritten = true;		// FSR2 leaves it SHADER_READ_ONLY after the SRV barrier
		}
	}

	// Motion-vector scale, derived in the plan (docs/fsr-temporal-pipeline.md C2) and
	// verified against the vendored shaders: A2 stores (currUV - prevUV) in +Y-up UV;
	// FSR2 wants (prevUV - currUV) in top-left +Y-down UV, in PIXELS. Reversing the
	// direction negates both axes; the Y-up -> Y-down flip negates Y again, so X gets
	// -renderW and Y's two negations cancel to +renderH.
	dd.motionVectorScale.x = -(float)w;
	dd.motionVectorScale.y = (float)h;
	// Jitter: FSR2's fJitter is the CONTENT's displacement in storage pixels (top-left,
	// +y down): the accumulate pass samples the jittered input at fHrUv + fJitter/Size
	// (ffx_fsr2_accumulate.h). The engine shifts the frustum WINDOW by +jitter
	// (tr_main.cpp xmin/xmax += j), and with GL-style z_eye NEGATIVE in front the
	// (r+l)/w term flips sign after the perspective divide — so the content moves
	// OPPOSITE the applied jitter: -jx columns, and -jy in NDC+y-up = +jy rows under
	// the negative-viewport flip (row 0 = top). Hence {-jx, +jy}. (Empirically gated:
	// the inverted sign showed as micro-boiling on a static scene, 2026-08-18.)
	dd.jitterOffset.x = -args.jitterX;
	dd.jitterOffset.y = args.jitterY;
	dd.renderSize.width = w;
	dd.renderSize.height = h;
	dd.enableSharpening = args.sharpness >= 0.0f;
	dd.sharpness = args.sharpness >= 0.0f ? args.sharpness : 0.0f;
	dd.frameTimeDelta = args.frameTimeMs;
	dd.preExposure = 1.0f;
	dd.reset = args.reset || fsr2FirstDispatch;
	dd.cameraNear = args.zNear;
	dd.cameraFar = 100000.0f;					// unused with FFX_FSR2_ENABLE_DEPTH_INFINITE
	dd.cameraFovAngleVertical = args.fovYRadians;
	dd.viewSpaceToMetersFactor = 0.0254f;		// 1 Doom unit ~= 1 inch

	const FfxErrorCode err = ffxFsr2ContextDispatch( fsr2Ctx, &dd );
	fsr2FirstDispatch = false;
	if ( err != FFX_OK ) {
		// restore depth for the rest of the frame even on failure
		VkImageMemoryBarrier depthBack = pre[0];
		depthBack.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
		depthBack.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		depthBack.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		depthBack.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		vkCmdPipelineBarrier( cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
			0, 0, NULL, 0, NULL, 1, &depthBack );
		common->Warning( "FSR2: dispatch failed (code %d)", (int)err );
		return false;
	}

	// Copy the resolved image back over the scene color (Native-AA: same size/format),
	// and return depth to attachment layout. FSR2 leaves the output in GENERAL
	// (UNORDERED_ACCESS was its last use) and the sampled inputs in SHADER_READ_ONLY.
	VkImageMemoryBarrier toCopy[2] = {};
	toCopy[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	toCopy[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	toCopy[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	toCopy[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
	toCopy[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	toCopy[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toCopy[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toCopy[0].image = fsr2Out;
	toCopy[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	toCopy[0].subresourceRange.levelCount = 1;
	toCopy[0].subresourceRange.layerCount = 1;
	toCopy[1] = toCopy[0];
	toCopy[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
	toCopy[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	toCopy[1].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	toCopy[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	toCopy[1].image = scene->colorImage[0];
	vkCmdPipelineBarrier( cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		0, 0, NULL, 0, NULL, 2, toCopy );

	VkImageCopy region = {};
	region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.srcSubresource.layerCount = 1;
	region.dstSubresource = region.srcSubresource;
	region.extent = { w, h, 1 };
	vkCmdCopyImage( cb, fsr2Out, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		scene->colorImage[0], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region );
	fsr2OutWritten = true;		// next frame's pre-barrier: TRANSFER_SRC -> GENERAL

	VkImageMemoryBarrier post[2] = {};
	post[0] = toCopy[1];
	post[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	post[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	post[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	post[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;	// the between-pass resting layout
	post[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	post[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
	post[1].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	post[1].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	post[1].newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	post[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	post[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	post[1].image = scene->dsImage;
	post[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
	post[1].subresourceRange.levelCount = 1;
	post[1].subresourceRange.layerCount = 1;
	vkCmdPipelineBarrier( cb,
		VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
			| VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
		0, 0, NULL, 0, NULL, 2, post );

	return true;
}

/*
====================
VulkanBackend::Mrt3SelfTest

r_mrt3Test: MRT/format plumbing validation for the R1 motion-vector work (A0,
docs/fsr-temporal-pipeline.md). Creates the 3-attachment velocity gbuffer layout
(RGBA8 normal @0 + RGBA8 SSR @1 + RG16F velocity @2 + depth) through the normal target
lifecycle, then destroys it. Proves RG16F is an accepted color-attachment format on this
driver and that the distinct 3-MRT pass class (8) builds a valid render pass + framebuffer
without aliasing class 6 (the shipping 2-attachment SSR normal prepass). No velocity is
emitted and the frontend is untouched; with r_mrt3Test 0 no 3-MRT target is ever created.
====================
*/
void VulkanBackend::Mrt3SelfTest() {
	if ( device == VK_NULL_HANDLE ) {
		common->Printf( "VK MRT3 self-test: unavailable (no VK device)\n" );
		return;
	}

	RenderTargetHandle rt = CreateRenderTargetColorDepth( IF_RGBA8, 256, 256, 3 );
	if ( rt == 0 ) {
		common->Warning( "VK MRT3 self-test: FAIL - 3-MRT RG16F target creation failed (driver may reject RG16F as a color attachment)" );
		return;
	}

	RenderTarget *t = LookupTarget( rt );
	common->Printf( "VK MRT3 self-test: PASS - 3-MRT target (RGBA8 + RGBA8 + RG16F + depth, passClass %u) created\n",
		t ? (unsigned)t->passClass : 0 );

	DestroyRenderTarget( rt );			// full teardown (VK objects + imageTable slots)
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
		// NEAREST mip mode: an explicit textureLod picks the nearest discrete level
		// (matches GL's GL_LINEAR_MIPMAP_NEAREST for the SSAO depth mip). maxLod stays
		// 0 for non-mipped RTs; the mipped depth chain needs the whole range unclamped.
		si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
		si.maxLod = hasMips ? VK_LOD_CLAMP_NONE : 0.0f;
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

	// build the RGBA8 mip chain on the calling thread (level 0 borrows the caller's
	// pixels), then hand it to the shared uploader
	std::vector<PrebuiltMip> levels;
	std::vector<byte *> owned;		// R_MipMap results to free
	levels.push_back( { pixels, w, h } );
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

	ImageHandle handle = UploadTexture2DLevels( w, h, levels.data(), (int)levels.size(),
		textureFilter, textureRepeat );

	for ( size_t i = 0; i < owned.size(); i++ ) { R_StaticFree( owned[i] ); }
	return handle;
}

/*
====================
VulkanBackend::CreateTexture2DPrebuilt

DUDE parallel image load: the RGBA8 mip chain was built on a worker thread; just
upload it here on the main thread (GPU submit is not thread-safe — one shared
uploadCb/uploadFence). The worker owns and frees the level buffers.
====================
*/
ImageHandle VulkanBackend::CreateTexture2DPrebuilt( int w, int h, const PrebuiltMip *levels,
                                                    int numLevels, int textureFilter,
                                                    int textureRepeat ) {
	return UploadTexture2DLevels( w, h, levels, numLevels, textureFilter, textureRepeat );
}

/*
====================
VulkanBackend::UploadTexture2DLevels

Shared GPU upload for CreateTexture2D / CreateTexture2DPrebuilt: stage the supplied
RGBA8 mip levels and copy them into a new sampled image with a blocking submit. Does
not own the level buffers (the caller frees them once this returns).
====================
*/
ImageHandle VulkanBackend::UploadTexture2DLevels( int w, int h, const PrebuiltMip *inLevels,
                                                  int numLevels, int textureFilter,
                                                  int textureRepeat ) {
	if ( device == VK_NULL_HANDLE || w <= 0 || h <= 0 || numLevels < 1 || inLevels == NULL
			|| inLevels[0].data == NULL || uploadCb == VK_NULL_HANDLE ) {
		return 0;
	}

	// mirror into the local descriptor type the rest of this function already uses;
	// owned stays empty because the caller owns the level pixel buffers
	struct level_t { const byte *data; int w, h; };
	std::vector<level_t> levels;
	levels.reserve( numLevels );
	for ( int i = 0; i < numLevels; i++ ) {
		levels.push_back( { (const byte *)inLevels[i].data, inLevels[i].w, inLevels[i].h } );
	}
	std::vector<byte *> owned;

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
	ImageHandle handle = 0;
	for ( size_t i = 0; i < imageTable.size(); i++ ) {
		if ( !imageTable[i].live ) {
			imageTable[i] = rec;
			handle = (ImageHandle)( i + 1 );
			break;
		}
	}
	if ( handle == 0 ) {
		imageTable.push_back( rec );
		handle = (ImageHandle)imageTable.size();
	}
	SyncBindlessSlot( handle );		// RR4: register this texture in the bindless array (slot = handle-1)
	return handle;
}

// RR4 bindless materials (docs/rtx-reflections.md): point bindless slot (h-1) at handle h's view+sampler,
// or the 1x1 dummy when the handle is dead / out of range. UPDATE_AFTER_BIND makes the write legal even
// while the set is bound in an in-flight frame — ssr_rt only samples slots for currently-resident diffuse
// textures, never one being retired. No-op without descriptor indexing or before the bindless set exists.
void VulkanBackend::SyncBindlessSlot( ImageHandle h ) {
	if ( bindlessSet == VK_NULL_HANDLE || h < 1 ) {
		return;
	}
	const uint32_t slot = (uint32_t)( h - 1 );
	if ( slot >= bindlessCapacity ) {
		return;		// beyond the array; the shader's texIndex fetch guards on this and falls back to baseColor
	}
	VkDescriptorImageInfo ii = {};
	const ImageRec &rec = imageTable[h - 1];
	if ( rec.live && rec.view != VK_NULL_HANDLE && rec.sampler != VK_NULL_HANDLE ) {
		ii.sampler = rec.sampler;
		ii.imageView = rec.view;
	} else if ( dummyImage != 0 && dummyImage <= (ImageHandle)imageTable.size() ) {
		const ImageRec &d = imageTable[dummyImage - 1];
		ii.sampler = d.sampler;
		ii.imageView = d.view;
	} else {
		return;		// nothing valid to bind yet (very early init); slot stays partially-bound
	}
	ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	VkWriteDescriptorSet w = {};
	w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	w.dstSet = bindlessSet;
	w.dstBinding = 0;
	w.dstArrayElement = slot;
	w.descriptorCount = 1;
	w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	w.pImageInfo = &ii;
	vkUpdateDescriptorSets( device, 1, &w, 0, NULL );
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
	// idImage::GenerateImage destroys then recreates dynamically-updated 2D images
	// (scratch GUIs, video, menu logos) and can do so either between frames (front
	// end reload) or mid-frame (a recording command buffer has already bound a
	// descriptor set sampling this view). The two cases need different lifetimes:
	//
	//   * no frame open  -> vkDeviceWaitIdle waits every in-flight submission, so
	//     the objects are safe to free immediately.
	//   * frame open      -> vkDeviceWaitIdle does NOT wait for the still-recording
	//     command buffer, so freeing the view here would pull it out from under a
	//     live set and the GPU would sample a dead view on submit (VK_ERROR_DEVICE_
	//     LOST). Defer to this slot's retire list (freed after its fence, once no
	//     in-flight cb can reference it) exactly like RetireImage.
	//
	// (Deferring unconditionally is wrong between frames: EndFrame has already
	// advanced frameIndex, so the retire list would drain on the wrong slot's
	// fence, before the just-submitted frame that used the view has completed.)
	ImageRec &rec = imageTable[h - 1];
	if ( frameOpen ) {
		retiredImages[frameIndex].push_back( { rec.image, rec.alloc, rec.view } );
	} else {
		vkDeviceWaitIdle( device );
		if ( rec.view )  { vkDestroyImageView( device, rec.view, NULL ); }
		if ( rec.image ) { vmaDestroyImage( vma, rec.image, rec.alloc ); }
	}
	rec = ImageRec();
	SyncBindlessSlot( h );		// RR4: the freed slot shows the dummy until reused
	// drop the cross-frame texture-set cache: any cached set keyed on handle h is
	// now stale, and reusing the slot would otherwise sample the wrong texture.
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
	SyncBindlessSlot( h );		// RR4: the freed slot shows the dummy until reused
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
filtering GL sets after every capture), RGBA16F when hdrFloat (an HDR frame's
_currentRender, so refraction samples the un-clamped float scene), or the
scene's depth-stencil format (nearest, sampled through a depth-aspect view).
Contents are undefined until the first CopyFramebufferToImage.
====================
*/
ImageHandle VulkanBackend::CreateCaptureImage( int w, int h, bool depth, bool hdrFloat ) {
	if ( device == VK_NULL_HANDLE || w <= 0 || h <= 0 ) {
		return 0;
	}
	if ( depth && sceneDepthFormat == VK_FORMAT_UNDEFINED ) {
		return 0;
	}

	VkImageCreateInfo ici = {};
	ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = depth ? sceneDepthFormat
	           : ( hdrFloat ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM );
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

int VulkanBackend::ClampTargetSize( int size, const char *kind, bool cube ) const {
	if ( size <= 0 ) {
		return 1;
	}
	// Clamp to the actual device limit only. Exceeding maxImageDimension is guaranteed-invalid
	// usage (a validation-error / device-lost path), so that is the real safety bound. Do NOT
	// impose an arbitrary lower ceiling: color targets are screen-sized and a hard 4096 cap
	// would squish the scene on high-res / ultrawide displays (this class of GPU reports far
	// more than 4096, e.g. 16384-32768).
	int limit = cube ? (int)physProps.limits.maxImageDimensionCube
	                 : (int)physProps.limits.maxImageDimension2D;
	if ( limit < 1 ) {
		limit = 16384; // defensive fallback if the driver reported nothing
	}
	if ( size <= limit ) {
		return size;
	}
	common->Warning( "VK: %s size %d exceeds the device limit %d; clamping to %d to avoid an invalid-usage / device-lost path",
		kind, size, limit, limit );
	return limit;
}

// Allocate a depth image (2D or cube), its sample view + per-face render views,
// framebuffer(s), and register the sampleable ImageRec. Returns false (and
// leaves t clean) on failure.
bool VulkanBackend::CreateDepthTarget( RenderTarget &t, int w, int h, bool cube ) {
	if ( device == VK_NULL_HANDLE || !EnsureShadowPass() ) {
		return false;
	}
	const int safeW = ClampTargetSize( w, cube ? "cube shadow" : "shadow map", cube );
	const int safeH = ClampTargetSize( h, cube ? "cube shadow" : "shadow map", cube );
	const int layers = cube ? 6 : 1;
	t.cube = cube;
	t.w = safeW;
	t.h = safeH;
	t.depthFormat = VK_FORMAT_D32_SFLOAT;

	VkImageCreateInfo ici = {};
	ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ici.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = t.depthFormat;
	ici.extent = { (uint32_t)safeW, (uint32_t)safeH, 1 };
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
		fbi.width = (uint32_t)safeW;
		fbi.height = (uint32_t)safeH;
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
	            : ( fmt == IF_R16F ) ? VK_FORMAT_R16_SFLOAT
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

// SSAO Phase 1: a color target carrying a render-generated mip chain. Level 0 is a
// normal color attachment (a fullscreen linearize pass renders into it); levels 1..N
// are filled by a per-level max-downsample (BeginTargetMipPass + a downsample shader).
// The whole chain is sampled with an explicit textureLod (see CreateColorTarget).
RenderTargetHandle VulkanBackend::CreateRenderTargetMipped( ImageFormat fmt, int w, int h, int mipLevels ) {
	if ( device == VK_NULL_HANDLE || w <= 0 || h <= 0 ) {
		return 0;
	}
	VkFormat cf = ( fmt == IF_RGBA16F ) ? VK_FORMAT_R16G16B16A16_SFLOAT
	            : ( fmt == IF_R16F ) ? VK_FORMAT_R16_SFLOAT
	            : ( fmt == IF_RGBA8 ) ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_UNDEFINED;
	if ( cf == VK_FORMAT_UNDEFINED ) {
		return 0;
	}
	// clamp the request to the image's real mip count (floor(log2(max(w,h))) + 1)
	int maxLevels = 1;
	for ( int d = ( w > h ? w : h ); d > 1; d >>= 1 ) { maxLevels++; }
	if ( mipLevels < 1 ) { mipLevels = 1; }
	if ( mipLevels > maxLevels ) { mipLevels = maxLevels; }
	// the per-level view/framebuffer/input arrays are [MAX_MIP]; never index past them
	if ( mipLevels > RenderTarget::MAX_MIP ) { mipLevels = RenderTarget::MAX_MIP; }

	int slot = AllocTargetSlot();
	if ( !CreateColorTarget( targetTable[slot], w, h, cf, 1, /*ds*/false, /*frameCapable*/false, mipLevels ) ) {
		targetTable[slot] = RenderTarget();
		return 0;
	}
	return (RenderTargetHandle)( slot + 1 );
}

// Begin a fullscreen pass rendering into mip LEVEL of a mipped color target (level >= 1;
// level 0 is the linearize pass via BeginTargetPass). Uses the level's own framebuffer.
// The color-target render pass's external dependencies serialize this write against the
// previous frame's ssao.frag reads of this level (WAR) and make the just-written source
// level visible to the next level's sample — so no manual barriers are needed.
void VulkanBackend::BeginTargetMipPass( RenderTargetHandle rt, int level, const ClearArgs *clear ) {
	if ( !frameOpen || skipFrame ) {
		return;
	}
	RenderTarget *t = LookupTarget( rt );
	if ( t == NULL || !t->colorTarget || level < 1 || level >= t->colorMipLevels
	     || t->colorLevelFb[level] == VK_NULL_HANDLE ) {
		if ( frameOpen ) { fakePassDepth++; }		// balance the caller's EndPass
		return;
	}
	VkCommandBuffer cb = frames[frameIndex].cb;
	if ( insideScenePass ) {
		vkCmdEndRenderPass( cb );
		insideScenePass = false;
	}
	const int lw = ( ( t->w >> level ) > 1 ) ? ( t->w >> level ) : 1;
	const int lh = ( ( t->h >> level ) > 1 ) ? ( t->h >> level ) : 1;
	VkClearValue cv = {};
	if ( clear != NULL && clear->color ) {
		cv.color.float32[0] = clear->rgba[0]; cv.color.float32[1] = clear->rgba[1];
		cv.color.float32[2] = clear->rgba[2]; cv.color.float32[3] = clear->rgba[3];
	}
	VkRenderPassBeginInfo rbi = {};
	rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	rbi.renderPass = t->colorClearPass;
	rbi.framebuffer = t->colorLevelFb[level];
	rbi.renderArea.extent = { (uint32_t)lw, (uint32_t)lh };
	rbi.clearValueCount = 1;
	rbi.pClearValues = &cv;
	vkCmdBeginRenderPass( cb, &rbi, VK_SUBPASS_CONTENTS_INLINE );
	EnterTargetPass( lw, lh, /*flipY*/true, t->colorClearPass, t->passClass, 1 );
	t->everWritten = true;
}

// A single-level sampleable handle for one mip level, to feed as the downsample source.
ImageHandle VulkanBackend::GetRenderTargetMipImage( RenderTargetHandle rt, int mipLevel ) {
	RenderTarget *t = LookupTarget( rt );
	if ( t == NULL || !t->colorTarget || mipLevel < 0 || mipLevel >= t->colorMipLevels ) {
		return 0;
	}
	return t->colorLevelInput[mipLevel];
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
	if ( rt != 0 && rt == mergeNormalTarget ) {
		return mergeNormalSampleImage;		// merged SSAO normal (no targetTable entry)
	}
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
		VkClearValue cv[4] = {};		// up to 3 color + 1 depth (R1/A0)
		if ( clear != NULL && clear->color ) {
			for ( int c = 0; c < t->colorCount; c++ ) {
				// attachment 2 is the RG16F velocity MRT (R1/A2): sky/uncovered pixels are
				// never drawn into the normal prepass, so they keep the clear value — which
				// for velocity must be ZERO motion, not the flat-normal color the other
				// attachments clear to. Leave cv[2] zero-initialised.
				if ( c >= 2 ) { continue; }
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
	// FSR2 (R1/C2) caches a depth-only view keyed on the scene target's dsImage. Drop the
	// cache when that image dies: a recreated target could get the SAME handle value back
	// from the driver (ABA), which would false-negative the rebuild check in RunFsr2 and
	// leave FSR2 sampling a view of a destroyed image.
	if ( t.dsImage != VK_NULL_HANDLE && t.dsImage == fsr2DepthSrc ) {
		if ( fsr2DepthView != VK_NULL_HANDLE ) {
			retiredImages[frameIndex].push_back( { VK_NULL_HANDLE, NULL, fsr2DepthView } );
			fsr2DepthView = VK_NULL_HANDLE;
		}
		fsr2DepthSrc = VK_NULL_HANDLE;
	}
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
	// SSAO Phase 1 mip chain per-level views + framebuffers (L >= 1; L 0 = colorView[0])
	for ( int L = 1; L < RenderTarget::MAX_MIP; L++ ) {
		if ( t.colorLevelFb[L] )   { vkDestroyFramebuffer( device, t.colorLevelFb[L], NULL ); t.colorLevelFb[L] = VK_NULL_HANDLE; }
		if ( t.colorLevelView[L] ) { vkDestroyImageView( device, t.colorLevelView[L], NULL ); t.colorLevelView[L] = VK_NULL_HANDLE; }
	}
	for ( int c = 0; c < 3; c++ ) {		// R1/A0: incl. the 3rd (RG16F velocity) attachment on a 3-MRT; unused slots are null-guarded
		if ( t.colorSampleView[c] ) { vkDestroyImageView( device, t.colorSampleView[c], NULL ); t.colorSampleView[c] = VK_NULL_HANDLE; }
		if ( t.colorView[c] )  { vkDestroyImageView( device, t.colorView[c], NULL ); t.colorView[c] = VK_NULL_HANDLE; }
		if ( t.colorImage[c] ) { vmaDestroyImage( vma, t.colorImage[c], t.colorAlloc[c] ); t.colorImage[c] = VK_NULL_HANDLE; t.colorAlloc[c] = NULL; }
	}
	if ( t.dsView )          { vkDestroyImageView( device, t.dsView, NULL ); t.dsView = VK_NULL_HANDLE; }
	if ( t.dsImage )         { vmaDestroyImage( vma, t.dsImage, t.dsAlloc ); t.dsImage = VK_NULL_HANDLE; t.dsAlloc = NULL; }
}

// free every imageTable slot a target lent out (depth sample + color samples)
void VulkanBackend::ReleaseTargetSampleSlots( RenderTarget &t ) {
	ImageHandle handles[4] = { t.sampleImage, t.colorSampleImage[0], t.colorSampleImage[1], t.colorSampleImage[2] };
	for ( int i = 0; i < 4; i++ ) {
		ImageHandle h = handles[i];
		if ( h >= 1 && h <= (ImageHandle)imageTable.size() ) {
			imageTable[h - 1] = ImageRec();
			imageTable[h - 1].live = false;
		}
	}
	// SSAO Phase 1: per-level source handles
	for ( int L = 0; L < RenderTarget::MAX_MIP; L++ ) {
		ImageHandle h = t.colorLevelInput[L];
		if ( h >= 1 && h <= (ImageHandle)imageTable.size() ) {
			imageTable[h - 1] = ImageRec();
			imageTable[h - 1].live = false;
		}
		t.colorLevelInput[L] = 0;
	}
	t.sampleImage = 0;
	t.colorSampleImage[0] = t.colorSampleImage[1] = t.colorSampleImage[2] = 0;
}

void VulkanBackend::DestroyRenderTarget( RenderTargetHandle rt ) {
	RenderTarget *t = LookupTarget( rt );
	if ( t == NULL ) {
		return;
	}
	// stop new draws from sampling it immediately (the ImageRec slots are freed
	// for reuse). The shadow caches evict mid-frame, so when a frame is recording
	// defer the Vulkan-object destruction to this slot's retire list (freed after
	// its fence, once no in-flight cb can reference the sample views). Between
	// frames frameIndex has already advanced, so the retire list would drain on
	// the wrong fence — waitIdle + free immediately instead (same reasoning as
	// DestroyImage).
	ReleaseTargetSampleSlots( *t );
	if ( frameOpen ) {
		retiredTargets[frameIndex].push_back( *t );
	} else {
		vkDeviceWaitIdle( device );
		FreeTargetObjects( *t );
	}
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
	if ( colorFmt == VK_FORMAT_R16_SFLOAT ) {
		return 7;							// single-channel half-float (SSAO linear-depth mip, Phase 2)
	}
	// RGBA8 family (SSAO buffers later)
	if ( !hasDepth )      { return 4; }		// color-only RGBA8
	if ( colorCount >= 3 ) { return 8; }	// 3-MRT velocity gbuffer (RGBA8+RGBA8+RG16F+depth): distinct layout, MUST NOT alias class 6
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

		VkAttachmentDescription atts[4] = {};		// up to 3 color + 1 depth (R1/A0)
		VkAttachmentReference   colorRefs[3] = {};
		for ( int c = 0; c < nColor; c++ ) {
			atts[c].format = t.colorFormat[c];
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
                                       int colorCount, bool wantDepthStencil, bool frameCapable,
                                       int mipLevels, const VkFormat *mrtFormats ) {
	if ( device == VK_NULL_HANDLE || colorCount < 1 || colorCount > 3 ) {
		return false;
	}
	if ( wantDepthStencil && sceneDepthFormat == VK_FORMAT_UNDEFINED ) {
		return false;
	}
	const int safeW = ClampTargetSize( w, "color target" );
	const int safeH = ClampTargetSize( h, "color target" );
	if ( mipLevels < 1 ) { mipLevels = 1; }
	const bool mipped = mipLevels > 1;
	t.colorTarget = true;
	t.colorCount = colorCount;
	for ( int c = 0; c < colorCount; c++ ) {
		t.colorFormat[c] = mrtFormats ? mrtFormats[c] : colorFmt;	// NULL = broadcast (bit-identical for existing callers)
	}
	t.hasDepth = wantDepthStencil;
	t.colorMipLevels = mipLevels;
	t.w = safeW;
	t.h = safeH;
	t.passClass = PassClassFor( colorFmt, wantDepthStencil, colorCount );

	VmaAllocationCreateInfo aci = {};
	aci.usage = VMA_MEMORY_USAGE_AUTO;

	for ( int c = 0; c < colorCount; c++ ) {
		VkImageCreateInfo ici = {};
		ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		ici.imageType = VK_IMAGE_TYPE_2D;
		ici.format = t.colorFormat[c];
		ici.extent = { (uint32_t)safeW, (uint32_t)safeH, 1 };
		ici.mipLevels = (uint32_t)mipLevels;
		ici.arrayLayers = 1;
		ici.samples = VK_SAMPLE_COUNT_1_BIT;
		ici.tiling = VK_IMAGE_TILING_OPTIMAL;
		// SAMPLED: read back by the resolve/next feature; TRANSFER_SRC: the M5
		// _currentRender capture blits from it while this is the frame target. The SSAO
		// Phase 1 mip chain renders each level as a COLOR_ATTACHMENT (already set), so no
		// transfer-dst is needed — coarse levels are drawn by a downsample shader.
		// TRANSFER_DST: the FSR2 resolve (R1/C2) copies its output back over the HDR scene
		// buffer's attachment 0. Universally supported for color formats, so unconditional.
		ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
		          | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		if ( !vkCheck( vmaCreateImage( vma, &ici, &aci, &t.colorImage[c], &t.colorAlloc[c], NULL ),
		               "vmaCreateImage(color target)" ) ) {
			FreeTargetObjects( t );
			return false;
		}
		// level-0-only view: the framebuffer attachment (fullscreen draws write level 0).
		VkImageViewCreateInfo vwi = {};
		vwi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		vwi.image = t.colorImage[c];
		vwi.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vwi.format = t.colorFormat[c];
		vwi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		vwi.subresourceRange.levelCount = 1;
		vwi.subresourceRange.layerCount = 1;
		if ( !vkCheck( vkCreateImageView( device, &vwi, NULL, &t.colorView[c] ), "vkCreateImageView(color target)" ) ) {
			FreeTargetObjects( t );
			return false;
		}
		// all-levels sample view: ssao.frag textureLods coarser mips through this.
		// Single-mip targets reuse colorView[c] (colorSampleView stays null).
		if ( mipped ) {
			VkImageViewCreateInfo svi = vwi;
			svi.subresourceRange.levelCount = (uint32_t)mipLevels;
			if ( !vkCheck( vkCreateImageView( device, &svi, NULL, &t.colorSampleView[c] ), "vkCreateImageView(color mip sample)" ) ) {
				FreeTargetObjects( t );
				return false;
			}
		}
	}

	if ( wantDepthStencil ) {
		VkImageCreateInfo ici = {};
		ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		ici.imageType = VK_IMAGE_TYPE_2D;
		ici.format = sceneDepthFormat;
		ici.extent = { (uint32_t)safeW, (uint32_t)safeH, 1 };
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

	VkImageView views[4] = {};		// up to 3 color + 1 depth (R1/A0)
	int nv = 0;
	for ( int c = 0; c < colorCount; c++ ) { views[nv++] = t.colorView[c]; }
	if ( wantDepthStencil ) { views[nv++] = t.dsView; }
	VkFramebufferCreateInfo fbi = {};
	fbi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	fbi.renderPass = t.colorClearPass;		// compatible with the load/clearDS variants
	fbi.attachmentCount = (uint32_t)nv;
	fbi.pAttachments = views;
	fbi.width = (uint32_t)safeW;
	fbi.height = (uint32_t)safeH;
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
		// mipped targets sample the whole chain (all-levels view + a maxLod-unclamped,
		// NEAREST-mip sampler so an explicit textureLod reaches every coarse level; the
		// plain TF_LINEAR sampler clamps maxLod to 0 and would pin every fetch to mip 0).
		rec.view = mipped ? t.colorSampleView[c] : t.colorView[c];
		rec.sampler = GetSampler( TF_LINEAR, TR_CLAMP, mipped );
		rec.live = true;
		rec.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		rec.isDepth = false;
		rec.isColorTarget = true;
		rec.width = safeW;
		rec.height = safeH;
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

	// SSAO Phase 1 mip chain: per-level single-level views (attachment + downsample
	// source), a framebuffer per level 1..N-1 to render the max-downsample into, and a
	// single-level ImageRec per level so it can be bound as the next level's source.
	// Level 0 reuses colorView[0] (its framebuffer is colorFb, the linearize target).
	if ( mipped ) {
		for ( int L = 0; L < mipLevels; L++ ) {
			const int lw = ( ( safeW >> L ) > 1 ) ? ( safeW >> L ) : 1;
			const int lh = ( ( safeH >> L ) > 1 ) ? ( safeH >> L ) : 1;
			VkImageView lv = t.colorView[0];
			if ( L > 0 ) {
				VkImageViewCreateInfo lvi = {};
				lvi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
				lvi.image = t.colorImage[0];
				lvi.viewType = VK_IMAGE_VIEW_TYPE_2D;
				lvi.format = colorFmt;
				lvi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				lvi.subresourceRange.baseMipLevel = (uint32_t)L;
				lvi.subresourceRange.levelCount = 1;
				lvi.subresourceRange.layerCount = 1;
				if ( !vkCheck( vkCreateImageView( device, &lvi, NULL, &t.colorLevelView[L] ), "vkCreateImageView(mip level)" ) ) {
					FreeTargetObjects( t );
					return false;
				}
				lv = t.colorLevelView[L];
				VkFramebufferCreateInfo lfb = {};
				lfb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
				lfb.renderPass = t.colorClearPass;		// compatible: same format, 1 color, no depth
				lfb.attachmentCount = 1;
				lfb.pAttachments = &lv;
				lfb.width = (uint32_t)lw;
				lfb.height = (uint32_t)lh;
				lfb.layers = 1;
				if ( !vkCheck( vkCreateFramebuffer( device, &lfb, NULL, &t.colorLevelFb[L] ), "vkCreateFramebuffer(mip level)" ) ) {
					FreeTargetObjects( t );
					return false;
				}
			}
			// a single-level sampleable ImageRec so this level can feed the next as a
			// downsample source (texelFetch lod 0; NEAREST, no filtering). isColorTarget
			// is left false — the downsample reads by absolute texel, not the flipped quad.
			ImageRec rec;
			rec.image = t.colorImage[0];
			rec.alloc = NULL;
			rec.view = lv;
			rec.sampler = GetSampler( TF_NEAREST, TR_CLAMP, false );
			rec.live = true;
			rec.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			rec.isColorTarget = false;
			rec.width = lw;
			rec.height = lh;
			ImageHandle lh2 = 0;
			for ( size_t i = 0; i < imageTable.size(); i++ ) {
				if ( !imageTable[i].live ) { imageTable[i] = rec; lh2 = (ImageHandle)( i + 1 ); break; }
			}
			if ( lh2 == 0 ) { imageTable.push_back( rec ); lh2 = (ImageHandle)imageTable.size(); }
			t.colorLevelInput[L] = lh2;
		}
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

// color + depth(-stencil) target for a depth-tested offscreen geometry pass (the
// SSAO normal G-buffer). colorCount 2 adds a second RGBA8 attachment (SSR
// material MRT, GetRenderTargetImage2). Rendered via BeginTargetPass, single-pass,
// the depth attachment written/tested but not sampled. Uses sceneDepthFormat
// (carries an unused stencil vs GL's DEPTH_COMPONENT24 — harmless).
RenderTargetHandle VulkanBackend::CreateRenderTargetColorDepth( ImageFormat fmt, int w, int h, int colorCount ) {
	if ( device == VK_NULL_HANDLE || w <= 0 || h <= 0 ) {
		return 0;
	}
	if ( fmt != IF_RGBA8 ) {
		common->Warning( "VK CreateRenderTargetColorDepth: only IF_RGBA8 supported" );
		return 0;
	}
	if ( colorCount < 1 || colorCount > 3 ) {
		common->Warning( "VK CreateRenderTargetColorDepth: colorCount must be 1-3" );
		return 0;
	}
	// colorCount 3 = the R1 velocity gbuffer: RGBA8 normal @0, RGBA8 SSR @1, RG16F velocity @2.
	const VkFormat velFormats[3] = { VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16_SFLOAT };
	const VkFormat *mrt = ( colorCount == 3 ) ? velFormats : NULL;
	int slot = AllocTargetSlot();
	if ( !CreateColorTarget( targetTable[slot], w, h, VK_FORMAT_R8G8B8A8_UNORM, colorCount, /*ds*/true, /*frameCapable*/false, /*mipLevels*/1, mrt ) ) {
		targetTable[slot] = RenderTarget();
		return 0;
	}
	common->DPrintf( "VK: created %dx%d RGBA8 color+depth render target (handle %d, %d color)\n",
		w, h, slot + 1, colorCount );
	return (RenderTargetHandle)( slot + 1 );
}

ImageHandle VulkanBackend::GetRenderTargetImage2( RenderTargetHandle rt ) {
	if ( rt != 0 && rt == mergeNormalTarget ) {
		return mergeMatSampleImage;			// merged SSR material MRT (no targetTable entry)
	}
	RenderTarget *t = LookupTarget( rt );
	return ( t && t->colorCount >= 2 ) ? t->colorSampleImage[1] : 0;
}

// third color attachment = the RG16F velocity MRT of the 3-MRT normal prepass (R1/A2).
ImageHandle VulkanBackend::GetRenderTargetImage3( RenderTargetHandle rt ) {
	RenderTarget *t = LookupTarget( rt );
	return ( t && t->colorCount >= 3 ) ? t->colorSampleImage[2] : 0;
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
VkImageView VulkanBackend::FrameDepthView() const {
	if ( frameTarget != 0 ) {
		const RenderTarget &t = targetTable[frameTarget - 1];
		if ( t.live && t.hasDepth ) { return t.dsView; }
	}
	return sceneDepthView;
}

// SSAO normal-pass merge (docs/ssao-normal-merge.md). A dedicated handle special-cased
// in GetRenderTargetImage so BindRTUnit works without a targetTable entry (the merged
// normal owns its own image/pass/fb, freed by DestroyMergeNormal — no double-free).
static const rhi::RenderTargetHandle MERGE_NORMAL_HANDLE = 0x7FFF0001u;

void VulkanBackend::DestroyMergeNormal() {
	if ( mergeNormalFb )    { vkDestroyFramebuffer( device, mergeNormalFb, NULL ); mergeNormalFb = VK_NULL_HANDLE; }
	if ( mergeNormalPass )  { vkDestroyRenderPass( device, mergeNormalPass, NULL ); mergeNormalPass = VK_NULL_HANDLE; }
	if ( mergeNormalView )  { vkDestroyImageView( device, mergeNormalView, NULL ); mergeNormalView = VK_NULL_HANDLE; }
	if ( mergeNormalImage ) { vmaDestroyImage( vma, mergeNormalImage, mergeNormalAlloc ); mergeNormalImage = VK_NULL_HANDLE; mergeNormalAlloc = NULL; }
	if ( mergeMatView )     { vkDestroyImageView( device, mergeMatView, NULL ); mergeMatView = VK_NULL_HANDLE; }
	if ( mergeMatImage )    { vmaDestroyImage( vma, mergeMatImage, mergeMatAlloc ); mergeMatImage = VK_NULL_HANDLE; mergeMatAlloc = NULL; }
	if ( mergeNormalSampleImage >= 1 && mergeNormalSampleImage <= (ImageHandle)imageTable.size() ) {
		imageTable[mergeNormalSampleImage - 1].live = false;
	}
	if ( mergeMatSampleImage >= 1 && mergeMatSampleImage <= (ImageHandle)imageTable.size() ) {
		imageTable[mergeMatSampleImage - 1].live = false;
	}
	mergeNormalSampleImage = 0;
	mergeMatSampleImage = 0;
	mergeNormalMrt = false;
	mergeNormalW = mergeNormalH = 0;
	mergeNormalFbDepth = VK_NULL_HANDLE;
	mergeNormalTarget = 0;
}

// register a sampleable RGBA8 ImageRec (view already created) into imageTable, returning
// its 1-based handle; used for the merged normal (+ optional SSR material) color images.
ImageHandle VulkanBackend::RegisterMergeSampleImage( VkImage img, VkImageView view, int w, int h ) {
	ImageRec rec;
	rec.image = img;
	rec.alloc = NULL;					// owned by the merge* allocs, freed in DestroyMergeNormal
	rec.view = view;
	rec.sampler = GetSampler( TF_LINEAR, TR_CLAMP, false );
	rec.live = true;
	rec.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	rec.isDepth = false;
	rec.isColorTarget = true;
	rec.width = w; rec.height = h;
	for ( size_t i = 0; i < imageTable.size(); i++ ) {
		if ( !imageTable[i].live ) { imageTable[i] = rec; return (ImageHandle)( i + 1 ); }
	}
	imageTable.push_back( rec );
	return (ImageHandle)imageTable.size();
}

// (re)create the normal color image (+ optional SSR material MRT), the render pass, and
// their sampleable ImageRecs at size w*h. The framebuffer (which binds the *scene* depth)
// is (re)built lazily in BeginNormalPrepass since FrameDepthImage() changes with HDR mode.
bool VulkanBackend::EnsureMergeNormal( int w, int h, bool wantMrt ) {
	if ( mergeNormalImage != VK_NULL_HANDLE && mergeNormalW == w && mergeNormalH == h
	     && mergeNormalMrt == wantMrt ) {
		return true;
	}
	DestroyMergeNormal();
	if ( sceneDepthFormat == VK_FORMAT_UNDEFINED ) {
		return false;
	}

	VkImageCreateInfo ici = {};
	ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = VK_FORMAT_R8G8B8A8_UNORM;
	ici.extent = { (uint32_t)w, (uint32_t)h, 1 };
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VmaAllocationCreateInfo aci = {};
	aci.usage = VMA_MEMORY_USAGE_AUTO;
	if ( !vkCheck( vmaCreateImage( vma, &ici, &aci, &mergeNormalImage, &mergeNormalAlloc, NULL ), "vmaCreateImage(merge normal)" ) ) {
		mergeNormalImage = VK_NULL_HANDLE; return false;
	}
	VkImageViewCreateInfo vwi = {};
	vwi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	vwi.image = mergeNormalImage;
	vwi.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vwi.format = VK_FORMAT_R8G8B8A8_UNORM;
	vwi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	vwi.subresourceRange.levelCount = 1;
	vwi.subresourceRange.layerCount = 1;
	if ( !vkCheck( vkCreateImageView( device, &vwi, NULL, &mergeNormalView ), "vkCreateImageView(merge normal)" ) ) {
		DestroyMergeNormal(); return false;
	}

	// optional 2nd color attachment (SSR rough/metal). Same format/usage as the normal.
	if ( wantMrt ) {
		if ( !vkCheck( vmaCreateImage( vma, &ici, &aci, &mergeMatImage, &mergeMatAlloc, NULL ), "vmaCreateImage(merge mat)" ) ) {
			mergeMatImage = VK_NULL_HANDLE; DestroyMergeNormal(); return false;
		}
		vwi.image = mergeMatImage;
		if ( !vkCheck( vkCreateImageView( device, &vwi, NULL, &mergeMatView ), "vkCreateImageView(merge mat)" ) ) {
			DestroyMergeNormal(); return false;
		}
	}

	// render pass: normal (+ mat) color (clear -> shader-read) + shared scene depth. The depth
	// is CLEARed and sealed by the gbuffer geometry here, then STOREd so the resumed scene pass
	// loads it for the depth-EQUAL interactions. (B1 used DON'T_CARE because it kept zfill to
	// re-seal depth afterwards; steps 2-3 skip zfill, so this pass IS the seal and MUST preserve
	// it — with DON'T_CARE the driver discards the sealed depth and the scene fails depth-EQUAL.)
	const int nColor = wantMrt ? 2 : 1;
	const int depthIdx = nColor;			// depth is the last attachment
	VkAttachmentDescription atts[3] = {};
	for ( int c = 0; c < nColor; c++ ) {
		atts[c].format = VK_FORMAT_R8G8B8A8_UNORM;
		atts[c].samples = VK_SAMPLE_COUNT_1_BIT;
		atts[c].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		atts[c].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		atts[c].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		atts[c].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		atts[c].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		atts[c].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	}
	atts[depthIdx].format = sceneDepthFormat;
	atts[depthIdx].samples = VK_SAMPLE_COUNT_1_BIT;
	atts[depthIdx].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	atts[depthIdx].storeOp = VK_ATTACHMENT_STORE_OP_STORE;			// preserve the sealed depth
	atts[depthIdx].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	atts[depthIdx].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;	// preserve the cleared (0) stencil
	atts[depthIdx].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	atts[depthIdx].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VkAttachmentReference colorRefs[2] = {
		{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL },
		{ 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL },
	};
	VkAttachmentReference depthRef = { (uint32_t)depthIdx, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
	VkSubpassDescription sub = {};
	sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	sub.colorAttachmentCount = (uint32_t)nColor;
	sub.pColorAttachments = colorRefs;
	sub.pDepthStencilAttachment = &depthRef;
	VkSubpassDependency deps[2] = {};
	deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
	deps[0].dstSubpass = 0;
	deps[0].srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
	deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
	                     | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	// outgoing: color -> shader read (SSAO/SSR sample the normal + mat); depth write -> the scene
	// pass's depth test (EARLY/LATE fragment tests read the sealed depth for depth-EQUAL).
	deps[1].srcSubpass = 0;
	deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
	deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
	                     | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
	                     | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT
	                     | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	VkRenderPassCreateInfo rpi = {};
	rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	rpi.attachmentCount = (uint32_t)( nColor + 1 ); rpi.pAttachments = atts;
	rpi.subpassCount = 1; rpi.pSubpasses = &sub;
	rpi.dependencyCount = 2; rpi.pDependencies = deps;
	if ( !vkCheck( vkCreateRenderPass( device, &rpi, NULL, &mergeNormalPass ), "vkCreateRenderPass(merge normal)" ) ) {
		DestroyMergeNormal(); return false;
	}

	// sampleable ImageRecs so SSAO/SSR bind them via GetRenderTargetImage / GetRenderTargetImage2
	mergeNormalSampleImage = RegisterMergeSampleImage( mergeNormalImage, mergeNormalView, w, h );
	if ( wantMrt ) {
		mergeMatSampleImage = RegisterMergeSampleImage( mergeMatImage, mergeMatView, w, h );
	}

	mergeNormalMrt = wantMrt;
	mergeNormalW = w; mergeNormalH = h;
	mergeNormalFb = VK_NULL_HANDLE;
	mergeNormalFbDepth = VK_NULL_HANDLE;
	mergeNormalTarget = MERGE_NORMAL_HANDLE;
	return true;
}

RenderTargetHandle VulkanBackend::BeginNormalPrepass( int w, int h, const ClearArgs *clear, bool wantMrt ) {
	if ( !frameOpen || skipFrame || device == VK_NULL_HANDLE || w <= 0 || h <= 0 ) {
		return 0;
	}
	if ( !EnsureMergeNormal( w, h, wantMrt ) ) {
		return 0;
	}
	const int nColor = wantMrt ? 2 : 1;
	// (re)build the framebuffer when the scene depth image changes (HDR toggle / resize)
	VkImage depthImg = FrameDepthImage();
	if ( mergeNormalFb == VK_NULL_HANDLE || mergeNormalFbDepth != depthImg ) {
		if ( mergeNormalFb ) { vkDestroyFramebuffer( device, mergeNormalFb, NULL ); mergeNormalFb = VK_NULL_HANDLE; }
		VkImageView views[3];
		views[0] = mergeNormalView;
		if ( wantMrt ) { views[1] = mergeMatView; }
		views[nColor] = FrameDepthView();
		VkFramebufferCreateInfo fbi = {};
		fbi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		fbi.renderPass = mergeNormalPass;
		fbi.attachmentCount = (uint32_t)( nColor + 1 ); fbi.pAttachments = views;
		fbi.width = (uint32_t)w; fbi.height = (uint32_t)h; fbi.layers = 1;
		if ( !vkCheck( vkCreateFramebuffer( device, &fbi, NULL, &mergeNormalFb ), "vkCreateFramebuffer(merge normal)" ) ) {
			mergeNormalFb = VK_NULL_HANDLE; return 0;
		}
		mergeNormalFbDepth = depthImg;
	}

	VkCommandBuffer cb = frames[frameIndex].cb;
	if ( insideScenePass ) {
		vkCmdEndRenderPass( cb );			// suspend the scene pass (like BeginTargetPass)
		insideScenePass = false;
	}
	VkClearValue cv[3] = {};
	if ( clear != NULL && clear->color ) {
		for ( int c = 0; c < nColor; c++ ) {
			cv[c].color.float32[0] = clear->rgba[0]; cv[c].color.float32[1] = clear->rgba[1];
			cv[c].color.float32[2] = clear->rgba[2]; cv[c].color.float32[3] = clear->rgba[3];
		}
	}
	cv[nColor].depthStencil.depth = 1.0f;
	VkRenderPassBeginInfo rbi = {};
	rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	rbi.renderPass = mergeNormalPass;
	rbi.framebuffer = mergeNormalFb;
	rbi.renderArea.extent = { (uint32_t)w, (uint32_t)h };
	rbi.clearValueCount = (uint32_t)( nColor + 1 );
	rbi.pClearValues = cv;
	vkCmdBeginRenderPass( cb, &rbi, VK_SUBPASS_CONTENTS_INLINE );
	// same passClass as the standalone RGBA8+depth normal target (colorCount 1 or 2) so the
	// gbuffer pipeline is shared with the standalone path
	EnterTargetPass( w, h, /*flipY*/true, mergeNormalPass, PassClassFor( VK_FORMAT_R8G8B8A8_UNORM, true, nColor ), /*colorAtt*/nColor );
	return mergeNormalTarget;
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
	// desc.tessellate (bit 18, free between RELEVANT's bit 17 and passClass at 24)
	// keeps the tessellated (patch-list, tesc+tese) and flat variants of the same
	// shader on distinct cache entries. Only honoured when the shader actually
	// carries a tess pair and the device supports tessellation.
	const bool tessellate = desc.tessellate && haveTessellation
		&& desc.shader >= 1 && desc.shader <= (ShaderHandle)shaderTable.size()
		&& shaderTable[desc.shader - 1].tesc != VK_NULL_HANDLE
		&& shaderTable[desc.shader - 1].tese != VK_NULL_HANDLE;
	// curPassClass (bits 24-31, free above RELEVANT's bit 17) keeps pipelines for
	// render-pass-incompatible destinations apart: the same shader+state built for
	// the RGBA8 swapchain scene (class 0) and the RGBA16F HDR buffer (class 2) are
	// distinct cache entries built against distinct render passes.
	const unsigned long long key = (unsigned long long)bits
		| ( (unsigned long long)( tessellate ? 1 : 0 ) << 18 )
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

	VkPipelineShaderStageCreateInfo stages[4] = {};
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = sh.vert;
	stages[0].pName = "main";
	stages[1] = stages[0];
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = sh.frag;
	uint32_t stageCount = 2;
	if ( tessellate ) {
		// tesc + tese ride alongside vert + frag; the tese re-runs the vertex
		// body on the PN-evaluated position (docs/tessellation.md)
		stages[stageCount].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[stageCount].stage = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
		stages[stageCount].module = sh.tesc;
		stages[stageCount].pName = "main";
		stageCount++;
		stages[stageCount].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[stageCount].stage = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
		stages[stageCount].module = sh.tese;
		stages[stageCount].pName = "main";
		stageCount++;
	}

	// vertex layouts (must match the GL3 VAO setups / shader locations)
	VkVertexInputBindingDescription binding = {};
	VkVertexInputAttributeDescription attrs[6] = {};
	uint32_t attrCount = 0;
	uint32_t bindingCount = 1;
	if ( desc.vertexLayout == VL_NONE ) {
		// no vertex input: the shader fetches vertices itself via device address
		// (Phase 3.2b batched zfill). No bound vertex buffer, so no binding either.
		bindingCount = 0;
		attrCount = 0;
	} else if ( desc.vertexLayout == VL_DRAWVERT ) {
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
	vin.vertexBindingDescriptionCount = bindingCount;
	vin.pVertexBindingDescriptions = bindingCount ? &binding : NULL;
	vin.vertexAttributeDescriptionCount = attrCount;
	vin.pVertexAttributeDescriptions = attrs;

	VkPipelineInputAssemblyStateCreateInfo ia = {};
	ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	// a tessellated draw feeds the tessellator triangular patches: the same
	// indexed triangle list, reinterpreted as 3-control-point patches. Skip the
	// GL primMode switch below (only immediate-mode debug draws set desc.topology).
	if ( tessellate ) {
		ia.topology = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
	} else
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
	VkPipelineColorBlendAttachmentState attArr[3] = { att, att, att };	// up to 3 color attachments (R1/A0)
	cb.attachmentCount = (uint32_t)curColorAtt;
	cb.pAttachments = ( curColorAtt > 0 ) ? attArr : NULL;

	const VkDynamicState dyn[3] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
	                                VK_DYNAMIC_STATE_DEPTH_BIAS };
	VkPipelineDynamicStateCreateInfo dsi = {};
	dsi.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dsi.dynamicStateCount = 3;
	dsi.pDynamicStates = dyn;

	// triangular patches carry 3 control points (the triangle's corners); the
	// tesc/tese do PN evaluation over the barycentric domain.
	VkPipelineTessellationStateCreateInfo ts = {};
	ts.sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO;
	ts.patchControlPoints = 3;

	VkGraphicsPipelineCreateInfo pci = {};
	pci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pci.stageCount = stageCount;
	pci.pStages = stages;
	pci.pVertexInputState = &vin;
	pci.pInputAssemblyState = &ia;
	pci.pTessellationState = tessellate ? &ts : NULL;
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
	if ( !vkCheck( vkCreateGraphicsPipelines( device, diskPipelineCache, 1, &pci, NULL, &pipeline ),
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
	// r_vkBdaZfill (Phase 3.2b consume): route the flat world-static depth-prepass through the
	// manual-vertex-fetch variant, which reads positions from the vertex buffer's DEVICE ADDRESS
	// instead of bound attributes — the first GPU-driven-draw building block. Pixel-identical
	// (same position bytes, same invariant u_mvpMatrix*pos, same clip/texcoord). Fires only for the
	// flat (non-tessellated) zfill shader over an addressable persistent buffer; streamed/skinned
	// surfaces report address 0 and fall through to the normal draw below.
	if ( r_vkBdaZfill.GetInteger() >= 1 && zfillBdaShaderHandle != 0
	     && currentDesc.shader == zfillShaderHandle && !currentDesc.tessellate ) {
		unsigned long long vbAddr = GetBufferDeviceAddress( args.vertexBuffer );
		if ( vbAddr != 0 ) {
			vbAddr += (unsigned long long)(uint32_t)args.vertexOffset;	// point at this surface's idDrawVert[0]
			ShaderHandle saved = currentDesc.shader;
			currentDesc.shader = zfillBdaShaderHandle;					// select the BDA pipeline for this draw
			VkCommandBuffer cb;
			bool ok = BindForDraw( args, cb );
			currentDesc.shader = saved;
			if ( ok ) {
				vkCmdPushConstants( cb, pipeLayout, VK_SHADER_STAGE_VERTEX_BIT, 0,
				                    (uint32_t)sizeof( vbAddr ), &vbAddr );
				vkCmdDrawIndexed( cb, (uint32_t)args.indexCount, 1, (uint32_t)args.firstIndex, 0, 0 );
				bdaZfillDraws++;
			}
			return;
		}
		bdaZfillFallback++;		// address 0 (streamed/skinned) — fall through to the normal draw
	}
	// r_vkIndirectTest: validate the Phase-3 indirect-draw seed by routing this exact draw
	// through DrawIndexedIndirect — write a 1-command VkDrawIndexedIndirectCommand into the
	// per-frame indirect ring and draw from it. Pixel-identical when the plumbing is right.
	// vertexOffset stays 0 in the command: the vertex buffer's byte offset lives in the buffer
	// binding (BindForDraw), matching the 0 vertexOffset vkCmdDrawIndexed uses in the direct path.
	if ( r_vkIndirectTest.GetBool() ) {
		VkDrawIndexedIndirectCommand cmd;
		cmd.indexCount = (uint32_t)args.indexCount;
		cmd.instanceCount = 1;
		cmd.firstIndex = (uint32_t)args.firstIndex;
		cmd.vertexOffset = 0;
		cmd.firstInstance = 0;
		BufferHandle ah = 0;
		int aofs = AllocFromRing( indirectRing[frameIndex], &cmd, (int)sizeof( cmd ), 4, 0, &ah );
		if ( ah ) {
			DrawIndexedIndirect( args, ah, aofs, 1, (int)sizeof( cmd ), 0, 0 );
			return;
		}
		// ring not ready (pre-init / overflow past a failed grow) — fall through to the direct draw
	}
	VkCommandBuffer cb;
	if ( !BindForDraw( args, cb ) ) {
		return;
	}
	vkCmdDrawIndexed( cb, (uint32_t)args.indexCount, 1, (uint32_t)args.firstIndex, 0, 0 );
}

/*
====================
VulkanBackend::DrawIndexedIndirect

The Phase-3 indirect-draw seed: same bindings as Draw, but the draw parameters come
from a GPU-resident VkDrawIndexedIndirectCommand[] (a compute cull pass will write it).
See RHI.h. The 1.4 floor guarantees the vkCmdDraw*Indirect[Count] COMMANDS exist, but the
count-buffer form needs the drawIndirectCount FEATURE and a >1 drawCount needs
multiDrawIndirect — both enabled at device creation when present, gated below when absent.
====================
*/
void VulkanBackend::DrawIndexedIndirect( const DrawArgs &args, BufferHandle argsBuffer, int argsOffset,
                                         int drawCount, int stride, BufferHandle countBuffer, int countOffset ) {
	if ( drawCount <= 0 ) {
		return;
	}
	VkBuffer ab = LookupBuffer( argsBuffer );
	if ( ab == VK_NULL_HANDLE ) {
		return;
	}
	// feature gates: without the enabled feature the command is UB / a validation error, so
	// degrade (skip + warn once) rather than issue it. Universal on desktop, so this is a net.
	if ( countBuffer && !haveDrawIndirectCount ) {
		if ( !indirectFeatureWarned ) {
			indirectFeatureWarned = true;
			common->Warning( "VK: DrawIndexedIndirect count form needs drawIndirectCount (unsupported) - skipped" );
		}
		return;
	}
	if ( drawCount > 1 && !haveMultiDrawIndirect ) {
		if ( !indirectFeatureWarned ) {
			indirectFeatureWarned = true;
			common->Warning( "VK: DrawIndexedIndirect drawCount>1 needs multiDrawIndirect (unsupported) - skipped" );
		}
		return;
	}
	VkCommandBuffer cb;
	if ( !BindForDraw( args, cb ) ) {
		return;
	}
	if ( countBuffer ) {
		VkBuffer cbuf = LookupBuffer( countBuffer );
		if ( cbuf == VK_NULL_HANDLE ) {
			return;
		}
		// GPU-written count form (Phase 3 cull output): draw min(*count, drawCount) commands
		vkCmdDrawIndexedIndirectCount( cb, ab, (VkDeviceSize)argsOffset, cbuf, (VkDeviceSize)countOffset,
		                               (uint32_t)drawCount, (uint32_t)stride );
	} else {
		vkCmdDrawIndexedIndirect( cb, ab, (VkDeviceSize)argsOffset, (uint32_t)drawCount, (uint32_t)stride );
	}
}

/*
====================
VulkanBackend::DrawZfillBatch

Phase 3.2b consume — Increment 2. One non-indexed vkCmdDrawIndirect for the whole
solid-opaque world-static depth-prepass bucket. Each item's geometry is addressed by
device-address pointers (no bound vb/ib): the batch shader fetches the index via
obj.ib[gl_VertexIndex] then the vertex via obj.vb[index], and the MVP from the same
per-object record (indexed by gl_InstanceIndex == the command's firstInstance). Per-frame
double-buffered SSBO (ObjRec[]) + VkDrawIndirectCommand[], both BU_STORAGE (host-visible,
addressable, INDIRECT usage), grown on demand. The frontend gates this to front-sided
(non-mirror) views over the flat solid bucket, so the state below matches the normal zfill.
Pixel-identical: same MVP bytes, same invariant `mvp * vec4(pos,1)` -> bit-identical depth.
====================
*/
void VulkanBackend::DrawZfillBatch( const ZfillBatchItem *items, int count,
                                    const ZfillBatchGroup *groups, int groupCount ) {
	if ( count <= 0 || !items || groupCount <= 0 || !groups || !frameOpen || skipFrame
	     || !haveBufferDeviceAddress || zfillBatchShaderHandle == 0
	     || !haveDrawIndirectFirstInstance || !haveMultiDrawIndirect ) {
		return;
	}
	EnsureScenePass();
	if ( !insideScenePass ) {
		return;
	}
	const int slot = frameIndex;
	struct ObjRec { unsigned long long vb, ib; float mvp[16]; };	// 80 B, matches std430 (align 16)
	const int objStride = (int)sizeof( ObjRec );
	const int cmdStride = (int)sizeof( VkDrawIndirectCommand );

	// grow the per-slot buffers to hold `count` objects (safe to destroy+recreate: this slot's
	// prior contents were consumed FRAMES_IN_FLIGHT frames ago, fence-waited at BeginFrame).
	if ( batchCapacity[slot] < count ) {
		int newCap = batchCapacity[slot] ? batchCapacity[slot] : 256;
		while ( newCap < count ) { newCap *= 2; }
		if ( batchObjBuf[slot] ) { DestroyBuffer( batchObjBuf[slot] ); batchObjBuf[slot] = 0; }
		if ( batchCmdBuf[slot] ) { DestroyBuffer( batchCmdBuf[slot] ); batchCmdBuf[slot] = 0; }
		batchObjBuf[slot] = CreateBuffer( BU_STORAGE, newCap * objStride, NULL );
		batchCmdBuf[slot] = CreateBuffer( BU_STORAGE, newCap * cmdStride, NULL );
		batchCapacity[slot] = ( batchObjBuf[slot] && batchCmdBuf[slot] ) ? newCap : 0;
	}
	if ( batchObjBuf[slot] == 0 || batchCmdBuf[slot] == 0 ) {
		return;		// alloc failed — skip (surfaces were skipped in the loop → a transient hole; dev path)
	}
	byte *objMap = bufferMapped[batchObjBuf[slot] - 1];
	byte *cmdMap = bufferMapped[batchCmdBuf[slot] - 1];
	if ( objMap == NULL || cmdMap == NULL ) {
		return;
	}
	// write the object records + one indirect command per item straight into host-visible memory
	for ( int i = 0; i < count; i++ ) {
		ObjRec *r = (ObjRec *)( objMap + (size_t)i * objStride );
		r->vb = items[i].vbAddr;
		r->ib = items[i].ibAddr;
		memcpy( r->mvp, items[i].mvp, sizeof( r->mvp ) );
		VkDrawIndirectCommand *c = (VkDrawIndirectCommand *)( cmdMap + (size_t)i * cmdStride );
		c->vertexCount = (uint32_t)items[i].indexCount;
		c->instanceCount = 1;
		c->firstVertex = 0;
		c->firstInstance = (uint32_t)i;			// -> gl_InstanceIndex selects ObjRec i
	}

	unsigned long long objAddr = GetBufferDeviceAddress( batchObjBuf[slot] );
	VkBuffer cmdVk = LookupBuffer( batchCmdBuf[slot] );
	if ( objAddr == 0 || cmdVk == VK_NULL_HANDLE ) {
		return;
	}

	VkCommandBuffer cb = frames[slot].cb;

	// batch pipeline: no vertex input (VL_NONE — verts fetched via BDA), the batch shader, and the
	// flat solid zfill depth state. cullType default = CT_FRONT_SIDED (frontend gates to it).
	PipelineDesc pd;
	pd.stateBits = GLS_DEPTHFUNC_LESS;
	pd.shader = zfillBatchShaderHandle;
	pd.vertexLayout = VL_NONE;
	pd.stencilState = SS_ALWAYS;
	VkPipeline pipeline = GetPipeline( pd );
	if ( pipeline == VK_NULL_HANDLE ) {
		return;
	}
	vkCmdBindPipeline( cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline );
	boundPipeline = pipeline;					// keep the redundant-bind tracking honest
	// the batch shader uses no descriptor sets (MVP/geometry all via BDA push constant), so bind none
	vkCmdPushConstants( cb, pipeLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, (uint32_t)sizeof( objAddr ), &objAddr );

	// one indirect draw per scissor group, over the group's contiguous command sub-range. All groups
	// read the SAME uploaded SSBO (firstInstance is the global item index), so nothing is re-written
	// between draws — the multi-draw hazard the single-buffer-per-call version had. Set each group's
	// scissor via the shared ApplyDynState path (scRect + dirty), then restore the caller's scissor.
	const int savedSc[4] = { scRect[0], scRect[1], scRect[2], scRect[3] };
	for ( int g = 0; g < groupCount; g++ ) {
		if ( groups[g].itemCount <= 0 || groups[g].firstItem < 0
		     || groups[g].firstItem + groups[g].itemCount > count ) {
			continue;
		}
		scRect[0] = groups[g].scissorX; scRect[1] = groups[g].scissorY;
		scRect[2] = groups[g].scissorW; scRect[3] = groups[g].scissorH;
		dynStateDirty = true;
		ApplyDynState( cb, curFlipY );			// scene viewport + this group's scissor, bias 0
		vkCmdDrawIndirect( cb, cmdVk, (VkDeviceSize)( (size_t)groups[g].firstItem * cmdStride ),
		                   (uint32_t)groups[g].itemCount, (uint32_t)cmdStride );
		bdaZfillBatchDraws++;
	}
	scRect[0] = savedSc[0]; scRect[1] = savedSc[1]; scRect[2] = savedSc[2]; scRect[3] = savedSc[3];
	dynStateDirty = true;						// force the next real draw to re-apply the caller's scissor
	bdaZfillBatched += count;
}

/*
====================
VulkanBackend::ApplyDynState

Flush the dynamic viewport/scissor/depth-bias when dirty (shared by BindForDraw and
the batched DrawZfillBatch). effFlipY selects the scene's negative-height y-flip; the
scene rects (vpRect/scRect) and bias (polyOfs factor/units) come from the Set* setters.
====================
*/
void VulkanBackend::ApplyDynState( VkCommandBuffer cb, bool effFlipY ) {
	if ( !dynStateDirty ) {
		return;
	}
	// scene pass: negative-height viewport (y-up NDC like GL) with the GL bottom-left
	// rect converted to Vulkan's top-left. Offscreen target pass: a plain top-left
	// viewport at the target's height (the shadow map's projective write reads back
	// self-consistently, matching GL).
	const int renderH = curRenderH;
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
	// shadow-map passes always want the full [0,1] depth range: the weapon/model
	// depth hack (SetDepthRange) is a scene-only concern
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

/*
====================
VulkanBackend::BindForDraw

Shared per-draw binding for Draw and DrawIndexedIndirect (viewport/scissor/depth-bias,
pipeline, set0 UBO, set1 texture set, vertex+index buffers). Returns false + draws nothing
when the frame/pass/pipeline/buffers aren't ready. Does not gate on args.indexCount — the
indirect path's count lives in its args buffer.
====================
*/
bool VulkanBackend::BindForDraw( const DrawArgs &args, VkCommandBuffer &cbOut ) {
	if ( !frameOpen || skipFrame ) {
		return false;
	}
	VkPipeline pipeline = GetPipeline( currentDesc );
	if ( pipeline == VK_NULL_HANDLE ) {
		return false;		// missing shader — degrade by not drawing
	}
	VkBuffer vb = LookupBuffer( args.vertexBuffer );
	VkBuffer ib = LookupBuffer( args.indexBuffer );
	if ( vb == VK_NULL_HANDLE || ib == VK_NULL_HANDLE ) {
		return false;
	}

	if ( insideTargetPass ) {
		// rendering into an offscreen shadow-map target: its render pass is
		// already open (BeginTargetPass/BeginCubeFacePass); don't touch the scene pass
	} else {
		EnsureScenePass();
		if ( !insideScenePass ) {
			return false;
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

	ApplyDynState( cb, effFlipY );

	if ( pipeline != boundPipeline ) {
		vkCmdBindPipeline( cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline );
		boundPipeline = pipeline;
	}

	// set 0: the slot's dynamic-offset UBO
	uint32_t dynOfs = (uint32_t)args.uniformOffset;
	vkCmdBindDescriptorSets( cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeLayout,
		0, 1, &uboSet[frameIndex], 1, &dynOfs );

	// set 2 (RR4 bindless materials): bind the persistent bindless texture array once per frame cb. Only
	// ssr_rt statically uses set 2; every other pipeline ignores it. Same pipeLayout, so the per-draw
	// set 0/1 rebinds below never disturb it.
	if ( bindlessSet != VK_NULL_HANDLE && !bindlessBoundThisCb ) {
		vkCmdBindDescriptorSets( cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeLayout, 2, 1, &bindlessSet, 0, NULL );
		bindlessBoundThisCb = true;
	}

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
		mixKey( key, (uint32_t)args.ssao );
		mixKey( key, (uint32_t)args.occlusion );
		mixKey( key, (uint32_t)args.parallax );
		mixKey( key, (uint32_t)args.shadowCubeDyn );
		bool needsTexBind = true;
		if ( boundTexSet != VK_NULL_HANDLE && boundTexKey == key ) {
			texSet = boundTexSet;
			needsTexBind = false;
		} else {
			// The cross-frame cache has no natural eviction: wandering a large level
			// accumulates unique (texture x light x ssao x occlusion) combos forever,
			// and once its live set count reaches texturePool's capacity
			// vkAllocateDescriptorSets fails, this Draw bails, and lit geometry drops
			// out (the level goes dark, self-lit GUI screens aside). Flush the whole
			// cache at a high-water mark well under the pool size (the sets retire
			// behind the fence and rebuild on demand); with FRAMES_IN_FLIGHT slots the
			// peak occupancy stays ~(FRAMES_IN_FLIGHT+1)*cap, inside the pool's 4x cap.
			if ( texturePool && (int)textureSetCache.size() >= MAX_FRAME_SETS ) {
				InvalidateTextureSets();
			}
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
					return false;
				}
				// bindings 0-7 = units, 8 = shadow cube, 9 = SSAO, 10 = occlusion map,
				// 11 = parallax height map. Empty slots take a dummy typed for what the
				// shaders statically declare: unit 7 is interaction.frag's sampler2DShadow
				// and 8 its samplerCubeShadow (depth-compare dummies until M7 shadow maps);
				// everything else is sampler2D (white). Real handles always win.
				VkDescriptorImageInfo infos[13];
				VkWriteDescriptorSet writes[13];
				const ImageRec &dummy = imageTable[dummyImage - 1];
				for ( int i = 0; i < 13; i++ ) {
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
					} else if ( i == 9 && args.ssao >= 1 && args.ssao <= (ImageHandle)imageTable.size()
					            && imageTable[args.ssao - 1].live ) {
						const ImageRec &rec = imageTable[args.ssao - 1];		// SSAO/GTAO buffer (else white)
						sampler = rec.sampler;
						view = rec.view;
					} else if ( i == 10 && args.occlusion >= 1 && args.occlusion <= (ImageHandle)imageTable.size()
					            && imageTable[args.occlusion - 1].live ) {
						const ImageRec &rec = imageTable[args.occlusion - 1];	// baked occlusion map (else white)
						sampler = rec.sampler;
						view = rec.view;
					} else if ( i == 11 && args.parallax >= 1 && args.parallax <= (ImageHandle)imageTable.size()
					            && imageTable[args.parallax - 1].live ) {
						const ImageRec &rec = imageTable[args.parallax - 1];	// parallax height map (else white)
						sampler = rec.sampler;
						view = rec.view;
					} else if ( i == 12 ) {
						// dynamic-layer shadow cube (lever B). samplerCubeShadow -> the cube depth
						// dummy when no movers-only layer is bound, mirroring unit 8.
						if ( args.shadowCubeDyn >= 1 && args.shadowCubeDyn <= (ImageHandle)imageTable.size()
						     && imageTable[args.shadowCubeDyn - 1].live ) {
							const ImageRec &rec = imageTable[args.shadowCubeDyn - 1];
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
				vkUpdateDescriptorSets( device, 13, writes, 0, NULL );
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
	cbOut = cb;
	return true;
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
			VkDescriptorImageInfo infos[13];
			VkWriteDescriptorSet writes[13];
			const ImageRec &dummy = imageTable[dummyImage - 1];
			for ( int i = 0; i < 13; i++ ) {
				// units 8 and 12 are samplerCubeShadow -> the cube depth dummy; 7 is the 2D shadow dummy
				const bool cubeShadow = ( i == 8 || i == 12 );
				infos[i] = {};
				infos[i].sampler = ( i == 7 ) ? dummyShadow2D.sampler : ( cubeShadow ? dummyShadowCube.sampler : dummy.sampler );
				infos[i].imageView = ( i == 7 ) ? dummyShadow2D.view : ( cubeShadow ? dummyShadowCube.view : dummy.view );
				infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				writes[i] = {};
				writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
				writes[i].dstSet = texSet;
				writes[i].dstBinding = (uint32_t)i;
				writes[i].descriptorCount = 1;
				writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
				writes[i].pImageInfo = &infos[i];
			}
			vkUpdateDescriptorSets( device, 13, writes, 0, NULL );
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
// ---------------------------------------------------------------------------
// Deep-dive debug HUD (r_vkDebugHud) + optional NVIDIA telemetry via NVML.
//
// NVML (libnvidia-ml) is loaded lazily and is entirely optional: on non-NVIDIA
// GPUs, or when the library isn't installed, every field reads back "unavailable"
// and the HUD shows N/A. It is the boost-clock / thermal / power canary the
// device-loss hunt needs — Vulkan itself exposes none of those.
// ---------------------------------------------------------------------------
// NVML entry points we use (subset). Typedef'd locally so we don't need the NVML SDK headers.
typedef int ( *PFN_nvmlInit )( void );                                // nvmlInit_v2
typedef int ( *PFN_nvmlDeviceGetHandleByIndex )( unsigned, void ** ); // _v2
typedef int ( *PFN_nvmlDeviceGetClockInfo )( void *, int, unsigned * );
typedef int ( *PFN_nvmlDeviceGetTemperature )( void *, int, unsigned * );
typedef int ( *PFN_nvmlDeviceGetPowerUsage )( void *, unsigned * );
typedef int ( *PFN_nvmlDeviceGetEnforcedPowerLimit )( void *, unsigned * );
typedef int ( *PFN_nvmlDeviceGetMemoryInfo )( void *, void * );
typedef int ( *PFN_nvmlDeviceGetPerformanceState )( void *, int * );  // P0..P15 (0 = max perf)

static struct NvmlApi {
	bool      tried = false;
	uintptr_t lib = 0;
	void     *dev = NULL;
	PFN_nvmlInit                        Init = NULL;
	PFN_nvmlDeviceGetHandleByIndex      GetHandle = NULL;
	PFN_nvmlDeviceGetClockInfo          GetClock = NULL;
	PFN_nvmlDeviceGetTemperature        GetTemp = NULL;
	PFN_nvmlDeviceGetPowerUsage         GetPower = NULL;
	PFN_nvmlDeviceGetEnforcedPowerLimit GetPowerLimit = NULL;
	PFN_nvmlDeviceGetMemoryInfo         GetMem = NULL;
	PFN_nvmlDeviceGetPerformanceState   GetPstate = NULL;
} nvml;

static bool NvmlEnsure() {
	if ( nvml.tried ) {
		return nvml.dev != NULL;
	}
	nvml.tried = true; // only attempt to load once per session

#ifdef _WIN32
	nvml.lib = Sys_DLL_Load( "nvml.dll" );
#else
	nvml.lib = Sys_DLL_Load( "libnvidia-ml.so.1" );
	if ( !nvml.lib ) {
		nvml.lib = Sys_DLL_Load( "libnvidia-ml.so" );
	}
#endif
	if ( !nvml.lib ) {
		return false; // not an NVIDIA setup, or the management lib isn't installed
	}

	nvml.Init          = (PFN_nvmlInit)                        Sys_DLL_GetProcAddress( nvml.lib, "nvmlInit_v2" );
	nvml.GetHandle     = (PFN_nvmlDeviceGetHandleByIndex)      Sys_DLL_GetProcAddress( nvml.lib, "nvmlDeviceGetHandleByIndex_v2" );
	nvml.GetClock      = (PFN_nvmlDeviceGetClockInfo)          Sys_DLL_GetProcAddress( nvml.lib, "nvmlDeviceGetClockInfo" );
	nvml.GetTemp       = (PFN_nvmlDeviceGetTemperature)        Sys_DLL_GetProcAddress( nvml.lib, "nvmlDeviceGetTemperature" );
	nvml.GetPower      = (PFN_nvmlDeviceGetPowerUsage)         Sys_DLL_GetProcAddress( nvml.lib, "nvmlDeviceGetPowerUsage" );
	nvml.GetPowerLimit = (PFN_nvmlDeviceGetEnforcedPowerLimit) Sys_DLL_GetProcAddress( nvml.lib, "nvmlDeviceGetEnforcedPowerLimit" );
	nvml.GetMem        = (PFN_nvmlDeviceGetMemoryInfo)         Sys_DLL_GetProcAddress( nvml.lib, "nvmlDeviceGetMemoryInfo" );
	nvml.GetPstate     = (PFN_nvmlDeviceGetPerformanceState)  Sys_DLL_GetProcAddress( nvml.lib, "nvmlDeviceGetPerformanceState" );

	if ( !nvml.Init || !nvml.GetHandle || nvml.Init() != 0 /*NVML_SUCCESS==0*/ ) {
		return false;
	}
	if ( nvml.GetHandle( 0, &nvml.dev ) != 0 || nvml.dev == NULL ) {
		nvml.dev = NULL;
		return false;
	}
	return true;
}

// Sample NVML into the ring, throttled to ~10 Hz. Called once per frame from BeginFrame while the
// device is alive (never after a loss — by then the ring already holds the pre-crash history).
static void VkSampleTelemetry() {
	if ( !NvmlEnsure() ) {
		return;
	}
	unsigned int now = Sys_Milliseconds();
	if ( vkTelemetryCount > 0 && ( now - vkTelemetryLastMs ) < 100 ) {
		return;
	}
	vkTelemetryLastMs = now;

	VkTelemetrySample s;
	s.timeMs = now;
	unsigned v = 0;
	if ( nvml.GetClock  && nvml.GetClock( nvml.dev, 1 /*SM*/, &v ) == 0 )  { s.smClockMHz = v; }
	if ( nvml.GetClock  && nvml.GetClock( nvml.dev, 2 /*MEM*/, &v ) == 0 ) { s.memClockMHz = v; }
	if ( nvml.GetTemp   && nvml.GetTemp( nvml.dev, 0 /*GPU*/, &v ) == 0 )  { s.tempC = v; }
	if ( nvml.GetPower  && nvml.GetPower( nvml.dev, &v ) == 0 )            { s.powerW = ( v + 500 ) / 1000; }
	int ps = 32;
	if ( nvml.GetPstate && nvml.GetPstate( nvml.dev, &ps ) == 0 )          { s.pstate = (unsigned)ps; }

	vkTelemetryRing[vkTelemetryHead] = s;
	vkTelemetryHead = ( vkTelemetryHead + 1 ) % VK_TELEMETRY_RING;
	if ( vkTelemetryCount < VK_TELEMETRY_RING ) {
		vkTelemetryCount++;
	}

	// slow-changing extras for the HUD's power-limit / VRAM lines (~1 Hz)
	if ( vkTelemetryExtraMs == 0 || ( now - vkTelemetryExtraMs ) >= 1000 ) {
		vkTelemetryExtraMs = now;
		if ( nvml.GetPowerLimit && nvml.GetPowerLimit( nvml.dev, &v ) == 0 ) { vkTelemetryPowerLimitW = ( v + 500 ) / 1000; }
		if ( nvml.GetMem ) {
			// nvmlMemory_t (v1) = { unsigned long long total, free, used }; over-size the buffer defensively.
			unsigned long long mem[8] = { 0 };
			if ( nvml.GetMem( nvml.dev, mem ) == 0 ) {
				vkTelemetryMemTotalMB = mem[0] / ( 1024ull * 1024ull );
				vkTelemetryMemUsedMB  = mem[2] / ( 1024ull * 1024ull );
			}
		}
	}
}

// Newest ring sample for the HUD. Returns false if NVML is unavailable or nothing sampled yet.
static bool VkLatestTelemetry( VkTelemetrySample &out ) {
	if ( !NvmlEnsure() || vkTelemetryCount == 0 ) {
		return false;
	}
	out = vkTelemetryRing[ ( vkTelemetryHead - 1 + VK_TELEMETRY_RING ) % VK_TELEMETRY_RING ];
	return true;
}

void VulkanBackend::DrawDebugHud() {
#ifndef IMGUI_DISABLE
	// VRAM from VMA's per-heap budget; sum device-local heaps for the "app VRAM" figure.
	uint64_t vramUsed = 0, vramBudget = 0, hostUsed = 0;
	if ( vma != NULL && physical != VK_NULL_HANDLE ) {
		VkPhysicalDeviceMemoryProperties mp;
		vkGetPhysicalDeviceMemoryProperties( physical, &mp );
		VmaBudget budgets[VK_MAX_MEMORY_HEAPS];
		vmaGetHeapBudgets( vma, budgets );
		for ( uint32_t i = 0; i < mp.memoryHeapCount; ++i ) {
			if ( mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ) {
				vramUsed   += budgets[i].usage;
				vramBudget += budgets[i].budget;
			} else {
				hostUsed   += budgets[i].usage;
			}
		}
	}

	VkTelemetrySample gpu;
	const bool haveGpu = VkLatestTelemetry( gpu );

	ImGui::SetNextWindowPos( ImVec2( 12.0f, 12.0f ), ImGuiCond_Always );
	ImGui::SetNextWindowBgAlpha( 0.72f );
	ImGui::SetNextWindowSizeConstraints( ImVec2( 250.0f, 0.0f ), ImVec2( 560.0f, 10000.0f ) );
	const int flags = ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration
		| ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing;
	if ( !ImGui::Begin( "VkDebugHud", NULL, flags ) ) {
		ImGui::End();
		return;
	}

	ImGui::TextUnformatted( "VULKAN DEEP-DIVE" );
	ImGui::SameLine();
	if ( vkDeviceDead ) {
		ImGui::TextColored( ImVec4( 1.0f, 0.30f, 0.30f, 1.0f ), "DEVICE LOST" );
	} else {
		ImGui::TextColored( ImVec4( 0.40f, 0.90f, 0.45f, 1.0f ), "device OK" );
	}
	ImGui::Separator();

	// --- GPU canary (NVML): clock coloured as it climbs toward the boost danger zone ---
	if ( haveGpu ) {
		ImVec4 clkCol( 0.55f, 0.95f, 0.55f, 1.0f );
		if ( gpu.smClockMHz >= 1900 )      clkCol = ImVec4( 1.0f, 0.45f, 0.30f, 1.0f );
		else if ( gpu.smClockMHz >= 1700 ) clkCol = ImVec4( 1.0f, 0.85f, 0.35f, 1.0f );
		// P-state: a frame cap makes this oscillate P8<->P0, and the big up-ramps are the di/dt
		// transients suspected of killing a marginal card. Colour it like the clock.
		if ( gpu.pstate <= 15 ) {
			ImGui::TextColored( clkCol, "P-state  : P%u", gpu.pstate );
		}
		ImGui::TextColored( clkCol, "SM clock : %u MHz", gpu.smClockMHz );
		ImGui::Text( "Mem clock: %u MHz", gpu.memClockMHz );
		ImVec4 tCol = ( gpu.tempC >= 83 ) ? ImVec4( 1.0f, 0.5f, 0.3f, 1.0f ) : ImVec4( 0.80f, 0.80f, 0.82f, 1.0f );
		ImGui::TextColored( tCol, "Temp     : %u C", gpu.tempC );
		if ( vkTelemetryPowerLimitW > 0 ) {
			ImGui::Text( "Power    : %u / %u W", gpu.powerW, vkTelemetryPowerLimitW );
		} else {
			ImGui::Text( "Power    : %u W", gpu.powerW );
		}
	} else {
		ImGui::TextDisabled( "GPU clock/temp/power: NVML N/A" );
	}
	ImGui::Separator();

	// --- VRAM (VMA app view + NVML whole-GPU view) ---
	if ( vramBudget > 0 ) {
		const double usedMB = (double)vramUsed   / ( 1024.0 * 1024.0 );
		const double budMB  = (double)vramBudget / ( 1024.0 * 1024.0 );
		ImGui::Text( "VRAM app : %.0f / %.0f MB", usedMB, budMB );
		ImGui::ProgressBar( (float)( (double)vramUsed / (double)vramBudget ), ImVec2( 210.0f, 0.0f ) );
	} else {
		ImGui::TextDisabled( "VRAM app : VMA budget N/A" );
	}
	if ( haveGpu && vkTelemetryMemTotalMB > 0 ) {
		ImGui::Text( "VRAM GPU : %llu / %llu MB", (unsigned long long)vkTelemetryMemUsedMB, (unsigned long long)vkTelemetryMemTotalMB );
	}
	if ( hostUsed > 0 ) {
		ImGui::Text( "Host mem : %.0f MB", (double)hostUsed / ( 1024.0 * 1024.0 ) );
	}
	ImGui::Separator();

	// --- Validation errors + recent messages ---
	const int errors = vkValidationErrors;
	ImVec4 eCol = ( errors > 0 ) ? ImVec4( 1.0f, 0.5f, 0.3f, 1.0f ) : ImVec4( 0.40f, 0.90f, 0.45f, 1.0f );
	ImGui::TextColored( eCol, "Validation errors: %d", errors );
	{
		std::lock_guard<std::mutex> lock( vkDebugMsgMutex );
		const size_t n = vkDebugMsgHistory.size();
		if ( n == 0 ) {
			ImGui::TextDisabled( "no validation/debug messages" );
		} else {
			const size_t showN = ( n < 4 ) ? n : 4;
			for ( size_t i = n - showN; i < n; ++i ) {
				char buf[120];
				idStr::Copynz( buf, vkDebugMsgHistory[i].c_str(), sizeof( buf ) );
				ImGui::TextColored( ImVec4( 0.82f, 0.78f, 0.60f, 1.0f ), "- %s", buf );
			}
		}
	}

	ImGui::End();
#endif
}

void VK_ImGuiDrawDebugHud() {
#ifndef IMGUI_DISABLE
	if ( vkBackend.ImGuiUp() ) {
		vkBackend.DrawDebugHud();
	}
#endif
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
