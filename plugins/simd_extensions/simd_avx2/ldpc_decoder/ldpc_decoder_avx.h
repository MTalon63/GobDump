#pragma once

#include "common/codings/ldpc/ldpc_decoder.h"
#include "common/codings/ldpc/ldpc_decoder_generic.h"

#include <xmmintrin.h>
#include <tmmintrin.h>
#include <smmintrin.h>
#include <immintrin.h>

// Based on gr-ccsds, modified to utilize SIMD
namespace codings
{
    namespace ldpc
    {
        class LDPCDecoderAVX : public LDPCDecoder
        {
        public:
            LDPCDecoderAVX(Sparse_matrix pcm);
            ~LDPCDecoderAVX();

            int decode(uint8_t *out, const int8_t *in, int it);

            int simd() { return 16; }

            void set_algorithm(ldpc_algorithm_t a);

        private:
            int d_pcm_num_cn;
            int d_pcm_num_vn;
            int d_pcm_max_cn_degree;
            int d_pcm_num_edges;

            /* Buffer holding vn values during decode */
            __m256i *d_vns;
            /* Buffer to hold all messages coming from VNs to a single CN */
            __m256i *d_vns_to_cn_msgs;
            /* Buffer to hold the absolute value of messages coming from VNs
             * to CNs */
            __m256i *d_abs_msgs;
            /* Buffer to hold all messages from CNs to VNs containing
             * error correcting values. */
            __m256i *d_cn_to_vn_msgs;
            /* Previous iteration's VN to CN messages, and the sign-flip erased view of
             * the current ones. Only allocated for self-corrected min-sum. */
            __m256i *d_prev_vn_to_cn_msgs = nullptr;
            __m256i *d_sc_msgs = nullptr;

            /* Buffer representing the parity check matrix. Instead of having all offsets of
             * each VN in the VN buffer, we store directly its addresses in the buffer */
            __m256i **d_vn_addr;
            /* Array of row position and degree of each cn */
            int *d_row_pos_deg;

            void generic_cn_kernel(int cn_idx);

            /* Sum-product (belief propagation) fallback.
             *
             * The AVX2 kernel implements the min-sum family only. A correct per-lane
             * sum-product check node would require a per-lane φ gather (16-bit lanes
             * have no direct AVX2 gather), which is risky to get right without a build
             * to validate. Instead, when LDPC_SUM_PRODUCT is selected we fall back to
             * the generic (scalar) decoder, which implements the true BP check node.
             * The 16 interleaved frames are decoded one at a time by the generic
             * decoder and the results are written back to the same output layout, so
             * the rest of the pipeline is unaffected. This is a documented limitation:
             * sum-product runs on the generic path, not the AVX kernel. */
            Sparse_matrix d_pcm;
            LDPCDecoderGeneric *d_generic_fallback = nullptr;

            // Used by generic_cn_kernel
            __m256i sign;
            __m256i parity;
            __m256i min, min1, min2;
            __m256i abs_msg;
            __m256i new_msg;
            __m256i msg;
            __m256i equ_wip;
            __m256i equ_min1;
            __m256i to_vn;
            int cn_deg;
            int cn_row_base; // offset to base of corresponding row in the PCM
            int vn_offset;
            int cn_offset; // CN offset in the buffer holding CN to VN messages
        };
    }
}
