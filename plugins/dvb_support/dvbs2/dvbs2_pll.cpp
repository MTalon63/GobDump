#include "dvbs2_pll.h"

namespace
{
    constexpr int NCO_SIZE = 1024;
    struct NcoTable
    {
        float c[NCO_SIZE + 1];
        float s[NCO_SIZE + 1];
        NcoTable()
        {
            for (int i = 0; i <= NCO_SIZE; i++)
            {
                double a = 2.0 * M_PI * (double)i / (double)NCO_SIZE;
                c[i] = (float)cos(a);
                s[i] = (float)sin(a);
            }
        }
    };
    const NcoTable &nco_table()
    {
        static const NcoTable t;
        return t;
    }
}
namespace dvbs2
{
    S2PLLBlock::S2PLLBlock(std::shared_ptr<dsp::stream<complex_t>> input, float loop_bw)
        : Block(input)
    {
        float damping = sqrtf(2.0f) / 2.0f;
        float denom = (1.0 + 2.0 * damping * loop_bw + loop_bw * loop_bw);

        alpha = (4 * damping * loop_bw) / denom;
        beta = (4 * loop_bw * loop_bw) / denom;
    }

    S2PLLBlock::~S2PLLBlock()
    {
    }

    void S2PLLBlock::work()
    {
        int nsamples = input_stream->read();
        if (nsamples <= 0)
        {
            input_stream->flush();
            return;
        }

        auto _lut = constellation->acquire_lut_view();
        for (int i = 0; i < (frame_slot_count + 1) * 90 + pilot_cnt * 36; i++)
        {
            float ph = phase;
            if (ph < 0.0f)
                ph += (float)(2.0 * M_PI);
            if (ph >= (float)(2.0 * M_PI))
                ph -= (float)(2.0 * M_PI);
            const NcoTable &nco = nco_table();
            float nf = ph * (float)(NCO_SIZE / (2.0 * M_PI));
            int ni = (int)nf;
            if (ni >= NCO_SIZE)
                ni = NCO_SIZE - 1;
            float nt = nf - (float)ni;
            float nc = nco.c[ni] + nt * (nco.c[ni + 1] - nco.c[ni]);
            float ns = nco.s[ni] + nt * (nco.s[ni + 1] - nco.s[ni]);
            tmp_val = input_stream->readBuf[i] * complex_t(nc, -ns);

            float error = 0;
            if (i >= 90)
            {
                constellation->demod_soft_lut_view(_lut, tmp_val, nullptr, &error);
                output_stream->writeBuf[i] = tmp_val;
            }
            else
            {
                if (i < 26) // Use known symbols for SOF
                    error = (tmp_val * sof.symbols[i].conj()).arg();
                else // And also use known Modcod and config to synchronize onto the PLS
                    error = (tmp_val * pls.symbols[pls_code][i - 26].conj()).arg();

                // We're done, convert to proper 45 degs BPSK
                output_stream->writeBuf[i] = (i & 1) ? complex_t(-tmp_val.real, tmp_val.imag) : complex_t(tmp_val.imag, tmp_val.real);
            }

            // Compute new freq and phase.
            freq += beta * error;
            phase += freq + alpha * error;

            // Wrap phase
            while (phase > (2 * M_PI))
                phase -= 2 * M_PI;
            while (phase < (-2 * M_PI))
                phase += 2 * M_PI;

            // Clamp freq
            if (freq > 1.0)
                freq = 1.0;
            if (freq < -1.0)
                freq = -1.0;
        }

        input_stream->flush();
        output_stream->swap(nsamples);
    }
}