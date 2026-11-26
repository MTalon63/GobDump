#include "fft_pan.h"
#include "logger.h"
#include "common/dsp/window/window.h"

namespace dsp
{
    FFTPanBlock::FFTPanBlock(std::shared_ptr<dsp::stream<complex_t>> input)
        : Block(input)
    {
    }

    FFTPanBlock::~FFTPanBlock()
    {
        if (fft_output_buffer != nullptr)
            destroy_fft();
    }

    void FFTPanBlock::set_fft_settings(int size, uint64_t samplerate, int rate)
    {
        fft_mutex.lock();

        if (rate < 1)
            rate = 1;

        fft_size = size;

        if (fft_output_buffer != nullptr)
            destroy_fft();

        // Compute FFT settings. One line is produced per rbuffer_rate input samples and
        // uses only the leading rbuffer_size of them.
        rbuffer_rate = (samplerate / rate);
        rbuffer_size = std::min<int>(rbuffer_rate, fft_size);
        line_pos = 0;
        line_fill = 0;
        logger->trace("FFT Rate %d, Samplerate %d, Final Size %d, Skip %d", rbuffer_rate, samplerate, rbuffer_size, rbuffer_rate - rbuffer_size);

        // Init taps, rectangular window
        fft_taps.resize(rbuffer_size);
        for (int i = 0; i < rbuffer_size; i++)
            fft_taps[i] = window::nuttall(i, rbuffer_size - 1) * ((i % 2) ? 1.0f : -1.0f);

        // Init FFTW
        fftw_in = (fftwf_complex *)fftwf_malloc(sizeof(fftwf_complex) * fft_size);
        fftw_out = (fftwf_complex *)fftwf_malloc(sizeof(fftwf_complex) * fft_size);
        fftw_plan = fftwf_plan_dft_1d(fft_size, fftw_in, fftw_out, FFTW_FORWARD, FFTW_ESTIMATE);

        memset(fftw_in, 0, sizeof(fftwf_complex) * fft_size);
        memset(fftw_out, 0, sizeof(fftwf_complex) * fft_size);

        // Output buffer. fft_input_buffer holds a single line only (rbuffer_size is at
        // most fft_size), so it is allocated once and reused for every line.
        fft_input_buffer = create_volk_buffer<complex_t>(fft_size);
        fft_output_buffer = create_volk_buffer<float>(fft_size);

        fft_mutex.unlock();
    }

    void FFTPanBlock::destroy_fft()
    {
        fftwf_free(fftw_in);
        fftwf_free(fftw_out);
        fftwf_destroy_plan(fftw_plan);
        volk_free(fft_input_buffer);
        volk_free(fft_output_buffer);
    }

    void FFTPanBlock::emit_line()
    {
        volk_32fc_32f_multiply_32fc((lv_32fc_t *)fftw_in, (lv_32fc_t *)fft_input_buffer, fft_taps.data(), rbuffer_size);

        fftwf_execute(fftw_plan);

        volk_32fc_s32f_power_spectrum_32f(fft_output_buffer, (lv_32fc_t *)fftw_out, fft_size, fft_size);

        if (avg_num < 1)
            avg_num = 1;
        float avg_rate = 1.0 / avg_num;

        for (int i = 0; i < fft_size; i++)
            output_stream->writeBuf[i] = output_stream->writeBuf[i] * (1.0f - avg_rate) + fft_output_buffer[i] * avg_rate;

        on_fft(output_stream->writeBuf);
    }

    void FFTPanBlock::work()
    {
        int nsamples = input_stream->read();
        if (nsamples <= 0)
        {
            input_stream->flush();
            return;
        }

        fft_mutex.lock();

        // Decimate by counting rather than by buffering. At low FFT rates rbuffer_rate is
        // samplerate/rate and can be many millions of samples, while only rbuffer_size of
        // them are ever transformed. The previous implementation kept every skipped sample
        // in a reshape buffer sized (samplerate/rate) * 10 -- hundreds of megabytes to
        // gigabytes at 25-53 MSPS and single digit FFT rates -- and memmoved the remainder
        // on every call. Walking the input and skipping in place costs one pass and
        // O(fft_size) memory, and removes the silent drop that occurred when that buffer
        // filled up.
        complex_t *in = input_stream->readBuf;
        int remaining = nsamples;

        while (remaining > 0)
        {
            int take = std::min<int>(remaining, rbuffer_rate - line_pos);
            int copy = std::min<int>(take, rbuffer_size - line_fill);

            if (copy > 0)
            {
                memcpy(&fft_input_buffer[line_fill], in, copy * sizeof(complex_t));
                line_fill += copy;
            }

            in += take;
            remaining -= take;
            line_pos += take;

            if (line_pos >= rbuffer_rate)
            {
                if (line_fill == rbuffer_size)
                    emit_line();

                line_pos = 0;
                line_fill = 0;
            }
        }

        input_stream->flush();

        fft_mutex.unlock();
    }
}
