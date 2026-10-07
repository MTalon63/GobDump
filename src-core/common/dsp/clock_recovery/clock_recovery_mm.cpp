#include "clock_recovery_mm.h"
#include "common/dsp/block.h"
#include "common/dsp/window/window.h"
#include <cmath>

#define DO_BRANCH 0

#if (defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)) && defined(DSP_HAVE_AVX2_ISA)
#include <immintrin.h>

// Complex x real 8-tap dot; VOLK's AVX2 kernel falls back to scalar at this tap count.
static inline void mm_dot8_avx2(const float *a, const float *tp, float *out)
{
    // moveldup duplicates even lanes, so taps are pre-permuted for the interleaved complex layout.
    const __m256i idx_lo = _mm256_setr_epi32(0, 0, 1, 1, 2, 2, 3, 3);
    const __m256i idx_hi = _mm256_setr_epi32(4, 4, 5, 5, 6, 6, 7, 7);

    const __m256 v0 = _mm256_loadu_ps(a);
    const __m256 v1 = _mm256_loadu_ps(a + 8);
    const __m256 t = _mm256_loadu_ps(tp);
    const __m256 td0 = _mm256_permutevar8x32_ps(t, idx_lo);
    const __m256 td1 = _mm256_permutevar8x32_ps(t, idx_hi);

    const __m256 re = _mm256_add_ps(_mm256_mul_ps(_mm256_moveldup_ps(v0), td0), _mm256_mul_ps(_mm256_moveldup_ps(v1), td1));
    const __m256 im = _mm256_add_ps(_mm256_mul_ps(_mm256_movehdup_ps(v0), td0), _mm256_mul_ps(_mm256_movehdup_ps(v1), td1));

    __m128 r = _mm_add_ps(_mm256_castps256_ps128(re), _mm256_extractf128_ps(re, 1));
    __m128 i = _mm_add_ps(_mm256_castps256_ps128(im), _mm256_extractf128_ps(im, 1));
    r = _mm_hadd_ps(r, r);
    r = _mm_hadd_ps(r, r);
    i = _mm_hadd_ps(i, i);
    i = _mm_hadd_ps(i, i);

    out[0] = _mm_cvtss_f32(r) * 0.5f; // moveldup duplicates every lane, so each product lands twice
    out[1] = _mm_cvtss_f32(i) * 0.5f;
}
#define MM_HAVE_AVX2_PATH 1
#endif

namespace dsp
{
    template <typename T>
    MMClockRecoveryBlock<T>::MMClockRecoveryBlock(std::shared_ptr<dsp::stream<T>> input, float omega, float omegaGain, float mu, float muGain, float omegaLimit, int nfilt, int ntaps)
        : Block<T, T>(input), mu(mu), omega(omega), omega_gain(omegaGain), mu_gain(muGain), omega_relative_limit(omegaLimit), p_2T(0), p_1T(0), p_0T(0), c_2T(0), c_1T(0), c_0T(0)
    {
        // Omega setup
        omega_mid = omega;
        omega_limit = omega_relative_limit * omega;

        // Init interpolator
        pfb.init(windowed_sinc(nfilt * ntaps, hz_to_rad(0.5 / (double)nfilt, 1.0), window::nuttall, nfilt), nfilt);

        // Buffer
#if DO_BRANCH
        buffer = create_volk_buffer<T>(pfb.ntaps * 4);
#else
        // Speculative read can lead nsamples by up to MM_RING*ceil(omega); size the buffer for that.
        buffer = create_volk_buffer<T>(STREAM_BUFFER_SIZE + MM_RING * (int)ceilf(omega * (1.0f + omega_relative_limit)) + 2 * ntaps);
#endif
    }

    template <typename T>
    MMClockRecoveryBlock<T>::~MMClockRecoveryBlock()
    {
        volk_free(buffer);
    }

    template <typename T>
    void MMClockRecoveryBlock<T>::work()
    {
        int nsamples = Block<T, T>::input_stream->read();
        if (nsamples <= 0)
        {
            Block<T, T>::input_stream->flush();
            return;
        }

        // Copy NTAPS samples in the buffer from input, as that's required for the last samples
#if DO_BRANCH
        memcpy(&buffer[pfb.ntaps - 1], Block<T, T>::input_stream->readBuf, (pfb.ntaps - 1) * sizeof(T));
#else
        memcpy(&buffer[pfb.ntaps - 1], Block<T, T>::input_stream->readBuf, nsamples * sizeof(T));
#endif
        ouc = 0;

        // TED form is chosen outside the loop; an in-body branch measured ~8-10% slower here.
        if (bpsk_real_ted)
            work_loop<true>(nsamples);
        else
            work_loop<false>(nsamples);

        inc -= nsamples;

        if (inc < 0)
            inc = 0;

        // We need some history for the next run, so copy it over into our buffer
#if DO_BRANCH
        memcpy(buffer, &Block<T, T>::input_stream->readBuf[nsamples - pfb.ntaps + 1], (pfb.ntaps - 1) * sizeof(T));
#else
        memmove(&buffer[0], &buffer[nsamples], pfb.ntaps * sizeof(T));
#endif

        Block<T, T>::input_stream->flush();
        Block<T, T>::output_stream->swap(ouc);
    }

    template <typename T>
    template <bool REAL_TED>
    void MMClockRecoveryBlock<T>::work_loop(int nsamples)
    {
        for (; inc < nsamples && ouc < STREAM_BUFFER_SIZE;)
        {
            if constexpr (std::is_same_v<T, complex_t>)
            {
                // Propagate delay
                p_2T = p_1T;
                p_1T = p_0T;
                c_2T = c_1T;
                c_1T = c_0T;
            }

            // Last finite phase error, used to hold state instead of re-seeding on a bad sample
            float prev_phase_error = phase_error;
            int pinc = inc;

            // Complex input predicts interpolation instant from the rate estimate, re-anchored on
            // (mu, inc) every MM_RING symbols, so no address waits on the TED. The T=float path is
            // left byte-identical (FSK/SDPSK/orbcomm have no decode ladder to cover a change).
            int imu;
            if constexpr (std::is_same_v<T, complex_t>)
            {
                if (mm_i >= MM_RING)
                {
                    mm_base = mu;
                    mm_omega = omega; // frozen: live omega re-adds the TED to the critical path
                    mm_inc = inc;     // re-anchor on the loop's own position: cannot drift apart
                    mm_i = 0;
                }
                float ppos = mm_base + mm_omega * (float)mm_i++;
                float pfl = floorf(ppos);
                pinc = mm_inc + (int)pfl;
                imu = (int)rint((ppos - pfl) * pfb.nfilt);
                // Pull the dot's sample window in early: it is the only load that can miss cache.
                _mm_prefetch((const char *)&buffer[pinc + 8], _MM_HINT_T0);
            }
            else
            {
                imu = (int)rint(mu * pfb.nfilt);
            }
            if (imu < 0) // If we're out of bounds, clamp
                imu = 0;
            if (imu >= pfb.nfilt)
                imu = pfb.nfilt - 1;

            if constexpr (std::is_same_v<T, float>)
            {
#if DO_BRANCH
                if (inc < (pfb.ntaps - 1))
                    volk_32f_x2_dot_prod_32f(&sample, &buffer[inc], pfb.taps[imu], pfb.ntaps);
                else
                    volk_32f_x2_dot_prod_32f(&sample, &Block<T, T>::input_stream->readBuf[inc - (pfb.ntaps - 1)], pfb.taps[imu], pfb.ntaps);
#else
                volk_32f_x2_dot_prod_32f(&sample, &buffer[inc], pfb.taps[imu], pfb.ntaps);
#endif

                // Phase error
                phase_error = (last_sample < 0 ? -1.0f : 1.0f) * sample - (sample < 0 ? -1.0f : 1.0f) * last_sample;
                phase_error = branched_clip(phase_error, 1.0);
                last_sample = sample;

                // Write output sample
                Block<T, T>::output_stream->writeBuf[ouc++] = sample;
            }
            if constexpr (std::is_same_v<T, complex_t>)
            {
#if DO_BRANCH
                if (inc < (pfb.ntaps - 1))
                    volk_32fc_32f_dot_prod_32fc((lv_32fc_t *)&p_0T, (lv_32fc_t *)&buffer[inc], pfb.taps[imu], pfb.ntaps);
                else
                    volk_32fc_32f_dot_prod_32fc((lv_32fc_t *)&p_0T, (lv_32fc_t *)&Block<T, T>::input_stream->readBuf[inc - (pfb.ntaps - 1)], pfb.taps[imu], pfb.ntaps);
#else
                const float *row = pfb.taps_flat + (size_t)imu * pfb.stride; // no table load: imu depends on the previous symbol
#ifdef MM_HAVE_AVX2_PATH
                if (pfb.ntaps == 8)
                {
                    float dot[2];
                    mm_dot8_avx2((const float *)&buffer[pinc], row, dot);
                    p_0T = complex_t(dot[0], dot[1]);
                }
                else
#endif
                    volk_32fc_32f_dot_prod_32fc((lv_32fc_t *)&p_0T, (lv_32fc_t *)&buffer[pinc], row, pfb.ntaps);
#endif

                // Slice it
                if constexpr (REAL_TED)
                {
                    // BPSK real-form M&M: +/-1 decisions collapse the conj() to 4 multiplies.
                    c_0T = complex_t(p_0T.real > 0.0f ? 1.0f : -1.0f, 0.0f);
                    phase_error = c_1T.real * p_0T.real - c_0T.real * p_1T.real;
                }
                else
                {
                    c_0T = complex_t(p_0T.real > 0.0f ? 1.0f : 0.0f, p_0T.imag > 0.0f ? 1.0f : 0.0f);
                    phase_error = (((p_0T - p_2T) * c_1T.conj()) - ((c_0T - c_2T) * p_1T.conj())).real;
                }
                phase_error = branched_clip(phase_error, 1.0);

                // Write output
                Block<T, T>::output_stream->writeBuf[ouc++] = p_0T;
            }

            // omega/mu/phase_error are feedback accumulators, and branched_clip is comparison-based so
            // NaN passes straight through and sticks forever. mu is then cast to int to index the buffer,
            // and (int)NaN is UB - measured as INT_MIN. Hold the last good values instead of indexing
            // with garbage / re-seeding, so a single bad sample does not lose lock.
            if (!std::isfinite(phase_error))
                phase_error = prev_phase_error;

            float prev_omega = omega;
            float prev_mu = mu;

            // Adjust omega
            omega = omega + omega_gain * phase_error;
            omega = omega_mid + branched_clip((omega - omega_mid), omega_limit);

            // Adjust phase
            mu = mu + omega + mu_gain * phase_error;

            if (!std::isfinite(omega))
                omega = prev_omega;
            if (!std::isfinite(mu))
                mu = prev_mu;

            inc += int(floor(mu));
            mu -= floor(mu);
            if (inc < 0)
                inc = 0;
        }
    }

    template class MMClockRecoveryBlock<complex_t>;
    template class MMClockRecoveryBlock<float>;
} // namespace dsp