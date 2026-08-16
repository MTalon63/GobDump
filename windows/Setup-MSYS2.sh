#!/usr/bin/env bash

set -euo pipefail

PKG_PREFIX="mingw-w64-ucrt-x86_64"
SRC_DIR="${TMPDIR:-/tmp}/gobdump-deps"

main() {

if [ "${MSYSTEM:-}" != "UCRT64" ]; then
    echo "ERROR: This script must be run from an MSYS2 UCRT64 shell."
    echo "       Current MSYSTEM is '${MSYSTEM:-unset}'."
    exit 1
fi

echo "==> Updating package databases..."
pacman -Sy --noconfirm

echo "==> Installing toolchain..."
pacman -S --needed --noconfirm \
    base-devel git \
    ${PKG_PREFIX}-toolchain \
    ${PKG_PREFIX}-cmake \
    ${PKG_PREFIX}-ninja \
    ${PKG_PREFIX}-pkgconf

echo "==> Installing core dependencies..."
pacman -S --needed --noconfirm \
    ${PKG_PREFIX}-volk \
    ${PKG_PREFIX}-fftw \
    ${PKG_PREFIX}-libpng \
    ${PKG_PREFIX}-libjpeg-turbo \
    ${PKG_PREFIX}-libtiff \
    ${PKG_PREFIX}-glfw \
    ${PKG_PREFIX}-libusb \
    ${PKG_PREFIX}-libxml2 \
    ${PKG_PREFIX}-portaudio \
    ${PKG_PREFIX}-nng \
    ${PKG_PREFIX}-zstd \
    ${PKG_PREFIX}-curl \
    ${PKG_PREFIX}-hdf5 \
    ${PKG_PREFIX}-sqlite3 \
    ${PKG_PREFIX}-armadillo \
    ${PKG_PREFIX}-opencl-icd \
    ${PKG_PREFIX}-opencl-headers

echo "==> Installing SDR dependencies available in MSYS2..."
pacman -S --needed --noconfirm \
    ${PKG_PREFIX}-rtl-sdr \
    ${PKG_PREFIX}-hackrf \
    ${PKG_PREFIX}-libiio \
    ${PKG_PREFIX}-libad9361-iio \
    ${PKG_PREFIX}-libuhd \
    ${PKG_PREFIX}-boost \
    ${PKG_PREFIX}-soapysdr

# cpu_features is not packaged; GobDump needs it for runtime SIMD dispatch.
build_cmake_dep() {
    local name="$1" url="$2" branch="$3" subdir="$4" marker="$5"
    shift 5

    if [ -e "${MINGW_PREFIX}/${marker}" ]; then
        echo "==> ${name} already installed, skipping."
        return
    fi

    echo "==> Building ${name}..."
    rm -rf "${SRC_DIR:?}/${name}"
    git clone --depth 1 -b "$branch" "$url" "${SRC_DIR}/${name}"

    if declare -F "patch_${name}" >/dev/null; then
        "patch_${name}" "${SRC_DIR}/${name}"
    fi

    cmake -S "${SRC_DIR}/${name}/${subdir}" -B "${SRC_DIR}/${name}/build" \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$MINGW_PREFIX" \
        "$@"
    cmake --build "${SRC_DIR}/${name}/build"
    cmake --install "${SRC_DIR}/${name}/build"
    rm -rf "${SRC_DIR:?}/${name}"
}

mkdir -p "$SRC_DIR"

build_cmake_dep cpu_features https://github.com/google/cpu_features v0.10.1 . \
    include/cpu_features/cpuinfo_x86.h \
    -DBUILD_TESTING=OFF -DBUILD_EXECUTABLE=OFF -DBUILD_SHARED_LIBS=ON

# These predate C23, where bool became a keyword.
LEGACY_C=(-DCMAKE_C_FLAGS=-std=gnu11)

build_cmake_dep airspy https://github.com/airspy/airspyone_host master libairspy \
    include/libairspy/airspy.h \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 "${LEGACY_C[@]}"

build_cmake_dep airspyhf https://github.com/airspy/airspyhf master libairspyhf \
    include/libairspyhf/airspyhf.h \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 "${LEGACY_C[@]}"

patch_bladeRF() {
    # Only stages pthreadVC2.dll for MSVC, but hard-errors when pthreads-win32
    # is missing. MinGW links winpthreads, so drop the module.
    : > "$1/host/cmake/modules/FindLibPThreadsWin32.cmake"

    # The bundled CLI/FSK tools don't link under GCC 16 (-fno-common, and no
    # setenv on Windows). GobDump only needs libbladeRF itself.
    : > "$1/host/utilities/CMakeLists.txt"

    # Installs the DLL to lib/, where Windows won't find it at runtime.
    sed -i 's|RUNTIME DESTINATION ${CMAKE_INSTALL_LIBDIR}|RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}|' \
        "$1/host/libraries/libbladeRF/CMakeLists.txt"
}

# bladeRF's libusb version probe assumes an MSVC ".lib" name and so can't load
# MinGW's "libusb-1.0.dll.a". It reports 0.0.0 and fails its own >=1.0.19 gate,
# hence the manual version. LIBUSB_PATH defaults to an upstream Windows install
# dir that doesn't exist here, breaking a post-build DLL copy.
build_cmake_dep bladeRF https://github.com/Nuand/bladeRF 2024.05 host \
    include/libbladeRF.h \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 "${LEGACY_C[@]}" \
    -DLIBUSB_SKIP_VERSION_CHECK=ON \
    -DLIBUSB_VERSION="$(pkg-config --modversion libusb-1.0)" \
    -DLIBUSB_PATH="$MINGW_PREFIX" \
    -DLIBUSB_LIBRARY_PATH_SUFFIX=bin \
    -DTREAT_WARNINGS_AS_ERRORS=OFF -DTEST_LIBBLADERF=OFF -DENABLE_backend_usb=ON

rmdir "$SRC_DIR" 2>/dev/null || true

echo
echo "==> Done. Build GobDump with:"
echo "      cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPLUGINS_ALL=ON"
echo "      cmake --build build"

}

main "$@"
