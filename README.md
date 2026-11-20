# vkcapture: Vulkan Frame & Audio Capture for Linux (64-bit & 32-bit)

Linux (Vulkan) port of [`d3d9capture`](https://github.com/op30mmd/d3d9capture). Captures frames directly inside `vkQueuePresentKHR` via quad-buffered asynchronous `VkBuffer` readback, renders an in-game Dear ImGui control center onto the swapchain, isolates game-only audio via a temporary PulseAudio/PipeWire virtual sink, and encodes H.264/AAC MP4s through a lock-free 4-slot POSIX shared-memory host daemon (`shm_reader --daemon`) or in-process `ffmpeg` pipe. Compatible with Proton / Wine / DXVK (`steamrt4` seccomp-safe, 32-bit and 64-bit) and native Linux Vulkan titles on X11, XWayland, and Wayland.

## Contents

1. [Architecture Mapping (Windows to Linux)](#architecture-mapping-windows-to-linux)
2. [Prerequisites](#prerequisites)
3. [Building (64-bit & 32-bit)](#building-64-bit--32-bit)
4. [Usage](#usage)
   - [Heroic Games Launcher / Proton / DXVK](#heroic-games-launcher--proton--dxvk)
   - [Native Linux Vulkan Apps (`inject_tool`)](#native-linux-vulkan-apps-inject_tool)
   - [Out-of-process Consumer & Daemon (`shm_reader`)](#out-of-process-consumer--daemon-shm_reader)
   - [In-game Overlay & Hotkeys](#in-game-overlay--hotkeys)
   - [Where Things End Up](#where-things-end-up)
5. [How It Works](#how-it-works)
6. [Verifying a Change](#verifying-a-change)
7. [Troubleshooting & Known Notes](#troubleshooting--known-notes)
8. [Project Layout](#project-layout)

## Architecture Mapping (Windows to Linux)

| Component | Windows (`d3d9capture`) | Linux (`vkcapture`) |
|---|---|---|
| Hooking / Injection | `CreateRemoteThread` + IAT/`GetProcAddress`/vtable scan | Vulkan Implicit Layer (`VK_LAYER_VKCAPTURE_capture`) + `LD_PRELOAD` (`libvkcapture.so`) |
| GPU Readback | `GetRenderTargetData` / `CopyResource` | Quad-buffered `vkCmdCopyImageToBuffer` (`HOST_CACHED` GTT) + `VkFence` + semaphore sync |
| In-Game Overlay | ImGui DX9 / DX11 + `WndProc` hook | `imgui_impl_vulkan` (`IMGUI_IMPL_VULKAN_NO_PROTOTYPES`) + X11/XCB/XWayland + Wayland input hooks |
| Video/Audio Encoder | Media Foundation `IMFSinkWriter` | `shm_reader --daemon` on host (`libavcodec` `libx264` `superfast` CRF 26 + VBV cap + 128 kbps AAC) or `ffmpeg` pipe |
| Audio Capture | WASAPI `IAudioRenderClient` + loopback | Host virtual-sink game-only isolation (`vkcapture_game_sink` + 10 ms loopback) with `@DEFAULT_MONITOR@` fallback |
| Shared Memory IPC | `CreateFileMappingA` + `CreateEventA` | Lock-free 4-slot POSIX `shm_open("/vkcapture_shm_v3")` (atomic `writeIdx`/`readIdx`, 32-bit/64-bit safe) |
| Data Directory | `C:\d3d9capture\` | `$XDG_DATA_HOME/vkcapture` (`~/.local/share/vkcapture`) |

## Prerequisites

### 64-bit (Ubuntu / Debian)

```bash
sudo apt update && sudo apt install -y \
  build-essential cmake pkg-config git \
  libvulkan-dev libx11-dev \
  libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev \
  libpulse-dev libasound2-dev
```

### 32-bit (`i386`, for 32-bit native Vulkan or 32-bit Wine/DXVK prefixes)

```bash
sudo dpkg --add-architecture i386
sudo apt update && sudo apt install -y \
  g++-multilib libvulkan-dev:i386 libx11-dev:i386
```

*(32-bit builds automatically compile with `VKCAPTURE_NO_LOCAL_AUDIO` if `libpulse-dev:i386` / `libasound2-dev:i386` are absent; audio is still recorded on the 64-bit host by `shm_reader --daemon`.)*

Dear ImGui (`v1.91.8`) is automatically downloaded via CMake `FetchContent` into `build/_deps/imgui-src` if `src/imgui/` is empty.

## Building (64-bit & 32-bit)

```bash
chmod +x build.sh

# 64-bit (builds bin/libvkcapture.so, inject_tool, shm_reader, vk_testapp, mp4_frame)
./build.sh

# 32-bit only (builds bin32/libvkcapture.so)
./build.sh --32

# Both 64-bit and 32-bit
./build.sh --all
```

`build.sh` also:
- Registers `~/.local/share/vulkan/implicit_layer.d/vkcapture_layer.json` (64-bit) and `vkcapture_layer_32.json` (32-bit).
- Generates `bin/start_daemon.sh` for Heroic / Lutris / Steam launch integration.

## Usage

### Heroic Games Launcher / Proton / DXVK

Proton runs inside the `steamrt4` (`pressure-vessel`) container and installs a `seccomp-bpf` filter on Wine processes (which kills child `ffmpeg` processes with `SIGSYS` / signal 31). Running `shm_reader --daemon` on the host via Heroic's **Before Launch Script** avoids `seccomp` completely while keeping **F9** / **Insert**, Pause/Resume, game-only audio, and the live dB meter working in-game.

In Heroic -> **Game Settings -> Advanced**:

1. **Before Launch Script**:
   `/path/to/linux_port/bin/start_daemon.sh`
2. **Environment Variables**:

   | Variable | Value |
   |---|---|
   | `ENABLE_VKCAPTURE` | `1` *(set to `0` or set `DISABLE_VKCAPTURE=1` to bypass all Vulkan, input, and audio hooks)* |
   | `LD_PRELOAD` | `/path/to/linux_port/bin/libvkcapture.so` *(or `bin32/libvkcapture.so` for 32-bit Wine)* |

> **Wine Wayland note**: If you enable **Wine Wayland** (`enableWineWayland: true`) in Heroic with Proton inside `umu`, also add `PROTON_ENABLE_WAYLAND=1`. On X11/XWayland (`enableWineWayland: false`), X11/XCB event filtering, cross-process `menuOpen` SHM sharing, and `XGrabPointer` + `XQueryKeymap`/`XQueryPointer` handle input automatically.

### Native Linux Vulkan Apps (`inject_tool`)

```bash
# 64-bit target
./bin/inject_tool --launch /path/to/game ./bin/libvkcapture.so --wait -- [game args...]

# 32-bit target
./bin/inject_tool --launch /path/to/game32 ./bin32/libvkcapture.so --wait -- [game args...]
```

### Out-of-process Consumer & Daemon (`shm_reader`)

```bash
# Host daemon (started automatically by bin/start_daemon.sh; driven by F9 / overlay)
./bin/shm_reader --daemon

# Standalone CLI capture
./bin/shm_reader --record out.mp4 --fps 30 --bitrate 8000
./bin/shm_reader --save 6 ./frames --seconds 10
```

### In-game Overlay & Hotkeys

- **Insert** (configurable: `Insert`, `Home`, `End`, `F11`, `` ` ``): Toggle the ImGui Control Center (250 ms debounce).
- **F9** (configurable: `F9`-`F12`, `F8`, `Scroll Lock`, `Pause`): Start / stop recording (350 ms debounce).

| Tab | Contents |
|---|---|
| **Dashboard** | Live Present/Capture FPS, readback latency, live audio peak plots, quick record, Pause/Resume, and BMP screenshot |
| **Video Recording** | Prefix, FPS (24-120), VBV max bitrate (4-50 Mbps, default 8 Mbps), auto-stop timer, hotkeys, capture limiter |
| **Audio & Sound** | Enable Audio Capture toggle (forwarded to daemon), capture mode, software gain (0-300%), live peak dBFS meter (works idle and while recording), 440 Hz test tone |
| **Live Logs** | In-memory log viewer with tag filter (`[rec]`, `[audio]`, `[capvk]`, `[overlay]`) and search |
| **Appearance & HUD** | 6 themes (Emerald, Cyberpunk, Midnight Purple, Dark, Light, Classic), Mini-HUD, recording beacon |
| **System & GPU** | Vulkan version, GPU adapter name, VRAM, process RSS, SHM status |
| **Info & Help** | Architecture & hotkey reference |

### Where Things End Up

| Path | Contents |
|---|---|
| `~/.local/share/vkcapture/debug.log` | Layer, capture, overlay, and recorder log (override base dir with `VKCAPTURE_DIR`) |
| `~/.local/share/vkcapture/recordings/` | Recorded `.mp4` files |
| `~/.local/share/vkcapture/screenshots/` | Instant `.bmp` screenshots |
| `/tmp/vkcapture_daemon.log` | Host `shm_reader --daemon` log |

## How It Works

- **Layer + `LD_PRELOAD` separation (`layer_main.cpp`)**: `vkcapture_GetInstanceProcAddr` and `vkcapture_GetDeviceProcAddr` are `static` and returned via `vkNegotiateLoaderLayerInterfaceVersion`, so `LD_PRELOAD` never shadows `libvulkan.so.1`'s loader trampolines. Multi-instance/multi-device Vulkan apps are tracked via `g_PhysDevToInstance` and `g_SwapchainToDevice`, and `Overlay_Init` resolves all device Vulkan functions through `g_OvVk` (`IMGUI_IMPL_VULKAN_NO_PROTOTYPES`). When `ENABLE_VKCAPTURE=0` or `DISABLE_VKCAPTURE=1` is set, all Vulkan, X11, Wayland, and audio hooks immediately pass through.
- **Zero-stutter GPU readback & lock-free idle path (`capture.cpp`, `overlay.cpp`)**: `Hook_vkCreateSwapchainKHR` adds `VK_IMAGE_USAGE_TRANSFER_SRC_BIT` and `VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT`. When idle (not recording and menu/HUD hidden), both `Overlay_OnPresent` and `Capture_OnPresent` return immediately without locking, allocating staging buffers, or flushing heartbeat logs. While recording, `Capture_OnPresent` uses 4 staging `VkBuffer`s in `HOST_VISIBLE | HOST_CACHED` memory, stamps `timestampNs` at `vkQueuePresentKHR` submit time, chains a `VkSemaphore` into present, reaps completed slots non-blockingly via `vkGetFenceStatus`, and delivers frames on a background worker thread.
- **Lock-free 4-slot POSIX SHM ring (`consumer_backend.cpp` <-> `shm_reader.cpp`)**: Uses atomic `uint32_t` `writeIdx`/`readIdx` counters in `/vkcapture_shm_v3` (avoiding cross-architecture 32-bit/64-bit POSIX semaphore layout mismatches) with per-session `recordSessionId` tagging and Pause/Resume support (`recordRequested == 2`). `shm_reader --daemon` runs at `nice(10)` with a 16-frame async pool feeding `libx264` (`superfast`, `crf=26`, VBV `maxrate`/`bufsize`, `rc-lookahead=10`, `threads = hw/2`) and `128 kbps` AAC.
- **Game-only virtual-sink audio isolation (`shm_reader.cpp`)**: On recording start, `shm_reader --daemon` loads a temporary `module-null-sink` (`vkcapture_game_sink`) and a 10 ms `module-loopback` to `@DEFAULT_SINK@`, moves all matching game sink-inputs (matched by PID, `/proc/<pid>/cmdline`, or Wine/Proton name, while excluding browsers/Discord/media players) into `vkcapture_game_sink`, subscribes to `PA_SUBSCRIPTION_MASK_SINK_INPUT` to auto-route newly created game streams, and records `vkcapture_game_sink.monitor` exclusively. On stop, streams are moved back to their original sink and both temporary modules are unloaded. While idle with the menu or Mini-HUD audio meter open, a lightweight `meterThread` updates `daemonAudioPeakMilliDb`.
- **Input isolation (`layer_main.cpp`, `overlay.cpp`)**:
  - **X11 / XWayland**: Filters `XNextEvent`, `XCheckIfEvent`, `XCheckMaskEvent`, `XCheckWindowEvent`, `XCheckTypedEvent`, `XCheckTypedWindowEvent`, `XIfEvent`, `XWindowEvent`, `XMaskEvent`, `XPeekEvent`, `XGetEventData`, `xcb_poll_for_event`, `xcb_poll_for_queued_event`, and `xcb_wait_for_event`; freezes `XQueryPointer`/`XIQueryPointer`; releases the game's core + XI2 grabs before grabbing on the overlay's display; and shares `menuOpen` via `/vkcapture_shm_v3` so separate Wine helper processes (such as `explorer.exe`) also drop `XI_RawMotion`.
  - **Wayland**: Hooks `wl_proxy_add_listener` (resolved from `libwayland-client.so.0` with `RTLD_LAZY | RTLD_NOLOAD`) for `wl_keyboard`, `wl_pointer`, and `zwp_relative_pointer_v1`, queueing all ImGui input updates onto the Vulkan present thread.

## Verifying a Change

```bash
cd bin
./inject_tool --launch ./vk_testapp ./libvkcapture.so --wait -- 300 640 480

# Decode frames from a recorded MP4 back to BMP to verify orientation & colors:
./mp4_frame ~/.local/share/vkcapture/recordings/<file>.mp4 frame_ 3 5
```

## Troubleshooting & Known Notes

- **32-bit `libvkcapture.so` in 64-bit games (or vice versa)**: The Vulkan loader only loads the `.so` matching the target process bitness; run `./build.sh --all` so both `bin/` (64-bit) and `bin32/` (32-bit) are built and registered.
- **No `vkcapture` entries in `debug.log` under Heroic/Proton**: Verify `~/.local/share/vulkan/implicit_layer.d/vkcapture_layer.json` points to your current `bin/libvkcapture.so` and `ENABLE_VKCAPTURE=1` is set (do not set `VK_LAYER_PATH` / `VK_INSTANCE_LAYERS` alongside the implicit layer).
- **Disabling `vkcapture` temporarily**: Set `ENABLE_VKCAPTURE=0` or `DISABLE_VKCAPTURE=1` in Heroic; you do not need to remove `LD_PRELOAD`.
- **Verify game-only audio isolation**: Run `grep "Audio source" /tmp/vkcapture_daemon.log` after starting a recording. You should see `game-only virtual sink` (or `@DEFAULT_MONITOR@ fallback` if no matching sink-input was active yet).

## Project Layout

| File | Purpose |
|---|---|
| `src/platform.h` | Monotonic nanosecond clock, XDG data paths, directory & string helpers |
| `src/layer_main.cpp` | Vulkan layer entrypoints, swapchain/present hooks, X11/XCB & Wayland input interposition |
| `src/capture.h/cpp` | Quad-buffered `VkBuffer` GPU readback, `HOST_CACHED` GTT allocation, semaphore sync, idle fast-path |
| `src/consumer_backend.cpp` | Lock-free 4-slot POSIX SHM ring publisher, daemon IPC control, shared `menuOpen`/meter state, BMP screenshots |
| `src/shm_reader.cpp` | Host consumer & `--daemon` (async `libx264`, virtual-sink game-only audio isolation, AAC muxer, idle dB meter) |
| `src/recorder.h/cpp` | In-process `ffmpeg` pipe encoder (used when host daemon is not running) |
| `src/audio.h/cpp` | `LD_PRELOAD` PulseAudio/ALSA hooks, `@DEFAULT_MONITOR@` loopback, BS.775 downmix, daemon dB stats bridge |
| `src/overlay.h/cpp` | Dear ImGui Vulkan overlay, 7-tab control center, themes, Mini-HUD, thread-safe input queue, X11/Wayland fallback |
| `src/inject_tool.cpp` | `--launch` / `--wait-for` launcher |
| `tools/vk_testapp.cpp` | Minimal Vulkan + X11 verification app (overlay, recording, stop+restart, swapchain resize) |
| `tools/mp4_frame.cpp` | FFmpeg MP4-to-BMP frame decoder for visual verification |
| `CMakeLists.txt`, `build.sh` | 64-bit & 32-bit build system, `FetchContent` ImGui, implicit layer registration |
