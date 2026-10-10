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

#pragma once

#include "bitman.hh"
#include "reed_solomon_error_correction.hh"

namespace dvbs2
{
    namespace CODE
    {
        template <int ROOTS, int FCR, int MSG, typename GF>
        class BoseChaudhuriHocquenghemDecoder
        {
        public:
            typedef typename GF::value_type value_type;
            typedef typename GF::ValueType ValueType;
            typedef typename GF::IndexType IndexType;
            static const int NR = ROOTS;
            static const int N = GF::N, K = MSG, NP = N - K;

        private:
            ReedSolomonErrorCorrection<NR, FCR, GF> algorithm;
            struct SyndByteTables
            {
                IndexType r8[NR];
                ValueType T[256][NR];
            };

            static const SyndByteTables &synd_byte_tables()
            {
                static const SyndByteTables t = [] {
                    SyndByteTables t;
                    IndexType root(FCR), pe(1);
                    for (int i = 0; i < NR; ++i)
                    {
                        IndexType rp(0);
                        ValueType rpow[8];
                        for (int m = 0; m < 8; ++m)
                        {
                            rpow[m] = value(rp);
                            rp *= root;
                        }
                        t.r8[i] = rp;
                        for (int v = 0; v < 256; ++v)
                        {
                            ValueType acc = ValueType::zero();
                            for (int m = 0; m < 8; ++m)
                                if ((v >> m) & 1)
                                    acc = acc + rpow[m];
                            t.T[v][i] = acc;
                        }
                        root *= pe;
                    }
                    return t;
                }();
                return t;
            }

            void
            update_syndromes(uint8_t *poly, ValueType *syndromes, int begin, int end)
            {
                IndexType roots[NR];
                {
                    IndexType root(FCR), pe(1);
                    for (int i = 0; i < NR; ++i)
                    {
                        roots[i] = root;
                        root *= pe;
                    }
                }

                int j = begin;
                for (; j < end && (j & 7) != 0; ++j)
                {
                    ValueType coeff(get_be_bit(poly, j));
                    for (int i = 0; i < NR; ++i)
                        syndromes[i] = fma(roots[i], syndromes[i], coeff);
                }

                // 8 bits per step: s_i <- s_i * r_i^8 + T[v][i]
                const SyndByteTables &bt = synd_byte_tables();
                for (; j + 8 <= end; j += 8)
                {
                    const ValueType *tv = bt.T[poly[j >> 3]];
                    for (int i = 0; i < NR; ++i)
                        syndromes[i] = fma(bt.r8[i], syndromes[i], tv[i]);
                }

                for (; j < end; ++j)
                {
                    ValueType coeff(get_be_bit(poly, j));
                    for (int i = 0; i < NR; ++i)
                        syndromes[i] = fma(roots[i], syndromes[i], coeff);
                }
            }

        public:
            int
            compute_syndromes(uint8_t *data,
                              uint8_t *parity,
                              ValueType *syndromes,
                              int data_len = K)
            {
                //   assert(0 < data_len && data_len <= K);
                // $syndromes_i = code(pe^{FCR+i})$
                ValueType coeff(get_be_bit(data, 0));
                for (int i = 0; i < NR; ++i)
                {
                    syndromes[i] = coeff;
                }
                update_syndromes(data, syndromes, 1, data_len);
                update_syndromes(parity, syndromes, 0, NP);
                int nonzero = 0;
                for (int i = 0; i < NR; ++i)
                {
                    nonzero += !!syndromes[i];
                }
                return nonzero;
            }

            int
            compute_syndromes(uint8_t *data,
                              uint8_t *parity,
                              value_type *syndromes,
                              int data_len = K)
            {
                return compute_syndromes(
                    data, parity, reinterpret_cast<ValueType *>(syndromes), data_len);
            }

            int
            operator()(uint8_t *data,
                       uint8_t *parity,
                       value_type *erasures = 0,
                       int erasures_count = 0,
                       int data_len = K)
            {
                //   assert(0 <= erasures_count && erasures_count <= NR);
                //   assert(0 < data_len && data_len <= K);
                if (0)
                {
                    for (int i = 0; i < erasures_count; ++i)
                    {
                        int idx = (int)erasures[i];
                        if (idx < data_len)
                        {
                            set_be_bit(data, idx, 0);
                        }
                        else
                        {
                            set_be_bit(parity, idx - data_len, 0);
                        }
                    }
                }
                if (erasures_count && data_len < K)
                {
                    for (int i = 0; i < erasures_count; ++i)
                    {
                        erasures[i] += K - data_len;
                    }
                }
                ValueType syndromes[NR];
                if (!compute_syndromes(data, parity, syndromes, data_len))
                {
                    return 0;
                }
                IndexType locations[NR];
                ValueType magnitudes[NR];
                int count = algorithm(syndromes,
                                      locations,
                                      magnitudes,
                                      reinterpret_cast<IndexType *>(erasures),
                                      erasures_count);
                if (count <= 0)
                {
                    return count;
                }
                for (int i = 0; i < count; ++i)
                {
                    if ((int)locations[i] < K - data_len)
                    {
                        return -1;
                    }
                }
                for (int i = 0; i < count; ++i)
                {
                    if (1 < (int)magnitudes[i])
                    {
                        return -1;
                    }
                }
                for (int i = 0; i < count; ++i)
                {
                    int idx = (int)locations[i] + data_len - K;
                    bool err = (bool)magnitudes[i];
                    if (idx < data_len)
                    {
                        xor_be_bit(data, idx, err);
                    }
                    else
                    {
                        xor_be_bit(parity, idx - data_len, err);
                    }
                }
                int corrections_count = 0;
                for (int i = 0; i < count; ++i)
                {
                    corrections_count += !!magnitudes[i];
                }
                return corrections_count;
            }
        };
    } // namespace CODE
}