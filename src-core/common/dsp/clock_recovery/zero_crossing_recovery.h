#pragma once

#include "common/dsp/block.h"

/*
Simple zero-crossing clock recovery.
Emits one interpolated symbol per recovered symbol period and
decimates its input (the number of emitted samples is what gets swapped).
*/
namespace dsp
{
    class ZeroCrossingRecoveryBlock : public Block<float, float>
    {
    private:
        float prev = 0, phase = 0;
        float period;
        float alpha;

        void work();

    public:
        ZeroCrossingRecoveryBlock(std::shared_ptr<dsp::stream<float>> input, int sps, float alpha);
        ~ZeroCrossingRecoveryBlock();
    };
}