#!/bin/bash
set -Eeo pipefail

if [[ -z "${GITHUB_WORKSPACE:-}" ]]; then
    GITHUB_WORKSPACE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." &> /dev/null && pwd)"
fi

output_dir="$GITHUB_WORKSPACE/deps"
work_dir="$GITHUB_WORKSPACE/deps-temp"
if [[ ! -d "$output_dir/include" ]]; then
    echo "deps/ is missing; run macOS/build_deps.sh first." >&2
    exit 1
fi
mkdir -p "$work_dir"

if [[ -f "$output_dir/lib/libnng.dylib" ]] && grep -q '^#define NNG_MAJOR_VERSION 1$' "$output_dir/include/nng/nng.h"; then
    echo "NNG 1.x already present in $output_dir, skipping."
    exit 0
fi

rm -rf "$work_dir/nng"
git clone --depth 1 -b v1.10.0 https://github.com/nanomsg/nng.git "$work_dir/nng"

cmake -S "$work_dir/nng" -B "$work_dir/nng/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$output_dir" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DBUILD_SHARED_LIBS=ON \
    -DNNG_ENABLE_TLS=OFF \
    -DNNG_TESTS=OFF \
    -DNNG_TOOLS=OFF
cmake --build "$work_dir/nng/build" --target install
rm -rf "$work_dir/nng"

echo "Installed NNG:"
grep -E '^#define NNG_(MAJOR|MINOR|PATCH)_VERSION' "$output_dir/include/nng/nng.h"
