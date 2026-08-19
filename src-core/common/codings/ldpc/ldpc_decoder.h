#pragma once

#include "matrix/sparse_matrix.h"
#include <functional>
#include <map>
#include <string>


// Based on gr-ccsds
namespace codings
{
    namespace ldpc
    {
        enum ldpc_algorithm_t
        {
            LDPC_MIN_SUM,                // Plain min-sum (optionally offset via beta)
            LDPC_NORMALIZED_MIN_SUM,     // Min-sum scaled by alpha
            LDPC_SELF_CORRECTED_MIN_SUM, // Min-sum, erasing sign-flipping VN->CN messages
            LDPC_SUM_PRODUCT,            // True sum-product (belief propagation) check node
        };

        ldpc_algorithm_t ldpc_algorithm_from_string(std::string str);
        std::string ldpc_algorithm_to_string(ldpc_algorithm_t a);

        class LDPCDecoder
        {
        public:
            LDPCDecoder(Sparse_matrix pcm);
            virtual ~LDPCDecoder();
            virtual int decode(uint8_t *out, const int8_t *in, int it) = 0;
            virtual int simd() = 0;

            // Alpha is Q8 fixed-point (256 == 1.0). Only used by LDPC_NORMALIZED_MIN_SUM.
            void set_nms_alpha(int16_t a_q8) { d_nms_alpha_q8 = a_q8; }

            virtual void set_algorithm(ldpc_algorithm_t a) { d_algorithm = a; }

            // Number of iterations actually used by the last decode() call. With early
            // termination this is <= the requested iteration count; without it, it equals
            // the requested count. For SIMD decoders this is the count for the whole batch.
            virtual int last_iterations() const { return d_last_iterations; }

        protected:
            int16_t d_nms_alpha_q8 = 205; // ~0.8
            ldpc_algorithm_t d_algorithm = LDPC_MIN_SUM;
            int d_last_iterations = 0;
        };

        struct GetLDPCDecodersEvent
        {
            std::map<std::string, std::function<LDPCDecoder *(Sparse_matrix)>> &decoder_list;
        };

        LDPCDecoder *get_best_ldpc_decoder(Sparse_matrix pcm);
    } // namespace ldpc
} // namespace codings
