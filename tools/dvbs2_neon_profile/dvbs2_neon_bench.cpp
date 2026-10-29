// dvbs2_neon_bench.cpp - measures the DVB-S2 DSP kernels on ANY platform.
// Part of tools/dvbs2_neon_profile/. Generated/maintained by hand; see the .sh wrapper.
//
// It compiles the EXACT kernel bodies from fir.cpp and clock_recovery_mm.cpp for whichever
// ISA it is built on, so the same source produces meaningful numbers on:
//   - Apple Silicon  -> NEON kernels vs scalar vs libvolk
//   - x86-64         -> AVX2 kernels vs scalar vs libvolk (+ NEON rows are absent)
//   - other / 32-bit -> scalar vs libvolk only
//
// Methodology notes (these matter for trustworthy numbers):
//  * best-of-N, not mean. The minimum is the least noisy estimator on a laptop.
//  * a volatile sink consumes every result so nothing is dead-code eliminated.
//  * the scalar baseline is compiled with auto-vectorization DISABLED via a build flag,
//    otherwise the compiler vectorizes it at -O3 and the comparison is meaningless.
//  * buffers are 64-byte aligned and over-allocated: VOLK loads whole vectors and the
//    kernels read up to n+ntaps past their nominal end.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <chrono>
#include <string>

#if defined(__aarch64__)
#include <arm_neon.h>
#define BENCH_HAVE_NEON 1
#elif (defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86))
#include <immintrin.h>
#if defined(__AVX2__)
#define BENCH_HAVE_AVX2 1
#endif
#endif

#if defined(HAVE_VOLK)
#include <volk/volk.h>
#endif

static volatile double g_sink = 0.0;

static void *aligned_alloc64(size_t bytes)
{
    void *p = nullptr;
#if defined(_MSC_VER)
    p = _aligned_malloc(bytes, 64);
#else
    if (posix_memalign(&p, 64, bytes) != 0) p = nullptr;
#endif
    if (!p) { fprintf(stderr, "aligned alloc failed\n"); exit(1); }
    memset(p, 0, bytes);
    return p;
}
static void aligned_free(void *p)
{
#if defined(_MSC_VER)
    _aligned_free(p);
#else
    free(p);
#endif
}

template <typename F>
static double best_ms(F fn, int reps)
{
    double best = 1e300;
    fn(); // warmup: fault in pages, prime caches and the branch predictor
    for (int r = 0; r < reps; r++)
    {
        const auto t0 = std::chrono::steady_clock::now();
        fn();
        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        if (ms < best) best = ms;
    }
    return best;
}

static void row(const char *name, double ms, double ns_per_pt)
{
    printf("  %-36s %9.3f ms  %9.2f ns/pt\n", name, ms, ns_per_pt);
}

// =====================================================================================
// M&M: complex x real 8-tap dot - EXACT bodies from clock_recovery_mm.cpp
// =====================================================================================
#if defined(BENCH_HAVE_NEON)
static inline void mm_dot8_neon(const float *o, const float *t, float *out)
{
    const float32x4x2_t a = vld2q_f32(o);
    const float32x4x2_t b = vld2q_f32(o + 8);
    const float32x4_t t0 = vld1q_f32(t);
    const float32x4_t t1 = vld1q_f32(t + 4);
    const float32x4_t re = vfmaq_f32(vmulq_f32(a.val[0], t0), b.val[0], t1);
    const float32x4_t im = vfmaq_f32(vmulq_f32(a.val[1], t0), b.val[1], t1);
    float rr[4], ii[4];
    vst1q_f32(rr, re);
    vst1q_f32(ii, im);
    out[0] = (rr[0] + rr[1]) + (rr[2] + rr[3]);
    out[1] = (ii[0] + ii[1]) + (ii[2] + ii[3]);
}
#endif

#if defined(BENCH_HAVE_AVX2)
static inline void mm_dot8_avx2(const float *a, const float *tp, float *out)
{
    const __m256i idx_lo = _mm256_setr_epi32(0, 0, 1, 1, 2, 2, 3, 3);
    const __m256i idx_hi = _mm256_setr_epi32(4, 4, 5, 5, 6, 6, 7, 7);
    const __m256 v0 = _mm256_loadu_ps(a);
    const __m256 v1 = _mm256_loadu_ps(a + 8);
    const __m256 t = _mm256_loadu_ps(tp);
    const __m256 td0 = _mm256_permutevar8x32_ps(t, idx_lo);
    const __m256 td1 = _mm256_permutevar8x32_ps(t, idx_hi);
    const __m256 re = _mm256_add_ps(_mm256_mul_ps(_mm256_moveldup_ps(v0), td0),
                                  _mm256_mul_ps(_mm256_moveldup_ps(v1), td1));
    const __m256 im = _mm256_add_ps(_mm256_mul_ps(_mm256_movehdup_ps(v0), td0),
                                  _mm256_mul_ps(_mm256_movehdup_ps(v1), td1));
    __m128 r = _mm_add_ps(_mm256_castps256_ps128(re), _mm256_extractf128_ps(re, 1));
    __m128 i = _mm_add_ps(_mm256_castps256_ps128(im), _mm256_extractf128_ps(im, 1));
    r = _mm_hadd_ps(r, r); r = _mm_hadd_ps(r, r);
    i = _mm_hadd_ps(i, i); i = _mm_hadd_ps(i, i);
    out[0] = _mm_cvtss_f32(r) * 0.5f;
    out[1] = _mm_cvtss_f32(i) * 0.5f;
}
#endif

// Scalar reference (see the build flag note at the top of this file).
static __attribute__((noinline)) void mm_dot8_scalar(const float *o, const float *t, float *out)
{
    float re = 0.f, im = 0.f;
    for (int i = 0; i < 8; i++)
    {
        re += o[2 * i + 0] * t[i];
        im += o[2 * i + 1] * t[i];
    }
    out[0] = re;
    out[1] = im;
}

// =====================================================================================
// FIR: split interleaved -> re/im, then complex filter - EXACT bodies from fir.cpp
// =====================================================================================
#if defined(BENCH_HAVE_NEON)
static inline void fir_split8_neon(const float *__restrict src, float *__restrict re, float *__restrict im, int n)
{
    int j = 0;
    for (; j + 8 <= n; j += 8)
    {
        const float32x4x2_t a = vld2q_f32(src + 2 * j);
        const float32x4x2_t b = vld2q_f32(src + 2 * j + 8);
        vst1q_f32(re + j, a.val[0]);
        vst1q_f32(re + j + 4, b.val[0]);
        vst1q_f32(im + j, a.val[1]);
        vst1q_f32(im + j + 4, b.val[1]);
    }
    for (; j < n; j++) { re[j] = src[2 * j]; im[j] = src[2 * j + 1]; }
}

static inline void fir_complex_blocked_neon(const float *__restrict re, const float *__restrict im,
                                            const float *__restrict h, int n, int ntaps, float *__restrict out)
{
    int i = 0;
    for (; i + 8 <= n; i += 8)
    {
        float32x4_t ar0 = vdupq_n_f32(0), ar1 = vdupq_n_f32(0), ar2 = vdupq_n_f32(0), ar3 = vdupq_n_f32(0);
        float32x4_t ar4 = vdupq_n_f32(0), ar5 = vdupq_n_f32(0), ar6 = vdupq_n_f32(0), ar7 = vdupq_n_f32(0);
        float32x4_t ai0 = vdupq_n_f32(0), ai1 = vdupq_n_f32(0), ai2 = vdupq_n_f32(0), ai3 = vdupq_n_f32(0);
        float32x4_t ai4 = vdupq_n_f32(0), ai5 = vdupq_n_f32(0), ai6 = vdupq_n_f32(0), ai7 = vdupq_n_f32(0);
        int m = 0;
        for (; m + 4 <= ntaps; m += 4)
        {
            const float32x4_t h0 = vdupq_n_f32(h[m + 0]);
            const float32x4_t h1 = vdupq_n_f32(h[m + 1]);
            const float32x4_t h2 = vdupq_n_f32(h[m + 2]);
            const float32x4_t h3 = vdupq_n_f32(h[m + 3]);
            ar0 = vfmaq_f32(ar0, h0, vld1q_f32(re + i + m + 0));
            ar1 = vfmaq_f32(ar1, h1, vld1q_f32(re + i + m + 1));
            ar2 = vfmaq_f32(ar2, h2, vld1q_f32(re + i + m + 2));
            ar3 = vfmaq_f32(ar3, h3, vld1q_f32(re + i + m + 3));
            ar4 = vfmaq_f32(ar4, h0, vld1q_f32(re + i + m + 4));
            ar5 = vfmaq_f32(ar5, h1, vld1q_f32(re + i + m + 5));
            ar6 = vfmaq_f32(ar6, h2, vld1q_f32(re + i + m + 6));
            ar7 = vfmaq_f32(ar7, h3, vld1q_f32(re + i + m + 7));
            ai0 = vfmaq_f32(ai0, h0, vld1q_f32(im + i + m + 0));
            ai1 = vfmaq_f32(ai1, h1, vld1q_f32(im + i + m + 1));
            ai2 = vfmaq_f32(ai2, h2, vld1q_f32(im + i + m + 2));
            ai3 = vfmaq_f32(ai3, h3, vld1q_f32(im + i + m + 3));
            ai4 = vfmaq_f32(ai4, h0, vld1q_f32(im + i + m + 4));
            ai5 = vfmaq_f32(ai5, h1, vld1q_f32(im + i + m + 5));
            ai6 = vfmaq_f32(ai6, h2, vld1q_f32(im + i + m + 6));
            ai7 = vfmaq_f32(ai7, h3, vld1q_f32(im + i + m + 7));
        }
        for (; m < ntaps; m++)
        {
            const float32x4_t h0 = vdupq_n_f32(h[m]);
            ar0 = vfmaq_f32(ar0, h0, vld1q_f32(re + i + m));
            ar4 = vfmaq_f32(ar4, h0, vld1q_f32(re + i + m + 4));
            ai0 = vfmaq_f32(ai0, h0, vld1q_f32(im + i + m));
            ai4 = vfmaq_f32(ai4, h0, vld1q_f32(im + i + m + 4));
        }
        const float32x4_t ar_lo = vaddq_f32(vaddq_f32(ar0, ar1), vaddq_f32(ar2, ar3));
        const float32x4_t ar_hi = vaddq_f32(vaddq_f32(ar4, ar5), vaddq_f32(ar6, ar7));
        const float32x4_t ai_lo = vaddq_f32(vaddq_f32(ai0, ai1), vaddq_f32(ai2, ai3));
        const float32x4_t ai_hi = vaddq_f32(vaddq_f32(ai4, ai5), vaddq_f32(ai6, ai7));
        // Four separate stores: ar_lo/ai_lo hold outputs 0-3, ar_hi/ai_hi hold 4-7.
        vst1q_f32(out + 2 * i + 0, vzip1q_f32(ar_lo, ai_lo));
        vst1q_f32(out + 2 * i + 4, vzip2q_f32(ar_lo, ai_lo));
        vst1q_f32(out + 2 * i + 8, vzip1q_f32(ar_hi, ai_hi));
        vst1q_f32(out + 2 * i + 12, vzip2q_f32(ar_hi, ai_hi));
    }
    for (; i < n; i++)
    {
        float re_s = 0.f, im_s = 0.f;
        for (int m = 0; m < ntaps; m++) { re_s += re[i + m] * h[m]; im_s += im[i + m] * h[m]; }
        out[2 * i + 0] = re_s; out[2 * i + 1] = im_s;
    }
}
#endif

#if defined(BENCH_HAVE_AVX2)
static inline void fir_split8_avx2(const float *__restrict src, float *__restrict re, float *__restrict im, int n)
{
    const __m256i idx = _mm256_setr_epi32(0, 1, 4, 5, 2, 3, 6, 7);
    int j = 0;
    for (; j + 8 <= n; j += 8)
    {
        const __m256 v0 = _mm256_loadu_ps(src + 2 * j);
        const __m256 v1 = _mm256_loadu_ps(src + 2 * j + 8);
        const __m256 t0 = _mm256_shuffle_ps(v0, v1, _MM_SHUFFLE(2, 0, 2, 0));
        const __m256 t1 = _mm256_shuffle_ps(v0, v1, _MM_SHUFFLE(3, 1, 3, 1));
        _mm256_storeu_ps(re + j, _mm256_permutevar8x32_ps(t0, idx));
        _mm256_storeu_ps(im + j, _mm256_permutevar8x32_ps(t1, idx));
    }
    for (; j < n; j++) { re[j] = src[2 * j]; im[j] = src[2 * j + 1]; }
}

static inline void fir_complex_blocked_avx2(const float *__restrict re, const float *__restrict im,
                                            const float *__restrict h, int n, int ntaps, float *__restrict out)
{
    int i = 0;
    for (; i + 8 <= n; i += 8)
    {
        __m256 ar0 = _mm256_setzero_ps(), ar1 = _mm256_setzero_ps(), ar2 = _mm256_setzero_ps(), ar3 = _mm256_setzero_ps();
        __m256 ai0 = _mm256_setzero_ps(), ai1 = _mm256_setzero_ps(), ai2 = _mm256_setzero_ps(), ai3 = _mm256_setzero_ps();
        int m = 0;
        for (; m + 4 <= ntaps; m += 4)
        {
            const __m256 h0 = _mm256_set1_ps(h[m + 0]);
            const __m256 h1 = _mm256_set1_ps(h[m + 1]);
            const __m256 h2 = _mm256_set1_ps(h[m + 2]);
            const __m256 h3 = _mm256_set1_ps(h[m + 3]);
            ar0 = _mm256_fmadd_ps(h0, _mm256_loadu_ps(re + i + m + 0), ar0);
            ar1 = _mm256_fmadd_ps(h1, _mm256_loadu_ps(re + i + m + 1), ar1);
            ar2 = _mm256_fmadd_ps(h2, _mm256_loadu_ps(re + i + m + 2), ar2);
            ar3 = _mm256_fmadd_ps(h3, _mm256_loadu_ps(re + i + m + 3), ar3);
            ai0 = _mm256_fmadd_ps(h0, _mm256_loadu_ps(im + i + m + 0), ai0);
            ai1 = _mm256_fmadd_ps(h1, _mm256_loadu_ps(im + i + m + 1), ai1);
            ai2 = _mm256_fmadd_ps(h2, _mm256_loadu_ps(im + i + m + 2), ai2);
            ai3 = _mm256_fmadd_ps(h3, _mm256_loadu_ps(im + i + m + 3), ai3);
        }
        for (; m < ntaps; m++)
        {
            const __m256 h0 = _mm256_set1_ps(h[m]);
            ar0 = _mm256_fmadd_ps(h0, _mm256_loadu_ps(re + i + m), ar0);
            ai0 = _mm256_fmadd_ps(h0, _mm256_loadu_ps(im + i + m), ai0);
        }
        const __m256 ar = _mm256_add_ps(_mm256_add_ps(ar0, ar1), _mm256_add_ps(ar2, ar3));
        const __m256 ai = _mm256_add_ps(_mm256_add_ps(ai0, ai1), _mm256_add_ps(ai2, ai3));
        const __m256 lo = _mm256_unpacklo_ps(ar, ai);
        const __m256 hi = _mm256_unpackhi_ps(ar, ai);
        _mm256_storeu_ps(out + 2 * i, _mm256_permute2f128_ps(lo, hi, 0x20));
        _mm256_storeu_ps(out + 2 * i + 8, _mm256_permute2f128_ps(lo, hi, 0x31));
    }
    for (; i < n; i++)
    {
        float r = 0, q = 0;
        for (int m = 0; m < ntaps; m++) { r += h[m] * re[i + m]; q += h[m] * im[i + m]; }
        out[2 * i] = r; out[2 * i + 1] = q;
    }
}
#endif

static __attribute__((noinline)) void fir_split_scalar(const float *src, float *re, float *im, int n)
{
    for (int j = 0; j < n; j++) { re[j] = src[2 * j]; im[j] = src[2 * j + 1]; }
}

static __attribute__((noinline)) void fir_complex_scalar(const float *re, const float *im,
                                                         const float *h, int n, int ntaps, float *out)
{
    for (int i = 0; i < n; i++)
    {
        float re_s = 0.f, im_s = 0.f;
        for (int m = 0; m < ntaps; m++) { re_s += re[i + m] * h[m]; im_s += im[i + m] * h[m]; }
        out[2 * i + 0] = re_s; out[2 * i + 1] = im_s;
    }
}

// =====================================================================================
int main(int argc, char **argv)
{
    const int REPS = getenv("BENCH_REPS") ? atoi(getenv("BENCH_REPS")) : 200;
    const int N    = getenv("BENCH_N")    ? atoi(getenv("BENCH_N"))    : (1 << 20);

    printf("\n=== DVB-S2 DSP kernel profile ===\n");
#if defined(BENCH_HAVE_NEON)
    printf("ISA            : aarch64 NEON\n");
#elif defined(BENCH_HAVE_AVX2)
    printf("ISA            : x86-64 AVX2 + FMA\n");
#elif defined(__x86_64__) || defined(_M_X64)
    printf("ISA            : x86-64 (built WITHOUT -mavx2/-mfma: AVX2 rows absent)\n");
#else
    printf("ISA            : portable (no SIMD kernel available for this target)\n");
#endif
    printf("points/call    : %d\n", N);
    printf("reps           : %d (reported figure is the best of these)\n", REPS);
#if defined(HAVE_VOLK)
    {
        const char *m = volk_get_machine();
        printf("VOLK machine   : %s\n", m ? m : "(null)");
        printf("VOLK alignment : %zu\n", (size_t)volk_get_alignment());
    }
#else
    printf("VOLK           : not compiled in (build with -DHAVE_VOLK to compare)\n");
#endif
    printf("NOTE           : compare only rows from the SAME machine. Absolute times are\n");
    printf("                 not comparable across CPUs/OSes; the ratios are.\n");

    // ---------------- M&M 8-tap complex x real dot -------------------------
    {
        const int ntaps = 8;
        float *o   = (float *)aligned_alloc64(sizeof(float) * (2 * N + 2 * ntaps + 16));
        float *t   = (float *)aligned_alloc64(sizeof(float) * (ntaps + 8));
        float *out = (float *)aligned_alloc64(sizeof(float) * 4);
        for (int i = 0; i < 2 * N + 2 * ntaps + 16; i++) o[i] = (float)sin(0.001 * i);
        for (int i = 0; i < ntaps; i++) t[i] = (float)cos(0.01 * i);

        printf("\n--- M&M interpolating dot, %d taps (%d points/call) ---\n", ntaps, N);

        // Reference: accumulate the scalar kernel over the SAME positions the SIMD
        // loops cover, so the checksums are directly comparable.
        double sc = 0;
        for (int i = 0; i + 2 * ntaps + 2 < 2 * N; i += 2) { mm_dot8_scalar(o + i, t, out); sc += out[0] + out[1]; }

#if defined(BENCH_HAVE_NEON) || defined(BENCH_HAVE_AVX2)
        {
            // Capture exactly ONE pass (best_ms runs warmup+REPS; do not read g_sink).
            double acc1 = 0;
            const double ms = best_ms([&]() {
                double acc = 0;
                for (int i = 0; i + 2 * ntaps + 2 < 2 * N; i += 2) {
#if defined(BENCH_HAVE_NEON)
                    mm_dot8_neon(o + i, t, out);
#else
                    mm_dot8_avx2(o + i, t, out);
#endif
                    acc += out[0] + out[1];
                }
                acc1 = acc; g_sink += acc;
            }, REPS);
#if defined(BENCH_HAVE_NEON)
            row("SIMD: NEON (repo kernel)", ms, ms * 1e6 / N);
#else
            row("SIMD: AVX2 (repo kernel)", ms, ms * 1e6 / N);
#endif
            {
                const double denom = (sc != 0.0) ? fabs(sc) : 1.0;
                printf("       agrees with scalar to %.3g relative (SIMD %.6f vs scalar %.6f)\n",
                       fabs(acc1 - sc) / denom, acc1, sc);
            }
        }
#endif
        {
            const double ms = best_ms([&]() {
                double acc = 0;
                for (int i = 0; i + 2 * ntaps + 2 < 2 * N; i += 2) { mm_dot8_scalar(o + i, t, out); acc += out[0] + out[1]; }
                g_sink += acc;
            }, REPS);
            row("scalar (auto-vec disabled)", ms, ms * 1e6 / N);
        }
#if defined(HAVE_VOLK)
        {
            const double ms = best_ms([&]() {
                double acc = 0;
                lv_32fc_t res = 0;
                for (int i = 0; i + 2 * ntaps + 2 < 2 * N; i += 2) {
                    volk_32fc_32f_dot_prod_32fc(&res, (const lv_32fc_t *)(o + i), t, ntaps);
                    acc += lv_creal(res) + lv_cimag(res);
                }
                g_sink += acc;
            }, REPS);
            row("libvolk (dispatched)", ms, ms * 1e6 / N);
        }
#endif
        aligned_free(o); aligned_free(t); aligned_free(out);
    }

    // ---------------- FIR complex filter -----------------------------------
    const int tapset[] = {31, 63};
    for (int ti = 0; ti < 2; ti++)
    {
        const int ntaps = tapset[ti];
        printf("\n--- Complex FIR, %d taps (%d output points/call) ---\n", ntaps, N);
        float *src = (float *)aligned_alloc64(sizeof(float) * (2 * (N + ntaps + 16)));
        float *re  = (float *)aligned_alloc64(sizeof(float) * (N + ntaps + 16));
        float *im  = (float *)aligned_alloc64(sizeof(float) * (N + ntaps + 16));
        float *h   = (float *)aligned_alloc64(sizeof(float) * (ntaps + 8));
        float *out = (float *)aligned_alloc64(sizeof(float) * (2 * (N + ntaps + 16)));
        float *ref = (float *)aligned_alloc64(sizeof(float) * (2 * (N + ntaps + 16)));
        for (int i = 0; i < 2 * (N + ntaps + 16); i++) src[i] = (float)sin(0.0007 * i);
        for (int i = 0; i < ntaps; i++) h[i] = (float)(cos(0.02 * i) / (1.0 + i));

        // reference output for the SIMD-vs-scalar agreement check
        fir_split_scalar(src, re, im, N);
        fir_complex_scalar(re, im, h, N, ntaps, ref);

#if defined(BENCH_HAVE_NEON) || defined(BENCH_HAVE_AVX2)
        {
            const double ms = best_ms([&]() {
#if defined(BENCH_HAVE_NEON)
                fir_split8_neon(src, re, im, N);
                fir_complex_blocked_neon(re, im, h, N, ntaps, out);
#else
                fir_split8_avx2(src, re, im, N);
                fir_complex_blocked_avx2(re, im, h, N, ntaps, out);
#endif
                g_sink += out[0] + out[2 * N - 1];
            }, REPS);
#if defined(BENCH_HAVE_NEON)
            row("SIMD: NEON (repo kernel)", ms, ms * 1e6 / N);
#else
            row("SIMD: AVX2 (repo kernel)", ms, ms * 1e6 / N);
#endif
            double worst = 0;
            for (int k = 0; k < 2 * N; k++) {
                const double d = fabs(ref[k]) > 1e-12 ? fabs(out[k] - ref[k]) / fabs(ref[k]) : fabs(out[k]);
                if (d > worst) worst = d;
            }
            printf("       max relative difference vs scalar: %.3g\n", worst);
        }
#endif
        {
            const double ms = best_ms([&]() {
                fir_split_scalar(src, re, im, N);
                fir_complex_scalar(re, im, h, N, ntaps, out);
                g_sink += out[0] + out[2 * N - 1];
            }, REPS);
            row("scalar (auto-vec disabled)", ms, ms * 1e6 / N);
        }
#if defined(HAVE_VOLK)
        {
            const double ms = best_ms([&]() {
                fir_split_scalar(src, re, im, N);
                lv_32fc_t acc;
                float taps2[128 * 2];
                for (int m = 0; m < ntaps; m++) { taps2[2 * m] = h[m]; taps2[2 * m + 1] = 0.f; }
                for (int i = 0; i < N; i++) {
                    volk_32fc_x2_dot_prod_32fc(&acc, (const lv_32fc_t *)(re + i),
                                               (const lv_32fc_t *)taps2, ntaps);
                    out[2 * i] = lv_creal(acc);
                    out[2 * i + 1] = lv_cimag(acc);
                }
                g_sink += out[0] + out[2 * N - 1];
            }, REPS);
            row("libvolk dot (dispatched)", ms, ms * 1e6 / N);
        }
#endif
        aligned_free(src); aligned_free(re); aligned_free(im);
        aligned_free(h); aligned_free(out); aligned_free(ref);
    }

    printf("\n(sink %.6f - printed only to defeat dead-code elimination)\n", g_sink);
    return 0;
}