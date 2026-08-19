#include "ldpc_decoder.h"
#include "core/plugin.h"

#include "common/cpu_features.h"
#include "logger.h"

#include "ldpc_decoder_generic.h"

#include <cstdlib>
#include <stdexcept>

namespace codings
{
    namespace ldpc
    {
        ldpc_algorithm_t ldpc_algorithm_from_string(std::string str)
        {
            if (str == "min_sum" || str == "ms")
                return LDPC_MIN_SUM;
            else if (str == "normalized_min_sum" || str == "nms")
                return LDPC_NORMALIZED_MIN_SUM;
            else if (str == "self_corrected_min_sum" || str == "scms")
                return LDPC_SELF_CORRECTED_MIN_SUM;
            else if (str == "sum_product" || str == "belief_propagation" || str == "bp")
                return LDPC_SUM_PRODUCT;
            else
                throw std::runtime_error("Invalid LDPC algorithm " + str);
        }

        std::string ldpc_algorithm_to_string(ldpc_algorithm_t a)
        {
            if (a == LDPC_NORMALIZED_MIN_SUM)
                return "normalized_min_sum";
            else if (a == LDPC_SELF_CORRECTED_MIN_SUM)
                return "self_corrected_min_sum";
            else if (a == LDPC_SUM_PRODUCT)
                return "sum_product";
            else
                return "min_sum";
        }

        LDPCDecoder::LDPCDecoder(Sparse_matrix)
        {
        }

        LDPCDecoder::~LDPCDecoder()
        {
        }

        LDPCDecoder *get_best_ldpc_decoder(Sparse_matrix pcm)
        {
            std::map<std::string, std::function<LDPCDecoder *(Sparse_matrix)>> decoder_list;

            satdump::eventBus->fire_event<GetLDPCDecodersEvent>({decoder_list});
            decoder_list.insert({"generic", [](Sparse_matrix pcm)
                                 { return new LDPCDecoderGeneric(pcm); }});

            cpu_features::cpu_features_t cpu_caps = cpu_features::get_cpu_features();

            // The SIMD kernels don't currently produce bit-identical results to the
            // generic one, so allow forcing it while that's investigated.
            if (const char *force = std::getenv("GOBDUMP_LDPC_GENERIC"))
            {
                if (force[0] == '1')
                {
                    logger->info("LDPC: forcing generic decoder (GOBDUMP_LDPC_GENERIC=1)");
                    return decoder_list["generic"](pcm);
                }
            }

            std::string chosen = "generic";
            if (cpu_caps.CPU_X86_AVX2 && decoder_list.count("avx2") > 0)
                chosen = "avx2";
            else if (cpu_caps.CPU_X86_SSE41 && decoder_list.count("sse41") > 0)
                chosen = "sse41";
            else if (cpu_caps.CPU_ARM_NEON && decoder_list.count("neon") > 0)
                chosen = "neon";

            logger->debug("LDPC: using %s decoder", chosen.c_str());
            return decoder_list[chosen](pcm);
        }
    }
}