# vkcapture — Vulkan Frame & Audio Capture for Linux (64-bit & 32-bit)

Linux (Vulkan) port of [`d3d9capture`](https://github.com/op30mmd/d3d9capture). Captures frames directly inside `vkQueuePresentKHR` via quad-buffered asynchronous `VkBuffer` readback, renders an in-game Dear ImGui control center onto the swapchain, captures audio via PulseAudio/PipeWire/ALSA, and encodes H.264/AAC MP4s either in-process (`ffmpeg` pipe) or through a zero-stutter 4-slot POSIX shared-memory host daemon (`shm_reader --daemon`) compatible with Proton / Wine / DXVK (`steamrt4` seccomp-safe) and native Linux Vulkan titles on X11, XWayland, and Wayland.

---

## Contents

1. [Architecture Mapping (Windows → Linux)](#architecture-mapping-windows--linux)
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

---

## Architecture Mapping (Windows → Linux)

| Component | Windows (`d3d9capture`) | Linux (`vkcapture`) |
|---|---|---|
| Hooking / Injection | `CreateRemoteThread` + IAT/`GetProcAddress`/vtable scan | Vulkan Implicit Layer (`VK_LAYER_VKCAPTURE_capture`) + `LD_PRELOAD` (`libvkcapture.so`) |
| GPU Readback | `GetRenderTargetData` / `CopyResource` | Quad-buffered `vkCmdCopyImageToBuffer` (`HOST_CACHED` GTT) + `VkFence` + semaphore sync |
| In-Game Overlay | ImGui DX9 / DX11 + `WndProc` hook | `imgui_impl_vulkan` (`IMGUI_IMPL_VULKAN_NO_PROTOTYPES`) + X11/XWayland + Wayland input hooks |
| Video/Audio Encoder | Media Foundation `IMFSinkWriter` | `shm_reader --daemon` on host (`libavcodec` `libx264` + AAC via `@DEFAULT_MONITOR@`) or `ffmpeg` pipe |
| Audio Capture | WASAPI `IAudioRenderClient` + loopback | PulseAudio/PipeWire/ALSA `LD_PRELOAD` + `@DEFAULT_MONITOR@` loopback |
| Shared Memory IPC | `CreateFileMappingA` + `CreateEventA` | 4-slot POSIX `shm_open("/vkcapture_shm_v2")` + `sem_open` |
| Data Directory | `C:\d3d9capture\` | `$XDG_DATA_HOME/vkcapture` (`~/.local/share/vkcapture`) |

---

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

---

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

---

## Usage

### Heroic Games Launcher / Proton / DXVK

Proton runs inside the `steamrt4` (`pressure-vessel`) container and installs a `seccomp-bpf` filter on Wine processes (which kills child `ffmpeg` processes with `SIGSYS` / signal 31). Running `shm_reader --daemon` on the host via Heroic's **Before Launch Script** avoids `seccomp` completely while keeping **F9** / **Insert** working in-game.

In Heroic → **Game Settings → Advanced**:

1. **Before Launch Script**:
   `/path/to/linux_port/bin/start_daemon.sh`
2. **Environment Variables**:

   | Variable | Value |
   |---|---|
   | `ENABLE_VKCAPTURE` | `1` |
   | `LD_PRELOAD` | `/path/to/linux_port/bin/libvkcapture.so` *(or `bin32/libvkcapture.so` for 32-bit Wine)* |

> **Wine Wayland note**: If you enable **Wine Wayland** (`enableWineWayland: true`) in Heroic with Proton inside `umu`, also add `PROTON_ENABLE_WAYLAND=1`. On XWayland (`enableWineWayland: false`), `XIGrabDevice` + `XQueryKeymap`/`XQueryPointer` handle input automatically.

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
./bin/shm_reader --record out.mp4 --fps 30 --bitrate 16000
./bin/shm_reader --save 6 ./frames --seconds 10
```

### In-game Overlay & Hotkeys

- **Insert** (configurable: `Insert`, `Home`, `End`, `F11`, `` ` ``): Toggle the ImGui Control Center.
- **F9** (configurable: `F9`–`F12`, `F8`, `Scroll Lock`, `Pause`): Start / stop recording.

| Tab | Contents |
|---|---|
| **Dashboard** | Live Present/Capture FPS, readback latency, audio peak plots, quick record & BMP screenshot |
| **Video Recording** | Prefix, FPS (24–120), bitrate (4–50 Mbps), AAC bitrate, auto-stop timer, hotkeys, capture limiter |
| **Audio & Sound** | Capture mode (Auto / Direct Hook / Monitor Loopback), software gain (0–300%), peak meter, 440 Hz test tone |
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

---

## How It Works

- **Layer + `LD_PRELOAD` separation**: `vkcapture_GetInstanceProcAddr` / `vkcapture_GetDeviceProcAddr` are `static` and returned via `vkNegotiateLoaderLayerInterfaceVersion`, so `LD_PRELOAD` never shadows `libvulkan.so.1`'s loader trampolines. `Overlay_Init` and `ImGui_ImplVulkan_LoadFunctions` (`IMGUI_IMPL_VULKAN_NO_PROTOTYPES`) route `VkPhysicalDevice` queries through the next-layer chain.
- **Zero-stutter GPU readback (`capture.cpp`)**: `Hook_vkCreateSwapchainKHR` adds `VK_IMAGE_USAGE_TRANSFER_SRC_BIT` and `VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT`. `Capture_OnPresent` uses 4 staging `VkBuffer`s in `HOST_VISIBLE | HOST_CACHED` memory (fast GTT RAM on AMD RADV rather than uncached WC), chains a `VkSemaphore` into `vkQueuePresentKHR` so presentation waits for `vkCmdCopyImageToBuffer`, and reaps completed slots non-blockingly via `vkGetFenceStatus`.
- **4-slot POSIX SHM ring (`consumer_backend.cpp` ↔ `shm_reader.cpp`)**: `sem_done` is initialized to 4 slots so the producer never drops frames waiting for the consumer's `memcpy`. `shm_reader --daemon` copies slots into a 64-frame async `libx264` (`ultrafast`, `crf 23`, `SWS_FAST_BILINEAR`) queue and captures `@DEFAULT_MONITOR@` audio via `libpulse-simple` (`fragsize = 4096`, flushed on the first video frame) muxed to AAC.
- **Input isolation (`layer_main.cpp`, `overlay.cpp`)**:
  - **X11 / XWayland**: Polls `XQueryKeymap` / `XQueryPointer` and grabs pointer/keyboard (`XGrabPointer`, `XGrabKeyboard`, and XInput2 `XIGrabDevice` via `libXi.so.6`) while the menu is open.
  - **Wayland**: Hooks `wl_proxy_add_listener` (resolved from `libwayland-client.so.0` with `RTLD_LAZY | RTLD_NOLOAD` to bypass `RTLD_LOCAL`) for `wl_keyboard`, `wl_pointer`, and `zwp_relative_pointer_v1` (so locked-pointer games keep a visible, movable software cursor).

---

## Verifying a Change

```bash
cd bin
./inject_tool --launch ./vk_testapp ./libvkcapture.so --wait -- 300 640 480

# Decode frames from a recorded MP4 back to BMP to verify orientation & colors:
./mp4_frame ~/.local/share/vkcapture/recordings/<file>.mp4 frame_ 3 5
```

---

## Troubleshooting & Known Notes

- **32-bit `libvkcapture.so` in 64-bit games (or vice versa)**: The Vulkan loader only loads the `.so` matching the target process bitness; run `./build.sh --all` so both `bin/` (64-bit) and `bin32/` (32-bit) are built and registered.
- **No `vkcapture` entries in `debug.log` under Heroic/Proton**: Verify `~/.local/share/vulkan/implicit_layer.d/vkcapture_layer.json` points to your current `bin/libvkcapture.so` and `ENABLE_VKCAPTURE=1` is set (do not set `VK_LAYER_PATH` / `VK_INSTANCE_LAYERS` alongside the implicit layer).
- **Audio missing in `shm_reader --daemon`**: Check `/tmp/vkcapture_daemon.log` and verify PulseAudio / `pipewire-pulse` has an active `@DEFAULT_MONITOR@` source (`pactl info`).

---

## Project Layout

| File | Purpose |
|---|---|
| `src/platform.h` | Monotonic nanosecond clock, XDG data paths, directory & string helpers |
| `src/layer_main.cpp` | Vulkan layer entrypoints, swapchain/present hooks, X11 & Wayland input interposition |
| `src/capture.h/cpp` | Quad-buffered `VkBuffer` GPU readback, `HOST_CACHED` GTT allocation, semaphore sync, heartbeat |
| `src/consumer_backend.cpp` | 4-slot POSIX SHM ring publisher, daemon IPC control, BMP screenshots & dumps |
| `src/shm_reader.cpp` | Host consumer & `--daemon` (async `libx264` + PulseAudio/PipeWire AAC muxer) |
| `src/recorder.h/cpp` | In-process `ffmpeg` pipe encoder (used when host daemon is not running) |
| `src/audio.h/cpp` | `LD_PRELOAD` PulseAudio/ALSA hooks, `@DEFAULT_MONITOR@` loopback, BS.775 downmix, 30 Hz HPF |
| `src/overlay.h/cpp` | Dear ImGui Vulkan overlay, 7-tab control center, themes, Mini-HUD, X11/XInput2 fallback |
| `src/inject_tool.cpp` | `--launch` / `--wait-for` launcher |
| `tools/vk_testapp.cpp` | Minimal Vulkan + X11 verification app (overlay, recording, stop+restart, swapchain resize) |
| `tools/mp4_frame.cpp` | FFmpeg MP4-to-BMP frame decoder for visual verification |
| `CMakeLists.txt`, `build.sh` | 64-bit & 32-bit build system, `FetchContent` ImGui, implicit layer registration |
