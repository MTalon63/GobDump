#pragma once

#include "common/dsp/block.h"
#include <vector>

/*
Quadrature demodulator
*/
namespace dsp
{
    class QuadratureDemodBlock : public Block<complex_t, float>
    {
    private:
        float gain;
        // complex_t *buffer, *buffer_out;
        void work();
        complex_t qd_last = 1;          // one-sample history (unit => first call matches the scalar reference)
        std::vector<complex_t> qd_in;   // capacity nsamples + 1: [previous, current...]
        std::vector<complex_t> qd_prod; // capacity nsamples: in[i] * conj(in[i - 1])

    public:
        QuadratureDemodBlock(std::shared_ptr<dsp::stream<complex_t>> input, float gain);
        ~QuadratureDemodBlock();

        int process(complex_t *input, int nsamples, float *output);
        void set_gain(float gain);
    };
}