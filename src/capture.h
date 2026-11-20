#pragma once
/**
 * capture.h  —  Vulkan GPU frame capture interface (Linux port)
 *
 * Ownership model
 * ───────────────
 * Capture_Init / Capture_Shutdown are defined ONCE in consumer_backend.cpp.
 * They own top-level setup/teardown, and must call Capture_ReleaseSurfaces()
 * during shutdown to free the Vulkan staging buffers/images owned by capture.cpp.
 *
 * Lifecycle (called from layer_main.cpp):
 *   Capture_Init()              – consumer one-time setup (consumer_backend.cpp)
 *   Capture_OnSwapchainCreate() – tracks swapchain images, format, extent, MSAA
 *   Capture_OnPresent()         – called every frame inside hooked vkQueuePresentKHR
 *   Capture_OnPreReset()        – waits for in-flight fences & frees staging resources
 *   Capture_Shutdown()          – consumer teardown; must call Capture_ReleaseSurfaces()
 */

#include <vulkan/vulkan.h>
#include <cstdint>
#include <cstddef>
#include "platform.h"

// ── logging helper (defined in layer_main.cpp) ───────────────────────────────
void Log(const char* fmt, ...);

// ── Vulkan device/dispatch context passed to capture & overlay ───────────────
struct VkCaptureDeviceInfo
{
    VkInstance               instance = VK_NULL_HANDLE;
    VkPhysicalDevice         physicalDevice = VK_NULL_HANDLE;
    VkDevice                 device = VK_NULL_HANDLE;
    VkQueue                  graphicsQueue = VK_NULL_HANDLE;
    uint32_t                 graphicsQueueFamily = 0;

    // Device dispatch pointers (resolved via next-layer vkGetDeviceProcAddr)
    PFN_vkGetDeviceQueue           GetDeviceQueue = nullptr;
    PFN_vkCreateCommandPool        CreateCommandPool = nullptr;
    PFN_vkDestroyCommandPool       DestroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers   AllocateCommandBuffers = nullptr;
    PFN_vkFreeCommandBuffers       FreeCommandBuffers = nullptr;
    PFN_vkBeginCommandBuffer       BeginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer         EndCommandBuffer = nullptr;
    PFN_vkResetCommandBuffer       ResetCommandBuffer = nullptr;
    PFN_vkQueueSubmit              QueueSubmit = nullptr;
    PFN_vkCreateFence              CreateFence = nullptr;
    PFN_vkDestroyFence             DestroyFence = nullptr;
    PFN_vkCreateSemaphore          CreateSemaphore = nullptr;
    PFN_vkDestroySemaphore         DestroySemaphore = nullptr;
    PFN_vkGetFenceStatus           GetFenceStatus = nullptr;
    PFN_vkWaitForFences            WaitForFences = nullptr;
    PFN_vkResetFences              ResetFences = nullptr;
    PFN_vkCreateBuffer             CreateBuffer = nullptr;
    PFN_vkDestroyBuffer            DestroyBuffer = nullptr;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements = nullptr;
    PFN_vkAllocateMemory           AllocateMemory = nullptr;
    PFN_vkFreeMemory               FreeMemory = nullptr;
    PFN_vkBindBufferMemory         BindBufferMemory = nullptr;
    PFN_vkMapMemory                MapMemory = nullptr;
    PFN_vkUnmapMemory              UnmapMemory = nullptr;
    PFN_vkInvalidateMappedMemoryRanges InvalidateMappedMemoryRanges = nullptr;
    PFN_vkCreateImage              CreateImage = nullptr;
    PFN_vkDestroyImage             DestroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements = nullptr;
    PFN_vkBindImageMemory          BindImageMemory = nullptr;
    PFN_vkCmdPipelineBarrier       CmdPipelineBarrier = nullptr;
    PFN_vkCmdCopyImageToBuffer     CmdCopyImageToBuffer = nullptr;
    PFN_vkCmdResolveImage          CmdResolveImage = nullptr;
    PFN_vkGetSwapchainImagesKHR    GetSwapchainImagesKHR = nullptr;
    PFN_vkDeviceWaitIdle           DeviceWaitIdle = nullptr;

    PFN_vkGetDeviceProcAddr           GetDeviceProcAddr = nullptr;
    PFN_vkGetInstanceProcAddr         GetInstanceProcAddr = nullptr;

    // Instance dispatch pointers
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkGetPhysicalDeviceProperties       GetPhysicalDeviceProperties = nullptr;
};

// ── public API (Capture_Init / Capture_Shutdown defined in consumer_backend.cpp)
void Capture_Init();
void Capture_OnSwapchainCreate(const VkCaptureDeviceInfo& devInfo,
                               VkSwapchainKHR swapchain,
                               uint32_t width,
                               uint32_t height,
                               VkFormat format,
                               uint32_t imageCount,
                               const VkImage* images);
VkSemaphore Capture_OnPresent(const VkCaptureDeviceInfo& devInfo,
                              VkQueue presentQueue,
                              VkSwapchainKHR swapchain,
                              uint32_t imageIndex,
                              uint32_t waitSemaphoreCount,
                              const VkSemaphore* waitSemaphores);
void Capture_OnPreReset(const VkCaptureDeviceInfo* devInfo);
void Capture_Shutdown();

// ── internal helper implemented in capture.cpp, called by Capture_Shutdown() ──
void Capture_ReleaseSurfaces();

bool Capture_HasActiveWork();
bool Capture_WantsFrame();
void Capture_CancelPendingFrame();
void Capture_ReportPresentTiming(double captureMs, double overlayMs, double presentMs);
bool Capture_IsConsumerActive();
bool Capture_IsDaemonActive();
void Capture_SetDaemonRecord(bool active, uint32_t fps, uint32_t bitrateKbps, const char* path, uint64_t* outRecordedFrames);
uint64_t Capture_GetDaemonRecordedFrames();
float    Capture_GetDaemonAudioPeakDb();
void     Capture_SetSharedMenuOpen(bool open);
bool     Capture_IsSharedMenuOpen();
void     Capture_SetMeterRequested(bool active);
void Capture_SetDaemonPaused(bool paused);

struct FrameData
{
    const void* pixels;   // top-left origin, tightly-packed 32-bpp rows
    uint32_t    width;
    uint32_t    height;
    uint32_t    stride;   // bytes per row (width * 4)
    uint32_t    format;   // VkFormat value
    uint64_t    frameIdx; // monotonically increasing counter
    uint64_t    timestampNs;
};

struct CaptureStats
{
    uint64_t  presentCalls;
    uint64_t  capturedFrames;
    uint32_t  width;
    uint32_t  height;
    uint32_t  format;
    float     presentFps;
    float     captureFps;
    float     readbackMs;
    bool      isConsumerActive;
    bool      captureEnabled;
    int       targetFps;
    int       dumpQuotaRemaining;
    char      lastScreenshotPath[MAX_PATH];
};

void Capture_GetStats(CaptureStats* pStats);
void Capture_SetEnabled(bool enabled);
bool Capture_IsEnabled();
void Capture_SetTargetFps(int targetFps);
int  Capture_GetTargetFps();
void Capture_TriggerSnapshot();
bool Capture_IsSnapshotPending();
bool Capture_TakeSnapshotPending();
void Capture_SetLastScreenshotPath(const char* path);
void Capture_GetLastScreenshotPath(char* dst, size_t maxLen);
void Capture_SetDumpQuota(int count);
int  Capture_GetDumpQuota();

void Capture_FrameReady(const FrameData& frame);

// ── In-Memory Log Ring Buffer for In-Game Console ────────────────────────────
struct LogEntry
{
    char text[512];
};

size_t Log_GetRecentEntries(LogEntry* outEntries, size_t maxCount);
void   Log_ClearRecentEntries();
void   Log_ClearLogFile();
