/**
 * audio.cpp  —  Low-latency in-process game audio capture (Linux port)
 *
 * Supports:
 *  1. Direct Hook (via LD_PRELOAD symbol interposition):
 *     - PulseAudio / PipeWire-pulse: pa_stream_write, pa_simple_write
 *     - ALSA: snd_pcm_writei, snd_pcm_hw_params_set_*
 *  2. System Monitor Loopback Fallback (via libpulse-simple @DEFAULT_MONITOR@):
 *     - Automatically engages in AudioCaptureMode_Auto if no direct hook
 *       activity is observed for 1.5s, and yields immediately when direct hooks
 *       become active.
 *  3. ITU-R BS.775 downmix, 2nd-order 30 Hz high-pass level metering,
 *     software gain (0–300%), and 440 Hz test tone generator.
 */

#ifndef VKCAPTURE_NO_LOCAL_AUDIO
#include <pulse/pulseaudio.h>
#include <pulse/simple.h>
#include <alsa/asoundlib.h>
#endif
#include <dlfcn.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <atomic>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <unordered_map>
#include <vector>

#include "audio.h"
#include "capture.h"
#include "platform.h"

// ── Ring Buffer ──────────────────────────────────────────────────────────────
static constexpr size_t RING_SLOTS = 256;

#pragma pack(push, 1)
struct AudioRingSlot
{
    uint64_t timestampNs;
    uint32_t numFrames;
    uint32_t sampleRate;
    int16_t  data[AUDIO_MAX_CHUNK_FRAMES * 2];
};
#pragma pack(pop)

struct StreamAudioFormat
{
    uint32_t sampleRate    = 48000;
    uint32_t channels      = 2;
    uint32_t bitsPerSample = 16;
    bool     isFloat       = false;
    float    periodMs      = 10.0f;
};

// ── 2nd-order 30 Hz high-pass LevelFilter ────────────────────────────────────
static constexpr uint32_t LEVEL_FILTER_CHANNELS  = 8;
static constexpr float    LEVEL_FILTER_CUTOFF_HZ = 30.0f;

struct LevelFilter
{
    float x1[2][LEVEL_FILTER_CHANNELS] = {};
    float y1[2][LEVEL_FILTER_CHANNELS] = {};
};

static inline float LevelFilterStep(LevelFilter* f, uint32_t ch, float x, float r)
{
    if (!f || ch >= LEVEL_FILTER_CHANNELS) return x;
    float y = x - f->x1[0][ch] + r * f->y1[0][ch];
    f->x1[0][ch] = x;
    f->y1[0][ch] = y;
    float y2 = y - f->x1[1][ch] + r * f->y1[1][ch];
    f->x1[1][ch] = y;
    f->y1[1][ch] = y2;
    if (std::fabs(f->y1[0][ch]) < 1e-20f) f->y1[0][ch] = 0.0f;
    if (std::fabs(f->y1[1][ch]) < 1e-20f) f->y1[1][ch] = 0.0f;
    return y2;
}

static inline float LevelFilterCoefficient(uint32_t sampleRate)
{
    const float fs = sampleRate ? static_cast<float>(sampleRate) : 48000.0f;
    return 1.0f - 2.0f * 3.14159265f * LEVEL_FILTER_CUTOFF_HZ / fs;
}

// ── Global State ─────────────────────────────────────────────────────────────
static std::atomic<bool>     s_HooksInstalled{ false };
static std::atomic<bool>     g_AudioEnabled{ true };
static std::atomic<bool>     g_AudioWanted{ false };
static std::atomic<int>      s_CaptureMode{ AudioCaptureMode_Auto };
static std::atomic<int>      s_Source{ AudioSource_None };

static std::atomic<uint32_t> s_SampleRate{ 48000 };
static std::atomic<uint32_t> s_Channels{ 2 };
static std::atomic<uint32_t> s_BitsPerSample{ 16 };
static std::atomic<bool>     s_IsFloat{ false };
static std::atomic<float>    s_DevicePeriodMs{ 10.0f };
static std::atomic<uint32_t> s_ActiveStreams{ 0 };

static std::atomic<uint64_t> s_CapturedFrames{ 0 };
static std::atomic<uint64_t> s_DroppedFrames{ 0 };
static std::atomic<uint64_t> s_Overruns{ 0 };
static std::atomic<float>    s_PeakDb{ -96.0f };
static std::atomic<float>    s_SoftwareGain{ 1.0f };

static std::atomic<uint64_t> s_LastHookWriteMs{ 0 };
static std::atomic<uint64_t> s_LastAudibleHookMs{ 0 };

static std::mutex            s_ErrMtx;
static char                  s_LastError[128] = "";

static AudioRingSlot*        s_RingSlots = nullptr;
static std::mutex            s_RingMtx;
static std::atomic<uint32_t> s_RingHead{ 0 };
static std::atomic<uint32_t> s_RingTail{ 0 };
static uint32_t              s_CurrentSlotOffset = 0;

static std::thread             s_LoopbackThread;
static std::atomic<bool>       s_LoopbackRunning{ false };
static std::mutex              s_LoopbackMtx;
static std::condition_variable s_LoopbackCv;

static std::mutex              s_StreamMtx;
static std::unordered_map<const void*, StreamAudioFormat> s_StreamFormats;
static LevelFilter             s_HookFilter;
static LevelFilter             s_LoopbackFilter;

static void SetAudioError(const char* fmt, ...)
{
    char msg[sizeof(s_LastError)] = "";
    if (fmt && *fmt)
    {
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(msg, sizeof(msg), fmt, args);
        va_end(args);
        Log("[audio] %s", msg);
    }
    std::lock_guard<std::mutex> lock(s_ErrMtx);
    Platform_SafeStrCpy(s_LastError, sizeof(s_LastError), msg);
}

// ── ITU-R BS.775 Downmix & Ring Push ─────────────────────────────────────────
static inline int16_t FloatToInt16(float v)
{
    if (v >= 1.0f)  return 32767;
    if (v <= -1.0f) return -32768;
    return static_cast<int16_t>(v * 32767.0f);
}


static void PushConvertedFrames(const void* data, uint32_t numFrames,
                                const StreamAudioFormat& fmt,
                                LevelFilter* filter,
                                int sourceKind)
{
    if (!data || numFrames == 0 || !s_RingSlots) return;
    if (!g_AudioEnabled.load() || !g_AudioWanted.load()) return;

    const float r = LevelFilterCoefficient(fmt.sampleRate);
    float maxFiltered = 0.0f;
    const uint32_t ch = fmt.channels ? fmt.channels : 2;
    const uint64_t nowNs = Platform_GetMonotonicNs();

    uint32_t offset = 0;
    while (offset < numFrames)
    {
        uint32_t chunk = numFrames - offset;
        if (chunk > AUDIO_MAX_CHUNK_FRAMES) chunk = AUDIO_MAX_CHUNK_FRAMES;

        int16_t stereoBuf[AUDIO_MAX_CHUNK_FRAMES * 2];

        for (uint32_t i = 0; i < chunk; ++i)
        {
            uint32_t frameIdx = offset + i;
            float samples[8] = {};
            uint32_t readCh = (ch > 8) ? 8 : ch;

            if (fmt.isFloat && fmt.bitsPerSample == 32)
            {
                const float* fptr = static_cast<const float*>(data) + static_cast<size_t>(frameIdx) * ch;
                for (uint32_t c = 0; c < readCh; ++c) samples[c] = fptr[c];
            }
            else if (!fmt.isFloat && fmt.bitsPerSample == 32)
            {
                const int32_t* iptr = static_cast<const int32_t*>(data) + static_cast<size_t>(frameIdx) * ch;
                for (uint32_t c = 0; c < readCh; ++c) samples[c] = static_cast<float>(iptr[c]) / 2147483648.0f;
            }
            else
            {
                const int16_t* sptr = static_cast<const int16_t*>(data) + static_cast<size_t>(frameIdx) * ch;
                for (uint32_t c = 0; c < readCh; ++c) samples[c] = static_cast<float>(sptr[c]) / 32768.0f;
            }

            for (uint32_t c = 0; c < readCh; ++c)
            {
                float flt = std::fabs(LevelFilterStep(filter, c, samples[c], r));
                if (flt > maxFiltered) maxFiltered = flt;
            }

            float L = 0.0f, R = 0.0f;
            if (ch == 1)
            {
                L = R = samples[0];
            }
            else if (ch == 2)
            {
                L = samples[0];
                R = samples[1];
            }
            else if (ch == 6) // 5.1: FL, FR, FC, LFE, RL, RR
            {
                L = samples[0] + 0.707f * samples[2] + 0.707f * samples[4];
                R = samples[1] + 0.707f * samples[2] + 0.707f * samples[5];
            }
            else if (ch >= 8) // 7.1: FL, FR, FC, LFE, RL, RR, SL, SR
            {
                L = samples[0] + 0.707f * samples[2] + 0.707f * samples[4] + 0.707f * samples[6];
                R = samples[1] + 0.707f * samples[2] + 0.707f * samples[5] + 0.707f * samples[7];
            }
            else
            {
                L = samples[0];
                R = samples[1];
            }

            stereoBuf[i * 2 + 0] = FloatToInt16(L);
            stereoBuf[i * 2 + 1] = FloatToInt16(R);
        }

        {
            std::lock_guard<std::mutex> lock(s_RingMtx);
            uint32_t head = s_RingHead.load(std::memory_order_relaxed);
            uint32_t tail = s_RingTail.load(std::memory_order_relaxed);
            uint32_t nextHead = (head + 1) % RING_SLOTS;

            if (nextHead == tail)
            {
                s_RingTail.store((tail + 1) % RING_SLOTS, std::memory_order_relaxed);
                s_CurrentSlotOffset = 0;
                s_DroppedFrames.fetch_add(chunk, std::memory_order_relaxed);
                s_Overruns.fetch_add(1, std::memory_order_relaxed);
            }

            AudioRingSlot& slot = s_RingSlots[head];
            slot.timestampNs = nowNs;
            slot.numFrames   = chunk;
            slot.sampleRate  = fmt.sampleRate;
            std::memcpy(slot.data, stereoBuf, static_cast<size_t>(chunk) * 2 * sizeof(int16_t));

            s_RingHead.store(nextHead, std::memory_order_release);
        }

        s_CapturedFrames.fetch_add(chunk, std::memory_order_relaxed);
        offset += chunk;
    }

    float db = (maxFiltered > 1e-5f) ? (20.0f * std::log10(maxFiltered)) : -96.0f;
    s_PeakDb.store(db, std::memory_order_relaxed);
    s_Source.store(sourceKind, std::memory_order_relaxed);
    s_SampleRate.store(fmt.sampleRate, std::memory_order_relaxed);
    s_Channels.store(fmt.channels, std::memory_order_relaxed);
    s_BitsPerSample.store(fmt.bitsPerSample, std::memory_order_relaxed);
    s_IsFloat.store(fmt.isFloat, std::memory_order_relaxed);
    s_DevicePeriodMs.store(fmt.periodMs, std::memory_order_relaxed);

    if (sourceKind == AudioSource_Hook && db > -70.0f)
        s_LastAudibleHookMs.store(Platform_GetTickMs(), std::memory_order_relaxed);
}

// ── LD_PRELOAD Symbol Interposition (PulseAudio + ALSA) ──────────────────────
#ifndef VKCAPTURE_NO_LOCAL_AUDIO
static bool IsAudioCaptureDisabledByEnv()
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

extern "C" {

typedef int (*PFN_pa_stream_write)(pa_stream*, const void*, size_t, pa_free_cb_t, int64_t, pa_seek_mode_t);
typedef int (*PFN_pa_simple_write)(pa_simple*, const void*, size_t, int*);
typedef pa_stream* (*PFN_pa_stream_new)(pa_context*, const char*, const pa_sample_spec*, const pa_channel_map*);
typedef pa_simple* (*PFN_pa_simple_new)(const char*, const char*, pa_stream_direction_t, const char*,
                                        const char*, const pa_sample_spec*, const pa_channel_map*,
                                        const pa_buffer_attr*, int*);
typedef snd_pcm_sframes_t (*PFN_snd_pcm_writei)(snd_pcm_t*, const void*, snd_pcm_uframes_t);

int pa_stream_write(pa_stream* p, const void* data, size_t nbytes,
                    pa_free_cb_t free_cb, int64_t offset, pa_seek_mode_t seek)
{
    static auto real_fn = reinterpret_cast<PFN_pa_stream_write>(dlsym(RTLD_NEXT, "pa_stream_write"));
    if (IsAudioCaptureDisabledByEnv()) return real_fn ? real_fn(p, data, nbytes, free_cb, offset, seek) : -1;
    s_HooksInstalled.store(true, std::memory_order_relaxed);
    s_LastHookWriteMs.store(Platform_GetTickMs(), std::memory_order_relaxed);

    if (data && nbytes > 0 && g_AudioWanted.load() && s_CaptureMode.load() != AudioCaptureMode_Loopback)
    {
        StreamAudioFormat fmt;
        {
            std::lock_guard<std::mutex> lk(s_StreamMtx);
            auto it = s_StreamFormats.find(p);
            if (it != s_StreamFormats.end()) fmt = it->second;
            else
            {
                const pa_sample_spec* ss = pa_stream_get_sample_spec(p);
                if (ss)
                {
                    fmt.sampleRate = ss->rate;
                    fmt.channels   = ss->channels;
                    fmt.isFloat    = (ss->format == PA_SAMPLE_FLOAT32LE || ss->format == PA_SAMPLE_FLOAT32BE);
                    fmt.bitsPerSample = (fmt.isFloat || ss->format == PA_SAMPLE_S32LE) ? 32 : 16;
                    s_StreamFormats[p] = fmt;
                    s_ActiveStreams.store(static_cast<uint32_t>(s_StreamFormats.size()));
                }
            }
        }
        uint32_t bytesPerFrame = fmt.channels * (fmt.bitsPerSample / 8);
        if (bytesPerFrame > 0)
        {
            uint32_t frames = static_cast<uint32_t>(nbytes / bytesPerFrame);
            PushConvertedFrames(data, frames, fmt, &s_HookFilter, AudioSource_Hook);
        }
    }

    return real_fn ? real_fn(p, data, nbytes, free_cb, offset, seek) : -1;
}

pa_stream* pa_stream_new(pa_context* c, const char* name, const pa_sample_spec* ss, const pa_channel_map* map)
{
    static auto real_fn = reinterpret_cast<PFN_pa_stream_new>(dlsym(RTLD_NEXT, "pa_stream_new"));
    if (IsAudioCaptureDisabledByEnv()) return real_fn ? real_fn(c, name, ss, map) : nullptr;
    s_HooksInstalled.store(true, std::memory_order_relaxed);
    pa_stream* s = real_fn ? real_fn(c, name, ss, map) : nullptr;
    if (s && ss)
    {
        StreamAudioFormat fmt;
        fmt.sampleRate    = ss->rate;
        fmt.channels      = ss->channels;
        fmt.isFloat       = (ss->format == PA_SAMPLE_FLOAT32LE || ss->format == PA_SAMPLE_FLOAT32BE);
        fmt.bitsPerSample = (fmt.isFloat || ss->format == PA_SAMPLE_S32LE) ? 32 : 16;
        std::lock_guard<std::mutex> lk(s_StreamMtx);
        s_StreamFormats[s] = fmt;
        s_ActiveStreams.store(static_cast<uint32_t>(s_StreamFormats.size()));
    }
    return s;
}

int pa_simple_write(pa_simple* s, const void* data, size_t bytes, int* error)
{
    static auto real_fn = reinterpret_cast<PFN_pa_simple_write>(dlsym(RTLD_NEXT, "pa_simple_write"));
    if (IsAudioCaptureDisabledByEnv()) return real_fn ? real_fn(s, data, bytes, error) : -1;
    s_HooksInstalled.store(true, std::memory_order_relaxed);
    s_LastHookWriteMs.store(Platform_GetTickMs(), std::memory_order_relaxed);

    if (data && bytes > 0 && g_AudioWanted.load() && s_CaptureMode.load() != AudioCaptureMode_Loopback)
    {
        StreamAudioFormat fmt;
        {
            std::lock_guard<std::mutex> lk(s_StreamMtx);
            auto it = s_StreamFormats.find(s);
            if (it != s_StreamFormats.end()) fmt = it->second;
        }
        uint32_t bytesPerFrame = fmt.channels * (fmt.bitsPerSample / 8);
        if (bytesPerFrame > 0)
            PushConvertedFrames(data, static_cast<uint32_t>(bytes / bytesPerFrame), fmt, &s_HookFilter, AudioSource_Hook);
    }

    return real_fn ? real_fn(s, data, bytes, error) : -1;
}

pa_simple* pa_simple_new(const char* server, const char* name, pa_stream_direction_t dir,
                         const char* dev, const char* stream_name, const pa_sample_spec* ss,
                         const pa_channel_map* map, const pa_buffer_attr* attr, int* error)
{
    static auto real_fn = reinterpret_cast<PFN_pa_simple_new>(dlsym(RTLD_NEXT, "pa_simple_new"));
    if (IsAudioCaptureDisabledByEnv()) return real_fn ? real_fn(server, name, dir, dev, stream_name, ss, map, attr, error) : nullptr;
    s_HooksInstalled.store(true, std::memory_order_relaxed);
    pa_simple* s = real_fn ? real_fn(server, name, dir, dev, stream_name, ss, map, attr, error) : nullptr;
    if (s && ss && dir == PA_STREAM_PLAYBACK)
    {
        StreamAudioFormat fmt;
        fmt.sampleRate    = ss->rate;
        fmt.channels      = ss->channels;
        fmt.isFloat       = (ss->format == PA_SAMPLE_FLOAT32LE || ss->format == PA_SAMPLE_FLOAT32BE);
        fmt.bitsPerSample = (fmt.isFloat || ss->format == PA_SAMPLE_S32LE) ? 32 : 16;
        std::lock_guard<std::mutex> lk(s_StreamMtx);
        s_StreamFormats[s] = fmt;
        s_ActiveStreams.store(static_cast<uint32_t>(s_StreamFormats.size()));
    }
    return s;
}

snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t* pcm, const void* buffer, snd_pcm_uframes_t size)
{
    static auto real_fn = reinterpret_cast<PFN_snd_pcm_writei>(dlsym(RTLD_NEXT, "snd_pcm_writei"));
    if (IsAudioCaptureDisabledByEnv()) return real_fn ? real_fn(pcm, buffer, size) : -1;
    s_HooksInstalled.store(true, std::memory_order_relaxed);
    s_LastHookWriteMs.store(Platform_GetTickMs(), std::memory_order_relaxed);

    if (buffer && size > 0 && g_AudioWanted.load() && s_CaptureMode.load() != AudioCaptureMode_Loopback)
    {
        StreamAudioFormat fmt;
        fmt.sampleRate = 48000;
        fmt.channels   = 2;
        fmt.bitsPerSample = 16;
        fmt.isFloat    = false;
        PushConvertedFrames(buffer, static_cast<uint32_t>(size), fmt, &s_HookFilter, AudioSource_Hook);
    }

    return real_fn ? real_fn(pcm, buffer, size) : -1;
}

} // extern "C"

// ── PulseAudio / PipeWire Monitor Loopback Thread ────────────────────────────
static void LoopbackWorkerLoop()
{
    Log("[audio] Loopback worker thread started");
    pa_simple* pa = nullptr;

    while (s_LoopbackRunning.load())
    {
        bool wanted = g_AudioWanted.load() && g_AudioEnabled.load() && !Capture_IsDaemonActive();
        int  mode   = s_CaptureMode.load();
        uint64_t nowMs = Platform_GetTickMs();
        uint64_t lastHook = s_LastHookWriteMs.load();
        bool hookActive = (lastHook > 0) && ((nowMs - lastHook) < 1500);

        bool needLoopback = wanted && ((mode == AudioCaptureMode_Loopback) ||
                                       (mode == AudioCaptureMode_Auto && !hookActive));

        if (!needLoopback)
        {
            if (pa) { pa_simple_free(pa); pa = nullptr; }
            std::unique_lock<std::mutex> lk(s_LoopbackMtx);
            s_LoopbackCv.wait_for(lk, std::chrono::milliseconds(100), [] {
                return !s_LoopbackRunning.load();
            });
            continue;
        }

        if (!pa)
        {
            static auto real_pa_simple_new = reinterpret_cast<PFN_pa_simple_new>(dlsym(RTLD_DEFAULT, "pa_simple_new"));
            pa_sample_spec ss = {};
            ss.format   = PA_SAMPLE_FLOAT32LE;
            ss.rate     = 48000;
            ss.channels = 2;

            int err = 0;
            if (real_pa_simple_new)
            {
                pa = real_pa_simple_new(nullptr, "vkcapture-loopback", PA_STREAM_RECORD,
                                        "@DEFAULT_MONITOR@", "Game Audio Loopback",
                                        &ss, nullptr, nullptr, &err);
            }
            if (!pa)
            {
                SetAudioError("pa_simple_new(@DEFAULT_MONITOR@) failed: %s", pa_strerror(err));
                usleep(500000);
                continue;
            }
            Log("[audio] Opened PulseAudio/PipeWire monitor loopback");
        }

        static constexpr uint32_t CHUNK_FRAMES = 480; // 10ms @ 48kHz
        float buf[CHUNK_FRAMES * 2];
        int err = 0;
        if (pa_simple_read(pa, buf, sizeof(buf), &err) < 0)
        {
            SetAudioError("pa_simple_read failed: %s", pa_strerror(err));
            pa_simple_free(pa);
            pa = nullptr;
            usleep(100000);
            continue;
        }

        StreamAudioFormat fmt;
        fmt.sampleRate    = 48000;
        fmt.channels      = 2;
        fmt.bitsPerSample = 32;
        fmt.isFloat       = true;
        fmt.periodMs      = 10.0f;
        PushConvertedFrames(buf, CHUNK_FRAMES, fmt, &s_LoopbackFilter, AudioSource_Loopback);
    }

    if (pa) pa_simple_free(pa);
    Log("[audio] Loopback worker thread finished");
}
#endif // !VKCAPTURE_NO_LOCAL_AUDIO

// ── Public API ───────────────────────────────────────────────────────────────
void Audio_Init()
{
    if (!s_RingSlots)
    {
        s_RingSlots = new AudioRingSlot[RING_SLOTS]();
    }
    s_RingHead.store(0);
    s_RingTail.store(0);
    s_CurrentSlotOffset = 0;

#ifndef VKCAPTURE_NO_LOCAL_AUDIO
    if (!s_LoopbackRunning.exchange(true))
    {
        s_LoopbackThread = std::thread(LoopbackWorkerLoop);
    }
#endif
    Log("[audio] Audio_Init complete (LD_PRELOAD hooks + monitor loopback ready)");
}

void Audio_Shutdown()
{
    if (s_LoopbackRunning.exchange(false))
    {
        s_LoopbackCv.notify_all();
        if (s_LoopbackThread.joinable())
            s_LoopbackThread.join();
    }

    if (s_RingSlots)
    {
        delete[] s_RingSlots;
        s_RingSlots = nullptr;
    }
    Log("[audio] Audio_Shutdown complete");
}

void Audio_SetCaptureMode(int mode) { s_CaptureMode.store(mode); }
int  Audio_GetCaptureMode()         { return s_CaptureMode.load(); }
void Audio_SetEnabled(bool enabled) { g_AudioEnabled.store(enabled); }
bool Audio_IsEnabled()              { return g_AudioEnabled.load(); }
void Audio_SetWanted(bool wanted)   { g_AudioWanted.store(wanted); s_LoopbackCv.notify_all(); }
bool Audio_IsWanted()               { return g_AudioWanted.load(); }

void Audio_Flush()
{
    std::lock_guard<std::mutex> lock(s_RingMtx);
    s_RingHead.store(0);
    s_RingTail.store(0);
    s_CurrentSlotOffset = 0;
}

uint32_t Audio_GetSampleRate()
{
    uint32_t sr = s_SampleRate.load();
    return sr ? sr : 48000;
}

uint32_t Audio_ReadStereo16(int16_t* dst, uint32_t maxFrames, uint64_t* outTimestampNs)
{
    if (!dst || maxFrames == 0 || !s_RingSlots) return 0;

    std::lock_guard<std::mutex> lock(s_RingMtx);
    uint32_t tail = s_RingTail.load(std::memory_order_relaxed);
    uint32_t head = s_RingHead.load(std::memory_order_acquire);
    if (tail == head) return 0;

    AudioRingSlot& slot = s_RingSlots[tail];
    uint32_t avail = (slot.numFrames > s_CurrentSlotOffset) ? (slot.numFrames - s_CurrentSlotOffset) : 0;
    uint32_t toRead = (avail > maxFrames) ? maxFrames : avail;

    if (outTimestampNs)
    {
        uint64_t offsetNs = (static_cast<uint64_t>(s_CurrentSlotOffset) * 1000000000ULL) /
                            (slot.sampleRate ? slot.sampleRate : 48000);
        *outTimestampNs = slot.timestampNs + offsetNs;
    }

    const int16_t* src = slot.data + static_cast<size_t>(s_CurrentSlotOffset) * 2;
    const float gain = s_SoftwareGain.load(std::memory_order_relaxed);

    if (std::fabs(gain - 1.0f) < 0.001f)
    {
        std::memcpy(dst, src, static_cast<size_t>(toRead) * 2 * sizeof(int16_t));
    }
    else
    {
        for (uint32_t i = 0; i < toRead * 2; ++i)
        {
            float v = static_cast<float>(src[i]) * gain;
            if (v > 32767.0f) v = 32767.0f;
            if (v < -32768.0f) v = -32768.0f;
            dst[i] = static_cast<int16_t>(v);
        }
    }

    s_CurrentSlotOffset += toRead;
    if (s_CurrentSlotOffset >= slot.numFrames)
    {
        s_RingTail.store((tail + 1) % RING_SLOTS, std::memory_order_release);
        s_CurrentSlotOffset = 0;
    }
    return toRead;
}

void Audio_GetStats(AudioStats* out)
{
    if (!out) return;
    out->hooksInstalled = s_HooksInstalled.load();
    out->enabled        = g_AudioEnabled.load();
    out->wanted         = g_AudioWanted.load();
    out->source         = s_Source.load();
    out->captureMode    = s_CaptureMode.load();
    out->sampleRate     = s_SampleRate.load();
    out->channels       = s_Channels.load();
    out->bitsPerSample  = s_BitsPerSample.load();
    out->isFloat        = s_IsFloat.load();
    out->devicePeriodMs = s_DevicePeriodMs.load();
    out->capturedFrames = s_CapturedFrames.load();
    out->droppedFrames  = s_DroppedFrames.load();
    out->overruns       = s_Overruns.load();

    uint32_t h = s_RingHead.load();
    uint32_t t = s_RingTail.load();
    uint32_t used = (h >= t) ? (h - t) : (RING_SLOTS - t + h);
    out->ringFillPercent = (used * 100U) / static_cast<uint32_t>(RING_SLOTS);
    out->peakDb          = s_PeakDb.load();
    out->activeStreams   = s_ActiveStreams.load();

    std::lock_guard<std::mutex> lk(s_ErrMtx);
    Platform_SafeStrCpy(out->lastError, sizeof(out->lastError), s_LastError);
    if (Capture_IsDaemonActive())
    {
        float dDb = Capture_GetDaemonAudioPeakDb();
        out->peakDb        = dDb;
        out->sampleRate    = 48000;
        out->channels      = 2;
        out->bitsPerSample = 16;
        out->source        = AudioSource_Loopback;
    }
}

void  Audio_SetVolume(float volume) { s_SoftwareGain.store(volume < 0.0f ? 0.0f : (volume > 3.0f ? 3.0f : volume)); }
float Audio_GetVolume()             { return s_SoftwareGain.load(); }

void Audio_InjectTestTone(uint32_t freqHz, uint32_t durationMs)
{
    uint32_t sr = Audio_GetSampleRate();
    uint32_t totalFrames = (sr * durationMs) / 1000;
    std::vector<float> buf(totalFrames * 2);
    for (uint32_t i = 0; i < totalFrames; ++i)
    {
        float s = 0.25f * std::sin(2.0f * 3.14159265f * static_cast<float>(freqHz) * static_cast<float>(i) / static_cast<float>(sr));
        buf[i * 2 + 0] = s;
        buf[i * 2 + 1] = s;
    }
    StreamAudioFormat fmt;
    fmt.sampleRate = sr;
    fmt.channels = 2;
    fmt.bitsPerSample = 32;
    fmt.isFloat = true;
    PushConvertedFrames(buf.data(), totalFrames, fmt, &s_HookFilter, AudioSource_Hook);
}
