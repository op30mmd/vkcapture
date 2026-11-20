#pragma once
/**
 * audio.h  —  Low-latency in-process game audio capture (Linux port)
 *
 * Captures PCM directly from the game's audio stream via LD_PRELOAD symbol
 * interposition (PulseAudio pa_stream_write / pa_simple_write, PipeWire via
 * pipewire-pulse, and ALSA snd_pcm_writei / snd_pcm_mmap_commit), with
 * automatic fallback to PulseAudio/PipeWire @DEFAULT_MONITOR@ loopback.
 */

#include <cstdint>

enum AudioSourceKind
{
    AudioSource_None     = 0,  // nothing has been captured yet
    AudioSource_Hook     = 1,  // the game's own audio API write hook (low latency)
    AudioSource_Loopback = 2   // PulseAudio/PipeWire monitor loopback fallback
};

enum AudioCaptureMode
{
    AudioCaptureMode_Auto       = 0, // Direct hook prioritized; auto-fallback to loopback
    AudioCaptureMode_DirectHook = 1, // Only in-process PulseAudio/ALSA hooks
    AudioCaptureMode_Loopback   = 2  // System monitor loopback capture
};

static constexpr uint32_t AUDIO_MAX_CHUNK_FRAMES = 4096;

struct AudioStats
{
    bool     hooksInstalled;   // LD_PRELOAD audio hooks active
    bool     enabled;          // master switch (overlay)
    bool     wanted;           // a consumer is currently listening
    int      source;           // AudioSourceKind (active source)
    int      captureMode;      // AudioCaptureMode (configured mode)
    uint32_t sampleRate;
    uint32_t channels;
    uint32_t bitsPerSample;
    bool     isFloat;
    float    devicePeriodMs;
    uint64_t capturedFrames;
    uint64_t droppedFrames;
    uint64_t overruns;
    uint32_t ringFillPercent;
    float    peakDb;
    uint32_t activeStreams;
    char     lastError[128];
};

void Audio_Init();
void Audio_Shutdown();

void Audio_SetCaptureMode(int mode);
int  Audio_GetCaptureMode();

void Audio_SetEnabled(bool enabled);
bool Audio_IsEnabled();

void Audio_SetWanted(bool wanted);
bool Audio_IsWanted();

void Audio_Flush();
uint32_t Audio_GetSampleRate();

/**
 * Drain one capture buffer, converted to interleaved 16-bit stereo.
 * *outTimestampNs receives the CLOCK_MONOTONIC nanosecond stamp.
 */
uint32_t Audio_ReadStereo16(int16_t* dst, uint32_t maxFrames, uint64_t* outTimestampNs);

void Audio_GetStats(AudioStats* out);

void  Audio_SetVolume(float volume);
float Audio_GetVolume();

void Audio_InjectTestTone(uint32_t freqHz = 440, uint32_t durationMs = 500);
