#include "quadrature_demod.h"
#include "dsp/block_simple.h"
#include "dsp/flowgraph/flowgraph.h"
#include <complex.h>
#include <cstring>
#include <volk/volk.h>

namespace satdump
{
    namespace ndsp
    {
        QuadratureDemodBlock::QuadratureDemodBlock() : satdump::ndsp::BlockSimple<complex_t, float>("quadrature_demod_cf", {{"in", DSP_SAMPLE_TYPE_CF32}}, {{"out", DSP_SAMPLE_TYPE_F32}}) {}

        QuadratureDemodBlock::~QuadratureDemodBlock() {}

        uint32_t QuadratureDemodBlock::process(complex_t *input, uint32_t nsamples, float *output)
        {
            if (nsamples == 0)
                return 0;

            if (qd_in.size() < nsamples + 1u)
            {
                qd_in.resize(nsamples + 1u);
                qd_prod.resize(nsamples);
            }

            // Prepend the one-sample history so prod[i] = in[i] * conj(in[i - 1])
            qd_in[0] = qd_last;
            memcpy(&qd_in[1], input, nsamples * sizeof(complex_t));

            volk_32fc_x2_multiply_conjugate_32fc((lv_32fc_t *)qd_prod.data(), (lv_32fc_t *)&qd_in[1], (lv_32fc_t *)&qd_in[0], nsamples);

            // Keep the last sample for the next call
            qd_last = input[nsamples - 1];

            // VOLK divides by normalizeFactor -> pass 1/gain to get (gain * atan2())
            volk_32fc_s32f_atan2_32f(output, (lv_32fc_t *)qd_prod.data(), 1.0f / gain, nsamples);

            return nsamples;
        }
    } // namespace ndsp
} // namespace satdump