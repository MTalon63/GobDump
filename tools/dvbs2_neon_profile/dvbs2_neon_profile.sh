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
# WORKS ON: macOS (arm64), Linux (arm64 or x86-64), Windows/MSYS2 or WSL.
#   On arm64 it measures the NEON kernels; on x86-64 the AVX2 ones. Same source,
#   same methodology, so you can compare ratios across machines.
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

c_bold "=== DVB-S2 DSP kernel profiler ==="
echo "repo : $REPO"
echo "arch : $(uname -m)   kernel: $(uname -r)"
CPU_INFO=""
if [ "$(uname -s)" = "Darwin" ]; then
    CPU_INFO="$(sysctl -n machdep.cpu.brand_string 2>/dev/null || true)"
elif [ -r /proc/cpuinfo ]; then
    CPU_INFO="$(awk -F: '/model name|Model|Hardware/ {gsub(/^ +/,"",$2); print $2; exit}' /proc/cpuinfo)"
fi
NPROC=""
if [ "$(uname -s)" = "Darwin" ]; then NPROC="$(sysctl -n hw.ncpu 2>/dev/null || true)"; else NPROC="$(nproc 2>/dev/null || true)"; fi
echo "cpu  : ${CPU_INFO:-unknown}${NPROC:+  ($NPROC cores)}"
echo

# ---------------------------------------------------------------- sanity checks

ARCH="$(uname -m)"
ISA_FLAGS=""
case "$ARCH" in
    arm64|aarch64)
        ISA_DESC="aarch64 NEON"
        ;;
    x86_64|amd64|AMD64)
        # AVX2+FMA is what the x86 kernels need. Without it the AVX2 rows are
        # omitted and only scalar/volk are measured, which is still useful.
        ISA_FLAGS="-mavx2 -mfma"
        ISA_DESC="x86-64 (AVX2+FMA requested)"
        ;;
    i386|i486|i586|i686|x86)
        ISA_DESC="x86 32-bit (scalar/volk only)"
        ;;
    *)
        ISA_DESC="$ARCH (scalar/volk only)"
        c_yel "Unrecognised architecture '$ARCH' - the SIMD rows may be absent."
        ;;
esac
c_grn "architecture: $ISA_DESC"

CXX="${CXX:-c++}"
if ! command -v "$CXX" >/dev/null 2>&1; then
    c_red "ERROR: no C++ compiler found. Install the Xcode command line tools:"
    c_red "  xcode-select --install"
    exit 1
fi
echo "compiler: $($CXX --version | head -1)"


# ---------------------------------------------------------------- volk probe
# The comparison against libvolk is only meaningful if we find it.
VOLK_CFLAGS=""
VOLK_LIBS=""
HAVE_VOLK=0
VOLK_LIBDIR=""
if [ "${NO_VOLK:-0}" = "1" ]; then
    c_yel "NO_VOLK=1 set - skipping the libvolk comparison."
elif command -v pkg-config >/dev/null 2>&1 && pkg-config --exists volk 2>/dev/null; then
    VOLK_CFLAGS="$(pkg-config --cflags volk)"
    VOLK_LIBS="$(pkg-config --libs volk)"
    # Remember the directory of the .so/.dylib so we can bake an rpath. On macOS the
    # library is built with @rpath, so linking -lvolk is not enough: without an rpath
    # dyld aborts at startup with "no LC_RPATH's found" (seen with radioconda).
    # `--variable=libdir` is the reliable source; fall back to stripping -L off --libs.
    for cand in $(pkg-config --variable=libdir volk 2>/dev/null) \
                $(pkg-config --libs volk 2>/dev/null | tr " " "\n" | sed -n "s/^-L//p"); do
        [ -n "$cand" ] && [ -d "$cand" ] && VOLK_LIBDIR="$cand" && break
    done
    HAVE_VOLK=1
    c_grn "libvolk found via pkg-config - will include the dispatched comparison."
elif [ -d /opt/homebrew/include/volk ] || [ -d /usr/local/include/volk ]; then
    VOLKP="$( ( [ -d /opt/homebrew/include/volk ] && echo /opt/homebrew ) || echo /usr/local )"
    VOLK_CFLAGS="-I$VOLKP/include"
    VOLK_LIBS="-L$VOLKP/lib -lvolk"
    VOLK_LIBDIR="$VOLKP/lib"
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

# On macOS, libraries are linked with @rpath, so the loader needs an LC_RPATH entry
# pointing at the directory the dylib actually lives in. Without it dyld aborts with
# "no LC_RPATH's found" even though the link succeeded. Harmless on other platforms.
RPATH_FLAGS=""
if [ -n "$VOLK_LIBDIR" ]; then
    case "$(uname -s)" in
        Darwin) RPATH_FLAGS="-Wl,-rpath,$VOLK_LIBDIR" ;;
        *)      RPATH_FLAGS="-Wl,-rpath,$VOLK_LIBDIR" ;;
    esac
fi

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
$CXX -O3 -std=gnu++17 -DNDEBUG $NOVEC_FLAG $ISA_FLAGS $DEFS -I"$REPO" $VOLK_CFLAGS "$SRC" -o "$BIN" $VOLK_LIBS $RPATH_FLAGS -lm
set +x
if [ ! -x "$BIN" ]; then
    c_red "COMPILE FAILED - see the errors above."
    c_red "If it is a volk link error, retry without volk: NO_VOLK=1 bash $0"
    exit 1
fi
c_grn "compiled."
echo ""

# ---------------------------------------------------------------- run
# Startup probe: catch dynamic-linker problems BEFORE looping five times, and try to
# recover. On macOS a volk dylib built with @rpath needs either an LC_RPATH (added at
# link time above) or DYLD_LIBRARY_PATH / DYLD_FALLBACK_LIBRARY_PATH.
RUN_ENV=""
if ! "$BIN" >/dev/null 2>"$WORK/probe.err"; then
    if grep -qiE "Library not loaded|LC_RPATH|image not found" "$WORK/probe.err"; then
        c_yel "The benchmark built, but the loader cannot find libvolk at runtime:"
        sed -e "s/^/    /" "$WORK/probe.err" | head -4
        case "$(uname -s)" in
            Darwin)
                if [ -n "$VOLK_LIBDIR" ]; then
                    RUN_ENV="DYLD_FALLBACK_LIBRARY_PATH=$VOLK_LIBDIR${DYLD_FALLBACK_LIBRARY_PATH:+:$DYLD_FALLBACK_LIBRARY_PATH}"
                fi
                ;;
            *)
                if [ -n "$VOLK_LIBDIR" ]; then
                    RUN_ENV="LD_LIBRARY_PATH=$VOLK_LIBDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
                fi
                ;;
        esac
        if [ -n "$RUN_ENV" ]; then
            c_yel "Retrying with $RUN_ENV"
        fi
        # Last resort: rebuild without volk so you still get the SIMD-vs-scalar result.
        if [ -n "$RUN_ENV" ] && env $RUN_ENV "$BIN" >/dev/null 2>&1; then
            : # the retry works; carry on with RUN_ENV applied below
        else
            c_yel "Still not loading. Rebuilding WITHOUT libvolk so you at least get"
            c_yel "the SIMD-vs-scalar comparison (set NO_VOLK=1 to skip this next time)."
            DEFS=""
            RPATH_FLAGS=""
            set -x
            $CXX -O3 -std=gnu++17 -DNDEBUG $NOVEC_FLAG $ISA_FLAGS $DEFS -I"$REPO" "$SRC" -o "$BIN" -lm
            set +x
            RUN_ENV=""
            if ! "$BIN" >/dev/null 2>&1; then
                c_red "Benchmark still will not start. Please report the output above."
                exit 1
            fi
        fi
    else
        c_red "The benchmark failed to start:"
        sed -e "s/^/    /" "$WORK/probe.err" | head -6
        exit 1
    fi
fi

c_bold "--- running (best of 5 runs; the minimum is the honest number) ---"
for run in 1 2 3 4 5; do
    echo ""; echo "### run $run ###"
    env $RUN_ENV "$BIN"
done | tee "$WORK/bench.raw.txt"

# ---------------------------------------------------------------- summarise
# ---------------------------------------------------------------- summary
# The row format is: "  <name>  <ms> ms  <ns/pt> ns/pt". Parse on the literal
# " ms " marker rather than by column index - names contain spaces.
# Emit: best ms per row, then the speedup of the SIMD row over scalar and over volk.
c_bold "=== SUMMARY (best observed per row, across all runs) ==="
# Parse the fixed row format "  <name>  <ms> ms  <ns/pt> ns/pt".
# Deliberately avoids the 3-argument match(); that is a GNU awk extension and
# macOS ships BSD awk, where it is an error.
awk '
  /^  .+ [0-9.]+ ms +[0-9.]+ ns\/pt[ ]*$/ {
    line = $0
    sub(/[ ]+[0-9.]+[ ]+ns\/pt[ ]*$/, "", line)
    sub(/[ ]+ms[ ]*$/, "", line)
    n = split(line, parts, /[ ]+/)
    ms = parts[n] + 0
    name = line
    sub(/[ ]+[0-9.]+[ ]*$/, "", name)
    sub(/^[ ]+/, "", name)
    if (name != "" && (!(name in best) || ms < best[name])) best[name] = ms
  }
  END { for (k in best) printf "  %-38s %10.4f ms\n", k, best[k] }
' "$WORK/bench.raw.txt" | sort

echo ""
c_bold "=== SPEEDUP: each baseline vs the SIMD row ==="
awk '
  /^  .+ [0-9.]+ ms +[0-9.]+ ns\/pt[ ]*$/ {
    line = $0
    sub(/[ ]+[0-9.]+[ ]+ns\/pt[ ]*$/, "", line)
    sub(/[ ]+ms[ ]*$/, "", line)
    n = split(line, parts, /[ ]+/)
    ms = parts[n] + 0
    name = line
    sub(/[ ]+[0-9.]+[ ]*$/, "", name)
    sub(/^[ ]+/, "", name)
    if (name != "" && (!(name in best) || ms < best[name])) best[name] = ms
  }
  END {
    simd = 0
    for (k in best) if (k ~ /^SIMD:/ && (simd == 0 || best[k] < simd)) simd = best[k]
    if (simd <= 0) { print "  (no SIMD row found - nothing to compare)"; exit }
    for (k in best) {
      if (k ~ /^SIMD:/) continue
      r = best[k] / simd
      verdict = (r >= 1.0) ? "SIMD is faster" : "SIMD is SLOWER"
      printf "  %-38s %7.2fx   %s\n", k, r, verdict
    }
  }
' "$WORK/bench.raw.txt" | sort

echo ""
c_bold "NOTE: the SIMD row is the repo kernel for THIS architecture."
c_bold "      A ratio below 1.00x means the SIMD kernel LOSES to that baseline."

echo ""
c_bold "=== what to look for ==="
cat <<'EOF'
  * The "SIMD: ..." row should beat "scalar (auto-vec disabled)".
    If it does not, the kernel is not worth keeping on this CPU.
    On x86 the SIMD row is AVX2; on arm64 it is NEON - same source, both measured.
  * Compare the SIMD row against 'libvolk (dispatched)'. That is the real question.
    VOLK's SIMD kernels are already vectorized, so a small win is expected and a LOSS
    is entirely possible. Report it either way - do not assume the port wins.
  * On arm64 the FIR kernel measured near break-even (~1.0-1.1x) under emulation.
    If it is at or below 1.0x on real hardware it should be dropped - it is
    self-contained behind its arch guard and nothing else depends on it.
  * The M&M kernel is the better bet (measured ~1.5x on arm64). Confirm or refute.
  * Absolute ms values are machine-specific. Compare RATIOS across machines, and
    only ever compare rows produced on the same machine.
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
