// DUDE shaderstats: a tiny compute runner for offline shader EQUIVALENCE tests (docs: tools/shaderstats/README.md).
// RADV only (VK_ICD_FILENAMES=radeon_icd.json): it runs on the CPU's integrated GPU, never on the NVIDIA card, never the game.
//
// vkrun <comp.spv> <data.bin> <out.bin> <nPatch> <nTest> [pushconst.bin <rebase word list>]
// One host-visible buffer with a device address holds data.bin; push constants = { uvec2 base, uint n, uint mode }. Dispatch
// mode 0 over nPatch invocations (a harness may patch addresses into its data first), a barrier, then mode 1 over nTest; the
// buffer is written back to out.bin. With pushconst.bin the mode-1 dispatch takes those raw bytes instead, and the comma list
// names word indices of (lo, hi) pairs that get the buffer's device address added (offsets -> addresses).
// VKRUN_REPEAT=n repeats the mode-1 dispatch n times between two timestamps and prints the time per dispatch.
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s failed: %d (line %d)\n", #x, r_, __LINE__); exit(1); } } while (0)
static void *readAll(const char *p, size_t *n) { FILE *f = fopen(p, "rb"); if (!f) { perror(p); exit(1); } fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET); void *b = malloc(*n); if (fread(b, 1, *n, f) != *n) exit(1); fclose(f); return b; }
int main(int argc, char **argv) {
	if (argc < 6) { fprintf(stderr, "usage: vkrun <comp.spv> <data.bin> <out.bin> <nPatch> <nTest> [pushconst.bin <rebase words>]\n"); return 1; }
	size_t spvN, dataN; void *spv = readAll(argv[1], &spvN), *data = readAll(argv[2], &dataN);
	unsigned nPatch = atoi(argv[4]), nTest = atoi(argv[5]);
	VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "vkrun", 1, "none", 1, VK_API_VERSION_1_3 };
	VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &app };
	VkInstance inst; CHECK(vkCreateInstance(&ici, 0, &inst));
	uint32_t nPd = 8; VkPhysicalDevice pds[8]; CHECK(vkEnumeratePhysicalDevices(inst, &nPd, pds));
	VkPhysicalDevice pd = 0; for (uint32_t i = 0; i < nPd; i++) { VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pds[i], &p); if (strstr(p.deviceName, "RADV")) pd = pds[i]; }
	if (!pd) { fprintf(stderr, "no RADV device (set VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.json)\n"); return 1; }
	VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES }; f12.bufferDeviceAddress = 1;
	VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12 };
	float prio = 1; uint32_t qf = 0;
	{ uint32_t nq = 16; VkQueueFamilyProperties qp[16]; vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp); for (uint32_t i = 0; i < nq; i++) if (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { qf = i; break; } }
	VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, qf, 1, &prio };
	VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2, 0, 1, &qci };
	VkDevice dev; CHECK(vkCreateDevice(pd, &dci, 0, &dev));
	VkQueue q; vkGetDeviceQueue(dev, qf, 0, &q);
	VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, dataN, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT };
	VkBuffer buf; CHECK(vkCreateBuffer(dev, &bci, 0, &buf));
	VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, buf, &mr);
	VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	uint32_t mt = ~0u; for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & 6) == 6) { mt = i; break; }
	VkMemoryAllocateFlagsInfo mafi = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, 0, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT };
	VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &mafi, mr.size, mt };
	VkDeviceMemory mem; CHECK(vkAllocateMemory(dev, &mai, 0, &mem)); CHECK(vkBindBufferMemory(dev, buf, mem, 0));
	void *map; CHECK(vkMapMemory(dev, mem, 0, dataN, 0, &map)); memcpy(map, data, dataN);
	VkBufferDeviceAddressInfo bai = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, 0, buf };
	VkDeviceAddress addr = vkGetBufferDeviceAddress(dev, &bai);
	unsigned pcw[32] = { 0 }; size_t pcN = 16;
	if (argc > 6) { size_t n; void *b = readAll(argv[6], &n); if (n > sizeof(pcw)) n = sizeof(pcw); memcpy(pcw, b, n); pcN = n; }
	VkShaderModuleCreateInfo smci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, spvN, spv };
	VkShaderModule sm; CHECK(vkCreateShaderModule(dev, &smci, 0, &sm));
	VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, (uint32_t)((pcN + 3) & ~3u) };
	VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, 0, 0, 0, 1, &pcr };
	VkPipelineLayout pl; CHECK(vkCreatePipelineLayout(dev, &plci, 0, &pl));
	VkComputePipelineCreateInfo cpci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, 0, 0, { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main", 0 }, pl };
	VkPipeline pipe; CHECK(vkCreateComputePipelines(dev, 0, 1, &cpci, 0, &pipe));
	VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, 0, qf };
	VkCommandPool pool; CHECK(vkCreateCommandPool(dev, &cpi, 0, &pool));
	VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
	VkCommandBuffer cb; CHECK(vkAllocateCommandBuffers(dev, &cbai, &cb));
	VkQueryPoolCreateInfo qpci = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, 0, 0, VK_QUERY_TYPE_TIMESTAMP, 2 };
	VkQueryPool qpool; CHECK(vkCreateQueryPool(dev, &qpci, 0, &qpool));
	VkCommandBufferBeginInfo cbbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
	CHECK(vkBeginCommandBuffer(cb, &cbbi));
	vkCmdResetQueryPool(cb, qpool, 0, 2);
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
	VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT };
	unsigned pcv[4] = { (unsigned)(addr & 0xffffffffu), (unsigned)(addr >> 32), nPatch, 0 };
	if (nPatch) {
		vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, pcv); vkCmdDispatch(cb, (nPatch + 63) / 64, 1, 1);
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, 0, 0, 0);
	}
	pcv[2] = nTest; pcv[3] = 1;
	if (argc > 7) {
		for (char *s = argv[7]; *s; ) { int w = atoi(s); unsigned long long v = ((unsigned long long)pcw[w + 1] << 32 | pcw[w]) + addr; pcw[w] = (unsigned)v; pcw[w + 1] = (unsigned)(v >> 32); while (*s && *s != ',') s++; if (*s) s++; }
	}
	int repeat = getenv("VKRUN_REPEAT") ? atoi(getenv("VKRUN_REPEAT")) : 1;
	vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qpool, 0);
	for (int rep = 0; rep < repeat; rep++) {
		if (argc > 6) vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, (uint32_t)((pcN + 3) & ~3u), pcw);
		else vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, pcv);
		vkCmdDispatch(cb, (nTest + 63) / 64, 1, 1);
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, 0, 0, 0);
	}
	vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qpool, 1);
	VkMemoryBarrier hb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT };
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, 0, 0, 0);
	CHECK(vkEndCommandBuffer(cb));
	VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 1, &cb };
	CHECK(vkQueueSubmit(q, 1, &si, 0)); CHECK(vkQueueWaitIdle(q));
	{
		unsigned long long ts[2]; VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(pd, &pp);
		if (vkGetQueryPoolResults(dev, qpool, 0, 2, sizeof(ts), ts, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS)
			fprintf(stderr, "dispatch: %.3f ms each (%d repeats)\n", (double)(ts[1] - ts[0]) * pp.limits.timestampPeriod * 1e-6 / repeat, repeat);
	}
	FILE *f = fopen(argv[3], "wb"); fwrite(map, 1, dataN, f); fclose(f);
	return 0;
}
