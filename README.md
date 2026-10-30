# GobDump

<img src='./icon.png' width='100px' />

GobDump is a fork of SatDump, one of the best open-source satellite data processing software.
The goal of this fork is to have _more stable_ releases of v2 branch that will work on Windows no matter what, and I can write guides for it without worrying about breaking changes. It will also be used to test new features and decoders, mostly for my weather balloon experiments.

Code quality of new features will be abysmal compared to SatDump, but as long as they work and don't break anything, I don't care. I will also likely not merge all features to SatDump, if any at all.

A generic satellite data processing software.

If something is broken, please report it on the [GobDump issue tracker](https://github.com/MTalon63/GobDump/issues). GobDump reports should go there, not to the original SatDump project.

# Introduction

_Note : This is a very basic "how-to" skipping details and assuming some knowledge of what you are doing. For more details and advanced use cases, please see the [GobDump documentation](https://mtalon63.github.io/GobDump/)._

## GUI Version

### Processing recorded data

Quick-Start :

- Open the processing menu by selecting `File` > `Processing`
- Select the appropriate pipeline
- Select the input file (baseband, frames, soft symbols...)
- Set the appropriate input level (what type your input file is)
- Check settings shown below are right (such as sample rate)
- Press "Start"!

![Image showing the processing window](gui-menu.png)

![As described below](gui-processing.png)
_GobDump (1.0) demodulating a DVB-S2 baseband_

### Live processing or recording (directly from your SDR)

Quick-Start :

- Add a recorder by selecting `Add` > `Recorder`
- Select and start your SDR Device
- Choose a pipeline
- Start it, and done!

![GobDump as described below](gui-live-source.png)
_GobDump tuned to 103 MHz using a HydraSDR_

## CLI Version

![GobDump CLI initializing](cli-init.png)

### Offline processing

```
Usage : gobdump pipeline [pipeline_id] [input_level] [input_file] [output_file_or_directory] [additional options as required]
Extra options can be found by adding --help to the end of the command
Sample command :
gobdump pipeline metop_ahrpt baseband /home/user/metop_baseband.cs16 metop_output_directory --samplerate 6e6 --baseband_format cs16
```

You can find a list of Satellite pipelines and their parameters [Here](https://mtalon63.github.io/GobDump/Pipelines/).

### MSU-GS layer alignment (Elektro-L / Arktika-M)

MSU-GS VIS layers are aligned in-process at the end of the `elektro_rdas` and
`arktika_rdas` pipelines - no separate binary or OpenCV dependency. The three
detector layers are staggered along the scan, so the calibration seeds an ECC fit
with the 3606 / 1804 px offsets and refines the residual. It is kept in GobDump's
configuration directory (`rdas_aligner/<satellite>/config.json`), migrated from
older `rdas_aligner` files when present, and reused on later decodes; pass
`--recalibrate_aligner true` (or tick the checkbox in the MSU-GS decoder window)
to redo it.

An existing output directory can be (re)aligned without decoding again:

```
gobdump rdas_align [output_directory] --satellite L2 [--recalibrate]
```

This writes `MSUGS_VIS_Aligned` (or `MSUGS_VIS-1_Aligned` / `MSUGS_VIS-2_Aligned`
when the merge ROIs are unavailable), restores the raw products' channel
transforms in their `product.cbor` if they were lost, and updates the directory's
`dataset.json`.

### Live processing

```
Usage : gobdump legacy live [pipeline_id] [output_file_or_directory] [additional options as required]
Extra options (examples. Any parameter used in modules or sources can be used here) :
  --samplerate [baseband_samplerate] --baseband_format [cf32/cs32/cs16/cs8/w8] --dc_block --iq_swap
  --source [airspy/rtlsdr/etc] --gain 20 --bias
As well as --timeout in seconds
Sample command :
gobdump legacy live metop_ahrpt metop_output_directory --source airspy --samplerate 6e6 --frequency 1701.3e6 --general_gain 18 --bias --timeout 780
```

You can find a list of all SDR Options [Here](https://mtalon63.github.io/GobDump/SDR_Options/). Run `gobdump sdr_probe` to get a list of available SDRs and their IDs.

### Recording

```
Usage : gobdump legacy record [output_baseband (without extension!)] [additional options as required]
Extra options (examples. Any parameter used in sources can be used here) :
  --samplerate [baseband_samplerate] --baseband_format [cf32/cs32/cs16/cs8/cu8/w16] --dc_block --iq_swap
  --source [airspy/rtlsdr/etc] --gain 20 --bias
As well as --timeout in seconds
Sample command :
gobdump legacy record baseband_name --source airspy --samplerate 6e6 --frequency 1701.3e6 --general_gain 18 --bias --timeout 780 --baseband_format cf32
```

# Documentation

The documentation is self-hosted from this repository and published at <https://mtalon63.github.io/GobDump/>. To preview it locally:

```bash
python -m pip install -r docs/requirements.txt
mkdocs serve
```

Then open <http://127.0.0.1:8000>. `make docs` builds the site and `make docs-serve` starts the preview server.

# Building / Installing

### Quick start (any platform)

There is a `Makefile` that wraps the whole CMake flow. On a machine with the
dependencies already installed, building is:

```bash
git clone -b ports https://github.com/MTalon63/GobDump.git
cd GobDump
make build        # configure (if needed) + build
make run          # build, then run the CLI
```

The **`-b ports`** branch matters: `master` does not contain the current work,
including the arm64/NEON support and the built-in RDAS layer alignment. No
submodules are needed for a normal build or for the documentation.

Useful `make` variables and targets (`make help` lists everything):

```bash
make BUILD_TYPE=Debug BUILD_DIR=build-debug   # debug build, separate tree
make JOBS=4                                   # limit parallelism
make CMAKE_ARGS='-DBUILD_GUI=OFF'             # pass extra CMake flags
make test                                     # build, then run ctest
make install                                  # install to CMAKE_INSTALL_PREFIX
make reconfigure                              # re-run CMake after changing options
make clean / make distclean                   # clean outputs / wipe the build dir
```

The CMake commands below remain the canonical way to build; the `Makefile` only
saves typing. It passes `-DPLUGINS_ALL=ON` by default, since a normal dev build
wants the plugins.

#### Build options

| Option | Default | Notes |
|---|---|---|
| `BUILD_GUI` | `ON` | The `gobdump-ui` GUI |
| `BUILD_OPENCL` | `ON` | GPU accelerations |
| `BUILD_OPENMP` | `ON` | OpenMP |
| `BUILD_ZIQ` | `ON` | ZIQ recording compression |
| `BUILD_TESTING` | `OFF` | `ctest` test programs |
| `BUILD_TOOLS` | `OFF` | Extra tools programs |
| `BUILD_DOCS` | `OFF` | Doxygen docs |
| `ENABLE_I18N` | `ON` | Translations |

#### Profiling the DSP kernels

A self-contained profiler measures the DVB-S2 SIMD kernels against the scalar path and
against libvolk's dispatched kernels. It works on **any** platform - arm64 (NEON),
x86-64 (AVX2+FMA), or anywhere else (scalar + volk) - so you can compare machines:

```bash
bash tools/dvbs2_neon_profile/dvbs2_neon_profile.sh
```

It compiles the exact kernel bodies from `fir.cpp` and `clock_recovery_mm.cpp` into a
standalone benchmark (it never modifies the git tree) and reports best-of-N timings plus
a numeric comparison against the scalar path, so you can see both the speed and that the
result is unchanged. Environment knobs: `BENCH_N` (points per call), `BENCH_REPS`
(timing repetitions). Run from the repo root.

Reading the output: compare only rows from the same machine - absolute times are not
comparable across CPUs, but the ratios are. VOLK's SIMD kernels are already vectorized,
so a small win, or even a loss, is a realistic outcome here rather than a bug.

> **Building for your own machine vs. distributing:** a local (non-CI) build adds
> `-march=native`, so the resulting binaries only run on the CPU that built them. That
> is usually what you want, but do not ship those binaries. Building with the `CI`
> environment variable set (e.g. `CI=1 make build`) turns it off for portable output.

### Windows

Prebuilt installers and portable builds are on the [Releases](https://github.com/MTalon63/GobDump/releases)
page - no compilation necessary.

#### Building with MSYS2 (MinGW-w64 UCRT64)

Visual Studio is not required. From an **MSYS2 UCRT64** shell:

```bash
./windows/Setup-MSYS2.sh          # installs all dependencies (one time)
make build                        # or the equivalent CMake commands below
```

The equivalent CMake flow, if you prefer it explicit:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPLUGINS_ALL=ON
cmake --build build --parallel
```

_Note : the SDDC (RX666/RX888) plugin is unavailable on MinGW, as it depends on the MSVC-only Cypress CyAPI library._

### macOS

Dependency-free macOS builds are on the [Releases](https://github.com/MTalon63/GobDump/releases) page.

General build instructions (Homebrew is required):

```bash
# Install build tools and dependencies
brew install cmake ninja dylibbundler libtool autoconf automake meson orc libxml2 zlib \
             portaudio boost volk libomp glfw fftw jemalloc zstd armadillo hdf5 \
             sqlite3 pkg-config protobuf grpc

# Clone GobDump
git clone -b ports https://github.com/MTalon63/GobDump.git
cd GobDump

# Build the dependencies GobDump builds from source (not from Homebrew)
macOS/build_deps.sh
macOS/build_nng.sh   # NNG 1.x; Homebrew's nng 2.x removed the 1.x headers

# Build
make build        # or: cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPLUGINS_ALL=ON && cmake --build build --parallel
make run          # CLI;  make ui  for the GUI
```

To produce a bundle (`.app` / `.dmg`), configure with `-DBUNDLING_MODE=ON` and run
`macOS/bundle.sh`; the result lands in `MacApp/`.

### Linux

Building from source is recommended. Prebuilt `.deb` packages are produced for
Ubuntu by CI.

---

<details>

<summary>Install Dependencies - Debian, Ubuntu, and other Debian-based distros</summary>

```bash
# Required
sudo apt install git build-essential cmake ninja-build pkgconf libfftw3-dev libpng-dev \
                 libtiff-dev libjemalloc-dev libcurl4-openssl-dev libsqlite3-dev
sudo apt install libvolk-dev       # If this package is not found, use libvolk2-dev or libvolk1-dev
sudo apt install libnng-dev        # If this package is not found, follow the NNG instructions below

sudo apt install ocl-icd-opencl-dev opencl-headers

# (Optional)
sudo apt install libglfw3-dev libdbus-1-dev                      # Only if you want to build the GUI Version
sudo apt install portaudio19-dev                                  # Only if you want audio output
sudo apt install libzstd-dev                                      # Only if you want to build with ZIQ Recording compression
sudo apt install libhdf5-dev                                      # Only if you want official product support (ex, EUMETSAT)
sudo apt install librtlsdr-dev libhackrf-dev libairspy-dev libairspyhf-dev   # Libraries required for live processing
sudo apt install libad9361-dev libiio-dev                         # For ADALM-Pluto, PlutoPlus, AntSDR and other AD9361-based SDRs
sudo apt install libbladerf-dev                                   # For BladeRF
sudo apt install libomp-dev                                       # Shouldn't be required in general, but in case you have errors with OMP
sudo apt install intel-opencl-icd                                 # Enables OpenCL for Intel Integrated Graphics
sudo apt install mesa-opencl-icd                                  # For AMD Radeon cards *BEFORE series 5000*; newer cards need the official AMD drivers
sudo apt install libarmadillo-dev

# One-liner to install all dependencies:
sudo apt install git build-essential cmake ninja-build pkgconf libfftw3-dev libpng-dev \
                 libtiff-dev libjemalloc-dev libcurl4-openssl-dev libvolk-dev libnng-dev \
                 ocl-icd-opencl-dev opencl-headers \
                 libglfw3-dev zenity portaudio19-dev libzstd-dev libhdf5-dev librtlsdr-dev \
                 libhackrf-dev libairspy-dev libairspyhf-dev libad9361-dev libiio-dev \
                 libbladerf-dev libomp-dev intel-opencl-icd mesa-opencl-icd \
                 libdbus-1-dev libarmadillo-dev libsqlite3-dev
```

</details>
<details>

<summary>Install Dependencies - Red-Hat-based systems</summary>

```bash
sudo dnf install git cmake ninja-build g++ fftw-devel volk-devel libpng-devel jemalloc-devel \
                 libtiff-devel nng-devel curl-devel sqlite-devel
sudo dnf install ocl-icd opencl-headers

# (Optional)
sudo dnf install glfw-devel zenity dbus-devel                       # Only if you want to build the GUI Version
sudo dnf install portaudio-devel                                    # Only if you want audio output
sudo dnf install libzstd-devel                                      # Only if you want to build with ZIQ Recording compression
sudo dnf install hdf5-devel                                         # Only if you want official product support (ex, EUMETSAT)
sudo dnf install rtl-sdr-devel hackrf-devel airspyone_host-devel    # Libraries required for live processing
sudo dnf install libomp-devel                                       # Shouldn't be required in general, but in case you have errors with OMP
sudo dnf install intel-opencl                                       # Enables OpenCL for Intel Integrated Graphics
```

</details>
<details>

<summary>Install Dependencies - Alpine-based systems</summary>

```bash
# Adding the testing repository is required for libvolk-dev.
# You need to build libnng from source, see below.
sudo apk add git cmake ninja make g++ pkgconf fftw-dev libvolk-dev libpng-dev \
             jemalloc-dev tiff-dev curl-dev sqlite-dev
sudo apk add opencl-dev

# (Optional)
sudo apk add glfw-dev zenity dbus-dev                  # Only if you want to build the GUI Version
sudo apk add portaudio-dev                             # Only if you want audio output
sudo apk add zstd-dev                                  # Only if you want to build with ZIQ Recording compression
sudo apk add hdf5-dev                                  # Only if you want official product support (ex, EUMETSAT)
sudo apk add librtlsdr-dev hackrf-dev airspyone-host-dev airspyhf-dev  # Libraries required for live processing
```

</details>
<details>

<summary>Install Dependencies - Gentoo</summary>

```bash
# Assumes you already have gcc, g++, pkgconf, and cmake installed.
#
# NOTE: You will need to build nng yourself. See below for details.
#       Some SDR sources, such as AirSpy, also need to be built from
#       source if needed.

sudo emerge --ask dev-vcs/git sci-libs/fftw dev-libs/jemalloc sci-libs/volk \
                  media-libs/libpng media-libs/tiff net-misc/curl dev-db/sqlite

sudo emerge --ask media-libs/glfw gnome-extra/zenity sys-apps/dbus          # Only if you want to build the GUI Version
sudo emerge --ask media-libs/portaudio                                      # Only if you want audio output
sudo emerge --ask app-arch/zstd                                             # Only if you want to build with ZIQ Recording compression
sudo emerge --ask sci-libs/hdf5                                             # Only if you want official product support (ex, EUMETSAT)
sudo emerge --ask net-wireless/rtl-sdr                                      # RTL-SDR Support
sudo emerge --ask net-libs/libhackrf                                        # HackRF Support
sudo emerge --ask net-wireless/bladerf                                      # bladeRF Support
sudo emerge --ask net-libs/libiio net-libs/libad9361-iio                    # For ADALM-Pluto, PlutoPlus, AntSDR and other AD9361-based SDRs
sudo emerge --ask dev-libs/opencl-icd-loader                                # OpenCL Support, Optional, but recommended
```

</details>
<details>

<summary>Install Dependencies - openSUSE</summary>

```bash
sudo zypper install git gcc-c++ cmake ninja pkgconf fftw3-devel libpng-devel \
                    libtiff-devel jemalloc-devel libcurl-devel volk-devel \
                    nng-devel sqlite3-devel
sudo zypper install opencl-headers libOpenCL1

# (Optional)
sudo zypper install libglfw-devel zenity dbus-1-devel                       # Only if you want to build the GUI Version
sudo zypper install portaudio-devel                                        # Only if you want audio output
sudo zypper install libzstd-devel                                          # Only if you want to build with ZIQ Recording compression
sudo zypper install hdf5-devel                                             # Only if you want official product support (ex, EUMETSAT)
sudo zypper install rtl-sdr-devel hackrf-devel airspy-devel airspyhf-devel  # Libraries required for live processing
sudo zypper install libad9361-iio-devel libiio-devel                       # For ADALM-Pluto, PlutoPlus, AntSDR and other AD9361-based SDRs
sudo zypper install bladeRF-devel                                          # For BladeRF
sudo zypper install libomp-devel                                           # Shouldn't be required in general, but in case you have errors with OMP
sudo zypper install intel-opencl                                           # Enables OpenCL for Intel Integrated Graphics
sudo zypper install Mesa-libOpenCL                                         # For AMD Radeon cards *BEFORE series 5000*; newer cards need the official AMD drivers
```

</details>

---

If nng is not available for your distro, you will have to build it from source:

```bash
git clone https://github.com/nanomsg/nng.git -b v1.9.0
cd nng
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON -DCMAKE_INSTALL_PREFIX=/usr ..
make -j4
sudo make install
cd ../..
rm -rf nng
```

---

#### Build GobDump (Linux)

```bash
git clone -b ports https://github.com/MTalon63/GobDump.git
cd GobDump

make build   # or: cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPLUGINS_ALL=ON && cmake --build build --parallel

# To run without installing, symlink the runtime data next to the binary
cd build
ln -sf ../pipelines .
ln -sf ../resources .
ln -sf ../gobdump_cfg.json .
cd ..

# To install system-wide instead
sudo make install

# Run
make run     # CLI;  make ui  for the GUI
```

Add `-DBUILD_GUI=OFF` if you do not want the GUI. MSU-GS layer alignment is
built into GobDump - no external binary or OpenCV dependency.

### Raspberry Pi

Same as the Linux instructions above. On a Raspberry Pi 3 (any revision) or older, use
`make JOBS=1` **otherwise the system will crash** from running out of memory.

### Android

Android APKs are built by the GobDump CI and attached to the [GobDump Releases](https://github.com/MTalon63/GobDump/releases) page (including nightly builds).

Do keep in mind that while pretty much all features perfectly function on Android, there may be some limitations (either due to the hardware) in some places. For example, not all SDR Devices can be used.
Supported SDR devices are :

- RTL-SDR
- Airspy
- AirspyHF
- LimeSDR Mini
- HackRF

### Docker

Building and running under docker is a nice way to separate the build environment and libraries from the host OS.
The build process is a multistage build that uses two images, one with the -dev packages and another for the runtime.
This means that the runtime image can be kept smaller, although the disk space is still needed to complete the build.

To match the system user for the shared files get the same owner, set these in `.env` before building the image.
The user inside the container will always be named `satdump`, but the uid and gid will match the system user.

```bash
# set the current uid and gid
printf "HOST_UID: $(id -u)\nHOST_GID: $(id -g)\n" > .env

# create the shared directory
mkdir -p srv

# Build the images with compose, 8 parallel
docker compose build --build-arg CMAKE_BUILD_PARALLEL_LEVEL=8

# Launch a shell inside the container/service
docker compose run --rm -it satdump
```

The command that is started inside the container can either be specified at the end on the commandline mentioned above,
or put in the `.env` with for example `COMMAND: satdump_sdr_server`. If you want to run the sdr server in the background
and have it start automatically on boot, simply launch the container with `docker compose up -d` and check the logs with
`docker compose logs -f`

#### X11 under docker

To use the `gobdump-ui` under docker you need to make a few changes.
It is possible to run this on WSL2 as well, change the source to `/run/desktop/mnt/host/wslg/.X11-unix` instead.
In the [docker-compose.yml](docker-compose.yml) you need to uncomment a few lines and make it looks like this:

```yaml
- type: "bind"
  source: "/tmp/.X11-unix"
  target: "/tmp/.X11-unix"
```

If the user in the container is not authorized for X11, you will probably get this error message:
`Authorization required, ...`
This is due to the ACL controlling access to your screen.
There's several ways to solve this, a few very broad and insecure, but the following should be acceptable on a non-shared system.

On the host, run: `xhost +local:docker` , then to start the ui: `docker compose run --rm -it satdump gobdump-ui`
