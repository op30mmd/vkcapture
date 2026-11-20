/**
 * shm_reader.cpp  —  Out-of-process 4-slot SHM consumer & host A/V recorder daemon
 */

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <csignal>
#include <ctime>
#include <cmath>
#include <algorithm>
#include <vector>
#include <string>
#include <cctype>
#include <deque>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>

#include <pulse/simple.h>
#include <pulse/pulseaudio.h>
#include <pulse/error.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

#ifndef MAX_PATH
#define MAX_PATH PATH_MAX
#endif

static constexpr int    SHM_SLOTS         = 4;
static constexpr size_t SHM_SLOT_BYTES    = 3840ULL * 2160ULL * 4ULL + 128ULL;
static constexpr size_t SHM_TOTAL_BYTES   = 4096ULL + SHM_SLOTS * SHM_SLOT_BYTES;
static constexpr char   SHM_NAME[]        = "/vkcapture_shm_v3";
static constexpr uint32_t SHM_MAGIC       = 0x564B4332u;

#pragma pack(push, 1)

static constexpr uint32_t AUDIO_SHM_MAGIC  = 0x564B4131u;
static constexpr uint32_t AUDIO_SHM_FRAMES = 96000;
static constexpr char     AUDIO_SHM_NAME[] = "/vkcapture_audio_v1";

struct AudioShmRing
{
    uint32_t magic;
    uint32_t sampleRate;
    uint32_t channels;
    uint32_t reserved;
    uint64_t writeFrames;
    uint64_t lastPushNs;
    int16_t  pcm[AUDIO_SHM_FRAMES * 2];
};

struct ShmSlotHeader
{
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    uint64_t frameIdx;
    uint64_t timestampNs;
    uint32_t dataOffset;
    uint32_t sessionId;
};

struct ShmControlHeader
{
    uint32_t magic;
    uint32_t numSlots;
    uint64_t slotBytes;
    uint32_t writeIdx;
    uint32_t readIdx;
    uint32_t recordRequested;
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

static uint64_t GetMonotonicNs()
{
    struct timespec ts = {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

static bool IsRgbaFormat(uint32_t vkFormat)
{
    return (vkFormat >= 37 && vkFormat <= 43) || (vkFormat >= 51 && vkFormat <= 57);
}

static bool SaveBmp(const char* path, const uint8_t* pixels,
                    uint32_t width, uint32_t height, uint32_t stride, uint32_t vkFormat)
{
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;

    const uint32_t rowBytes = width * 4;
    const uint32_t dataSize = rowBytes * height;

    BmpFileHeader fh = {};
    fh.bfType    = 0x4D42;
    fh.bfOffBits = sizeof(BmpFileHeader) + sizeof(BmpInfoHeader);
    fh.bfSize    = fh.bfOffBits + dataSize;

    BmpInfoHeader ih = {};
    ih.biSize        = sizeof(ih);
    ih.biWidth       = static_cast<int32_t>(width);
    ih.biHeight      = static_cast<int32_t>(height);
    ih.biPlanes      = 1;
    ih.biBitCount    = 32;
    ih.biCompression = 0;
    ih.biSizeImage   = dataSize;

    std::fwrite(&fh, sizeof(fh), 1, f);
    std::fwrite(&ih, sizeof(ih), 1, f);

    bool swapRB = IsRgbaFormat(vkFormat);
    std::vector<uint8_t> tmp;
    if (swapRB) tmp.resize(rowBytes);

    for (int y = static_cast<int>(height) - 1; y >= 0; --y)
    {
        const uint8_t* row = pixels + static_cast<size_t>(y) * stride;
        if (!swapRB)
        {
            std::fwrite(row, rowBytes, 1, f);
        }
        else
        {
            for (uint32_t x = 0; x < width; ++x)
            {
                tmp[x * 4 + 0] = row[x * 4 + 2];
                tmp[x * 4 + 1] = row[x * 4 + 1];
                tmp[x * 4 + 2] = row[x * 4 + 0];
                tmp[x * 4 + 3] = row[x * 4 + 3];
            }
            std::fwrite(tmp.data(), rowBytes, 1, f);
        }
    }

    std::fclose(f);
    return true;
}

class Recorder
{
public:
    bool Start(const char* path, uint32_t width, uint32_t height,
               uint32_t fps, uint32_t bitrateKbps, uint32_t vkFormat, bool withAudio);
    bool WriteFrame(const uint8_t* pixels, uint32_t srcStride, uint64_t timestampNs);
    void Finish();
    bool Active() const { return m_fmtCtx != nullptr; }
    uint64_t Frames() const { return m_frames; }
    void SetReqPtr(const uint32_t* p, const uint32_t* pidPtr = nullptr, int32_t* peakPtr = nullptr) { m_reqPtr = p; m_gamePidPtr = pidPtr; m_audioPeakMilliDbPtr = peakPtr; }
    void AdvanceBaseForPause(uint64_t deltaNs) { if (m_baseNs.load() > 0) m_baseNs.fetch_add(deltaNs); }
    uint32_t Width() const { return m_width; }
    uint32_t Height() const { return m_height; }

private:
    void DrainPackets(AVCodecContext* ctx, AVStream* stream, AVPacket* pkt);
    void AudioLoop();

    std::mutex       m_muxMtx;
    AVFormatContext* m_fmtCtx    = nullptr;
    AVCodecContext*  m_vCodecCtx = nullptr;
    AVStream*        m_vStream   = nullptr;
    AVFrame*         m_vFrame    = nullptr;
    AVPacket*        m_vPkt      = nullptr;
    SwsContext*      m_swsCtx    = nullptr;

    AVCodecContext*  m_aCodecCtx = nullptr;
    AVStream*        m_aStream   = nullptr;
    AVFrame*         m_aFrame    = nullptr;
    AVPacket*        m_aPkt      = nullptr;
    SwrContext*      m_swrCtx    = nullptr;
    std::thread      m_audioThread;
    std::atomic<bool> m_audioRun{ false };
    int64_t          m_nextAudioPts = 0;

    uint32_t         m_width = 0, m_height = 0, m_fps = 30;
    std::atomic<uint64_t> m_baseNs{ 0 };
    const uint32_t*       m_reqPtr = nullptr;
    const uint32_t*       m_gamePidPtr = nullptr;
    int32_t*              m_audioPeakMilliDbPtr = nullptr;
    int64_t          m_lastPts = -1;
    uint64_t         m_frames = 0;
};

void Recorder::DrainPackets(AVCodecContext* ctx, AVStream* stream, AVPacket* pkt)
{
    if (!pkt) return;
    while (avcodec_receive_packet(ctx, pkt) == 0)
    {
        av_packet_rescale_ts(pkt, ctx->time_base, stream->time_base);
        pkt->stream_index = stream->index;
        std::lock_guard<std::mutex> lk(m_muxMtx);
        av_interleaved_write_frame(m_fmtCtx, pkt);
        av_packet_unref(pkt);
    }
}

static std::string ToLowerStr(const char* s)
{
    std::string out;
    if (!s) return out;
    out.reserve(std::strlen(s));
    for (; *s; ++s) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(*s))));
    return out;
}

static std::string ReadGameExeToken(uint32_t pid)
{
    if (pid == 0) return {};
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%u/cmdline", pid);
    FILE* f = std::fopen(path, "rb");
    if (!f) return {};
    char buf[512] = {};
    size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    if (n == 0) return {};
    // Walk null-separated argv to find a .exe or the first non-wine binary name
    std::string best;
    size_t pos = 0;
    while (pos < n)
    {
        std::string arg(buf + pos);
        pos += arg.size() + 1;
        if (arg.empty()) continue;
        size_t slash = arg.find_last_of("/\\");
        std::string base = (slash != std::string::npos) ? arg.substr(slash + 1) : arg;
        std::string low = ToLowerStr(base.c_str());
        if (low.size() > 4 && low.substr(low.size() - 4) == ".exe")
        {
            best = low.substr(0, low.size() - 4);
            break;
        }
        if (best.empty() && low.find("wine") == std::string::npos && low.find("proton") == std::string::npos)
            best = low;
    }
    return best;
}

static bool IsExcludedDesktopApp(const std::string& low)
{
    static const char* kExcl[] = {
        "firefox", "chrome", "chromium", "brave", "vivaldi", "opera", "edge",
        "discord", "vesktop", "telegram", "slack", "teams", "zoom",
        "spotify", "vlc", "mpv", "audacious", "rhythmbox", "clementine",
        "vkcapture", "pipewire", "pulseaudio", "gnome-shell", "obs"
    };
    for (const char* ex : kExcl)
        if (low.find(ex) != std::string::npos) return true;
    return false;
}

static bool IsGameSinkInput(const pa_sink_input_info* info, const std::string& pidStr, const std::string& exeToken)
{
    if (!info || !info->proplist) return false;
    const char* p    = pa_proplist_gets(info->proplist, PA_PROP_APPLICATION_PROCESS_ID);
    const char* name = pa_proplist_gets(info->proplist, PA_PROP_APPLICATION_NAME);
    const char* bin  = pa_proplist_gets(info->proplist, PA_PROP_APPLICATION_PROCESS_BINARY);
    std::string combined = ToLowerStr(name) + " " + ToLowerStr(bin) + " " + ToLowerStr(info->name);
    if (IsExcludedDesktopApp(combined)) return false;
    if (!pidStr.empty() && p && pidStr == p) return true;
    if (exeToken.size() >= 3 && combined.find(exeToken) != std::string::npos) return true;
    if (combined.find("wine") != std::string::npos ||
        combined.find("proton") != std::string::npos ||
        combined.find("grand theft") != std::string::npos ||
        combined.find("gta") != std::string::npos ||
        combined.find(".exe") != std::string::npos)
        return true;
    return false;
}

void Recorder::AudioLoop()
{
    const int frameSize = m_aFrame->nb_samples;
    std::vector<int16_t> pcm(static_cast<size_t>(frameSize) * 2, 0);
    auto updatePeakMeter = [&](const int16_t* samples, size_t count) {
        if (!m_audioPeakMilliDbPtr) return;
        int peak = 0;
        for (size_t i = 0; i < count; ++i)
        {
            int v = samples[i] < 0 ? -static_cast<int>(samples[i]) : static_cast<int>(samples[i]);
            if (v > peak) peak = v;
        }
        float lin = static_cast<float>(peak) / 32768.0f;
        float db = (lin > 1e-5f) ? std::max(-96.0f, 20.0f * std::log10(lin)) : -96.0f;
        __atomic_store_n(m_audioPeakMilliDbPtr, static_cast<int32_t>(db * 1000.0f), __ATOMIC_RELAXED);
    };

    uint32_t gamePid = m_gamePidPtr ? __atomic_load_n(m_gamePidPtr, __ATOMIC_RELAXED) : 0;
    std::string pidStr = (gamePid > 0) ? std::to_string(gamePid) : std::string();
    std::string exeToken = ReadGameExeToken(gamePid);

    // ── Virtual sink isolation: route ONLY the game's sink-inputs into vkcapture_game_sink,
    //    loop it back to @DEFAULT_SINK@ (10ms) so the user hears the game normally, and
    //    record vkcapture_game_sink.monitor so desktop/browser/Discord audio is 100% excluded.
    struct MovedStream { uint32_t idx; uint32_t origSink; };
    struct RouteState {
        pa_threaded_mainloop* ml = nullptr;
        pa_context* ctx = nullptr;
        std::string pidStr;
        std::string exeToken;
        bool ready = false;
        bool opDone = false;
        uint32_t nullModIdx = PA_INVALID_INDEX;
        uint32_t loopModIdx = PA_INVALID_INDEX;
        uint32_t gameSinkIdx = PA_INVALID_INDEX;
        std::vector<MovedStream> moved;
        std::mutex mtx;
    } rs;
    rs.pidStr = pidStr;
    rs.exeToken = exeToken;

    std::string recordSource = "@DEFAULT_MONITOR@";
    pa_threaded_mainloop* ml = pa_threaded_mainloop_new();
    pa_context* ctx = nullptr;
    if (ml)
    {
        rs.ml = ml;
        ctx = pa_context_new(pa_threaded_mainloop_get_api(ml), "vkcapture-router");
        rs.ctx = ctx;
        if (ctx && pa_context_connect(ctx, nullptr, PA_CONTEXT_NOFLAGS, nullptr) >= 0)
        {
            pa_context_set_state_callback(ctx, [](pa_context* c, void* ud) {
                auto* s = static_cast<RouteState*>(ud);
                pa_context_state_t cs = pa_context_get_state(c);
                if (cs == PA_CONTEXT_READY || cs == PA_CONTEXT_FAILED || cs == PA_CONTEXT_TERMINATED)
                {
                    s->ready = (cs == PA_CONTEXT_READY);
                    pa_threaded_mainloop_signal(s->ml, 0);
                }
            }, &rs);

            pa_threaded_mainloop_lock(ml);
            pa_threaded_mainloop_start(ml);
            while (pa_context_get_state(ctx) != PA_CONTEXT_READY &&
                   pa_context_get_state(ctx) != PA_CONTEXT_FAILED &&
                   pa_context_get_state(ctx) != PA_CONTEXT_TERMINATED)
                pa_threaded_mainloop_wait(ml);

            auto waitOp = [&](pa_operation* op) {
                if (!op) return;
                while (!rs.opDone && pa_operation_get_state(op) == PA_OPERATION_RUNNING)
                    pa_threaded_mainloop_wait(ml);
                pa_operation_unref(op);
            };

            if (rs.ready)
            {
                // 1. Discover matching game sink-inputs first
                rs.opDone = false;
                waitOp(pa_context_get_sink_input_info_list(ctx,
                    [](pa_context*, const pa_sink_input_info* info, int eol, void* ud) {
                        auto* s = static_cast<RouteState*>(ud);
                        if (eol > 0) { s->opDone = true; pa_threaded_mainloop_signal(s->ml, 0); return; }
                        if (!info) return;
                        const char* p    = info->proplist ? pa_proplist_gets(info->proplist, PA_PROP_APPLICATION_PROCESS_ID) : nullptr;
                        const char* name = info->proplist ? pa_proplist_gets(info->proplist, PA_PROP_APPLICATION_NAME) : nullptr;
                        const char* bin  = info->proplist ? pa_proplist_gets(info->proplist, PA_PROP_APPLICATION_PROCESS_BINARY) : nullptr;
                        bool match = IsGameSinkInput(info, s->pidStr, s->exeToken);
                        std::printf("[rec] sink-input #%u pid=%s name=%s bin=%s sink=%u match=%d\n",
                                    info->index, p ? p : "?", name ? name : "?", bin ? bin : "?", info->sink, match ? 1 : 0);
                        if (match)
                        {
                            std::lock_guard<std::mutex> lk(s->mtx);
                            s->moved.push_back({ info->index, info->sink });
                        }
                    }, &rs));

                if (!rs.moved.empty())
                {
                    // 2. Create isolated virtual sink + low-latency loopback to user's speakers
                    rs.opDone = false;
                    waitOp(pa_context_load_module(ctx, "module-null-sink",
                        "sink_name=vkcapture_game_sink sink_properties=device.description=vkcapture_game_audio",
                        [](pa_context*, uint32_t idx, void* ud) {
                            auto* s = static_cast<RouteState*>(ud);
                            s->nullModIdx = idx;
                            s->opDone = true;
                            pa_threaded_mainloop_signal(s->ml, 0);
                        }, &rs));

                    if (rs.nullModIdx != PA_INVALID_INDEX)
                    {
                        rs.opDone = false;
                        waitOp(pa_context_get_sink_info_by_name(ctx, "vkcapture_game_sink",
                            [](pa_context*, const pa_sink_info* info, int eol, void* ud) {
                                auto* s = static_cast<RouteState*>(ud);
                                if (eol > 0) { s->opDone = true; pa_threaded_mainloop_signal(s->ml, 0); return; }
                                if (info) s->gameSinkIdx = info->index;
                            }, &rs));

                        rs.opDone = false;
                        waitOp(pa_context_load_module(ctx, "module-loopback",
                            "source=vkcapture_game_sink.monitor latency_msec=10",
                            [](pa_context*, uint32_t idx, void* ud) {
                                auto* s = static_cast<RouteState*>(ud);
                                s->loopModIdx = idx;
                                s->opDone = true;
                                pa_threaded_mainloop_signal(s->ml, 0);
                            }, &rs));
                    }

                    if (rs.gameSinkIdx != PA_INVALID_INDEX && rs.loopModIdx != PA_INVALID_INDEX)
                    {
                        // 3. Move all matched game sink-inputs into vkcapture_game_sink
                        for (const auto& m : rs.moved)
                        {
                            rs.opDone = false;
                            waitOp(pa_context_move_sink_input_by_index(ctx, m.idx, rs.gameSinkIdx,
                                [](pa_context*, int, void* ud) {
                                    auto* s = static_cast<RouteState*>(ud);
                                    s->opDone = true;
                                    pa_threaded_mainloop_signal(s->ml, 0);
                                }, &rs));
                        }

                        // 4. Auto-route any new sink-inputs opened by the game mid-recording
                        pa_context_set_subscribe_callback(ctx,
                            [](pa_context* c, pa_subscription_event_type_t t, uint32_t idx, void* ud) {
                                if ((t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) == PA_SUBSCRIPTION_EVENT_SINK_INPUT &&
                                    (t & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_NEW)
                                {
                                    pa_operation* op = pa_context_get_sink_input_info(c, idx,
                                        [](pa_context* c2, const pa_sink_input_info* info, int eol, void* ud2) {
                                            if (eol != 0 || !info) return;
                                            auto* s = static_cast<RouteState*>(ud2);
                                            if (info->sink == s->gameSinkIdx) return;
                                            if (IsGameSinkInput(info, s->pidStr, s->exeToken))
                                            {
                                                {
                                                    std::lock_guard<std::mutex> lk(s->mtx);
                                                    s->moved.push_back({ info->index, info->sink });
                                                }
                                                pa_operation* mop = pa_context_move_sink_input_by_index(c2, info->index, s->gameSinkIdx, nullptr, nullptr);
                                                if (mop) pa_operation_unref(mop);
                                            }
                                        }, ud);
                                    if (op) pa_operation_unref(op);
                                }
                            }, &rs);
                        pa_operation* sop = pa_context_subscribe(ctx, PA_SUBSCRIPTION_MASK_SINK_INPUT, nullptr, nullptr);
                        if (sop) pa_operation_unref(sop);

                        recordSource = "vkcapture_game_sink.monitor";
                        std::printf("[rec] Audio source: game-only virtual sink (%zu stream(s) isolated, exe=%s, pid=%u)\n",
                                    rs.moved.size(), exeToken.empty() ? "?" : exeToken.c_str(), gamePid);
                        std::fflush(stdout);
                    }
                }
            }
            pa_threaded_mainloop_unlock(ml);
        }
    }

    if (recordSource == "@DEFAULT_MONITOR@")
    {
        std::printf("[rec] Audio source: @DEFAULT_MONITOR@ fallback (pid=%u exe=%s)\n",
                    gamePid, exeToken.empty() ? "?" : exeToken.c_str());
        std::fflush(stdout);
    }

    pa_sample_spec ss = {};
    ss.format   = PA_SAMPLE_S16LE;
    ss.rate     = 48000;
    ss.channels = 2;

    pa_buffer_attr attr = {};
    attr.maxlength = static_cast<uint32_t>(-1);
    attr.fragsize  = 4096;

    int err = 0;
    pa_simple* pa = pa_simple_new(nullptr, "vkcapture-daemon", PA_STREAM_RECORD,
                                  recordSource.c_str(), "Game Audio",
                                  &ss, nullptr, &attr, &err);
    if (!pa)
    {
        std::printf("[rec] PulseAudio monitor open failed (%s): %s\n", recordSource.c_str(), pa_strerror(err));
        std::fflush(stdout);
    }
    else
    {
        bool synced = false;
        while (m_audioRun.load())
        {
            if (pa_simple_read(pa, pcm.data(), pcm.size() * sizeof(int16_t), &err) < 0)
                break;

            if (!synced)
            {
                if (m_baseNs.load(std::memory_order_acquire) == 0)
                    continue;
                pa_simple_flush(pa, &err);
                synced = true;
                continue;
            }
            if (m_reqPtr && __atomic_load_n(m_reqPtr, __ATOMIC_ACQUIRE) == 2)
            {
                pa_simple_flush(pa, &err);
                continue;
            }

            updatePeakMeter(pcm.data(), pcm.size());
            av_frame_make_writable(m_aFrame);
            const uint8_t* inData[1] = { reinterpret_cast<const uint8_t*>(pcm.data()) };
            swr_convert(m_swrCtx, m_aFrame->data, frameSize, inData, frameSize);

            m_aFrame->pts = m_nextAudioPts;
            m_nextAudioPts += frameSize;

            if (avcodec_send_frame(m_aCodecCtx, m_aFrame) == 0)
                DrainPackets(m_aCodecCtx, m_aStream, m_aPkt);
        }
        pa_simple_free(pa);
    }

    // ── Restore moved game streams back to their original sink and unload virtual sink modules
    if (ml && ctx)
    {
        pa_threaded_mainloop_lock(ml);
        if (rs.ready)
        {
            pa_context_set_subscribe_callback(ctx, nullptr, nullptr);
            auto waitOpClean = [&](pa_operation* op) {
                if (!op) return;
                while (!rs.opDone && pa_operation_get_state(op) == PA_OPERATION_RUNNING)
                    pa_threaded_mainloop_wait(ml);
                pa_operation_unref(op);
            };
            std::vector<MovedStream> toRestore;
            {
                std::lock_guard<std::mutex> lk(rs.mtx);
                toRestore = rs.moved;
            }
            for (const auto& m : toRestore)
            {
                rs.opDone = false;
                waitOpClean(pa_context_move_sink_input_by_index(ctx, m.idx, m.origSink,
                    [](pa_context*, int, void* ud) {
                        auto* s = static_cast<RouteState*>(ud);
                        s->opDone = true;
                        pa_threaded_mainloop_signal(s->ml, 0);
                    }, &rs));
            }
            if (rs.loopModIdx != PA_INVALID_INDEX)
            {
                rs.opDone = false;
                waitOpClean(pa_context_unload_module(ctx, rs.loopModIdx,
                    [](pa_context*, int, void* ud) {
                        auto* s = static_cast<RouteState*>(ud);
                        s->opDone = true;
                        pa_threaded_mainloop_signal(s->ml, 0);
                    }, &rs));
            }
            if (rs.nullModIdx != PA_INVALID_INDEX)
            {
                rs.opDone = false;
                waitOpClean(pa_context_unload_module(ctx, rs.nullModIdx,
                    [](pa_context*, int, void* ud) {
                        auto* s = static_cast<RouteState*>(ud);
                        s->opDone = true;
                        pa_threaded_mainloop_signal(s->ml, 0);
                    }, &rs));
            }
        }
        pa_threaded_mainloop_unlock(ml);
        pa_threaded_mainloop_stop(ml);
        pa_context_disconnect(ctx);
    }
    if (ctx) pa_context_unref(ctx);
    if (ml)  pa_threaded_mainloop_free(ml);
}

bool Recorder::Start(const char* path, uint32_t width, uint32_t height,
                     uint32_t fps, uint32_t bitrateKbps, uint32_t vkFormat, bool withAudio)
{
    m_width = width;
    m_height = height;
    m_fps = fps ? fps : 30;
    m_baseNs = 0;
    m_lastPts = -1;
    m_frames = 0;
    m_nextAudioPts = 0;

    if (avformat_alloc_output_context2(&m_fmtCtx, nullptr, nullptr, path) < 0 || !m_fmtCtx)
        return false;

    const AVCodec* vCodec = avcodec_find_encoder_by_name("libx264");
    if (!vCodec) vCodec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!vCodec) { Finish(); return false; }

    m_vStream = avformat_new_stream(m_fmtCtx, nullptr);
    m_vCodecCtx = avcodec_alloc_context3(vCodec);
    if (!m_vStream || !m_vCodecCtx) { Finish(); return false; }

    m_vCodecCtx->codec_id     = vCodec->id;
    m_vCodecCtx->width        = static_cast<int>(m_width);
    m_vCodecCtx->height       = static_cast<int>(m_height);
    m_vCodecCtx->time_base    = AVRational{ 1, 90000 };
    m_vCodecCtx->framerate    = AVRational{ static_cast<int>(m_fps), 1 };
    uint32_t maxKbps = bitrateKbps ? bitrateKbps : 8000;
    m_vCodecCtx->bit_rate     = 0; // use CRF with VBV maxrate cap
    m_vCodecCtx->rc_max_rate  = static_cast<int64_t>(maxKbps) * 1000LL;
    m_vCodecCtx->rc_buffer_size = static_cast<int>(maxKbps * 2000U);
    m_vCodecCtx->gop_size     = static_cast<int>(m_fps * 2);
    m_vCodecCtx->max_b_frames = 0;
    m_vCodecCtx->pix_fmt      = AV_PIX_FMT_YUV420P;
    unsigned hw = std::thread::hardware_concurrency();
    m_vCodecCtx->thread_count = static_cast<int>(std::max(1u, std::min(4u, hw ? (hw / 2) : 2u)));

    if (m_fmtCtx->oformat->flags & AVFMT_GLOBALHEADER)
        m_vCodecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    av_opt_set(m_vCodecCtx->priv_data, "preset", "superfast", 0);
    av_opt_set(m_vCodecCtx->priv_data, "crf", "26", 0);
    av_opt_set(m_vCodecCtx->priv_data, "x264-params", "rc-lookahead=10:scenecut=0", 0);

    if (avcodec_open2(m_vCodecCtx, vCodec, nullptr) < 0) { Finish(); return false; }
    avcodec_parameters_from_context(m_vStream->codecpar, m_vCodecCtx);
    m_vStream->time_base = m_vCodecCtx->time_base;

    m_vFrame = av_frame_alloc();
    m_vFrame->format = m_vCodecCtx->pix_fmt;
    m_vFrame->width  = static_cast<int>(m_width);
    m_vFrame->height = static_cast<int>(m_height);
    av_frame_get_buffer(m_vFrame, 32);
    m_vPkt = av_packet_alloc();

    AVPixelFormat srcFmt = IsRgbaFormat(vkFormat) ? AV_PIX_FMT_RGBA : AV_PIX_FMT_BGRA;
    m_swsCtx = sws_getContext(static_cast<int>(m_width), static_cast<int>(m_height), srcFmt,
                              static_cast<int>(m_width), static_cast<int>(m_height), AV_PIX_FMT_YUV420P,
                              SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    if (!m_swsCtx) { Finish(); return false; }

    bool audioReady = false;
    if (withAudio)
    {
        const AVCodec* aCodec = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (aCodec)
        {
            m_aStream = avformat_new_stream(m_fmtCtx, nullptr);
            m_aCodecCtx = avcodec_alloc_context3(aCodec);
            if (m_aStream && m_aCodecCtx)
            {
                m_aCodecCtx->sample_rate = 48000;
                m_aCodecCtx->bit_rate    = 128000;
                m_aCodecCtx->sample_fmt  = AV_SAMPLE_FMT_FLTP;
                m_aCodecCtx->time_base   = AVRational{ 1, 48000 };
#if LIBAVCODEC_VERSION_MAJOR >= 60
                AVChannelLayout layout = AV_CHANNEL_LAYOUT_STEREO;
                av_channel_layout_copy(&m_aCodecCtx->ch_layout, &layout);
#else
                m_aCodecCtx->channel_layout = AV_CH_LAYOUT_STEREO;
                m_aCodecCtx->channels = 2;
#endif
                if (m_fmtCtx->oformat->flags & AVFMT_GLOBALHEADER)
                    m_aCodecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

                if (avcodec_open2(m_aCodecCtx, aCodec, nullptr) == 0)
                {
                    avcodec_parameters_from_context(m_aStream->codecpar, m_aCodecCtx);
                    m_aStream->time_base = m_aCodecCtx->time_base;

#if LIBAVCODEC_VERSION_MAJOR >= 60
                    AVChannelLayout inLayout = AV_CHANNEL_LAYOUT_STEREO;
                    swr_alloc_set_opts2(&m_swrCtx,
                                        &m_aCodecCtx->ch_layout, m_aCodecCtx->sample_fmt, 48000,
                                        &inLayout, AV_SAMPLE_FMT_S16, 48000, 0, nullptr);
#else
                    m_swrCtx = swr_alloc_set_opts(nullptr,
                                                  AV_CH_LAYOUT_STEREO, m_aCodecCtx->sample_fmt, 48000,
                                                  AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, 48000, 0, nullptr);
#endif
                    if (m_swrCtx && swr_init(m_swrCtx) >= 0)
                    {
                        m_aFrame = av_frame_alloc();
                        m_aFrame->nb_samples = m_aCodecCtx->frame_size ? m_aCodecCtx->frame_size : 1024;
                        m_aFrame->format     = m_aCodecCtx->sample_fmt;
#if LIBAVCODEC_VERSION_MAJOR >= 60
                        av_channel_layout_copy(&m_aFrame->ch_layout, &m_aCodecCtx->ch_layout);
#else
                        m_aFrame->channel_layout = m_aCodecCtx->channel_layout;
#endif
                        av_frame_get_buffer(m_aFrame, 0);
                        m_aPkt = av_packet_alloc();
                        audioReady = true;
                    }
                }
            }
        }
    }

    if (!(m_fmtCtx->oformat->flags & AVFMT_NOFILE))
    {
        if (avio_open(&m_fmtCtx->pb, path, AVIO_FLAG_WRITE) < 0) { Finish(); return false; }
    }
    if (avformat_write_header(m_fmtCtx, nullptr) < 0) { Finish(); return false; }

    if (audioReady)
    {
        m_audioRun.store(true);
        m_audioThread = std::thread(&Recorder::AudioLoop, this);
    }

    std::printf("[rec] Recording %ux%u @ %u fps (superfast crf=26 max=%ukbps, audio=%s) -> %s\n",
                m_width, m_height, m_fps, maxKbps, audioReady ? "AAC 128k" : "off", path);
    std::fflush(stdout);
    return true;
}

bool Recorder::WriteFrame(const uint8_t* pixels, uint32_t srcStride, uint64_t timestampNs)
{
    if (!m_fmtCtx || !m_vCodecCtx) return false;
    av_frame_make_writable(m_vFrame);
    const uint8_t* srcSlice[1] = { pixels };
    int srcLinesize[1] = { static_cast<int>(srcStride) };
    sws_scale(m_swsCtx, srcSlice, srcLinesize, 0, static_cast<int>(m_height),
              m_vFrame->data, m_vFrame->linesize);

    if (timestampNs == 0) timestampNs = GetMonotonicNs();
    uint64_t base = m_baseNs.load(std::memory_order_relaxed);
    if (base == 0) { base = timestampNs; m_baseNs.store(base, std::memory_order_release); }
    int64_t stepPts = std::max<int64_t>(1, 90000 / static_cast<int64_t>(m_fps ? m_fps : 30));
    int64_t pts = (m_lastPts < 0) ? 0 : (m_lastPts + stepPts);
    if (timestampNs >= base)
    {
        uint64_t deltaNs = timestampNs - base;
        int64_t candPts = static_cast<int64_t>((deltaNs / 1000000000ULL) * 90000ULL +
                                               ((deltaNs % 1000000000ULL) * 90000ULL) / 1000000000ULL);
        if (m_lastPts < 0)
            pts = candPts;
        else if (candPts > m_lastPts && (candPts - m_lastPts) <= 90000LL * 3600)
            pts = candPts;
    }
    if (pts <= m_lastPts) pts = m_lastPts + 1;
    m_lastPts = pts;
    m_vFrame->pts = pts;

    if (avcodec_send_frame(m_vCodecCtx, m_vFrame) < 0) return false;
    DrainPackets(m_vCodecCtx, m_vStream, m_vPkt);
    ++m_frames;
    return true;
}

void Recorder::Finish()
{
    if (m_audioRun.exchange(false))
    {
        if (m_audioThread.joinable()) m_audioThread.join();
    }

    if (m_fmtCtx && m_fmtCtx->pb)
    {
        if (m_vCodecCtx) { avcodec_send_frame(m_vCodecCtx, nullptr); DrainPackets(m_vCodecCtx, m_vStream, m_vPkt); }
        if (m_aCodecCtx) { avcodec_send_frame(m_aCodecCtx, nullptr); DrainPackets(m_aCodecCtx, m_aStream, m_aPkt); }
        av_write_trailer(m_fmtCtx);
    }

    if (m_swsCtx)    { sws_freeContext(m_swsCtx); m_swsCtx = nullptr; }
    if (m_vPkt)      { av_packet_free(&m_vPkt); }
    if (m_vFrame)    { av_frame_free(&m_vFrame); }
    if (m_vCodecCtx) { avcodec_free_context(&m_vCodecCtx); }
    if (m_swrCtx)    { swr_free(&m_swrCtx); }
    if (m_aPkt)      { av_packet_free(&m_aPkt); }
    if (m_aFrame)    { av_frame_free(&m_aFrame); }
    if (m_aCodecCtx) { avcodec_free_context(&m_aCodecCtx); }
    if (m_fmtCtx)
    {
        if (!(m_fmtCtx->oformat->flags & AVFMT_NOFILE) && m_fmtCtx->pb)
            avio_closep(&m_fmtCtx->pb);
        avformat_free_context(m_fmtCtx);
        m_fmtCtx = nullptr;
    }
    if (m_frames > 0)
    {
        std::printf("[rec] Wrote %llu frames.\n", static_cast<unsigned long long>(m_frames));
        std::fflush(stdout);
    }
}

static volatile sig_atomic_t g_stop = 0;
static void SigHandler(int) { g_stop = 1; }

struct AsyncFrame
{
    std::vector<uint8_t> pixels;
    uint32_t width = 0, height = 0, stride = 0, format = 0;
    uint32_t fps = 30, bitrateKbps = 16000;
    bool     withAudio = true;
    uint64_t timestampNs = 0;
    char     path[512] = "";
    bool     isEndSession = false;
};

static int RunDaemon()
{
    int shmFd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shmFd < 0) { std::perror("[daemon] shm_open"); return 1; }
    if (ftruncate(shmFd, static_cast<off_t>(SHM_TOTAL_BYTES)) != 0)
    {
        std::perror("[daemon] ftruncate");
        close(shmFd);
        return 1;
    }
    void* pView = mmap(nullptr, SHM_TOTAL_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, shmFd, 0);
    if (pView == MAP_FAILED) { std::perror("[daemon] mmap"); close(shmFd); return 1; }

    ShmControlHeader* ctrl = static_cast<ShmControlHeader*>(pView);
    if (ctrl->magic != SHM_MAGIC)
    {
        std::memset(ctrl, 0, sizeof(*ctrl));
        ctrl->magic     = SHM_MAGIC;
        ctrl->numSlots  = SHM_SLOTS;
        ctrl->slotBytes = SHM_SLOT_BYTES;
    }
    __atomic_store_n(&ctrl->recordRequested, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&ctrl->menuOpen, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&ctrl->daemonRecordedFrames, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&ctrl->daemonAudioPeakMilliDb, -96000, __ATOMIC_RELAXED);



    std::mutex              qMtx;
    std::condition_variable qCv;
    std::deque<AsyncFrame>  q;
    std::vector<std::vector<uint8_t>> pool;
    std::atomic<bool>       encRun{ true };

    std::thread encThread([&]() {
        Recorder recorder;
        recorder.SetReqPtr(&ctrl->recordRequested, &ctrl->gamePid, &ctrl->daemonAudioPeakMilliDb);
        while (encRun.load() || !q.empty())
        {
            AsyncFrame af;
            {
                std::unique_lock<std::mutex> lk(qMtx);
                qCv.wait(lk, [&]() { return !q.empty() || !encRun.load(); });
                if (q.empty()) continue;
                af = std::move(q.front());
                q.pop_front();
            }

            if (af.isEndSession)
            {
                if (recorder.Active()) recorder.Finish();
                __atomic_store_n(&ctrl->daemonRecordedFrames, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&ctrl->daemonAudioPeakMilliDb, -96000, __ATOMIC_RELAXED);
            }
            else
            {
                if (af.width == 0 && af.height == 0 && af.timestampNs > 0)
                    recorder.AdvanceBaseForPause(af.timestampNs);
                if (!recorder.Active() && af.width > 0 && af.height > 0)
                    recorder.Start(af.path, af.width, af.height, af.fps, af.bitrateKbps, af.format, af.withAudio);
                if (recorder.Active() && af.width == recorder.Width() && af.height == recorder.Height())
                {
                    recorder.WriteFrame(af.pixels.data(), af.stride, af.timestampNs);
                    __atomic_store_n(&ctrl->daemonRecordedFrames, recorder.Frames(), __ATOMIC_RELAXED);
                }
                std::lock_guard<std::mutex> lk(qMtx);
                if (pool.size() < 16)
                {
                    af.pixels.clear();
                    pool.push_back(std::move(af.pixels));
                }
            }
        }
        if (recorder.Active()) recorder.Finish();
    });

    std::thread meterThread([&]() {
        pa_simple* pa = nullptr;
        std::vector<int16_t> buf(960 * 2, 0); // 20ms @ 48kHz stereo
        while (encRun.load())
        {
            uint32_t req = __atomic_load_n(&ctrl->recordRequested, __ATOMIC_RELAXED);
            uint32_t met = __atomic_load_n(&ctrl->meterRequested, __ATOMIC_RELAXED);
            if (req != 0 || met == 0)
            {
                if (pa) { pa_simple_free(pa); pa = nullptr; }
                if (req == 0)
                    __atomic_store_n(&ctrl->daemonAudioPeakMilliDb, -96000, __ATOMIC_RELAXED);
                usleep(50000);
                continue;
            }
            if (!pa)
            {
                pa_sample_spec ss = { PA_SAMPLE_S16LE, 48000, 2 };
                pa_buffer_attr attr = {};
                attr.maxlength = static_cast<uint32_t>(-1);
                attr.fragsize  = static_cast<uint32_t>(buf.size() * sizeof(int16_t));
                int err = 0;
                pa = pa_simple_new(nullptr, "vkcapture-meter", PA_STREAM_RECORD,
                                   "@DEFAULT_MONITOR@", "Live Audio Meter",
                                   &ss, nullptr, &attr, &err);
                if (!pa) { usleep(250000); continue; }
            }
            int err = 0;
            if (pa_simple_read(pa, buf.data(), buf.size() * sizeof(int16_t), &err) < 0)
            {
                pa_simple_free(pa);
                pa = nullptr;
                usleep(100000);
                continue;
            }
            int peak = 0;
            for (int16_t s : buf)
            {
                int v = s < 0 ? -static_cast<int>(s) : static_cast<int>(s);
                if (v > peak) peak = v;
            }
            float lin = static_cast<float>(peak) / 32768.0f;
            float db = (lin > 1e-5f) ? std::max(-96.0f, 20.0f * std::log10(lin)) : -96.0f;
            if (__atomic_load_n(&ctrl->recordRequested, __ATOMIC_RELAXED) == 0)
                __atomic_store_n(&ctrl->daemonAudioPeakMilliDb, static_cast<int32_t>(db * 1000.0f), __ATOMIC_RELAXED);
        }
        if (pa) pa_simple_free(pa);
    });

    std::printf("[daemon] 4-slot A/V recorder daemon active...\n");
    std::fflush(stdout);

    uint64_t lastIdx = 0;
    uint32_t curSessionId = 0;
    bool     armed = false;
    bool     wasRecording = false;

    while (!g_stop)
    {
        __atomic_store_n(&ctrl->daemonHeartbeatNs, GetMonotonicNs(), __ATOMIC_RELAXED);
        uint32_t req = __atomic_load_n(&ctrl->recordRequested, __ATOMIC_ACQUIRE);

        static uint64_t s_pauseStartNs = 0;
        if (req == 2)
        {
            if (s_pauseStartNs == 0) s_pauseStartNs = GetMonotonicNs();
            usleep(20000);
            continue;
        }
        if (req == 1 && s_pauseStartNs != 0)
        {
            uint64_t pausedDelta = GetMonotonicNs() - s_pauseStartNs;
            s_pauseStartNs = 0;
            // Adjust baseNs via a zero-width pause marker or directly on next frame:
            // push a pause-delta AsyncFrame? Even simpler: handle via recorder.AdvanceBaseForPause in encThread!
            AsyncFrame p;
            p.isEndSession = false;
            p.width = 0;
            p.height = 0;
            p.timestampNs = pausedDelta;
            std::lock_guard<std::mutex> lk(qMtx);
            q.push_back(std::move(p));
            qCv.notify_one();
        }
        if (!req)
        {
            s_pauseStartNs = 0;
            if (wasRecording)
            {
                wasRecording = false;
                AsyncFrame end;
                end.isEndSession = true;
                std::lock_guard<std::mutex> lk(qMtx);
                q.push_back(std::move(end));
                qCv.notify_one();
            }
            armed = false;
            lastIdx = 0;
            usleep(20000);
            continue;
        }

        wasRecording = true;
        if (!armed)
        {
            curSessionId = __atomic_load_n(&ctrl->recordSessionId, __ATOMIC_ACQUIRE);
            lastIdx = 0;
            armed = true;
            std::printf("[daemon] armed sessionId=%u readIdx=%u writeIdx=%u\n",
                        curSessionId, ctrl->readIdx, ctrl->writeIdx);
            std::fflush(stdout);
        }

        uint32_t rIdx = __atomic_load_n(&ctrl->readIdx, __ATOMIC_RELAXED);
        uint32_t wIdx = __atomic_load_n(&ctrl->writeIdx, __ATOMIC_ACQUIRE);
        if (rIdx == wIdx)
        {
            usleep(4000);
            continue;
        }
        if (__atomic_load_n(&ctrl->recordRequested, __ATOMIC_ACQUIRE) == 0)
        {
            __atomic_store_n(&ctrl->readIdx, rIdx + 1, __ATOMIC_RELEASE);
            continue;
        }

        uint32_t slotIdx = rIdx % SHM_SLOTS;
        const uint8_t* slotBase = static_cast<const uint8_t*>(pView) + 4096ULL + static_cast<size_t>(slotIdx) * SHM_SLOT_BYTES;
        const ShmSlotHeader* sh = reinterpret_cast<const ShmSlotHeader*>(slotBase);

        uint32_t fps = ctrl->recordFps ? ctrl->recordFps : 30;
        uint32_t br  = ctrl->recordBitrateKbps ? ctrl->recordBitrateKbps : 16000;

        if (!(sh->sessionId == curSessionId && sh->frameIdx > lastIdx && sh->width > 0 && sh->height > 0))
        {
            static int s_dropLog = 0;
            if (s_dropLog < 10)
            {
                ++s_dropLog;
                std::printf("[daemon] drop slot=%u sid=%u(cur=%u) frameIdx=%llu(last=%llu) %ux%u\n",
                            slotIdx, sh->sessionId, curSessionId,
                            static_cast<unsigned long long>(sh->frameIdx),
                            static_cast<unsigned long long>(lastIdx),
                            sh->width, sh->height);
                std::fflush(stdout);
            }
        }
        else
        {
            lastIdx = sh->frameIdx;
            size_t bytes = static_cast<size_t>(sh->stride) * sh->height;
            AsyncFrame af;
            af.width       = sh->width;
            af.height      = sh->height;
            af.stride      = sh->stride;
            af.format      = sh->format;
            af.fps         = fps;
            af.bitrateKbps = br;
            af.withAudio   = (__atomic_load_n(&ctrl->recordAudio, __ATOMIC_RELAXED) != 0);
            af.timestampNs = sh->timestampNs;
            std::snprintf(af.path, sizeof(af.path), "%s",
                          ctrl->recordPath[0] ? ctrl->recordPath : "/tmp/vkcapture_out.mp4");

            {
                std::lock_guard<std::mutex> lk(qMtx);
                if (!pool.empty()) { af.pixels = std::move(pool.back()); pool.pop_back(); }
            }
            af.pixels.resize(bytes);
            std::memcpy(af.pixels.data(), slotBase + sh->dataOffset, bytes);

            {
                std::lock_guard<std::mutex> lk(qMtx);
                if (q.size() < 16)
                {
                    size_t qsz = q.size() + 1;
                    if (sh->frameIdx <= 5 || (sh->frameIdx % 120) == 0)
                    {
                        std::printf("[daemon] rx frameIdx=%llu slot=%u ts=%llu q=%zu\n",
                                    static_cast<unsigned long long>(sh->frameIdx),
                                    slotIdx,
                                    static_cast<unsigned long long>(sh->timestampNs),
                                    qsz);
                        std::fflush(stdout);
                    }
                    q.push_back(std::move(af));
                    qCv.notify_one();
                }
            }
        }

        __atomic_store_n(&ctrl->readIdx, rIdx + 1, __ATOMIC_RELEASE);
    }

    encRun.store(false);
    qCv.notify_all();
    encThread.join();
    meterThread.join();
    __atomic_store_n(&ctrl->daemonHeartbeatNs, 0, __ATOMIC_RELAXED);
    munmap(pView, SHM_TOTAL_BYTES);
    close(shmFd);
    return 0;
}

int main(int argc, char* argv[])
{
    bool        daemonMode  = false;
    int         saveCount   = 0;
    char        saveDir[MAX_PATH] = ".";
    const char* recordPath  = nullptr;
    uint32_t    fps         = 30;
    uint32_t    bitrateKbps = 16000;
    double      seconds     = 0.0;

    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--daemon") == 0)                      daemonMode = true;
        else if (std::strcmp(argv[i], "--save") == 0 && i + 1 < argc)
        {
            saveCount = std::atoi(argv[++i]);
            if (i + 1 < argc && argv[i + 1][0] != '-')
                std::snprintf(saveDir, sizeof(saveDir), "%s", argv[++i]);
        }
        else if (std::strcmp(argv[i], "--record") == 0 && i + 1 < argc)  recordPath = argv[++i];
        else if (std::strcmp(argv[i], "--fps") == 0 && i + 1 < argc)     fps = static_cast<uint32_t>(std::atoi(argv[++i]));
        else if (std::strcmp(argv[i], "--bitrate") == 0 && i + 1 < argc) bitrateKbps = static_cast<uint32_t>(std::atoi(argv[++i]));
        else if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) seconds = std::atof(argv[++i]);
    }
    if (fps == 0) fps = 30;

    std::signal(SIGINT, SigHandler);
    std::signal(SIGTERM, SigHandler);

    if (daemonMode)
    {
        if (nice(10) == -1) {}
        return RunDaemon();
    }

    int saved = 0;
    int shmFd = -1;
    for (int retry = 0; retry < 100 && shmFd < 0 && !g_stop; ++retry)
    {
        shmFd = shm_open(SHM_NAME, O_RDONLY, 0666);
        if (shmFd < 0) usleep(100000);
    }
    if (shmFd < 0) { std::printf("[reader] No shared memory yet.\n"); return 1; }

    const void* pView = mmap(nullptr, SHM_TOTAL_BYTES, PROT_READ, MAP_SHARED, shmFd, 0);
    if (pView == MAP_FAILED) { close(shmFd); return 1; }

    const ShmControlHeader* ctrl = static_cast<const ShmControlHeader*>(pView);
    Recorder recorder;
    uint64_t startNs = GetMonotonicNs();
    uint32_t readIdx = __atomic_load_n(&ctrl->writeIdx, __ATOMIC_ACQUIRE);

    uint64_t lastIdx = 0;
    while (!g_stop)
    {
        uint32_t writeIdx = __atomic_load_n(&ctrl->writeIdx, __ATOMIC_ACQUIRE);
        if (readIdx == writeIdx)
        {
            if (seconds > 0.0 && (static_cast<double>(GetMonotonicNs() - startNs) / 1e9 >= seconds))
                break;
            usleep(1000);
            continue;
        }
        if (static_cast<uint32_t>(writeIdx - readIdx) > SHM_SLOTS)
            readIdx = writeIdx - 1;

        const uint8_t* slotBase = static_cast<const uint8_t*>(pView) + 4096ULL + static_cast<size_t>(readIdx % SHM_SLOTS) * SHM_SLOT_BYTES;
        ++readIdx;
        const ShmSlotHeader* sh = reinterpret_cast<const ShmSlotHeader*>(slotBase);
        const uint8_t* data = slotBase + sh->dataOffset;

        if (sh->frameIdx > lastIdx && sh->width > 0 && sh->height > 0)
        {
            lastIdx = sh->frameIdx;
            if (recordPath && !recorder.Active())
                if (!recorder.Start(recordPath, sh->width, sh->height, fps, bitrateKbps, sh->format, true))
                    recordPath = nullptr;
            if (recorder.Active())
                recorder.WriteFrame(data, sh->stride, sh->timestampNs);

            if (saved < saveCount)
            {
                char path[MAX_PATH];
                std::snprintf(path, sizeof(path), "%s/shm_%05llu.bmp",
                              saveDir, static_cast<unsigned long long>(sh->frameIdx));
                if (SaveBmp(path, data, sh->width, sh->height, sh->stride, sh->format)) ++saved;
                else saveCount = 0;
            }
        }

        if (seconds > 0.0 && (static_cast<double>(GetMonotonicNs() - startNs) / 1e9 >= seconds))
            break;
    }

    if (recorder.Active()) recorder.Finish();
    munmap(const_cast<void*>(pView), SHM_TOTAL_BYTES);
    close(shmFd);
    return 0;
}
