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

#include "renderer/tr_local.h"
#include "renderer/rhi/RHI.h"

// dev-time validation layer (VK_LAYER_KHRONOS_validation); default on while
// the backend is being brought up — every milestone's exit bar includes
// "validation clean". Not archived: a missing layer must never stick.
static idCVar r_vkValidation( "r_vkValidation", "1", CVAR_RENDERER | CVAR_BOOL,
	"Vulkan: enable the Khronos validation layer if installed (dev)" );
// explicit adapter pick; -1 = auto (first discrete GPU, else first usable)
static idCVar r_vkDevice( "r_vkDevice", "-1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER,
	"Vulkan: physical device index to use (-1 = auto-select)" );

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
	virtual void	BeginTargetPass( RenderTargetHandle rt, const ClearArgs *clear ) { if ( frameOpen ) fakePassDepth++; }
	virtual void	EndPass();
	virtual void	SetViewport( int x, int y, int w, int h ) {}
	virtual void	SetScissor( int x, int y, int w, int h ) {}

	// ---- resources: M2+ (returning 0 is the documented "absent" contract) ----
	virtual BufferHandle	CreateBuffer( BufferUsage, int, const void * ) { return 0; }
	virtual void			UpdateBuffer( BufferHandle, int, int, const void * ) {}
	virtual void			DestroyBuffer( BufferHandle ) {}
	virtual ImageHandle		CreateImage( ImageFormat, int, int, const void * ) { return 0; }
	virtual void			DestroyImage( ImageHandle ) {}
	virtual ShaderHandle	LoadShader( const char * ) { return 0; }

	virtual RenderTargetHandle	CreateRenderTarget( ImageFormat, int, int ) { return 0; }
	virtual RenderTargetHandle	CreateRenderTargetCube( ImageFormat, int ) { return 0; }
	virtual RenderTargetHandle	CreateRenderTargetColorDepth( ImageFormat, int, int, int ) { return 0; }
	virtual RenderTargetHandle	CreateRenderTargetColorDepthStencil( ImageFormat, int, int ) { return 0; }
	virtual void				DestroyRenderTarget( RenderTargetHandle ) {}
	virtual void				SetFrameTarget( RenderTargetHandle ) {}
	virtual void				BeginCubeFacePass( RenderTargetHandle, int, const ClearArgs * ) { if ( frameOpen ) fakePassDepth++; }
	virtual ImageHandle			GetRenderTargetImage( RenderTargetHandle ) { return 0; }
	virtual ImageHandle			GetRenderTargetImage2( RenderTargetHandle ) { return 0; }

	virtual int		AllocUniforms( const void *, int, BufferHandle *buffer ) { if ( buffer ) *buffer = 0; return 0; }
	virtual int		AllocVertices( const void *, int, BufferHandle *buffer ) { if ( buffer ) *buffer = 0; return 0; }
	virtual int		AllocIndices( const void *, int, BufferHandle *buffer ) { if ( buffer ) *buffer = 0; return 0; }
	virtual int		StreamGeneration() { return 0; }

	// ---- drawing: M2+ ----
	virtual void	BindPipeline( const PipelineDesc & ) {}
	virtual void	Draw( const DrawArgs & ) {}
	virtual void	CopyFramebufferToImage( ImageHandle, int, int ) {}
	virtual void	DrawImmediate( const void *, int, unsigned int, const float[16], bool ) {}

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
	VkRenderPass				passClear = VK_NULL_HANDLE;
	VkRenderPass				passLoad = VK_NULL_HANDLE;
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

	// instance-level Vulkan version — the loader must give us 1.1
	uint32_t instVersion = VK_API_VERSION_1_0;
	vkEnumerateInstanceVersion( &instVersion );
	if ( instVersion < VK_API_VERSION_1_1 ) {
		common->Warning( "VK: instance only supports Vulkan 1.0 (baseline is 1.1)" );
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
	app.apiVersion = VK_API_VERSION_1_1;

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

		if ( props.apiVersion < VK_API_VERSION_1_1 ) {
			common->Printf( "VK:   skipped (needs Vulkan 1.1)\n" );
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

	VkDeviceCreateInfo dci = {};
	dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	dci.queueCreateInfoCount = queueCount;
	dci.pQueueCreateInfos = queues;
	dci.enabledExtensionCount = 1;
	dci.ppEnabledExtensionNames = devExts;
	// no device features needed to clear + blit (M1)

	if ( !vkCheck( vkCreateDevice( physical, &dci, NULL, &device ), "vkCreateDevice" ) ) {
		return false;
	}
	vkGetDeviceQueue( device, gfxFamily, 0, &gfxQueue );
	vkGetDeviceQueue( device, presentFamily, 0, &presentQueue );

	VmaAllocatorCreateInfo aci = {};
	aci.physicalDevice = physical;
	aci.device = device;
	aci.instance = instance;
	aci.vulkanApiVersion = VK_API_VERSION_1_1;
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
	return true;
}

/*
====================
VulkanBackend::DestroySwapchain
====================
*/
void VulkanBackend::DestroySwapchain( bool destroyHandle ) {
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

	// depth-stencil image
	ici.format = sceneDepthFormat;
	ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	if ( !vkCheck( vmaCreateImage( vma, &ici, &vci, &sceneDepth, &sceneDepthAlloc, NULL ), "vmaCreateImage(scene depth)" ) ) {
		return false;
	}
	vwi.image = sceneDepth;
	vwi.format = sceneDepthFormat;
	vwi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
	if ( !vkCheck( vkCreateImageView( device, &vwi, NULL, &sceneDepthView ), "vkCreateImageView(scene depth)" ) ) {
		return false;
	}

	// render passes: clear + load variants, color ends TRANSFER_SRC for the blit
	for ( int variant = 0; variant < 2; variant++ ) {
		const bool isClear = ( variant == 0 );

		VkAttachmentDescription atts[2] = {};
		atts[0].format = VK_FORMAT_R8G8B8A8_UNORM;
		atts[0].samples = VK_SAMPLE_COUNT_1_BIT;
		atts[0].loadOp = isClear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
		atts[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		atts[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		atts[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		atts[0].initialLayout = isClear ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		atts[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

		atts[1].format = sceneDepthFormat;
		atts[1].samples = VK_SAMPLE_COUNT_1_BIT;
		atts[1].loadOp = isClear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
		atts[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		atts[1].stencilLoadOp = isClear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
		atts[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
		atts[1].initialLayout = isClear ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
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

		VkRenderPass *dst = isClear ? &passClear : &passLoad;
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
	if ( !CreateSwapchain() || !CreateSceneTargets() || !CreateFrameSlots() ) {
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

	frameOpen = true;
	skipFrame = false;
	insideScenePass = false;
	sceneWritten = false;
	fakePassDepth = 0;
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
	const bool doClear = ( clear != NULL ) || !sceneEverWritten;

	VkClearValue cv[2] = {};
	if ( clear != NULL && clear->color ) {
		cv[0].color.float32[0] = clear->rgba[0];
		cv[0].color.float32[1] = clear->rgba[1];
		cv[0].color.float32[2] = clear->rgba[2];
		cv[0].color.float32[3] = clear->rgba[3];
	}
	cv[1].depthStencil.depth = 1.0f;
	cv[1].depthStencil.stencil = clear != NULL ? clear->stencilValue : 0;

	VkRenderPassBeginInfo rbi = {};
	rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	rbi.renderPass = doClear ? passClear : passLoad;
	rbi.framebuffer = sceneFb;
	rbi.renderArea.extent = sceneExtent;
	rbi.clearValueCount = 2;
	rbi.pClearValues = cv;
	vkCmdBeginRenderPass( frames[frameIndex].cb, &rbi, VK_SUBPASS_CONTENTS_INLINE );

	insideScenePass = true;
	sceneWritten = true;
	sceneEverWritten = true;
}

/*
====================
VulkanBackend::EndPass
====================
*/
void VulkanBackend::EndPass() {
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

	// swapchain image → PRESENT
	VkImageMemoryBarrier toPresent = toDst;
	toPresent.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	toPresent.dstAccessMask = 0;
	toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	vkCmdPipelineBarrier( f.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
		0, 0, NULL, 0, NULL, 1, &toPresent );

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

	frameIndex = ( frameIndex + 1 ) % FRAMES_IN_FLIGHT;
}

} // namespace rhi

#endif // DHEWM3_VULKAN
