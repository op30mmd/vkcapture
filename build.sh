#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODE="${1:---64}"

build_64() {
    local BUILD_DIR="${ROOT_DIR}/build"
    cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release -DVKCAPTURE_BUILD_32BIT=OFF
    cmake --build "${BUILD_DIR}" -j"$(nproc)"
    echo ""
    echo "64-bit build succeeded. Outputs in ${ROOT_DIR}/bin/:"
    ls -lh "${ROOT_DIR}/bin/"

    local LAYER_DIR="${HOME}/.local/share/vulkan/implicit_layer.d"
    mkdir -p "${LAYER_DIR}"
    cat > "${LAYER_DIR}/vkcapture_layer.json" <<EOF
{
    "file_format_version": "1.1.2",
    "layer": {
        "name": "VK_LAYER_VKCAPTURE_capture",
        "type": "GLOBAL",
        "library_path": "${ROOT_DIR}/bin/libvkcapture.so",
        "api_version": "1.3.0",
        "implementation_version": "1",
        "description": "vkcapture Vulkan frame, audio, and ImGui overlay capture layer (64-bit)",
        "enable_environment": {
            "ENABLE_VKCAPTURE": "1"
        },
        "disable_environment": {
            "DISABLE_VKCAPTURE": "1"
        }
    }
}
EOF
    echo "Registered 64-bit implicit Vulkan layer at ${LAYER_DIR}/vkcapture_layer.json"

    cat > "${ROOT_DIR}/bin/start_daemon.sh" <<'SH'
#!/usr/bin/env bash
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
pkill -x shm_reader 2>/dev/null || true
nohup "${DIR}/shm_reader" --daemon </dev/null >/tmp/vkcapture_daemon.log 2>&1 &
SH
    chmod +x "${ROOT_DIR}/bin/start_daemon.sh"
    echo "Created Heroic Before-Launch script at ${ROOT_DIR}/bin/start_daemon.sh"
}

build_32() {
    local BUILD_DIR="${ROOT_DIR}/build32"
    cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release -DVKCAPTURE_BUILD_32BIT=ON
    cmake --build "${BUILD_DIR}" -j"$(nproc)"
    echo ""
    echo "32-bit build succeeded. Outputs in ${ROOT_DIR}/bin32/:"
    ls -lh "${ROOT_DIR}/bin32/"

    local LAYER_DIR="${HOME}/.local/share/vulkan/implicit_layer.d"
    mkdir -p "${LAYER_DIR}"
    cat > "${LAYER_DIR}/vkcapture_layer_32.json" <<EOF
{
    "file_format_version": "1.1.2",
    "layer": {
        "name": "VK_LAYER_VKCAPTURE_capture_32",
        "type": "GLOBAL",
        "library_path": "${ROOT_DIR}/bin32/libvkcapture.so",
        "api_version": "1.3.0",
        "implementation_version": "1",
        "description": "vkcapture Vulkan frame, audio, and ImGui overlay capture layer (32-bit)",
        "enable_environment": {
            "ENABLE_VKCAPTURE": "1"
        },
        "disable_environment": {
            "DISABLE_VKCAPTURE": "1"
        }
    }
}
EOF
    echo "Registered 32-bit implicit Vulkan layer at ${LAYER_DIR}/vkcapture_layer_32.json"
}

case "${MODE}" in
    --32)  build_32 ;;
    --all) build_64; build_32 ;;
    *)     build_64 ;;
esac
