#!/bin/bash
set -Eeo pipefail

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
REPO_DIR="$( cd -- "$SCRIPT_DIR/.." && pwd )"

UHD_TAG="v4.10.0.0"
PATCH_FILE="$REPO_DIR/patches/uhd-4.10-b200-hotplug.patch"
working_dir="$REPO_DIR/deps-temp"
output_dir="$REPO_DIR/deps"

if [ -f "$output_dir/.uhd-built" ]; then
    echo "Patched UHD already built at $output_dir, skipping."
    exit 0
fi

for tool in git cmake ninja; do
    if ! command -v "$tool" > /dev/null 2>&1; then
        echo "error: '$tool' is required to build UHD" >&2
        exit 1
    fi
done

if ! command -v python3 > /dev/null 2>&1 || ! python3 -c "import mako" > /dev/null 2>&1; then
    echo "error: UHD needs python3 with the 'mako' module (apt install python3-mako)" >&2
    exit 1
fi

mkdir -p "$working_dir"
cd "$working_dir"

if [ ! -d uhd/.git ]; then
    rm -rf uhd
    git clone https://github.com/EttusResearch/uhd --depth 1 -b "$UHD_TAG" uhd
fi

if git -C uhd apply --check --ignore-whitespace -p1 "$PATCH_FILE" 2> /dev/null; then
    echo "Applying GobDump UHD hotplug patch..."
    git -C uhd apply --ignore-whitespace -p1 "$PATCH_FILE"
elif ! git -C uhd apply --reverse --check --ignore-whitespace -p1 "$PATCH_FILE" 2> /dev/null; then
    echo "error: cannot apply $PATCH_FILE" >&2
    exit 1
fi

cmake -S uhd/host -B uhd/host/build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$output_dir" \
    -DUHD_IMAGES_DIR=/usr/share/uhd/images \
    -DENABLE_MAN_PAGES=OFF \
    -DENABLE_MANUAL=OFF \
    -DENABLE_PYTHON_API=OFF \
    -DENABLE_EXAMPLES=OFF \
    -DENABLE_UTILS=OFF \
    -DENABLE_TESTS=OFF

cmake --build uhd/host/build --parallel "$(nproc)"
cmake --install uhd/host/build

touch "$output_dir/.uhd-built"
echo "Patched UHD ($UHD_TAG) installed to $output_dir"
