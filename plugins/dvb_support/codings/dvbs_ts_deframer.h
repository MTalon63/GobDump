#pragma once

#include <cstdint>
#include <cstring>
#include "common/repack_bits_byte.h"

namespace deframing
{
    // Deframes 8 interleaved, randomized TS frames.
    class DVBS_TS_Deframer
    {
    public:
        const int TS_SIZE = 1632 * 8; // RS-Encoded

    private:
        // Compare 2 8-bit values and return the differing bit count
        int compare_8(uint8_t v1, uint8_t v2)
        {
            int cor = 0;
            uint8_t diff = v1 ^ v2;
            for (; diff; cor++)
                diff &= diff - 1;
            return cor;
        }

        uint8_t *full_frame_shifter;
        uint8_t sync_bytes[8];

        RepackBitsByte repack;
        uint8_t *buf_bytes; // Repacked bytes buffer

    public:
        DVBS_TS_Deframer();
        ~DVBS_TS_Deframer();

        /*
        Actual deframing function. Takes unpacked bits in,
        returns frames as packed bytes in the output buffer.

        The return value is the frame count.
        */
        int work(uint8_t *input, int size, uint8_t *output);
    };
}