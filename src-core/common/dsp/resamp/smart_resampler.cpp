#include "smart_resampler.h"
#include "logger.h"
#include <cmath>
#include <algorithm>

namespace dsp
{
    // Reduce a sample-rate ratio to small integers with a continued fraction.
    //
    // The previous code scaled both rates by 10 until both were integers, which is exact but can
    // produce enormous co-prime pairs: an RTL-SDR at 2.56 Msps against a 2566666.25 Hz target gave
    // 1283333/1280000, i.e. a 1.28M-tap filter whose design alone cost 2.6 seconds of pipeline
    // startup (seen as a "hang" when starting any pipeline that needed resampling). A convergent
    // with a bounded denominator is within a few ppm - irrelevant to a demodulator - and the filter
    // design cost scales with the denominator instead.
    static void approximate_sample_rate_ratio(double num, double den, unsigned &out_p, unsigned &out_q, unsigned max_term = 1024)
    {
        double x = num / den;
        long long h2 = 0, h1 = 1, k2 = 1, k1 = 0; // h[-2], h[-1], k[-2], k[-1]
        double b = x;

        for (int i = 0; i < 64; i++)
        {
            double a_f = std::floor(b);
            if (a_f < 1.0)
                break;

            long long a = (long long)std::min<double>(a_f, (double)max_term + 1.0);
            long long h = a * h1 + h2;
            long long k = a * k1 + k2;

            if (h > (long long)max_term || k > (long long)max_term)
                break;

            h2 = h1;
            h1 = h;
            k2 = k1;
            k1 = k;

            double frac = b - a_f;
            if (frac < 1e-12)
                break;
            b = 1.0 / frac;
        }

        if (h1 <= 0 || k1 <= 0) // ratio below 1 or degenerate - keep the caller's values
        {
            out_p = (unsigned)std::round(num);
            out_q = (unsigned)std::round(den);
            return;
        }

        out_p = (unsigned)h1;
        out_q = (unsigned)k1;
    }

    template <typename T>
    SmartResamplerBlock<T>::SmartResamplerBlock(std::shared_ptr<dsp::stream<T>> input, unsigned interpolation, unsigned decimation)
        : Block<T, T>(input),
          d_interpolation(interpolation),
          d_decimation(decimation)
    {
        if (d_decimation > d_interpolation)
        {
            use_decim = false;
            use_resamp = false;

            int best_power = floor(log2(decimation / interpolation));
            double rsamp_in = decimation;
            double fout = interpolation;

            if (best_power > 0)
            {
                int best_decim = std::min<int>(1 << best_power, PowerDecimatorBlock<T>::max_decim());
                rsamp_in = (double)decimation / (double)best_decim;
                pdecim = std::make_unique<PowerDecimatorBlock<T>>(nullptr, best_decim);
                use_decim = true;
                // logger->info("Using Decim ratio %d", best_decim);
            }

            // logger->info("SampRate %f %f", rsamp_in, fout);

            if (rsamp_in != fout)
            {
                unsigned p, q;
                approximate_sample_rate_ratio(fout, rsamp_in, p, q);
                rresamp = std::make_unique<RationalResamplerBlock<T>>(nullptr, p, q);
                use_resamp = true;
                // logger->info("Using Rsamp %f / %f", fout, rsamp_in);
            }
        }
        else if (d_decimation < d_interpolation)
        {
            use_decim = false;
            use_resamp = true;

            unsigned p, q;
            approximate_sample_rate_ratio(interpolation, decimation, p, q);
            rresamp = std::make_unique<RationalResamplerBlock<T>>(nullptr, p, q);

            // logger->info("Using Rsamp %d / %d", interpolation, decimation);
        }
        else
        {
            use_decim = false;
            use_resamp = false;
        }
    }

    template <typename T>
    SmartResamplerBlock<T>::~SmartResamplerBlock()
    {
    }

    template <typename T>
    int SmartResamplerBlock<T>::process(T *input, int nsamples, T *output)
    {
        if (use_decim && use_resamp)
        {
            nsamples = pdecim->process(input, nsamples, output);
            nsamples = rresamp->process(output, nsamples, output);
        }
        else if (use_resamp)
        {
            nsamples = rresamp->process(input, nsamples, output);
        }
        else if (use_decim)
        {
            nsamples = pdecim->process(input, nsamples, output);
        }
        else
        {
            memcpy(output, input, nsamples * sizeof(T));
        }

        return nsamples;
    }

    template <typename T>
    void SmartResamplerBlock<T>::work()
    {
        int nsamples = Block<T, T>::input_stream->read();
        if (nsamples <= 0)
        {
            Block<T, T>::input_stream->flush();
            return;
        }

        int outn = process(Block<T, T>::input_stream->readBuf, nsamples, Block<T, T>::output_stream->writeBuf);

        Block<T, T>::input_stream->flush();
        Block<T, T>::output_stream->swap(outn);
    }

    template class SmartResamplerBlock<complex_t>;
    template class SmartResamplerBlock<float>;
}
