#include "cc_encoder.h"
#include "cc_common.h"
#include <volk/volk.h>
#include <volk/volk_typedefs.h>
#include <cmath>

namespace viterbi
{
    CCEncoder::CCEncoder(int frame_size, int k, int rate, std::vector<int> polys, int start_state)
        : d_rate(rate),
          d_k(k),
          d_polys(polys),
          d_start_state(start_state)
    {
        if (static_cast<size_t>(d_rate) != d_polys.size())
            throw std::runtime_error("cc_encoder: Number of polynomials must be the same as the value of rate");

        if (d_rate < 2)
            throw std::runtime_error("cc_encoder: inverse rate r must be > 2");

        if (k < 2 || k > 31)
            throw std::runtime_error("cc_encoder: constraint length K must in be the range [2, 31]");

        if (d_start_state >= (1u << (d_k - 1)))
            throw std::runtime_error("cc_encoder: start state is invalid; must be in range [0, 2^(K-1)-1] where K is the constraint length");

        if (frame_size < 1)
            throw std::runtime_error("cc_encoder: frame_size must be > 0");

        partab_init();
        build_out_table();

        d_max_frame_size = frame_size;
        set_frame_size(frame_size);
    }

    CCEncoder::~CCEncoder()
    {
    }

    bool CCEncoder::set_frame_size(unsigned int frame_size)
    {
        bool ret = true;
        if (frame_size > d_max_frame_size)
        {
            frame_size = d_max_frame_size;
            ret = false;
        }

        d_frame_size = frame_size;

        d_output_size = d_rate * d_frame_size;

        return ret;
    }

    double CCEncoder::rate()
    {
        return static_cast<double>(d_rate);
    }

    int CCEncoder::parity(int x)
    {
        x ^= (x >> 16);
        x ^= (x >> 8);
        return parityb(x);
    }

    int CCEncoder::parityb(unsigned char x)
    {
        return Partab[x];
    }

    void CCEncoder::partab_init(void)
    {
        int i, cnt, ti;

        /* Initialize parity lookup table */
        for (i = 0; i < 256; i++)
        {
            cnt = 0;
            ti = i;
            while (ti)
            {
                if (ti & 1)
                    cnt++;
                ti >>= 1;
            }
            Partab[i] = cnt & 1;
        }
    }

    void CCEncoder::build_out_table()
    {
        // The table is only valid when every polynomial fits in K bits.
        d_use_table = d_k <= 16;
        for (int p : d_polys)
            if ((unsigned int)std::abs(p) >= (1u << d_k))
                d_use_table = false;
        if (!d_use_table)
            return;

        d_low_mask = (1 << d_k) - 1;
        d_table_stride = (size_t)1 << d_k;
        out_table.assign((size_t)d_rate * d_table_stride, 0);
        for (unsigned int j = 0; j < d_rate; j++)
            for (int v = 0; v <= d_low_mask; v++)
                out_table[(size_t)j * d_table_stride + v] = (d_polys[j] < 0) ^ parity(v & std::abs(d_polys[j]));
    }

    void CCEncoder::work(uint8_t *in, uint8_t *out) { work(in, out, d_frame_size); }

    void CCEncoder::work(uint8_t *in, uint8_t *out, int size)
    {
        unsigned int my_state = d_start_state;

        for (int i = 0; i < size; ++i)
        {
            my_state = (my_state << 1) | (in[i] & 1);
            if (d_use_table)
            {
                const unsigned int low = my_state & d_low_mask;
                for (unsigned int j = 0; j < d_rate; ++j)
                    out[i * d_rate + j] = out_table[(size_t)j * d_table_stride + low];
            }
            else
            {
                for (unsigned int j = 0; j < d_rate; ++j)
                    out[i * d_rate + j] = (d_polys[j] < 0) ^ parity(my_state & std::abs(d_polys[j])) ? 1 : 0;
            }
        }

        d_start_state = my_state;
    }
}
