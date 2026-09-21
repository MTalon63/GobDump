param(
    [bool]$is_cross=0,
    [bool]$is_arm=0
)

$ErrorActionPreference = "Stop"

function Invoke-CheckedNative {
    param([string]$Exe, [string[]]$Arguments)
    $app = Get-Command $Exe -CommandType Application -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($null -eq $app) {
        throw "Executable '$Exe' not found on PATH"
    }
    & $app.Source @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command '$Exe $($Arguments -join ' ')' failed with exit code $LASTEXITCODE"
    }
}
function ninja { Invoke-CheckedNative "ninja" $args }

# if(!!(Get-Command 'tf' -ErrorAction SilentlyContinue) -eq $false)
# {
#     Write-Error "You must run this script within Developer Powershell for Visual Studio"
#     exit
# }

mkdir deps
cd deps

mkdir output
$output_folder=$(Resolve-Path output)
$output_folder_fwd=$output_folder -replace '\\', '/'
$python_interpreter=$($(Get-Command python).Path)
$cmake_params="-G Ninja", "-DCMAKE_FIND_ROOT_PATH='$output_folder'", "-DCMAKE_INSTALL_PREFIX='$output_folder'", "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_SYSTEM_NAME=Windows", "-DPYTHON_EXECUTABLE:FILEPATH=$python_interpreter"

if($is_arm) {
    $cmake_params+="-DCMAKE_SYSTEM_PROCESSOR=ARM64"
} else {
    $cmake_params+="-DCMAKE_SYSTEM_PROCESSOR=AMD64"
}

if($is_cross) {
    $cmake_params+="-DCMAKE_CROSSCOMPILING=ON"
}

# ZLib
git clone https://github.com/madler/zlib --depth 1 -b v1.3.1
cd zlib
mkdir build
cd build
cmake $cmake_params .. -DBUILD_SHARED_LIBS=ON
ninja install
cd ../..

# Curl
git clone https://github.com/curl/curl --depth 1 -b curl-8_9_1
cd curl
mkdir build
cd build
cmake $cmake_params .. -DHTTP_ONLY=ON -DBUILD_STATIC_LIBS=OFF -DCURL_USE_SCHANNEL=ON -DUSE_LIBIDN2=OFF -DCURL_USE_LIBPSL=OFF
ninja install
cd ../..

# VOLK v3.3.0. The MSVC-ARM64 / NEON enablement (not yet upstreamed in
# gnuradio/volk) is re-applied locally from windows/patches/volk-3.3.0-msvc-arm64.patch,
# ported from JVital2013/volk commit 51c251846ba591453a92480a95a27a3ffe901d4b
# ("Windows: ARM64/NEON Support"). Apply it unconditionally for both arches: the
# changes are MSVC/NEON-guarded and are a no-op (or benign) on x64. This replaces
# the old two-branch scheme (stock v3.1.2 on x64, fork on arm64) with one 3.3.0
# source plus the local patch, keeping both arches deterministic at the same tag.
# Pin core.autocrlf=false so the checked-out files are LF: this patch is LF and
# a CRLF working tree would make `git apply` fail on hunk context mismatch.
$py = Get-Command python -CommandType Application -ErrorAction SilentlyContinue |
    Select-Object -First 1
if ($null -ne $py) {
    & $py.Source -c "import mako" 2>$null
    if ($LASTEXITCODE -ne 0) {
        Invoke-CheckedNative "python" @("-m", "pip", "install", "mako")
    }
}
git clone -c core.autocrlf=false https://github.com/gnuradio/volk --depth 1 -b v3.3.0 volk
cd volk
# Init submodules from the volk repo ROOT, before entering build/.
Invoke-CheckedNative "git" @("submodule", "update", "--init")
# $PSScriptRoot is windows/ (where this script lives). Apply -p1 from the repo
# root so the patch's a/b paths (e.g. kernels/...) resolve under the volk tree.
# native `git apply` returns non-zero without throwing (EAP=Stop does not cover
# native exit codes), so route it through Invoke-CheckedNative to fail loudly.
Invoke-CheckedNative "git" @("apply", "-p1", "$($PSScriptRoot)/patches/volk-3.3.0-msvc-arm64.patch")
mkdir build
cd build
cmake $cmake_params .. -DENABLE_TESTING=OFF -DENABLE_MODTOOL=OFF -DENABLE_STATIC_LIBS=OFF
ninja install
cd ../..

# NNG
git clone https://github.com/nanomsg/nng --depth 1 -b v1.8.0
cd nng
mkdir build
cd build
cmake $cmake_params .. -DNNG_TOOLS=OFF -DNNG_TESTS=OFF -DNNG_ENABLE_NNGCAT=OFF -DBUILD_SHARED_LIBS=ON
ninja install
cd ../..

# Zstd
git clone https://github.com/facebook/zstd --depth 1 -b v1.5.6
cd zstd
mkdir build2
cd build2
cmake $cmake_params ../build/cmake -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_STATIC=ON -DZSTD_BUILD_SHARED=ON -DCMAKE_RC_FLAGS="-I $output_folder_fwd/../zstd/lib"
ninja install
cd ../..

# SQLite
git clone https://github.com/sjinks/sqlite3-cmake --depth 1 -b master sqlite
cd sqlite
mkdir build
cd build
cmake $cmake_params .. -DBUILD_SHARED_LIBS=ON  -DCMAKE_WINDOWS_EXPORT_ALL_SYMBOLS=ON
ninja install
cd ../..

# LibPNG
git clone https://github.com/glennrp/libpng --depth 1 -b v1.6.43
cd libpng
mkdir build
cd build
cmake $cmake_params .. -DPNG_EXECUTABLES=OFF -DPNG_TESTS=OFF -DPNG_SHARED=ON
ninja install
cd ../..

# TIFF
git clone https://github.com/libsdl-org/libtiff --depth 1 -b v4.6.0
cd libtiff
mkdir build2
cd build2
cmake $cmake_params .. -DBUILD_SHARED_LIBS=ON -Dtiff-tools=OFF -Dtiff-tests=OFF -Dtiff-docs=OFF
ninja install
cd ../..

# HDF5
git clone https://github.com/HDFGroup/hdf5 --depth 1 -b  2.0.0
cd hdf5
mkdir build
cd build
cmake $cmake_params .. -DBUILD_SHARED_LIBS=ON -DHDF5_BUILD_CPP_LIB=1 -DHDF5_ENABLE_ZLIB_SUPPORT=ON -DBUILD_TESTING=OFF
ninja install
cd ../..

# FFTW
Invoke-WebRequest -Uri http://www.fftw.org/fftw-3.3.10.tar.gz -OutFile fftw.tar.gz
tar -zxf fftw.tar.gz
cd fftw-3.3.10
mkdir build
cd build
cmake $cmake_params .. -DBUILD_SHARED_LIBS=ON -DENABLE_FLOAT=ON -DCMAKE_POLICY_VERSION_MINIMUM="3.5"
ninja install
cd ../..

# pthread
git clone https://github.com/GerHobbelt/pthread-win32 --depth 1 -b v4.1.0.9
cd pthread-win32
mkdir build
cd build
cmake $cmake_params .. -DBUILD_SHARED_LIBS=ON
ninja install
cd ../..

# GLFW
git clone https://github.com/glfw/glfw --depth 1 -b 3.5.1
cd glfw
mkdir build
cd build
cmake $cmake_params .. -DBUILD_SHARED_LIBS=ON
ninja install
cd ../..

# Libusb
git clone https://github.com/libusb/libusb-cmake libusb --depth 1 -b v1.0.30
cd libusb
mkdir build
cd build
cmake $cmake_params .. -DBUILD_SHARED_LIBS=ON
ninja install
cd ../..

# Libairspy
git clone https://github.com/airspy/airspyone_host libairspy --depth 1 -b v1.0.10
cd libairspy
mkdir build
cd build
cmake $cmake_params ..  -DCMAKE_WINDOWS_EXPORT_ALL_SYMBOLS=ON -DBUILD_SHARED_LIBS=ON -DCMAKE_POLICY_VERSION_MINIMUM="3.5" -DLIBUSB_INCLUDE_DIR="$output_folder_fwd/include/libusb-1.0" -DTHREADS_PTHREADS_WIN32_LIBRARY="$output_folder_fwd/lib/pthreadVC3.lib"
ninja install
cd ../..

# HydraSDR
if(!$is_arm) {
    git clone https://github.com/hydrasdr/hydrasdr-host libhydrasdr --depth 1 -b v1.1.2
    cd libhydrasdr
    mkdir build
    cd build
    cmake $cmake_params .. -DBUILD_SHARED_LIBS=ON -DCMAKE_POLICY_VERSION_MINIMUM="3.5" -DLIBUSB_INCLUDE_DIR="$output_folder_fwd/include/libusb-1.0" -DLIBUSB_LIBRARIES="$output_folder_fwd/lib/usb-1.0.lib" -DTHREADS_PTHREADS_WIN32_LIBRARY="$output_folder_fwd/lib/pthreadVC3.lib"
    ninja install
    cd ../..
}

# RTL-SDR
git clone https://github.com/rtlsdrblog/rtl-sdr-blog --depth 1 -b master
cd rtl-sdr-blog
mkdir build
cd build
cmake $cmake_params .. -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON -DBUILD_SHARED_LIBS=ON -DCMAKE_POLICY_VERSION_MINIMUM="3.5" -DLIBUSB_INCLUDE_DIRS="$output_folder_fwd/include/libusb-1.0" -DLIBUSB_LIBRARIES="$output_folder_fwd/lib/usb-1.0.lib" -DTHREADS_PTHREADS_LIBRARY="$output_folder_fwd/lib/pthreadVC3.lib" -DTHREADS_PTHREADS_INCLUDE_DIR="$output_folder_fwd/include/"
ninja install
cd ../..

# Portaudio
git clone https://github.com/PortAudio/portaudio --depth 1 -b v19.7.0
cd portaudio
mkdir build2
cd build2
cmake $cmake_params .. -DBUILD_SHARED_LIBS=ON -DCMAKE_POLICY_VERSION_MINIMUM="3.5"
ninja install
cd ../..

# OpenCL
git clone https://github.com/KhronosGroup/OpenCL-SDK --depth 1 -b v2026.05.29 --recursive
cd OpenCL-SDK
mkdir build
cd build
cmake $cmake_params .. -DBUILD_SHARED_LIBS=ON
ninja install
cd ../..

# LimeSuite v23.11.0
if(!$is_arm) {
    git clone -c core.autocrlf=false https://github.com/myriadrf/Limesuite --depth 1 -b v23.11.0
    cd Limesuite
    Invoke-CheckedNative "git" @("apply", "-p1", "$($PSScriptRoot)/patches/limesuite-v23.11.0-msvc-chrono.patch")
    mkdir build2
    cd build2
    cmake $cmake_params .. -DBUILD_SHARED_LIBS=ON -DCMAKE_POLICY_VERSION_MINIMUM="3.5"
    ninja install
    cd ../..
}

# libxml2
git clone https://github.com/gnome/libxml2 --depth 1 -b v2.15.4
cd libxml2
mkdir build
cd build
cmake $cmake_params .. -DBUILD_SHARED_LIBS=ON -DLIBXML2_WITH_ICONV=OFF
ninja install
cd ../..

# LIBIIO
# libiio v0.26 only probes libusb inside `if(PkgConfig_FOUND)`; its
# find_library/find_path fallback is nested there as well, so on Windows (no
# pkg-config) LIBUSB_LIBRARIES/LIBUSB_INCLUDE_DIR are never set and configure
# aborts with "Unable to find libusb-1.0 dependency." libiio does NOT call
# find_package(libusb), so libusb-cmake's installed package config is no help.
# Pass the libusb paths explicitly (libusb was built earlier into
# $output_folder) and disable pkg-config so those cache values are used
# directly -- mirrors the rtl-sdr/fobos invocations above. Applies to both
# x64 and ARM64 (the same $output_folder holds the arch-matched usb-1.0.lib).
git clone https://github.com/analogdevicesinc/libiio --depth 1 -b v0.26
cd libiio
mkdir build
cd build
cmake $cmake_params .. -DBUILD_SHARED_LIBS=ON -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON -DLIBUSB_INCLUDE_DIR="$output_folder_fwd/include/libusb-1.0" -DLIBUSB_LIBRARIES="$output_folder_fwd/lib/usb-1.0.lib"
ninja install
cd ../..

# libad9361
git clone https://github.com/analogdevicesinc/libad9361-iio --depth 1 -b v0.4.0
cd libad9361-iio
mkdir build
cd build
cmake $cmake_params .. -DBUILD_SHARED_LIBS=ON
ninja install
cd ../..

# LibairspyHF
git clone https://github.com/airspy/airspyhf --depth 1 -b master
cd airspyhf
mkdir build
cd build
cmake $cmake_params .. -DCMAKE_WINDOWS_EXPORT_ALL_SYMBOLS=ON -DBUILD_SHARED_LIBS=ON -DCMAKE_POLICY_VERSION_MINIMUM="3.5" -DLIBUSB_INCLUDE_DIR="$output_folder_fwd/include/libusb-1.0" -DTHREADS_PTHREADS_WIN32_LIBRARY="$output_folder_fwd/lib/pthreadVC3.lib"
ninja install
cd ../..

# Libbladerf
git clone https://github.com/nuand/bladeRF --depth 1 -b libbladeRF_v2.6.0
cd bladeRF
mkdir build
cd build
cmake $cmake_params .. -DBUILD_SHARED_LIBS=ON -DTREAT_WARNINGS_AS_ERRORS=NO
ninja install
cd ../..
cp $output_folder/lib/bladeRF.dll $output_folder/bin

# Fobos
git clone https://github.com/rigexpert/libfobos --depth 1 -b v2.4.0
cd libfobos
mkdir libusb/include
cp -r $output_folder/include/libusb-1.0 libusb/include
mkdir libusb/MS64
mkdir libusb/MS64/dll
mkdir libusb/MS32
mkdir libusb/MS32/dll
cp $output_folder/lib/usb-1.0.lib libusb/MS64/dll/libusb-1.0.lib
cp $output_folder/lib/usb-1.0.lib libusb/MS32/dll/libusb-1.0.lib
cp $output_folder/bin/libusb-1.0.dll libusb/MS64/dll/libusb-1.0.dll
cp $output_folder/bin/libusb-1.0.dll libusb/MS32/dll/libusb-1.0.dll
mkdir build
cp -r libusb build
cd build
mkdir Release
mkdir Debug
cmake $cmake_params ..  -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON -DBUILD_SHARED_LIBS=ON -DCMAKE_POLICY_VERSION_MINIMUM="3.5" -DLIBUSB_LIBRARIES="./libusb/MS64/dll"
ninja install
cd ../..

# HackRF
git clone https://github.com/greatscottgadgets/hackrf --depth 1 -b v2026.01.3
cd hackrf
mkdir build
cd build
cmake $cmake_params ../host -DCMAKE_WINDOWS_EXPORT_ALL_SYMBOLS=ON -DBUILD_SHARED_LIBS=ON -DCMAKE_POLICY_VERSION_MINIMUM="3.5" -DLIBUSB_INCLUDE_DIR="$output_folder_fwd/include/libusb-1.0" -DCMAKE_C_FLAGS="-I$output_folder_fwd/include" -DCMAKE_USE_PTHREADS_INIT=ON -DTHREADS_FOUND=TRUE -DCMAKE_THREAD_LIBS_INIT="$output_folder_fwd/lib/pthreadVC3.lib"
ninja install
cd ../..

if(!$is_arm) {
    # Boost
    git clone https://github.com/boostorg/boost --depth 1 -b boost-1.92.0 --recursive
    cd boost
    mkdir build
    cd build
    cmake $cmake_params .. -DBUILD_SHARED_LIBS=ON
    ninja install
    cd ../..
    cp -r $output_folder/include/boost-1_92/boost $output_folder/include

    # Protobuf
    git clone https://github.com/protocolbuffers/protobuf --depth 1 -b v36.1
    cd protobuf
    mkdir build
    cd build
    cmake $cmake_params .. -DBUILD_SHARED_LIBS=ON
    ninja install
    cd ../..

    # LIBUHD
    git clone https://github.com/ettusresearch/uhd --depth 1 -b v4.10.0.0
    cd uhd
    mkdir build
    cd build
    cmake $cmake_params ../host -DCMAKE_CXX_FLAGS="/EHsc /FIwinsock2.h" -DCMAKE_POLICY_VERSION_MINIMUM="3.5" -DBUILD_SHARED_LIBS=ON -DENABLE_MAN_PAGES=OFF -DENABLE_MANUAL=OFF -DENABLE_PYTHON_API=OFF -DENABLE_EXAMPLES=OFF -DENABLE_UTILS=OFF -DENABLE_TESTS=OFF 
    ninja install
    cd ../..
}

<#
# libarmadillo
git clone https://gitlab.com/armadillo-lib/armadillo-code armadillo --depth 1 -b 15.6.x
cd armadillo
mkdir build
cd build
cmake $cmake_params .. -DBUILD_SHARED_LIBS=ON 
ninja install
cd ../..
#>

cd ..