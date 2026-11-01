#pragma once

#include "common/dsp/block.h"
#include <cmath>
#include <functional>

namespace dvbs
{
    class DVBSymToSoftBlock : public dsp::Block<complex_t, int8_t>
    {
    private:
        void work();

        int in_sym_buffer = 0;
        int8_t *sym_buffer;

        // Util
        int8_t clamp(float x)
        {
            if (!std::isfinite(x))
                return 0;
            if (x <= -127.0f)
                return -127;
            if (x >= 127.0f)
                return 127;
            return (int8_t)std::round(x);
        }

    public:
        std::function<void(complex_t *, int)> syms_callback;

    public:
        DVBSymToSoftBlock(std::shared_ptr<dsp::stream<complex_t>> input, int bufsize);
        ~DVBSymToSoftBlock();
    };
}