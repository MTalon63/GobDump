#include "polyphase_bank.h"
#include <volk/volk.h>

namespace dsp
{
    void PolyphaseBank::init(std::vector<float> rtaps, int nfilt2)
    {
        if (is_init)
        {
            volk_free(taps_flat);
            volk_free(taps);
        }

        this->nfilt = nfilt2;
        this->ntaps = (rtaps.size() + nfilt - 1) / nfilt;

        // Get alignement parameters
        int align = volk_get_alignment();

        // If Ntaps is slightly over 1, add 1 tap
        if (fmod(double(rtaps.size()) / double(nfilt), 1.0) > 0.0)
            ntaps++;

        // Rows live in one contiguous aligned block rather than nfilt separate allocations. taps[i]
        // stays valid for every existing caller, and taps_flat + i * stride addresses a row directly.
        const int align_floats = align / (int)sizeof(float);
        stride = ((ntaps + align_floats - 1) / align_floats) * align_floats;

        taps = (float **)volk_malloc(nfilt * sizeof(float *), align);
        taps_flat = (float *)volk_malloc((size_t)nfilt * stride * sizeof(float), align);
        for (int y = 0; y < nfilt * stride; y++)
            taps_flat[y] = 0;
        for (int i = 0; i < nfilt; i++)
            this->taps[i] = taps_flat + (size_t)i * stride;

        // Setup taps
        for (int i = 0; i < nfilt * ntaps; i++)
            taps[(nfilt - 1) - (i % nfilt)][i / nfilt] = (i < (int)rtaps.size()) ? rtaps[i] : 0;

        is_init = true;
    }

    PolyphaseBank::PolyphaseBank() {}

    PolyphaseBank::~PolyphaseBank()
    {
        if (is_init)
        {
            volk_free(taps_flat);
            volk_free(taps);
        }
    }
} // namespace dsp
