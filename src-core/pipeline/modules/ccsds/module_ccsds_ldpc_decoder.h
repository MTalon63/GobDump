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
                int d_ldpc_iterations;                  // LDPC Iterations (required param, legacy cap)
                int d_ldpc_cap_iterations;              // Effective iteration cap (max_iterations override, else ldpc_iterations)

                codings::ldpc::ldpc_algorithm_t d_ldpc_algorithm = codings::ldpc::LDPC_MIN_SUM;
                int16_t d_ldpc_nms_alpha_q8 = 205; // ~0.8, only used by (layered) normalized min-sum
                int16_t d_ldpc_offset_beta_q8 = 0; // 0 = offset min-sum off (Q8); used by (layered) min-sum

                // Iteration / early-termination controls.
                int d_ldpc_max_iterations = 50; // Cap used when ldpc_max_iterations is given
                int d_ldpc_min_iterations = 1;  // Minimum iterations before early termination
                bool d_ldpc_early_termination = true;
                // LLR output clamp applied to the decoded soft hard decisions.
                int d_ldpc_llr_clamp = 127;

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

                // Higher-order (8PSK / 16APSK) front-end.
                bool d_is_higher_order = false;
                int d_bits_per_sym = 1;
                int d_num_phases = 1;
                int d_current_phase = 0;
                bool hd_tracking = false;
                bool hd_syncing = false;
                bool hd_grid_ok = false; // frame grid confirmed by a good ASM bracket
                // Frame accounting; the CADU count is quantised to the batch of 16.
                uint64_t hd_frames_fed = 0;
                uint64_t hd_frames_dropped = 0;
                uint64_t hd_frames_weak_fed = 0;
                int hd_sync_fail_count = 0;
                // Acquisition chunk: >= 2 frames, so the phase is scored on two ASMs
                // (8192 symbols = 3 frames at 8PSK, 4 at 16APSK).
                static constexpr int HD_CHUNK_SYMS = 8192;
                int8_t *hd_sym_buffer = nullptr;
                int8_t *hd_llr_buffer = nullptr;
                int8_t *hd_llr_fifo = nullptr;
                int hd_fifo_len = 0;
                int hd_fifo_cap = 0;
                float hd_phase_cos[16] = {0};
                float hd_phase_sin[16] = {0};
                float d_correlator_threshold = 0.5f;
                std::vector<uint8_t> hd_asm_bits;
                // One matched filter per bit phase, sized for up to 8 bits/symbol.
                float hd_asm_ref[8][64] = {};
                float hd_asm_ref_sq[8] = {0};
                int hd_fifo_bit_phase = 0; // bit phase (mod d_bits_per_sym) of hd_llr_fifo[0]
                bool d_conjugate = false;
                std::unique_ptr<dsp::constellation_t> hd_constellation;

                // 16APSK demapper calibration: npwr from the per-chunk MER plus an int8
                // LLR gain. At npwr = 1 the two rings put neighbours on both sides of a
                // bit decision and ~13% of LLR signs come out wrong. Off = npwr 1 / 50.
                //
                // "16apsk" = dsp::APSK16_CCSDS; the legacy dsp::APSK16 mapping is not used.
                float d_gamma = 2.60f;       // 16APSK ring ratio (TX/RX must match)
                bool d_llr_noise_cal = false; // true for 16APSK
                float d_demap_npwr = 1.0f;   // per-axis noise variance used for demapping (fixed per lock)
                float d_npwr_implied = 1.0f; // latest per-axis noise variance implied by the MER
                float d_npwr_smooth = 0.0f;  // smoothed version, the one actually used
                int d_mer_updates = 0;
                int d_mer_settle = 2;        // chunks to ignore after a reset (estimator warm-up)
                float d_llr_gain = 50.0f;    // current const_sca for the demapper
                float d_llr_target = 28.0f;  // mean |LLR| the gain controller aims for
                bool d_mer_valid = false;
                void hd_build_asm_ref(float npwr);
                void hd_update_llr_gain(const int8_t *llr, int count);

                std::unique_ptr<CorrelatorGeneric> correlator;
                std::unique_ptr<codings::ldpc::CCSDSLDPC> ldpc_dec;
                std::unique_ptr<labrador::code_params_t> ldpc2_params;
                std::unique_ptr<deframing::BPSK_CCSDS_Deframer> deframer;

                // UI Stuff
                float ber_history[200];
                dsp::Random rng;

                // UI Stuff
                float cor_history[200];

                float correlator_cor = 0.0f;
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
                int ldpc_corr = 0;

                // LDPC iterations actually used (with early termination) and its history.
                int ldpc_iterations_used = 0;
                float ldpc_iter_history[200];

                // Whether the last batch satisfied the syndrome (H*ch == 0); only set when exposed.
                bool ldpc_last_converged = false;

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
                // LLR scaling mode. true = calibrated 0.5*sqrt(sig/noi) (default),
                // false = legacy heuristic 1/npwr (for A/B comparison).
                bool d_llr_calibrated = true;

                bool is_started = false;

                // 8PSK helpers.
                void feed_frame(int8_t *frame);
                // Valid frames in the batch (-1 = full), used to flush at EOF.
                void decode_batch(int lanes = -1);
                void process_higher_order();
                void demap_higher_order(const int8_t *sym, int nsyms, int phase, bool conjugate, int8_t *out);
                float correlate_asm(const int8_t *bits, int pos, int nbits, const float *ref, float ref_sq, float &raw_out);
                bool find_asm_window(const int8_t *bits, int center, int half, int nbits, int bit_phase, int &pos, float &corr, float &raw);
                void hd_append_llrs(const int8_t *llr, int count);

            public:
                CCSDSLDPCDecoderModule(std::string input_file, std::string output_file_hint, nlohmann::json parameters);
                ~CCSDSLDPCDecoderModule();
                void process();
                void drawUI(bool window);

                nlohmann::json getModuleStats();

            public:
                static std::string getID();
                virtual std::string getIDM() { return getID(); };
                static nlohmann::json getParams()
                {
                    nlohmann::json v;
                    v["constellation"] = "bpsk";
                    v["ldpc_rate"] = "7/8";
                    v["ldpc_block_size"] = 0;
                    v["ldpc_iterations"] = 10;
                    v["ldpc_algorithm"] = "min_sum";
                    v["ldpc_nms_alpha"] = 0.8;
                    v["ldpc_offset_beta"] = 0.0;
                    v["ldpc_max_iterations"] = 50;
                    v["ldpc_min_iterations"] = 1;
                    v["ldpc_early_termination"] = true;
                    v["ldpc_llr_clamp"] = 127;
                    v["derandomize"] = true;
                    v["long_poly"] = false;
                    v["ccsds"] = true;
                    v["ldpc2"] = false;
                    v["internal_stream"] = false;
                    v["llr_scaling"] = "calibrated";
                    v["correlator_threshold"] = 0.5;
                    v["gamma"] = 2.60;
                    v["llr_noise_calibration"] = true;
                    return v;
                }
                static std::shared_ptr<ProcessingModule> getInstance(std::string input_file, std::string output_file_hint, nlohmann::json parameters);
            };
        } // namespace ccsds
    } // namespace pipeline
} // namespace satdump