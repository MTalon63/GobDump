#pragma once

#include "common/dsp/block.h"
#include "common/dsp/resamp/polyphase_bank.h"

/*
Complex and real clock recovery M&M.
This is like 0.1% slower than using  aligned taps,
but leads to much greater stability and output.
*/
namespace dsp
{
    template <typename T>
    class MMClockRecoveryBlock : public Block<T, T>
    {
    private:
        // Buffer
        T *buffer;

        // Variables
        float mu;
        float omega;
        float omega_gain;
        float mu_gain;
        float omega_relative_limit;
        float omega_mid;
        float omega_limit;

        float sample;
        float last_sample;

        complex_t p_2T, p_1T, p_0T;
        complex_t c_2T, c_1T, c_0T;

        void work();

        // The symbol loop, with the TED form fixed at compile time so the branch is not in the body.
        template <bool REAL_TED>
        void work_loop(int nsamples);

        PolyphaseBank pfb;

        float phase_error = 0;

        int ouc = 0;
        int inc = 0;

        // Predicted interpolation instant: tap arm and sample offset advance by the rate estimate
        // off a state re-anchored on (mu, inc) every MM_RING symbols (measured arm error ~1.2 RMS).
        static constexpr int MM_RING = 8;
        unsigned mm_i = MM_RING;
        float mm_base = 0.5f;
        float mm_omega = 1.0f;
        int mm_inc = 0;

    public:
        // BPSK only: real-form M&M with +/-1 decisions is 4 multiplies instead of two dependent
        // complex ones, and keeps the complex slicer's imag sign (pure noise here) out of the error.
        // A field, not a ctor arg, so the mangled name cannot change. Off unless a demod enables it.
        bool bpsk_real_ted = false;

        MMClockRecoveryBlock(std::shared_ptr<dsp::stream<T>> input, float omega, float omegaGain, float mu, float muGain, float omegaLimit, int nfilt = 128, int ntaps = 8);
        ~MMClockRecoveryBlock();
    };
}