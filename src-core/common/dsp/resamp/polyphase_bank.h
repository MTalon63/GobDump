#pragma once

#include <cstring>
#include <vector>

namespace dsp
{
    class PolyphaseBank
    {
    private:
        bool is_init = false;

    public:
        int nfilt;
        int ntaps;
        int stride = 0;
        float **taps;
        float *taps_flat = nullptr;

    public:
        PolyphaseBank();
        void init(std::vector<float> rtaps, int nfilt);
        ~PolyphaseBank();
        PolyphaseBank(const PolyphaseBank &) = delete;
    };
} // namespace dsp
