// Offline shader cost probe: compiles SPIR-V pipelines with Mesa RADV (the Ryzen iGPU's driver - nothing is submitted, the
// NVIDIA card is never touched) and prints VK_KHR_pipeline_executable_properties statistics (VGPRs, instructions, ACO latency).
// Usage: vkstats <vert.spv> <frag.spv> [isa-out.txt]     (graphics)
//        vkstats <comp.spv> [isa-out.txt]                (compute; the file name must contain ".comp")
// Run with: VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.json
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s failed: %d (line %d)\n", #x, r_, __LINE__); exit(1); } } while (0)

typedef struct { uint32_t *w; size_t n; } Spv;
static Spv readSpv(const char *p) {
	FILE *f = fopen(p, "rb"); if (!f) { perror(p); exit(1); }
	fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
	Spv s; s.w = malloc(sz); s.n = sz / 4; if (fread(s.w, 1, sz, f) != (size_t)sz) { exit(1); } fclose(f); return s;
}

#define MAXID 400000
typedef struct { int set, binding, location, builtin, isBlock; } Deco;
typedef struct { uint32_t op; uint32_t a, b, c; } TypeInfo;
static Deco deco[MAXID];
static TypeInfo ty[MAXID];
static uint32_t constVal[MAXID];
typedef struct { int set, binding; VkDescriptorType type; uint32_t count; int runtime; } Binding;
static Binding binds[256]; static int nBinds = 0;
static int hasPush = 0;
static int vertInputs[32]; static int nVertInputs = 0;
static int maxFragOut = -1;

static void addBinding(int set, int binding, VkDescriptorType t, uint32_t count, int runtime) {
	for (int i = 0; i < nBinds; i++) if (binds[i].set == set && binds[i].binding == binding) return;
	binds[nBinds++] = (Binding){ set, binding, t, count, runtime };
}

static void reflect(Spv s, int stageIsVert) {
	for (int i = 0; i < MAXID; i++) { deco[i].set = deco[i].binding = deco[i].location = -1; deco[i].builtin = 0; deco[i].isBlock = 0; }
	memset(ty, 0, sizeof(ty));
	for (size_t i = 5; i < s.n; ) {
		uint32_t wc = s.w[i] >> 16, op = s.w[i] & 0xffff;
		uint32_t *o = &s.w[i + 1];
		switch (op) {
		case 71: if (o[1] == 34) deco[o[0]].set = o[2]; else if (o[1] == 33) deco[o[0]].binding = o[2]; else if (o[1] == 30) deco[o[0]].location = o[2]; else if (o[1] == 11) deco[o[0]].builtin = 1; else if (o[1] == 2 || o[1] == 3) deco[o[0]].isBlock = 1; break;
		case 72: if (o[2] == 11) deco[o[0]].builtin = 1; break;
		case 43: constVal[o[1]] = o[2]; break;
		case 32: ty[o[0]] = (TypeInfo){ op, o[1], o[2], 0 }; break;
		case 28: ty[o[0]] = (TypeInfo){ op, o[1], o[2], 0 }; break;
		case 29: ty[o[0]] = (TypeInfo){ op, o[1], 0, 0 }; break;
		case 27: ty[o[0]] = (TypeInfo){ op, o[1], 0, 0 }; break;
		case 25: ty[o[0]] = (TypeInfo){ op, o[1], o[2], o[6] }; break;
		case 26: ty[o[0]] = (TypeInfo){ op, 0, 0, 0 }; break;
		case 30: ty[o[0]] = (TypeInfo){ op, 0, 0, 0 }; break;
		case 5341: ty[o[0]] = (TypeInfo){ op, 0, 0, 0 }; break;
		case 59: {
			uint32_t ptr = o[0], id = o[1], sc = o[2];
			uint32_t t = ty[ptr].b;
			if (sc == 9) { hasPush = 1; break; }
			if (sc == 1 && stageIsVert && !deco[id].builtin && deco[id].location != -1 && !deco[t].builtin) { vertInputs[nVertInputs++] = deco[id].location; break; }
			if (sc == 3 && !stageIsVert && deco[id].location != -1) { if (deco[id].location > maxFragOut) maxFragOut = deco[id].location; break; }
			if (sc != 0 && sc != 2 && sc != 12) break;
			if (deco[id].set == -1) break;
			uint32_t count = 1; int runtime = 0;
			if (ty[t].op == 28) { count = constVal[ty[t].b]; t = ty[t].a; }
			else if (ty[t].op == 29) { runtime = 1; count = 4096; t = ty[t].a; }
			VkDescriptorType dt;
			if (sc == 12) dt = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			else if (sc == 2) dt = deco[t].isBlock ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			else if (ty[t].op == 27) dt = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			else if (ty[t].op == 26) dt = VK_DESCRIPTOR_TYPE_SAMPLER;
			else if (ty[t].op == 5341) dt = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
			else if (ty[t].op == 25) dt = (ty[t].c == 2) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
			else break;
			addBinding(deco[id].set, deco[id].binding, dt, count, runtime);
			break;
		}
		}
		i += wc ? wc : 1;
	}
}

static void *chainAlloc(VkStructureType st, size_t sz, void *next) { void *p = calloc(1, sz); ((VkBaseOutStructure *)p)->sType = st; ((VkBaseOutStructure *)p)->pNext = next; return p; }

int main(int argc, char **argv) {
	if (argc < 2) { fprintf(stderr, "usage\n"); return 1; }
	int compute = strstr(argv[1], ".comp") != NULL;
	Spv vs = { 0 }, fs = { 0 }, cs = { 0 };
	const char *isaOut = NULL;
	if (compute) { cs = readSpv(argv[1]); reflect(cs, 0); if (argc > 2) isaOut = argv[2]; }
	else { vs = readSpv(argv[1]); fs = readSpv(argv[2]); reflect(vs, 1); reflect(fs, 0); if (argc > 3) isaOut = argv[3]; }
	VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "vkstats", 1, "none", 1, VK_API_VERSION_1_3 };
	VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &app };
	VkInstance inst; CHECK(vkCreateInstance(&ici, 0, &inst));
	uint32_t nPd = 8; VkPhysicalDevice pds[8]; CHECK(vkEnumeratePhysicalDevices(inst, &nPd, pds));
	VkPhysicalDevice pd = VK_NULL_HANDLE;
	for (uint32_t i = 0; i < nPd; i++) { VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pds[i], &p); if (strstr(p.deviceName, "RADV")) { pd = pds[i]; fprintf(stderr, "device: %s\n", p.deviceName); } }
	if (!pd) { fprintf(stderr, "no RADV device\n"); return 1; }
	void *chain = NULL;
	chain = chainAlloc(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR, sizeof(VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR), chain);
	chain = chainAlloc(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR, sizeof(VkPhysicalDeviceRayQueryFeaturesKHR), chain);
	chain = chainAlloc(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, sizeof(VkPhysicalDeviceAccelerationStructureFeaturesKHR), chain);
	chain = chainAlloc(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, sizeof(VkPhysicalDeviceVulkan13Features), chain);
	chain = chainAlloc(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, sizeof(VkPhysicalDeviceVulkan12Features), chain);
	chain = chainAlloc(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, sizeof(VkPhysicalDeviceVulkan11Features), chain);
	VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, chain };
	vkGetPhysicalDeviceFeatures2(pd, &f2);
	for (VkBaseOutStructure *p = chain; p; p = p->pNext) if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR) { VkPhysicalDeviceAccelerationStructureFeaturesKHR *a = (void *)p; a->accelerationStructureCaptureReplay = 0; a->accelerationStructureIndirectBuild = 0; a->accelerationStructureHostCommands = 0; }
	const char *exts[] = { "VK_KHR_ray_query", "VK_KHR_acceleration_structure", "VK_KHR_deferred_host_operations", "VK_KHR_pipeline_executable_properties" };
	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, 0, 1, &prio };
	VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2, 0, 1, &qci, 0, 0, 4, exts, 0 };
	VkDevice dev; CHECK(vkCreateDevice(pd, &dci, 0, &dev));
	int maxSet = -1; for (int i = 0; i < nBinds; i++) if (binds[i].set > maxSet) maxSet = binds[i].set;
	VkDescriptorSetLayout setLayouts[8];
	for (int s = 0; s <= maxSet; s++) {
		VkDescriptorSetLayoutBinding lb[64]; VkDescriptorBindingFlags bf[64]; int n = 0, anyRuntime = 0;
		for (int i = 0; i < nBinds; i++) if (binds[i].set == s) {
			lb[n] = (VkDescriptorSetLayoutBinding){ binds[i].binding, binds[i].type, binds[i].count, VK_SHADER_STAGE_ALL, 0 };
			bf[n] = binds[i].runtime ? ( VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT ) : 0;
			anyRuntime |= binds[i].runtime; n++;
		}
		VkDescriptorSetLayoutBindingFlagsCreateInfo bfi = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, 0, n, bf };
		VkDescriptorSetLayoutCreateInfo sci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, anyRuntime ? &bfi : 0, 0, n, lb };
		CHECK(vkCreateDescriptorSetLayout(dev, &sci, 0, &setLayouts[s]));
	}
	VkPushConstantRange pcr = { VK_SHADER_STAGE_ALL, 0, 128 };
	VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, 0, maxSet + 1, setLayouts, hasPush ? 1 : 0, &pcr };
	VkPipelineLayout pl; CHECK(vkCreatePipelineLayout(dev, &plci, 0, &pl));
	VkPipeline pipe;
	VkPipelineCreateFlags flags = VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR | VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR;
	if (compute) {
		VkShaderModuleCreateInfo smci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, cs.n * 4, cs.w };
		VkShaderModule m; CHECK(vkCreateShaderModule(dev, &smci, 0, &m));
		VkComputePipelineCreateInfo cpci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, 0, flags, { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main", 0 }, pl, 0, 0 };
		CHECK(vkCreateComputePipelines(dev, 0, 1, &cpci, 0, &pipe));
	} else {
		VkShaderModuleCreateInfo vci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, vs.n * 4, vs.w };
		VkShaderModuleCreateInfo fci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, fs.n * 4, fs.w };
		VkShaderModule vm, fm; CHECK(vkCreateShaderModule(dev, &vci, 0, &vm)); CHECK(vkCreateShaderModule(dev, &fci, 0, &fm));
		VkPipelineShaderStageCreateInfo st[2] = {
			{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_VERTEX_BIT, vm, "main", 0 },
			{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fm, "main", 0 } };
		VkVertexInputBindingDescription vb = { 0, 256, VK_VERTEX_INPUT_RATE_VERTEX };
		VkVertexInputAttributeDescription va[32];
		for (int i = 0; i < nVertInputs; i++) va[i] = (VkVertexInputAttributeDescription){ vertInputs[i], 0, VK_FORMAT_R32G32B32A32_SFLOAT, 16 * vertInputs[i] };
		VkPipelineVertexInputStateCreateInfo vis = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, 0, 0, nVertInputs ? 1 : 0, &vb, nVertInputs, va };
		VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, 0, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0 };
		VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, 0, 0, 1, 0, 1, 0 };
		VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, 0, 0, 0, 0, VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, 0, 0, 0, 0, 1.0f };
		VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, 0, 0, VK_SAMPLE_COUNT_1_BIT };
		int nOut = maxFragOut + 1; if (nOut < 1) nOut = 1;
		VkPipelineColorBlendAttachmentState cba[8]; VkFormat fmts[8];
		for (int i = 0; i < nOut; i++) { memset(&cba[i], 0, sizeof(cba[i])); cba[i].colorWriteMask = 0xF; fmts[i] = VK_FORMAT_R16G16B16A16_SFLOAT; }
		if (nOut > 1) fmts[1] = VK_FORMAT_R32G32_SFLOAT;
		if (nOut > 3) fmts[3] = VK_FORMAT_R32G32B32A32_SFLOAT;
		VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, 0, 0, 0, 0, nOut, cba };
		VkDynamicState ds[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
		VkPipelineDynamicStateCreateInfo dyn = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, 0, 0, 2, ds };
		VkPipelineRenderingCreateInfo pri = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, 0, 0, nOut, fmts, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED };
		VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &pri, flags, 2, st, &vis, &ia, 0, &vp, &rs, &ms, 0, &cb, &dyn, pl, 0, 0, 0, -1 };
		CHECK(vkCreateGraphicsPipelines(dev, 0, 1, &gp, 0, &pipe));
	}
	PFN_vkGetPipelineExecutablePropertiesKHR getProps = (void *)vkGetDeviceProcAddr(dev, "vkGetPipelineExecutablePropertiesKHR");
	PFN_vkGetPipelineExecutableStatisticsKHR getStats = (void *)vkGetDeviceProcAddr(dev, "vkGetPipelineExecutableStatisticsKHR");
	PFN_vkGetPipelineExecutableInternalRepresentationsKHR getIR = (void *)vkGetDeviceProcAddr(dev, "vkGetPipelineExecutableInternalRepresentationsKHR");
	VkPipelineInfoKHR pinfo = { VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR, 0, pipe };
	uint32_t nExe = 0; CHECK(getProps(dev, &pinfo, &nExe, 0));
	VkPipelineExecutablePropertiesKHR props[8]; for (uint32_t i = 0; i < nExe; i++) props[i] = (VkPipelineExecutablePropertiesKHR){ VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR };
	CHECK(getProps(dev, &pinfo, &nExe, props));
	for (uint32_t e = 0; e < nExe; e++) {
		if (!compute && !(props[e].stages & VK_SHADER_STAGE_FRAGMENT_BIT)) continue;
		printf("[%s]\n", props[e].name);
		VkPipelineExecutableInfoKHR ei = { VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR, 0, pipe, e };
		uint32_t nS = 0; CHECK(getStats(dev, &ei, &nS, 0));
		VkPipelineExecutableStatisticKHR *ss = calloc(nS, sizeof(*ss)); for (uint32_t i = 0; i < nS; i++) ss[i].sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR;
		CHECK(getStats(dev, &ei, &nS, ss));
		for (uint32_t i = 0; i < nS; i++) {
			if (ss[i].format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR) printf("  %-24s %llu\n", ss[i].name, (unsigned long long)ss[i].value.u64);
			else if (ss[i].format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR) printf("  %-24s %lld\n", ss[i].name, (long long)ss[i].value.i64);
			else if (ss[i].format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR) printf("  %-24s %f\n", ss[i].name, ss[i].value.f64);
		}
		if (isaOut) {
			uint32_t nIR = 0; CHECK(getIR(dev, &ei, &nIR, 0));
			VkPipelineExecutableInternalRepresentationKHR *ir = calloc(nIR, sizeof(*ir));
			for (uint32_t i = 0; i < nIR; i++) ir[i].sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INTERNAL_REPRESENTATION_KHR;
			CHECK(getIR(dev, &ei, &nIR, ir));
			for (uint32_t i = 0; i < nIR; i++) ir[i].pData = malloc(ir[i].dataSize);
			CHECK(getIR(dev, &ei, &nIR, ir));
			FILE *f = fopen(isaOut, "w");
			for (uint32_t i = 0; i < nIR; i++) if (strstr(ir[i].name, "Assembly")) { fprintf(f, "== %s\n", ir[i].name); fwrite(ir[i].pData, 1, strnlen(ir[i].pData, ir[i].dataSize), f); }
			fclose(f);
		}
	}
	return 0;
}
