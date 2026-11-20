/**
 * capture.cpp  —  Asynchronous Vulkan GPU frame readback (Linux port)
 *
 * Uses quad-buffered host-visible VkBuffers + VkFences so GPU->CPU DMA never
 * stalls the game's render thread:
 *   1. Completed slots from prior frames are reaped non-blockingly via vkGetFenceStatus.
 *   2. If a consumer wants the current frame, we enqueue vkCmdCopyImageToBuffer
 *      into a free slot's VkBuffer and submit with its VkFence.
 *   3. If all 4 slots are still in flight, the frame is skipped rather than
 *      blocking vkQueuePresentKHR.
 */

#include <cstdio>
#include <cstring>
#include <mutex>
#include <atomic>
#include <vector>
#include <thread>
#include <condition_variable>
#include <deque>

#include "capture.h"
#include "recorder.h"

static constexpr int      NUM_STAGING = 4;
static constexpr uint32_t MAX_WIDTH   = 7680;
static constexpr uint32_t MAX_HEIGHT  = 4320;

enum class SlotState { Free, Pending, Delivering };

struct StagingSlot
{
    VkBuffer        buffer       = VK_NULL_HANDLE;
    VkDeviceMemory  memory       = VK_NULL_HANDLE;
    void*           mapped       = nullptr;
    bool            coherent     = false;
    VkCommandBuffer cmdBuf       = VK_NULL_HANDLE;
    VkFence         fence        = VK_NULL_HANDLE;
    VkSemaphore     doneSem      = VK_NULL_HANDLE;
    VkDeviceSize    bufferSize   = 0;
    uint32_t        width        = 0;
    uint32_t        height       = 0;
    VkFormat        format       = VK_FORMAT_UNDEFINED;
    uint64_t        frameIdx     = 0;
    uint64_t        timestampNs  = 0;
    SlotState       state        = SlotState::Free;
};

struct StageTiming
{
    double   sumMs = 0.0;
    double   maxMs = 0.0;
    uint64_t count = 0;
    void Add(double ms) { sumMs += ms; if (ms > maxMs) maxMs = ms; ++count; }
    double Avg() const { return count ? sumMs / static_cast<double>(count) : 0.0; }
    void Reset() { sumMs = 0.0; maxMs = 0.0; count = 0; }
};

// ── Module State ─────────────────────────────────────────────────────────────
static std::mutex              g_CapMtx;
static VkCaptureDeviceInfo     g_DevInfo;
static VkCommandPool           g_CmdPool = VK_NULL_HANDLE;
static StagingSlot             g_Slots[NUM_STAGING];
static int                     g_WriteIdx = 0;
static int                     g_ReadIdx  = 0;

static VkSwapchainKHR          g_Swapchain = VK_NULL_HANDLE;
static uint32_t                g_ScWidth   = 0;
static uint32_t                g_ScHeight  = 0;
static VkFormat                g_ScFormat  = VK_FORMAT_UNDEFINED;
static std::vector<VkImage>    g_ScImages;

static std::atomic<uint64_t>   g_FrameIdx{ 0 };
static std::atomic<int>        g_InFlightSlots{ 0 };
static std::atomic<uint64_t>   g_PresentCalls{ 0 };
static std::atomic<bool>       g_Shutdown{ false };
static std::atomic<bool>       g_CaptureEnabled{ true };
static std::atomic<int>        g_TargetFps{ 0 };

static std::atomic<uint32_t>   g_LastWidth{ 0 };
static std::atomic<uint32_t>   g_LastHeight{ 0 };
static std::atomic<uint32_t>   g_LastFormat{ 0 };
static std::atomic<float>      g_LastReadbackMs{ 0.0f };
static std::atomic<uint64_t>   g_MapSkips{ 0 };
static std::atomic<float>      g_PresentFps{ 0.0f };
static std::atomic<float>      g_CaptureFps{ 0.0f };

static uint64_t                g_LastFpsNs = 0;
static uint64_t                g_LastFpsPresentCount = 0;
static uint64_t                g_LastFpsCaptureCount = 0;
static uint64_t                g_NextCaptureNs = 0;

static StageTiming             g_TCopy;
static StageTiming             g_TDeliver;
static StageTiming             g_TCapture;
static StageTiming             g_TOverlay;
static StageTiming             g_TPresent;
static uint64_t                g_LastHeartbeatNs = 0;

static std::mutex              g_DeliverMtx;
static std::condition_variable g_DeliverCv;
static std::condition_variable g_DeliverIdleCv;
static std::deque<int>         g_DeliverQueue;
static std::thread             g_DeliverThread;
static bool                    g_DeliverStarted = false;
static bool                    g_DeliverStop    = false;
static bool                    g_DeliverBusy    = false;

static void DeliverWorkerLoop()
{
    for (;;)
    {
        int idx = -1;
        {
            std::unique_lock<std::mutex> dlk(g_DeliverMtx);
            g_DeliverCv.wait(dlk, [] { return g_DeliverStop || !g_DeliverQueue.empty(); });
            if (g_DeliverStop && g_DeliverQueue.empty())
                return;
            idx = g_DeliverQueue.front();
            g_DeliverQueue.pop_front();
            g_DeliverBusy = true;
        }

        StagingSlot& s = g_Slots[idx];
        uint64_t t0 = Platform_GetMonotonicNs();
        if (s.mapped)
        {
            FrameData fd = {};
            fd.pixels   = s.mapped;
            fd.width    = s.width;
            fd.height   = s.height;
            fd.stride   = s.width * 4;
            fd.format   = static_cast<uint32_t>(s.format);
            fd.frameIdx    = s.frameIdx;
            fd.timestampNs = s.timestampNs;
            Capture_FrameReady(fd);
        }
        double deliverMs = static_cast<double>(Platform_GetMonotonicNs() - t0) / 1e6;

        {
            std::lock_guard<std::mutex> clk(g_CapMtx);
            g_TDeliver.Add(deliverMs);
            g_LastReadbackMs.store(static_cast<float>(deliverMs));
            s.state = SlotState::Free;
            g_InFlightSlots.fetch_sub(1, std::memory_order_release);
        }

        {
            std::lock_guard<std::mutex> dlk(g_DeliverMtx);
            g_DeliverBusy = false;
            if (g_DeliverQueue.empty())
                g_DeliverIdleCv.notify_all();
        }
    }
}

static void EnsureDeliverThreadStarted()
{
    std::lock_guard<std::mutex> dlk(g_DeliverMtx);
    if (!g_DeliverStarted)
    {
        g_DeliverStop    = false;
        g_DeliverStarted = true;
        g_DeliverThread  = std::thread(DeliverWorkerLoop);
    }
}

static void WaitDeliverIdle()
{
    std::unique_lock<std::mutex> dlk(g_DeliverMtx);
    if (!g_DeliverStarted) return;
    g_DeliverIdleCv.wait(dlk, [] { return !g_DeliverBusy && g_DeliverQueue.empty(); });
}

void Capture_SetEnabled(bool enabled)   { g_CaptureEnabled.store(enabled); }
bool Capture_IsEnabled()                { return g_CaptureEnabled.load(); }
void Capture_SetTargetFps(int fps)      { g_TargetFps.store(fps); }
int  Capture_GetTargetFps()             { return g_TargetFps.load(); }

void Capture_GetStats(CaptureStats* pStats)
{
    if (!pStats) return;
    pStats->presentCalls       = g_PresentCalls.load();
    pStats->capturedFrames     = g_FrameIdx.load();
    pStats->width              = g_LastWidth.load();
    pStats->height             = g_LastHeight.load();
    pStats->format             = g_LastFormat.load();
    pStats->presentFps         = g_PresentFps.load();
    pStats->captureFps         = g_CaptureFps.load();
    pStats->readbackMs         = g_LastReadbackMs.load();
    pStats->isConsumerActive   = Capture_IsConsumerActive();
    pStats->captureEnabled     = g_CaptureEnabled.load();
    pStats->targetFps          = g_TargetFps.load();
    pStats->dumpQuotaRemaining = Capture_GetDumpQuota();
    Capture_GetLastScreenshotPath(pStats->lastScreenshotPath, sizeof(pStats->lastScreenshotPath));
}

static uint32_t FindMemoryType(const VkCaptureDeviceInfo& d, uint32_t typeFilter,
                               VkMemoryPropertyFlags preferred, VkMemoryPropertyFlags required,
                               bool* outHasPreferred)
{
    VkPhysicalDeviceMemoryProperties memProps = {};
    d.GetPhysicalDeviceMemoryProperties(d.physicalDevice, &memProps);

    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
    {
        if ((typeFilter & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & preferred) == preferred)
        {
            if (outHasPreferred) *outHasPreferred = true;
            return i;
        }
    }
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
    {
        if ((typeFilter & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & required) == required)
        {
            if (outHasPreferred) *outHasPreferred = false;
            return i;
        }
    }
    return UINT32_MAX;
}

static void DestroySlotLocked(StagingSlot& s)
{
    if (g_DevInfo.device == VK_NULL_HANDLE) return;

    if (s.state == SlotState::Pending)
    {
        if (s.fence != VK_NULL_HANDLE)
            g_DevInfo.WaitForFences(g_DevInfo.device, 1, &s.fence, VK_TRUE, 1000000000ULL);
        Capture_CancelPendingFrame();
        g_InFlightSlots.fetch_sub(1, std::memory_order_release);
    }

    if (s.mapped && s.memory != VK_NULL_HANDLE)
    {
        g_DevInfo.UnmapMemory(g_DevInfo.device, s.memory);
        s.mapped = nullptr;
    }
    if (s.buffer != VK_NULL_HANDLE) { g_DevInfo.DestroyBuffer(g_DevInfo.device, s.buffer, nullptr); s.buffer = VK_NULL_HANDLE; }
    if (s.memory != VK_NULL_HANDLE) { g_DevInfo.FreeMemory(g_DevInfo.device, s.memory, nullptr);    s.memory = VK_NULL_HANDLE; }
    if (s.fence   != VK_NULL_HANDLE) { g_DevInfo.DestroyFence(g_DevInfo.device, s.fence, nullptr);       s.fence   = VK_NULL_HANDLE; }
    if (s.doneSem != VK_NULL_HANDLE) { g_DevInfo.DestroySemaphore(g_DevInfo.device, s.doneSem, nullptr); s.doneSem = VK_NULL_HANDLE; }
    if (s.cmdBuf != VK_NULL_HANDLE && g_CmdPool != VK_NULL_HANDLE)
    {
        g_DevInfo.FreeCommandBuffers(g_DevInfo.device, g_CmdPool, 1, &s.cmdBuf);
        s.cmdBuf = VK_NULL_HANDLE;
    }

    s.bufferSize = 0;
    s.width = s.height = 0;
    s.format = VK_FORMAT_UNDEFINED;
    s.state = SlotState::Free;
}

static bool EnsureSlotLocked(StagingSlot& s, uint32_t w, uint32_t h, VkFormat fmt)
{
    VkDeviceSize needed = static_cast<VkDeviceSize>(w) * h * 4;
    if (s.buffer != VK_NULL_HANDLE && s.bufferSize >= needed && s.cmdBuf != VK_NULL_HANDLE && s.fence != VK_NULL_HANDLE)
    {
        s.width = w;
        s.height = h;
        s.format = fmt;
        return true;
    }

    DestroySlotLocked(s);

    if (g_CmdPool == VK_NULL_HANDLE)
    {
        VkCommandPoolCreateInfo cpci = {};
        cpci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cpci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        cpci.queueFamilyIndex = g_DevInfo.graphicsQueueFamily;
        if (g_DevInfo.CreateCommandPool(g_DevInfo.device, &cpci, nullptr, &g_CmdPool) != VK_SUCCESS)
        {
            Log("[capvk] vkCreateCommandPool failed");
            return false;
        }
    }

    VkBufferCreateInfo bci = {};
    bci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size        = needed;
    bci.usage       = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (g_DevInfo.CreateBuffer(g_DevInfo.device, &bci, nullptr, &s.buffer) != VK_SUCCESS)
    {
        Log("[capvk] vkCreateBuffer failed (%ux%u)", w, h);
        return false;
    }

    VkMemoryRequirements req = {};
    g_DevInfo.GetBufferMemoryRequirements(g_DevInfo.device, s.buffer, &req);

    bool hasCached = false;
    uint32_t memType = FindMemoryType(
        g_DevInfo, req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
        &hasCached);

    if (memType != UINT32_MAX)
    {
        s.coherent = hasCached; // true only if HOST_COHERENT was also matched
    }
    else
    {
        memType = FindMemoryType(
            g_DevInfo, req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
            &hasCached);
        s.coherent = hasCached;
    }

    if (memType == UINT32_MAX)
    {
        Log("[capvk] No host-visible memory type found");
        DestroySlotLocked(s);
        return false;
    }

    VkMemoryAllocateInfo mai = {};
    mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize  = req.size;
    mai.memoryTypeIndex = memType;
    if (g_DevInfo.AllocateMemory(g_DevInfo.device, &mai, nullptr, &s.memory) != VK_SUCCESS)
    {
        Log("[capvk] vkAllocateMemory failed");
        DestroySlotLocked(s);
        return false;
    }

    g_DevInfo.BindBufferMemory(g_DevInfo.device, s.buffer, s.memory, 0);
    if (g_DevInfo.MapMemory(g_DevInfo.device, s.memory, 0, VK_WHOLE_SIZE, 0, &s.mapped) != VK_SUCCESS)
    {
        Log("[capvk] vkMapMemory failed");
        DestroySlotLocked(s);
        return false;
    }

    VkCommandBufferAllocateInfo cbai = {};
    cbai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool        = g_CmdPool;
    cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    if (g_DevInfo.AllocateCommandBuffers(g_DevInfo.device, &cbai, &s.cmdBuf) != VK_SUCCESS)
    {
        DestroySlotLocked(s);
        return false;
    }

    VkFenceCreateInfo fci = {};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (g_DevInfo.CreateFence(g_DevInfo.device, &fci, nullptr, &s.fence) != VK_SUCCESS)
    {
        DestroySlotLocked(s);
        return false;
    }
    VkSemaphoreCreateInfo sci = {};
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    if (g_DevInfo.CreateSemaphore(g_DevInfo.device, &sci, nullptr, &s.doneSem) != VK_SUCCESS)
    {
        DestroySlotLocked(s);
        return false;
    }

    s.bufferSize = needed;
    s.width      = w;
    s.height     = h;
    s.format     = fmt;
    s.state      = SlotState::Free;
    return true;
}

static void ReapCompletedSlotsLocked()
{
    for (int k = 0; k < NUM_STAGING; ++k)
    {
        int idx = (g_ReadIdx + k) % NUM_STAGING;
        StagingSlot& s = g_Slots[idx];
        if (s.state == SlotState::Delivering) continue;
        if (s.state != SlotState::Pending) continue;

        if (g_DevInfo.GetFenceStatus(g_DevInfo.device, s.fence) != VK_SUCCESS)
            break; // preserve frame order

        if (!s.coherent)
        {
            VkMappedMemoryRange range = {};
            range.sType  = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
            range.memory = s.memory;
            range.offset = 0;
            range.size   = VK_WHOLE_SIZE;
            g_DevInfo.InvalidateMappedMemoryRanges(g_DevInfo.device, 1, &range);
        }

        g_DevInfo.ResetFences(g_DevInfo.device, 1, &s.fence);
        s.state = SlotState::Delivering;
        g_ReadIdx = (idx + 1) % NUM_STAGING;

        EnsureDeliverThreadStarted();
        {
            std::lock_guard<std::mutex> dlk(g_DeliverMtx);
            g_DeliverQueue.push_back(idx);
        }
        g_DeliverCv.notify_one();
    }
}

static void StopDeliverThreadIfShutdown()
{
    std::thread toJoin;
    {
        std::lock_guard<std::mutex> dlk(g_DeliverMtx);
        if (!g_DeliverStarted) return;
        g_DeliverStop = true;
        g_DeliverCv.notify_all();
        toJoin = std::move(g_DeliverThread);
        g_DeliverStarted = false;
    }
    if (toJoin.joinable()) toJoin.join();
}

void Capture_ReleaseSurfaces()
{
    WaitDeliverIdle();
    if (g_Shutdown.load()) StopDeliverThreadIfShutdown();
    std::lock_guard<std::mutex> lk(g_CapMtx);
    for (int i = 0; i < NUM_STAGING; ++i)
        DestroySlotLocked(g_Slots[i]);
    if (g_CmdPool != VK_NULL_HANDLE && g_DevInfo.device != VK_NULL_HANDLE)
    {
        g_DevInfo.DestroyCommandPool(g_DevInfo.device, g_CmdPool, nullptr);
        g_CmdPool = VK_NULL_HANDLE;
    }
    g_WriteIdx = g_ReadIdx = 0;
    g_ScImages.clear();
    g_Swapchain = VK_NULL_HANDLE;
}

void Capture_OnSwapchainCreate(const VkCaptureDeviceInfo& devInfo,
                               VkSwapchainKHR swapchain,
                               uint32_t width,
                               uint32_t height,
                               VkFormat format,
                               uint32_t imageCount,
                               const VkImage* images)
{
    std::lock_guard<std::mutex> lk(g_CapMtx);
    g_DevInfo   = devInfo;
    g_Swapchain = swapchain;
    g_ScWidth   = width;
    g_ScHeight  = height;
    g_ScFormat  = format;
    g_ScImages.assign(images, images + imageCount);

    g_LastWidth.store(width);
    g_LastHeight.store(height);
    g_LastFormat.store(static_cast<uint32_t>(format));
    Recorder_SetDefaultResolution(width, height);

    Log("[capvk] Swapchain tracked: %ux%u fmt=%u images=%u",
        width, height, static_cast<unsigned>(format), imageCount);
}

void Capture_OnPreReset(const VkCaptureDeviceInfo* devInfo)
{
    (void)devInfo;
    Capture_ReleaseSurfaces();
}

void Capture_ReportPresentTiming(double captureMs, double overlayMs, double presentMs)
{
    g_TCapture.Add(captureMs);
    g_TOverlay.Add(overlayMs);
    g_TPresent.Add(presentMs);

    uint64_t nowNs = Platform_GetMonotonicNs();
    if (g_LastHeartbeatNs == 0) g_LastHeartbeatNs = nowNs;
    double elapsedSec = static_cast<double>(nowNs - g_LastHeartbeatNs) / 1e9;
    if (elapsedSec >= 5.0)
    {
        if (g_TCopy.count > 0 || g_TDeliver.count > 0 || Capture_HasActiveWork())
        {
            Log("[capvk] heartbeat: presents=%llu captured=%llu skips=%llu | "
                "copy=%.2f/%.2fms deliver=%.2f/%.2fms overlay=%.2f/%.2fms present=%.2f/%.2fms",
                static_cast<unsigned long long>(g_PresentCalls.load()),
                static_cast<unsigned long long>(g_FrameIdx.load()),
                static_cast<unsigned long long>(g_MapSkips.load()),
                g_TCopy.Avg(), g_TCopy.maxMs,
                g_TDeliver.Avg(), g_TDeliver.maxMs,
                g_TOverlay.Avg(), g_TOverlay.maxMs,
                g_TPresent.Avg(), g_TPresent.maxMs);
        }
        g_TCopy.Reset();
        g_TDeliver.Reset();
        g_TCapture.Reset();
        g_TOverlay.Reset();
        g_TPresent.Reset();
        g_LastHeartbeatNs = nowNs;
    }
}

VkSemaphore Capture_OnPresent(const VkCaptureDeviceInfo& devInfo,
                              VkQueue presentQueue,
                              VkSwapchainKHR swapchain,
                              uint32_t imageIndex,
                              uint32_t waitSemaphoreCount,
                              const VkSemaphore* waitSemaphores)
{
    uint64_t presentNum = g_PresentCalls.fetch_add(1) + 1;
    uint64_t nowNs = Platform_GetMonotonicNs();

    if (g_LastFpsNs == 0) g_LastFpsNs = nowNs;
    double fpsDt = static_cast<double>(nowNs - g_LastFpsNs) / 1e9;
    if (fpsDt >= 0.5)
    {
        uint64_t curCap = g_FrameIdx.load();
        g_PresentFps.store(static_cast<float>((presentNum - g_LastFpsPresentCount) / fpsDt));
        g_CaptureFps.store(static_cast<float>((curCap - g_LastFpsCaptureCount) / fpsDt));
        g_LastFpsPresentCount = presentNum;
        g_LastFpsCaptureCount = curCap;
        g_LastFpsNs = nowNs;
    }

    if (g_Shutdown.load(std::memory_order_relaxed) || !g_CaptureEnabled.load(std::memory_order_relaxed))
        return VK_NULL_HANDLE;
    if (g_InFlightSlots.load(std::memory_order_acquire) == 0 && !Capture_HasActiveWork())
    {
        g_NextCaptureNs = 0;
        return VK_NULL_HANDLE;
    }

    std::lock_guard<std::mutex> lk(g_CapMtx);
    if (swapchain != g_Swapchain || imageIndex >= g_ScImages.size()) return VK_NULL_HANDLE;
    if (g_ScWidth == 0 || g_ScHeight == 0 || g_ScWidth > MAX_WIDTH || g_ScHeight > MAX_HEIGHT) return VK_NULL_HANDLE;

    g_DevInfo = devInfo;
    ReapCompletedSlotsLocked();

    int targetFps = g_TargetFps.load();
    uint64_t intervalNs = (targetFps > 0) ? (1000000000ULL / static_cast<uint64_t>(targetFps)) : 0;
    if (intervalNs > 0 && !Capture_IsSnapshotPending() && !Recorder_WantsFrame())
    {
        if (g_NextCaptureNs > 0 && nowNs < g_NextCaptureNs)
            return VK_NULL_HANDLE;
    }

    StagingSlot& slot = g_Slots[g_WriteIdx];
    if (slot.state != SlotState::Free)
    {
        g_MapSkips.fetch_add(1);
        return VK_NULL_HANDLE;
    }

    if (!Capture_WantsFrame())
    {
        g_NextCaptureNs = 0;
        return VK_NULL_HANDLE;
    }

    if (!EnsureSlotLocked(slot, g_ScWidth, g_ScHeight, g_ScFormat))
    {
        Capture_CancelPendingFrame();
        return VK_NULL_HANDLE;
    }

    uint64_t copyStartNs = Platform_GetMonotonicNs();
    VkImage srcImage = g_ScImages[imageIndex];

    g_DevInfo.ResetCommandBuffer(slot.cmdBuf, 0);
    VkCommandBufferBeginInfo cbbi = {};
    cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    g_DevInfo.BeginCommandBuffer(slot.cmdBuf, &cbbi);

    VkImageMemoryBarrier toTransfer = {};
    toTransfer.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toTransfer.srcAccessMask       = VK_ACCESS_MEMORY_READ_BIT;
    toTransfer.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
    toTransfer.oldLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toTransfer.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image               = srcImage;
    toTransfer.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    g_DevInfo.CmdPipelineBarrier(slot.cmdBuf,
                                 VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &toTransfer);

    VkBufferImageCopy region = {};
    region.bufferOffset      = 0;
    region.bufferRowLength   = 0; // tightly packed width * 4
    region.bufferImageHeight = 0;
    region.imageSubresource  = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageOffset       = { 0, 0, 0 };
    region.imageExtent       = { g_ScWidth, g_ScHeight, 1 };

    g_DevInfo.CmdCopyImageToBuffer(slot.cmdBuf, srcImage,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   slot.buffer, 1, &region);

    VkImageMemoryBarrier toPresent = {};
    toPresent.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toPresent.srcAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
    toPresent.dstAccessMask       = VK_ACCESS_MEMORY_READ_BIT;
    toPresent.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toPresent.newLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toPresent.image               = srcImage;
    toPresent.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    g_DevInfo.CmdPipelineBarrier(slot.cmdBuf,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &toPresent);

    g_DevInfo.EndCommandBuffer(slot.cmdBuf);

    VkPipelineStageFlags stackWaitStages[16];
    std::vector<VkPipelineStageFlags> heapWaitStages;
    VkPipelineStageFlags* pWaitStages = stackWaitStages;
    if (waitSemaphoreCount > 16)
    {
        heapWaitStages.assign(waitSemaphoreCount, VK_PIPELINE_STAGE_TRANSFER_BIT);
        pWaitStages = heapWaitStages.data();
    }
    else
    {
        for (uint32_t i = 0; i < waitSemaphoreCount; ++i)
            stackWaitStages[i] = VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    VkSubmitInfo si = {};
    si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount   = waitSemaphoreCount;
    si.pWaitSemaphores      = waitSemaphores;
    si.pWaitDstStageMask    = pWaitStages;
    si.commandBufferCount   = 1;
    si.pCommandBuffers      = &slot.cmdBuf;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores    = &slot.doneSem;

    VkSemaphore outSem = VK_NULL_HANDLE;
    if (g_DevInfo.QueueSubmit(presentQueue, 1, &si, slot.fence) == VK_SUCCESS)
    {
        slot.frameIdx    = g_FrameIdx.fetch_add(1) + 1;
        slot.timestampNs = nowNs;
        slot.state       = SlotState::Pending;
        g_InFlightSlots.fetch_add(1, std::memory_order_release);
        g_WriteIdx    = (g_WriteIdx + 1) % NUM_STAGING;
        if (intervalNs > 0)
        {
            if (g_NextCaptureNs == 0 || nowNs > g_NextCaptureNs + intervalNs)
                g_NextCaptureNs = nowNs + intervalNs;
            else
                g_NextCaptureNs += intervalNs;
        }
        outSem = slot.doneSem;
    }
    else
    {
        Capture_CancelPendingFrame();
    }

    double copyMs = static_cast<double>(Platform_GetMonotonicNs() - copyStartNs) / 1e6;
    g_TCopy.Add(copyMs);
    return outSem;
}
