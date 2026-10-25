#pragma once

#include <cstdint>
#include <vector>

/*
DVB-S interleaver / deinterleaver. Adapted from GNU Radio.
Each register is a fixed-depth delay line of length M*i. The register lengths and the
deinterleave index order are load-bearing; do not reorder.
*/
namespace dvbs
{
    class DVBSInterleaving
    {
    private:
        const int d_MUX_PKT = 8;
        const int d_I = 12;
        const int d_M = 17;

        // buf holds the last L inputs; w is the next slot to emit/store.
        struct Delay
        {
            std::vector<uint8_t> buf;
            int w = 0;

            void init(int n) { buf.assign(n, 0); w = 0; }
        };

        std::vector<Delay> d_shift_interleave;
        std::vector<Delay> d_shift_deinterleave;

    public:
        DVBSInterleaving()
        {
            // Interleaving: register i has length M*i.
            d_shift_interleave.resize(d_I);
            for (int i = 0; i < d_I; i++)
                d_shift_interleave[i].init(d_M * i);

            // Deinterleaving: registers are emplaced in reverse, so index j has length M*(I-1-j).
            d_shift_deinterleave.resize(d_I);
            for (int i = (d_I - 1); i >= 0; i--)
                d_shift_deinterleave[d_I - 1 - i].init(d_M * i);
        }

        // Emit the oldest value, then store v where it was.
        static inline uint8_t delay_step(Delay &d, uint8_t v)
        {
            const int L = (int)d.buf.size();
            if (L == 0)
                return v; // empty register: v is immediately the oldest
            const uint8_t out = d.buf[d.w];
            d.buf[d.w] = v;
            d.w++;
            if (d.w == L)
                d.w = 0;
            return out;
        }

        void interleave(uint8_t *in, uint8_t *out)
        {
            for (unsigned int j = 0; j < d_shift_interleave.size(); j++)
                out[j] = delay_step(d_shift_interleave[j], in[j]);
        }

        void deinterleave(uint8_t *in, uint8_t *out)
        {
            int count = 0;
            for (int mux_pkt = 0; mux_pkt < d_MUX_PKT; mux_pkt++)
            {
                // This is actually the deinterleaver
                for (int k = 0; k < (d_M * d_I); k++)
                {
                    out[count] = delay_step(d_shift_deinterleave[k % d_I], in[count]);
                    count++;
                }
            }
        }
    };
};
