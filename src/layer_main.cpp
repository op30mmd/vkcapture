/**
 * layer_main.cpp  —  Vulkan Layer + LD_PRELOAD entry point (Linux port of dllmain.cpp)
 *
 * Works both as:
 *   1. An explicit/implicit Vulkan Layer (VK_LAYER_VKCAPTURE_capture)
 *   2. An LD_PRELOAD library (intercepting X11 input, audio, and process exit)
 */

#define VK_USE_PLATFORM_XLIB_KHR
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <dlfcn.h>
#include <unistd.h>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "capture.h"
#include "overlay.h"
#include "recorder.h"
#include "audio.h"
#include "platform.h"

// ── Logging Ring Buffer ──────────────────────────────────────────────────────
static constexpr size_t LOG_RING_SIZE = 512;
static std::mutex g_LogMtx;
static LogEntry   g_LogRing[LOG_RING_SIZE];
static size_t     g_LogHead  = 0;
static size_t     g_LogCount = 0;
static FILE*      g_LogFile  = nullptr;

void Log(const char* fmt, ...)
{
    char msg[460];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    struct timespec ts = {};
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tmLocal = {};
    localtime_r(&ts.tv_sec, &tmLocal);

    char line[512];
    std::snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03ld] %s",
                  tmLocal.tm_hour, tmLocal.tm_min, tmLocal.tm_sec,
                  ts.tv_nsec / 1000000L, msg);

    std::lock_guard<std::mutex> lk(g_LogMtx);
    Platform_SafeStrCpy(g_LogRing[g_LogHead].text, sizeof(g_LogRing[g_LogHead].text), line);
    g_LogHead = (g_LogHead + 1) % LOG_RING_SIZE;
    if (g_LogCount < LOG_RING_SIZE) ++g_LogCount;

    if (!g_LogFile)
    {
        char logPath[MAX_PATH];
        std::snprintf(logPath, sizeof(logPath), "%s/debug.log", Platform_GetBaseDir());
        g_LogFile = std::fopen(logPath, "a");
    }
    if (g_LogFile)
    {
        std::fprintf(g_LogFile, "%s\n", line);
        std::fflush(g_LogFile);
    }
}

size_t Log_GetRecentEntries(LogEntry* outEntries, size_t maxCount)
{
    if (!outEntries || maxCount == 0) return 0;
    std::lock_guard<std::mutex> lk(g_LogMtx);
    size_t n = (g_LogCount < maxCount) ? g_LogCount : maxCount;
    size_t start = (g_LogHead + LOG_RING_SIZE - n) % LOG_RING_SIZE;
    for (size_t i = 0; i < n; ++i)
        outEntries[i] = g_LogRing[(start + i) % LOG_RING_SIZE];
    return n;
}

void Log_ClearRecentEntries()
{
    std::lock_guard<std::mutex> lk(g_LogMtx);
    g_LogHead = 0;
    g_LogCount = 0;
}

void Log_ClearLogFile()
{
    std::lock_guard<std::mutex> lk(g_LogMtx);
    if (g_LogFile) { std::fclose(g_LogFile); g_LogFile = nullptr; }
    char logPath[MAX_PATH];
    std::snprintf(logPath, sizeof(logPath), "%s/debug.log", Platform_GetBaseDir());
    g_LogFile = std::fopen(logPath, "w");
}

// ── Instance & Device Dispatch Tables ────────────────────────────────────────
struct InstanceDispatch
{
    VkInstance                        instance = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr         GetInstanceProcAddr = nullptr;
    PFN_vkDestroyInstance             DestroyInstance = nullptr;
    PFN_vkEnumeratePhysicalDevices       EnumeratePhysicalDevices = nullptr;
    PFN_vkEnumeratePhysicalDeviceGroups  EnumeratePhysicalDeviceGroups = nullptr;
    PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR GetPhysicalDeviceSurfaceCapabilitiesKHR = nullptr;
    PFN_vkCreateXlibSurfaceKHR        CreateXlibSurfaceKHR = nullptr;
};

struct DeviceDispatch
{
    VkCaptureDeviceInfo               info;
    PFN_vkGetDeviceProcAddr           GetDeviceProcAddr = nullptr;
    PFN_vkDestroyDevice               DestroyDevice = nullptr;
    PFN_vkCreateSwapchainKHR          CreateSwapchainKHR = nullptr;
    PFN_vkDestroySwapchainKHR         DestroySwapchainKHR = nullptr;
    PFN_vkQueuePresentKHR             QueuePresentKHR = nullptr;
};

static std::mutex g_DispatchMtx;
static std::unordered_map<VkInstance, InstanceDispatch> g_InstanceMap;
static std::unordered_map<VkDevice, DeviceDispatch>   g_DeviceMap;
static std::unordered_map<VkPhysicalDevice, VkInstance> g_PhysDevToInstance;
static std::unordered_map<VkSwapchainKHR, VkDevice>   g_SwapchainToDevice;
static std::unordered_map<VkPhysicalDevice, VkInstance> g_PhysDevToInst;
static std::unordered_map<VkQueue, VkDevice>            g_QueueToDev;
static VkSwapchainKHR                                 g_CurrentSwapchain = VK_NULL_HANDLE;
static std::atomic<bool> g_CaptureInitialized{ false };

static inline void* DispatchKey(void* obj)
{
    return *reinterpret_cast<void**>(obj);
}

static void EnsureCaptureInitialized()
{
    if (!g_CaptureInitialized.exchange(true))
    {
        Log("[layer] Initializing vkcapture subsystems (pid=%d)", static_cast<int>(getpid()));
        Capture_Init();
    }
}

// ── Shutdown / Auto-save on exit ─────────────────────────────────────────────
static std::atomic<bool> g_ShuttingDown{ false };

static void PerformGracefulShutdown()
{
    if (g_ShuttingDown.exchange(true)) return;
    if (g_CaptureInitialized.load())
    {
        if (Recorder_GetAutoSaveOnExit() && (Recorder_IsRecording() || Recorder_IsStarting()))
        {
            Log("[layer] Auto-saving active recording on exit...");
            Recorder_StopSync(5000);
        }
        Capture_Shutdown();
    }
}

static bool IsCaptureDisabledByEnv()
{
    static std::atomic<int> s_disabled{ -1 };
    int v = s_disabled.load(std::memory_order_relaxed);
    if (v < 0)
    {
        const char* dis = std::getenv("DISABLE_VKCAPTURE");
        const char* en  = std::getenv("ENABLE_VKCAPTURE");
        bool off = (dis && std::strcmp(dis, "1") == 0) ||
                   (en  && std::strcmp(en, "0") == 0);
        v = off ? 1 : 0;
        s_disabled.store(v, std::memory_order_relaxed);
    }
    return v == 1;
}

__attribute__((constructor))
static void OnLibraryLoad()
{
    std::atexit(PerformGracefulShutdown);
}

__attribute__((destructor))
static void OnLibraryUnload()
{
    PerformGracefulShutdown();
}

// ── X11 Event Filtering (LD_PRELOAD) ─────────────────────────────────────────
static std::atomic<uint32_t> g_DbgXNext{0}, g_DbgXChkIf{0}, g_DbgXChkMask{0}, g_DbgXChkWin{0}, g_DbgXChkTyp{0};
static std::atomic<uint32_t> g_DbgXGetEvData{0}, g_DbgXQPtr{0}, g_DbgXWarp{0}, g_DbgXGrab{0}, g_DbgXIGrab{0}, g_DbgXcb{0}, g_DbgXPeek{0};
static std::atomic<uint64_t> g_DbgLastX11LogMs{0};

static void MaybeLogX11Dbg()
{
    return;
    if (!Overlay_IsMenuOpen()) return;
    uint64_t now = Platform_GetTickMs();
    uint64_t prev = g_DbgLastX11LogMs.load(std::memory_order_relaxed);
    if (now - prev >= 1000 && g_DbgLastX11LogMs.compare_exchange_strong(prev, now))
    {
        Log("[x11dbg] hooks/1s while menu OPEN: next=%u chkIf=%u chkMask=%u chkWin=%u chkTyp=%u peek=%u getEvData=%u qPtr=%u warp=%u grab=%u xiGrab=%u xcb=%u",
            g_DbgXNext.exchange(0), g_DbgXChkIf.exchange(0), g_DbgXChkMask.exchange(0),
            g_DbgXChkWin.exchange(0), g_DbgXChkTyp.exchange(0), g_DbgXPeek.exchange(0),
            g_DbgXGetEvData.exchange(0), g_DbgXQPtr.exchange(0), g_DbgXWarp.exchange(0),
            g_DbgXGrab.exchange(0), g_DbgXIGrab.exchange(0), g_DbgXcb.exchange(0));
    }
}

static std::atomic<bool> g_MenuUngrabSent{ false };

static void MaybeUngrabGamePointerOnce(Display* dpy)
{
    if (!dpy) return;
    if (Overlay_IsMenuOpen())
    {
        if (!g_MenuUngrabSent.exchange(true, std::memory_order_relaxed))
        {
            using PFN_UG    = int (*)(Display*, Time);
            using PFN_XIUG  = int (*)(Display*, int, Time);
            using PFN_Flush = int (*)(Display*);
            static auto real_ug    = reinterpret_cast<PFN_UG>(dlsym(RTLD_NEXT, "XUngrabPointer"));
            static auto real_flush = reinterpret_cast<PFN_Flush>(dlsym(RTLD_NEXT, "XFlush"));
            static void* libXi     = dlopen("libXi.so.6", RTLD_LAZY | RTLD_NOLOAD);
            static auto real_xiug  = libXi ? reinterpret_cast<PFN_XIUG>(dlsym(libXi, "XIUngrabDevice")) : nullptr;
            if (real_ug) real_ug(dpy, CurrentTime);
            if (real_xiug) real_xiug(dpy, 2, CurrentTime);
            if (real_flush) real_flush(dpy);
            Log("[x11dbg] Released game pointer grab on dpy=%p (xi=%d)", static_cast<void*>(dpy), real_xiug ? 1 : 0);
        }
    }
    else
    {
        g_MenuUngrabSent.store(false, std::memory_order_relaxed);
    }
}

static bool HandleX11Event(XEvent* ev)
{
    if (!ev || IsCaptureDisabledByEnv()) return false;

    if (ev->type == KeyPress)
    {
        KeySym ks = XLookupKeysym(&ev->xkey, 0);
        Overlay_OnKeyEvent(static_cast<int>(ks), true);
        if (Overlay_IsMenuOpen()) return true;
    }
    else if (ev->type == KeyRelease)
    {
        if (Overlay_IsMenuOpen()) return true;
    }
    else if (ev->type == MotionNotify)
    {
        Overlay_OnMouseMove(static_cast<float>(ev->xmotion.x), static_cast<float>(ev->xmotion.y));
        if (Overlay_IsMenuOpen()) return true;
    }
    else if (ev->type == ButtonPress || ev->type == ButtonRelease)
    {
        bool pressed = (ev->type == ButtonPress);
        unsigned int btn = ev->xbutton.button;
        if (Overlay_IsMenuOpen())
        {
            if (btn == Button4 && pressed)      Overlay_AddMouseWheel(0.0f,  1.0f);
            else if (btn == Button5 && pressed) Overlay_AddMouseWheel(0.0f, -1.0f);
            else if (btn == 6 && pressed)       Overlay_AddMouseWheel(-1.0f, 0.0f);
            else if (btn == 7 && pressed)       Overlay_AddMouseWheel( 1.0f, 0.0f);
            else if (btn >= Button1 && btn <= Button3)
                Overlay_OnMouseButton(static_cast<int>(btn - Button1), pressed);
            return true;
        }
    }
    else if (ev->type == GenericEvent && Overlay_IsMenuOpen())
    {
        return true;
    }
    return false;
}

extern "C" int XNextEvent(Display* display, XEvent* event_return)
{
    using PFN_XNextEvent = int (*)(Display*, XEvent*);
    using PFN_XPending   = int (*)(Display*);
    static auto real_fn      = reinterpret_cast<PFN_XNextEvent>(dlsym(RTLD_NEXT, "XNextEvent"));
    static auto real_pending = reinterpret_cast<PFN_XPending>(dlsym(RTLD_NEXT, "XPending"));
    if (!real_fn) return 0;
    if (IsCaptureDisabledByEnv()) return real_fn(display, event_return);
    MaybeUngrabGamePointerOnce(display);

    for (;;)
    {
        int r = real_fn(display, event_return);
        if (Overlay_IsMenuOpen()) { g_DbgXNext.fetch_add(1, std::memory_order_relaxed); MaybeLogX11Dbg(); }
        if (!HandleX11Event(event_return))
            return r;
        if (!real_pending || real_pending(display) <= 0)
        {
            std::memset(event_return, 0, sizeof(*event_return));
            event_return->type = NoExpose;
            return 0;
        }
    }
}



static bool IsMouseOrRawEvent(const XEvent* ev)
{
    if (!ev) return false;
    return ev->type == MotionNotify || ev->type == ButtonPress ||
           ev->type == ButtonRelease || ev->type == GenericEvent;
}

extern "C" Bool XCheckIfEvent(Display* dpy, XEvent* ev,
                               Bool (*predicate)(Display*, XEvent*, XPointer), XPointer arg)
{
    using PFN = Bool (*)(Display*, XEvent*, Bool (*)(Display*, XEvent*, XPointer), XPointer);
    static auto real_fn = reinterpret_cast<PFN>(dlsym(RTLD_NEXT, "XCheckIfEvent"));
    if (!real_fn) return False;
    if (IsCaptureDisabledByEnv()) return real_fn(dpy, ev, predicate, arg);
    MaybeUngrabGamePointerOnce(dpy);

    while (real_fn(dpy, ev, predicate, arg))
    {
        if (Overlay_IsMenuOpen()) { g_DbgXChkIf.fetch_add(1, std::memory_order_relaxed); MaybeLogX11Dbg(); }
        if (HandleX11Event(ev) || (Overlay_IsMenuOpen() && IsMouseOrRawEvent(ev)))
            continue;
        return True;
    }
    return False;
}

extern "C" Bool XCheckMaskEvent(Display* dpy, long event_mask, XEvent* ev)
{
    using PFN = Bool (*)(Display*, long, XEvent*);
    static auto real_fn = reinterpret_cast<PFN>(dlsym(RTLD_NEXT, "XCheckMaskEvent"));
    if (!real_fn) return False;
    if (IsCaptureDisabledByEnv()) return real_fn(dpy, event_mask, ev);

    while (real_fn(dpy, event_mask, ev))
    {
        if (Overlay_IsMenuOpen()) { g_DbgXChkMask.fetch_add(1, std::memory_order_relaxed); MaybeLogX11Dbg(); }
        if (HandleX11Event(ev) || (Overlay_IsMenuOpen() && IsMouseOrRawEvent(ev)))
            continue;
        return True;
    }
    return False;
}

extern "C" Bool XCheckWindowEvent(Display* dpy, Window w, long event_mask, XEvent* ev)
{
    using PFN = Bool (*)(Display*, Window, long, XEvent*);
    static auto real_fn = reinterpret_cast<PFN>(dlsym(RTLD_NEXT, "XCheckWindowEvent"));
    if (!real_fn) return False;
    if (IsCaptureDisabledByEnv()) return real_fn(dpy, w, event_mask, ev);

    while (real_fn(dpy, w, event_mask, ev))
    {
        if (Overlay_IsMenuOpen()) { g_DbgXChkWin.fetch_add(1, std::memory_order_relaxed); MaybeLogX11Dbg(); }
        if (HandleX11Event(ev) || (Overlay_IsMenuOpen() && IsMouseOrRawEvent(ev)))
            continue;
        return True;
    }
    return False;
}

extern "C" Bool XCheckTypedEvent(Display* dpy, int event_type, XEvent* ev)
{
    using PFN = Bool (*)(Display*, int, XEvent*);
    static auto real_fn = reinterpret_cast<PFN>(dlsym(RTLD_NEXT, "XCheckTypedEvent"));
    if (!real_fn) return False;
    if (IsCaptureDisabledByEnv()) return real_fn(dpy, event_type, ev);

    while (real_fn(dpy, event_type, ev))
    {
        if (Overlay_IsMenuOpen()) { g_DbgXChkTyp.fetch_add(1, std::memory_order_relaxed); MaybeLogX11Dbg(); }
        if (HandleX11Event(ev) || (Overlay_IsMenuOpen() && IsMouseOrRawEvent(ev)))
            continue;
        return True;
    }
    return False;
}

extern "C" Bool XCheckTypedWindowEvent(Display* dpy, Window w, int event_type, XEvent* ev)
{
    using PFN = Bool (*)(Display*, Window, int, XEvent*);
    static auto real_fn = reinterpret_cast<PFN>(dlsym(RTLD_NEXT, "XCheckTypedWindowEvent"));
    if (!real_fn) return False;
    if (IsCaptureDisabledByEnv()) return real_fn(dpy, w, event_type, ev);

    while (real_fn(dpy, w, event_type, ev))
    {
        if (HandleX11Event(ev) || (Overlay_IsMenuOpen() && IsMouseOrRawEvent(ev)))
            continue;
        return True;
    }
    return False;
}

extern "C" int XIGrabDevice(Display* dpy, int deviceid, Window grab_window, Time time,
                            Cursor cursor, int grab_mode, int paired_device_mode,
                            Bool owner_events, void* mask)
{
    using PFN = int (*)(Display*, int, Window, Time, Cursor, int, int, Bool, void*);
    static auto real_fn = reinterpret_cast<PFN>(dlsym(RTLD_NEXT, "XIGrabDevice"));
    if (!IsCaptureDisabledByEnv() && Overlay_IsMenuOpen()) { g_DbgXIGrab.fetch_add(1, std::memory_order_relaxed); MaybeLogX11Dbg(); return GrabSuccess; }
    return real_fn ? real_fn(dpy, deviceid, grab_window, time, cursor, grab_mode, paired_device_mode, owner_events, mask) : 0;
}

extern "C" int XIfEvent(Display* dpy, XEvent* ev,
                        Bool (*predicate)(Display*, XEvent*, XPointer), XPointer arg)
{
    using PFN = int (*)(Display*, XEvent*, Bool (*)(Display*, XEvent*, XPointer), XPointer);
    static auto real_fn = reinterpret_cast<PFN>(dlsym(RTLD_NEXT, "XIfEvent"));
    if (!real_fn) return 0;
    if (IsCaptureDisabledByEnv()) return real_fn(dpy, ev, predicate, arg);

    for (;;)
    {
        int r = real_fn(dpy, ev, predicate, arg);
        if (HandleX11Event(ev) || (Overlay_IsMenuOpen() && IsMouseOrRawEvent(ev)))
            continue;
        return r;
    }
}

extern "C" int XWindowEvent(Display* dpy, Window w, long event_mask, XEvent* ev)
{
    using PFN = int (*)(Display*, Window, long, XEvent*);
    static auto real_fn = reinterpret_cast<PFN>(dlsym(RTLD_NEXT, "XWindowEvent"));
    if (!real_fn) return 0;
    if (IsCaptureDisabledByEnv()) return real_fn(dpy, w, event_mask, ev);

    for (;;)
    {
        int r = real_fn(dpy, w, event_mask, ev);
        if (HandleX11Event(ev) || (Overlay_IsMenuOpen() && IsMouseOrRawEvent(ev)))
            continue;
        return r;
    }
}

extern "C" int XMaskEvent(Display* dpy, long event_mask, XEvent* ev)
{
    using PFN = int (*)(Display*, long, XEvent*);
    static auto real_fn = reinterpret_cast<PFN>(dlsym(RTLD_NEXT, "XMaskEvent"));
    if (!real_fn) return 0;
    if (IsCaptureDisabledByEnv()) return real_fn(dpy, event_mask, ev);

    for (;;)
    {
        int r = real_fn(dpy, event_mask, ev);
        if (HandleX11Event(ev) || (Overlay_IsMenuOpen() && IsMouseOrRawEvent(ev)))
            continue;
        return r;
    }
}

extern "C" int XPeekEvent(Display* dpy, XEvent* ev)
{
    using PFN_Peek    = int (*)(Display*, XEvent*);
    using PFN_Next    = int (*)(Display*, XEvent*);
    using PFN_Pending = int (*)(Display*);
    static auto real_peek    = reinterpret_cast<PFN_Peek>(dlsym(RTLD_NEXT, "XPeekEvent"));
    static auto real_next    = reinterpret_cast<PFN_Next>(dlsym(RTLD_NEXT, "XNextEvent"));
    static auto real_pending = reinterpret_cast<PFN_Pending>(dlsym(RTLD_NEXT, "XPending"));
    if (!real_peek) return 0;
    if (IsCaptureDisabledByEnv()) return real_peek(dpy, ev);

    for (;;)
    {
        int r = real_peek(dpy, ev);
        if (!Overlay_IsMenuOpen() || !(IsMouseOrRawEvent(ev) || ev->type == KeyPress || ev->type == KeyRelease))
            return r;
        if (real_next)
        {
            XEvent dropped = {};
            real_next(dpy, &dropped);
            HandleX11Event(&dropped);
        }
        if (!real_pending || real_pending(dpy) <= 0)
        {
            std::memset(ev, 0, sizeof(*ev));
            ev->type = NoExpose;
            return 0;
        }
    }
}

struct xcb_connection_t;
struct xcb_generic_event_t { uint8_t response_type; uint8_t pad0; uint16_t sequence; uint32_t pad[7]; uint32_t full_sequence; };

static bool IsXcbBlockedWhenMenuOpen(const xcb_generic_event_t* ev)
{
    if (!ev) return false;
    uint8_t t = ev->response_type & 0x7Fu;
    // 2=KeyPress, 3=KeyRelease, 4=ButtonPress, 5=ButtonRelease, 6=MotionNotify, 35=GE_GENERIC (XI2 RawMotion)
    return t == 2 || t == 3 || t == 4 || t == 5 || t == 6 || t == 35;
}

extern "C" xcb_generic_event_t* xcb_poll_for_event(xcb_connection_t* c)
{
    using PFN = xcb_generic_event_t* (*)(xcb_connection_t*);
    static auto real_fn = reinterpret_cast<PFN>(dlsym(RTLD_NEXT, "xcb_poll_for_event"));
    if (!real_fn) return nullptr;
    if (IsCaptureDisabledByEnv()) return real_fn(c);

    while (xcb_generic_event_t* ev = real_fn(c))
    {
        if (Overlay_IsMenuOpen() && IsXcbBlockedWhenMenuOpen(ev))
        {
            g_DbgXcb.fetch_add(1, std::memory_order_relaxed); MaybeLogX11Dbg();
            std::free(ev);
            continue;
        }
        return ev;
    }
    return nullptr;
}

extern "C" xcb_generic_event_t* xcb_poll_for_queued_event(xcb_connection_t* c)
{
    using PFN = xcb_generic_event_t* (*)(xcb_connection_t*);
    static auto real_fn = reinterpret_cast<PFN>(dlsym(RTLD_NEXT, "xcb_poll_for_queued_event"));
    if (!real_fn) return nullptr;
    if (IsCaptureDisabledByEnv()) return real_fn(c);

    while (xcb_generic_event_t* ev = real_fn(c))
    {
        if (Overlay_IsMenuOpen() && IsXcbBlockedWhenMenuOpen(ev))
        {
            std::free(ev);
            continue;
        }
        return ev;
    }
    return nullptr;
}

extern "C" xcb_generic_event_t* xcb_wait_for_event(xcb_connection_t* c)
{
    using PFN = xcb_generic_event_t* (*)(xcb_connection_t*);
    static auto real_fn = reinterpret_cast<PFN>(dlsym(RTLD_NEXT, "xcb_wait_for_event"));
    if (!real_fn) return nullptr;
    if (IsCaptureDisabledByEnv()) return real_fn(c);

    for (;;)
    {
        xcb_generic_event_t* ev = real_fn(c);
        if (!ev) return nullptr;
        if (Overlay_IsMenuOpen() && IsXcbBlockedWhenMenuOpen(ev))
        {
            g_DbgXcb.fetch_add(1, std::memory_order_relaxed);
            std::free(ev);
            continue;
        }
        return ev;
    }
}

extern "C" Bool XIQueryPointer(Display* dpy, int deviceid, Window win,
                                Window* root_ret, Window* child_ret,
                                double* root_x, double* root_y,
                                double* win_x, double* win_y,
                                void* buttons_ret, void* mods_ret, void* group_ret)
{
    using PFN = Bool (*)(Display*, int, Window, Window*, Window*, double*, double*, double*, double*, void*, void*, void*);
    static auto real_fn = reinterpret_cast<PFN>(dlsym(RTLD_NEXT, "XIQueryPointer"));
    if (!real_fn) return False;
    if (IsCaptureDisabledByEnv())
        return real_fn(dpy, deviceid, win, root_ret, child_ret, root_x, root_y, win_x, win_y, buttons_ret, mods_ret, group_ret);

    Bool ok = real_fn(dpy, deviceid, win, root_ret, child_ret, root_x, root_y, win_x, win_y, buttons_ret, mods_ret, group_ret);
    static double s_LastRx = 0.0, s_LastRy = 0.0, s_LastWx = 0.0, s_LastWy = 0.0;
    if (ok)
    {
        if (Overlay_IsMenuOpen())
        {
            if (root_x) *root_x = s_LastRx;
            if (root_y) *root_y = s_LastRy;
            if (win_x)  *win_x  = s_LastWx;
            if (win_y)  *win_y  = s_LastWy;
        }
        else
        {
            if (root_x) s_LastRx = *root_x;
            if (root_y) s_LastRy = *root_y;
            if (win_x)  s_LastWx = *win_x;
            if (win_y)  s_LastWy = *win_y;
        }
    }
    return ok;
}

extern "C" Bool XGetEventData(Display* dpy, XGenericEventCookie* cookie)
{
    using PFN      = Bool (*)(Display*, XGenericEventCookie*);
    using PFN_Free = void (*)(Display*, XGenericEventCookie*);
    static auto real_fn   = reinterpret_cast<PFN>(dlsym(RTLD_NEXT, "XGetEventData"));
    static auto real_free = reinterpret_cast<PFN_Free>(dlsym(RTLD_NEXT, "XFreeEventData"));
    if (!real_fn) return False;
    if (IsCaptureDisabledByEnv()) return real_fn(dpy, cookie);
    Bool ok = real_fn(dpy, cookie);
    if (ok && Overlay_IsMenuOpen())
    {
        g_DbgXGetEvData.fetch_add(1, std::memory_order_relaxed); MaybeLogX11Dbg();
        if (real_free) real_free(dpy, cookie);
        return False;
    }
    return ok;
}

extern "C" int XWarpPointer(Display* dpy, Window src_w, Window dest_w,
                            int src_x, int src_y, unsigned int src_width, unsigned int src_height,
                            int dest_x, int dest_y)
{
    using PFN = int (*)(Display*, Window, Window, int, int, unsigned int, unsigned int, int, int);
    static auto real_fn = reinterpret_cast<PFN>(dlsym(RTLD_NEXT, "XWarpPointer"));
    if (!IsCaptureDisabledByEnv() && Overlay_IsMenuOpen()) { g_DbgXWarp.fetch_add(1, std::memory_order_relaxed); MaybeLogX11Dbg(); return 0; }
    return real_fn ? real_fn(dpy, src_w, dest_w, src_x, src_y, src_width, src_height, dest_x, dest_y) : 0;
}


extern "C" Bool XQueryPointer(Display* dpy, Window w, Window* root_return, Window* child_return,
                               int* root_x_return, int* root_y_return,
                               int* win_x_return, int* win_y_return, unsigned int* mask_return)
{
    using PFN = Bool (*)(Display*, Window, Window*, Window*, int*, int*, int*, int*, unsigned int*);
    static auto real_fn = reinterpret_cast<PFN>(dlsym(RTLD_NEXT, "XQueryPointer"));
    if (!real_fn) return False;
    if (IsCaptureDisabledByEnv()) return real_fn(dpy, w, root_return, child_return, root_x_return, root_y_return, win_x_return, win_y_return, mask_return);
    Bool ok = real_fn(dpy, w, root_return, child_return, root_x_return, root_y_return, win_x_return, win_y_return, mask_return);
    static int s_LastRootX = 0, s_LastRootY = 0, s_LastWinX = 0, s_LastWinY = 0;
    if (ok)
    {
        if (Overlay_IsMenuOpen())
        {
            g_DbgXQPtr.fetch_add(1, std::memory_order_relaxed); MaybeLogX11Dbg();
            if (root_x_return) *root_x_return = s_LastRootX;
            if (root_y_return) *root_y_return = s_LastRootY;
            if (win_x_return)  *win_x_return  = s_LastWinX;
            if (win_y_return)  *win_y_return  = s_LastWinY;
            if (mask_return)   *mask_return   = 0;
        }
        else
        {
            if (root_x_return) s_LastRootX = *root_x_return;
            if (root_y_return) s_LastRootY = *root_y_return;
            if (win_x_return)  s_LastWinX  = *win_x_return;
            if (win_y_return)  s_LastWinY  = *win_y_return;
        }
    }
    return ok;
}


extern "C" int XGrabPointer(Display* dpy, Window grab_window, Bool owner_events,
                            unsigned int event_mask, int pointer_mode, int keyboard_mode,
                            Window confine_to, Cursor cursor, Time time)
{
    using PFN = int (*)(Display*, Window, Bool, unsigned int, int, int, Window, Cursor, Time);
    static auto real_fn = reinterpret_cast<PFN>(dlsym(RTLD_NEXT, "XGrabPointer"));
    if (!IsCaptureDisabledByEnv() && Overlay_IsMenuOpen()) { g_DbgXGrab.fetch_add(1, std::memory_order_relaxed); MaybeLogX11Dbg(); return GrabSuccess; }
    return real_fn ? real_fn(dpy, grab_window, owner_events, event_mask, pointer_mode, keyboard_mode, confine_to, cursor, time) : 0;
}



extern "C" int XEventsQueued(Display* dpy, int mode)
{
    using PFN = int (*)(Display*, int);
    static auto real_fn = reinterpret_cast<PFN>(dlsym(RTLD_NEXT, "XEventsQueued"));
    if (!IsCaptureDisabledByEnv()) MaybeUngrabGamePointerOnce(dpy);
    return real_fn ? real_fn(dpy, mode) : 0;
}

extern "C" int XPending(Display* dpy)
{
    using PFN = int (*)(Display*);
    static auto real_fn = reinterpret_cast<PFN>(dlsym(RTLD_NEXT, "XPending"));
    if (!IsCaptureDisabledByEnv()) MaybeUngrabGamePointerOnce(dpy);
    return real_fn ? real_fn(dpy) : 0;
}

// ── Wayland Input Interposition (LD_PRELOAD, no libwayland-dev needed) ───────
#include <memory>
#include <linux/input-event-codes.h>

struct wl_proxy;
struct wl_array;
struct wl_surface;
using wl_fixed_t = int32_t;

struct WaylandProxyHook
{
    void* origImpl[12]   = {};
    void* hookedImpl[12] = {};
    void* origData       = nullptr;
};

static std::mutex g_WlMtx;
static std::unordered_map<void*, std::unique_ptr<WaylandProxyHook>> g_WlHooks;

static int EvdevToKeySym(uint32_t code)
{
    switch (code)
    {
    case KEY_INSERT:     return XK_Insert;
    case KEY_HOME:       return XK_Home;
    case KEY_END:        return XK_End;
    case KEY_GRAVE:      return XK_grave;
    case KEY_F8:         return XK_F8;
    case KEY_F9:         return XK_F9;
    case KEY_F10:        return XK_F10;
    case KEY_F11:        return XK_F11;
    case KEY_F12:        return XK_F12;
    case KEY_SCROLLLOCK: return XK_Scroll_Lock;
    case KEY_PAUSE:      return XK_Pause;
    default:             return 0;
    }
}

static void Hook_wl_keyboard_key(void* data, void* kb, uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
    using PFN_key = void (*)(void*, void*, uint32_t, uint32_t, uint32_t, uint32_t);
    PFN_key orig = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_WlMtx);
        auto it = g_WlHooks.find(kb);
        if (it != g_WlHooks.end()) orig = reinterpret_cast<PFN_key>(it->second->origImpl[3]);
    }

    Overlay_SetWaylandActive();
    int ks = EvdevToKeySym(key);
    if (ks != 0) Overlay_OnKeyEvent(ks, state == 1);

    if (Overlay_IsMenuOpen()) return;
    if (orig) orig(data, kb, serial, time, key, state);
}

static void Hook_wl_pointer_motion(void* data, void* ptr, uint32_t time, wl_fixed_t sx, wl_fixed_t sy)
{
    using PFN_motion = void (*)(void*, void*, uint32_t, wl_fixed_t, wl_fixed_t);
    PFN_motion orig = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_WlMtx);
        auto it = g_WlHooks.find(ptr);
        if (it != g_WlHooks.end()) orig = reinterpret_cast<PFN_motion>(it->second->origImpl[2]);
    }

    Overlay_SetWaylandActive();
    Overlay_OnMouseMove(static_cast<float>(sx) / 256.0f, static_cast<float>(sy) / 256.0f);
    if (Overlay_IsMenuOpen()) return;
    if (orig) orig(data, ptr, time, sx, sy);
}

static void Hook_wl_pointer_button(void* data, void* ptr, uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
    using PFN_button = void (*)(void*, void*, uint32_t, uint32_t, uint32_t, uint32_t);
    PFN_button orig = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_WlMtx);
        auto it = g_WlHooks.find(ptr);
        if (it != g_WlHooks.end()) orig = reinterpret_cast<PFN_button>(it->second->origImpl[3]);
    }

    if (Overlay_IsMenuOpen())
    {
        int btn = -1;
        if (button == BTN_LEFT)        btn = 0;
        else if (button == BTN_RIGHT)  btn = 1;
        else if (button == BTN_MIDDLE) btn = 2;
        if (btn >= 0) Overlay_OnMouseButton(btn, state == 1);
        return;
    }
    if (orig) orig(data, ptr, serial, time, button, state);
}

static void Hook_wl_pointer_axis(void* data, void* ptr, uint32_t time, uint32_t axis, wl_fixed_t value)
{
    using PFN_axis = void (*)(void*, void*, uint32_t, uint32_t, wl_fixed_t);
    PFN_axis orig = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_WlMtx);
        auto it = g_WlHooks.find(ptr);
        if (it != g_WlHooks.end()) orig = reinterpret_cast<PFN_axis>(it->second->origImpl[4]);
    }

    if (Overlay_IsMenuOpen())
    {
        float v = static_cast<float>(value) / 256.0f / 15.0f;
        if (axis == 0) Overlay_AddMouseWheel(0.0f, -v);
        else           Overlay_AddMouseWheel(v, 0.0f);
        return;
    }
    if (orig) orig(data, ptr, time, axis, value);
}


static void Hook_zwp_relative_pointer_motion(void* data, void* relPtr,
                                             uint32_t utime_hi, uint32_t utime_lo,
                                             wl_fixed_t dx, wl_fixed_t dy,
                                             wl_fixed_t dx_unaccel, wl_fixed_t dy_unaccel)
{
    using PFN_rel = void (*)(void*, void*, uint32_t, uint32_t, wl_fixed_t, wl_fixed_t, wl_fixed_t, wl_fixed_t);
    PFN_rel orig = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_WlMtx);
        auto it = g_WlHooks.find(relPtr);
        if (it != g_WlHooks.end()) orig = reinterpret_cast<PFN_rel>(it->second->origImpl[0]);
    }

    Overlay_SetWaylandActive();
    if (Overlay_IsMenuOpen())
    {
        Overlay_AddMouseDelta(static_cast<float>(dx) / 256.0f, static_cast<float>(dy) / 256.0f);
        return;
    }
    if (orig) orig(data, relPtr, utime_hi, utime_lo, dx, dy, dx_unaccel, dy_unaccel);
}

extern "C" int wl_proxy_add_listener(struct wl_proxy* proxy, void (**implementation)(void), void* data)
{
    using PFN_add = int (*)(struct wl_proxy*, void (**)(void), void*);
    using PFN_cls = const char* (*)(struct wl_proxy*);
    static void* libWl = dlopen("libwayland-client.so.0", RTLD_LAZY | RTLD_NOLOAD);
    if (!libWl) libWl = dlopen("libwayland-client.so.0", RTLD_LAZY);
    static auto real_add = libWl ? reinterpret_cast<PFN_add>(dlsym(libWl, "wl_proxy_add_listener")) : nullptr;
    static auto real_cls = libWl ? reinterpret_cast<PFN_cls>(dlsym(libWl, "wl_proxy_get_class")) : nullptr;
    if (!real_add) return -1;
    if (IsCaptureDisabledByEnv()) return real_add(proxy, implementation, data);

    const char* cls = (proxy && real_cls) ? real_cls(proxy) : nullptr;
    if (cls && implementation)
    {
        if (std::strcmp(cls, "wl_keyboard") == 0)
        {
            auto h = std::make_unique<WaylandProxyHook>();
            std::memcpy(h->origImpl, implementation, sizeof(void*) * 6);
            std::memcpy(h->hookedImpl, implementation, sizeof(void*) * 6);
            h->origData = data;
            h->hookedImpl[3] = reinterpret_cast<void*>(Hook_wl_keyboard_key);
            void (**hookPtr)(void) = reinterpret_cast<void (**)(void)>(h->hookedImpl);
            {
                std::lock_guard<std::mutex> lk(g_WlMtx);
                g_WlHooks[proxy] = std::move(h);
            }
            return real_add(proxy, hookPtr, data);
        }
        else if (std::strcmp(cls, "zwp_relative_pointer_v1") == 0)
        {
            auto h = std::make_unique<WaylandProxyHook>();
            h->origImpl[0]   = reinterpret_cast<void*>(implementation[0]);
            h->hookedImpl[0] = reinterpret_cast<void*>(Hook_zwp_relative_pointer_motion);
            h->origData      = data;
            void (**hookPtr)(void) = reinterpret_cast<void (**)(void)>(h->hookedImpl);
            {
                std::lock_guard<std::mutex> lk(g_WlMtx);
                g_WlHooks[proxy] = std::move(h);
            }
            return real_add(proxy, hookPtr, data);
        }
        else if (std::strcmp(cls, "wl_pointer") == 0)
        {
            auto h = std::make_unique<WaylandProxyHook>();
            std::memcpy(h->origImpl, implementation, sizeof(void*) * 11);
            std::memcpy(h->hookedImpl, implementation, sizeof(void*) * 11);
            h->origData = data;
            h->hookedImpl[2] = reinterpret_cast<void*>(Hook_wl_pointer_motion);
            h->hookedImpl[3] = reinterpret_cast<void*>(Hook_wl_pointer_button);
            h->hookedImpl[4] = reinterpret_cast<void*>(Hook_wl_pointer_axis);
            void (**hookPtr)(void) = reinterpret_cast<void (**)(void)>(h->hookedImpl);
            {
                std::lock_guard<std::mutex> lk(g_WlMtx);
                g_WlHooks[proxy] = std::move(h);
            }
            return real_add(proxy, hookPtr, data);
        }
    }
    return real_add(proxy, implementation, data);
}

// ── Vulkan Layer Hooks ───────────────────────────────────────────────────────
static VKAPI_ATTR VkResult VKAPI_CALL Hook_vkCreateXlibSurfaceKHR(
    VkInstance instance,
    const VkXlibSurfaceCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSurfaceKHR* pSurface)
{
    InstanceDispatch id = {};
    {
        std::lock_guard<std::mutex> lk(g_DispatchMtx);
        auto it = g_InstanceMap.find(instance);
        if (it != g_InstanceMap.end()) id = it->second;
    }
    if (!id.CreateXlibSurfaceKHR) return VK_ERROR_INITIALIZATION_FAILED;
    if (pCreateInfo)
    {
        OverlaySurfaceInfo osi;
        osi.x11Display = pCreateInfo->dpy;
        osi.x11Window  = pCreateInfo->window;
        Overlay_SetSurfaceInfo(osi);
    }
    return id.CreateXlibSurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);
}

static VKAPI_ATTR VkResult VKAPI_CALL Hook_vkEnumeratePhysicalDeviceGroups(
    VkInstance instance,
    uint32_t* pPhysicalDeviceGroupCount,
    VkPhysicalDeviceGroupProperties* pPhysicalDeviceGroupProperties)
{
    InstanceDispatch id = {};
    {
        std::lock_guard<std::mutex> lk(g_DispatchMtx);
        id = g_InstanceMap[instance];
    }
    if (!id.EnumeratePhysicalDeviceGroups) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult res = id.EnumeratePhysicalDeviceGroups(instance, pPhysicalDeviceGroupCount, pPhysicalDeviceGroupProperties);
    if ((res == VK_SUCCESS || res == VK_INCOMPLETE) && pPhysicalDeviceGroupProperties && pPhysicalDeviceGroupCount)
    {
        std::lock_guard<std::mutex> lk(g_DispatchMtx);
        for (uint32_t g = 0; g < *pPhysicalDeviceGroupCount; ++g)
            for (uint32_t i = 0; i < pPhysicalDeviceGroupProperties[g].physicalDeviceCount; ++i)
                g_PhysDevToInstance[pPhysicalDeviceGroupProperties[g].physicalDevices[i]] = instance;
    }
    return res;
}

static VKAPI_ATTR VkResult VKAPI_CALL Hook_vkEnumeratePhysicalDevices(
    VkInstance instance,
    uint32_t* pPhysicalDeviceCount,
    VkPhysicalDevice* pPhysicalDevices)
{
    InstanceDispatch id = {};
    {
        std::lock_guard<std::mutex> lk(g_DispatchMtx);
        id = g_InstanceMap[instance];
    }
    if (!id.EnumeratePhysicalDevices) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult res = id.EnumeratePhysicalDevices(instance, pPhysicalDeviceCount, pPhysicalDevices);
    if ((res == VK_SUCCESS || res == VK_INCOMPLETE) && pPhysicalDevices && pPhysicalDeviceCount)
    {
        std::lock_guard<std::mutex> lk(g_DispatchMtx);
        for (uint32_t i = 0; i < *pPhysicalDeviceCount; ++i)
            g_PhysDevToInstance[pPhysicalDevices[i]] = instance;
    }
    return res;
}

static VKAPI_ATTR VkResult VKAPI_CALL Hook_vkCreateInstance(
    const VkInstanceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkInstance* pInstance)
{

    VkLayerInstanceCreateInfo* chainInfo = const_cast<VkLayerInstanceCreateInfo*>(
        reinterpret_cast<const VkLayerInstanceCreateInfo*>(pCreateInfo->pNext));
    while (chainInfo && (chainInfo->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO ||
                         chainInfo->function != VK_LAYER_LINK_INFO))
    {
        chainInfo = const_cast<VkLayerInstanceCreateInfo*>(
            reinterpret_cast<const VkLayerInstanceCreateInfo*>(chainInfo->pNext));
    }
    if (!chainInfo || !chainInfo->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr fpGetInstanceProcAddr = chainInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    chainInfo->u.pLayerInfo = chainInfo->u.pLayerInfo->pNext;

    auto fpCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(fpGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!fpCreateInstance) return VK_ERROR_INITIALIZATION_FAILED;

    VkResult res = fpCreateInstance(pCreateInfo, pAllocator, pInstance);
    if (res != VK_SUCCESS) return res;

    InstanceDispatch id = {};
    id.instance = *pInstance;
    id.GetInstanceProcAddr = fpGetInstanceProcAddr;
    id.DestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(fpGetInstanceProcAddr(*pInstance, "vkDestroyInstance"));
    id.EnumeratePhysicalDevices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(fpGetInstanceProcAddr(*pInstance, "vkEnumeratePhysicalDevices"));
    id.EnumeratePhysicalDeviceGroups = reinterpret_cast<PFN_vkEnumeratePhysicalDeviceGroups>(fpGetInstanceProcAddr(*pInstance, "vkEnumeratePhysicalDeviceGroups"));
    if (!id.EnumeratePhysicalDeviceGroups)
        id.EnumeratePhysicalDeviceGroups = reinterpret_cast<PFN_vkEnumeratePhysicalDeviceGroups>(fpGetInstanceProcAddr(*pInstance, "vkEnumeratePhysicalDeviceGroupsKHR"));
    id.GetPhysicalDeviceProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(fpGetInstanceProcAddr(*pInstance, "vkGetPhysicalDeviceProperties"));
    id.GetPhysicalDeviceMemoryProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(fpGetInstanceProcAddr(*pInstance, "vkGetPhysicalDeviceMemoryProperties"));
    id.GetPhysicalDeviceQueueFamilyProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(fpGetInstanceProcAddr(*pInstance, "vkGetPhysicalDeviceQueueFamilyProperties"));
    id.GetPhysicalDeviceSurfaceCapabilitiesKHR = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(fpGetInstanceProcAddr(*pInstance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));
    id.CreateXlibSurfaceKHR = reinterpret_cast<PFN_vkCreateXlibSurfaceKHR>(fpGetInstanceProcAddr(*pInstance, "vkCreateXlibSurfaceKHR"));

    std::lock_guard<std::mutex> lk(g_DispatchMtx);
    g_InstanceMap[*pInstance] = id;
    if (!IsCaptureDisabledByEnv())
        Log("[layer] vkCreateInstance hooked (inst=%p)", static_cast<void*>(*pInstance));
    return res;
}

static VKAPI_ATTR void VKAPI_CALL Hook_vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator)
{
    InstanceDispatch id = {};
    {
        std::lock_guard<std::mutex> lk(g_DispatchMtx);
        auto it = g_InstanceMap.find(instance);
        if (it != g_InstanceMap.end()) { id = it->second; g_InstanceMap.erase(it); }
        for (auto pit = g_PhysDevToInstance.begin(); pit != g_PhysDevToInstance.end(); )
        {
            if (pit->second == instance) pit = g_PhysDevToInstance.erase(pit);
            else ++pit;
        }
    }
    if (id.DestroyInstance) id.DestroyInstance(instance, pAllocator);
}

static VKAPI_ATTR VkResult VKAPI_CALL Hook_vkCreateDevice(
    VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDevice* pDevice)
{
    VkLayerDeviceCreateInfo* chainInfo = const_cast<VkLayerDeviceCreateInfo*>(
        reinterpret_cast<const VkLayerDeviceCreateInfo*>(pCreateInfo->pNext));
    while (chainInfo && (chainInfo->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO ||
                         chainInfo->function != VK_LAYER_LINK_INFO))
    {
        chainInfo = const_cast<VkLayerDeviceCreateInfo*>(
            reinterpret_cast<const VkLayerDeviceCreateInfo*>(chainInfo->pNext));
    }
    if (!chainInfo || !chainInfo->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr fpGetInstanceProcAddr = chainInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr   fpGetDeviceProcAddr   = chainInfo->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    chainInfo->u.pLayerInfo = chainInfo->u.pLayerInfo->pNext;

    auto fpCreateDevice = reinterpret_cast<PFN_vkCreateDevice>(fpGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateDevice"));
    if (!fpCreateDevice) return VK_ERROR_INITIALIZATION_FAILED;

    VkResult res = fpCreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);
    if (res != VK_SUCCESS) return res;

    DeviceDispatch dd = {};
    dd.GetDeviceProcAddr = fpGetDeviceProcAddr;
    dd.DestroyDevice       = reinterpret_cast<PFN_vkDestroyDevice>(fpGetDeviceProcAddr(*pDevice, "vkDestroyDevice"));
    if (IsCaptureDisabledByEnv())
    {
        std::lock_guard<std::mutex> lk(g_DispatchMtx);
        g_DeviceMap[*pDevice] = dd;
        return res;
    }
    dd.CreateSwapchainKHR  = reinterpret_cast<PFN_vkCreateSwapchainKHR>(fpGetDeviceProcAddr(*pDevice, "vkCreateSwapchainKHR"));
    dd.DestroySwapchainKHR = reinterpret_cast<PFN_vkDestroySwapchainKHR>(fpGetDeviceProcAddr(*pDevice, "vkDestroySwapchainKHR"));
    dd.QueuePresentKHR     = reinterpret_cast<PFN_vkQueuePresentKHR>(fpGetDeviceProcAddr(*pDevice, "vkQueuePresentKHR"));

    VkCaptureDeviceInfo& info = dd.info;
    info.physicalDevice = physicalDevice;
    info.device         = *pDevice;
    info.graphicsQueueFamily = pCreateInfo->queueCreateInfoCount > 0 ? pCreateInfo->pQueueCreateInfos[0].queueFamilyIndex : 0;

    info.GetDeviceQueue                = reinterpret_cast<PFN_vkGetDeviceQueue>(fpGetDeviceProcAddr(*pDevice, "vkGetDeviceQueue"));
    info.CreateCommandPool             = reinterpret_cast<PFN_vkCreateCommandPool>(fpGetDeviceProcAddr(*pDevice, "vkCreateCommandPool"));
    info.DestroyCommandPool            = reinterpret_cast<PFN_vkDestroyCommandPool>(fpGetDeviceProcAddr(*pDevice, "vkDestroyCommandPool"));
    info.AllocateCommandBuffers        = reinterpret_cast<PFN_vkAllocateCommandBuffers>(fpGetDeviceProcAddr(*pDevice, "vkAllocateCommandBuffers"));
    info.FreeCommandBuffers            = reinterpret_cast<PFN_vkFreeCommandBuffers>(fpGetDeviceProcAddr(*pDevice, "vkFreeCommandBuffers"));
    info.BeginCommandBuffer            = reinterpret_cast<PFN_vkBeginCommandBuffer>(fpGetDeviceProcAddr(*pDevice, "vkBeginCommandBuffer"));
    info.EndCommandBuffer              = reinterpret_cast<PFN_vkEndCommandBuffer>(fpGetDeviceProcAddr(*pDevice, "vkEndCommandBuffer"));
    info.ResetCommandBuffer            = reinterpret_cast<PFN_vkResetCommandBuffer>(fpGetDeviceProcAddr(*pDevice, "vkResetCommandBuffer"));
    info.QueueSubmit                   = reinterpret_cast<PFN_vkQueueSubmit>(fpGetDeviceProcAddr(*pDevice, "vkQueueSubmit"));
    info.CreateFence                   = reinterpret_cast<PFN_vkCreateFence>(fpGetDeviceProcAddr(*pDevice, "vkCreateFence"));
    info.DestroyFence                  = reinterpret_cast<PFN_vkDestroyFence>(fpGetDeviceProcAddr(*pDevice, "vkDestroyFence"));
    info.CreateSemaphore               = reinterpret_cast<PFN_vkCreateSemaphore>(fpGetDeviceProcAddr(*pDevice, "vkCreateSemaphore"));
    info.DestroySemaphore              = reinterpret_cast<PFN_vkDestroySemaphore>(fpGetDeviceProcAddr(*pDevice, "vkDestroySemaphore"));
    info.GetFenceStatus                = reinterpret_cast<PFN_vkGetFenceStatus>(fpGetDeviceProcAddr(*pDevice, "vkGetFenceStatus"));
    info.WaitForFences                 = reinterpret_cast<PFN_vkWaitForFences>(fpGetDeviceProcAddr(*pDevice, "vkWaitForFences"));
    info.ResetFences                   = reinterpret_cast<PFN_vkResetFences>(fpGetDeviceProcAddr(*pDevice, "vkResetFences"));
    info.CreateBuffer                  = reinterpret_cast<PFN_vkCreateBuffer>(fpGetDeviceProcAddr(*pDevice, "vkCreateBuffer"));
    info.DestroyBuffer                 = reinterpret_cast<PFN_vkDestroyBuffer>(fpGetDeviceProcAddr(*pDevice, "vkDestroyBuffer"));
    info.GetBufferMemoryRequirements   = reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(fpGetDeviceProcAddr(*pDevice, "vkGetBufferMemoryRequirements"));
    info.AllocateMemory                = reinterpret_cast<PFN_vkAllocateMemory>(fpGetDeviceProcAddr(*pDevice, "vkAllocateMemory"));
    info.FreeMemory                    = reinterpret_cast<PFN_vkFreeMemory>(fpGetDeviceProcAddr(*pDevice, "vkFreeMemory"));
    info.BindBufferMemory              = reinterpret_cast<PFN_vkBindBufferMemory>(fpGetDeviceProcAddr(*pDevice, "vkBindBufferMemory"));
    info.MapMemory                     = reinterpret_cast<PFN_vkMapMemory>(fpGetDeviceProcAddr(*pDevice, "vkMapMemory"));
    info.UnmapMemory                   = reinterpret_cast<PFN_vkUnmapMemory>(fpGetDeviceProcAddr(*pDevice, "vkUnmapMemory"));
    info.InvalidateMappedMemoryRanges  = reinterpret_cast<PFN_vkInvalidateMappedMemoryRanges>(fpGetDeviceProcAddr(*pDevice, "vkInvalidateMappedMemoryRanges"));
    info.CreateImage                   = reinterpret_cast<PFN_vkCreateImage>(fpGetDeviceProcAddr(*pDevice, "vkCreateImage"));
    info.DestroyImage                  = reinterpret_cast<PFN_vkDestroyImage>(fpGetDeviceProcAddr(*pDevice, "vkDestroyImage"));
    info.GetImageMemoryRequirements    = reinterpret_cast<PFN_vkGetImageMemoryRequirements>(fpGetDeviceProcAddr(*pDevice, "vkGetImageMemoryRequirements"));
    info.BindImageMemory               = reinterpret_cast<PFN_vkBindImageMemory>(fpGetDeviceProcAddr(*pDevice, "vkBindImageMemory"));
    info.CmdPipelineBarrier            = reinterpret_cast<PFN_vkCmdPipelineBarrier>(fpGetDeviceProcAddr(*pDevice, "vkCmdPipelineBarrier"));
    info.CmdCopyImageToBuffer          = reinterpret_cast<PFN_vkCmdCopyImageToBuffer>(fpGetDeviceProcAddr(*pDevice, "vkCmdCopyImageToBuffer"));
    info.CmdResolveImage               = reinterpret_cast<PFN_vkCmdResolveImage>(fpGetDeviceProcAddr(*pDevice, "vkCmdResolveImage"));
    info.GetSwapchainImagesKHR         = reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(fpGetDeviceProcAddr(*pDevice, "vkGetSwapchainImagesKHR"));
    info.DeviceWaitIdle                = reinterpret_cast<PFN_vkDeviceWaitIdle>(fpGetDeviceProcAddr(*pDevice, "vkDeviceWaitIdle"));

    if (info.GetDeviceQueue)
        info.GetDeviceQueue(*pDevice, info.graphicsQueueFamily, 0, &info.graphicsQueue);

    {
        std::lock_guard<std::mutex> lk(g_DispatchMtx);
        auto pit = g_PhysDevToInstance.find(physicalDevice);
        if (pit == g_PhysDevToInstance.end())
        {
            for (const auto& kv : g_InstanceMap)
            {
                if (!kv.second.EnumeratePhysicalDevices) continue;
                uint32_t cnt = 0;
                if (kv.second.EnumeratePhysicalDevices(kv.first, &cnt, nullptr) == VK_SUCCESS && cnt > 0)
                {
                    std::vector<VkPhysicalDevice> pds(cnt);
                    if (kv.second.EnumeratePhysicalDevices(kv.first, &cnt, pds.data()) == VK_SUCCESS)
                    {
                        for (uint32_t i = 0; i < cnt; ++i)
                            g_PhysDevToInstance[pds[i]] = kv.first;
                    }
                }
            }
            pit = g_PhysDevToInstance.find(physicalDevice);
        }
        auto iit = (pit != g_PhysDevToInstance.end()) ? g_InstanceMap.find(pit->second) : g_InstanceMap.end();
        if (iit == g_InstanceMap.end() && !g_InstanceMap.empty())
            iit = g_InstanceMap.begin();
        if (iit != g_InstanceMap.end())
        {
            const auto& id = iit->second;
            info.instance = id.instance;
            info.GetPhysicalDeviceProperties       = id.GetPhysicalDeviceProperties;
            info.GetInstanceProcAddr               = fpGetInstanceProcAddr;
            info.GetPhysicalDeviceMemoryProperties = id.GetPhysicalDeviceMemoryProperties;
    info.GetDeviceProcAddr                 = fpGetDeviceProcAddr;
        }
        g_DeviceMap[*pDevice] = dd;
    }

    Log("[layer] vkCreateDevice hooked (dev=%p)", static_cast<void*>(*pDevice));
    return res;
}

static VKAPI_ATTR void VKAPI_CALL Hook_vkDestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator)
{
    DeviceDispatch dd = {};
    {
        std::lock_guard<std::mutex> lk(g_DispatchMtx);
        auto it = g_DeviceMap.find(device);
        if (it != g_DeviceMap.end()) { dd = it->second; g_DeviceMap.erase(it); }
    }
    Overlay_Shutdown(&dd.info);
    Capture_OnPreReset(&dd.info);
    if (dd.DestroyDevice) dd.DestroyDevice(device, pAllocator);
}

static VKAPI_ATTR VkResult VKAPI_CALL Hook_vkCreateSwapchainKHR(
    VkDevice device,
    const VkSwapchainCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSwapchainKHR* pSwapchain)
{
    DeviceDispatch dd = {};
    {
        std::lock_guard<std::mutex> lk(g_DispatchMtx);
        auto it = g_DeviceMap.find(device);
        if (it != g_DeviceMap.end()) dd = it->second;
    }
    if (!dd.CreateSwapchainKHR) return VK_ERROR_INITIALIZATION_FAILED;

    if (pCreateInfo && pCreateInfo->oldSwapchain != VK_NULL_HANDLE)
    {
        Overlay_OnPreReset(&dd.info);
        Capture_OnPreReset(&dd.info);
    }

    VkSwapchainCreateInfoKHR modified = *pCreateInfo;
    modified.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    VkResult res = dd.CreateSwapchainKHR(device, &modified, pAllocator, pSwapchain);
    if (res != VK_SUCCESS)
    {
        // Fallback to original flags if driver rejected added usage
        res = dd.CreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
    }
    if (res != VK_SUCCESS) return res;

    EnsureCaptureInitialized();

    uint32_t imgCount = 0;
    dd.info.GetSwapchainImagesKHR(device, *pSwapchain, &imgCount, nullptr);
    std::vector<VkImage> images(imgCount);
    dd.info.GetSwapchainImagesKHR(device, *pSwapchain, &imgCount, images.data());

    {
        std::lock_guard<std::mutex> lk(g_DispatchMtx);
        g_SwapchainToDevice[*pSwapchain] = device;
    }
    g_CurrentSwapchain = *pSwapchain;
    Capture_OnSwapchainCreate(dd.info, *pSwapchain,
                              modified.imageExtent.width, modified.imageExtent.height,
                              modified.imageFormat, imgCount, images.data());

    Overlay_Init(dd.info, *pSwapchain,
                 modified.imageExtent.width, modified.imageExtent.height,
                 modified.imageFormat, modified.minImageCount,
                 imgCount, images.data());

    return res;
}

static VKAPI_ATTR void VKAPI_CALL Hook_vkDestroySwapchainKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    const VkAllocationCallbacks* pAllocator)
{
    DeviceDispatch dd = {};
    {
        std::lock_guard<std::mutex> lk(g_DispatchMtx);
        auto it = g_DeviceMap.find(device);
        if (it != g_DeviceMap.end()) dd = it->second;
        g_SwapchainToDevice.erase(swapchain);
    }
    if (swapchain == g_CurrentSwapchain)
    {
        Overlay_OnPreReset(&dd.info);
        Capture_OnPreReset(&dd.info);
        g_CurrentSwapchain = VK_NULL_HANDLE;
    }
    if (dd.DestroySwapchainKHR) dd.DestroySwapchainKHR(device, swapchain, pAllocator);
}

static VKAPI_ATTR VkResult VKAPI_CALL Hook_vkQueuePresentKHR(
    VkQueue queue,
    const VkPresentInfoKHR* pPresentInfo)
{
    if (!pPresentInfo || pPresentInfo->swapchainCount == 0)
    {
        DeviceDispatch dd = {};
        std::lock_guard<std::mutex> lk(g_DispatchMtx);
        if (!g_DeviceMap.empty()) dd = g_DeviceMap.begin()->second;
        return dd.QueuePresentKHR ? dd.QueuePresentKHR(queue, pPresentInfo) : VK_ERROR_DEVICE_LOST;
    }

    DeviceDispatch dd = {};
    {
        std::lock_guard<std::mutex> lk(g_DispatchMtx);
        auto sit = g_SwapchainToDevice.find(pPresentInfo->pSwapchains[0]);
        auto dit = (sit != g_SwapchainToDevice.end()) ? g_DeviceMap.find(sit->second) : g_DeviceMap.end();
        if (dit == g_DeviceMap.end() && !g_DeviceMap.empty())
            dit = g_DeviceMap.begin();
        if (dit != g_DeviceMap.end())
            dd = dit->second;
    }


    uint64_t tOverlay0 = Platform_GetMonotonicNs();
    VkSemaphore overlaySem = Overlay_OnPresent(
        dd.info, queue, pPresentInfo->pImageIndices[0],
        pPresentInfo->waitSemaphoreCount, pPresentInfo->pWaitSemaphores);
    double overlayMs = static_cast<double>(Platform_GetMonotonicNs() - tOverlay0) / 1e6;

    VkPresentInfoKHR presentInfo = *pPresentInfo;
    if (overlaySem != VK_NULL_HANDLE)
    {
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores    = &overlaySem;
    }

    uint64_t tCap0 = Platform_GetMonotonicNs();
    VkSemaphore capSem = Capture_OnPresent(
        dd.info, queue, pPresentInfo->pSwapchains[0], pPresentInfo->pImageIndices[0],
        presentInfo.waitSemaphoreCount, presentInfo.pWaitSemaphores);
    if (capSem != VK_NULL_HANDLE)
    {
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores    = &capSem;
    }
    double captureMs = static_cast<double>(Platform_GetMonotonicNs() - tCap0) / 1e6;

    uint64_t tPres0 = Platform_GetMonotonicNs();
    VkResult res = dd.QueuePresentKHR ? dd.QueuePresentKHR(queue, &presentInfo) : VK_ERROR_DEVICE_LOST;
    double presentMs = static_cast<double>(Platform_GetMonotonicNs() - tPres0) / 1e6;

    MaybeLogX11Dbg();
    Capture_ReportPresentTiming(captureMs, overlayMs, presentMs);
    return res;
}

// ── Layer Exported Proc Lookup ───────────────────────────────────────────────
extern "C" {

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkcapture_GetDeviceProcAddr(VkDevice device, const char* pName)
{
    if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0)   return reinterpret_cast<PFN_vkVoidFunction>(vkcapture_GetDeviceProcAddr);
    if (std::strcmp(pName, "vkDestroyDevice") == 0)       return reinterpret_cast<PFN_vkVoidFunction>(Hook_vkDestroyDevice);
    if (!IsCaptureDisabledByEnv())
    {
        if (std::strcmp(pName, "vkCreateSwapchainKHR") == 0)  return reinterpret_cast<PFN_vkVoidFunction>(Hook_vkCreateSwapchainKHR);
    if (std::strcmp(pName, "vkDestroySwapchainKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(Hook_vkDestroySwapchainKHR);
        if (std::strcmp(pName, "vkQueuePresentKHR") == 0)     return reinterpret_cast<PFN_vkVoidFunction>(Hook_vkQueuePresentKHR);
    }

    PFN_vkGetDeviceProcAddr nextGpa = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_DispatchMtx);
        auto it = g_DeviceMap.find(device);
        if (it != g_DeviceMap.end())
            nextGpa = it->second.GetDeviceProcAddr;
    }
    return nextGpa ? nextGpa(device, pName) : nullptr;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkcapture_GetInstanceProcAddr(VkInstance instance, const char* pName)
{
    if (std::strcmp(pName, "vkGetInstanceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(vkcapture_GetInstanceProcAddr);
    if (std::strcmp(pName, "vkCreateInstance") == 0)      return reinterpret_cast<PFN_vkVoidFunction>(Hook_vkCreateInstance);
    if (std::strcmp(pName, "vkDestroyInstance") == 0)     return reinterpret_cast<PFN_vkVoidFunction>(Hook_vkDestroyInstance);
    if (std::strcmp(pName, "vkCreateDevice") == 0)        return reinterpret_cast<PFN_vkVoidFunction>(Hook_vkCreateDevice);
    if (!IsCaptureDisabledByEnv())
    {
        if (std::strcmp(pName, "vkEnumeratePhysicalDevices") == 0) return reinterpret_cast<PFN_vkVoidFunction>(Hook_vkEnumeratePhysicalDevices);
        if (std::strcmp(pName, "vkEnumeratePhysicalDeviceGroups") == 0) return reinterpret_cast<PFN_vkVoidFunction>(Hook_vkEnumeratePhysicalDeviceGroups);
        if (std::strcmp(pName, "vkEnumeratePhysicalDeviceGroupsKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(Hook_vkEnumeratePhysicalDeviceGroups);
        if (std::strcmp(pName, "vkCreateXlibSurfaceKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(Hook_vkCreateXlibSurfaceKHR);
    }
    if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0)   return reinterpret_cast<PFN_vkVoidFunction>(vkcapture_GetDeviceProcAddr);

    PFN_vkGetInstanceProcAddr nextGpa = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_DispatchMtx);
        auto it = g_InstanceMap.find(instance);
        if (it != g_InstanceMap.end())
            nextGpa = it->second.GetInstanceProcAddr;
    }
    return nextGpa ? nextGpa(instance, pName) : nullptr;
}

VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* pVersionStruct)
{
    if (!pVersionStruct || pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT)
        return VK_ERROR_INITIALIZATION_FAILED;

    if (pVersionStruct->loaderLayerInterfaceVersion >= 2)
    {
        pVersionStruct->loaderLayerInterfaceVersion = 2;
        pVersionStruct->pfnGetInstanceProcAddr      = vkcapture_GetInstanceProcAddr;
        pVersionStruct->pfnGetDeviceProcAddr        = vkcapture_GetDeviceProcAddr;
        pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;
    }
    return VK_SUCCESS;
}

} // extern "C"
