#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

PKG_FILENAME="cp0-sync_1.0.0_m5stack1_arm64.deb"
OUT_DIR="${SCRIPT_DIR}/dist"
STAGE_ONLY_DIR=""
LOCAL_BUILD=0
UI_BIN=""

usage() {
    cat <<EOF
Usage: $(basename "$0") [options]
  --local                 Build device-ui and .deb directly on Linux ARM64 host
  --stage-only <dir>      Stage .deb directory tree into <dir> and exit
  --ui-bin <path>         Path to prebuilt cp0-sync-ui binary
  --out-dir <dir>         Output directory for .deb (default: packaging/dist)
  -h, --help              Show this help message
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --local)
            LOCAL_BUILD=1
            shift
            ;;
        --stage-only)
            [[ $# -ge 2 ]] || { echo "Missing argument for --stage-only" >&2; exit 2; }
            STAGE_ONLY_DIR="$2"
            shift 2
            ;;
        --ui-bin)
            [[ $# -ge 2 ]] || { echo "Missing argument for --ui-bin" >&2; exit 2; }
            UI_BIN="$2"
            shift 2
            ;;
        --out-dir)
            [[ $# -ge 2 ]] || { echo "Missing argument for --out-dir" >&2; exit 2; }
            OUT_DIR="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

stage_package_tree() {
    local stage_dir="$1"
    local ui_bin="$2"

    rm -rf "${stage_dir}"
    install -d "${stage_dir}/DEBIAN"
    install -d "${stage_dir}/usr/bin"
    install -d "${stage_dir}/usr/lib/cp0-sync/cp0_syncd"
    install -d "${stage_dir}/usr/lib/systemd/user"
    install -d "${stage_dir}/usr/share/APPLaunch/applications"
    install -d "${stage_dir}/usr/share/APPLaunch/share/images"

    install -m 0644 "${SCRIPT_DIR}/debian/control" "${stage_dir}/DEBIAN/control"
    install -m 0755 "${SCRIPT_DIR}/debian/postinst" "${stage_dir}/DEBIAN/postinst"
    install -m 0755 "${SCRIPT_DIR}/debian/prerm" "${stage_dir}/DEBIAN/prerm"

    install -m 0755 "${ui_bin}" "${stage_dir}/usr/bin/cp0-sync-ui"
    install -m 0755 "${ROOT_DIR}/device-daemon/bin/cp0-syncd" "${stage_dir}/usr/bin/cp0-syncd"

    while IFS= read -r -d '' py_file; do
        local rel_path="${py_file#${ROOT_DIR}/device-daemon/cp0_syncd/}"
        install -d "${stage_dir}/usr/lib/cp0-sync/cp0_syncd/$(dirname "${rel_path}")"
        install -m 0644 "${py_file}" "${stage_dir}/usr/lib/cp0-sync/cp0_syncd/${rel_path}"
    done < <(find "${ROOT_DIR}/device-daemon/cp0_syncd" -type f -name '*.py' -print0)

    install -m 0644 "${SCRIPT_DIR}/cp0-syncd.service" "${stage_dir}/usr/lib/systemd/user/cp0-syncd.service"
    install -m 0644 "${SCRIPT_DIR}/cp0-sync.desktop" "${stage_dir}/usr/share/APPLaunch/applications/cp0-sync.desktop"
    install -m 0644 "${SCRIPT_DIR}/cp0-sync.png" "${stage_dir}/usr/share/APPLaunch/share/images/cp0-sync.png"

    for font_dir in "${ROOT_DIR}/device-ui/assets/fonts" "${SCRIPT_DIR}/fonts"; do
        if [[ -d "${font_dir}" ]]; then
            while IFS= read -r -d '' font_file; do
                install -d "${stage_dir}/usr/share/cp0-sync/fonts"
                install -m 0644 "${font_file}" "${stage_dir}/usr/share/cp0-sync/fonts/$(basename "${font_file}")"
            done < <(find "${font_dir}" -maxdepth 1 -type f \( -name '*.ttf' -o -name '*.otf' \) -print0)
        fi
    done
}

resolve_ui_bin() {
    if [[ -n "${UI_BIN}" ]]; then
        [[ -f "${UI_BIN}" ]] || { echo "Specified --ui-bin not found: ${UI_BIN}" >&2; exit 1; }
        echo "${UI_BIN}"
        return
    fi
    for candidate in "${ROOT_DIR}/device-ui/build-arm64/cp0-sync-ui" "${ROOT_DIR}/device-ui/build/cp0-sync-ui"; do
        if [[ -f "${candidate}" ]]; then
            echo "${candidate}"
            return
        fi
    done
    echo ""
}

if [[ -n "${STAGE_ONLY_DIR}" ]]; then
    RESOLVED_UI="$(resolve_ui_bin)"
    [[ -n "${RESOLVED_UI}" ]] || { echo "cp0-sync-ui binary not found; pass --ui-bin <path>" >&2; exit 1; }
    stage_package_tree "${STAGE_ONLY_DIR}" "${RESOLVED_UI}"
    echo "Staged package tree at ${STAGE_ONLY_DIR}"
    exit 0
fi

if [[ "${LOCAL_BUILD}" -eq 1 ]]; then
    if [[ -z "${UI_BIN}" ]]; then
        BUILD_DIR="${ROOT_DIR}/device-ui/build-arm64"
        cmake -S "${ROOT_DIR}/device-ui" -B "${BUILD_DIR}" \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_CXX_FLAGS_RELEASE="-O2" \
            -DCP0_UI_BUILD_APP=ON \
            -DCP0_UI_TARGET_DEVICE=ON \
            -DCP0_UI_BUILD_TESTS=ON
        cmake --build "${BUILD_DIR}" -j"$(nproc 2>/dev/null || echo 2)"
        ctest --test-dir "${BUILD_DIR}" --output-on-failure
        UI_BIN="${BUILD_DIR}/cp0-sync-ui"
    fi

    TMP_STAGE="$(mktemp -d)"
    trap 'rm -rf "${TMP_STAGE}"' EXIT
    stage_package_tree "${TMP_STAGE}/pkg" "${UI_BIN}"

    mkdir -p "${OUT_DIR}"
    dpkg-deb --build --root-owner-group "${TMP_STAGE}/pkg" "${OUT_DIR}/${PKG_FILENAME}"
    echo "Built ${OUT_DIR}/${PKG_FILENAME}"
    exit 0
fi

# Default: build inside linux/arm64 Debian Bookworm Docker container
IMAGE_TAG="cp0-sync-deb-builder:arm64"
docker build --platform linux/arm64 -f "${SCRIPT_DIR}/Dockerfile.arm64" -t "${IMAGE_TAG}" "${ROOT_DIR}"

mkdir -p "${OUT_DIR}"
CONTAINER_ID="$(docker create --platform linux/arm64 "${IMAGE_TAG}")"
trap 'docker rm -f "${CONTAINER_ID}" >/dev/null 2>&1 || true' EXIT
docker cp "${CONTAINER_ID}:/app/packaging/dist/${PKG_FILENAME}" "${OUT_DIR}/${PKG_FILENAME}"
echo "Built ${OUT_DIR}/${PKG_FILENAME}"
