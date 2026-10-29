#pragma once

#include "common/dsp/complex.h"
#include "dsp/block.h"
#include "dsp/block_simple.h"
#include <cstdint>
#include <vector>

namespace satdump
{
    namespace ndsp
    {
        class QuadratureDemodBlock : public BlockSimple<complex_t, float>
        {
        private:
            float gain = 1;
            complex_t qd_last = 1;          // one-sample history (unit => first call matches the scalar reference)
            std::vector<complex_t> qd_in;   // capacity nsamples + 1: [previous, current...]
            std::vector<complex_t> qd_prod; // capacity nsamples: in[i] * conj(in[i - 1])

        public:
            QuadratureDemodBlock();
            ~QuadratureDemodBlock();

            uint32_t process(complex_t *input, uint32_t nsamples, float *output);

            void init() {}

            nlohmann::ordered_json get_cfg_list()
            {
                nlohmann::ordered_json p;
                add_param_simple(p, "gain", "float");
                return p;
            }

            nlohmann::json get_cfg(std::string key)
            {
                if (key == "gain")
                    return 1.0 / gain;
                else
                    throw satdump_exception(key);
            }

            cfg_res_t set_cfg(std::string key, nlohmann::json v)
            {
                if (key == "gain")
                    gain = 1.0 / ((double)v);
                else
                    throw satdump_exception(key);
                return RES_OK;
            }
        };
    } // namespace ndsp
} // namespace satdump