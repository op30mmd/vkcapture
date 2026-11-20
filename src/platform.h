#pragma once
/**
 * platform.h — Linux/POSIX utility helpers (clock, paths, safe strings)
 */

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cstdlib>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>

#ifndef MAX_PATH
#define MAX_PATH PATH_MAX
#endif

// Monotonic nanosecond timestamp (replaces QueryPerformanceCounter)
inline uint64_t Platform_GetMonotonicNs()
{
    struct timespec ts = {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

// Monotonic millisecond tick count (replaces GetTickCount)
inline uint64_t Platform_GetTickMs()
{
    return Platform_GetMonotonicNs() / 1000000ULL;
}

inline void Platform_SafeStrCpy(char* dst, size_t dstSize, const char* src)
{
    if (!dst || dstSize == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    std::snprintf(dst, dstSize, "%s", src);
}

inline bool Platform_EnsureDir(const char* path)
{
    if (!path || !path[0]) return false;
    char tmp[MAX_PATH];
    Platform_SafeStrCpy(tmp, sizeof(tmp), path);
    size_t len = std::strlen(tmp);
    if (len > 1 && tmp[len - 1] == '/')
        tmp[len - 1] = '\0';

    for (char* p = tmp + 1; *p; ++p)
    {
        if (*p == '/')
        {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
                return false;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        return false;
    return true;
}

// Base directory: $VKCAPTURE_DIR, or $XDG_DATA_HOME/vkcapture, or $HOME/.local/share/vkcapture, or /tmp/vkcapture
inline const char* Platform_GetBaseDir()
{
    static char s_base[MAX_PATH] = "";
    if (s_base[0] != '\0') return s_base;

    const char* envDir = std::getenv("VKCAPTURE_DIR");
    if (envDir && envDir[0])
    {
        std::snprintf(s_base, sizeof(s_base), "%s", envDir);
    }
    else
    {
        const char* xdg = std::getenv("XDG_DATA_HOME");
        const char* home = std::getenv("HOME");
        if (xdg && xdg[0])
            std::snprintf(s_base, sizeof(s_base), "%s/vkcapture", xdg);
        else if (home && home[0])
            std::snprintf(s_base, sizeof(s_base), "%s/.local/share/vkcapture", home);
        else
            std::snprintf(s_base, sizeof(s_base), "/tmp/vkcapture");
    }

    size_t len = std::strlen(s_base);
    if (len > 1 && s_base[len - 1] == '/')
        s_base[len - 1] = '\0';

    Platform_EnsureDir(s_base);
    return s_base;
}
