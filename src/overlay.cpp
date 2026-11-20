/**
 * overlay.cpp  —  In-game ImGui overlay and Control Center (Vulkan + X11 port)
 *
 * Renders Dear ImGui directly onto the Vulkan swapchain image inside
 * vkQueuePresentKHR using imgui_impl_vulkan, chaining a completion semaphore
 * into the game's present call.
 */

#include "overlay.h"
#include "capture.h"
#include "recorder.h"
#include "audio.h"
#include "platform.h"

#include "imgui.h"
#include "imgui_impl_vulkan.h"

#include <X11/Xlib.h>
#include <dlfcn.h>
#include <X11/keysym.h>

#include <atomic>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <vector>
#include <utility>
#include <string>

// ── Vulkan Overlay Resources ─────────────────────────────────────────────────
struct OverlayFrameResources
{
    VkImageView     imageView = VK_NULL_HANDLE;
    VkFramebuffer   framebuffer = VK_NULL_HANDLE;
    VkCommandBuffer cmdBuf = VK_NULL_HANDLE;
    VkFence         fence = VK_NULL_HANDLE;
    VkSemaphore     renderSem = VK_NULL_HANDLE;
};

static std::atomic<bool>   g_OverlayInitialized{ false };
static std::mutex          g_OverlayInitMtx;
static std::atomic<bool>   g_ShowMenu{ false };
static std::atomic<bool>   g_WaylandInputActive{ false };
static float               s_WlCursorX = -1.0f;
static float               s_WlCursorY = -1.0f;
static std::mutex          s_WlCursorMtx;
static std::vector<std::pair<int, bool>> s_PendingMouseBtns;

static OverlaySurfaceInfo  g_SurfaceInfo;
static VkDescriptorPool    g_DescriptorPool = VK_NULL_HANDLE;
static VkRenderPass        g_RenderPass     = VK_NULL_HANDLE;
static VkCommandPool       g_CmdPool        = VK_NULL_HANDLE;
static uint32_t            g_Width          = 0;
static uint32_t            g_Height         = 0;
static std::vector<OverlayFrameResources> g_Frames;

// ── Mini-HUD & UI State ──────────────────────────────────────────────────────
static bool  s_ShowMiniHud      = false;
static int   s_MiniHudPos       = 0;
static bool  s_MiniHudFps       = true;
static bool  s_MiniHudLatency   = true;
static bool  s_MiniHudRes       = true;
static bool  s_MiniHudAudio     = true;
static bool  s_MiniHudClock     = true;
static bool  s_MiniHudRam       = false;
static float s_MiniHudAlpha     = 0.70f;
static bool  s_ShowRecIndicator = true;

static int   s_RecFpsIdx          = 1; // 30 FPS (smooth on 4-core CPUs)
static int   s_RecBitrateIdx      = 1; // 8000 kbps
static int   s_RecAudioBitrateIdx = 1; // 192 kbps
static int   s_AutoStopMinutes    = 0;
static bool  s_HardwareEncoding   = true;
static char  s_CustomPrefix[64]   = "";

static int   s_MenuHotkeyIdx   = 0; // Insert
static int   s_RecordHotkeyIdx = 0; // F9

static int   s_SelectedTheme     = 0; // Emerald
static float s_WindowAlpha       = 0.94f;
static bool  s_ShowDemoWindow    = false;
static bool  s_ShowMetricsWindow = false;

static float    s_AudioVolSlider      = 1.0f;
static uint64_t s_TestToneTriggerMs   = 0;

static int   s_LogCategoryFilter = 0;
static char  s_LogSearch[64]     = "";
static bool  s_LogAutoScroll     = true;

static char     s_GpuName[128]   = "Auto-detecting Vulkan GPU...";
static uint64_t s_GpuVramMb      = 0;
static uint32_t s_VulkanApiVer   = 0;

static constexpr int HISTORY_SIZE = 120;
static float    s_PresentFpsHistory[HISTORY_SIZE] = {};
static float    s_LatencyHistory[HISTORY_SIZE]    = {};
static float    s_AudioPeakHistory[HISTORY_SIZE]  = {};
static int      s_HistoryOffset  = 0;
static uint64_t s_LastHistoryMs  = 0;

static std::atomic<float> s_PendingWheelX{ 0.0f };
static std::atomic<float> s_PendingWheelY{ 0.0f };
static uint64_t           s_LastFrameNs = 0;

bool Overlay_IsMenuOpen() { return g_ShowMenu.load(std::memory_order_relaxed) || Capture_IsSharedMenuOpen(); }
void Overlay_SetWaylandActive() { g_WaylandInputActive.store(true, std::memory_order_relaxed); }

void Overlay_SetSurfaceInfo(const OverlaySurfaceInfo& info)
{
    g_SurfaceInfo = info;
}

static void GetMemoryStats(size_t* outRssMb, size_t* outPeakRssMb)
{
    size_t rssKb = 0, peakKb = 0;
    FILE* f = std::fopen("/proc/self/status", "r");
    if (f)
    {
        char line[256];
        while (std::fgets(line, sizeof(line), f))
        {
            if (std::sscanf(line, "VmRSS: %zu kB", &rssKb) == 1) continue;
            if (std::sscanf(line, "VmHWM: %zu kB", &peakKb) == 1) continue;
        }
        std::fclose(f);
    }
    if (outRssMb)     *outRssMb     = rssKb / 1024;
    if (outPeakRssMb) *outPeakRssMb = peakKb / 1024;
}

static void OpenPathWithXdg(const char* path)
{
    char cmd[MAX_PATH + 32];
    std::snprintf(cmd, sizeof(cmd), "xdg-open \"%s\" >/dev/null 2>&1 &", path);
    int ignored = std::system(cmd);
    (void)ignored;
}

static void StartRecordingWithUiSettings()
{
    static const uint32_t s_FpsVals[]     = { 24, 30, 48, 60, 90, 120 };
    static const uint32_t s_BitrateVals[] = { 4000, 8000, 12000, 16000, 24000, 35000, 50000 };
    static const uint32_t s_AudioVals[]   = { 128, 192, 256, 320 };

    uint32_t fps = s_FpsVals[std::clamp(s_RecFpsIdx, 0, 5)];
    uint32_t br  = s_BitrateVals[std::clamp(s_RecBitrateIdx, 0, 6)];
    uint32_t abr = s_AudioVals[std::clamp(s_RecAudioBitrateIdx, 0, 3)];

    Recorder_SetAudioBitrate(abr);
    Recorder_SetHardwareAccel(s_HardwareEncoding);

    if (s_CustomPrefix[0] != '\0')
    {
        char recDir[MAX_PATH];
        std::snprintf(recDir, sizeof(recDir), "%s/recordings", Platform_GetBaseDir());
        Platform_EnsureDir(recDir);

        struct timespec ts = {};
        clock_gettime(CLOCK_REALTIME, &ts);
        struct tm tmLocal = {};
        localtime_r(&ts.tv_sec, &tmLocal);

        char path[MAX_PATH];
        std::snprintf(path, sizeof(path), "%s/%s_%04d%02d%02d_%02d%02d%02d.mp4",
                      recDir, s_CustomPrefix,
                      tmLocal.tm_year + 1900, tmLocal.tm_mon + 1, tmLocal.tm_mday,
                      tmLocal.tm_hour, tmLocal.tm_min, tmLocal.tm_sec);
        Recorder_Start(path, fps, br);
    }
    else
    {
        Recorder_Start(nullptr, fps, br);
    }
}

static std::atomic<uint64_t> g_LastEventKeyMs{ 0 };

void Overlay_ToggleMenu()
{
    static std::atomic<uint64_t> s_LastMenuToggleMs{ 0 };
    uint64_t now = Platform_GetTickMs();
    uint64_t prev = s_LastMenuToggleMs.load(std::memory_order_relaxed);
    if (now - prev < 250 || !s_LastMenuToggleMs.compare_exchange_strong(prev, now))
        return;

    bool next = !g_ShowMenu.load();
    if (next)
    {
        std::lock_guard<std::mutex> lk(s_WlCursorMtx);
        if (s_WlCursorX < 0.0f || s_WlCursorY < 0.0f)
        {
            s_WlCursorX = g_Width > 0 ? static_cast<float>(g_Width) * 0.5f : 320.0f;
            s_WlCursorY = g_Height > 0 ? static_cast<float>(g_Height) * 0.5f : 240.0f;
        }
    }
    g_ShowMenu.store(next);
    Capture_SetSharedMenuOpen(next);
    Capture_SetMeterRequested(next || (s_ShowMiniHud && s_MiniHudAudio));
    Log("[overlay] Menu %s", next ? "OPENED" : "CLOSED");
}

void Overlay_ToggleRecord()
{
    static std::atomic<uint64_t> s_LastRecToggleMs{ 0 };
    uint64_t now = Platform_GetTickMs();
    uint64_t prev = s_LastRecToggleMs.load(std::memory_order_relaxed);
    if (now - prev < 350 || !s_LastRecToggleMs.compare_exchange_strong(prev, now))
        return;

    if (Recorder_IsRecording() || Recorder_IsStarting())
        Recorder_Stop();
    else
        StartRecordingWithUiSettings();
}

void Overlay_AddMouseWheel(float wheelX, float wheelY)
{
    float curY = s_PendingWheelY.load(std::memory_order_relaxed);
    while (!s_PendingWheelY.compare_exchange_weak(curY, curY + wheelY, std::memory_order_relaxed)) {}
    float curX = s_PendingWheelX.load(std::memory_order_relaxed);
    while (!s_PendingWheelX.compare_exchange_weak(curX, curX + wheelX, std::memory_order_relaxed)) {}
}

void Overlay_OnKeyEvent(int keySym, bool pressed)
{
    if (!pressed) return;
    static const int s_MenuSyms[]   = { XK_Insert, XK_Home, XK_End, XK_F11, XK_grave };
    static const int s_RecordSyms[] = { XK_F9, XK_F10, XK_F11, XK_F12, XK_F8, XK_Scroll_Lock, XK_Pause };

    int menuKey = s_MenuSyms[std::clamp(s_MenuHotkeyIdx, 0, 4)];
    int recKey  = s_RecordSyms[std::clamp(s_RecordHotkeyIdx, 0, 6)];

    if (keySym == menuKey || keySym == recKey) g_LastEventKeyMs.store(Platform_GetTickMs());
    if (keySym == menuKey)
        Overlay_ToggleMenu();
    else if (keySym == recKey)
        Overlay_ToggleRecord();
}

void Overlay_OnMouseMove(float x, float y)
{
    std::lock_guard<std::mutex> lk(s_WlCursorMtx);
    s_WlCursorX = x;
    s_WlCursorY = y;
}

void Overlay_AddMouseDelta(float dx, float dy)
{
    std::lock_guard<std::mutex> lk(s_WlCursorMtx);
    if (s_WlCursorX < 0.0f) s_WlCursorX = g_Width > 0 ? static_cast<float>(g_Width) * 0.5f : 320.0f;
    if (s_WlCursorY < 0.0f) s_WlCursorY = g_Height > 0 ? static_cast<float>(g_Height) * 0.5f : 240.0f;
    float maxW = g_Width > 0 ? static_cast<float>(g_Width) : 1920.0f;
    float maxH = g_Height > 0 ? static_cast<float>(g_Height) : 1080.0f;
    s_WlCursorX = std::clamp(s_WlCursorX + dx, 0.0f, maxW);
    s_WlCursorY = std::clamp(s_WlCursorY + dy, 0.0f, maxH);
}

void Overlay_OnMouseButton(int button, bool pressed)
{
    if (!g_ShowMenu.load() || button < 0 || button >= 5) return;
    std::lock_guard<std::mutex> lk(s_WlCursorMtx);
    if (s_PendingMouseBtns.size() < 64)
        s_PendingMouseBtns.emplace_back(button, pressed);
}

// ── Themes ───────────────────────────────────────────────────────────────────
static void ApplyTheme(int themeIdx)
{
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding    = 8.0f;
    style.FrameRounding     = 5.0f;
    style.GrabRounding      = 5.0f;
    style.TabRounding       = 5.0f;
    style.ScrollbarRounding = 6.0f;
    style.WindowPadding     = ImVec2(12, 12);
    style.FramePadding      = ImVec2(8, 5);
    style.ItemSpacing       = ImVec2(8, 6);

    if (themeIdx == 3) { ImGui::StyleColorsDark(); }
    else if (themeIdx == 4) { ImGui::StyleColorsLight(); }
    else if (themeIdx == 5) { ImGui::StyleColorsClassic(); }
    else
    {
        ImGui::StyleColorsDark();
        ImVec4* c = style.Colors;
        if (themeIdx == 0) // Emerald
        {
            c[ImGuiCol_WindowBg]       = ImVec4(0.06f, 0.08f, 0.07f, s_WindowAlpha);
            c[ImGuiCol_TitleBgActive]  = ImVec4(0.08f, 0.28f, 0.18f, 1.0f);
            c[ImGuiCol_Button]         = ImVec4(0.10f, 0.38f, 0.24f, 0.85f);
            c[ImGuiCol_ButtonHovered]  = ImVec4(0.14f, 0.52f, 0.34f, 1.0f);
            c[ImGuiCol_FrameBg]        = ImVec4(0.10f, 0.16f, 0.13f, 0.90f);
            c[ImGuiCol_Tab]            = ImVec4(0.08f, 0.22f, 0.15f, 0.85f);
            c[ImGuiCol_TabActive]      = ImVec4(0.12f, 0.44f, 0.28f, 1.0f);
            c[ImGuiCol_CheckMark]      = ImVec4(0.25f, 0.90f, 0.55f, 1.0f);
            c[ImGuiCol_SliderGrab]     = ImVec4(0.20f, 0.78f, 0.48f, 1.0f);
        }
        else if (themeIdx == 1) // Cyberpunk
        {
            c[ImGuiCol_WindowBg]       = ImVec4(0.07f, 0.06f, 0.11f, s_WindowAlpha);
            c[ImGuiCol_TitleBgActive]  = ImVec4(0.45f, 0.08f, 0.35f, 1.0f);
            c[ImGuiCol_Button]         = ImVec4(0.55f, 0.10f, 0.42f, 0.85f);
            c[ImGuiCol_ButtonHovered]  = ImVec4(0.75f, 0.15f, 0.58f, 1.0f);
            c[ImGuiCol_TabActive]      = ImVec4(0.00f, 0.55f, 0.68f, 1.0f);
            c[ImGuiCol_CheckMark]      = ImVec4(0.00f, 0.92f, 0.95f, 1.0f);
        }
        else // Midnight Purple
        {
            c[ImGuiCol_WindowBg]       = ImVec4(0.07f, 0.06f, 0.10f, s_WindowAlpha);
            c[ImGuiCol_TitleBgActive]  = ImVec4(0.24f, 0.14f, 0.45f, 1.0f);
            c[ImGuiCol_Button]         = ImVec4(0.32f, 0.20f, 0.58f, 0.85f);
            c[ImGuiCol_ButtonHovered]  = ImVec4(0.42f, 0.28f, 0.74f, 1.0f);
            c[ImGuiCol_TabActive]      = ImVec4(0.36f, 0.22f, 0.66f, 1.0f);
        }
    }
    style.Alpha = s_WindowAlpha;
}

static void UpdateHistory(const CaptureStats& stats)
{
    uint64_t now = Platform_GetTickMs();
    if (now - s_LastHistoryMs < 100) return;
    s_LastHistoryMs = now;

    AudioStats aStats = {};
    Audio_GetStats(&aStats);

    s_PresentFpsHistory[s_HistoryOffset] = stats.presentFps;
    s_LatencyHistory[s_HistoryOffset]    = stats.readbackMs;
    s_AudioPeakHistory[s_HistoryOffset]  = aStats.peakDb;
    s_HistoryOffset = (s_HistoryOffset + 1) % HISTORY_SIZE;
}

static void CheckAutoStopTimer()
{
    if (s_AutoStopMinutes <= 0 || !Recorder_IsRecording()) return;
    RecorderStats rStats = {};
    Recorder_GetStats(&rStats);
    if (rStats.durationMs >= static_cast<uint64_t>(s_AutoStopMinutes) * 60000ULL)
    {
        Log("[overlay] Auto-stop timer reached (%d min)", s_AutoStopMinutes);
        Recorder_Stop();
    }
}

static void RenderRecordingBeacon()
{
    if (!s_ShowRecIndicator) return;
    bool starting  = Recorder_IsStarting();
    bool recording = Recorder_IsRecording();
    if (!starting && !recording) return;

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;

    ImGui::SetNextWindowPos(ImVec2(16.0f, 16.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.75f);
    if (ImGui::Begin("##RecBeacon", nullptr, flags))
    {
        if (starting)
        {
            bool blink = ((Platform_GetTickMs() / 300) % 2) == 0;
            ImGui::TextColored(blink ? ImVec4(1.0f, 0.75f, 0.1f, 1.0f) : ImVec4(0.6f, 0.45f, 0.1f, 0.6f),
                               "[STARTING] Pre-arming encoder...");
        }
        else
        {
            RecorderStats rStats = {};
            Recorder_GetStats(&rStats);
            uint64_t sec = rStats.durationMs / 1000;
            ImGui::TextColored(ImVec4(1.0f, 0.22f, 0.22f, 1.0f), "[REC] %02llu:%02llu",
                               static_cast<unsigned long long>(sec / 60),
                               static_cast<unsigned long long>(sec % 60));
        }
    }
    ImGui::End();
}

static void RenderMiniHud(const CaptureStats& stats, int pos)
{
    ImGuiIO& io = ImGui::GetIO();
    float pad = 12.0f;
    ImVec2 pivot(0, 0), winPos(pad, pad);
    if (pos == 0)      { winPos = ImVec2(io.DisplaySize.x - pad, pad); pivot = ImVec2(1, 0); }
    else if (pos == 1) { winPos = ImVec2(pad, pad); pivot = ImVec2(0, 0); }
    else if (pos == 2) { winPos = ImVec2(io.DisplaySize.x - pad, io.DisplaySize.y - pad); pivot = ImVec2(1, 1); }
    else               { winPos = ImVec2(pad, io.DisplaySize.y - pad); pivot = ImVec2(0, 1); }

    ImGui::SetNextWindowPos(winPos, ImGuiCond_Always, pivot);
    ImGui::SetNextWindowBgAlpha(s_MiniHudAlpha);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##MiniHud", nullptr, flags))
    {
        if (s_MiniHudFps)     ImGui::Text("FPS: %.1f (Cap: %.1f)", stats.presentFps, stats.captureFps);
        if (s_MiniHudLatency) ImGui::Text("Readback: %.2f ms", stats.readbackMs);
        if (s_MiniHudRes)     ImGui::Text("Res: %ux%u", stats.width, stats.height);
        if (s_MiniHudAudio)
        {
            AudioStats a = {}; Audio_GetStats(&a);
            ImGui::Text("Audio: %.1f dBFS", a.peakDb);
        }
        if (s_MiniHudRam)
        {
            size_t rss = 0; GetMemoryStats(&rss, nullptr);
            ImGui::Text("RAM: %zu MB", rss);
        }
        if (s_MiniHudClock)
        {
            std::time_t t = std::time(nullptr);
            struct tm lt = {}; localtime_r(&t, &lt);
            ImGui::Text("%02d:%02d:%02d", lt.tm_hour, lt.tm_min, lt.tm_sec);
        }
    }
    ImGui::End();
}

static void RenderControlCenter(const CaptureStats& stats)
{
    if (!g_ShowMenu.load()) return;

    ImGui::SetNextWindowSize(ImVec2(760, 520), ImGuiCond_FirstUseEver);
    bool open = true;
    if (!ImGui::Begin("vkcapture Control Center (Linux / Vulkan)", &open))
    {
        ImGui::End();
        if (!open) Overlay_ToggleMenu();
        return;
    }
    if (!open) { ImGui::End(); Overlay_ToggleMenu(); return; }

    RecorderStats rStats = {};
    Recorder_GetStats(&rStats);
    AudioStats aStats = {};
    Audio_GetStats(&aStats);

    if (ImGui::BeginTabBar("MainTabs"))
    {
        // ── 1. Dashboard ─────────────────────────────────────────────────────
        if (ImGui::BeginTabItem("Dashboard"))
        {
            ImGui::Text("Resolution: %ux%u  |  Present: %.1f FPS  |  Capture: %.1f FPS  |  Readback: %.2f ms",
                        stats.width, stats.height, stats.presentFps, stats.captureFps, stats.readbackMs);
            ImGui::Text("Presents: %llu  |  Captured: %llu  |  IPC Consumer: %s",
                        static_cast<unsigned long long>(stats.presentCalls),
                        static_cast<unsigned long long>(stats.capturedFrames),
                        stats.isConsumerActive ? "Connected" : "Idle");
            ImGui::Separator();

            if (rStats.isRecording || rStats.isStarting)
            {
                if (ImGui::Button("Stop Recording (F9)", ImVec2(180, 32))) Recorder_Stop();
                ImGui::SameLine();
                if (rStats.isPaused) { if (ImGui::Button("Resume", ImVec2(100, 32))) Recorder_Resume(); }
                else                 { if (ImGui::Button("Pause",  ImVec2(100, 32))) Recorder_Pause(); }
                ImGui::Text("Output: %s  (%llu frames, %llu dropped)",
                            rStats.outputPath,
                            static_cast<unsigned long long>(rStats.recordedFrames),
                            static_cast<unsigned long long>(rStats.droppedFrames));
            }
            else
            {
                if (ImGui::Button("Start Recording (F9)", ImVec2(180, 32))) StartRecordingWithUiSettings();
                ImGui::SameLine();
                if (ImGui::Button("Screenshot (BMP)", ImVec2(150, 32))) Capture_TriggerSnapshot();
            }

            if (rStats.lastError[0])
                ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "Error: %s", rStats.lastError);

            ImGui::Separator();
            ImGui::PlotLines("Present FPS", s_PresentFpsHistory, HISTORY_SIZE, s_HistoryOffset, nullptr, 0.0f, 240.0f, ImVec2(-1, 65));
            ImGui::PlotLines("Readback (ms)", s_LatencyHistory, HISTORY_SIZE, s_HistoryOffset, nullptr, 0.0f, 16.0f, ImVec2(-1, 55));
            ImGui::PlotLines("Audio Peak (dBFS)", s_AudioPeakHistory, HISTORY_SIZE, s_HistoryOffset, nullptr, -96.0f, 0.0f, ImVec2(-1, 55));
            ImGui::EndTabItem();
        }

        // ── 2. Video Recording ───────────────────────────────────────────────
        if (ImGui::BeginTabItem("Video Recording"))
        {
            ImGui::InputText("Filename Prefix", s_CustomPrefix, sizeof(s_CustomPrefix));
            const char* fpsItems[] = { "24 FPS", "30 FPS", "48 FPS", "60 FPS", "90 FPS", "120 FPS" };
            ImGui::Combo("Target FPS", &s_RecFpsIdx, fpsItems, 6);

            const char* brItems[] = { "4 Mbps", "8 Mbps", "12 Mbps", "16 Mbps", "24 Mbps", "35 Mbps", "50 Mbps" };
            ImGui::Combo("Video Bitrate", &s_RecBitrateIdx, brItems, 7);

            const char* abrItems[] = { "128 kbps", "192 kbps", "256 kbps", "320 kbps" };
            ImGui::Combo("AAC Audio Bitrate", &s_RecAudioBitrateIdx, abrItems, 4);

            ImGui::Checkbox("Hardware Encoding (NVENC/QSV -> libx264 fallback)", &s_HardwareEncoding);

            bool autoSave = Recorder_GetAutoSaveOnExit();
            if (ImGui::Checkbox("Auto-save MP4 on exit", &autoSave))
                Recorder_SetAutoSaveOnExit(autoSave);

            const char* stopItems[] = { "Disabled", "1 min", "5 min", "10 min", "15 min", "30 min", "60 min" };
            static const int stopMins[] = { 0, 1, 5, 10, 15, 30, 60 };
            int stopIdx = 0;
            for (int i = 0; i < 7; ++i) if (stopMins[i] == s_AutoStopMinutes) stopIdx = i;
            if (ImGui::Combo("Auto-stop Timer", &stopIdx, stopItems, 7))
                s_AutoStopMinutes = stopMins[stopIdx];

            const char* menuKeys[] = { "Insert", "Home", "End", "F11", "` (Grave)" };
            const char* recKeys[]  = { "F9", "F10", "F11", "F12", "F8", "Scroll Lock", "Pause" };
            ImGui::Combo("Menu Hotkey", &s_MenuHotkeyIdx, menuKeys, 5);
            ImGui::Combo("Record Hotkey", &s_RecordHotkeyIdx, recKeys, 7);

            int capTargetFps = Capture_GetTargetFps();
            if (ImGui::SliderInt("Capture Limiter (0 = unlimited)", &capTargetFps, 0, 240))
                Capture_SetTargetFps(capTargetFps);

            if (ImGui::Button("Open Recordings Folder"))
            {
                char dir[MAX_PATH];
                std::snprintf(dir, sizeof(dir), "%s/recordings", Platform_GetBaseDir());
                Platform_EnsureDir(dir);
                OpenPathWithXdg(dir);
            }
            ImGui::SameLine();
            if (ImGui::Button("Open Screenshots Folder"))
            {
                char dir[MAX_PATH];
                std::snprintf(dir, sizeof(dir), "%s/screenshots", Platform_GetBaseDir());
                Platform_EnsureDir(dir);
                OpenPathWithXdg(dir);
            }
            ImGui::EndTabItem();
        }

        // ── 3. Audio & Sound ─────────────────────────────────────────────────
        if (ImGui::BeginTabItem("Audio & Sound"))
        {
            bool aEn = Audio_IsEnabled();
            if (ImGui::Checkbox("Enable Audio Capture", &aEn)) Audio_SetEnabled(aEn);

            const char* modes[] = { "Auto (Hook + Loopback Fallback)", "Direct Hook Only (Pulse/ALSA)", "Monitor Loopback Only" };
            int mode = Audio_GetCaptureMode();
            if (ImGui::Combo("Capture Mode", &mode, modes, 3)) Audio_SetCaptureMode(mode);

            if (ImGui::SliderFloat("Software Gain", &s_AudioVolSlider, 0.0f, 3.0f, "%.2fx"))
                Audio_SetVolume(s_AudioVolSlider);

            ImGui::Text("Hooks Installed: %s  |  Active Source: %s  |  Streams: %u",
                        aStats.hooksInstalled ? "Yes" : "No",
                        aStats.source == AudioSource_Hook ? "Direct Hook" :
                        (aStats.source == AudioSource_Loopback ? "Monitor Loopback" : "None"),
                        aStats.activeStreams);
            ImGui::Text("Format: %u Hz, %u ch, %u-bit %s  |  Captured: %llu  |  Overruns: %llu",
                        aStats.sampleRate, aStats.channels, aStats.bitsPerSample,
                        aStats.isFloat ? "float" : "int",
                        static_cast<unsigned long long>(aStats.capturedFrames),
                        static_cast<unsigned long long>(aStats.overruns));

            float norm = std::clamp((aStats.peakDb + 72.0f) / 72.0f, 0.0f, 1.0f);
            char dbText[32];
            std::snprintf(dbText, sizeof(dbText), "%.1f dBFS", aStats.peakDb);
            ImGui::ProgressBar(norm, ImVec2(-1, 20), dbText);

            if (ImGui::Button("Inject 440 Hz Test Tone (0.5s)"))
            {
                Audio_InjectTestTone(440, 500);
                s_TestToneTriggerMs = Platform_GetTickMs();
            }
            if (s_TestToneTriggerMs > 0 && (Platform_GetTickMs() - s_TestToneTriggerMs) < 1500)
            {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.5f, 1.0f), "Tone injected!");
            }
            ImGui::EndTabItem();
        }

        // ── 4. Live Logs ─────────────────────────────────────────────────────
        if (ImGui::BeginTabItem("Live Logs"))
        {
            const char* filters[] = { "All", "[rec]", "[audio]", "[capvk]", "[overlay]", "Errors" };
            ImGui::Combo("Tag Filter", &s_LogCategoryFilter, filters, 6);
            ImGui::SameLine();
            ImGui::InputText("Search", s_LogSearch, sizeof(s_LogSearch));
            ImGui::Checkbox("Auto-scroll", &s_LogAutoScroll);
            ImGui::SameLine();
            if (ImGui::Button("Clear")) Log_ClearRecentEntries();
            ImGui::SameLine();
            if (ImGui::Button("Open Log File"))
            {
                char logPath[MAX_PATH];
                std::snprintf(logPath, sizeof(logPath), "%s/debug.log", Platform_GetBaseDir());
                OpenPathWithXdg(logPath);
            }

            ImGui::BeginChild("LogScroll", ImVec2(0, -1), true);
            static std::vector<LogEntry> entries(256);
            size_t count = Log_GetRecentEntries(entries.data(), entries.size());
            for (size_t i = 0; i < count; ++i)
            {
                const char* line = entries[i].text;
                if (s_LogCategoryFilter == 1 && !std::strstr(line, "[rec]"))     continue;
                if (s_LogCategoryFilter == 2 && !std::strstr(line, "[audio]"))   continue;
                if (s_LogCategoryFilter == 3 && !std::strstr(line, "[capvk]"))   continue;
                if (s_LogCategoryFilter == 4 && !std::strstr(line, "[overlay]")) continue;
                if (s_LogCategoryFilter == 5 && !std::strstr(line, "fail") && !std::strstr(line, "ERROR")) continue;
                if (s_LogSearch[0] && !std::strstr(line, s_LogSearch)) continue;
                ImGui::TextUnformatted(line);
            }
            if (s_LogAutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
                ImGui::SetScrollHereY(1.0f);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        // ── 5. Appearance & HUD ──────────────────────────────────────────────
        if (ImGui::BeginTabItem("Appearance & HUD"))
        {
            const char* themes[] = { "Emerald", "Cyberpunk", "Midnight Purple", "Dark", "Light", "Classic" };
            if (ImGui::Combo("Theme", &s_SelectedTheme, themes, 6)) ApplyTheme(s_SelectedTheme);
            if (ImGui::SliderFloat("Window Opacity", &s_WindowAlpha, 0.4f, 1.0f)) ApplyTheme(s_SelectedTheme);

            ImGui::Separator();
            ImGui::Checkbox("Show Mini-HUD", &s_ShowMiniHud);
            const char* hudPos[] = { "Top-Right", "Top-Left", "Bottom-Right", "Bottom-Left" };
            ImGui::Combo("Mini-HUD Position", &s_MiniHudPos, hudPos, 4);
            ImGui::SliderFloat("Mini-HUD Alpha", &s_MiniHudAlpha, 0.2f, 1.0f);
            ImGui::Checkbox("FPS", &s_MiniHudFps); ImGui::SameLine();
            ImGui::Checkbox("Latency", &s_MiniHudLatency); ImGui::SameLine();
            ImGui::Checkbox("Resolution", &s_MiniHudRes);
            ImGui::Checkbox("Audio dBFS", &s_MiniHudAudio); ImGui::SameLine();
            ImGui::Checkbox("Clock", &s_MiniHudClock); ImGui::SameLine();
            ImGui::Checkbox("Process RAM", &s_MiniHudRam);
            ImGui::Checkbox("Show Recording Beacon", &s_ShowRecIndicator);
            ImGui::EndTabItem();
        }

        // ── 6. System & GPU ──────────────────────────────────────────────────
        if (ImGui::BeginTabItem("System & GPU"))
        {
            size_t rss = 0, peakRss = 0;
            GetMemoryStats(&rss, &peakRss);
            ImGui::Text("Backend: Vulkan %u.%u.%u",
                        VK_API_VERSION_MAJOR(s_VulkanApiVer),
                        VK_API_VERSION_MINOR(s_VulkanApiVer),
                        VK_API_VERSION_PATCH(s_VulkanApiVer));
            ImGui::Text("GPU Adapter: %s", s_GpuName);
            ImGui::Text("Device-Local VRAM: %llu MB", static_cast<unsigned long long>(s_GpuVramMb));
            ImGui::Text("Process RSS: %zu MB (Peak: %zu MB)", rss, peakRss);
            ImGui::Text("POSIX SHM: /vkcapture_shm (%s)", stats.isConsumerActive ? "Reader Connected" : "No Reader");
            ImGui::Text("Data Dir: %s", Platform_GetBaseDir());
            ImGui::EndTabItem();
        }

        // ── 7. Info & Help ───────────────────────────────────────────────────
        if (ImGui::BeginTabItem("Info & Help"))
        {
            ImGui::TextWrapped("vkcapture (Linux Vulkan Port) captures frames directly inside vkQueuePresentKHR "
                               "using quad-buffered asynchronous VkBuffer readback and captures audio via "
                               "PulseAudio/PipeWire/ALSA LD_PRELOAD hooks with monitor loopback fallback.");
            ImGui::Separator();
            ImGui::BulletText("Press Insert to toggle this control center");
            ImGui::BulletText("Press F9 to start/stop H.264/AAC MP4 recording");
            ImGui::BulletText("Run shm_reader --record out.mp4 for out-of-process capture");
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::End();
}

// ── Vulkan Lifecycle ─────────────────────────────────────────────────────────


static VKAPI_ATTR void VKAPI_CALL Dummy_vkDestroySurfaceKHR(VkInstance, VkSurfaceKHR, const VkAllocationCallbacks*) {}
static VKAPI_ATTR VkResult VKAPI_CALL Dummy_VkResultStub() { return VK_SUCCESS; }

struct OverlayVkFns
{
    PFN_vkCreateDescriptorPool   CreateDescriptorPool   = nullptr;
    PFN_vkDestroyDescriptorPool  DestroyDescriptorPool  = nullptr;
    PFN_vkCreateRenderPass       CreateRenderPass       = nullptr;
    PFN_vkDestroyRenderPass      DestroyRenderPass      = nullptr;
    PFN_vkCreateCommandPool      CreateCommandPool      = nullptr;
    PFN_vkDestroyCommandPool     DestroyCommandPool     = nullptr;
    PFN_vkCreateImageView        CreateImageView        = nullptr;
    PFN_vkDestroyImageView       DestroyImageView       = nullptr;
    PFN_vkCreateFramebuffer      CreateFramebuffer      = nullptr;
    PFN_vkDestroyFramebuffer     DestroyFramebuffer     = nullptr;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers = nullptr;
    PFN_vkCreateFence            CreateFence            = nullptr;
    PFN_vkDestroyFence           DestroyFence           = nullptr;
    PFN_vkCreateSemaphore        CreateSemaphore        = nullptr;
    PFN_vkDestroySemaphore       DestroySemaphore       = nullptr;
    PFN_vkCmdBeginRenderPass     CmdBeginRenderPass     = nullptr;
    PFN_vkCmdEndRenderPass       CmdEndRenderPass       = nullptr;
};
static OverlayVkFns g_OvVk;

static void ResolveOverlayVkFns(const VkCaptureDeviceInfo& d)
{
    auto gdpa = d.GetDeviceProcAddr;
    if (!gdpa || d.device == VK_NULL_HANDLE) return;
    g_OvVk.CreateDescriptorPool   = reinterpret_cast<PFN_vkCreateDescriptorPool>(gdpa(d.device, "vkCreateDescriptorPool"));
    g_OvVk.DestroyDescriptorPool  = reinterpret_cast<PFN_vkDestroyDescriptorPool>(gdpa(d.device, "vkDestroyDescriptorPool"));
    g_OvVk.CreateRenderPass       = reinterpret_cast<PFN_vkCreateRenderPass>(gdpa(d.device, "vkCreateRenderPass"));
    g_OvVk.DestroyRenderPass      = reinterpret_cast<PFN_vkDestroyRenderPass>(gdpa(d.device, "vkDestroyRenderPass"));
    g_OvVk.CreateCommandPool      = reinterpret_cast<PFN_vkCreateCommandPool>(gdpa(d.device, "vkCreateCommandPool"));
    g_OvVk.DestroyCommandPool     = reinterpret_cast<PFN_vkDestroyCommandPool>(gdpa(d.device, "vkDestroyCommandPool"));
    g_OvVk.CreateImageView        = reinterpret_cast<PFN_vkCreateImageView>(gdpa(d.device, "vkCreateImageView"));
    g_OvVk.DestroyImageView       = reinterpret_cast<PFN_vkDestroyImageView>(gdpa(d.device, "vkDestroyImageView"));
    g_OvVk.CreateFramebuffer      = reinterpret_cast<PFN_vkCreateFramebuffer>(gdpa(d.device, "vkCreateFramebuffer"));
    g_OvVk.DestroyFramebuffer     = reinterpret_cast<PFN_vkDestroyFramebuffer>(gdpa(d.device, "vkDestroyFramebuffer"));
    g_OvVk.AllocateCommandBuffers = reinterpret_cast<PFN_vkAllocateCommandBuffers>(gdpa(d.device, "vkAllocateCommandBuffers"));
    g_OvVk.CreateFence            = reinterpret_cast<PFN_vkCreateFence>(gdpa(d.device, "vkCreateFence"));
    g_OvVk.DestroyFence           = reinterpret_cast<PFN_vkDestroyFence>(gdpa(d.device, "vkDestroyFence"));
    g_OvVk.CreateSemaphore        = reinterpret_cast<PFN_vkCreateSemaphore>(gdpa(d.device, "vkCreateSemaphore"));
    g_OvVk.DestroySemaphore       = reinterpret_cast<PFN_vkDestroySemaphore>(gdpa(d.device, "vkDestroySemaphore"));
    g_OvVk.CmdBeginRenderPass     = reinterpret_cast<PFN_vkCmdBeginRenderPass>(gdpa(d.device, "vkCmdBeginRenderPass"));
    g_OvVk.CmdEndRenderPass       = reinterpret_cast<PFN_vkCmdEndRenderPass>(gdpa(d.device, "vkCmdEndRenderPass"));
}

static void DestroySwapchainOverlayResources(const VkCaptureDeviceInfo* devInfo)
{
    if (!devInfo || devInfo->device == VK_NULL_HANDLE) return;
    VkDevice dev = devInfo->device;
    devInfo->DeviceWaitIdle(dev);

    for (auto& f : g_Frames)
    {
        if (f.framebuffer != VK_NULL_HANDLE) g_OvVk.DestroyFramebuffer(dev, f.framebuffer, nullptr);
        if (f.imageView   != VK_NULL_HANDLE) g_OvVk.DestroyImageView(dev, f.imageView, nullptr);
        if (f.fence       != VK_NULL_HANDLE) g_OvVk.DestroyFence(dev, f.fence, nullptr);
        if (f.renderSem   != VK_NULL_HANDLE) g_OvVk.DestroySemaphore(dev, f.renderSem, nullptr);
    }
    g_Frames.clear();

    if (g_CmdPool != VK_NULL_HANDLE)
    {
        g_OvVk.DestroyCommandPool(dev, g_CmdPool, nullptr);
        g_CmdPool = VK_NULL_HANDLE;
    }
    if (g_RenderPass != VK_NULL_HANDLE)
    {
        g_OvVk.DestroyRenderPass(dev, g_RenderPass, nullptr);
        g_RenderPass = VK_NULL_HANDLE;
    }
}

void Overlay_Init(const VkCaptureDeviceInfo& devInfo,
                  VkSwapchainKHR swapchain,
                  uint32_t width,
                  uint32_t height,
                  VkFormat format,
                  uint32_t minImageCount,
                  uint32_t imageCount,
                  const VkImage* images)
{
    (void)swapchain;
    std::lock_guard<std::mutex> lk(g_OverlayInitMtx);

    Log("[overlay] Overlay_Init start (%ux%u fmt=%u imgs=%u inst=%p dev=%p q=%p qf=%u)",
        width, height, static_cast<unsigned>(format), imageCount,
        static_cast<void*>(devInfo.instance), static_cast<void*>(devInfo.device),
        static_cast<void*>(devInfo.graphicsQueue), devInfo.graphicsQueueFamily);
    ResolveOverlayVkFns(devInfo);
    VkDevice dev = devInfo.device;
    g_Width  = width;
    g_Height = height;

    if (!g_OverlayInitialized.load())
    {
            VkPhysicalDeviceProperties props = {};
        if (devInfo.GetPhysicalDeviceProperties && devInfo.physicalDevice != VK_NULL_HANDLE)
            devInfo.GetPhysicalDeviceProperties(devInfo.physicalDevice, &props);
        Platform_SafeStrCpy(s_GpuName, sizeof(s_GpuName), props.deviceName);
        s_VulkanApiVer = props.apiVersion;

        VkPhysicalDeviceMemoryProperties memProps = {};
        devInfo.GetPhysicalDeviceMemoryProperties(devInfo.physicalDevice, &memProps);
        uint64_t vramBytes = 0;
        for (uint32_t i = 0; i < memProps.memoryHeapCount; ++i)
            if (memProps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                vramBytes += memProps.memoryHeaps[i].size;
        s_GpuVramMb = vramBytes / (1024ULL * 1024ULL);

            IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
        ApplyTheme(s_SelectedTheme);
        if (!io.Fonts->IsBuilt()) {
            unsigned char* fpx = nullptr; int fw = 0, fh = 0;
            io.Fonts->GetTexDataAsRGBA32(&fpx, &fw, &fh);
                }

        VkDescriptorPoolSize poolSizes[] = {
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64 }
        };
        VkDescriptorPoolCreateInfo dpci = {};
        dpci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpci.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        dpci.maxSets       = 64;
        dpci.poolSizeCount = 1;
        dpci.pPoolSizes    = poolSizes;
        g_OvVk.CreateDescriptorPool(dev, &dpci, nullptr, &g_DescriptorPool);
    }
    else
    {
        ImGui_ImplVulkan_Shutdown();
        DestroySwapchainOverlayResources(&devInfo);
    }

    // Create RenderPass with LOAD_OP_LOAD to composite over the game's image
    VkAttachmentDescription att = {};
    att.format         = format;
    att.samples        = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
    att.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout  = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    att.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorRef = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments    = &colorRef;

    VkSubpassDependency dep = {};
    dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass    = 0;
    dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpci = {};
    rpci.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 1;
    rpci.pAttachments    = &att;
    rpci.subpassCount    = 1;
    rpci.pSubpasses      = &subpass;
    rpci.dependencyCount = 1;
    rpci.pDependencies   = &dep;
    g_OvVk.CreateRenderPass(dev, &rpci, nullptr, &g_RenderPass);

    VkCommandPoolCreateInfo cpci = {};
    cpci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = devInfo.graphicsQueueFamily;
    g_OvVk.CreateCommandPool(dev, &cpci, nullptr, &g_CmdPool);

    g_Frames.resize(imageCount);
    for (uint32_t i = 0; i < imageCount; ++i)
    {
        VkImageViewCreateInfo ivci = {};
        ivci.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ivci.image            = images[i];
        ivci.viewType         = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format           = format;
        ivci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        g_OvVk.CreateImageView(dev, &ivci, nullptr, &g_Frames[i].imageView);

        VkFramebufferCreateInfo fbci = {};
        fbci.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbci.renderPass      = g_RenderPass;
        fbci.attachmentCount = 1;
        fbci.pAttachments    = &g_Frames[i].imageView;
        fbci.width           = width;
        fbci.height          = height;
        fbci.layers          = 1;
        g_OvVk.CreateFramebuffer(dev, &fbci, nullptr, &g_Frames[i].framebuffer);

        VkCommandBufferAllocateInfo cbai = {};
        cbai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool        = g_CmdPool;
        cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        g_OvVk.AllocateCommandBuffers(dev, &cbai, &g_Frames[i].cmdBuf);

        VkFenceCreateInfo fci = {};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        g_OvVk.CreateFence(dev, &fci, nullptr, &g_Frames[i].fence);

        VkSemaphoreCreateInfo sci = {};
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        g_OvVk.CreateSemaphore(dev, &sci, nullptr, &g_Frames[i].renderSem);
    }

    ImGui_ImplVulkan_InitInfo initInfo = {};
    initInfo.Instance        = devInfo.instance;
    initInfo.PhysicalDevice  = devInfo.physicalDevice;
    initInfo.Device          = dev;
    initInfo.QueueFamily     = devInfo.graphicsQueueFamily;
    initInfo.Queue           = devInfo.graphicsQueue;
    initInfo.DescriptorPool  = g_DescriptorPool;
    initInfo.RenderPass      = g_RenderPass;
    initInfo.MinImageCount   = std::max(2u, minImageCount);
    initInfo.ImageCount      = imageCount;
    initInfo.MSAASamples     = VK_SAMPLE_COUNT_1_BIT;
    initInfo.CheckVkResultFn = [](VkResult err) {
        if (err != VK_SUCCESS) Log("[overlay] ImGui VkResult error: %d", static_cast<int>(err));
    };

    static VkCaptureDeviceInfo s_OverlayDevInfo = {};
    s_OverlayDevInfo = devInfo;
    bool loaded = ImGui_ImplVulkan_LoadFunctions(
        [](const char* fnName, void* userData) -> PFN_vkVoidFunction {
            auto* d = static_cast<const VkCaptureDeviceInfo*>(userData);
            if (std::strcmp(fnName, "vkGetPhysicalDeviceMemoryProperties") == 0)
                return reinterpret_cast<PFN_vkVoidFunction>(d->GetPhysicalDeviceMemoryProperties);
            if (std::strcmp(fnName, "vkGetPhysicalDeviceProperties") == 0)
                return reinterpret_cast<PFN_vkVoidFunction>(d->GetPhysicalDeviceProperties);
            PFN_vkVoidFunction fn = d->GetDeviceProcAddr ? d->GetDeviceProcAddr(d->device, fnName) : nullptr;
            if (!fn && d->GetInstanceProcAddr)
                fn = d->GetInstanceProcAddr(VK_NULL_HANDLE, fnName);
            if (!fn && std::strcmp(fnName, "vkDestroySurfaceKHR") == 0)
                return reinterpret_cast<PFN_vkVoidFunction>(Dummy_vkDestroySurfaceKHR);
            if (!fn && (std::strcmp(fnName, "vkEnumeratePhysicalDevices") == 0 ||
                        std::strncmp(fnName, "vkGetPhysicalDevice", 19) == 0))
                return reinterpret_cast<PFN_vkVoidFunction>(Dummy_VkResultStub);
            if (!fn)
                Log("[overlay] LoadFunctions MISSING: %s", fnName);
            return fn;
        },
        &s_OverlayDevInfo);
    if (!loaded)
        Log("[overlay] WARNING: ImGui_ImplVulkan_LoadFunctions returned false!");

    ImGui_ImplVulkan_Init(&initInfo);
    // Fonts texture is created automatically on first NewFrame in ImGui 1.91+

    g_OverlayInitialized.store(true);
    Log("[overlay] Vulkan ImGui initialized (%ux%u, %u images)", width, height, imageCount);
}

void Overlay_OnPreReset(const VkCaptureDeviceInfo* devInfo)
{
    std::lock_guard<std::mutex> lk(g_OverlayInitMtx);
    if (!g_OverlayInitialized.load()) return;
    DestroySwapchainOverlayResources(devInfo);
}

static void ReleaseX11GrabIfHeld();

void Overlay_Shutdown(const VkCaptureDeviceInfo* devInfo)
{
    std::lock_guard<std::mutex> lk(g_OverlayInitMtx);
    Capture_SetSharedMenuOpen(false);
    Capture_SetMeterRequested(false);
    ReleaseX11GrabIfHeld();
    if (!g_OverlayInitialized.exchange(false)) return;
    if (devInfo && devInfo->device != VK_NULL_HANDLE)
    {
        ImGui_ImplVulkan_Shutdown();
        DestroySwapchainOverlayResources(devInfo);
        if (g_DescriptorPool != VK_NULL_HANDLE)
        {
            g_OvVk.DestroyDescriptorPool(devInfo->device, g_DescriptorPool, nullptr);
            g_DescriptorPool = VK_NULL_HANDLE;
        }
    }
    if (ImGui::GetCurrentContext())
        ImGui::DestroyContext();
    Log("[overlay] Shutdown complete");
}



static void SetXInput2PointerGrab(Display* dpy, Window grabWin, bool grab)
{
    struct XIEventMaskLocal { int deviceid; int mask_len; unsigned char* mask; };
    using PFN_XIGrabDevice   = int (*)(Display*, int, Window, Time, Cursor, int, int, Bool, XIEventMaskLocal*);
    using PFN_XIUngrabDevice = int (*)(Display*, int, Time);

    static void* libXi = dlopen("libXi.so.6", RTLD_LAZY);
    static auto pfnGrab   = libXi ? reinterpret_cast<PFN_XIGrabDevice>(dlsym(libXi, "XIGrabDevice")) : reinterpret_cast<PFN_XIGrabDevice>(dlsym(RTLD_NEXT, "XIGrabDevice"));
    static auto pfnUngrab = libXi ? reinterpret_cast<PFN_XIUngrabDevice>(dlsym(libXi, "XIUngrabDevice")) : nullptr;

    constexpr int MASTER_POINTER_ID = 2; // Virtual core pointer
    if (grab && pfnGrab)
    {
        unsigned char maskBits[4] = {};
        // XI_ButtonPress=4, XI_ButtonRelease=5, XI_Motion=6
        maskBits[0] |= (1 << 4) | (1 << 5) | (1 << 6);
        XIEventMaskLocal mask = { MASTER_POINTER_ID, sizeof(maskBits), maskBits };
        int xiRc = pfnGrab(dpy, MASTER_POINTER_ID, grabWin, CurrentTime, None, GrabModeAsync, GrabModeAsync, False, &mask);
        Log("[x11dbg] XIGrabDevice(win=0x%lx) -> rc=%d", static_cast<unsigned long>(grabWin), xiRc);
    }
    else if (!grab && pfnUngrab)
    {
        pfnUngrab(dpy, MASTER_POINTER_ID, CurrentTime);
    }
}

static Display* s_PollDpy   = nullptr;
static bool     s_PollTried = false;
static bool     s_X11Grabbed = false;

static void ReleaseX11GrabIfHeld()
{
    if (s_PollDpy && s_X11Grabbed)
    {
        XUngrabPointer(s_PollDpy, CurrentTime);
        XUngrabKeyboard(s_PollDpy, CurrentTime);
        SetXInput2PointerGrab(s_PollDpy, None, false);
        XFlush(s_PollDpy);
        s_X11Grabbed = false;
    }
}

// ── X11 Keymap/Pointer Poll (catches Wine/Proton/XInput2 games) ──────────────
static void PollX11InputFallback()
{
    if (g_WaylandInputActive.load(std::memory_order_relaxed)) return;
    static const bool s_PureWaylandProton =
        (std::getenv("WAYLAND_DISPLAY") != nullptr && std::getenv("WAYLAND_DISPLAY")[0] != '\0') &&
        (std::getenv("PROTON_ENABLE_WAYLAND") != nullptr && std::strcmp(std::getenv("PROTON_ENABLE_WAYLAND"), "1") == 0);
    if (s_PureWaylandProton && g_SurfaceInfo.x11Window == 0) return;

    if (!s_PollTried)
    {
        s_PollTried = true;
        if (std::getenv("DISPLAY"))
            s_PollDpy = XOpenDisplay(nullptr);
    }
    if (!s_PollDpy) return;

    uint64_t nowMs = Platform_GetTickMs();
    bool menuOpen = g_ShowMenu.load();

    // Throttle XQueryKeymap to 20 Hz when menu is closed to avoid per-frame X11 round-trips
    static uint64_t s_LastKeyPollMs = 0;
    if (menuOpen || (nowMs - s_LastKeyPollMs) >= 16)
    {
        s_LastKeyPollMs = nowMs;
        char keys[32] = {};
        XQueryKeymap(s_PollDpy, keys);

        static const KeySym s_MenuSyms[]   = { XK_Insert, XK_Home, XK_End, XK_F11, XK_grave };
        static const KeySym s_RecordSyms[] = { XK_F9, XK_F10, XK_F11, XK_F12, XK_F8, XK_Scroll_Lock, XK_Pause };
        static int s_CachedMenuIdx = -1, s_CachedRecIdx = -1;
        static KeyCode s_CachedMenuKc = 0, s_CachedRecKc = 0;
        int mIdx = std::clamp(s_MenuHotkeyIdx, 0, 4);
        int rIdx = std::clamp(s_RecordHotkeyIdx, 0, 6);
        if (mIdx != s_CachedMenuIdx) { s_CachedMenuIdx = mIdx; s_CachedMenuKc = XKeysymToKeycode(s_PollDpy, s_MenuSyms[mIdx]); }
        if (rIdx != s_CachedRecIdx)  { s_CachedRecIdx  = rIdx; s_CachedRecKc  = XKeysymToKeycode(s_PollDpy, s_RecordSyms[rIdx]); }

        auto isKcDown = [&](KeyCode kc) -> bool {
            return kc != 0 && (keys[kc / 8] & (1 << (kc % 8))) != 0;
        };

        static bool prevMenu = false, prevRec = false;
        bool curMenu = isKcDown(s_CachedMenuKc);
        bool curRec  = isKcDown(s_CachedRecKc);
        if (nowMs - g_LastEventKeyMs.load() > 150)
        {
            if (curMenu && !prevMenu) Overlay_ToggleMenu();
            if (curRec  && !prevRec)  Overlay_ToggleRecord();
        }
        prevMenu = curMenu;
        prevRec  = curRec;
    }

    auto resolveTargetWindow = [&]() -> Window {
        if (g_SurfaceInfo.x11Window != 0)
            return static_cast<Window>(g_SurfaceInfo.x11Window);
        Window focusWin = None; int revert = 0;
        XGetInputFocus(s_PollDpy, &focusWin, &revert);
        return (focusWin != None && focusWin != PointerRoot) ? focusWin : DefaultRootWindow(s_PollDpy);
    };

    if (menuOpen && !s_X11Grabbed)
    {
        Window grabWin = resolveTargetWindow();
        using PFN_GP = int (*)(Display*, Window, Bool, unsigned int, int, int, Window, Cursor, Time);
        static auto real_gp = reinterpret_cast<PFN_GP>(dlsym(RTLD_NEXT, "XGrabPointer"));
        int gpRc = real_gp ? real_gp(s_PollDpy, grabWin, False,
                         ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                         GrabModeAsync, GrabModeAsync, None, None, CurrentTime) : -99;
        static uint64_t s_LastGrabLogMs = 0;
        if (nowMs - s_LastGrabLogMs >= 1000 || gpRc == GrabSuccess)
        {
            s_LastGrabLogMs = nowMs;
            Log("[x11dbg] XGrabPointer(win=0x%lx surfWin=0x%lx) -> rc=%d (0=Success,1=AlreadyGrabbed,3=NotViewable)",
                static_cast<unsigned long>(grabWin),
                static_cast<unsigned long>(g_SurfaceInfo.x11Window), gpRc);
        }
        if (gpRc == GrabSuccess)
        {
            XGrabKeyboard(s_PollDpy, grabWin, False, GrabModeAsync, GrabModeAsync, CurrentTime);
            s_X11Grabbed = true;
        }
    }
    else if (!menuOpen && s_X11Grabbed)
    {
        ReleaseX11GrabIfHeld();
    }

    if (menuOpen)
    {
        Window targetWin = resolveTargetWindow();
        Window retRoot = None, retChild = None;
        int rootX = 0, rootY = 0, winX = 0, winY = 0;
        unsigned int mask = 0;
        using PFN_XQP = Bool (*)(Display*, Window, Window*, Window*, int*, int*, int*, int*, unsigned int*);
        static auto real_xqp = reinterpret_cast<PFN_XQP>(dlsym(RTLD_NEXT, "XQueryPointer"));
        if (real_xqp && real_xqp(s_PollDpy, targetWin, &retRoot, &retChild, &rootX, &rootY, &winX, &winY, &mask))
        {
            static bool prevL = false, prevR = false, prevM = false;
            bool curL = (mask & Button1Mask) != 0;
            bool curR = (mask & Button3Mask) != 0;
            bool curM = (mask & Button2Mask) != 0;
            std::lock_guard<std::mutex> lk(s_WlCursorMtx);
            s_WlCursorX = static_cast<float>(winX);
            s_WlCursorY = static_cast<float>(winY);
            if (curL != prevL) { s_PendingMouseBtns.emplace_back(0, curL); prevL = curL; }
            if (curR != prevR) { s_PendingMouseBtns.emplace_back(1, curR); prevR = curR; }
            if (curM != prevM) { s_PendingMouseBtns.emplace_back(2, curM); prevM = curM; }
        }
    }
}

VkSemaphore Overlay_OnPresent(const VkCaptureDeviceInfo& devInfo,
                              VkQueue presentQueue,
                              uint32_t imageIndex,
                              uint32_t waitSemaphoreCount,
                              const VkSemaphore* waitSemaphores)
{
    if (!g_OverlayInitialized.load(std::memory_order_relaxed))
        return VK_NULL_HANDLE;

    PollX11InputFallback();

    bool wantMeter = g_ShowMenu.load(std::memory_order_relaxed) || (s_ShowMiniHud && s_MiniHudAudio);
    Capture_SetMeterRequested(wantMeter);
    if (!g_ShowMenu.load(std::memory_order_relaxed) && !s_ShowMiniHud && !Recorder_IsRecording() && !Recorder_IsStarting())
        return VK_NULL_HANDLE;

    std::unique_lock<std::mutex> initLk(g_OverlayInitMtx, std::try_to_lock);
    if (!initLk.owns_lock() || !g_OverlayInitialized.load() || imageIndex >= g_Frames.size())
        return VK_NULL_HANDLE;

    OverlayFrameResources& f = g_Frames[imageIndex];
    if (devInfo.GetFenceStatus && devInfo.GetFenceStatus(devInfo.device, f.fence) != VK_SUCCESS)
        return VK_NULL_HANDLE;

    ImGuiIO& io = ImGui::GetIO();
    uint64_t nowNs = Platform_GetMonotonicNs();
    io.DeltaTime = (s_LastFrameNs > 0) ? std::max(0.0001f, static_cast<float>(nowNs - s_LastFrameNs) / 1e9f) : (1.0f / 60.0f);
    s_LastFrameNs = nowNs;
    io.DisplaySize = ImVec2(static_cast<float>(g_Width), static_cast<float>(g_Height));
    io.MouseDrawCursor = g_ShowMenu.load();
    if (g_ShowMenu.load())
    {
        std::vector<std::pair<int, bool>> btns;
        float cx = -1.0f, cy = -1.0f;
        {
            std::lock_guard<std::mutex> lk(s_WlCursorMtx);
            cx = s_WlCursorX;
            cy = s_WlCursorY;
            btns.swap(s_PendingMouseBtns);
        }
        if (cx >= 0.0f && cy >= 0.0f)
            io.AddMousePosEvent(cx, cy);
        for (const auto& b : btns)
            io.AddMouseButtonEvent(b.first, b.second);
    }

    float wy = s_PendingWheelY.exchange(0.0f);
    float wx = s_PendingWheelX.exchange(0.0f);
    if (g_ShowMenu.load() && (wy != 0.0f || wx != 0.0f))
        io.AddMouseWheelEvent(wx, wy);

    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();

    CaptureStats stats = {};
    Capture_GetStats(&stats);
    UpdateHistory(stats);
    CheckAutoStopTimer();

    if (s_ShowMiniHud) RenderMiniHud(stats, s_MiniHudPos);
    RenderRecordingBeacon();
    RenderControlCenter(stats);

    if (s_ShowDemoWindow)    ImGui::ShowDemoWindow(&s_ShowDemoWindow);
    if (s_ShowMetricsWindow) ImGui::ShowMetricsWindow(&s_ShowMetricsWindow);

    ImGui::Render();

    if (!devInfo.GetFenceStatus)
        devInfo.WaitForFences(devInfo.device, 1, &f.fence, VK_TRUE, UINT64_MAX);
    devInfo.ResetFences(devInfo.device, 1, &f.fence);
    devInfo.ResetCommandBuffer(f.cmdBuf, 0);

    VkCommandBufferBeginInfo cbbi = {};
    cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    devInfo.BeginCommandBuffer(f.cmdBuf, &cbbi);

    VkRenderPassBeginInfo rpbi = {};
    rpbi.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass        = g_RenderPass;
    rpbi.framebuffer       = f.framebuffer;
    rpbi.renderArea.extent = { g_Width, g_Height };
    g_OvVk.CmdBeginRenderPass(f.cmdBuf, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), f.cmdBuf);

    g_OvVk.CmdEndRenderPass(f.cmdBuf);
    devInfo.EndCommandBuffer(f.cmdBuf);

    VkPipelineStageFlags stackWaitStages[16];
    std::vector<VkPipelineStageFlags> heapWaitStages;
    VkPipelineStageFlags* pWaitStages = stackWaitStages;
    if (waitSemaphoreCount > 16)
    {
        heapWaitStages.assign(waitSemaphoreCount, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        pWaitStages = heapWaitStages.data();
    }
    else
    {
        for (uint32_t i = 0; i < waitSemaphoreCount; ++i)
            stackWaitStages[i] = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    }
    VkSubmitInfo si = {};
    si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount   = waitSemaphoreCount;
    si.pWaitSemaphores      = waitSemaphores;
    si.pWaitDstStageMask    = pWaitStages;
    si.commandBufferCount   = 1;
    si.pCommandBuffers      = &f.cmdBuf;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores    = &f.renderSem;

    if (devInfo.QueueSubmit(presentQueue, 1, &si, f.fence) != VK_SUCCESS)
        return VK_NULL_HANDLE;

    return f.renderSem;
}
