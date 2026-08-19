#include "ldpc_decoder_generic.h"
#include <cassert>
#include <cmath>

namespace codings
{
    namespace ldpc
    {
        // Shared sum-product φ LUT (built once, see header for the math).
        int16_t LDPCDecoderGeneric::d_phi_lut[LDPCDecoderGeneric::d_phi_lut_size];
        bool LDPCDecoderGeneric::d_phi_lut_ready = false;

        LDPCDecoderGeneric::LDPCDecoderGeneric(Sparse_matrix pcm) : LDPCDecoder(pcm)
        {
            /* Build the φ lookup table once (shared by all instances). φ(x) is
             * computed in double, scaled by d_phi_scale and clamped to
             * d_phi_lut_max so the values fit int16 and saturate at a magnitude
             * comparable to a typical message.
             *
             * The table is indexed by a fine grid of the argument: entry i holds
             * φ(i / d_phi_step). This fine resolution is what makes the INVERSE
             * lookup accurate (see header). */
            if (!d_phi_lut_ready)
            {
                for (int i = 0; i < d_phi_lut_size; i++)
                {
                    double x = (double)i / (double)d_phi_step;
                    // φ(x) = -ln(tanh(x/2)), x>0. For x=0 this is +inf, which the
                    // clamp below saturates to d_phi_lut_max.
                    double phi = -log(tanh(x / 2.0));
                    double scaled = phi * d_phi_scale;
                    if (scaled > d_phi_lut_max)
                        scaled = d_phi_lut_max;
                    d_phi_lut[i] = (int16_t)lround(scaled);
                }
                d_phi_lut_ready = true;
            }

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

            d_vns = new int16_t[d_pcm_num_vn];
            d_vns_to_cn_msgs = new int16_t[d_pcm_max_cn_degree];
            d_cn_to_vn_msgs = new int16_t[d_pcm_num_cn * d_pcm_max_cn_degree];
            d_abs_msgs = new int16_t[d_pcm_max_cn_degree];

            d_vn_addr = new int16_t *[d_pcm_num_edges];
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

        LDPCDecoderGeneric::~LDPCDecoderGeneric()
        {
            delete[] d_vns;
            delete[] d_vns_to_cn_msgs;
            delete[] d_abs_msgs;
            delete[] d_cn_to_vn_msgs;
            delete[] d_prev_vn_to_cn_msgs;
            delete[] d_sc_msgs;
            delete[] d_vn_addr;
            delete[] d_row_pos_deg;
        }

        void LDPCDecoderGeneric::set_algorithm(ldpc_algorithm_t a)
        {
            d_algorithm = a;

            if (a == LDPC_SELF_CORRECTED_MIN_SUM && d_prev_vn_to_cn_msgs == nullptr)
            {
                d_prev_vn_to_cn_msgs = new int16_t[d_pcm_num_cn * d_pcm_max_cn_degree];
                d_sc_msgs = new int16_t[d_pcm_max_cn_degree];
            }
        }

        int LDPCDecoderGeneric::decode(uint8_t *out, const int8_t *in, int it)
        {
            int corrections = 0;

            /* The length of the input block should correspond to the length of a codeword. */
            // if (len != d_pcm->code.n)
            // {
            //     return -1;
            // }

            /* Copy the input codeword and give lowest LLR value to each punctured
             * bit. */
            for (int i = 0; i < d_pcm_num_vn; i++)
            {
                d_vns[i] = (int16_t)in[i];
            }

            /* Init of CN to VN messages */
            for (int i = 0; i < d_pcm_num_cn * d_pcm_max_cn_degree; i++)
            {
                d_cn_to_vn_msgs[i] = 0;
            }

            if (d_algorithm == LDPC_SELF_CORRECTED_MIN_SUM)
                for (int i = 0; i < d_pcm_num_cn * d_pcm_max_cn_degree; i++)
                    d_prev_vn_to_cn_msgs[i] = 0;

            /* Decode step */
            int it_used = 0;
            while (it--)
            {
                for (int cn_idx = 0; cn_idx < d_pcm_num_cn; cn_idx++)
                {
                    generic_cn_kernel(cn_idx);
                }
                it_used++;

                /* Early termination: compute the syndrome (parity) of every check node
                 * from the current hard decisions (sign >= 0 => 1, else 0). If every
                 * check node has even parity the codeword has converged, so stop. */
                bool converged = true;
                for (int cn_idx = 0; cn_idx < d_pcm_num_cn; cn_idx++)
                {
                    int row_base = d_row_pos_deg[cn_idx * 2];
                    int deg = d_row_pos_deg[cn_idx * 2 + 1];
                    int16_t parity = 0;
                    for (int vn_idx = 0; vn_idx < deg; vn_idx++)
                        parity ^= (int16_t)(*d_vn_addr[row_base + vn_idx] >= 0 ? 1 : 0);
                    if (parity != 0)
                    {
                        converged = false;
                        break;
                    }
                }

                if (converged)
                    break;
            }

            d_last_iterations = it_used;

            /* Hard decision */
            for (int i = 0; i < d_pcm_num_vn; i++)
            {
                out[i] = (uint8_t)(d_vns[i] >= 0 ? 1 : 0);

                if (i < d_pcm_num_vn - d_pcm_num_cn)
                    if ((out[i] > 0) != (in[i] > 0))
                        corrections++;
            }

            return corrections;
        }

        void LDPCDecoderGeneric::generic_cn_kernel(int cn_idx)
        {
            /* Given an indexed CN, gather the messages of all VNs connected
             * to that CN and determine a new estimation for each of the VNs. */

            cn_row_base = d_row_pos_deg[cn_idx * 2];
            cn_deg = d_row_pos_deg[cn_idx * 2 + 1];
            cn_offset = d_pcm_max_cn_degree * cn_idx;

            for (int vn_idx = 0; vn_idx < cn_deg; vn_idx++)
            {
                d_vns_to_cn_msgs[vn_idx] = *d_vn_addr[cn_row_base + vn_idx] - d_cn_to_vn_msgs[cn_offset + vn_idx];
                // printf("%d \n", d_vns_to_cn_msgs[vn_idx]);
            }

            /* Self-corrected min-sum: erase messages whose sign flipped since the last
             * iteration, as those are considered unreliable. Only the check node
             * computation sees the erased values; the VN update below still uses the
             * true extrinsic, otherwise the VN would lose its channel information. */
            const int16_t *cn_in = d_vns_to_cn_msgs;

            if (d_algorithm == LDPC_SELF_CORRECTED_MIN_SUM)
            {
                for (int vn_idx = 0; vn_idx < cn_deg; vn_idx++)
                {
                    int16_t new_m = d_vns_to_cn_msgs[vn_idx];
                    int16_t prev_m = d_prev_vn_to_cn_msgs[cn_offset + vn_idx];

                    if (prev_m != 0 && ((prev_m ^ new_m) < 0)) // Signs differ
                        new_m = 0;

                    d_prev_vn_to_cn_msgs[cn_offset + vn_idx] = new_m;
                    d_sc_msgs[vn_idx] = new_m;
                }

                cn_in = d_sc_msgs;
            }

            /* Sum-product (belief propagation) check node, log domain.
             *
             *   λ_ji = Π_{i'≠j} sign(m_i') · φ( Σ_{i'≠j} φ(|m_i'|) )
             *
             * with φ(x) = -ln(tanh(x/2)), x>0, φ its own inverse. We accumulate the
             * total φ sum and the total sign (XOR of all sign bits) over the connected
             * VNs, then for each VN subtract its own contribution to get the extrinsic
             * value. The sign convention matches the rest of the decoder: a negative
             * output drives the VN toward a hard '0' and a positive one toward '1'
             * (the final hard decision is d_vns[i] >= 0 ? 1 : 0). */
            if (d_algorithm == LDPC_SUM_PRODUCT)
            {
                int64_t total_phi = 0;
                int16_t total_sign = 0;

                for (int vn_idx = 0; vn_idx < cn_deg; vn_idx++)
                {
                    msg = cn_in[vn_idx];
                    total_sign ^= msg;
                    int mag = abs(msg);
                    int idx = mag * d_phi_step;
                    if (idx >= d_phi_lut_size)
                        idx = d_phi_lut_size - 1;
                    total_phi += d_phi_lut[idx];
                }

                for (int vn_idx = 0; vn_idx < cn_deg; vn_idx++)
                {
                    msg = cn_in[vn_idx];
                    int mag = abs(msg);
                    int idx = mag * d_phi_step;
                    if (idx >= d_phi_lut_size)
                        idx = d_phi_lut_size - 1;

                    /* Extrinsic φ sum (exclude this message). */
                    int64_t phi_excl = total_phi - d_phi_lut[idx];

                    /* Inverse lookup. φ is its own inverse. The LUT stores
                     * φ(x)*d_phi_scale indexed by x*d_phi_step, so the check-node
                     * output in the SAME "natural" units as the channel LLRs is
                     * LUT[phi_excl*d_phi_step/d_phi_scale] / d_phi_scale. */
                    int64_t idx64 = (phi_excl * d_phi_step + d_phi_scale / 2) / d_phi_scale;
                    if (idx64 >= d_phi_lut_size)
                        idx64 = d_phi_lut_size - 1;
                    int32_t mag_out = (d_phi_lut[(int)idx64] + d_phi_scale / 2) / d_phi_scale;

                    /* Sign = product of the other messages' signs = XOR of their sign
                     * bits. total_sign ^ msg gives the sign of all messages except this
                     * one. */
                    new_msg = (total_sign ^ msg) < 0 ? (int16_t)-mag_out : (int16_t)mag_out;

                    /* Add error correction value */
                    to_vn = new_msg + d_vns_to_cn_msgs[vn_idx];

                    /* Save new soft bit value and CN to VN message */
                    d_cn_to_vn_msgs[cn_offset + vn_idx] = new_msg;
                    *d_vn_addr[cn_row_base + vn_idx] = to_vn;
                }

                return;
            }

            parity = 0;
            min1 = UINT8_MAX;
            min2 = UINT8_MAX;

            if (cn_deg & 0x1)
                parity = ~parity;

            /* Compute the parity of all soft bits represented by the VNs to CN
             * messages, the absolute value of each soft bit, and the overall
             * first and second minimums. All these results are used later to
             * determine a new estimation sent back to each of the VNs connected
             * to the current CN. */
            for (int vn_idx = 0; vn_idx < cn_deg; vn_idx++)
            {
                msg = cn_in[vn_idx];
                parity ^= msg;

                /* Bit-hack to compute the absolute value of the message */
                // abs_mask = msg >> (sizeof(msg) * 8 - 1);
                // abs_msg = (int16_t)((msg + abs_mask) ^ abs_mask);
                abs_msg = abs(msg);

                /* Determine the first and second minimum */
                min2 = min2 > abs_msg ? (min1 > abs_msg ? min1 : abs_msg) : min2;
                min1 = min1 > abs_msg ? abs_msg : min1;

                /* Keep the computed absolute value for later */
                d_abs_msgs[vn_idx] = abs_msg;
                // printf("%d \n", d_abs_msgs[vn_idx]);
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
                equ_min1 = (~(uint16_t(d_abs_msgs[vn_idx] == min1))) + 1; // 0 or -1
                min = (min1 & ~equ_min1) | (min2 & equ_min1);

                /* Normalized Min-Sum: scale magnitude by alpha (Q8) */
                if (d_algorithm == LDPC_NORMALIZED_MIN_SUM)
                    min = (uint16_t)(((uint32_t)min * (uint32_t)d_nms_alpha_q8) >> 8);

                sign = (parity ^ cn_in[vn_idx]);

                /* Bit hack in order to multiply by the sign */
                sign = (sign >> (sizeof(sign) * 8 - 1));
                new_msg = (int16_t)((min + sign) ^ sign);

                /* Add error correction value */
                to_vn = new_msg + d_vns_to_cn_msgs[vn_idx];

                /* Save new soft bit value and CN to VN message */
                d_cn_to_vn_msgs[cn_offset + vn_idx] = new_msg;
                *d_vn_addr[cn_row_base + vn_idx] = to_vn;
            }
        }
    } // namespace ldpc
} // namespace codings