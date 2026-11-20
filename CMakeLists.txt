cmake_minimum_required(VERSION 3.16)
project(vkcapture LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(VKCAPTURE_BUILD_32BIT "Build 32-bit (i386) libvkcapture.so into bin32/" OFF)

find_package(Threads REQUIRED)
find_package(Vulkan REQUIRED)
find_package(X11 REQUIRED)
find_package(PkgConfig REQUIRED)

if(VKCAPTURE_BUILD_32BIT)
    set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} -m32")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -m32")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -m32")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin32")
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin32")

    find_library(PULSE_32_LIB        NAMES libpulse.so.0        pulse        PATHS /usr/lib/i386-linux-gnu /usr/lib32 NO_DEFAULT_PATH)
    find_library(PULSE_SIMPLE_32_LIB NAMES libpulse-simple.so.0 pulse-simple PATHS /usr/lib/i386-linux-gnu /usr/lib32 NO_DEFAULT_PATH)
    find_library(ALSA_32_LIB         NAMES libasound.so.2       asound       PATHS /usr/lib/i386-linux-gnu /usr/lib32 NO_DEFAULT_PATH)
else()
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin")

    pkg_check_modules(PULSE REQUIRED IMPORTED_TARGET libpulse libpulse-simple)
    pkg_check_modules(ALSA  REQUIRED IMPORTED_TARGET alsa)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
        libavcodec
        libavformat
        libavutil
        libswscale
        libswresample
    )
endif()

set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/imgui")
if(NOT EXISTS "${IMGUI_DIR}/imgui.cpp")
    include(FetchContent)
    message(STATUS "Dear ImGui not found in src/imgui; fetching automatically...")
    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG        v1.91.8
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(imgui)
    set(IMGUI_DIR "${imgui_SOURCE_DIR}")
endif()

# ── 1. libvkcapture.so (Vulkan Layer + LD_PRELOAD) ──────────────────────────
add_library(vkcapture SHARED
    src/layer_main.cpp
    src/capture.cpp
    src/consumer_backend.cpp
    src/recorder.cpp
    src/audio.cpp
    src/overlay.cpp
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/imgui_demo.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp"
)

target_include_directories(vkcapture PRIVATE
    src
    "${IMGUI_DIR}"
    "${IMGUI_DIR}/backends"
)

target_compile_definitions(vkcapture PRIVATE IMGUI_IMPL_VULKAN_NO_PROTOTYPES)
target_compile_options(vkcapture PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)

if(VKCAPTURE_BUILD_32BIT)
    # Link 32-bit runtime .so libraries directly so CMake never picks x86_64 .so paths
    target_link_directories(vkcapture PRIVATE /usr/lib/i386-linux-gnu /usr/lib32)
    if(PULSE_32_LIB AND PULSE_SIMPLE_32_LIB AND ALSA_32_LIB)
        message(STATUS "32-bit PulseAudio & ALSA found: ${PULSE_SIMPLE_32_LIB}, ${ALSA_32_LIB}")
        target_link_libraries(vkcapture PRIVATE
            :libvulkan.so.1
            :libX11.so.6
            Threads::Threads
            dl
            "${PULSE_32_LIB}"
            "${PULSE_SIMPLE_32_LIB}"
            "${ALSA_32_LIB}"
        )
    else()
        message(STATUS "32-bit Pulse/ALSA libraries not found; building with VKCAPTURE_NO_LOCAL_AUDIO (host daemon still records audio)")
        target_compile_definitions(vkcapture PRIVATE VKCAPTURE_NO_LOCAL_AUDIO)
        target_link_libraries(vkcapture PRIVATE :libvulkan.so.1 :libX11.so.6 Threads::Threads dl)
    endif()
    configure_file(src/vkcapture_layer.json "${CMAKE_CURRENT_SOURCE_DIR}/bin32/vkcapture_layer.json" COPYONLY)
else()
    target_link_libraries(vkcapture PRIVATE
        Vulkan::Vulkan
        X11::X11
        Threads::Threads
        dl
        PkgConfig::PULSE
        PkgConfig::ALSA
    )
    configure_file(src/vkcapture_layer.json "${CMAKE_CURRENT_SOURCE_DIR}/bin/vkcapture_layer.json" COPYONLY)

    # ── 2. Host Tools (64-bit) ───────────────────────────────────────────────
    add_executable(inject_tool src/inject_tool.cpp)
    target_compile_options(inject_tool PRIVATE -Wall -Wextra -O2)

    add_executable(shm_reader src/shm_reader.cpp)
    target_compile_options(shm_reader PRIVATE -Wall -Wextra -O2)
    target_link_libraries(shm_reader PRIVATE
        Threads::Threads
        rt
        PkgConfig::FFMPEG
        PkgConfig::PULSE
    )

    add_executable(vk_testapp tools/vk_testapp.cpp)
    target_compile_options(vk_testapp PRIVATE -Wall -Wextra -O2)
    target_link_libraries(vk_testapp PRIVATE Vulkan::Vulkan X11::X11)

    add_executable(mp4_frame tools/mp4_frame.cpp)
    target_compile_options(mp4_frame PRIVATE -Wall -Wextra -O2)
    target_link_libraries(mp4_frame PRIVATE PkgConfig::FFMPEG)
endif()
