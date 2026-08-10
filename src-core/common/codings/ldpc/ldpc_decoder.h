#pragma once

#include "matrix/sparse_matrix.h"
#include <functional>
#include <map>


// Based on gr-ccsds
namespace codings
{
    namespace ldpc
    {
        class LDPCDecoder
        {
        public:
            LDPCDecoder(Sparse_matrix pcm);
            virtual ~LDPCDecoder();
            virtual int decode(uint8_t *out, const int8_t *in, int it) = 0;
            virtual int simd() = 0;

            void set_oms_beta(int16_t b) { d_oms_beta = b; }

        protected:
            int16_t d_oms_beta = 0;
        };

        struct GetLDPCDecodersEvent
        {
            std::map<std::string, std::function<LDPCDecoder *(Sparse_matrix)>> &decoder_list;
        };

        LDPCDecoder *get_best_ldpc_decoder(Sparse_matrix pcm);
    } // namespace ldpc
} // namespace codings
