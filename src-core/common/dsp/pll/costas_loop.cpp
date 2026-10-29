#include "costas_loop.h"
#include <cmath>

namespace dsp
{
    CostasLoopBlock::CostasLoopBlock(std::shared_ptr<dsp::stream<complex_t>> input, float loop_bw, unsigned int order, float freq_limit)
        : Block(input), order(order), loop_bw(loop_bw), freq_limit_min(-freq_limit), freq_limit_max(freq_limit)
    {
        float damping = sqrtf(2.0f) / 2.0f;
        float denom = (1.0 + 2.0 * damping * loop_bw + loop_bw * loop_bw);
        alpha = (4 * damping * loop_bw) / denom;
        beta = (4 * loop_bw * loop_bw) / denom;

        freq_limit_min_re = cosf(freq_limit_min);
        freq_limit_min_im = -sinf(freq_limit_min);
        freq_limit_max_re = cosf(freq_limit_max);
        freq_limit_max_im = -sinf(freq_limit_max);
    }

    void CostasLoopBlock::work()
    {
        int nsamples = input_stream->read();
        if (nsamples <= 0)
        {
            input_stream->flush();
            return;
        }

        for (int i = 0; i < nsamples; i++)
        {
            // Mix input & VCO
            const complex_t in = input_stream->readBuf[i];
            float tmp_re = in.real * pha_re - in.imag * pha_im;
            float tmp_im = in.real * pha_im + in.imag * pha_re;
            output_stream->writeBuf[i] = complex_t(tmp_re, tmp_im);

            // Calculate error
            float error = 0;
            switch (order)
            {
            case 2: // Order 2, BPSK
                error = tmp_re * tmp_im;
                break;
            case 4: // Order 4, QPSK
                error = (tmp_re > 0.0f ? 1.0f : -1.0f) * tmp_im - (tmp_im > 0.0f ? 1.0f : -1.0f) * tmp_re;
                break;
            case 8: // Order 8, 8-PSK
                const float K = (sqrtf(2.0) - 1);
                if (fabsf(tmp_re) >= fabsf(tmp_im))
                    error = ((tmp_re > 0.0f ? 1.0f : -1.0f) * tmp_im - (tmp_im > 0.0f ? 1.0f : -1.0f) * tmp_re * K);
                else
                    error = ((tmp_re > 0.0f ? 1.0f : -1.0f) * tmp_im * K - (tmp_im > 0.0f ? 1.0f : -1.0f) * tmp_re);
                break;
            }

            // Clip error
            error = branchless_clip(error, 1.0);

            float prev_freq = freq, prev_pha_re = pha_re, prev_pha_im = pha_im, prev_fre_re = fre_re, prev_fre_im = fre_im;

            float df = beta * error;
            freq += df;
            float new_fre_re = fre_re + df * fre_im;
            float new_fre_im = fre_im - df * fre_re;
            fre_re = new_fre_re;
            fre_im = new_fre_im;

            float pa = alpha * error;
            float new_pha_re_off = pha_re + pa * pha_im;
            float new_pha_im_off = pha_im - pa * pha_re;
            float new_pha_re = new_pha_re_off * fre_re - new_pha_im_off * fre_im;
            float new_pha_im = new_pha_re_off * fre_im + new_pha_im_off * fre_re;
            pha_re = new_pha_re;
            pha_im = new_pha_im;

            // Clamp freq
            if (freq > freq_limit_max)
                freq = freq_limit_max;
            if (freq < freq_limit_min)
                freq = freq_limit_min;

            // The small-angle rotations drift in magnitude, renormalize and sync the clamped freq
            if (renorm_ctr++ >= 64)
            {
                renorm_ctr = 0;
                float inv = 1.0f / sqrtf(pha_re * pha_re + pha_im * pha_im);
                pha_re *= inv, pha_im *= inv;
                inv = 1.0f / sqrtf(fre_re * fre_re + fre_im * fre_im);
                fre_re *= inv, fre_im *= inv;

                if (freq >= freq_limit_max)
                    fre_re = freq_limit_max_re, fre_im = freq_limit_max_im;
                else if (freq <= freq_limit_min)
                    fre_re = freq_limit_min_re, fre_im = freq_limit_min_im;
            }

            // freq/phase are feedback accumulators: one NaN sample sticks forever, and neither the
            // clamp nor the renorm can recover (all NaN comparisons are false). Hold last good values.
            if (!std::isfinite(freq) || !std::isfinite(pha_re) || !std::isfinite(pha_im) || !std::isfinite(fre_re) || !std::isfinite(fre_im))
            {
                freq = prev_freq;
                pha_re = prev_pha_re;
                pha_im = prev_pha_im;
                fre_re = prev_fre_re;
                fre_im = prev_fre_im;
            }
        }

        input_stream->flush();
        output_stream->swap(nsamples);
    }
}