/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.h for license header)
===========================================================================
*/

// DUDE Vulkan backend — Phase 4 (docs/vulkan-backend.md).
//
// M0: scaffolding only. This translation unit proves the build wiring
// (Vulkan headers on the include path, C++17, the rhi/vk/ source group) and
// reserves the GetVulkanRHI() seam; the backend itself arrives at M1
// (instance/device/swapchain/clear). Compiled only with DHEWM3_VULKAN=ON —
// the CMake source list guards it, the #ifdef is belt-and-braces.

#ifdef DHEWM3_VULKAN

#include <vulkan/vulkan.h>

#include "renderer/rhi/RHI.h"

namespace rhi {

// M1: returns the VulkanBackend singleton once it exists. Until then callers
// (only rhi::GetRHI()'s defensive branch — R_InitOpenGL never selects
// BT_VULKAN in M0) get NULL and must fall back.
RHI *GetVulkanRHI() {
	return nullptr;
}

// header sanity hook for the M0 probe/logs: proves we compiled against real
// Vulkan headers and which revision
unsigned int GetVulkanHeaderVersion() {
	return VK_HEADER_VERSION;
}

} // namespace rhi

#endif // DHEWM3_VULKAN
