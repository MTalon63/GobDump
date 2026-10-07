#include "fir.h"
#include <cstring>
#include <volk/volk.h>
#include <algorithm>

#if (defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)) && defined(DSP_HAVE_AVX2_ISA)
#include <immintrin.h>
#define FIR_HAVE_AVX2_KERNEL 1
#endif

#ifdef FIR_HAVE_AVX2_KERNEL
namespace
{
    static inline void fir_split8(const float *__restrict src, float *__restrict re, float *__restrict im, int n)
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
        for (; j < n; j++)
        {
            re[j] = src[2 * j];
            im[j] = src[2 * j + 1];
        }
    }

    // 8 complex outputs per iteration so the sample window is reused instead of re-walked by VOLK.
    // __restrict matters: without it the store is assumed to alias the re/im window, serialising it.
    static inline void fir_complex_blocked(const float *__restrict re, const float *__restrict im, const float *__restrict h, int n, int ntaps, float *__restrict out)
    {
        int i = 0;
        for (; i + 8 <= n; i += 8)
        {
            // Four partial accumulators per component: a single one is a 31-deep serial FMA chain,
            // latency-bound at ~15 cycles/output; splitting it makes the loop throughput-bound.
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
            for (int m = 0; m < ntaps; m++)
            {
                r += h[m] * re[i + m];
                q += h[m] * im[i + m];
            }
            out[2 * i] = r;
            out[2 * i + 1] = q;
        }
    }
}
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846 /* pi */
#endif

namespace dsp
{
    template <typename T>
    FIRBlock<T>::FIRBlock(std::shared_ptr<dsp::stream<T>> input, std::vector<float> taps) : Block<T, T>(input)
    {
        // Get alignement parameters
        align = volk_get_alignment();
        aligned_tap_count = std::max<int>(1, align / sizeof(T));

        // Set taps
        ntaps = taps.size();

        // Init taps
        this->taps = (float **)volk_malloc(aligned_tap_count * sizeof(float *), align);
        for (int i = 0; i < aligned_tap_count; i++)
        {
            this->taps[i] = (float *)volk_malloc((ntaps + aligned_tap_count - 1) * sizeof(float), align);
            for (int y = 0; y < ntaps + aligned_tap_count - 1; y++)
                this->taps[i][y] = 0;
            for (int j = 0; j < ntaps; j++)
                this->taps[i][i + j] = taps[(ntaps - 1) - j]; // Reverse taps
        }

        // Init buffer
        buffer = create_volk_buffer<T>(2 * STREAM_BUFFER_SIZE);

#ifdef FIR_HAVE_AVX2_KERNEL
        if constexpr (std::is_same_v<T, complex_t>)
            fir_scratch = (float *)volk_malloc(2 * (FIR_CHUNK + ntaps) * sizeof(float), align);
#endif
    }

    template <typename T>
    FIRBlock<T>::~FIRBlock()
    {
        for (int i = 0; i < aligned_tap_count; i++)
            volk_free(taps[i]);
        volk_free(taps);
        volk_free(buffer);
        if (fir_scratch)
            volk_free(fir_scratch);
    }

    template <typename T>
    void FIRBlock<T>::work()
    {
        int nsamples = Block<T, T>::input_stream->read();
        if (nsamples <= 0)
        {
            Block<T, T>::input_stream->flush();
            return;
        }

        memcpy(&buffer[ntaps], Block<T, T>::input_stream->readBuf, nsamples * sizeof(T));
        Block<T, T>::input_stream->flush();

        if constexpr (std::is_same_v<T, float>)
        {
            for (int i = 0; i < nsamples; i++)
            {
                // Doing it this way instead of the normal :
                // volk_32f_x2_dot_prod_32f(&output_stream->writeBuf[i], &buffer[i + 1], taps, ntaps);
                // turns out to enhance performances quite a bit... Initially tested this after noticing
                // inconsistencies between the above simple way and GNU Radio's implementation
                const float *ar = (float *)((size_t)&buffer[i + 1] & ~(align - 1));
                const unsigned al = &buffer[i + 1] - ar;
                volk_32f_x2_dot_prod_32f_a(&Block<T, T>::output_stream->writeBuf[i], ar, taps[al], ntaps + al);
            }
        }
        if constexpr (std::is_same_v<T, complex_t>)
        {
#ifdef FIR_HAVE_AVX2_KERNEL
            // Same sum as the fallback below (taps[0] reversed row; "al" offsets are equivalent).
            if (fir_scratch && ntaps >= 4)
            {
                for (int base = 0; base < nsamples; base += FIR_CHUNK)
                {
                    const int n = std::min(FIR_CHUNK, nsamples - base);
                    const int navail = n + ntaps - 1;
                    float *re = fir_scratch;
                    float *im = fir_scratch + navail;
                    fir_split8((const float *)&buffer[1 + base], re, im, navail);
                    fir_complex_blocked(re, im, taps[0], n, ntaps, (float *)&Block<T, T>::output_stream->writeBuf[base]);
                }
            }
            else
#endif
                for (int i = 0; i < nsamples; i++)
                {
                    const complex_t *ar = (complex_t *)((size_t)&buffer[i + 1] & ~(align - 1));
                    const unsigned al = &buffer[i + 1] - ar;
                    volk_32fc_32f_dot_prod_32fc_a((lv_32fc_t *)&Block<T, T>::output_stream->writeBuf[i], (lv_32fc_t *)ar, taps[al], ntaps + al);
                }
        }

        Block<T, T>::output_stream->swap(nsamples);

        memmove(&buffer[0], &buffer[nsamples], ntaps * sizeof(T));
    }

    template class FIRBlock<complex_t>;
    template class FIRBlock<float>;
}