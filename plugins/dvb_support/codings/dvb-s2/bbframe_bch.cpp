/* -*- c++ -*- */
/*
 * Copyright 2018 Ahmet Inan, Ron Economos.
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

/*
 * This file has been adapted from https://github.com/drmpeg/gr-dvbs2rx
 */

#include "bbframe_bch.h"
#include <stdio.h>
#include <functional>
#include <cstring>

// BCH Code
#define BCH_CODE_N8 0
#define BCH_CODE_N10 1
#define BCH_CODE_N12 2
#define BCH_CODE_S12 3
#define BCH_CODE_M12 4

namespace dvbs2
{
    namespace
    {
        struct BchFrameParamsEntry
        {
            dvbs2_framesize_t framesize;
            dvbs2_code_rate_t rate;
            unsigned int kbch;
            unsigned int nbch;
            unsigned int bch_code;
        };

        const BchFrameParamsEntry bch_frame_params[] = {
            {FECFRAME_NORMAL, C1_4, 16008, 16200, BCH_CODE_N12},
            {FECFRAME_NORMAL, C1_3, 21408, 21600, BCH_CODE_N12},
            {FECFRAME_NORMAL, C2_5, 25728, 25920, BCH_CODE_N12},
            {FECFRAME_NORMAL, C1_2, 32208, 32400, BCH_CODE_N12},
            {FECFRAME_NORMAL, C3_5, 38688, 38880, BCH_CODE_N12},
            {FECFRAME_NORMAL, C2_3, 43040, 43200, BCH_CODE_N10},
            {FECFRAME_NORMAL, C3_4, 48408, 48600, BCH_CODE_N12},
            {FECFRAME_NORMAL, C4_5, 51648, 51840, BCH_CODE_N12},
            {FECFRAME_NORMAL, C5_6, 53840, 54000, BCH_CODE_N10},
            {FECFRAME_NORMAL, C8_9, 57472, 57600, BCH_CODE_N8},
            {FECFRAME_NORMAL, C9_10, 58192, 58320, BCH_CODE_N8},
            {FECFRAME_SHORT, C1_4, 3072, 3240, BCH_CODE_S12},
            {FECFRAME_SHORT, C1_3, 5232, 5400, BCH_CODE_S12},
            {FECFRAME_SHORT, C2_5, 6312, 6480, BCH_CODE_S12},
            {FECFRAME_SHORT, C1_2, 7032, 7200, BCH_CODE_S12},
            {FECFRAME_SHORT, C3_5, 9552, 9720, BCH_CODE_S12},
            {FECFRAME_SHORT, C2_3, 10632, 10800, BCH_CODE_S12},
            {FECFRAME_SHORT, C3_4, 11712, 11880, BCH_CODE_S12},
            {FECFRAME_SHORT, C4_5, 12432, 12600, BCH_CODE_S12},
            {FECFRAME_SHORT, C5_6, 13152, 13320, BCH_CODE_S12},
            {FECFRAME_SHORT, C8_9, 14232, 14400, BCH_CODE_S12},
        };
    } // namespace

    BBFrameBCH::FrameParams BBFrameBCH::frameParams(dvbs2_framesize_t framesize, dvbs2_code_rate_t rate)
    {
        FrameParams params;

        for (const BchFrameParamsEntry &entry : bch_frame_params)
        {
            if (entry.framesize != framesize || entry.rate != rate)
                continue;

            params.kbch = entry.kbch;
            params.nbch = entry.nbch;
            params.bch_code = entry.bch_code;

            switch (params.bch_code)
            {
            case BCH_CODE_N12:
                params.num_parity_bits = 192;
                break;
            case BCH_CODE_N10:
                params.num_parity_bits = 160;
                break;
            case BCH_CODE_N8:
                params.num_parity_bits = 128;
                break;
            case BCH_CODE_M12:
                params.num_parity_bits = 180;
                break;
            case BCH_CODE_S12:
                params.num_parity_bits = 168;
                break;
            default: // Unsupported MODCOD: sizes stay zeroed.
                break;
            }
            break;
        }

        return params;
    }

    BBFrameBCH::BBFrameBCH(dvbs2_framesize_t framesize, dvbs2_code_rate_t rate)
    {
        FrameParams params = frameParams(framesize, rate);
        kbch = params.kbch;
        nbch = params.nbch;
        bch_code = params.bch_code;
        num_parity_bits = params.num_parity_bits;

        // Init decoder stufff
        frame = 0;
        instance_n = new GF_NORMAL();
        instance_m = new GF_MEDIUM();
        instance_s = new GF_SHORT();
        decode_n_12 = new BCH_NORMAL_12();
        decode_n_10 = new BCH_NORMAL_10();
        decode_n_8 = new BCH_NORMAL_8();
        decode_m_12 = new BCH_MEDIUM_12();
        decode_s_12 = new BCH_SHORT_12();

        bch_poly_build_tables();
    }

    int BBFrameBCH::poly_mult(const int *ina, int lena, const int *inb, int lenb, int *out)
    {
        memset(out, 0, sizeof(int) * (lena + lenb));

        for (int i = 0; i < lena; i++)
        {
            for (int j = 0; j < lenb; j++)
            {
                if (ina[i] * inb[j] > 0)
                {
                    out[i + j]++; // count number of terms for this pwr of x
                }
            }
        }
        int max = 0;
        for (int i = 0; i < lena + lenb; i++)
        {
            out[i] = out[i] & 1; // If even ignore the term
            if (out[i])
            {
                max = i;
            }
        }
        // return the size of array to house the result.
        return max + 1;
    }

    void BBFrameBCH::calculate_crc_table(void)
    {
        for (int divident = 0; divident < 256;
             divident++)
        { /* iterate over all possible input byte values 0 - 255 */
            std::bitset<MAX_BCH_PARITY_BITS> curByte(divident);
            curByte <<= num_parity_bits - 8;

            for (unsigned char bit = 0; bit < 8; bit++)
            {
                if ((curByte[num_parity_bits - 1]) != 0)
                {
                    curByte <<= 1;
                    curByte ^= polynome;
                }
                else
                {
                    curByte <<= 1;
                }
            }
            crc_table[divident] = curByte;
        }
    }

    void BBFrameBCH::calculate_medium_crc_table(void)
    {
        for (int divident = 0; divident < 16;
             divident++)
        { /* iterate over all possible input byte values 0 - 15 */
            std::bitset<MAX_BCH_PARITY_BITS> curByte(divident);
            curByte <<= num_parity_bits - 4;

            for (unsigned char bit = 0; bit < 4; bit++)
            {
                if ((curByte[num_parity_bits - 1]) != 0)
                {
                    curByte <<= 1;
                    curByte ^= polynome;
                }
                else
                {
                    curByte <<= 1;
                }
            }
            crc_medium_table[divident] = curByte;
        }
    }

    void BBFrameBCH::bch_poly_build_tables()
    {
        if (kbch == 0 || nbch == 0 || num_parity_bits == 0) // Unsupported MODCOD
            return;

        // Normal polynomials
        const int polyn01[] = {1, 0, 1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
        const int polyn02[] = {1, 1, 0, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1};
        const int polyn03[] = {1, 0, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1};
        const int polyn04[] = {1, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 1};
        const int polyn05[] = {1, 1, 1, 1, 0, 1, 0, 0, 1, 1, 1, 1, 1, 0, 0, 0, 1};
        const int polyn06[] = {1, 0, 1, 0, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1};
        const int polyn07[] = {1, 0, 1, 0, 0, 1, 1, 0, 1, 1, 1, 1, 0, 1, 0, 1, 1};
        const int polyn08[] = {1, 1, 1, 0, 0, 1, 1, 0, 1, 1, 0, 0, 1, 1, 1, 0, 1};
        const int polyn09[] = {1, 0, 0, 0, 0, 1, 0, 1, 0, 1, 1, 1, 0, 0, 0, 0, 1};
        const int polyn10[] = {1, 1, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 1, 1, 1, 0, 1};
        const int polyn11[] = {1, 0, 1, 1, 0, 1, 0, 0, 0, 1, 0, 1, 1, 1, 0, 0, 1};
        const int polyn12[] = {1, 1, 0, 0, 0, 1, 1, 1, 0, 1, 0, 1, 1, 0, 0, 0, 1};

        // Medium polynomials
        const int polym01[] = {1, 0, 1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
        const int polym02[] = {1, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 1, 0, 0, 0, 1};
        const int polym03[] = {1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 1, 0, 1};
        const int polym04[] = {1, 0, 1, 1, 0, 1, 1, 0, 1, 0, 1, 1, 0, 0, 0, 1};
        const int polym05[] = {1, 1, 1, 0, 1, 0, 1, 1, 0, 0, 1, 0, 1, 0, 0, 1};
        const int polym06[] = {1, 0, 0, 0, 1, 0, 1, 1, 0, 0, 0, 0, 1, 1, 0, 1};
        const int polym07[] = {1, 0, 1, 0, 1, 1, 0, 1, 0, 0, 0, 1, 1, 0, 1, 1};
        const int polym08[] = {1, 0, 1, 0, 1, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 1};
        const int polym09[] = {1, 1, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1, 1, 1, 0, 1};
        const int polym10[] = {1, 1, 1, 1, 1, 0, 0, 1, 0, 0, 1, 1, 1, 1, 0, 1};
        const int polym11[] = {1, 1, 1, 0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 1};
        const int polym12[] = {1, 0, 1, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0, 1, 1, 1};

        // Short polynomials
        const int polys01[] = {1, 1, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1};
        const int polys02[] = {1, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 1, 0, 0, 1};
        const int polys03[] = {1, 1, 1, 0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 0, 1};
        const int polys04[] = {1, 0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 0, 1, 0, 1};
        const int polys05[] = {1, 0, 1, 0, 1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 1};
        const int polys06[] = {1, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0, 0, 0, 1, 1};
        const int polys07[] = {1, 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 1, 0, 1, 1};
        const int polys08[] = {1, 0, 0, 0, 0, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1};
        const int polys09[] = {1, 1, 1, 1, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 1};
        const int polys10[] = {1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1};
        const int polys11[] = {1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0, 1};
        const int polys12[] = {1, 1, 1, 1, 0, 1, 1, 1, 1, 0, 1, 0, 0, 1, 1};

        int len;
        int polyout[2][200];

#define COPY_BCH_POLYNOME                              \
    for (unsigned int i = 0; i < num_parity_bits; i++) \
    {                                                  \
        polynome[i] = polyout[0][i];                   \
    }

        switch (bch_code)
        {
        case BCH_CODE_N12:
        case BCH_CODE_N10:
        case BCH_CODE_N8:

            len = poly_mult(polyn01, 17, polyn02, 17, polyout[0]);
            len = poly_mult(polyn03, 17, polyout[0], len, polyout[1]);
            len = poly_mult(polyn04, 17, polyout[1], len, polyout[0]);
            len = poly_mult(polyn05, 17, polyout[0], len, polyout[1]);
            len = poly_mult(polyn06, 17, polyout[1], len, polyout[0]);
            len = poly_mult(polyn07, 17, polyout[0], len, polyout[1]);
            len = poly_mult(polyn08, 17, polyout[1], len, polyout[0]);
            if (bch_code == BCH_CODE_N8)
            {
                COPY_BCH_POLYNOME
            }

            len = poly_mult(polyn09, 17, polyout[0], len, polyout[1]);
            len = poly_mult(polyn10, 17, polyout[1], len, polyout[0]);
            if (bch_code == BCH_CODE_N10)
            {
                COPY_BCH_POLYNOME
            }

            len = poly_mult(polyn11, 17, polyout[0], len, polyout[1]);
            len = poly_mult(polyn12, 17, polyout[1], len, polyout[0]);
            if (bch_code == BCH_CODE_N12)
            {
                COPY_BCH_POLYNOME
            }
            break;

        case BCH_CODE_S12:
            len = poly_mult(polys01, 15, polys02, 15, polyout[0]);
            len = poly_mult(polys03, 15, polyout[0], len, polyout[1]);
            len = poly_mult(polys04, 15, polyout[1], len, polyout[0]);
            len = poly_mult(polys05, 15, polyout[0], len, polyout[1]);
            len = poly_mult(polys06, 15, polyout[1], len, polyout[0]);
            len = poly_mult(polys07, 15, polyout[0], len, polyout[1]);
            len = poly_mult(polys08, 15, polyout[1], len, polyout[0]);
            len = poly_mult(polys09, 15, polyout[0], len, polyout[1]);
            len = poly_mult(polys10, 15, polyout[1], len, polyout[0]);
            len = poly_mult(polys11, 15, polyout[0], len, polyout[1]);
            len = poly_mult(polys12, 15, polyout[1], len, polyout[0]);

            COPY_BCH_POLYNOME
            break;

        case BCH_CODE_M12:
            len = poly_mult(polym01, 16, polym02, 16, polyout[0]);
            len = poly_mult(polym03, 16, polyout[0], len, polyout[1]);
            len = poly_mult(polym04, 16, polyout[1], len, polyout[0]);
            len = poly_mult(polym05, 16, polyout[0], len, polyout[1]);
            len = poly_mult(polym06, 16, polyout[1], len, polyout[0]);
            len = poly_mult(polym07, 16, polyout[0], len, polyout[1]);
            len = poly_mult(polym08, 16, polyout[1], len, polyout[0]);
            len = poly_mult(polym09, 16, polyout[0], len, polyout[1]);
            len = poly_mult(polym10, 16, polyout[1], len, polyout[0]);
            len = poly_mult(polym11, 16, polyout[0], len, polyout[1]);
            len = poly_mult(polym12, 16, polyout[1], len, polyout[0]);

            COPY_BCH_POLYNOME
            break;
        }
        calculate_crc_table();
        calculate_medium_crc_table();
    }

    /*
     * Our virtual destructor.
     */
    BBFrameBCH::~BBFrameBCH()
    {
        delete decode_s_12;
        delete decode_m_12;
        delete decode_n_8;
        delete decode_n_10;
        delete decode_n_12;
        delete instance_s;
        delete instance_m;
        delete instance_n;
    }

    int BBFrameBCH::decode(uint8_t *frame)
    {
        int corrections = 0;

        if (kbch == 0 || nbch == 0)
            return 0;

        // Locals, so decode() stays re-entrant.
        uint8_t *code = &frame[0];
        uint8_t *parity = &frame[kbch / 8];

        switch (bch_code)
        {
        case BCH_CODE_N12:
            corrections = (*decode_n_12)(code, parity, 0, 0, kbch);
            break;
        case BCH_CODE_N10:
            corrections = (*decode_n_10)(code, parity, 0, 0, kbch);
            break;
        case BCH_CODE_N8:
            corrections = (*decode_n_8)(code, parity, 0, 0, kbch);
            break;
        case BCH_CODE_S12:
            corrections = (*decode_s_12)(code, parity, 0, 0, kbch);
            break;
        case BCH_CODE_M12:
            corrections = (*decode_m_12)(code, parity, 0, 0, kbch);
            break;
        }

        return corrections;
    }

    int BBFrameBCH::encode(uint8_t *frame)
    {
        int corrections = 0;

        // Repack to bits
        for (unsigned int i = 0; i < nbch; i++)
            encode_buffer[i] = (frame[i / 8] >> (7 - (i % 8))) & 1;

        std::bitset<MAX_BCH_PARITY_BITS> parity_bits;
        uint8_t *in = encode_buffer;
        uint8_t *out = encode_buffer + kbch;
        unsigned char b, temp, msb;

        for (int j = 0; j < (int)kbch / 8; j++)
        {
            b = 0;

            // calculate the crc using the lookup table, cf.
            // http://www.sunshine2k.de/articles/coding/crc/understanding_crc.html
            for (int e = 0; e < 8; e++)
            {
                temp = *in++;
                // consumed++;

                b |= temp << (7 - e);
            }

            msb = 0;
            for (int n = 1; n <= 8; n++)
            {
                temp = parity_bits[num_parity_bits - n];
                msb |= temp << (8 - n);
            }
            /* XOR-in next input byte into MSB of crc and get this MSB, that's our new
             * intermediate divident */
            unsigned char pos = (msb ^ b);
            /* Shift out the MSB used for division per lookuptable and XOR with the
             * remainder */
            parity_bits = (parity_bits << 8) ^ crc_table[pos];
        }

        // Now add the parity bits to the output
        for (unsigned int n = 0; n < num_parity_bits; n++)
        {
            *out++ = (char)parity_bits[num_parity_bits - 1];
            parity_bits <<= 1;
        }

        // Repack to bytes
        memset(&frame[kbch / 8], 0, (nbch - kbch) / 8);
        for (unsigned int i = 0; i < (nbch - kbch); i++)
            frame[(kbch / 8) + i / 8] = frame[(kbch / 8) + i / 8] << 1 | encode_buffer[kbch + i];

        return corrections;
    }
}