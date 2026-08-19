#include "ldpc_decoder_avx.h"
#include <cassert>

namespace codings
{
    namespace ldpc
    {
        LDPCDecoderAVX::LDPCDecoderAVX(Sparse_matrix pcm) : LDPCDecoder(pcm)
        {
            d_pcm = pcm; // Retained for the sum-product generic fallback.

            int max_deg = 0;
            for (size_t rows = 0; rows < pcm.get_n_rows(); rows++)
            {
                int deg = 0;
                std::vector<int> deg_pos;
                for (size_t cols = 0; cols < pcm.get_n_cols(); cols++)
                    if (pcm.at(rows, cols))
                        deg++;
                if (max_deg < deg)
                    max_deg = deg;
            }

            d_pcm_num_cn = pcm.get_n_rows();
            d_pcm_num_vn = pcm.get_n_cols();
            d_pcm_max_cn_degree = max_deg;
            d_pcm_num_edges = pcm.get_n_connections();

            d_vns = new __m256i[d_pcm_num_vn];
            d_vns_to_cn_msgs = new __m256i[d_pcm_max_cn_degree];
            d_cn_to_vn_msgs = new __m256i[d_pcm_num_cn * d_pcm_max_cn_degree];
            d_abs_msgs = new __m256i[d_pcm_max_cn_degree];

            d_vn_addr = new __m256i *[d_pcm_num_edges];
            d_row_pos_deg = new int[d_pcm_num_cn * 2];

            /* Precompute VN addresses in the VN buffer. Since each row has a different
             * degree the VN addresses are compacted. Keeping the offset and the degree
             * of each row is therefore required. */
            int row_base_idx = 0;
            auto mv = pcm;
            for (size_t row = 0; row < mv.get_n_rows(); row++)
            {
                int row_deg = 0;

                for (size_t col = 0; col < mv.get_n_cols(); col++)
                    if (mv.at(row, col))
                        row_deg++;

                assert(row_deg <= d_pcm_max_cn_degree);

                d_row_pos_deg[row * 2] = row_base_idx;
                d_row_pos_deg[row * 2 + 1] = row_deg;

                for (size_t col = 0; col < mv.get_n_cols(); col++)
                    if (mv.at(row, col))
                        d_vn_addr[row_base_idx++] = &(d_vns[col]);
            }
        }

        LDPCDecoderAVX::~LDPCDecoderAVX()
        {
            delete d_generic_fallback;
            delete[] d_vns;
            delete[] d_vns_to_cn_msgs;
            delete[] d_abs_msgs;
            delete[] d_cn_to_vn_msgs;
            delete[] d_prev_vn_to_cn_msgs;
            delete[] d_sc_msgs;
            delete[] d_vn_addr;
            delete[] d_row_pos_deg;
        }

        void LDPCDecoderAVX::set_algorithm(ldpc_algorithm_t a)
        {
            d_algorithm = a;

            if (a == LDPC_SELF_CORRECTED_MIN_SUM && d_prev_vn_to_cn_msgs == nullptr)
            {
                d_prev_vn_to_cn_msgs = new __m256i[d_pcm_num_cn * d_pcm_max_cn_degree];
                d_sc_msgs = new __m256i[d_pcm_max_cn_degree];
            }

            /* Sum-product is not implemented in the AVX kernel; run it on the generic
             * decoder instead (see header for the rationale). */
            if (a == LDPC_SUM_PRODUCT && d_generic_fallback == nullptr)
            {
                d_generic_fallback = new LDPCDecoderGeneric(d_pcm);
                d_generic_fallback->set_algorithm(LDPC_SUM_PRODUCT);
            }
        }

        int LDPCDecoderAVX::decode(uint8_t *out, const int8_t *in, int it)
        {
            int corrections = 0;

            /* Sum-product fallback: decode the 16 interleaved frames one at a time on
             * the generic (scalar) BP decoder. The input is laid out as 16 frames
             * concatenated (frame z at in[z*d_pcm_num_vn]), matching the generic
             * decoder's single-frame interface. */
            if (d_algorithm == LDPC_SUM_PRODUCT)
            {
                /* Defensive: set_algorithm normally creates the fallback, but guard
                 * against a decode() call without a prior set_algorithm(). */
                if (d_generic_fallback == nullptr)
                {
                    d_generic_fallback = new LDPCDecoderGeneric(d_pcm);
                    d_generic_fallback->set_algorithm(LDPC_SUM_PRODUCT);
                }
                for (int z = 0; z < 16; z++)
                    corrections += d_generic_fallback->decode(&out[z * d_pcm_num_vn], &in[z * d_pcm_num_vn], it);
                d_last_iterations = d_generic_fallback->last_iterations();
                return corrections;
            }

            /* The length of the input block should correspond to the length of a codeword. */
            // if (len != d_pcm->code.n)
            // {
            //     return -1;
            // }

            /* Copy the input codeword and give lowest LLR value to each punctured
             * bit. Also interleave */
            for (int i = 0; i < d_pcm_num_vn; i++)
            {
                // d_vns[i] = (int16_t)in[i];
                int16_t *ptrVar = (int16_t *)d_vns;
                for (int z = 0; z < 16; z++)
                    ptrVar[16 * i + z] = in[z * d_pcm_num_vn + i];
            }

            /* Init of CN to VN messages */
            for (int i = 0; i < d_pcm_num_cn * d_pcm_max_cn_degree; i++)
            {
                // d_cn_to_vn_msgs[i] = 0;
                d_cn_to_vn_msgs[i] = _mm256_set1_epi16(0);
            }

            if (d_algorithm == LDPC_SELF_CORRECTED_MIN_SUM)
                for (int i = 0; i < d_pcm_num_cn * d_pcm_max_cn_degree; i++)
                    d_prev_vn_to_cn_msgs[i] = _mm256_set1_epi16(0);

            /* Decode step */
            int it_used = 0;
            while (it--)
            {
                for (int cn_idx = 0; cn_idx < d_pcm_num_cn; cn_idx++)
                {
                    generic_cn_kernel(cn_idx);
                }
                it_used++;

                /* Early termination: per-lane syndrome check. The 16 lanes are
                 * interleaved inside each __m256i (lane z of VN i lives at
                 * ptrVar[16*i + z]). For every check node we XOR the hard decisions
                 * (sign >= 0 => 1, else 0) of its connected VNs, per lane. We stop
                 * early only when ALL 16 lanes satisfy every parity check. */
                __m256i all_parity = _mm256_setzero_si256();
                for (int cn_idx = 0; cn_idx < d_pcm_num_cn; cn_idx++)
                {
                    int row_base = d_row_pos_deg[cn_idx * 2];
                    int deg = d_row_pos_deg[cn_idx * 2 + 1];
                    __m256i parity = _mm256_setzero_si256();
                    for (int vn_idx = 0; vn_idx < deg; vn_idx++)
                    {
                        __m256i v = *d_vn_addr[row_base + vn_idx];
                        // Hard decision: 1 if v >= 0, else 0 (matches the final hard
                        // decision in decode()). cmpgt(v, -1) is true iff v >= 0.
                        __m256i ge0 = _mm256_cmpgt_epi16(v, _mm256_set1_epi16(-1));
                        __m256i hard = _mm256_and_si256(ge0, _mm256_set1_epi16(1));
                        parity = _mm256_xor_si256(parity, hard);
                    }
                    all_parity = _mm256_or_si256(all_parity, parity);
                }

                if (_mm256_testz_si256(all_parity, all_parity))
                    break;
            }

            d_last_iterations = it_used;

            /* Hard decision & Deinterleave */
            for (int i = 0; i < d_pcm_num_vn; i++)
            {
                int16_t *ptrVar = (int16_t *)d_vns;
                for (int z = 0; z < 16; z++)
                {
                    out[z * d_pcm_num_vn + i] = (uint8_t)(ptrVar[16 * i + z] >= 0 ? 1 : 0);

                    if (i < d_pcm_num_vn - d_pcm_num_cn)
                        if ((out[z * d_pcm_num_vn + i] > 0) != (in[z * d_pcm_num_vn + i] > 0))
                            corrections++;
                }
            }

            return corrections;
        }

        void LDPCDecoderAVX::generic_cn_kernel(int cn_idx)
        {
            /* Given an indexed CN, gather the messages of all VNs connected
             * to that CN and determine a new estimation for each of the VNs. */

            cn_row_base = d_row_pos_deg[cn_idx * 2];
            cn_deg = d_row_pos_deg[cn_idx * 2 + 1];
            cn_offset = d_pcm_max_cn_degree * cn_idx;

            for (int vn_idx = 0; vn_idx < cn_deg; vn_idx++)
            {
                d_vns_to_cn_msgs[vn_idx] =
                    _mm256_sub_epi16(*d_vn_addr[cn_row_base + vn_idx], d_cn_to_vn_msgs[cn_offset + vn_idx]);
            }

            /* Self-corrected min-sum: erase messages whose sign flipped since the last
             * iteration, as those are considered unreliable. Only the check node
             * computation sees the erased values; the VN update below still uses the
             * true extrinsic, otherwise the VN would lose its channel information. */
            const __m256i *cn_in = d_vns_to_cn_msgs;

            if (d_algorithm == LDPC_SELF_CORRECTED_MIN_SUM)
            {
                for (int vn_idx = 0; vn_idx < cn_deg; vn_idx++)
                {
                    __m256i new_m = d_vns_to_cn_msgs[vn_idx];
                    __m256i prev_m = d_prev_vn_to_cn_msgs[cn_offset + vn_idx];

                    /* Erase where prev != 0 AND signs differ */
                    __m256i flipped = _mm256_srai_epi16(_mm256_xor_si256(prev_m, new_m), 15);
                    __m256i prev_nz = _mm256_xor_si256(_mm256_cmpeq_epi16(prev_m, _mm256_setzero_si256()), _mm256_set1_epi16(-1));
                    __m256i keep = _mm256_xor_si256(_mm256_and_si256(flipped, prev_nz), _mm256_set1_epi16(-1));

                    new_m = _mm256_and_si256(new_m, keep);

                    d_prev_vn_to_cn_msgs[cn_offset + vn_idx] = new_m;
                    d_sc_msgs[vn_idx] = new_m;
                }

                cn_in = d_sc_msgs;
            }

            parity = _mm256_set1_epi16(0);
            min1 = _mm256_set1_epi16(UINT8_MAX);
            min2 = _mm256_set1_epi16(UINT8_MAX);

            if (cn_deg & 0x1)
                parity = _mm256_xor_si256(_mm256_set1_epi16(0xFFFF), parity);

            /* Compute the parity of all soft bits represented by the VNs to CN
             * messages, the absolute value of each soft bit, and the overall
             * first and second minimums. All these results are used later to
             * determine a new estimation sent back to each of the VNs connected
             * to the current CN. */
            for (int vn_idx = 0; vn_idx < cn_deg; vn_idx++)
            {
                msg = cn_in[vn_idx];
                parity = _mm256_xor_si256(parity, msg);

                /* Bit-hack to compute the absolute value of the message */
                // abs_mask = msg >> (sizeof(msg) * 8 - 1);
                // abs_msg = (int16_t)((msg + abs_mask) ^ abs_mask);
                abs_msg = _mm256_abs_epi16(msg);

                /* Determine the first and second minimum */
                min2 = _mm256_min_epi16(_mm256_max_epi16(min1, abs_msg), min2);
                min1 = _mm256_min_epi16(abs_msg, min1);

                /* Keep the computed absolute value for later */
                d_abs_msgs[vn_idx] = abs_msg;
            }

            /* Compute a new soft bit estimation for each VN respectively and send it
             * in a new message.
             * Basically, we are updating the value of each adjacent VN of the CN given the values of
             * the other VNs. We are computing an error correction value that we add to the
             * current value of a VN. The sign of the error correction value has to follow a
             * critical property of LDPC codes, that is, the set of all soft bits of VNs
             * adjacent to a CN has even parity. Ex: a hard bit should have value '0' if
             * if the remaining has even parity, otherwise '1'.
             * Concretely, the min-sum variant for belief-propagation works as follows to compute
             * a new estimation :
             * 1) Take the minimum absolute value among all soft bits of all other VNs
             *    (the value of the VN to be updated is not taken into account) which
             *    will become the magnitude of the error correcting value.
             * 2) Compute the sign of the error correcting value (which will make a soft
             *    bit converge toward a negative or positive value and thus to a hard
             *    '0' or '1' bit)
             * 3) Add it to the current soft bit value of a VN */
            for (int vn_idx = 0; vn_idx < cn_deg; vn_idx++)
            {
                equ_wip = _mm256_sign_epi16(_mm256_set1_epi16(-1), _mm256_cmpeq_epi16(d_abs_msgs[vn_idx], min1));
                equ_min1 = _mm256_add_epi16(_mm256_xor_si256(_mm256_set1_epi16(0xFFFF), equ_wip), _mm256_set1_epi16(1));
                min = _mm256_or_si256(_mm256_and_si256(min1, _mm256_xor_si256(_mm256_set1_epi16(0xFFFF), equ_min1)), _mm256_and_si256(min2, equ_min1));

                /* Normalized Min-Sum: scale magnitude by alpha (Q8). Magnitudes are
                 * bounded well under 2^15 so the low half of the product suffices. */
                if (d_algorithm == LDPC_NORMALIZED_MIN_SUM)
                    min = _mm256_srli_epi16(_mm256_mullo_epi16(min, _mm256_set1_epi16(d_nms_alpha_q8)), 8);

                sign = _mm256_xor_si256(parity, cn_in[vn_idx]);

                /* Bit hack in order to multiply by the sign */
                new_msg = _mm256_sign_epi16(min, sign);

                /* Add error correction value */
                to_vn = _mm256_add_epi16(new_msg, d_vns_to_cn_msgs[vn_idx]);

                /* Save new soft bit value and CN to VN message */
                d_cn_to_vn_msgs[cn_offset + vn_idx] = new_msg;
                *d_vn_addr[cn_row_base + vn_idx] = to_vn;
            }
        }
    }
}
