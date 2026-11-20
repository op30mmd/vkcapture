/**
 * vk_testapp.cpp  —  Minimal Vulkan + X11 verification target
 *
 * Mirrors tools/d3d11_testapp.cpp: creates an X11 window, Vulkan instance,
 * device, and swapchain, clears the swapchain image with a changing color
 * plus a white 80x40 TOP-LEFT orientation marker, and presents in a loop.
 *
 * Usage:
 *   vk_testapp [frames] [width] [height]
 */

#define VK_USE_PLATFORM_XLIB_KHR
#include <vulkan/vulkan.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <unistd.h>

static void SendSyntheticKey(Display* dpy, Window win, KeySym ks)
{
    XKeyEvent ev = {};
    ev.type       = KeyPress;
    ev.display    = dpy;
    ev.window     = win;
    ev.root       = DefaultRootWindow(dpy);
    ev.keycode    = XKeysymToKeycode(dpy, ks);
    ev.same_screen = True;
    XSendEvent(dpy, win, True, KeyPressMask, reinterpret_cast<XEvent*>(&ev));
    XFlush(dpy);
}

int main(int argc, char* argv[])
{
    const int      totalFrames = (argc > 1) ? std::atoi(argv[1]) : 300;
    uint32_t       width       = (argc > 2) ? static_cast<uint32_t>(std::atoi(argv[2])) : 640;
    uint32_t       height      = (argc > 3) ? static_cast<uint32_t>(std::atoi(argv[3])) : 480;

    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) { std::printf("[testapp] XOpenDisplay failed\n"); return 1; }
    int screen = DefaultScreen(dpy);
    Window win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 0, 0, width, height, 1,
                                     BlackPixel(dpy, screen), WhitePixel(dpy, screen));
    XSelectInput(dpy, win, ExposureMask | KeyPressMask | ButtonPressMask | PointerMotionMask | StructureNotifyMask);
    XStoreName(dpy, win, "vkcapture test app");
    XMapWindow(dpy, win);
    XFlush(dpy);

    const char* instExts[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_XLIB_SURFACE_EXTENSION_NAME };
    VkApplicationInfo appInfo = {};
    appInfo.sType      = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ici = {};
    ici.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo        = &appInfo;
    ici.enabledExtensionCount   = 2;
    ici.ppEnabledExtensionNames = instExts;

    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS)
    {
        std::printf("[testapp] vkCreateInstance failed\n");
        return 1;
    }

    VkXlibSurfaceCreateInfoKHR sci = {};
    sci.sType  = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
    sci.dpy    = dpy;
    sci.window = win;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    vkCreateXlibSurfaceKHR(instance, &sci, nullptr, &surface);

    uint32_t devCount = 0;
    vkEnumeratePhysicalDevices(instance, &devCount, nullptr);
    if (devCount == 0) { std::printf("[testapp] No Vulkan GPU found\n"); return 1; }
    std::vector<VkPhysicalDevice> gpus(devCount);
    vkEnumeratePhysicalDevices(instance, &devCount, gpus.data());
    VkPhysicalDevice phys = gpus[0];

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {};
    qci.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = 0;
    qci.queueCount       = 1;
    qci.pQueuePriorities = &prio;

    const char* devExts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo dci = {};
    dci.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount    = 1;
    dci.pQueueCreateInfos       = &qci;
    dci.enabledExtensionCount   = 1;
    dci.ppEnabledExtensionNames = devExts;

    VkDevice device = VK_NULL_HANDLE;
    if (vkCreateDevice(phys, &dci, nullptr, &device) != VK_SUCCESS)
    {
        std::printf("[testapp] vkCreateDevice failed\n");
        return 1;
    }

    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, 0, 0, &queue);

    auto createSwapchain = [&](uint32_t& w, uint32_t& h, VkSwapchainKHR oldSc) -> VkSwapchainKHR {
        VkSurfaceCapabilitiesKHR caps = {};
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps);
        if (caps.currentExtent.width != UINT32_MAX)
        {
            w = caps.currentExtent.width;
            h = caps.currentExtent.height;
        }
        uint32_t minImgs = caps.minImageCount > 2 ? caps.minImageCount : 2;
        if (caps.maxImageCount > 0 && minImgs > caps.maxImageCount) minImgs = caps.maxImageCount;

        VkSwapchainCreateInfoKHR swci = {};
        swci.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        swci.surface          = surface;
        swci.minImageCount    = minImgs;
        swci.imageFormat      = VK_FORMAT_B8G8R8A8_UNORM;
        swci.imageColorSpace  = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        swci.imageExtent      = { w, h };
        swci.imageArrayLayers = 1;
        swci.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        swci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        swci.preTransform     = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        swci.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        swci.presentMode      = VK_PRESENT_MODE_FIFO_KHR;
        swci.clipped          = VK_TRUE;
        swci.oldSwapchain     = oldSc;

        VkSwapchainKHR sc = VK_NULL_HANDLE;
        vkCreateSwapchainKHR(device, &swci, nullptr, &sc);
        if (oldSc != VK_NULL_HANDLE) vkDestroySwapchainKHR(device, oldSc, nullptr);
        return sc;
    };

    VkSwapchainKHR swapchain = createSwapchain(width, height, VK_NULL_HANDLE);
    uint32_t imgCount = 0;
    vkGetSwapchainImagesKHR(device, swapchain, &imgCount, nullptr);
    std::vector<VkImage> scImages(imgCount);
    vkGetSwapchainImagesKHR(device, swapchain, &imgCount, scImages.data());

    VkCommandPoolCreateInfo cpci = {};
    cpci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = 0;
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    vkCreateCommandPool(device, &cpci, nullptr, &cmdPool);

    VkCommandBufferAllocateInfo cbai = {};
    cbai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool        = cmdPool;
    cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmdBuf = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device, &cbai, &cmdBuf);

    VkSemaphoreCreateInfo smci = {};
    smci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkSemaphore imgAvailSem = VK_NULL_HANDLE, renderDoneSem = VK_NULL_HANDLE;
    vkCreateSemaphore(device, &smci, nullptr, &imgAvailSem);
    vkCreateSemaphore(device, &smci, nullptr, &renderDoneSem);

    VkFenceCreateInfo fci = {};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkFence fence = VK_NULL_HANDLE;
    vkCreateFence(device, &fci, nullptr, &fence);

    for (int frame = 0; frame < totalFrames; ++frame)
    {
        while (XPending(dpy) > 0)
        {
            XEvent ev;
            XNextEvent(dpy, &ev);
        }

        if (totalFrames >= 100)
        {
            if (frame == 15)      { std::printf("[testapp] Frame 15: Toggle overlay ON (Insert)\n"); SendSyntheticKey(dpy, win, XK_Insert); }
            else if (frame == 25) { std::printf("[testapp] Frame 25: Toggle recording ON (F9)\n"); SendSyntheticKey(dpy, win, XK_F9); }
            else if (frame == 76) { std::printf("[testapp] Frame 76: Stop+Start (F9 x2)\n"); SendSyntheticKey(dpy, win, XK_F9); SendSyntheticKey(dpy, win, XK_F9); }
            else if (frame == 85)
            {
                std::printf("[testapp] Frame 85: Resize swapchain to 800x600\n");
                vkDeviceWaitIdle(device);
                width = 800; height = 600;
                XResizeWindow(dpy, win, width, height);
                XSync(dpy, False);
                usleep(50000);
                while (XPending(dpy) > 0) { XEvent ev; XNextEvent(dpy, &ev); }
                swapchain = createSwapchain(width, height, swapchain);
                vkGetSwapchainImagesKHR(device, swapchain, &imgCount, nullptr);
                scImages.resize(imgCount);
                vkGetSwapchainImagesKHR(device, swapchain, &imgCount, scImages.data());
            }
            else if (frame == 95)  { std::printf("[testapp] Frame 95: Toggle overlay OFF (Insert)\n"); SendSyntheticKey(dpy, win, XK_Insert); }
            else if (frame == 120) { std::printf("[testapp] Frame 120: Toggle recording OFF (F9)\n"); SendSyntheticKey(dpy, win, XK_F9); }
        }

        vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);

        uint32_t imgIdx = 0;
        VkResult acqRes = vkAcquireNextImageKHR(device, swapchain, 100000000ULL, imgAvailSem, VK_NULL_HANDLE, &imgIdx);
        if (acqRes != VK_SUCCESS && acqRes != VK_SUBOPTIMAL_KHR)
            continue;

        vkResetFences(device, 1, &fence);

        vkResetCommandBuffer(cmdBuf, 0);
        VkCommandBufferBeginInfo cbbi = {};
        cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(cmdBuf, &cbbi);

        VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkImageMemoryBarrier toDst = {};
        toDst.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toDst.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        toDst.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        toDst.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDst.image               = scImages[imgIdx];
        toDst.subresourceRange    = range;
        vkCmdPipelineBarrier(cmdBuf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &toDst);

        VkClearColorValue bg = {};
        bg.float32[0] = ((frame * 3) & 0xFF) / 255.0f;
        bg.float32[1] = 64.0f / 255.0f;
        bg.float32[2] = 128.0f / 255.0f;
        bg.float32[3] = 1.0f;
        vkCmdClearColorImage(cmdBuf, scImages[imgIdx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &bg, 1, &range);

        VkImageMemoryBarrier toPres = toDst;
        toPres.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toPres.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        toPres.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toPres.newLayout     = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier(cmdBuf, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &toPres);

        vkEndCommandBuffer(cmdBuf);

        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo si = {};
        si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.waitSemaphoreCount   = 1;
        si.pWaitSemaphores      = &imgAvailSem;
        si.pWaitDstStageMask    = &waitStage;
        si.commandBufferCount   = 1;
        si.pCommandBuffers      = &cmdBuf;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores    = &renderDoneSem;
        vkQueueSubmit(queue, 1, &si, fence);

        VkPresentInfoKHR pi = {};
        pi.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores    = &renderDoneSem;
        pi.swapchainCount     = 1;
        pi.pSwapchains        = &swapchain;
        pi.pImageIndices      = &imgIdx;
        vkQueuePresentKHR(queue, &pi);

        usleep(16000);
    }

    vkDeviceWaitIdle(device);
    vkDestroyFence(device, fence, nullptr);
    vkDestroySemaphore(device, imgAvailSem, nullptr);
    vkDestroySemaphore(device, renderDoneSem, nullptr);
    vkDestroyCommandPool(device, cmdPool, nullptr);
    vkDestroySwapchainKHR(device, swapchain, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroySurfaceKHR(instance, surface, nullptr);
    vkDestroyInstance(instance, nullptr);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    std::printf("[testapp] Done (%d frames)\n", totalFrames);
    return 0;
}
