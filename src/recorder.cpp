/**
 * recorder.cpp  —  In-game H.264 MP4 recorder (Linux FFmpeg pipe port)
 *
 * Spawns ffmpeg (preferring a static binary next to libvkcapture.so, falling
 * back to `ffmpeg` in $PATH) over an OS pipe from a dedicated worker thread.
 * This keeps libvkcapture.so free of libavcodec/libavformat .so dependencies
 * so it loads cleanly inside Steam Linux Runtime / pressure-vessel containers.
 */

#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <atomic>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <vector>
#include <deque>
#include <string>
#include <unistd.h>
#include <sys/wait.h>
#include <dlfcn.h>
#include <libgen.h>
#include <signal.h>
#include <fcntl.h>

#include "recorder.h"
#include "capture.h"
#include "audio.h"

static constexpr size_t MAX_QUEUE_FRAMES = 128;

struct QueuedFrame
{
    std::vector<uint8_t> pixels;
    uint32_t width    = 0;
    uint32_t height   = 0;
    uint32_t stride   = 0;
    uint32_t vkFormat = 0;
    uint64_t timeNs   = 0;
};

enum class ItemKind { Frame, BeginSession, EndSession };

struct QueuedItem
{
    ItemKind    kind = ItemKind::Frame;
    QueuedFrame frame;
    char        path[MAX_PATH] = "";
    uint32_t    fps            = 30;
    uint32_t    bitrateKbps    = 8000;
    uint32_t    width          = 0;
    uint32_t    height         = 0;
};

// ── State ────────────────────────────────────────────────────────────────────
static std::atomic<bool>     g_IsStarting{ false };
static std::atomic<bool>     g_IsRecording{ false };
static std::atomic<bool>     g_IsPaused{ false };
static std::atomic<uint64_t> g_RecordedFrames{ 0 };
static std::atomic<uint64_t> g_DroppedFrames{ 0 };
static std::atomic<uint64_t> g_LastCaptureNs{ 0 };

static std::atomic<uint32_t> g_TargetFps{ 30 };
static std::atomic<uint32_t> g_BitrateKbps{ 8000 };
static std::atomic<uint32_t> g_EncoderWidth{ 0 };
static std::atomic<uint32_t> g_EncoderHeight{ 0 };
static std::atomic<uint32_t> g_DefaultWidth{ 0 };
static std::atomic<uint32_t> g_DefaultHeight{ 0 };
static std::atomic<uint32_t> g_AudioBitrateKbps{ 192 };
static std::atomic<bool>     g_HardwareAccel{ true };

void Recorder_SetDefaultResolution(uint32_t width, uint32_t height)
{
    if (width > 0 && height > 0)
    {
        g_DefaultWidth.store(width, std::memory_order_relaxed);
        g_DefaultHeight.store(height, std::memory_order_relaxed);
    }
}

static char                  g_CurrentPath[MAX_PATH] = "";
static std::mutex            g_ErrMtx;
static char                  g_LastError[128] = "";

static std::atomic<uint64_t> g_TotalPausedNs{ 0 };
static uint64_t              g_PauseStartNs = 0;
static uint64_t              g_RecordStartMs = 0;
static uint64_t              g_TotalPausedMs = 0;
static uint64_t              g_PauseStartMs = 0;

static std::mutex                        g_QueueMtx;
static std::condition_variable           g_QueueCv;
static std::deque<QueuedItem>            g_Queue;
static std::vector<std::vector<uint8_t>> g_BufferPool;
static std::atomic<bool>                 g_WorkerRunning{ false };

static std::mutex                        g_SessionDoneMtx;
static std::condition_variable           g_SessionDoneCv;
static std::atomic<bool>                 g_SessionPendingOrActive{ false };
static std::atomic<bool>                 g_AutoSaveOnExit{ true };
static std::atomic<bool>                 g_WorkerFinished{ true };
static std::atomic<bool>                 g_DaemonRecording{ false };

// ── Worker-local EncodeSession ───────────────────────────────────────────────
struct EncodeSession
{
    pid_t       ffmpegPid          = -1;
    int         pipeWriteFd        = -1;
    uint32_t    width              = 0;
    uint32_t    height             = 0;
    uint32_t    fps                = 30;
    uint32_t    bitrateKbps        = 8000;
    uint64_t    baseNs             = 0;
    uint64_t    framesWritten      = 0;
    bool        active             = false;
    bool        warnedSizeMismatch = false;
    char        path[MAX_PATH]     = "";
};

static bool IsRgbaVkFormat(uint32_t vkFormat)
{
    return (vkFormat >= 37 && vkFormat <= 43) || (vkFormat >= 51 && vkFormat <= 57);
}

static void SetRecorderError(const char* fmt, ...)
{
    char msg[sizeof(g_LastError)] = "";
    if (fmt && *fmt)
    {
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(msg, sizeof(msg), fmt, args);
        va_end(args);
        Log("[rec] %s", msg);
    }
    std::lock_guard<std::mutex> lock(g_ErrMtx);
    Platform_SafeStrCpy(g_LastError, sizeof(g_LastError), msg);
}

static void GenerateDefaultFileName(char* dst, size_t maxLen)
{
    char recDir[MAX_PATH];
    std::snprintf(recDir, sizeof(recDir), "%s/recordings", Platform_GetBaseDir());
    Platform_EnsureDir(recDir);

    struct timespec ts = {};
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tmLocal = {};
    localtime_r(&ts.tv_sec, &tmLocal);

    std::snprintf(dst, maxLen,
                  "%s/recording_%04d%02d%02d_%02d%02d%02d.mp4",
                  recDir,
                  tmLocal.tm_year + 1900, tmLocal.tm_mon + 1, tmLocal.tm_mday,
                  tmLocal.tm_hour, tmLocal.tm_min, tmLocal.tm_sec);
}

static std::string ResolveFfmpegBinary()
{
    Dl_info dl = {};
    if (dladdr(reinterpret_cast<void*>(&ResolveFfmpegBinary), &dl) && dl.dli_fname)
    {
        char buf[MAX_PATH];
        Platform_SafeStrCpy(buf, sizeof(buf), dl.dli_fname);
        char* dir = dirname(buf);
        std::string localFfmpeg = std::string(dir) + "/ffmpeg";
        if (access(localFfmpeg.c_str(), X_OK) == 0)
            return localFfmpeg;
    }
    return "ffmpeg";
}

static bool WriteAll(int fd, const void* data, size_t len)
{
    const uint8_t* p = static_cast<const uint8_t*>(data);
    while (len > 0)
    {
        ssize_t n = write(fd, p, len);
        if (n < 0)
        {
            if (errno == EINTR) continue;
            return false;
        }
        p   += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

static bool OpenSession(EncodeSession& s, uint32_t width, uint32_t height, uint32_t vkFormat)
{
    int pipeFds[2] = { -1, -1 };
    if (pipe(pipeFds) != 0)
    {
        SetRecorderError("pipe() failed: %s", std::strerror(errno));
        return false;
    }

    std::string ffmpegBin = ResolveFfmpegBinary();
    std::string sizeStr   = std::to_string(width) + "x" + std::to_string(height);
    std::string fpsStr    = std::to_string(s.fps ? s.fps : 30);
    std::string brStr     = std::to_string(s.bitrateKbps ? s.bitrateKbps : 8000) + "k";
    const char* pixFmt    = IsRgbaVkFormat(vkFormat) ? "rgba" : "bgra";

    pid_t pid = fork();
    if (pid < 0)
    {
        SetRecorderError("fork() failed: %s", std::strerror(errno));
        close(pipeFds[0]);
        close(pipeFds[1]);
        return false;
    }

    if (pid == 0)
    {
        dup2(pipeFds[0], STDIN_FILENO);
        close(pipeFds[0]);
        close(pipeFds[1]);
        char errLog[MAX_PATH];
        std::snprintf(errLog, sizeof(errLog), "%s/ffmpeg_error.log", Platform_GetBaseDir());
        int errFd = open(errLog, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (errFd >= 0) { dup2(errFd, STDERR_FILENO); close(errFd); }

        const char* argv[] = {
            ffmpegBin.c_str(),
            "-y",
            "-loglevel", "error",
            "-f", "rawvideo",
            "-pix_fmt", pixFmt,
            "-video_size", sizeStr.c_str(),
            "-framerate", fpsStr.c_str(),
            "-i", "pipe:0",
            "-c:v", "libx264",
            "-preset", "veryfast",
            "-tune", "zerolatency",
            "-b:v", brStr.c_str(),
            "-pix_fmt", "yuv420p",
            s.path,
            nullptr
        };
        unsetenv("LD_PRELOAD");
        unsetenv("ENABLE_VKCAPTURE");
        execvp(ffmpegBin.c_str(), const_cast<char* const*>(argv));
        _exit(127);
    }

    close(pipeFds[0]);
    s.ffmpegPid   = pid;
    s.pipeWriteFd = pipeFds[1];
    s.width       = width;
    s.height      = height;
    g_EncoderWidth.store(width);
    g_EncoderHeight.store(height);

    Log("[rec] FFmpeg pipe encoder started (%s): %ux%u @ %u fps, %u kbps -> %s",
        ffmpegBin.c_str(), width, height, s.fps, s.bitrateKbps, s.path);
    return true;
}

static void CloseSession(EncodeSession& s)
{
    if (s.pipeWriteFd >= 0)
    {
        close(s.pipeWriteFd);
        s.pipeWriteFd = -1;
    }
    if (s.ffmpegPid > 0)
    {
        Log("[rec] Finalizing MP4 via ffmpeg (pid=%d)...", static_cast<int>(s.ffmpegPid));
        int status = 0;
        waitpid(s.ffmpegPid, &status, 0);
        s.ffmpegPid = -1;
        if (WIFEXITED(status))
            Log("[rec] Finalized (ffmpeg exit=%d). Total recorded frames: %llu -> %s",
                WEXITSTATUS(status), static_cast<unsigned long long>(s.framesWritten), s.path);
        else if (WIFSIGNALED(status))
            Log("[rec] Finalized (ffmpeg killed by signal %d). Total recorded frames: %llu -> %s",
                WTERMSIG(status), static_cast<unsigned long long>(s.framesWritten), s.path);
    }
    s.active = false;
    g_SessionPendingOrActive.store(false);
    g_SessionDoneCv.notify_all();
}

static bool EncodeFrame(EncodeSession& s, const QueuedFrame& frame)
{
    if (s.pipeWriteFd < 0) return false;

    if (frame.width != s.width || frame.height != s.height)
    {
        if (!s.warnedSizeMismatch)
        {
            Log("[rec] Dropping %ux%u frame; this session encodes %ux%u",
                frame.width, frame.height, s.width, s.height);
            s.warnedSizeMismatch = true;
        }
        return false;
    }

    const size_t frameBytes = static_cast<size_t>(s.width) * s.height * 4;
    if (!WriteAll(s.pipeWriteFd, frame.pixels.data(), frameBytes))
    {
        SetRecorderError("ffmpeg pipe write failed (check ffmpeg_error.log)");
        CloseSession(s);
        g_IsStarting.store(false);
        g_IsRecording.store(false);
        return false;
    }

    ++s.framesWritten;
    g_RecordedFrames.fetch_add(1);
    return true;
}

// ── Queue helpers ────────────────────────────────────────────────────────────
static void RecycleBufferLocked(std::vector<uint8_t>&& pixels)
{
    if (g_BufferPool.size() < MAX_QUEUE_FRAMES)
    {
        pixels.clear();
        g_BufferPool.push_back(std::move(pixels));
    }
}

static size_t QueuedFrameCountLocked()
{
    size_t n = 0;
    for (const QueuedItem& i : g_Queue)
        if (i.kind == ItemKind::Frame) ++n;
    return n;
}

static void DropOldestFrameLocked()
{
    for (auto it = g_Queue.begin(); it != g_Queue.end(); ++it)
    {
        if (it->kind == ItemKind::Frame)
        {
            RecycleBufferLocked(std::move(it->frame.pixels));
            g_Queue.erase(it);
            g_DroppedFrames.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}

static void RecorderWorkerLoop()
{
    signal(SIGPIPE, SIG_IGN);
    Log("[rec] Background encoding worker thread started");
    EncodeSession session;

    for (;;)
    {
        QueuedItem item;
        {
            std::unique_lock<std::mutex> lock(g_QueueMtx);
            g_QueueCv.wait(lock, [] {
                return !g_Queue.empty() || !g_WorkerRunning.load();
            });

            if (g_Queue.empty())
            {
                if (!g_WorkerRunning.load()) break;
                continue;
            }

            item = std::move(g_Queue.front());
            g_Queue.pop_front();
        }

        switch (item.kind)
        {
        case ItemKind::BeginSession:
            CloseSession(session);
            session = EncodeSession{};
            Platform_SafeStrCpy(session.path, sizeof(session.path), item.path);
            session.fps         = item.fps;
            session.bitrateKbps = item.bitrateKbps;
            session.active      = true;
            g_SessionPendingOrActive.store(true);
            g_RecordedFrames.store(0);

            if (item.width > 0 && item.height > 0)
            {
                if (OpenSession(session, item.width, item.height, 44))
                {
                    g_RecordedFrames.store(0);
                    g_DroppedFrames.store(0);
                    g_LastCaptureNs.store(0);
                    g_RecordStartMs = Platform_GetTickMs();
                    g_TotalPausedMs = 0;
                    g_IsStarting.store(false);
                    g_IsRecording.store(true);
                    g_SessionDoneCv.notify_all();
                }
                else
                {
                    CloseSession(session);
                    g_IsStarting.store(false);
                    g_IsRecording.store(false);
                }
            }
            break;

        case ItemKind::Frame:
            if (!session.active) break;

            if (session.pipeWriteFd < 0)
            {
                if (!OpenSession(session, item.frame.width, item.frame.height, item.frame.vkFormat))
                {
                    CloseSession(session);
                    g_IsStarting.store(false);
                    g_IsRecording.store(false);
                    break;
                }
                g_RecordedFrames.store(0);
                g_DroppedFrames.store(0);
                g_LastCaptureNs.store(0);
                g_RecordStartMs = Platform_GetTickMs();
                g_TotalPausedMs = 0;
                g_IsStarting.store(false);
                g_IsRecording.store(true);
                g_SessionDoneCv.notify_all();
            }

            EncodeFrame(session, item.frame);
            break;

        case ItemKind::EndSession:
            CloseSession(session);
            break;
        }

        if (item.kind == ItemKind::Frame)
        {
            std::lock_guard<std::mutex> lock(g_QueueMtx);
            RecycleBufferLocked(std::move(item.frame.pixels));
        }
    }

    CloseSession(session);

    {
        std::lock_guard<std::mutex> lock(g_QueueMtx);
        g_Queue.clear();
        g_BufferPool.clear();
        g_BufferPool.shrink_to_fit();
    }

    Log("[rec] Background encoding worker thread finished");
    g_WorkerFinished.store(true);
    g_SessionPendingOrActive.store(false);
    g_SessionDoneCv.notify_all();
}

// ── Public API ───────────────────────────────────────────────────────────────
void Recorder_Init()
{
    char recDir[MAX_PATH];
    std::snprintf(recDir, sizeof(recDir), "%s/recordings", Platform_GetBaseDir());
    Platform_EnsureDir(recDir);

    if (!g_WorkerRunning.exchange(true))
    {
        g_WorkerFinished.store(false);
        std::thread(RecorderWorkerLoop).detach();
    }
}

void Recorder_Shutdown()
{
    Recorder_StopSync(3000);
    g_WorkerRunning.store(false);
    g_QueueCv.notify_all();
    for (int i = 0; i < 200 && !g_WorkerFinished.load(); ++i)
        usleep(10000);
}

bool Recorder_Start(const char* customPath, uint32_t fps, uint32_t bitrateKbps, uint32_t width, uint32_t height)
{
    if (g_IsRecording.load() || g_IsStarting.load()) return false;
    if (!g_WorkerRunning.load()) return false;

    if (width == 0 || height == 0)
    {
        width  = g_DefaultWidth.load(std::memory_order_relaxed);
        height = g_DefaultHeight.load(std::memory_order_relaxed);
    }

    g_TargetFps.store(fps > 0 ? fps : 30);
    g_BitrateKbps.store(bitrateKbps > 0 ? bitrateKbps : 8000);
    g_RecordedFrames.store(0);
    g_DroppedFrames.store(0);
    g_LastCaptureNs.store(0);
    g_EncoderWidth.store(width);
    g_EncoderHeight.store(height);
    g_TotalPausedNs.store(0);
    g_PauseStartNs = 0;
    g_TotalPausedMs = 0;
    g_PauseStartMs = 0;
    SetRecorderError("");

    if (customPath && customPath[0] != '\0')
        Platform_SafeStrCpy(g_CurrentPath, sizeof(g_CurrentPath), customPath);
    else
        GenerateDefaultFileName(g_CurrentPath, sizeof(g_CurrentPath));

    g_RecordStartMs = Platform_GetTickMs();

    if (Capture_IsDaemonActive())
    {
        g_DaemonRecording.store(true);
        g_IsPaused.store(false);
        g_IsStarting.store(false);
        g_IsRecording.store(true);
        Capture_SetTargetFps(static_cast<int>(g_TargetFps.load()));
        Capture_SetDaemonRecord(true, g_TargetFps.load(), g_BitrateKbps.load(), g_CurrentPath, nullptr);
        Log("[rec] Delegated recording to host shm_reader --daemon -> %s (%ux%u @ %u FPS)",
            g_CurrentPath, width, height, g_TargetFps.load());
        return true;
    }

    QueuedItem begin;
    begin.kind        = ItemKind::BeginSession;
    begin.fps         = g_TargetFps.load();
    begin.bitrateKbps = g_BitrateKbps.load();
    begin.width       = width;
    begin.height      = height;
    Platform_SafeStrCpy(begin.path, sizeof(begin.path), g_CurrentPath);
    {
        std::lock_guard<std::mutex> lock(g_QueueMtx);
        g_Queue.push_back(std::move(begin));
    }
    g_QueueCv.notify_one();

    g_IsPaused.store(false);
    g_IsStarting.store(true);
    g_IsRecording.store(width == 0 || height == 0);
    g_SessionPendingOrActive.store(true);
    return true;
}

bool Recorder_WaitForReady(uint32_t timeoutMs)
{
    if (g_IsRecording.load()) return true;
    if (!g_IsStarting.load()) return false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    std::unique_lock<std::mutex> lock(g_SessionDoneMtx);
    return g_SessionDoneCv.wait_until(lock, deadline, [] {
        return g_IsRecording.load() || !g_IsStarting.load() || !g_WorkerRunning.load();
    }) && g_IsRecording.load();
}

bool Recorder_IsStarting() { return g_IsStarting.load(); }

void Recorder_Pause()
{
    if (!g_IsRecording.load() || g_IsPaused.load()) return;
    g_PauseStartNs = Platform_GetMonotonicNs();
    g_PauseStartMs = Platform_GetTickMs();
    g_IsPaused.store(true);
    if (g_DaemonRecording.load()) Capture_SetDaemonPaused(true);
}

void Recorder_Resume()
{
    if (!g_IsRecording.load() || !g_IsPaused.load()) return;
    uint64_t nowNs = Platform_GetMonotonicNs();
    if (nowNs >= g_PauseStartNs) g_TotalPausedNs.fetch_add(nowNs - g_PauseStartNs);
    g_TotalPausedMs += (Platform_GetTickMs() - g_PauseStartMs);
    g_IsPaused.store(false);
    if (g_DaemonRecording.load()) Capture_SetDaemonPaused(false);
}

void Recorder_Stop()
{
    if (!g_IsRecording.load() && !g_IsStarting.load()) return;
    g_IsStarting.store(false);
    g_IsRecording.store(false);
    g_IsPaused.store(false);
    if (g_DaemonRecording.exchange(false))
    {
        Capture_SetDaemonRecord(false, 0, 0, nullptr, nullptr);
        Log("[rec] Stopped host daemon recording");
        return;
    }

    QueuedItem end;
    end.kind = ItemKind::EndSession;
    {
        std::lock_guard<std::mutex> lock(g_QueueMtx);
        g_Queue.push_back(std::move(end));
    }
    g_QueueCv.notify_one();
}

bool Recorder_StopSync(uint32_t timeoutMs)
{
    static std::mutex s_StopSyncMtx;
    std::lock_guard<std::mutex> syncLock(s_StopSyncMtx);
    if (!g_IsRecording.load() && !g_IsStarting.load() && !g_SessionPendingOrActive.load())
        return true;

    g_IsStarting.store(false);
    g_IsRecording.store(false);
    g_IsPaused.store(false);
    if (g_DaemonRecording.exchange(false))
    {
        Capture_SetDaemonRecord(false, 0, 0, nullptr, nullptr);
        Capture_SetTargetFps(0);
        return true;
    }
    if (!g_WorkerRunning.load() || g_WorkerFinished.load())
    {
        g_SessionPendingOrActive.store(false);
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(g_QueueMtx);
        bool hasEnd = false;
        for (const auto& item : g_Queue)
            if (item.kind == ItemKind::EndSession) { hasEnd = true; break; }
        if (!hasEnd)
        {
            QueuedItem end;
            end.kind = ItemKind::EndSession;
            g_Queue.push_back(std::move(end));
        }
    }
    g_QueueCv.notify_all();

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    std::unique_lock<std::mutex> lock(g_SessionDoneMtx);
    return g_SessionDoneCv.wait_until(lock, deadline, [] {
        return !g_SessionPendingOrActive.load() || g_WorkerFinished.load();
    });
}

void Recorder_SetAutoSaveOnExit(bool enable) { g_AutoSaveOnExit.store(enable); }
bool Recorder_GetAutoSaveOnExit()            { return g_AutoSaveOnExit.load(); }
bool Recorder_IsRecording()                  { return g_IsRecording.load(); }
bool Recorder_IsPaused()                     { return g_IsPaused.load(); }

void Recorder_GetStats(RecorderStats* outStats)
{
    if (!outStats) return;
    outStats->isRecording    = g_IsRecording.load();
    outStats->isStarting     = g_IsStarting.load();
    outStats->isPaused       = g_IsPaused.load();
    if (g_DaemonRecording.load())
    {
        outStats->recordedFrames = Capture_GetDaemonRecordedFrames();
    }
    else
    {
        outStats->recordedFrames = g_RecordedFrames.load();
    }
    outStats->droppedFrames  = g_DroppedFrames.load(std::memory_order_relaxed);
    outStats->width          = g_EncoderWidth.load();
    outStats->height         = g_EncoderHeight.load();
    outStats->fps            = g_TargetFps.load();
    outStats->bitrateKbps    = g_BitrateKbps.load();
    Platform_SafeStrCpy(outStats->outputPath, sizeof(outStats->outputPath), g_CurrentPath);
    {
        std::lock_guard<std::mutex> lock(g_ErrMtx);
        Platform_SafeStrCpy(outStats->lastError, sizeof(outStats->lastError), g_LastError);
    }

    if (g_IsRecording.load())
    {
        uint64_t now = Platform_GetTickMs();
        uint64_t elapsed = (now >= g_RecordStartMs + g_TotalPausedMs) ? (now - g_RecordStartMs - g_TotalPausedMs) : 0;
        if (g_IsPaused.load() && now >= g_PauseStartMs && elapsed >= (now - g_PauseStartMs))
            elapsed -= (now - g_PauseStartMs);
        outStats->durationMs = elapsed;
    }
    else
    {
        outStats->durationMs = 0;
    }
}

bool Recorder_WantsFrame()
{
    if (g_DaemonRecording.load() || !g_IsRecording.load() || g_IsPaused.load()) return false;
    const uint32_t targetFps = g_TargetFps.load();
    if (targetFps == 0) return true;

    uint64_t nowNs = Platform_GetMonotonicNs();
    uint64_t lastNs = g_LastCaptureNs.load(std::memory_order_relaxed);
    if (lastNs != 0)
    {
        uint64_t intervalNs = 1000000000ULL / targetFps;
        uint64_t minNs = (intervalNs * 70ULL) / 100ULL;
        if ((nowNs - lastNs) < minNs) return false;
    }
    g_LastCaptureNs.store(nowNs, std::memory_order_relaxed);
    return true;
}

void Recorder_OnFrameReady(const void* pixels, uint32_t width, uint32_t height,
                           uint32_t stride, uint32_t vkFormat, uint64_t frameIdx)
{
    (void)frameIdx;
    if (!g_IsRecording.load() || g_IsPaused.load() || !pixels) return;

    uint64_t nowNs = Platform_GetMonotonicNs();
    const size_t byteSize = static_cast<size_t>(width) * height * 4;

    QueuedItem item;
    item.kind           = ItemKind::Frame;
    item.frame.width    = width;
    item.frame.height   = height;
    item.frame.stride   = width * 4;
    item.frame.vkFormat = vkFormat;
    item.frame.timeNs   = nowNs;

    {
        std::unique_lock<std::mutex> lock(g_QueueMtx);
        if (QueuedFrameCountLocked() >= MAX_QUEUE_FRAMES)
            DropOldestFrameLocked();
        if (!g_BufferPool.empty())
        {
            item.frame.pixels = std::move(g_BufferPool.back());
            g_BufferPool.pop_back();
        }
    }

    if (item.frame.pixels.size() < byteSize)
        item.frame.pixels.resize(byteSize);

    const uint8_t* src = static_cast<const uint8_t*>(pixels);
    uint8_t* dst = item.frame.pixels.data();
    const uint32_t rowBytes = width * 4;
    if (stride == rowBytes)
    {
        std::memcpy(dst, src, byteSize);
    }
    else
    {
        for (uint32_t y = 0; y < height; ++y, src += stride, dst += rowBytes)
            std::memcpy(dst, src, rowBytes);
    }

    {
        std::lock_guard<std::mutex> lock(g_QueueMtx);
        g_Queue.push_back(std::move(item));
    }
    g_QueueCv.notify_one();
}

void     Recorder_SetAudioBitrate(uint32_t kbps) { g_AudioBitrateKbps.store(kbps > 0 ? kbps : 192); }
uint32_t Recorder_GetAudioBitrate()              { return g_AudioBitrateKbps.load(); }
void     Recorder_SetHardwareAccel(bool enable)  { g_HardwareAccel.store(enable); }
bool     Recorder_GetHardwareAccel()             { return g_HardwareAccel.load(); }
