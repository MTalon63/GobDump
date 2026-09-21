#include "quadrature_demod.h"
#include <volk/volk.h>
#include <cstring>

namespace dsp
{
    QuadratureDemodBlock::QuadratureDemodBlock(std::shared_ptr<dsp::stream<complex_t>> input, float gain) : Block(input), gain(1.0 / gain)
    {
        // buffer = create_volk_buffer<complex_t>(STREAM_BUFFER_SIZE);
        // buffer_out = create_volk_buffer<complex_t>(STREAM_BUFFER_SIZE);
    }

    QuadratureDemodBlock::~QuadratureDemodBlock()
    {
        // volk_free(buffer);
        // volk_free(buffer_out);
    }

    void QuadratureDemodBlock::set_gain(float gain)
    {
        this->gain = 1.0 / gain;
    }

    int QuadratureDemodBlock::process(complex_t *input, int nsamples, float *output)
    {
        if (nsamples <= 0)
            return 0;

        if (qd_in.size() < (size_t)nsamples + 1u)
        {
            qd_in.resize((size_t)nsamples + 1u);
            qd_prod.resize((size_t)nsamples);
        }

        // Prepend the one-sample history so prod[i] = in[i] * conj(in[i - 1])
        qd_in[0] = qd_last;
        memcpy(&qd_in[1], input, (size_t)nsamples * sizeof(complex_t));

        volk_32fc_x2_multiply_conjugate_32fc((lv_32fc_t *)qd_prod.data(), (lv_32fc_t *)&qd_in[1], (lv_32fc_t *)&qd_in[0], nsamples);

        // Keep the last sample for the next call
        qd_last = input[nsamples - 1];

        // VOLK divides by normalizeFactor -> pass 1/gain to get (gain * atan2())
        volk_32fc_s32f_atan2_32f(output, (lv_32fc_t *)qd_prod.data(), 1.0f / gain, nsamples);

        return nsamples;
    }

    void QuadratureDemodBlock::work()
    {
        int nsamples = input_stream->read();
        if (nsamples <= 0)
        {
            input_stream->flush();
            return;
        }

        process(input_stream->readBuf, nsamples, output_stream->writeBuf);

        input_stream->flush();
        output_stream->swap(nsamples);
    }
}