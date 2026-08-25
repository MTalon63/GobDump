#pragma once

#include "common/codings/deframing/bpsk_ccsds_deframer.h"
#include "common/codings/generic_correlator.h"
#include "common/codings/ldpc/ccsds_ldpc.h"
#include "common/codings/ldpc/labrador/decoder.h"
#include "common/dsp/demod/constellation.h"
#include "common/dsp/utils/random.h"
#include "common/dsp/utils/snr_estimator.h"
#include "pipeline/modules/base/filestream_to_filestream.h"
#include <vector>

namespace satdump
{
    namespace pipeline
    {
        namespace ccsds
        {
            /*
            This decoder is meant to decode LDPC codes from the
            CCSDS 131.0 Specification. Everything from the 7/8
            code to the AR4JA 1/2, 2/3, 3/4 codes should be covered.
            Optionally, the actual CADUs can be in a stream of STMFs.
            */
            class CCSDSLDPCDecoderModule : public base::FileStreamToFileStreamModule
            {
            protected:
                const bool is_ccsds; // Just to know if we should output .cadu or .frm
                const bool use_ldpc2;

                const std::string d_constellation_str;     // Constellation type string
                dsp::constellation_type_t d_constellation; // Constellation type
                // const bool d_iq_invert;                    // For some QPSK sats, can need to be inverted...

                const bool d_derand;           // Perform derandomizion or not
                const bool d_derand_long_poly; // If true, use 17-bit LFSR

                std::string d_ldpc_rate_str;            // LDPC Rate string
                codings::ldpc::ldpc_rate_t d_ldpc_rate; // LDPC Rate
                int d_ldpc_block_size;                  // LDPC Block size (for AR4JA only)
                int d_ldpc_iterations;                  // LDPC Iterations

                codings::ldpc::ldpc_algorithm_t d_ldpc_algorithm = codings::ldpc::LDPC_MIN_SUM;
                int16_t d_ldpc_nms_alpha_q8 = 205; // ~0.8, only used by normalized min-sum

                const bool d_internal_stream; // Does this have an internal CADU stream?
                const int d_cadu_size;        // CADU Size in bits, including ASM
                const int d_cadu_bytes;       // CADU Size in bytes, including ASM

                int d_ldpc_frame_size;
                int d_ldpc_codeword_size;
                int d_ldpc_asm_size;
                int d_ldpc_simd;
                int d_ldpc_data_size;

                int8_t *soft_buffer;
                int frames_in_ldpc_buffer;
                int8_t *ldpc_input_buffer;
                uint8_t *ldpc_output_buffer;
                uint8_t *deframer_buffer;

                std::unique_ptr<CorrelatorGeneric> correlator;
                std::unique_ptr<codings::ldpc::CCSDSLDPC> ldpc_dec;
                std::unique_ptr<labrador::code_params_t> ldpc2_params;
                std::unique_ptr<deframing::BPSK_CCSDS_Deframer> deframer;

                // UI Stuff
                float ber_history[200];
                dsp::Random rng;

                // UI Stuff
                float cor_history[200];

                float correlator_cor;
                float correlator_corr_norm = 0.0f;
                bool correlator_locked = false;

                // Correlator lock state machine (mirrors the CCSDS deframer):
                // require N consecutive good (above-threshold) correlations to lock,
                // M consecutive failures to drop back to NOSYNC.
                int correlator_lock_after = 3;
                int correlator_drop_after = 5;
                int correlator_good_count = 0;
                int correlator_bad_count = 0;
                // Optional search window (in symbols) around the expected ASM position
                // used while locked. 0 = search the whole frame.
                int correlator_search_window = 0;

                float ldpc_history[200];
                int ldpc_corr;

                // LDPC iterations actually used (with early termination) and its history.
                int ldpc_iterations_used = 0;
                float ldpc_iter_history[200];

                static constexpr int SNR_ESTIMATOR_SAMPLES = 4096;
                EVMSNREstimator snr_estimator{4};

                EVMSNREstimator mer_estimator{4};
                float mer_db = 0.0f;
                float mer_history[200] = {0};
                float peak_mer = 0.0f;
                float avg_mer = 0.0f;
                std::vector<complex_t> snr_sample_buffer = std::vector<complex_t>(SNR_ESTIMATOR_SAMPLES);
                int8_t llr_scale_lut[256];
                float llr_snr = 0;
                float llr_scale = 1.0f;
                float llr_sigma2 = 0;   // Noise variance of int8 soft samples (calibrated mode only)
                float llr_scale_history[200];
                // LLR scaling mode. true  = calibrated 2/sigma^2 (default),
                // false = legacy heuristic 1/npwr (for A/B comparison).
                bool d_llr_calibrated = true;

                bool is_started = false;

            public:
                CCSDSLDPCDecoderModule(std::string input_file, std::string output_file_hint, nlohmann::json parameters);
                ~CCSDSLDPCDecoderModule();
                void process();
                void drawUI(bool window);

                nlohmann::json getModuleStats();

            public:
                static std::string getID();
                virtual std::string getIDM() { return getID(); };
                static nlohmann::json getParams() { return {}; } // TODOREWORK
                static std::shared_ptr<ProcessingModule> getInstance(std::string input_file, std::string output_file_hint, nlohmann::json parameters);
            };
        } // namespace ccsds
    } // namespace pipeline
} // namespace satdump