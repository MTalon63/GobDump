#include "agc.h"
#include <algorithm>
#include <cmath>
#include <volk/volk.h>

namespace dsp
{
    template <typename T>
    AGCBlock<T>::AGCBlock(std::shared_ptr<dsp::stream<T>> input, float agc_rate, float reference, float gain, float max_gain)
        : Block<T, T>(input),
          rate(agc_rate),
          reference(reference),
          gain(gain),
          max_gain(max_gain)
    {
    }

    template <typename T>
    void AGCBlock<T>::work()
    {
        int nsamples = Block<T, T>::input_stream->read();
        if (nsamples <= 0)
        {
            Block<T, T>::input_stream->flush();
            return;
        }

        // Averaged gain update per group; the group-mean update must be the exact n-step form,
        // not rate*n, which exceeds |pole|<1 on any but tiny inputs and diverges (alternating).
        static constexpr int AGC_GROUP = 16;
        float mag[AGC_GROUP];

        for (int i = 0; i < nsamples; i += AGC_GROUP)
        {
            const int n = std::min(AGC_GROUP, nsamples - i);

            if constexpr (std::is_same_v<T, complex_t>)
                volk_32fc_magnitude_32f(mag, (lv_32fc_t *)&Block<T, T>::input_stream->readBuf[i], n);
            else
                for (int j = 0; j < n; j++)
                    mag[j] = fabsf(Block<T, T>::input_stream->readBuf[i + j]);

            float mean = 0;
            for (int j = 0; j < n; j++)
                mean += mag[j];
            mean /= (float)n;

            for (int j = 0; j < n; j++)
                Block<T, T>::output_stream->writeBuf[i + j] = Block<T, T>::input_stream->readBuf[i + j] * gain;

            float b = 1.0f - rate * mean;
            float bn = b;
            for (int k = 1; k < n; k++)
                bn *= b;

            if (mean < 1e-9f)
                gain += rate * reference * n;
            else
                gain = gain * bn + rate * reference * ((1.0f - bn) / (1.0f - b));

            if (max_gain > 0.0 && gain > max_gain)
                gain = max_gain;

            // gain is a feedback accumulator: one NaN input makes it NaN permanently, and the clamp
            // above cannot undo that because every comparison against NaN is false.
            if (!std::isfinite(gain))
                gain = 1.0f;
        }

        Block<T, T>::input_stream->flush();
        Block<T, T>::output_stream->swap(nsamples);
    }

    template class AGCBlock<complex_t>;
    template class AGCBlock<float>;
}