#pragma once

#include <cstdint>
#include "ldpc_decoder.h"

namespace codings
{
    namespace ldpc
    {
        enum ldpc_rate_t
        {
            RATE_1_2,
            RATE_2_3,
            RATE_4_5,
            RATE_7_8,
        };

        ldpc_rate_t ldpc_rate_from_string(std::string str);

        class CCSDSLDPC
        {
        private:
            ldpc_rate_t d_rate;
            int d_block_size;

            int8_t *depunc_buffer_in;
            uint8_t *depunc_buffer_ou;

            int d_codeword_size;
            int d_frame_size;
            int d_data_size;
            int d_is_geneneric;
            int d_simd;
            int d_M;

            int d_corr_errors;

            int d_last_iterations = 0;

            LDPCDecoder *ldpc_decoder;

            void init_dec(Sparse_matrix pcm);

        public:
            CCSDSLDPC(ldpc_rate_t rate, int block_size);
            ~CCSDSLDPC();

            // Get specifics of the current code
            int frame_length() { return d_frame_size; }
            int codeword_length() { return d_codeword_size; }
            int data_length() { return d_data_size; }
            int simd() { return d_simd; }

            // Number of iterations used by the last decode() call. For the SIMD path this
            // is the iteration count of the whole batch (all lanes converged together).
            int last_iterations() const { return d_last_iterations; }

            // Decode a LDPC codeword, takes soft-bits floats in, outputs bytes. Retuns corrected bits
            int decode(int8_t *codeword, uint8_t *frame, int iterations = 10);

            // Normalized Min-Sum alpha, Q8 fixed-point (256 == 1.0).
            void set_nms_alpha(int16_t a_q8) { ldpc_decoder->set_nms_alpha(a_q8); }

            // Offset Min-Sum beta, Q8 fixed-point (256 == 1.0). 0 disables the offset.
            void set_ldpc_offset_beta(int16_t b_q8) { ldpc_decoder->set_ldpc_offset_beta(b_q8); }

            // Early-termination controls: minimum iterations before checking the
            // syndrome, and whether to stop once the codeword has converged.
            void set_min_iterations(int m) { ldpc_decoder->set_min_iterations(m); }
            void set_early_termination(bool e) { ldpc_decoder->set_early_termination(e); }

            void set_algorithm(ldpc_algorithm_t a) { ldpc_decoder->set_algorithm(a); }

            // Whether the last decode() call satisfied the parity-check syndrome.
            bool converged() const { return ldpc_decoder->converged(); }
        };
    }
}