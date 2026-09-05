#!/usr/bin/env bash
# =============================================================================
# SteamFlipper - Linux Build Script
#
# Builds the hook module for both architectures:
#   build/64/SteamFlipper.so  - game-side / Proton hooking (x86_64)
#   build/32/SteamFlipper.so  - the Steam client core itself, which is still a
#                                32-bit binary (ubuntu12_32/steam) on Linux
#
# Usage: ./tools/build_linux.sh [Release|Debug] [both|64|32]
# =============================================================================
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

BUILD_TYPE="${1:-Release}"
ARCHES="${2:-both}"
JOBS="$(nproc 2>/dev/null || echo 4)"

build_arch() {
    local arch="$1"
    local build_dir="${REPO_ROOT}/build/${arch}"
    local -a extra_args=()

    if [ "${arch}" = "32" ]; then
        extra_args+=("-DCMAKE_TOOLCHAIN_FILE=${REPO_ROOT}/src/cmake/Linux32.cmake")
    fi

    echo "=========================================="
    echo "Building SteamFlipper ${arch}-bit (${BUILD_TYPE})"
    echo "=========================================="

    # Ninja, not the default Makefiles: the FetchContent deps are cached in a
    # shared .deps tree, and a subbuild populated by one generator cannot be
    # reused by another ("does not match the generator used previously").
    cmake -S "${REPO_ROOT}/src" -B "${build_dir}" -G Ninja \
        -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        "${extra_args[@]}"

    cmake --build "${build_dir}" --config "${BUILD_TYPE}" -j"${JOBS}"

    echo "  -> ${build_dir}/SteamFlipper.so"

    # The libXtst bootstrap, built here rather than at install time. It is the
    # only other thing that needs a compiler, and leaving it to the installer
    # meant a machine that cannot build 32-bit C could not install artifacts
    # somebody else had already built for it -- including ones built in a
    # container by --container.
    if [ "${arch}" = "32" ]; then
        gcc -m32 -shared -fPIC -O2 \
            -o "${build_dir}/libXtst.so.6" \
            "${REPO_ROOT}/src/Bootstrap/Linux/sf_bootstrap.c" -ldl \
            && echo "  -> ${build_dir}/libXtst.so.6"
    fi
}

case "${ARCHES}" in
    both) build_arch 64; build_arch 32 ;;
    64)   build_arch 64 ;;
    32)   build_arch 32 ;;
    *)    echo "[!] Unknown arch '${ARCHES}' (expected: both, 64, 32)" >&2; exit 1 ;;
esac

echo ""
echo "=========================================="
echo "Build Successful!"
echo "Install with: ./tools/install_linux.sh --no-build"
echo "=========================================="
