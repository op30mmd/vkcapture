/**
 * inject_tool.cpp  —  Linux Vulkan launcher & process attachment tool
 *
 * Modes:
 *   inject_tool --launch <game_bin> [libvkcapture.so] [--wait] [-- game args...]
 *     Launches the target with LD_PRELOAD + VK_LAYER_PATH + ENABLE_VKCAPTURE=1
 *     from the game's own working directory.
 *
 *   inject_tool --wait-for <process_name> <libvkcapture.so> [--timeout <sec>]
 *     Polls /proc for a process matching <process_name> that has loaded
 *     libvulkan.so.1, and reports/attaches.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <dirent.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <libgen.h>
#include <limits.h>

static void PrintUsage()
{
    std::printf(
        "Usage:\n"
        "  inject_tool --launch <game_bin> [libvkcapture.so] [--wait] [-- args...]\n"
        "  inject_tool --wait-for <process_name> <libvkcapture.so> [--timeout <sec>]\n");
}

static std::string ResolveDefaultLib(const char* argv0)
{
    char exePath[PATH_MAX] = {};
    ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (len > 0)
    {
        exePath[len] = '\0';
        char* dir = dirname(exePath);
        std::string candidate = std::string(dir) + "/libvkcapture.so";
        if (access(candidate.c_str(), R_OK) == 0) return candidate;
    }
    (void)argv0;
    return "libvkcapture.so";
}

static std::string ResolveLayerDir(const std::string& libPath)
{
    char buf[PATH_MAX] = {};
    if (realpath(libPath.c_str(), buf))
    {
        return std::string(dirname(buf));
    }
    return ".";
}

static int FindProcessWithVulkan(const char* procName)
{
    DIR* dir = opendir("/proc");
    if (!dir) return -1;

    struct dirent* ent = nullptr;
    while ((ent = readdir(dir)) != nullptr)
    {
        int pid = std::atoi(ent->d_name);
        if (pid <= 0) continue;

        char commPath[128];
        std::snprintf(commPath, sizeof(commPath), "/proc/%d/comm", pid);
        FILE* f = std::fopen(commPath, "r");
        if (!f) continue;

        char comm[256] = {};
        if (std::fgets(comm, sizeof(comm), f))
        {
            size_t l = std::strlen(comm);
            if (l > 0 && comm[l - 1] == '\n') comm[l - 1] = '\0';
        }
        std::fclose(f);

        if (std::strcmp(comm, procName) != 0) continue;

        char mapsPath[128];
        std::snprintf(mapsPath, sizeof(mapsPath), "/proc/%d/maps", pid);
        FILE* mf = std::fopen(mapsPath, "r");
        if (!mf) continue;

        bool hasVulkan = false;
        char line[512];
        while (std::fgets(line, sizeof(line), mf))
        {
            if (std::strstr(line, "libvulkan.so")) { hasVulkan = true; break; }
        }
        std::fclose(mf);

        if (hasVulkan) { closedir(dir); return pid; }
    }

    closedir(dir);
    return -1;
}

int main(int argc, char* argv[])
{
    if (argc < 3) { PrintUsage(); return 1; }

    if (std::strcmp(argv[1], "--launch") == 0)
    {
        const char* gameBin = argv[2];
        std::string libPath = ResolveDefaultLib(argv[0]);
        bool waitExit = false;
        std::vector<char*> gameArgs;
        gameArgs.push_back(const_cast<char*>(gameBin));

        int idx = 3;
        if (idx < argc && argv[idx][0] != '-' && std::strstr(argv[idx], ".so"))
            libPath = argv[idx++];

        for (; idx < argc; ++idx)
        {
            if (std::strcmp(argv[idx], "--wait") == 0) { waitExit = true; }
            else if (std::strcmp(argv[idx], "--") == 0)
            {
                for (int j = idx + 1; j < argc; ++j)
                    gameArgs.push_back(argv[j]);
                break;
            }
        }
        gameArgs.push_back(nullptr);

        char absLib[PATH_MAX] = {};
        if (realpath(libPath.c_str(), absLib)) libPath = absLib;

        std::string layerDir = ResolveLayerDir(libPath);

        pid_t pid = fork();
        if (pid < 0) { std::perror("[inject] fork"); return 1; }
        if (pid == 0)
        {
            const char* existingPreload = std::getenv("LD_PRELOAD");
            std::string preload = existingPreload && existingPreload[0]
                ? (libPath + ":" + existingPreload)
                : libPath;

            setenv("LD_PRELOAD", preload.c_str(), 1);
            setenv("VK_LAYER_PATH", layerDir.c_str(), 1);
            setenv("ENABLE_VKCAPTURE", "1", 1);

            const char* existingLayers = std::getenv("VK_INSTANCE_LAYERS");
            std::string layers = existingLayers && existingLayers[0]
                ? (std::string("VK_LAYER_VKCAPTURE_capture:") + existingLayers)
                : "VK_LAYER_VKCAPTURE_capture";
            setenv("VK_INSTANCE_LAYERS", layers.c_str(), 1);

            char gameCopy[PATH_MAX] = {};
            if (realpath(gameBin, gameCopy))
            {
                char* dir = dirname(gameCopy);
                if (chdir(dir) != 0)
                    std::perror("[inject] chdir");
            }

            execvp(gameBin, gameArgs.data());
            std::perror("[inject] execvp");
            _exit(127);
        }

        std::printf("[inject] Launched %s (pid=%d) with %s\n", gameBin, static_cast<int>(pid), libPath.c_str());
        if (waitExit)
        {
            int status = 0;
            waitpid(pid, &status, 0);
            int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            std::printf("[inject] Target exited with code %d\n", code);
            return code;
        }
        return 0;
    }
    else if (std::strcmp(argv[1], "--wait-for") == 0)
    {
        const char* procName = argv[2];
        int timeoutSec = 30;
        for (int i = 3; i < argc; ++i)
        {
            if (std::strcmp(argv[i], "--timeout") == 0 && i + 1 < argc)
                timeoutSec = std::atoi(argv[++i]);
        }

        std::printf("[inject] Waiting up to %ds for '%s' with libvulkan.so...\n", timeoutSec, procName);
        int foundPid = -1;
        for (int s = 0; s < timeoutSec * 10; ++s)
        {
            foundPid = FindProcessWithVulkan(procName);
            if (foundPid > 0) break;
            usleep(100000);
        }
        if (foundPid <= 0)
        {
            std::printf("[inject] Timed out waiting for %s\n", procName);
            return 1;
        }

        std::printf("[inject] Found %s (pid=%d) with Vulkan loaded.\n", procName, foundPid);
        std::printf("[inject] Note: On Linux, Vulkan layers must be enabled at vkCreateInstance time.\n");
        std::printf("[inject] Launch the game via: inject_tool --launch %s\n", procName);
        return 0;
    }

    PrintUsage();
    return 1;
}
