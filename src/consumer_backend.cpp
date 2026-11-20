/**
 * consumer_backend.cpp  —  4-slot POSIX shared-memory ring & BMP consumer
 */

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <ctime>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "capture.h"
#include "recorder.h"
#include "audio.h"

static constexpr int    DUMP_FRAMES       = 0;
static constexpr int    SHM_SLOTS         = 4;
static constexpr size_t SHM_SLOT_BYTES    = 3840ULL * 2160ULL * 4ULL + 128ULL; // up to 4K RGBA per slot
static constexpr size_t SHM_TOTAL_BYTES   = 4096ULL + SHM_SLOTS * SHM_SLOT_BYTES;
static constexpr char   SHM_NAME[]        = "/vkcapture_shm_v3";

#pragma pack(push, 1)
struct ShmSlotHeader
{
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    uint64_t frameIdx;
    uint64_t timestampNs;
    uint32_t dataOffset; // relative to start of this slot
    uint32_t sessionId;
};

struct ShmControlHeader
{
    uint32_t magic;            // 0x564B4332 ('VKC2')
    uint32_t numSlots;
    uint64_t slotBytes;
    uint32_t writeIdx;         // producer slot index (mod SHM_SLOTS)
    uint32_t readIdx;          // consumer slot index (mod SHM_SLOTS)
    uint32_t recordRequested;  // 1 = record active, 0 = idle/stop
    uint32_t recordFps;
    uint32_t recordBitrateKbps;
    uint64_t daemonHeartbeatNs;
    uint64_t daemonRecordedFrames;
    uint32_t recordSessionId;
    uint32_t gamePid;
    uint32_t recordAudio;
    int32_t  daemonAudioPeakMilliDb;
    uint32_t menuOpen;
    uint32_t meterRequested;
    char     recordPath[512];
};
#pragma pack(pop)

static constexpr uint32_t SHM_MAGIC = 0x564B4332u;

// ── module state ──────────────────────────────────────────────────────────────
static int                g_ShmFd       = -1;
static void*                   g_pView       = nullptr;
static const ShmControlHeader* g_RoCtrl      = nullptr;
static int                     g_RoFd        = -1;
static std::atomic<int>   g_DumpCount   { 0 };
static std::atomic<int>   g_PendingArms { 0 };

static std::atomic<bool>  g_SnapshotPending{ false };
static std::atomic<int>   g_DumpQuota{ 0 };
static char               g_LastScreenshotPath[MAX_PATH] = "";
static std::mutex         g_ScreenshotMtx;

void Capture_TriggerSnapshot()     { g_SnapshotPending.store(true); }
bool Capture_IsSnapshotPending()   { return g_SnapshotPending.load(); }
bool Capture_TakeSnapshotPending() { return g_SnapshotPending.exchange(false); }

void Capture_SetLastScreenshotPath(const char* path)
{
    std::lock_guard<std::mutex> lk(g_ScreenshotMtx);
    Platform_SafeStrCpy(g_LastScreenshotPath, sizeof(g_LastScreenshotPath), path);
}

void Capture_GetLastScreenshotPath(char* dst, size_t maxLen)
{
    std::lock_guard<std::mutex> lk(g_ScreenshotMtx);
    Platform_SafeStrCpy(dst, maxLen, g_LastScreenshotPath);
}

void Capture_SetDumpQuota(int count) { g_DumpQuota.store(count); }
int  Capture_GetDumpQuota()          { return g_DumpQuota.load(); }

// ── BMP writer ────────────────────────────────────────────────────────────────
#pragma pack(push, 1)
struct BmpFileHeader
{
    uint16_t bfType;
    uint32_t bfSize;
    uint16_t bfReserved1;
    uint16_t bfReserved2;
    uint32_t bfOffBits;
};
struct BmpInfoHeader
{
    uint32_t biSize;
    int32_t  biWidth;
    int32_t  biHeight;
    uint16_t biPlanes;
    uint16_t biBitCount;
    uint32_t biCompression;
    uint32_t biSizeImage;
    int32_t  biXPelsPerMeter;
    int32_t  biYPelsPerMeter;
    uint32_t biClrUsed;
    uint32_t biClrImportant;
};
#pragma pack(pop)

static bool IsRgbaOrder(uint32_t vkFormat)
{
    return (vkFormat >= 37 && vkFormat <= 43) || (vkFormat >= 51 && vkFormat <= 57);
}

static bool WriteBmp(const char* path, const FrameData& f)
{
    BmpFileHeader bfh = {};
    BmpInfoHeader bih = {};

    uint32_t pixelBytes = f.width * f.height * 4;
    bfh.bfType    = 0x4D42;
    bfh.bfSize    = sizeof(bfh) + sizeof(bih) + pixelBytes;
    bfh.bfOffBits = sizeof(bfh) + sizeof(bih);

    bih.biSize        = sizeof(bih);
    bih.biWidth       = static_cast<int32_t>(f.width);
    bih.biHeight      = -static_cast<int32_t>(f.height);
    bih.biPlanes      = 1;
    bih.biBitCount    = 32;
    bih.biCompression = 0;

    FILE* fp = std::fopen(path, "wb");
    if (!fp) return false;

    std::fwrite(&bfh, sizeof(bfh), 1, fp);
    std::fwrite(&bih, sizeof(bih), 1, fp);

    const uint8_t* row = static_cast<const uint8_t*>(f.pixels);
    uint32_t rowBytes = f.width * 4;
    bool swapRB = IsRgbaOrder(f.format);

    if (!swapRB)
    {
        for (uint32_t y = 0; y < f.height; ++y, row += f.stride)
            std::fwrite(row, rowBytes, 1, fp);
    }
    else
    {
        std::vector<uint8_t> tmp(rowBytes);
        for (uint32_t y = 0; y < f.height; ++y, row += f.stride)
        {
            for (uint32_t x = 0; x < f.width; ++x)
            {
                tmp[x * 4 + 0] = row[x * 4 + 2];
                tmp[x * 4 + 1] = row[x * 4 + 1];
                tmp[x * 4 + 2] = row[x * 4 + 0];
                tmp[x * 4 + 3] = row[x * 4 + 3];
            }
            std::fwrite(tmp.data(), rowBytes, 1, fp);
        }
    }

    std::fclose(fp);
    return true;
}

// ── shared-memory init/shutdown ───────────────────────────────────────────────
static void InitSharedMemory()
{
    Log("[shm] InitSharedMemory ...");
    g_ShmFd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (g_ShmFd < 0)
    {
        Log("[shm] shm_open(%s) failed: %s", SHM_NAME, std::strerror(errno));
        return;
    }

    if (ftruncate(g_ShmFd, static_cast<off_t>(SHM_TOTAL_BYTES)) != 0)
    {
        Log("[shm] ftruncate failed: %s", std::strerror(errno));
        close(g_ShmFd);
        g_ShmFd = -1;
        return;
    }

    g_pView = mmap(nullptr, SHM_TOTAL_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, g_ShmFd, 0);
    if (g_pView == MAP_FAILED)
    {
        Log("[shm] mmap failed: %s", std::strerror(errno));
        g_pView = nullptr;
        close(g_ShmFd);
        g_ShmFd = -1;
        return;
    }

    ShmControlHeader* ctrl = static_cast<ShmControlHeader*>(g_pView);
    if (ctrl->magic != SHM_MAGIC)
    {
        std::memset(ctrl, 0, sizeof(*ctrl));
        ctrl->magic     = SHM_MAGIC;
        ctrl->numSlots  = SHM_SLOTS;
        ctrl->slotBytes = SHM_SLOT_BYTES;
    }

    ctrl->gamePid = static_cast<uint32_t>(getpid());
    __atomic_store_n(&ctrl->daemonAudioPeakMilliDb, -96000, __ATOMIC_RELAXED);
    Log("[shm] 4-slot lock-free POSIX shared memory initialized (%s)", SHM_NAME);
}

static void ShutdownSharedMemory()
{
    if (g_pView) { auto* c = static_cast<ShmControlHeader*>(g_pView); __atomic_store_n(&c->menuOpen, 0u, __ATOMIC_RELEASE); __atomic_store_n(&c->meterRequested, 0u, __ATOMIC_RELAXED); munmap(g_pView, SHM_TOTAL_BYTES); g_pView = nullptr; }
    if (g_RoCtrl) { munmap(const_cast<ShmControlHeader*>(g_RoCtrl), 4096); g_RoCtrl = nullptr; }
    if (g_RoFd >= 0) { close(g_RoFd); g_RoFd = -1; }
    if (g_ShmFd >= 0) { close(g_ShmFd); g_ShmFd = -1; }
}

// ── public API ────────────────────────────────────────────────────────────────
void Capture_Init()
{
    Log("[con] Capture_Init");
    Platform_GetBaseDir();
    InitSharedMemory();
    Audio_Init();
    Recorder_Init();
}

void Capture_Shutdown()
{
    Recorder_Shutdown();
    Audio_Shutdown();
    ShutdownSharedMemory();
    Capture_ReleaseSurfaces();
}

bool Capture_IsDaemonActive()
{
    if (!g_pView) return false;
    const ShmControlHeader* ctrl = static_cast<const ShmControlHeader*>(g_pView);
    uint64_t hb = __atomic_load_n(&ctrl->daemonHeartbeatNs, __ATOMIC_RELAXED);
    if (hb == 0) return false;
    uint64_t now = Platform_GetMonotonicNs();
    return (now >= hb) && ((now - hb) < 3000000000ULL);
}

void Capture_SetDaemonRecord(bool active, uint32_t fps, uint32_t bitrateKbps,
                             const char* path, uint64_t* outRecordedFrames)
{
    if (!g_pView) return;
    ShmControlHeader* ctrl = static_cast<ShmControlHeader*>(g_pView);
    if (active)
    {
        if (__atomic_load_n(&ctrl->recordRequested, __ATOMIC_ACQUIRE) != 0)
        {
            if (outRecordedFrames)
                *outRecordedFrames = __atomic_load_n(&ctrl->daemonRecordedFrames, __ATOMIC_RELAXED);
            return;
        }
        g_PendingArms.store(0);
        __atomic_store_n(&ctrl->writeIdx, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&ctrl->readIdx,  0, __ATOMIC_RELAXED);
        for (uint32_t i = 0; i < SHM_SLOTS; ++i)
        {
            uint8_t* sb = static_cast<uint8_t*>(g_pView) + 4096ULL + static_cast<size_t>(i) * SHM_SLOT_BYTES;
            ShmSlotHeader* sh = reinterpret_cast<ShmSlotHeader*>(sb);
            sh->frameIdx  = 0;
            sh->sessionId = 0;
        }
        uint32_t sid = ctrl->recordSessionId + 1;
        if (sid == 0) sid = 1;
        ctrl->recordSessionId   = sid;
        ctrl->recordFps         = fps;
        ctrl->recordBitrateKbps = bitrateKbps;
        ctrl->recordAudio       = Audio_IsEnabled() ? 1u : 0u;
        Platform_SafeStrCpy(ctrl->recordPath, sizeof(ctrl->recordPath), path ? path : "");
        __atomic_store_n(&ctrl->recordRequested, 1, __ATOMIC_RELEASE);
    }
    else
    {
        __atomic_store_n(&ctrl->recordRequested, 0, __ATOMIC_RELEASE);
    }
    if (outRecordedFrames)
        *outRecordedFrames = __atomic_load_n(&ctrl->daemonRecordedFrames, __ATOMIC_RELAXED);
}

void Capture_SetDaemonPaused(bool paused)
{
    if (!g_pView) return;
    ShmControlHeader* ctrl = static_cast<ShmControlHeader*>(g_pView);
    if (__atomic_load_n(&ctrl->recordRequested, __ATOMIC_ACQUIRE) == 0) return;
    __atomic_store_n(&ctrl->recordRequested, paused ? 2u : 1u, __ATOMIC_RELEASE);
}

uint64_t Capture_GetDaemonRecordedFrames()
{
    if (!g_pView) return 0;
    const ShmControlHeader* ctrl = static_cast<const ShmControlHeader*>(g_pView);
    return __atomic_load_n(&ctrl->daemonRecordedFrames, __ATOMIC_RELAXED);
}

float Capture_GetDaemonAudioPeakDb()
{
    if (!g_pView) return -96.0f;
    const ShmControlHeader* ctrl = static_cast<const ShmControlHeader*>(g_pView);
    int32_t mdb = __atomic_load_n(&ctrl->daemonAudioPeakMilliDb, __ATOMIC_RELAXED);
    return static_cast<float>(mdb) / 1000.0f;
}

void Capture_SetMeterRequested(bool active)
{
    if (!g_pView) return;
    ShmControlHeader* ctrl = static_cast<ShmControlHeader*>(g_pView);
    __atomic_store_n(&ctrl->meterRequested, active ? 1u : 0u, __ATOMIC_RELAXED);
}

void Capture_SetSharedMenuOpen(bool open)
{
    if (!g_pView) return;
    ShmControlHeader* ctrl = static_cast<ShmControlHeader*>(g_pView);
    __atomic_store_n(&ctrl->menuOpen, open ? 1u : 0u, __ATOMIC_RELEASE);
}

bool Capture_IsSharedMenuOpen()
{
    if (g_pView)
    {
        const ShmControlHeader* ctrl = static_cast<const ShmControlHeader*>(g_pView);
        return __atomic_load_n(&ctrl->menuOpen, __ATOMIC_RELAXED) != 0;
    }
    if (!g_RoCtrl)
    {
        static std::atomic<uint64_t> s_LastTryMs{ 0 };
        uint64_t now = Platform_GetTickMs();
        uint64_t prev = s_LastTryMs.load(std::memory_order_relaxed);
        if (now - prev >= 1000 && s_LastTryMs.compare_exchange_strong(prev, now))
        {
            int fd = shm_open(SHM_NAME, O_RDONLY, 0);
            if (fd >= 0)
            {
                void* p = mmap(nullptr, 4096, PROT_READ, MAP_SHARED, fd, 0);
                if (p != MAP_FAILED)
                {
                    g_RoFd = fd;
                    g_RoCtrl = static_cast<const ShmControlHeader*>(p);
                }
                else
                {
                    close(fd);
                }
            }
        }
    }
    if (g_RoCtrl && g_RoCtrl->magic == SHM_MAGIC)
        return __atomic_load_n(&g_RoCtrl->menuOpen, __ATOMIC_RELAXED) != 0;
    return false;
}

static std::atomic<uint64_t> g_LastConsumerActiveMs{ 0 };

bool Capture_IsConsumerActive()
{
    if (Capture_IsDaemonActive()) return true;
    uint64_t last = g_LastConsumerActiveMs.load();
    if (last == 0) return false;
    return (Platform_GetTickMs() - last) < 1500;
}

bool Capture_HasActiveWork()
{
    if (g_SnapshotPending.load(std::memory_order_relaxed)) return true;
    if (g_DumpQuota.load(std::memory_order_relaxed) > 0) return true;
    if (DUMP_FRAMES > 0 && g_DumpCount.load(std::memory_order_relaxed) < DUMP_FRAMES) return true;
    if (g_pView)
    {
        const ShmControlHeader* ctrl = static_cast<const ShmControlHeader*>(g_pView);
        if (__atomic_load_n(&ctrl->recordRequested, __ATOMIC_RELAXED) == 1)
            return true;
    }
    if (Recorder_IsRecording()) return true;
    return false;
}

bool Capture_WantsFrame()
{
    static int s_wfLog = 0;
    if (g_SnapshotPending.load()) { if (s_wfLog < 5) { ++s_wfLog; Log("[con] WantsFrame -> snap"); } return true; }
    if (g_DumpQuota.load() > 0)   { if (s_wfLog < 5) { ++s_wfLog; Log("[con] WantsFrame -> quota"); } return true; }
    if (!Capture_IsDaemonActive() && Recorder_WantsFrame()) { if (s_wfLog < 5) { ++s_wfLog; Log("[con] WantsFrame -> inproc"); } return true; }
    if (DUMP_FRAMES > 0 && g_DumpCount.load() < DUMP_FRAMES) { if (s_wfLog < 5) { ++s_wfLog; Log("[con] WantsFrame -> dump"); } return true; }

    if (g_pView && Capture_IsDaemonActive())
    {
        const ShmControlHeader* ctrl = static_cast<const ShmControlHeader*>(g_pView);
        if (__atomic_load_n(&ctrl->recordRequested, __ATOMIC_ACQUIRE) != 1)
            return false;
        uint32_t w = __atomic_load_n(&ctrl->writeIdx, __ATOMIC_RELAXED);
        uint32_t r = __atomic_load_n(&ctrl->readIdx, __ATOMIC_ACQUIRE);
        int pending = g_PendingArms.load(std::memory_order_relaxed);
        if (static_cast<uint32_t>(w - r) + static_cast<uint32_t>(pending) < SHM_SLOTS)
        {
            g_PendingArms.fetch_add(1);
            g_LastConsumerActiveMs.store(Platform_GetTickMs());
            return true;
        }
    }
    return false;
}

void Capture_CancelPendingFrame()
{
    int prev = g_PendingArms.load();
    while (prev > 0 && !g_PendingArms.compare_exchange_weak(prev, prev - 1)) {}
}

void Capture_FrameReady(const FrameData& f)
{
    static bool logged = false;
    if (!logged) { Log("[con] Capture_FrameReady called (first time)"); logged = true; }

    if (!Capture_IsDaemonActive())
        Recorder_OnFrameReady(f.pixels, f.width, f.height, f.stride, f.format, f.frameIdx);

    if (Capture_TakeSnapshotPending())
    {
        char snapDir[MAX_PATH];
        std::snprintf(snapDir, sizeof(snapDir), "%s/screenshots", Platform_GetBaseDir());
        Platform_EnsureDir(snapDir);

        struct timespec ts = {};
        clock_gettime(CLOCK_REALTIME, &ts);
        struct tm tmLocal = {};
        localtime_r(&ts.tv_sec, &tmLocal);
        int ms = static_cast<int>(ts.tv_nsec / 1000000L);

        char snapPath[MAX_PATH];
        std::snprintf(snapPath, sizeof(snapPath),
                      "%s/shot_%04d%02d%02d_%02d%02d%02d_%03d.bmp",
                      snapDir,
                      tmLocal.tm_year + 1900, tmLocal.tm_mon + 1, tmLocal.tm_mday,
                      tmLocal.tm_hour, tmLocal.tm_min, tmLocal.tm_sec, ms);

        if (WriteBmp(snapPath, f))
        {
            Log("[con] Screenshot saved to %s", snapPath);
            Capture_SetLastScreenshotPath(snapPath);
        }
    }

    int quota = g_DumpQuota.load();
    if (quota > 0)
    {
        g_DumpQuota.fetch_sub(1);
        char dumpSubDir[MAX_PATH];
        std::snprintf(dumpSubDir, sizeof(dumpSubDir), "%s/dumps", Platform_GetBaseDir());
        Platform_EnsureDir(dumpSubDir);

        char dumpPath[MAX_PATH];
        std::snprintf(dumpPath, sizeof(dumpPath), "%s/dump_%06llu.bmp",
                      dumpSubDir, static_cast<unsigned long long>(f.frameIdx));
        WriteBmp(dumpPath, f);
    }

    if (g_pView && Capture_IsDaemonActive())
    {
        int prev = g_PendingArms.load();
        while (prev > 0 && !g_PendingArms.compare_exchange_weak(prev, prev - 1)) {}
        ShmControlHeader* ctrl = static_cast<ShmControlHeader*>(g_pView);
        if (__atomic_load_n(&ctrl->recordRequested, __ATOMIC_ACQUIRE) == 1)
        {
            uint32_t w = __atomic_load_n(&ctrl->writeIdx, __ATOMIC_RELAXED);
            uint32_t r = __atomic_load_n(&ctrl->readIdx, __ATOMIC_ACQUIRE);
            if (static_cast<uint32_t>(w - r) < SHM_SLOTS)
            {
                g_LastConsumerActiveMs.store(Platform_GetTickMs());
                uint32_t slotIdx = w % SHM_SLOTS;

                uint8_t* slotBase = static_cast<uint8_t*>(g_pView) + 4096ULL + static_cast<size_t>(slotIdx) * SHM_SLOT_BYTES;
                ShmSlotHeader* sh = reinterpret_cast<ShmSlotHeader*>(slotBase);
                sh->width       = f.width;
                sh->height      = f.height;
                sh->stride      = f.width * 4;
                sh->format      = f.format;
                sh->frameIdx    = f.frameIdx;
                sh->timestampNs = f.timestampNs ? f.timestampNs : Platform_GetMonotonicNs();
                sh->dataOffset  = sizeof(ShmSlotHeader);
                sh->sessionId   = ctrl->recordSessionId;

                uint8_t* dst = slotBase + sizeof(ShmSlotHeader);
                const uint8_t* src = static_cast<const uint8_t*>(f.pixels);
                uint32_t rowBytes = f.width * 4;
                if (static_cast<size_t>(rowBytes) * f.height + sizeof(ShmSlotHeader) <= SHM_SLOT_BYTES)
                {
                    if (f.stride == rowBytes)
                    {
                        std::memcpy(dst, src, static_cast<size_t>(rowBytes) * f.height);
                    }
                    else
                    {
                        for (uint32_t y = 0; y < f.height; ++y, src += f.stride, dst += rowBytes)
                            std::memcpy(dst, src, rowBytes);
                    }
                }

                __atomic_store_n(&ctrl->writeIdx, w + 1, __ATOMIC_RELEASE);
            }
        }
    }

    if (DUMP_FRAMES > 0)
    {
        int n = g_DumpCount.fetch_add(1);
        if (n < DUMP_FRAMES)
        {
            char path[MAX_PATH];
            std::snprintf(path, sizeof(path), "%s/frame_%05llu.bmp",
                          Platform_GetBaseDir(), static_cast<unsigned long long>(f.frameIdx));
            WriteBmp(path, f);
        }
    }
}
