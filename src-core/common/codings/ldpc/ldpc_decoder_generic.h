#pragma once

#include "ldpc_decoder.h"

// Based on gr-ccsds
namespace codings
{
    namespace ldpc
    {
        class LDPCDecoderGeneric : public LDPCDecoder
        {
        public:
            LDPCDecoderGeneric(Sparse_matrix pcm);
            ~LDPCDecoderGeneric();

            int decode(uint8_t *out, const int8_t *in, int it);

            int simd() { return 1; }

            void set_algorithm(ldpc_algorithm_t a);

        private:
            int d_pcm_num_cn;
            int d_pcm_num_vn;
            int d_pcm_max_cn_degree;
            int d_pcm_num_edges;

            /* Buffer holding vn values during decode */
            int16_t *d_vns;
            /* Buffer to hold all messages coming from VNs to a single CN */
            int16_t *d_vns_to_cn_msgs;
            /* Buffer to hold the absolute value of messages coming from VNs
             * to CNs */
            int16_t *d_abs_msgs;
            /* Buffer to hold all messages from CNs to VNs containing
             * error correcting values. */
            int16_t *d_cn_to_vn_msgs;
            /* Previous iteration's VN to CN messages, and the sign-flip erased view of
             * the current ones. Only allocated for self-corrected min-sum. */
            int16_t *d_prev_vn_to_cn_msgs = nullptr;
            int16_t *d_sc_msgs = nullptr;

            /* Buffer representing the parity check matrix. Instead of having all offsets of
             * each VN in the VN buffer, we store directly its addresses in the buffer */
            int16_t **d_vn_addr;
            /* Array of row position and degree of each cn */
            int *d_row_pos_deg;

            void generic_cn_kernel(int cn_idx);

            /* Sum-product (belief propagation) φ lookup table.
             *
             * φ(x) = -ln(tanh(x/2)) = ln((e^x+1)/(e^x-1)), x>0, and φ is its own
             * inverse. The table is shared by all instances (static) and built once.
             *
             * The LUT is indexed by a FINE grid of the φ-domain argument:
             *   d_phi_lut[i] = round(φ(i / d_phi_step) * d_phi_scale), clamped.
             * d_phi_step = 16 gives 1/16 resolution in the argument, which is
             * essential for the INVERSE lookup: the summed φ values (phi_excl) are
             * often small (< 1), and with a coarse integer-indexed table they all
             * rounded to index 0 and returned the saturated max, over-correcting and
             * preventing convergence at low SNR.
             *
             * Because φ is its own inverse, the same table serves both lookups:
             *   - forward:  φ(|m_i|)  -> d_phi_lut[|m_i| * d_phi_step]
             *   - inverse:  φ(phi_excl) -> d_phi_lut[phi_excl * d_phi_step / d_phi_scale]
             *
             * d_phi_scale = 1024 keeps the small-magnitude φ values (which dominate
             * the check-node sum) in a range that fits int16 while preserving enough
             * precision. d_phi_lut_max saturates φ(0)=∞ to a value that, after
             * dividing back by d_phi_scale, gives a check-node output magnitude
             * comparable to the channel LLRs (roughly tens).
             */
            static const int d_phi_lut_size = 2048;
            static const int d_phi_step = 16;   // argument resolution: 1/16
            static const int d_phi_scale = 1024;
            /* Saturates φ(0)=∞ to a value that, after dividing back by d_phi_scale,
             * gives a check-node output magnitude comparable to the (normalized)
             * channel LLRs. The BP path normalizes its input to a peak of
             * d_bp_peak (8), so the check output should saturate around that:
             * 8 * d_phi_scale = 8192. d_phi_lut is int16, so keep well under 32767. */
            static const int d_phi_lut_max = 8192;
            /* Peak magnitude the BP path normalizes its channel LLRs to, so the
             * φ-LUT operates in its meaningful range regardless of the pipeline's
             * LLR scaling. */
            static const int d_bp_peak = 8;
            static int16_t d_phi_lut[2048];
            static bool d_phi_lut_ready;

            // Used by generic_cn_kernel
            int16_t sign;
            int16_t parity;
            uint16_t min, min1, min2;
            uint16_t abs_msg;
            int16_t new_msg, msg;
            int16_t abs_mask;
            int16_t equ_min1;
            int16_t to_vn;
            int cn_deg;
            int cn_row_base; // offset to base of corresponding row in the PCM
            int vn_offset;
            int cn_offset; // CN offset in the buffer holding CN to VN messages
        };
    }
}
