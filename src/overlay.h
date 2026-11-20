#pragma once
/**
 * overlay.h  —  In-game ImGui overlay interface (Vulkan + X11/Wayland)
 */

#include <vulkan/vulkan.h>
#include <cstdint>
#include "capture.h"

struct OverlaySurfaceInfo
{
    void* x11Display = nullptr;  // Display*
    unsigned long x11Window = 0; // Window
};

void Overlay_SetSurfaceInfo(const OverlaySurfaceInfo& info);

void Overlay_Init(const VkCaptureDeviceInfo& devInfo,
                  VkSwapchainKHR swapchain,
                  uint32_t width,
                  uint32_t height,
                  VkFormat format,
                  uint32_t minImageCount,
                  uint32_t imageCount,
                  const VkImage* images);

// Renders ImGui onto the swapchain image and returns a semaphore that
// vkQueuePresentKHR should wait on (or VK_NULL_HANDLE if overlay didn't render).
VkSemaphore Overlay_OnPresent(const VkCaptureDeviceInfo& devInfo,
                              VkQueue presentQueue,
                              uint32_t imageIndex,
                              uint32_t waitSemaphoreCount,
                              const VkSemaphore* waitSemaphores);

void Overlay_OnPreReset(const VkCaptureDeviceInfo* devInfo);
void Overlay_Shutdown(const VkCaptureDeviceInfo* devInfo);

bool Overlay_IsMenuOpen();
void Overlay_ToggleMenu();
void Overlay_ToggleRecord();
void Overlay_AddMouseWheel(float wheelX, float wheelY);
void Overlay_OnKeyEvent(int keyCode, bool pressed);
void Overlay_OnMouseMove(float x, float y);
void Overlay_OnMouseButton(int button, bool pressed);
void Overlay_SetWaylandActive();
void Overlay_AddMouseDelta(float dx, float dy);
