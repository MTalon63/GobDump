#pragma once

#include <cstdint>

/*
DVB-S descrambler. The 15-bit Fibonacci LFSR is stepped 8 bits at a time using two
precomputed tables (low 8 state bits, high 7 state bits) combined with XOR.
*/
namespace dvbs
{
    class DVBSScrambling
    {
    private:
        int reg = 0;

        // out_lo[reg & 0xFF] ^ out_hi[reg >> 8] is the 8-bit PRBS byte for the next 8 clocks.
        // reg_lo/reg_hi are the same combination for the resulting 15-bit register state.
        uint8_t out_lo[256], out_hi[128];
        uint16_t reg_lo[256], reg_hi[128];

        static int prbs_bit_ref(int &r)
        {
            int feedback = ((r >> 13) ^ (r >> 14)) & 0x1;
            r = ((r << 1) | feedback) & 0x7fff;
            return feedback;
        }

        void build_tables()
        {
            // out_* : MSB-first byte assembled from 8 single-clock steps.
            // reg_* : the register after those 8 steps.
            for (int v = 0; v < 256; v++)
            {
                int r = v; // low 8 bits set, high bits zero
                int o = 0;
                for (int i = 0; i < 8; i++)
                    o = (o << 1) | prbs_bit_ref(r);
                out_lo[v] = (uint8_t)o;
                reg_lo[v] = (uint16_t)r;
            }
            for (int v = 0; v < 128; v++)
            {
                int r = v << 8; // high 7 bits set
                int o = 0;
                for (int i = 0; i < 8; i++)
                    o = (o << 1) | prbs_bit_ref(r);
                out_hi[v] = (uint8_t)o;
                reg_hi[v] = (uint16_t)r;
            }
        }

        // Returns the next 8 PRBS bits and advances the register.
        inline int prbs8()
        {
            const int lo = reg & 0xFF;
            const int hi = reg >> 8;
            reg = reg_lo[lo] ^ reg_hi[hi];
            return out_lo[lo] ^ out_hi[hi];
        }

    public:
        DVBSScrambling() { build_tables(); }

        void descramble(uint8_t *frm)
        {
            for (int pkt = 0; pkt < 8; pkt++)
            {
                int outc = 0;

                if (frm[pkt * 204 + outc] == 0xB8)
                    reg = 0xa9; // Reset
                else
                    prbs8(); // Skip

                frm[pkt * 204 + outc++] = 0x47;

                for (int k = 1; k < 188; k++)
                    frm[pkt * 204 + outc++] ^= prbs8();
            }
        }
    };
};
