/*
===========================================================================
Doom 3 GPL Source Code (see ArbProgram.h for license header)
===========================================================================
*/

#ifndef __VULKANIMGUI_H__
#define __VULKANIMGUI_H__

// DUDE Phase 4 M6 — ImGui on the Vulkan backend (imgui_impl_vulkan glue).
//
// sys_imgui.cpp owns the ImGui context and the SDL platform layer; the
// renderer side lives inside VulkanBackend.cpp because it needs the live
// instance/device/swapchain. Draw data is handed over per frame and rendered
// into the swapchain image between the scene blit and present, so ImGui
// stays out of screenshots (which read the scene image), matching GL.
//
// All entry points are safe to call when the backend is down (no-ops).

namespace rhi {

// ImGui_ImplVulkan_Init against the live backend; false if the backend is
// not up. Call after ImGui_ImplSDLx_InitForVulkan.
bool VK_ImGuiInit();

// ImGui_ImplVulkan_Shutdown (waits the device idle first); no-op if not up.
void VK_ImGuiShutdown();

// ImGui_ImplVulkan_NewFrame.
void VK_ImGuiNewFrame();

// hand the frame's ImDrawData* over; the backend renders it in EndFrame.
// Pointer must stay valid until the frame is submitted (it is: ImGui keeps
// it until the next NewFrame).
void VK_ImGuiSetDrawData( void *imDrawData );

} // namespace rhi

#endif /* !__VULKANIMGUI_H__ */
