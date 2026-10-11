#!/usr/bin/env bash
# dvbs2_neon_profile.sh - profile the DVB-S2 NEON kernels on an Apple Silicon Mac.
#
# WHAT IT DOES
#   1. checks you are actually on arm64 (otherwise the NEON kernels cannot be measured)
#   2. compiles a small benchmark containing the EXACT kernel bodies from this repo
#   3. runs it several times and reports the best (least noisy) numbers
#   4. optionally builds the three kernel-enabled/disabled variants to prove the
#      kernels change nothing about the output
#
# WHY IT IS SAFE TO RUN
#   It never touches your GobDump git tree. Everything is written to a temp dir.
#   It is read-only with respect to the repo.
#
# USAGE  (run from the repository root)
#   bash tools/dvbs2_neon_profile/dvbs2_neon_profile.sh          # bench only
#   bash tools/dvbs2_neon_profile/dvbs2_neon_profile.sh --full   # + end-to-end guidance
#
# Using "bash <script>" works even if the executable bit was not preserved by git.

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# The benchmark source sits next to this script.
NEONCHK="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK="${TMPDIR:-/tmp}/dvbs2_neon_profile.$$"
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

c_red()   { printf '\033[31m%s\033[0m\n' "$*"; }
c_grn()   { printf '\033[32m%s\033[0m\n' "$*"; }
c_yel()   { printf '\033[33m%s\033[0m\n' "$*"; }
c_bold()  { printf '\033[1m%s\033[0m\n' "$*"; }

c_bold "=== DVB-S2 NEON profiler ==="
echo "repo : $REPO"
echo "arch : $(uname -m)   kernel: $(uname -r)"
echo "cpu  : $(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo unknown)"
echo

# ---------------------------------------------------------------- sanity checks
if [ "$(uname -s)" != "Darwin" ]; then
    c_yel "WARNING: this is not macOS. The script still works on Linux/arm64,"
    c_yel "         but the sysctl/hw info below will be missing."
fi

ARCH="$(uname -m)"
if [ "$ARCH" != "arm64" ] && [ "$ARCH" != "aarch64" ]; then
    c_red "ERROR: this is not an arm64 machine (got '$ARCH')."
    c_red "The NEON kernels only exist on arm64, so there is nothing useful to measure."
    exit 1
fi
c_grn "arm64 confirmed."

CXX="${CXX:-c++}"
if ! command -v "$CXX" >/dev/null 2>&1; then
    c_red "ERROR: no C++ compiler found. Install the Xcode command line tools:"
    c_red "  xcode-select --install"
    exit 1
fi
echo "compiler: $($CXX --version | head -1)"


# ---------------------------------------------------------------- volk probe
# The comparison against libvolk is only meaningful if we find it.
VOLK_FLAGS=""
HAVE_VOLK=0
if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists volk 2>/dev/null; then
    VOLK_FLAGS="$(pkg-config --cflags --libs volk)"
    HAVE_VOLK=1
    c_grn "libvolk found via pkg-config - will include the dispatched comparison."
elif [ -d /opt/homebrew/include/volk ] || [ -d /usr/local/include/volk ]; then
    VOLKP="$( ( [ -d /opt/homebrew/include/volk ] && echo /opt/homebrew ) || echo /usr/local )"
    VOLK_FLAGS="-I$VOLKP/include -L$VOLKP/lib -lvolk"
    HAVE_VOLK=1
    c_grn "libvolk found under $VOLKP - will include the dispatched comparison."
else
    c_yel "libvolk not found - skipping the VOLK comparison."
    c_yel "(install it with:  brew install volk)"
fi
echo ""

# ---------------------------------------------------------------- compile
SRC="$NEONCHK/dvbs2_neon_bench.cpp"
if [ ! -f "$SRC" ]; then
    c_red "ERROR: $SRC not found."
    c_red "This script must sit next to dvbs2_neon_bench.cpp (tools/dvbs2_neon_profile/)."
    exit 1
fi

BIN="$WORK/bench"
DEFS=""
[ "$HAVE_VOLK" = "1" ] && DEFS="-DHAVE_VOLK=1"

c_bold "--- compiling benchmark ---"
# Disable AUTO-vectorization so the "scalar" baselines are genuinely scalar.
# Explicit NEON intrinsics are unaffected by these flags, so the NEON kernels
# still run at full width. The flag name differs between compilers.
if $CXX -fno-tree-vectorize -x c++ -c /dev/null -o /dev/null 2>/dev/null; then
    NOVEC_FLAG="-fno-tree-vectorize"          # GCC
elif $CXX -fno-vectorize -x c++ -c /dev/null -o /dev/null 2>/dev/null; then
    NOVEC_FLAG="-fno-vectorize"               # Clang (what the Mac uses)
else
    NOVEC_FLAG=""
    c_yel "WARNING: could not disable auto-vectorization. The scalar rows will be"
    c_yel "         auto-vectorized, flattering the baseline - treat the"
    c_yel "         NEON-vs-scalar ratio with suspicion in this run."
fi
[ -n "$NOVEC_FLAG" ] && echo "auto-vectorization: disabled via $NOVEC_FLAG"
# -O3 to match how the real kernels are built. No -ffast-math: the point is to
# measure the same code the decoder uses, and the M&M TU deliberately omits it.
set -x
$CXX -O3 -std=gnu++17 -DNDEBUG $NOVEC_FLAG $DEFS -I"$REPO" $VOLK_FLAGS "$SRC" -o "$BIN" -lm
set +x
c_grn "compiled."
echo ""

# ---------------------------------------------------------------- run
c_bold "--- running (best of 5 runs; the minimum is the honest number) ---"
for run in 1 2 3 4 5; do
    echo ""; echo "### run $run ###"
    "$BIN"
done | tee "$WORK/bench.raw.txt"

# ---------------------------------------------------------------- summarise
# Pull the NEON vs scalar vs volk lines and give a quick verdict.
echo ""
c_bold "=== SUMMARY (best observed per row) ==="
awk '
  /^  / {
    # strip the ns/pt and speedup columns, keep name + ms
    name=""; for (i=1;i<=NF-5;i++) name=name $i " ";
    ms=$(NF-3)+0;
    key=name;
    if (!(key in best) || ms < best[key]) { best[key]=ms }
  }
  END { for (k in best) printf "  %-36s %9.3f ms\n", k, best[k] }
' "$WORK/bench.raw.txt" | sort

echo ""
c_bold "=== what to look for ==="
cat <<'EOF'
  * 'NEON (this repo's kernel)' should beat 'scalar (vectorization off)'.
    If it does not, the kernel is not worth keeping on this CPU.
  * Compare 'NEON ...' against 'libvolk (dispatched)'. That is the real question:
    VOLK's NEON path is already vectorized, so a small win (~1.1x) is expected and
    a LOSS is entirely possible. Report it either way - do not assume the port wins.
  * The FIR kernel measured near break-even (~1.0-1.1x) on emulation. If it is at or
    below 1.0x on your M4, it should be dropped - it is self-contained behind its guard.
  * The M&M kernel was the better bet (~1.5x). Confirm or refute.
EOF

echo ""
if [ "${1:-}" = "--full" ]; then
    c_bold "=== --full: building the real DVB-S2 plugin and decoding ==="
    cat <<'EOF'
  This runs the actual pipeline end-to-end. It is the strongest check, because it
  proves the kernels do not change the DECODE, not just that they are fast.
EOF
    echo ""
    echo "  Not automated here - it needs a capture file and takes several minutes."
    echo "  See build/regress/run_dvbs2.ps1 for the parameters, and use:"
    echo ""
    echo "    make build                     # or: cmake -S . -B build -G Ninja -DPLUGINS_ALL=ON && cmake --build build --parallel"
    echo "    cd build && ./gobdump legacy dvbs2_bb baseband <clip.cs8> <outdir> \\"
    echo "        --samplerate 36500000 --baseband_format cs8 --symbolrate 27500000 \\"
    echo "        --modcod 11 --pilots false --rrc_alpha 0.35 --pll_bw 0.01"
    echo ""
    echo "  Then score the .bbframe output. Also worth checking, on the SAME build:"
    echo "    GOBDUMP_LDPC_GENERIC=1 ./gobdump ...   # bypasses the SIMD LDPC"
    echo "    # and compare decode timings with the NEON kernels compiled out: see"
    echo "    # build/neonchk/noneon_build.sh for how that was done."
fi

echo ""
c_grn "done."
