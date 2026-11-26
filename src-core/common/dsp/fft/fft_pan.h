#pragma once

#include "common/dsp/block.h"
#include <fftw3.h>
#include <volk/volk_alloc.hh>
#include <functional>

namespace dsp
{
    class FFTPanBlock : public Block<complex_t, float>
    {
    private:
        std::mutex fft_mutex;
        volk::vector<float> fft_taps;
        int fft_size;
        void work();

        void destroy_fft();

        complex_t *fft_input_buffer;
        float *fft_output_buffer = nullptr;

        fftwf_complex *fftw_in;
        fftwf_complex *fftw_out;
        fftwf_plan fftw_plan;

        int rbuffer_rate = 0;
        int rbuffer_size = 0;

        // Position within the current decimation window, and how much of that window's
        // leading samples have been collected for the pending line. Samples in between
        // are skipped in place instead of being copied into a reshape buffer.
        int line_pos = 0;
        int line_fill = 0;

        void emit_line();

    public:
        FFTPanBlock(std::shared_ptr<dsp::stream<complex_t>> input);
        ~FFTPanBlock();
        void set_fft_settings(int size, uint64_t samplerate, int rate = 60);

        std::function<void(float *)> on_fft = [](float *) {};

        float avg_num = 10;
    };
}