/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.h for license header)
===========================================================================
*/

// GPU tracer for the AO baker (aobake_gpu.h, docs/occlusion-maps.md).
//
// The bake's whole cost is the hemisphere ray casting (128 rays per texel by default); the CPU walks
// a uniform grid per ray. Here the model's occluders go into one hardware BLAS and every texel becomes
// one compute thread that casts its rays with GL_EXT_ray_query (shaders/aobake.comp). The tracer owns a
// small headless Vulkan device -- no surface, no swapchain, nothing shared with the renderer -- so it
// runs the same whether the game renders with GL3 or Vulkan, under xvfb, or from the lazy auto-bake
// mid-frame. Everything around the tracing (UV rasterization, bump cavity, contrast, dilation, TGA
// output) stays on the CPU in aobake.cpp and is shared with the CPU path.

#include "sys/platform.h"
#include "framework/Common.h"

#include "tools/compilers/aobake/aobake_gpu.h"

#include <vulkan/vulkan.h>
#include <vector>
#include <cstring>

extern "C" bool Dude_GetEmbeddedShader( const char *name, const unsigned char **data, int *len );

namespace {

struct aoBuf_t {
	VkBuffer		buf;
	VkDeviceMemory	mem;
	VkDeviceSize	size;
	void *			map;		// host-visible buffers stay mapped
	VkDeviceAddress	addr;		// buffers created with SHADER_DEVICE_ADDRESS
};

struct aoGpu_t {
	VkInstance			instance;
	VkPhysicalDevice	phys;
	VkDevice			device;
	VkQueue				queue;
	uint32_t			family;
	VkPhysicalDeviceMemoryProperties memProps;
	VkDeviceSize		scratchAlign;
	char				name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];

	VkCommandPool		pool;
	VkCommandBuffer		cmd;
	VkFence				fence;

	VkDescriptorSetLayout setLayout;
	VkPipelineLayout	layout;
	VkPipeline			pipeline;
	VkDescriptorPool	descPool;
	VkDescriptorSet		set;

	PFN_vkCreateAccelerationStructureKHR			createAS;
	PFN_vkDestroyAccelerationStructureKHR			destroyAS;
	PFN_vkGetAccelerationStructureBuildSizesKHR		buildSizes;
	PFN_vkCmdBuildAccelerationStructuresKHR			cmdBuildAS;
	PFN_vkGetAccelerationStructureDeviceAddressKHR	asAddress;

	aoBuf_t				verts, blasMem, inst, tlasMem, scratch;
	VkAccelerationStructureKHR blas, tlas;
	int					numTris;

	aoBuf_t				hemi, samples, results;
	int					rays;

	bool				broken;		// a device error happened: unusable until re-acquired
};

aoGpu_t *	aoGpu = NULL;
int			aoGpuRefs = 0;
bool		aoGpuFailed = false;	// init failed in the current acquisition chain: don't retry per model

// push constants; layout matches aobake.comp's PC block
struct aoPush_t {
	uint32_t	count;
	uint32_t	rays;
	float		maxDist;
	float		bias;
	float		contact;
};

// rays cast per dispatch: keeps one submission well under a driver watchdog (Windows TDR is 2 s) on
// a slow GPU, at any ray count, while a big surface still goes in a few large dispatches
const int AO_GPU_RAYS_PER_DISPATCH = 32 << 20;

bool AOGpu_HasExtension( const std::vector<VkExtensionProperties> &exts, const char *name ) {
	for ( size_t i = 0; i < exts.size(); i++ ) {
		if ( strcmp( exts[i].extensionName, name ) == 0 ) {
			return true;
		}
	}
	return false;
}

bool AOGpu_MemoryType( const aoGpu_t &g, uint32_t typeBits, VkMemoryPropertyFlags want, uint32_t &out ) {
	for ( uint32_t i = 0; i < g.memProps.memoryTypeCount; i++ ) {
		if ( ( typeBits & ( 1u << i ) ) && ( g.memProps.memoryTypes[i].propertyFlags & want ) == want ) {
			out = i;
			return true;
		}
	}
	return false;
}

void AOGpu_FreeBuffer( aoGpu_t &g, aoBuf_t &b ) {
	if ( b.map ) {
		vkUnmapMemory( g.device, b.mem );
	}
	if ( b.buf ) {
		vkDestroyBuffer( g.device, b.buf, NULL );
	}
	if ( b.mem ) {
		vkFreeMemory( g.device, b.mem, NULL );
	}
	memset( &b, 0, sizeof( b ) );
}

// (Re)create `b` with at least `size` bytes. Host buffers are coherent and stay mapped; the rest
// are device-local. Nothing is ever in flight here (every submission is waited on), so an old
// buffer can be dropped at once.
bool AOGpu_Reserve( aoGpu_t &g, aoBuf_t &b, VkDeviceSize size, VkBufferUsageFlags usage, bool host ) {
	if ( b.buf && b.size >= size ) {
		return true;
	}
	AOGpu_FreeBuffer( g, b );

	VkBufferCreateInfo bi = {};
	bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bi.size = size;
	bi.usage = usage;
	bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if ( vkCreateBuffer( g.device, &bi, NULL, &b.buf ) != VK_SUCCESS ) {
		return false;
	}
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements( g.device, b.buf, &req );

	const VkMemoryPropertyFlags want = host ? ( VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT )
	                                        : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	VkMemoryAllocateFlagsInfo flags = {};
	flags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
	flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
	VkMemoryAllocateInfo ai = {};
	ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	ai.pNext = ( usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT ) ? &flags : NULL;
	ai.allocationSize = req.size;
	if ( !AOGpu_MemoryType( g, req.memoryTypeBits, want, ai.memoryTypeIndex )
	  || vkAllocateMemory( g.device, &ai, NULL, &b.mem ) != VK_SUCCESS
	  || vkBindBufferMemory( g.device, b.buf, b.mem, 0 ) != VK_SUCCESS ) {
		AOGpu_FreeBuffer( g, b );
		return false;
	}
	if ( host && vkMapMemory( g.device, b.mem, 0, VK_WHOLE_SIZE, 0, &b.map ) != VK_SUCCESS ) {
		b.map = NULL;
		AOGpu_FreeBuffer( g, b );
		return false;
	}
	if ( usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT ) {
		VkBufferDeviceAddressInfo di = {};
		di.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
		di.buffer = b.buf;
		b.addr = vkGetBufferDeviceAddress( g.device, &di );
	}
	b.size = size;
	return true;
}

bool AOGpu_BeginCommands( aoGpu_t &g ) {
	VkCommandBufferBeginInfo bi = {};
	bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	return vkResetCommandBuffer( g.cmd, 0 ) == VK_SUCCESS && vkBeginCommandBuffer( g.cmd, &bi ) == VK_SUCCESS;
}

// End, submit and wait. Any failure marks the tracer broken (most likely a lost device).
bool AOGpu_SubmitAndWait( aoGpu_t &g, const char *what ) {
	VkResult r = vkEndCommandBuffer( g.cmd );
	if ( r == VK_SUCCESS ) {
		r = vkResetFences( g.device, 1, &g.fence );
	}
	if ( r == VK_SUCCESS ) {
		VkSubmitInfo si = {};
		si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		si.commandBufferCount = 1;
		si.pCommandBuffers = &g.cmd;
		r = vkQueueSubmit( g.queue, 1, &si, g.fence );
	}
	if ( r == VK_SUCCESS ) {
		r = vkWaitForFences( g.device, 1, &g.fence, VK_TRUE, UINT64_MAX );
	}
	if ( r != VK_SUCCESS ) {
		common->Warning( "bakeAO: GPU %s failed (VkResult %d) - baking the rest on the CPU", what, (int)r );
		g.broken = true;
		return false;
	}
	return true;
}

void AOGpu_FreeOccluders( aoGpu_t &g ) {
	if ( g.tlas ) {
		g.destroyAS( g.device, g.tlas, NULL );
		g.tlas = VK_NULL_HANDLE;
	}
	if ( g.blas ) {
		g.destroyAS( g.device, g.blas, NULL );
		g.blas = VK_NULL_HANDLE;
	}
	g.numTris = 0;
}

void AOGpu_Destroy( aoGpu_t &g ) {
	if ( g.device ) {
		vkDeviceWaitIdle( g.device );
		AOGpu_FreeOccluders( g );
		aoBuf_t *bufs[] = { &g.verts, &g.blasMem, &g.inst, &g.tlasMem, &g.scratch, &g.hemi, &g.samples, &g.results };
		for ( size_t i = 0; i < sizeof( bufs ) / sizeof( bufs[0] ); i++ ) {
			AOGpu_FreeBuffer( g, *bufs[i] );
		}
		if ( g.pipeline )  vkDestroyPipeline( g.device, g.pipeline, NULL );
		if ( g.layout )    vkDestroyPipelineLayout( g.device, g.layout, NULL );
		if ( g.descPool )  vkDestroyDescriptorPool( g.device, g.descPool, NULL );
		if ( g.setLayout ) vkDestroyDescriptorSetLayout( g.device, g.setLayout, NULL );
		if ( g.fence )     vkDestroyFence( g.device, g.fence, NULL );
		if ( g.pool )      vkDestroyCommandPool( g.device, g.pool, NULL );
		vkDestroyDevice( g.device, NULL );
	}
	if ( g.instance ) {
		vkDestroyInstance( g.instance, NULL );
	}
	memset( &g, 0, sizeof( g ) );
}

// Bring the tracer up. Returns NULL on success, else the reason it can't run.
const char *AOGpu_Init( aoGpu_t &g ) {
	VkApplicationInfo app = {};
	app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app.pApplicationName = "DUDE AO baker";
	app.pEngineName = "DUDE";
	app.apiVersion = VK_API_VERSION_1_3;		// the shader is SPIR-V 1.6 (compile_spv.py targets vulkan1.4)
	VkInstanceCreateInfo ici = {};
	ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	ici.pApplicationInfo = &app;
	if ( vkCreateInstance( &ici, NULL, &g.instance ) != VK_SUCCESS ) {
		g.instance = VK_NULL_HANDLE;
		return "no Vulkan instance";
	}

	uint32_t count = 0;
	vkEnumeratePhysicalDevices( g.instance, &count, NULL );
	std::vector<VkPhysicalDevice> devs( count );
	if ( count ) {
		vkEnumeratePhysicalDevices( g.instance, &count, devs.data() );
	}

	// Pick the best device with hardware ray query: discrete over integrated over the rest. CPU
	// implementations (lavapipe -- the only Vulkan device some xvfb setups expose) are skipped: the
	// threaded CPU tracer is the better software path.
	static const char *needExts[] = {
		VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
		VK_KHR_RAY_QUERY_EXTENSION_NAME,
		VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
	};
	const int numNeedExts = sizeof( needExts ) / sizeof( needExts[0] );
	int bestScore = -1;
	for ( uint32_t d = 0; d < count; d++ ) {
		VkPhysicalDeviceProperties props;
		vkGetPhysicalDeviceProperties( devs[d], &props );
		if ( props.apiVersion < VK_API_VERSION_1_3 || props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ) {
			continue;
		}
		uint32_t ne = 0;
		vkEnumerateDeviceExtensionProperties( devs[d], NULL, &ne, NULL );
		std::vector<VkExtensionProperties> exts( ne );
		if ( ne ) {
			vkEnumerateDeviceExtensionProperties( devs[d], NULL, &ne, exts.data() );
		}
		bool ok = true;
		for ( int e = 0; e < numNeedExts && ok; e++ ) {
			ok = AOGpu_HasExtension( exts, needExts[e] );
		}
		if ( !ok ) {
			continue;
		}
		VkPhysicalDeviceRayQueryFeaturesKHR rq = {};
		rq.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
		VkPhysicalDeviceAccelerationStructureFeaturesKHR as = {};
		as.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
		as.pNext = &rq;
		VkPhysicalDeviceVulkan12Features v12 = {};
		v12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
		v12.pNext = &as;
		VkPhysicalDeviceFeatures2 f2 = {};
		f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
		f2.pNext = &v12;
		vkGetPhysicalDeviceFeatures2( devs[d], &f2 );
		if ( !rq.rayQuery || !as.accelerationStructure || !v12.bufferDeviceAddress ) {
			continue;
		}
		uint32_t nq = 0;
		vkGetPhysicalDeviceQueueFamilyProperties( devs[d], &nq, NULL );
		std::vector<VkQueueFamilyProperties> qf( nq );
		if ( nq ) {
			vkGetPhysicalDeviceQueueFamilyProperties( devs[d], &nq, qf.data() );
		}
		int family = -1;
		for ( uint32_t q = 0; q < nq; q++ ) {
			if ( qf[q].queueFlags & VK_QUEUE_COMPUTE_BIT ) {
				family = (int)q;
				break;
			}
		}
		if ( family < 0 ) {
			continue;
		}
		const int score = ( props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ) ? 2
		                : ( props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ) ? 1 : 0;
		if ( score > bestScore ) {
			bestScore = score;
			g.phys = devs[d];
			g.family = (uint32_t)family;
			strncpy( g.name, props.deviceName, sizeof( g.name ) - 1 );
		}
	}
	if ( !g.phys ) {
		return "no GPU with Vulkan 1.3 + ray query";
	}

	vkGetPhysicalDeviceMemoryProperties( g.phys, &g.memProps );
	VkPhysicalDeviceAccelerationStructurePropertiesKHR asProps = {};
	asProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
	VkPhysicalDeviceProperties2 p2 = {};
	p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
	p2.pNext = &asProps;
	vkGetPhysicalDeviceProperties2( g.phys, &p2 );
	g.scratchAlign = asProps.minAccelerationStructureScratchOffsetAlignment ? asProps.minAccelerationStructureScratchOffsetAlignment : 1;

	VkPhysicalDeviceRayQueryFeaturesKHR rq = {};
	rq.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
	rq.rayQuery = VK_TRUE;
	VkPhysicalDeviceAccelerationStructureFeaturesKHR as = {};
	as.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
	as.pNext = &rq;
	as.accelerationStructure = VK_TRUE;
	VkPhysicalDeviceVulkan12Features v12 = {};
	v12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
	v12.pNext = &as;
	v12.bufferDeviceAddress = VK_TRUE;

	const float prio = 1.0f;
	VkDeviceQueueCreateInfo qi = {};
	qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	qi.queueFamilyIndex = g.family;
	qi.queueCount = 1;
	qi.pQueuePriorities = &prio;
	VkDeviceCreateInfo dci = {};
	dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	dci.pNext = &v12;
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qi;
	dci.enabledExtensionCount = numNeedExts;
	dci.ppEnabledExtensionNames = needExts;
	if ( vkCreateDevice( g.phys, &dci, NULL, &g.device ) != VK_SUCCESS ) {
		g.device = VK_NULL_HANDLE;
		return "vkCreateDevice failed";
	}
	vkGetDeviceQueue( g.device, g.family, 0, &g.queue );

	g.createAS   = (PFN_vkCreateAccelerationStructureKHR)vkGetDeviceProcAddr( g.device, "vkCreateAccelerationStructureKHR" );
	g.destroyAS  = (PFN_vkDestroyAccelerationStructureKHR)vkGetDeviceProcAddr( g.device, "vkDestroyAccelerationStructureKHR" );
	g.buildSizes = (PFN_vkGetAccelerationStructureBuildSizesKHR)vkGetDeviceProcAddr( g.device, "vkGetAccelerationStructureBuildSizesKHR" );
	g.cmdBuildAS = (PFN_vkCmdBuildAccelerationStructuresKHR)vkGetDeviceProcAddr( g.device, "vkCmdBuildAccelerationStructuresKHR" );
	g.asAddress  = (PFN_vkGetAccelerationStructureDeviceAddressKHR)vkGetDeviceProcAddr( g.device, "vkGetAccelerationStructureDeviceAddressKHR" );
	if ( !g.createAS || !g.destroyAS || !g.buildSizes || !g.cmdBuildAS || !g.asAddress ) {
		return "acceleration structure entry points missing";
	}

	VkCommandPoolCreateInfo pci = {};
	pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pci.queueFamilyIndex = g.family;
	VkCommandBufferAllocateInfo cai = {};
	cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cai.commandBufferCount = 1;
	VkFenceCreateInfo fci = {};
	fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	if ( vkCreateCommandPool( g.device, &pci, NULL, &g.pool ) != VK_SUCCESS ) {
		g.pool = VK_NULL_HANDLE;
		return "command pool";
	}
	cai.commandPool = g.pool;
	if ( vkAllocateCommandBuffers( g.device, &cai, &g.cmd ) != VK_SUCCESS || vkCreateFence( g.device, &fci, NULL, &g.fence ) != VK_SUCCESS ) {
		g.fence = VK_NULL_HANDLE;
		return "command buffer / fence";
	}

	// the compute pipeline: build-time SPIR-V from the embedded table (works on every build, no shaderc)
	const unsigned char *spv = NULL;
	int spvLen = 0;
	if ( !Dude_GetEmbeddedShader( "spv/aobake.comp.spv", &spv, &spvLen ) || spvLen <= 0 || ( spvLen & 3 ) ) {
		return "aobake.comp.spv not embedded";
	}
	std::vector<uint32_t> code( spvLen / 4 );		// vkCreateShaderModule wants 4-byte aligned words
	memcpy( code.data(), spv, spvLen );
	VkShaderModuleCreateInfo smi = {};
	smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	smi.codeSize = (size_t)spvLen;
	smi.pCode = code.data();
	VkShaderModule module;
	if ( vkCreateShaderModule( g.device, &smi, NULL, &module ) != VK_SUCCESS ) {
		return "shader module";
	}

	VkDescriptorSetLayoutBinding binds[4] = {};
	for ( int i = 0; i < 4; i++ ) {
		binds[i].binding = (uint32_t)i;
		binds[i].descriptorType = ( i == 0 ) ? VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		binds[i].descriptorCount = 1;
		binds[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	}
	VkDescriptorSetLayoutCreateInfo dsl = {};
	dsl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	dsl.bindingCount = 4;
	dsl.pBindings = binds;
	VkPushConstantRange pcr = {};
	pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	pcr.size = sizeof( aoPush_t );
	VkPipelineLayoutCreateInfo pli = {};
	pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pli.setLayoutCount = 1;
	pli.pSetLayouts = &g.setLayout;
	pli.pushConstantRangeCount = 1;
	pli.pPushConstantRanges = &pcr;
	VkComputePipelineCreateInfo cpi = {};
	cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	cpi.stage.module = module;
	cpi.stage.pName = "main";
	bool pipeOk = vkCreateDescriptorSetLayout( g.device, &dsl, NULL, &g.setLayout ) == VK_SUCCESS;
	if ( !pipeOk ) {
		g.setLayout = VK_NULL_HANDLE;
	}
	pipeOk = pipeOk && vkCreatePipelineLayout( g.device, &pli, NULL, &g.layout ) == VK_SUCCESS;
	if ( !pipeOk ) {
		g.layout = VK_NULL_HANDLE;
	} else {
		cpi.layout = g.layout;
		pipeOk = vkCreateComputePipelines( g.device, VK_NULL_HANDLE, 1, &cpi, NULL, &g.pipeline ) == VK_SUCCESS;
	}
	vkDestroyShaderModule( g.device, module, NULL );
	if ( !pipeOk ) {
		g.pipeline = VK_NULL_HANDLE;
		return "compute pipeline";
	}

	VkDescriptorPoolSize sizes[2] = {};
	sizes[0].type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
	sizes[0].descriptorCount = 1;
	sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	sizes[1].descriptorCount = 3;
	VkDescriptorPoolCreateInfo dpi = {};
	dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	dpi.maxSets = 1;
	dpi.poolSizeCount = 2;
	dpi.pPoolSizes = sizes;
	if ( vkCreateDescriptorPool( g.device, &dpi, NULL, &g.descPool ) != VK_SUCCESS ) {
		g.descPool = VK_NULL_HANDLE;
		return "descriptor pool";
	}
	VkDescriptorSetAllocateInfo dsa = {};
	dsa.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	dsa.descriptorPool = g.descPool;
	dsa.descriptorSetCount = 1;
	dsa.pSetLayouts = &g.setLayout;
	if ( vkAllocateDescriptorSets( g.device, &dsa, &g.set ) != VK_SUCCESS ) {
		return "descriptor set";
	}
	return NULL;
}

// Size the build of one acceleration structure, give it storage and create it.
bool AOGpu_CreateAS( aoGpu_t &g, VkAccelerationStructureBuildGeometryInfoKHR &build, uint32_t primitives,
                     aoBuf_t &storage, VkAccelerationStructureKHR &as, VkDeviceSize &scratchSize ) {
	VkAccelerationStructureBuildSizesInfoKHR sz = {};
	sz.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
	g.buildSizes( g.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build, &primitives, &sz );
	if ( !AOGpu_Reserve( g, storage, sz.accelerationStructureSize,
	                     VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false ) ) {
		return false;
	}
	VkAccelerationStructureCreateInfoKHR ci = {};
	ci.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
	ci.buffer = storage.buf;
	ci.size = sz.accelerationStructureSize;
	ci.type = build.type;
	if ( g.createAS( g.device, &ci, NULL, &as ) != VK_SUCCESS ) {
		as = VK_NULL_HANDLE;
		return false;
	}
	build.dstAccelerationStructure = as;
	scratchSize = sz.buildScratchSize;
	return true;
}

void AOGpu_ASBarrier( aoGpu_t &g, VkPipelineStageFlags dstStage, VkAccessFlags dstAccess ) {
	VkMemoryBarrier mb = {};
	mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	mb.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
	mb.dstAccessMask = dstAccess;
	vkCmdPipelineBarrier( g.cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, dstStage, 0, 1, &mb, 0, NULL, 0, NULL );
}

} // anonymous namespace

bool AOGpu_Acquire( void ) {
	aoGpuRefs++;
	if ( aoGpu ) {
		return !aoGpu->broken;
	}
	if ( aoGpuFailed ) {
		return false;
	}
	aoGpu = new aoGpu_t;
	memset( aoGpu, 0, sizeof( *aoGpu ) );
	const char *why = AOGpu_Init( *aoGpu );
	if ( why ) {
		common->Printf( "bakeAO: GPU tracer unavailable (%s) - baking on the CPU\n", why );
		AOGpu_Destroy( *aoGpu );
		delete aoGpu;
		aoGpu = NULL;
		aoGpuFailed = true;
		return false;
	}
	common->Printf( "bakeAO: GPU tracer on %s\n", aoGpu->name );
	return true;
}

void AOGpu_Release( void ) {
	if ( aoGpuRefs <= 0 ) {
		return;
	}
	if ( --aoGpuRefs == 0 ) {
		if ( aoGpu ) {
			AOGpu_Destroy( *aoGpu );
			delete aoGpu;
			aoGpu = NULL;
		}
		aoGpuFailed = false;
	}
}

bool AOGpu_SetOccluders( const float *xyz, int numTris ) {
	if ( !aoGpu || aoGpu->broken ) {
		return false;
	}
	aoGpu_t &g = *aoGpu;
	AOGpu_FreeOccluders( g );
	if ( numTris <= 0 ) {
		return true;		// nothing occludes: Trace reports every sample fully open
	}

	const VkBufferUsageFlags inputUsage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR
	                                    | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
	const VkDeviceSize vertBytes = (VkDeviceSize)numTris * 9 * sizeof( float );
	if ( !AOGpu_Reserve( g, g.verts, vertBytes, inputUsage, true ) ) {
		return false;
	}
	memcpy( g.verts.map, xyz, (size_t)vertBytes );

	// bottom level: the triangle soup, non-indexed, opaque
	VkAccelerationStructureGeometryKHR geo = {};
	geo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
	geo.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
	geo.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
	geo.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
	geo.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
	geo.geometry.triangles.vertexData.deviceAddress = g.verts.addr;
	geo.geometry.triangles.vertexStride = 3 * sizeof( float );
	geo.geometry.triangles.maxVertex = (uint32_t)( numTris * 3 - 1 );
	geo.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
	VkAccelerationStructureBuildGeometryInfoKHR blasBuild = {};
	blasBuild.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
	blasBuild.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
	blasBuild.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
	blasBuild.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	blasBuild.geometryCount = 1;
	blasBuild.pGeometries = &geo;
	VkDeviceSize blasScratch = 0;
	if ( !AOGpu_CreateAS( g, blasBuild, (uint32_t)numTris, g.blasMem, g.blas, blasScratch ) ) {
		return false;
	}

	// top level: one identity instance. Facing culling stays ENABLED (no FACING_CULL_DISABLE) --
	// aobake.comp relies on it for the front-face-only occluder rule.
	VkAccelerationStructureDeviceAddressInfoKHR ai = {};
	ai.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
	ai.accelerationStructure = g.blas;
	VkAccelerationStructureInstanceKHR instance = {};
	instance.transform.matrix[0][0] = instance.transform.matrix[1][1] = instance.transform.matrix[2][2] = 1.0f;
	instance.mask = 0xFF;
	instance.accelerationStructureReference = g.asAddress( g.device, &ai );
	if ( !AOGpu_Reserve( g, g.inst, sizeof( instance ), inputUsage, true ) ) {
		return false;
	}
	memcpy( g.inst.map, &instance, sizeof( instance ) );

	VkAccelerationStructureGeometryKHR igeo = {};
	igeo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
	igeo.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
	igeo.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
	igeo.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
	igeo.geometry.instances.data.deviceAddress = g.inst.addr;
	VkAccelerationStructureBuildGeometryInfoKHR tlasBuild = {};
	tlasBuild.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
	tlasBuild.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
	tlasBuild.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
	tlasBuild.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	tlasBuild.geometryCount = 1;
	tlasBuild.pGeometries = &igeo;
	VkDeviceSize tlasScratch = 0;
	if ( !AOGpu_CreateAS( g, tlasBuild, 1, g.tlasMem, g.tlas, tlasScratch ) ) {
		return false;
	}

	// one scratch for both builds (they run back to back, separated by a barrier)
	const VkDeviceSize scratchNeed = ( blasScratch > tlasScratch ? blasScratch : tlasScratch ) + g.scratchAlign;
	if ( !AOGpu_Reserve( g, g.scratch, scratchNeed, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false ) ) {
		return false;
	}
	const VkDeviceAddress scratchAddr = ( g.scratch.addr + g.scratchAlign - 1 ) & ~( g.scratchAlign - 1 );
	blasBuild.scratchData.deviceAddress = scratchAddr;
	tlasBuild.scratchData.deviceAddress = scratchAddr;

	VkAccelerationStructureBuildRangeInfoKHR blasRange = {};
	blasRange.primitiveCount = (uint32_t)numTris;
	VkAccelerationStructureBuildRangeInfoKHR tlasRange = {};
	tlasRange.primitiveCount = 1;
	const VkAccelerationStructureBuildRangeInfoKHR *pBlasRange = &blasRange;
	const VkAccelerationStructureBuildRangeInfoKHR *pTlasRange = &tlasRange;

	if ( !AOGpu_BeginCommands( g ) ) {
		g.broken = true;
		return false;
	}
	g.cmdBuildAS( g.cmd, 1, &blasBuild, &pBlasRange );
	// the TLAS build reads the BLAS and reuses its scratch
	AOGpu_ASBarrier( g, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
	                 VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR );
	g.cmdBuildAS( g.cmd, 1, &tlasBuild, &pTlasRange );
	// every later trace dispatch reads the TLAS (a barrier covers commands later in submission order)
	AOGpu_ASBarrier( g, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR );
	if ( !AOGpu_SubmitAndWait( g, "acceleration structure build" ) ) {
		return false;
	}
	g.numTris = numTris;
	return true;
}

bool AOGpu_SetHemisphere( const float *xyz, int rays ) {
	if ( !aoGpu || aoGpu->broken || rays <= 0 ) {
		return false;
	}
	aoGpu_t &g = *aoGpu;
	if ( !AOGpu_Reserve( g, g.hemi, (VkDeviceSize)rays * 4 * sizeof( float ), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true ) ) {
		return false;
	}
	float *dst = (float *)g.hemi.map;
	for ( int i = 0; i < rays; i++ ) {
		dst[i * 4 + 0] = xyz[i * 3 + 0];
		dst[i * 4 + 1] = xyz[i * 3 + 1];
		dst[i * 4 + 2] = xyz[i * 3 + 2];
		dst[i * 4 + 3] = 0.0f;
	}
	g.rays = rays;
	return true;
}

bool AOGpu_Trace( const aoGpuSample_t *samples, int count, float maxDist, float bias, float contact,
                  float *outVis, unsigned char *outBuried ) {
	if ( !aoGpu || aoGpu->broken || aoGpu->rays <= 0 ) {
		return false;
	}
	aoGpu_t &g = *aoGpu;
	if ( count <= 0 ) {
		return true;
	}
	if ( g.numTris == 0 ) {
		// no occluders at all (every surface translucent / eyewear): fully open, exactly what the
		// CPU tracer returns from an empty grid
		for ( int i = 0; i < count; i++ ) {
			outVis[i] = 1.0f;
			outBuried[i] = 0;
		}
		return true;
	}

	int chunk = AO_GPU_RAYS_PER_DISPATCH / g.rays;
	if ( chunk < 4096 ) chunk = 4096;
	if ( chunk > ( 1 << 20 ) ) chunk = 1 << 20;
	if ( chunk > count ) chunk = count;
	if ( !AOGpu_Reserve( g, g.samples, (VkDeviceSize)chunk * sizeof( aoGpuSample_t ), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true )
	  || !AOGpu_Reserve( g, g.results, (VkDeviceSize)chunk * 2 * sizeof( float ), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true ) ) {
		return false;
	}

	// point the set at this trace's buffers (nothing is in flight: every submission is waited on)
	VkWriteDescriptorSetAccelerationStructureKHR asw = {};
	asw.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
	asw.accelerationStructureCount = 1;
	asw.pAccelerationStructures = &g.tlas;
	VkDescriptorBufferInfo bufInfo[3] = {};
	bufInfo[0].buffer = g.samples.buf;
	bufInfo[0].range = VK_WHOLE_SIZE;
	bufInfo[1].buffer = g.hemi.buf;
	bufInfo[1].range = VK_WHOLE_SIZE;
	bufInfo[2].buffer = g.results.buf;
	bufInfo[2].range = VK_WHOLE_SIZE;
	VkWriteDescriptorSet writes[4] = {};
	for ( int i = 0; i < 4; i++ ) {
		writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstSet = g.set;
		writes[i].dstBinding = (uint32_t)i;
		writes[i].descriptorCount = 1;
		if ( i == 0 ) {
			writes[i].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
			writes[i].pNext = &asw;
		} else {
			writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			writes[i].pBufferInfo = &bufInfo[i - 1];
		}
	}
	vkUpdateDescriptorSets( g.device, 4, writes, 0, NULL );

	for ( int first = 0; first < count; first += chunk ) {
		const int n = ( count - first < chunk ) ? count - first : chunk;
		memcpy( g.samples.map, samples + first, (size_t)n * sizeof( aoGpuSample_t ) );

		aoPush_t pc;
		pc.count = (uint32_t)n;
		pc.rays = (uint32_t)g.rays;
		pc.maxDist = maxDist;
		pc.bias = bias;
		pc.contact = contact;

		if ( !AOGpu_BeginCommands( g ) ) {
			g.broken = true;
			return false;
		}
		vkCmdBindPipeline( g.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g.pipeline );
		vkCmdBindDescriptorSets( g.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g.layout, 0, 1, &g.set, 0, NULL );
		vkCmdPushConstants( g.cmd, g.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof( pc ), &pc );
		vkCmdDispatch( g.cmd, (uint32_t)( ( n + 63 ) / 64 ), 1, 1 );
		VkMemoryBarrier mb = {};
		mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
		vkCmdPipelineBarrier( g.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL );
		if ( !AOGpu_SubmitAndWait( g, "trace" ) ) {
			return false;
		}

		const float *res = (const float *)g.results.map;
		for ( int i = 0; i < n; i++ ) {
			outVis[first + i] = res[i * 2 + 0];
			outBuried[first + i] = ( res[i * 2 + 1] > 0.5f ) ? 1 : 0;
		}
	}
	return true;
}
