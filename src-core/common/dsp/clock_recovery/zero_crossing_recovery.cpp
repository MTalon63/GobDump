#include "zero_crossing_recovery.h"

namespace dsp
{
    ZeroCrossingRecoveryBlock::ZeroCrossingRecoveryBlock(std::shared_ptr<dsp::stream<float>> input, int sps, float alpha) : Block(input), period((float)sps), alpha(alpha)
    {
    }

    ZeroCrossingRecoveryBlock::~ZeroCrossingRecoveryBlock()
    {
    }

    void ZeroCrossingRecoveryBlock::work()
    {
        int nsamples = input_stream->read();
        if (nsamples <= 0)
        {
            input_stream->flush();
            return;
        }

        int ouc = 0;

        for (int i = 0; i < nsamples; i++)
        {
            float sample = input_stream->readBuf[i];
            bool fired = false;
            float sym = 0;

            // Zero-crossing detection
            if ((prev < 0.0f && sample >= 0.0f) || (prev >= 0.0f && sample < 0.0f))
            {
                float frac = prev / (prev - sample);

                // Phase error
                float expected = period / 2.0f;
                float actual = phase + frac;
                float err = actual - expected;

                phase -= err * alpha;
            }

            // Advance phase
            phase += 1.0f;

            // Generate (interpolate) output symbol
            if (phase >= period)
            {
                phase -= period;

                sym = prev + phase * (sample - prev);
                fired = true;
            }

            prev = sample;

            if (fired)
                output_stream->writeBuf[ouc++] = sym;
        }

        input_stream->flush();
        output_stream->swap(ouc);
    }
}