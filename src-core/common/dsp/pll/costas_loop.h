#pragma once

#include "common/dsp/block.h"
#include "common/dsp/demod/constellation.h"

/*
Costas loop implementation
with order 2, 4 and 8 support, plus an order-16 mode that is a
decision-directed PLL against an arbitrary constellation table.

Order 16 does NOT use a fixed PSK detector: 16APSK is not constant
modulus, so a Costas-style phase detector has no well defined lock
points. Instead the detector is the nearest constellation point
(16APSK has 4-fold rotational symmetry, so the 90 degree ambiguity is
resolved by the decoder's phase search, as for the other orders).

Order 16 REQUIRES set_dd_constellation() to have been called: without a
table there is no decision to steer by, the error stays 0 and the loop
silently never corrects (it does not crash, which makes the omission look
like a signal problem). See PSKDemodModule for the working set-up.
*/
namespace dsp
{
    class CostasLoopBlock : public Block<complex_t, complex_t>
    {
    private:
        int order;

        float freq = 0;
        float pha_re = 1, pha_im = 0; // Phase, used as NCO
        float fre_re = 1, fre_im = 0; // Freq, as phase increment
        unsigned int renorm_ctr = 0;
        float loop_bw;
        float alpha, beta;

        float freq_limit_min, freq_limit_max;
        float freq_limit_min_re, freq_limit_min_im, freq_limit_max_re, freq_limit_max_im;

        // Order 16 = decision-directed on this table, AGC-independent via a power EMA.
        const constellation_t *dd_const = nullptr;
        float dd_pwr = 1.0f;
        static constexpr float DD_PWR_ALPHA = 1.0f / 1024.0f;

        void work();

    public:
        CostasLoopBlock(std::shared_ptr<dsp::stream<complex_t>> input, float loop_bw, unsigned int order, float freq_limit = 1.0);

        // Retune at runtime (accumulators preserved).
        void set_loop_bw(float bw);

        // Table for the order-16 detector. Must outlive this block.
        void set_dd_constellation(const constellation_t *c)
        {
            dd_const = c;
            dd_pwr = 1.0f;
        }

        float getFreq() { return freq; }
    };
}