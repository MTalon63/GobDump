#pragma once

#include "common/dsp/block.h"

/*
Costas loop implementation
with order 2, 4 and 8 support
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

        void work();

    public:
        CostasLoopBlock(std::shared_ptr<dsp::stream<complex_t>> input, float loop_bw, unsigned int order, float freq_limit = 1.0);

        float getFreq() { return freq; }
    };
}