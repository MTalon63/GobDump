#pragma once

#include "common/dsp/clock_recovery/zero_crossing_recovery.h"
#include "common/dsp/demod/quadrature_demod.h"
#include "common/dsp/filter/fir.h"
#include "common/dsp/resamp/rational_resampler.h"
#include "common/dsp/utils/agc2.h"
#include "module_demod_base.h"

namespace satdump
{
    namespace pipeline
    {
        namespace demod
        {
            class GFSKDemodModule : public BaseDemodModule
            {
            protected:
                std::shared_ptr<dsp::FIRBlock<complex_t>> rrc;
                std::shared_ptr<dsp::QuadratureDemodBlock> qua;
                std::shared_ptr<dsp::RationalResamplerBlock<float>> resamp; // may be null
                std::shared_ptr<dsp::AGC2Block<float>> agc2;
                std::shared_ptr<dsp::FIRBlock<float>> lpf;
                std::shared_ptr<dsp::ZeroCrossingRecoveryBlock> rec;
                int8_t *sym_buffer;

                float d_deviation = 0;
                float d_rrc_alpha = 0.35f;
                int d_rrc_taps = 31;
                float d_lpf_transition_width = 0;
                int d_zc_sps = 0;
                float d_zc_alpha = 0.1f;
                float d_soft_scale = 10.0f;
                float d_agc_target = 5.0f;
                float d_agc_bias = 0.01f;
                float d_agc_gain = 0.001f;

                float d_snr_smoothed = 0.0f;
                int d_snr_publish_count = 0;
                static constexpr float k_snr_ema = 0.05f;
                static constexpr int k_snr_publish_buffers = 32;
                double d_resampled_rate = 0;
                int d_sps = 5;
                int d_interp = 1;
                int d_decim = 1;

            public:
                GFSKDemodModule(std::string input_file, std::string output_file_hint, nlohmann::json parameters);
                ~GFSKDemodModule();
                void init();
                void stop();
                void process();

                nlohmann::json getModuleStats();

            public:
                static std::string getID() { return "gfsk_demod"; }
                virtual std::string getIDM() { return getID(); };
                static nlohmann::json getParams();
                static std::shared_ptr<ProcessingModule> getInstance(std::string input_file, std::string output_file_hint, nlohmann::json parameters);
            };
        } // namespace demod
    } // namespace pipeline
} // namespace satdump