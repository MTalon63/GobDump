#include "module_ccsds_ldpc_decoder.h"
#include "common/codings/ldpc/ccsds_ldpc.h"
#include "common/codings/ldpc/labrador/decoder.h"
#include "common/codings/randomization.h"
#include "common/codings/rotation.h"
#include "common/dsp/complex.h"
#include "common/utils.h"
#include "common/widgets/constellation.h"
#include "common/widgets/themed_widgets.h"
#include "core/exception.h"
#include "logger.h"
#include "utils/binary.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <cstdint>
#include <vector>

namespace satdump
{
    namespace pipeline
    {
        namespace ccsds
        {
            CCSDSLDPCDecoderModule::CCSDSLDPCDecoderModule(std::string input_file, std::string output_file_hint, nlohmann::json parameters)
                : base::FileStreamToFileStreamModule(input_file, output_file_hint, parameters),

                  is_ccsds(parameters.count("ccsds") > 0 ? parameters["ccsds"].get<bool>() : true), //
                  use_ldpc2(parameters.count("ldpc2") > 0 ? parameters["ldpc2"].get<bool>() : false),

                  d_constellation_str(parameters["constellation"].get<std::string>()),
                  // d_iq_invert(parameters.count("iq_invert") > 0 ? parameters["iq_invert"].get<bool>() : false),

                  d_derand(parameters.count("derandomize") > 0 ? parameters["derandomize"].get<bool>() : true),
                  d_derand_long_poly(parameters.count("long_poly") > 0 ? parameters["long_poly"].get<bool>() : false),

                  d_ldpc_rate_str(parameters["ldpc_rate"].get<std::string>()), d_ldpc_block_size(parameters.count("ldpc_block_size") > 0 ? parameters["ldpc_block_size"].get<int>() : 0),
                  d_ldpc_iterations(parameters["ldpc_iterations"].get<int>()),

                  d_internal_stream(parameters.count("internal_stream") > 0 ? parameters["internal_stream"].get<bool>() : false),
                  d_cadu_size((parameters.count("internal_stream") > 0 && parameters["internal_stream"].get<bool>())
                                  ? parameters["internal_cadu_size"].get<int>()
                                  : 0),
                  d_cadu_bytes(ceil(d_cadu_size / 8.0)) // If we can't use complete bytes, add one and padding
            {
                // Get constellation
                if (d_constellation_str == "bpsk")
                    d_constellation = dsp::BPSK;
                else if (d_constellation_str == "qpsk")
                    d_constellation = dsp::QPSK;
                else if (d_constellation_str == "oqpsk")
                    d_constellation = dsp::OQPSK;
                else if (d_constellation_str == "8psk")
                {
                    d_constellation = dsp::PSK8;
                    d_is_higher_order = true;
                    d_bits_per_sym = 3;
                    d_num_phases = 16; // Costas can lock on a 22.5 deg half-step
                }
                else if (d_constellation_str == "16apsk")
                {
                    // 8192 bits = 2048 symbols, symbol aligned (8192 % 4 == 0); 4
                    // phases for slips/resync (4-fold rotational symmetry).
                    d_constellation = dsp::APSK16_CCSDS;
                    d_is_higher_order = true;
                    d_bits_per_sym = 4;
                    d_num_phases = 4;
                }
                else
                    throw satdump_exception("CCSDS LDPC Decoder : invalid constellation type! (bpsk, qpsk, oqpsk, 8psk, 16apsk)");

                // Parse LDPC settings
                d_ldpc_rate = codings::ldpc::ldpc_rate_from_string(d_ldpc_rate_str);

                ldpc_dec = std::make_unique<codings::ldpc::CCSDSLDPC>(d_ldpc_rate, d_ldpc_block_size);
                d_ldpc_simd = ldpc_dec->simd();

                if (use_ldpc2)
                {
                    d_ldpc_simd = 1;
                    labrador::ldpc_code_t c;
                    if (d_ldpc_block_size == 1024)
                    {
                        if (d_ldpc_rate == codings::ldpc::RATE_4_5)
                            c = labrador::TM1280;
                        else if (d_ldpc_rate == codings::ldpc::RATE_2_3)
                            c = labrador::TM1536;
                        else if (d_ldpc_rate == codings::ldpc::RATE_1_2)
                            c = labrador::TM2048;
                        else
                            throw satdump_exception("Invalid LDPC Option for LDPC2!");
                    }
                    else if (d_ldpc_block_size == 4096)
                    {
                        if (d_ldpc_rate == codings::ldpc::RATE_4_5)
                            c = labrador::TM5120;
                        else if (d_ldpc_rate == codings::ldpc::RATE_2_3)
                            c = labrador::TM6144;
                        else if (d_ldpc_rate == codings::ldpc::RATE_1_2)
                            c = labrador::TM8192;
                        else
                            throw satdump_exception("Invalid LDPC Option for LDPC2!");
                    }
                    else
                        throw satdump_exception("Invalid LDPC Option for LDPC2!");
                    ldpc2_params = std::make_unique<labrador::code_params_t>(labrador::get_code_params(c));
                }

                if (d_ldpc_rate == codings::ldpc::RATE_7_8)
                    d_ldpc_asm_size = 32;
                else
                    d_ldpc_asm_size = 64;

                d_ldpc_frame_size = ldpc_dec->frame_length() + d_ldpc_asm_size;
                d_ldpc_codeword_size = ldpc_dec->frame_length();
                d_ldpc_data_size = ldpc_dec->data_length();

                // Correlator lock state machine parameters (optional, with defaults).
                correlator_lock_after = parameters.count("correlator_lock_after") > 0 ? parameters["correlator_lock_after"].get<int>() : 3;
                correlator_drop_after = parameters.count("correlator_drop_after") > 0 ? parameters["correlator_drop_after"].get<int>() : 5;
                correlator_search_window = parameters.count("correlator_search_window") > 0 ? parameters["correlator_search_window"].get<int>() : 0;

                // LLR scaling mode: "calibrated" (2/sigma^2, default) or
                // "heuristic" (legacy 1/npwr formula, for A/B regression testing).
                {
                    const std::string llr_scaling = parameters.count("llr_scaling") > 0 ? parameters["llr_scaling"].get<std::string>() : "calibrated";
                    d_llr_calibrated = (llr_scaling == "calibrated");
                }

                int mer_order = d_constellation == dsp::BPSK ? 2 : 4;
                if (d_constellation == dsp::PSK8)
                    mer_order = 8;
                mer_estimator = EVMSNREstimator(mer_order, 0.001f);

                d_correlator_threshold = parameters.count("correlator_threshold") > 0 ? parameters["correlator_threshold"].get<float>() : 0.5f;

                // Calibration off for 8PSK (keeps npwr 1 / const_sca 50).
                if (d_constellation == dsp::APSK16_CCSDS)
                {
                    d_gamma = parameters.count("gamma") > 0 ? parameters["gamma"].get<float>() : 2.60f;
                    d_llr_noise_cal = parameters.count("llr_noise_calibration") > 0 ? parameters["llr_noise_calibration"].get<bool>() : true;
                    d_llr_target = parameters.count("llr_target_mean") > 0 ? parameters["llr_target_mean"].get<float>() : 28.0f;
                }

                if (d_is_higher_order)
                {
                    hd_asm_bits = d_ldpc_rate == codings::ldpc::RATE_7_8
                                      ? satdump::unsigned_to_bitvec<uint32_t>(0x1acffc1d)
                                      : satdump::unsigned_to_bitvec<uint64_t>(0x034776c7272895b0);

                    // Exact demap; the LUT clips 8PSK points.
                    hd_constellation = std::make_unique<dsp::constellation_t>(d_constellation, d_gamma);

                    // ASM matched filters, one per bit phase, at the demapper's npwr.
                    d_llr_gain = hd_constellation->get_scale();

                    if (d_constellation == dsp::APSK16_CCSDS)
                    {
                        // Nearest-point EVM; M2M4 and the PSK grid are invalid here.
                        mer_estimator.set_apsk_points(hd_constellation->get_points(), hd_constellation->get_states());

                        // Start near the 8PSK values; both converge within a few chunks.
                        if (d_llr_noise_cal)
                        {
                            d_demap_npwr = 0.05f; // 10 dB Es/N0; npwr is the per-axis variance
                            d_llr_gain = 5.0f;
                            hd_constellation->set_scale(d_llr_gain);
                        }

                        const complex_t p1 = hd_constellation->get_points()[3]; // inner 45 deg
                        const float r1 = sqrtf(p1.real * p1.real + p1.imag * p1.imag);
                        logger->info("16APSK: gamma %.3f (R1 %.4f R2 %.4f), %d phases, noise-calibrated LLRs: %s",
                                     d_gamma, r1, d_gamma * r1, d_num_phases, d_llr_noise_cal ? "yes" : "no");
                    }

                    hd_build_asm_ref(d_demap_npwr);

                    hd_sym_buffer = new int8_t[2 * HD_CHUNK_SYMS]();
                    hd_llr_buffer = new int8_t[d_bits_per_sym * HD_CHUNK_SYMS]();
                    hd_fifo_cap = d_bits_per_sym * HD_CHUNK_SYMS + d_ldpc_frame_size + 512;
                    hd_llr_fifo = new int8_t[hd_fifo_cap]();

                    for (int p = 0; p < d_num_phases; p++)
                    {
                        float a = p * (2.0f * 3.14159265358979323846f / (float)d_num_phases);
                        hd_phase_cos[p] = cosf(a);
                        hd_phase_sin[p] = sinf(a);
                    }
                }
                else
                {
                    correlator = std::make_unique<CorrelatorGeneric>(
                        d_constellation, d_ldpc_rate == codings::ldpc::RATE_7_8 ? satdump::unsigned_to_bitvec<uint32_t>(0x1acffc1d) : satdump::unsigned_to_bitvec<uint64_t>(0x034776c7272895b0),
                        d_ldpc_frame_size, d_correlator_threshold);
                }

                logger->trace("LDPC Frame size %d, SIMD %d", d_ldpc_frame_size, d_ldpc_simd);

                // Parse internal sync marker if set
                if (d_internal_stream)
                {
                    uint32_t asm_sync = 0x1acffc1d;
                    if (parameters.count("internal_asm") > 0)
                        asm_sync = std::stoul(parameters["internal_asm"].get<std::string>(), nullptr, 16);

                    deframer = std::make_unique<deframing::BPSK_CCSDS_Deframer>(d_cadu_size, asm_sync);

                    if (d_cadu_size % 8 != 0) // If this is not a perfect byte length match, pad the frames
                    {
                        deframer->CADU_PADDING = d_cadu_size % 8;
                        logger->info("Frames will be padded!");
                    }
                }

                memset(llr_scale_history, 0, sizeof(llr_scale_history));
                memset(ldpc_iter_history, 0, sizeof(ldpc_iter_history));

                // Decoding algorithm. Defaults to plain min-sum, i.e. previous behaviour.
                if (d_parameters.contains("ldpc_algorithm"))
                    d_ldpc_algorithm = codings::ldpc::ldpc_algorithm_from_string(d_parameters["ldpc_algorithm"].get<std::string>());

                if (d_parameters.contains("ldpc_nms_alpha"))
                {
                    float a = d_parameters["ldpc_nms_alpha"].get<float>();
                    if (a <= 0.0f || a > 1.0f)
                        throw satdump_exception("CCSDS LDPC Decoder : ldpc_nms_alpha must be in (0, 1]!");
                    d_ldpc_nms_alpha_q8 = (int16_t)lroundf(a * 256.0f);
                }

                // Offset-beta (Q8); default 0 (disabled) only applies to (layered) min-sum.
                if (d_parameters.contains("ldpc_offset_beta"))
                {
                    float b = d_parameters["ldpc_offset_beta"].get<float>();
                    if (b < 0.0f || b > 1.0f)
                        throw satdump_exception("CCSDS LDPC Decoder : ldpc_offset_beta must be in [0, 1]!");
                    d_ldpc_offset_beta_q8 = (int16_t)lroundf(b * 256.0f);
                }

                // Effective iteration cap; ldpc_max_iterations overrides the legacy cap.
                d_ldpc_cap_iterations = d_ldpc_iterations;
                if (d_parameters.contains("ldpc_max_iterations"))
                    d_ldpc_cap_iterations = d_parameters["ldpc_max_iterations"].get<int>();

                d_ldpc_min_iterations = d_parameters.count("ldpc_min_iterations") > 0 ? d_parameters["ldpc_min_iterations"].get<int>() : 1;
                d_ldpc_early_termination = d_parameters.count("ldpc_early_termination") > 0 ? d_parameters["ldpc_early_termination"].get<bool>() : true;
                d_ldpc_llr_clamp = d_parameters.count("ldpc_llr_clamp") > 0 ? d_parameters["ldpc_llr_clamp"].get<int>() : 127;

                ldpc_dec->set_nms_alpha(d_ldpc_nms_alpha_q8);
                ldpc_dec->set_ldpc_offset_beta(d_ldpc_offset_beta_q8);
                ldpc_dec->set_min_iterations(d_ldpc_min_iterations);
                ldpc_dec->set_early_termination(d_ldpc_early_termination);
                ldpc_dec->set_algorithm(d_ldpc_algorithm);

                logger->info("LDPC algorithm: %s (alpha %.3f, offset-beta %.3f), %d iterations", codings::ldpc::ldpc_algorithm_to_string(d_ldpc_algorithm).c_str(),
                             d_ldpc_nms_alpha_q8 / 256.0f, d_ldpc_offset_beta_q8 / 256.0f, d_ldpc_cap_iterations);

                soft_buffer = new int8_t[d_ldpc_frame_size];
                frames_in_ldpc_buffer = 0;
                ldpc_input_buffer = new int8_t[(d_ldpc_frame_size - d_ldpc_asm_size) * d_ldpc_simd];
                ldpc_output_buffer = new uint8_t[(d_ldpc_frame_size - d_ldpc_asm_size) * d_ldpc_simd];
                deframer_buffer = new uint8_t[d_ldpc_frame_size * 64];

                is_started = true;

                fsfsm_file_ext = is_ccsds ? ".cadu" : ".frm";
            }

            CCSDSLDPCDecoderModule::~CCSDSLDPCDecoderModule()
            {
                delete[] soft_buffer;
                delete[] deframer_buffer;
                delete[] ldpc_input_buffer;
                delete[] ldpc_output_buffer;
                delete[] hd_sym_buffer;
                delete[] hd_llr_buffer;
                delete[] hd_llr_fifo;
            }

            void CCSDSLDPCDecoderModule::process()
            {
                if (d_is_higher_order)
                {
                    process_higher_order();
                    cleanup();
                    return;
                }
                phase_t phase;
                bool swap;

                while (should_run())
                {
                    // Read a buffer
                    read_data((uint8_t *)soft_buffer, d_ldpc_frame_size);

                    // if (d_iq_invert)
                    // rotate_soft((int8_t *)soft_buffer, d_ldpc_frame_size, PHASE_0, true);

                    // When locked, optionally restrict the search to a small window
                    // around the expected ASM position (0) to improve robustness and
                    // reduce CPU. When unlocked, search the whole frame.
                    int search_start = 0, search_len = -1;
                    if (correlator_locked && correlator_search_window > 0)
                        search_len = correlator_search_window;

                    int pos = correlator->correlate((int8_t *)soft_buffer, phase, swap, correlator_cor, d_ldpc_frame_size, search_start, search_len);
                    correlator_corr_norm = correlator->last_normalized_corr();

                    // Lock state machine (mirrors the CCSDS deframer): require N
                    // consecutive good (above-threshold) correlations to enter LOCKED,
                    // and M consecutive failures to drop back to NOSYNC.
                    if (correlator->locked())
                    {
                        correlator_good_count++;
                        correlator_bad_count = 0;
                        if (correlator_good_count >= correlator_lock_after)
                            correlator_locked = true;
                    }
                    else
                    {
                        correlator_bad_count++;
                        correlator_good_count = 0;
                        if (correlator_bad_count >= correlator_drop_after)
                        {
                            if (correlator_locked)
                            {
                                mer_db = 0;
                                avg_mer = 0;
                                memset(mer_history, 0, sizeof(mer_history));
                                mer_estimator.reset();
                            }
                            correlator_locked = false;
                        }
                    }

                    if (pos != 0 && pos < d_ldpc_frame_size) // Safety
                    {
                        memmove(soft_buffer, &soft_buffer[pos], d_ldpc_frame_size - pos);

                        read_data((uint8_t *)&soft_buffer[d_ldpc_frame_size - pos], pos);
                    }

                    // Correct phase ambiguity
                    if (d_constellation == dsp::OQPSK)
                    {
                        rotate_soft((int8_t *)soft_buffer, d_ldpc_frame_size, phase, false);

                        if (swap)
                        {
                            int8_t last_q_oqpsk = 0;
                            for (int i = (d_ldpc_frame_size / 2) - 1; i >= 0; i--)
                            {
                                int8_t back = soft_buffer[i * 2 + 1];
                                soft_buffer[i * 2 + 1] = last_q_oqpsk;
                                last_q_oqpsk = back;
                            }
                        }
                    }
                    else
                    {
                        rotate_soft((int8_t *)soft_buffer, d_ldpc_frame_size, phase, swap);
                    }

                    if (correlator_locked)
                    {
                        int n_syms = d_constellation == dsp::BPSK ? d_ldpc_frame_size : d_ldpc_frame_size / 2;
                        static thread_local std::vector<complex_t> mer_buf;
                        if ((int)mer_buf.size() < n_syms)
                            mer_buf.resize(n_syms);
                        if (d_constellation == dsp::BPSK)
                        {
                            for (int i = 0; i < n_syms; i++)
                                mer_buf[i] = complex_t(soft_buffer[i] / 127.0f, 0.0f);
                        }
                        else
                        {
                            for (int i = 0; i < n_syms; i++)
                                mer_buf[i] = complex_t(soft_buffer[i * 2 + 0] / 127.0f, soft_buffer[i * 2 + 1] / 127.0f);
                        }
                        mer_estimator.update(mer_buf.data(), n_syms);
                        mer_db = mer_estimator.snr();
                        if (mer_db > peak_mer)
                            peak_mer = mer_db;
                        avg_mer = avg_mer * 0.99f + mer_db * 0.01f;
                    }

                    feed_frame(soft_buffer);
                }

                cleanup();
            }

            void CCSDSLDPCDecoderModule::feed_frame(int8_t *frame)
            {
                if (d_derand)
                {
                    if (d_derand_long_poly)
                        derand_ccsds17_soft(&frame[d_ldpc_asm_size], d_ldpc_codeword_size);
                    else
                        derand_ccsds_soft(&frame[d_ldpc_asm_size], d_ldpc_codeword_size);
                }

                memcpy(&ldpc_input_buffer[frames_in_ldpc_buffer * d_ldpc_codeword_size], &frame[d_ldpc_asm_size], d_ldpc_codeword_size);
                frames_in_ldpc_buffer++;

                if (frames_in_ldpc_buffer == d_ldpc_simd)
                {
                    decode_batch();
                    frames_in_ldpc_buffer = 0;
                }
            }

            void CCSDSLDPCDecoderModule::decode_batch(int lanes)
            {
                if (lanes < 0 || lanes > d_ldpc_simd)
                    lanes = d_ldpc_simd;

                // Adaptive LLR scaling for BPSK/QPSK.
                if (!d_is_higher_order)
                {
                    int total_soft = d_ldpc_simd * d_ldpc_codeword_size;

                    int navail = d_constellation == dsp::BPSK ? total_soft : total_soft / 2;
                    int nuse = std::min(navail, SNR_ESTIMATOR_SAMPLES);
                    int stride = navail / nuse;

                    if (d_constellation == dsp::BPSK)
                    {
                        for (int i = 0; i < nuse; i++)
                            snr_sample_buffer[i] = complex_t(ldpc_input_buffer[i * stride] / 127.0f, 0.0f);
                    }
                    else
                    {
                        for (int i = 0; i < nuse; i++)
                            snr_sample_buffer[i] = complex_t(ldpc_input_buffer[i * stride * 2 + 0] / 127.0f,
                                                             ldpc_input_buffer[i * stride * 2 + 1] / 127.0f);
                    }

                    snr_estimator.update(snr_sample_buffer.data(), nuse);

                    llr_snr = snr_estimator.snr();

                    if (d_llr_calibrated)
                    {
                        // Gain-invariant scale.
                        int ncomp = d_constellation == dsp::BPSK ? 1 : 2;
                        float sig_pow = powf(10.0f, snr_estimator.signal() / 10.0f);
                        float noi_pow = powf(10.0f, snr_estimator.noise() / 10.0f);

                        float var_norm = std::max(noi_pow / ncomp, 1e-6f);

                        float scale = 0.5f * std::sqrt(sig_pow / (noi_pow + 1e-6f));

                        llr_sigma2 = var_norm * 127.0f * 127.0f;

                        llr_scale = std::clamp(scale, 0.25f, 8.0f);
                    }
                    else
                    {
                        // Legacy heuristic.
                        float npwr = 2.0f * powf(10.0f, -llr_snr / 20.0f);
                        llr_scale = std::clamp(1.0f / npwr, 0.25f, 8.0f);
                    }

                    // Build the scale LUT once per batch.
                    float llr_clamp_f = (float)d_ldpc_llr_clamp;
                    for (int v = -128; v < 128; v++)
                        llr_scale_lut[v + 128] = (int8_t)std::clamp(v * llr_scale, -llr_clamp_f, llr_clamp_f);

                    for (int i = 0; i < total_soft; i++)
                        ldpc_input_buffer[i] = llr_scale_lut[ldpc_input_buffer[i] + 128];
                }

                if (use_ldpc2)
                {
                    for (int i = 0; i < d_ldpc_codeword_size; i++)
                    {
                        ldpc_input_buffer[i] = -ldpc_input_buffer[i];
                        ldpc_input_buffer[i] /= 4;
                    }

                    uint64_t trials2 = 0;
                    int8_t *working = new int8_t[ldpc2_params->decode_ms_working_len];
                    uint8_t *working_u8 = new uint8_t[ldpc2_params->decode_ms_working_u8_len];
                    labrador::decode_ms(*ldpc2_params, ldpc_input_buffer, deframer_buffer, working, working_u8, d_ldpc_iterations, &trials2);
                    ldpc_corr = trials2;

                    if (d_ldpc_asm_size == 32)
                    {
                        const uint32_t sync = 0x1acffc1d;
                        for (int b = 3; b >= 0; b--) // big-endian: a raw uint32_t store is host-endian
                        {
                            uint8_t v = (sync >> (b * 8)) & 0xFF;
                            write_data(&v, 1);
                        }
                    }
                    else if (d_ldpc_asm_size == 64)
                    {
                        const uint64_t sync = 0x034776c7272895b0;
                        for (int i = 7; i >= 0; i--)
                        {
                            uint8_t v = (sync >> i * 8) & 0xFF;
                            write_data((uint8_t *)&v, 1);
                        }
                    }

                    delete[] working;
                    delete[] working_u8;

                    write_data(deframer_buffer, (d_ldpc_frame_size - d_ldpc_asm_size) / 8);
                }
                else
                {
#if 1
                    ldpc_corr = ldpc_dec->decode(ldpc_input_buffer, ldpc_output_buffer, d_ldpc_cap_iterations);

                    ldpc_iterations_used = ldpc_dec->last_iterations();
                    std::memmove(&ldpc_iter_history[0], &ldpc_iter_history[1], (200 - 1) * sizeof(float));
                    ldpc_iter_history[200 - 1] = (float)ldpc_iterations_used;

                    ldpc_last_converged = ldpc_dec->converged();
#else
                    for (int i = 0; i < d_ldpc_simd * d_ldpc_codeword_size; i++)
                        ldpc_output_buffer[i] = ldpc_input_buffer[i] > 0;
#endif

                    if (d_internal_stream)
                    {
                        for (int i = 0; i < lanes; i++)
                        {
                            int frames = deframer->work(&ldpc_output_buffer[i * d_ldpc_codeword_size], d_ldpc_data_size, deframer_buffer);
                            for (int i = 0; i < frames; i++)
                                write_data(&deframer_buffer[i * d_cadu_bytes], d_cadu_bytes);
                        }
                    }
                    else
                    {
                        for (int i = 0; i < lanes * d_ldpc_codeword_size; i++)
                            deframer_buffer[i / 8] = deframer_buffer[i / 8] << 1 | ldpc_output_buffer[i];

                        for (int i = 0; i < lanes; i++)
                        {
                            if (d_ldpc_asm_size == 32)
                            {
                                const uint32_t sync = 0x1acffc1d;
                                for (int b = 3; b >= 0; b--) // big-endian: a raw uint32_t store is host-endian
                                {
                                    uint8_t v = (sync >> (b * 8)) & 0xFF;
                                    write_data(&v, 1);
                                }
                            }
                            else if (d_ldpc_asm_size == 64)
                            {
                                const uint64_t sync = 0x034776c7272895b0;
                                for (int i = 7; i >= 0; i--)
                                {
                                    uint8_t v = (sync >> i * 8) & 0xFF;
                                    write_data((uint8_t *)&v, 1);
                                }
                            }

                            write_data((uint8_t *)&deframer_buffer[i * (d_ldpc_codeword_size / 8)], (d_ldpc_frame_size - d_ldpc_asm_size) / 8);
                        }
                    }
                }
            }

            void CCSDSLDPCDecoderModule::hd_build_asm_ref(float npwr)
            {
                const int nbits = (int)hd_asm_bits.size();
                const int bps = d_bits_per_sym;

                // Must use the runtime npwr and const_sca: a wrong npwr flattens the
                // profile, and int8 truncates a scale difference instead of cancelling it.
                for (int off = 0; off < bps; off++)
                {
                    const int seq_len = nbits + off;
                    const int nsym = (seq_len + bps - 1) / bps;

                    for (int k = 0; k < nbits; k++)
                        hd_asm_ref[off][k] = 0.0f;

                    for (int s = 0; s < nsym; s++)
                    {
                        // Bits of this symbol that carry real ASM bits; the rest are pad.
                        bool known[8] = {false};
                        int n_unknown = 0;
                        for (int b = 0; b < bps; b++)
                        {
                            int idx = s * bps + b;
                            known[b] = (idx >= off && idx - off < nbits);
                            if (!known[b])
                                n_unknown++;
                        }

                        // 8PSK: zero is a neutral guess. APSK: '00' is a real point, so
                        // marginalise the pad bits over all combinations.
                        const int npat = (d_constellation == dsp::APSK16_CCSDS) ? (1 << n_unknown) : 1;

                        float acc[8] = {0};
                        for (int pat = 0; pat < npat; pat++)
                        {
                            int sym = 0, up = 0;
                            for (int b = 0; b < bps; b++)
                            {
                                int bit;
                                if (known[b])
                                    bit = hd_asm_bits[s * bps + b - off];
                                else
                                    bit = (pat >> up++) & 1;
                                sym = (sym << 1) | bit;
                            }

                            int8_t llr[8] = {0};
                            hd_constellation->demod_soft_calc(hd_constellation->mod((uint8_t)sym), llr, nullptr, npwr);
                            for (int b = 0; b < bps; b++)
                                if (known[b])
                                    acc[b] += (float)llr[b];
                        }

                        for (int b = 0; b < bps; b++)
                            if (known[b])
                                hd_asm_ref[off][s * bps + b - off] = acc[b] / (float)npat;
                    }

                    float sq = 0.0f;
                    for (int k = 0; k < nbits; k++)
                        sq += hd_asm_ref[off][k] * hd_asm_ref[off][k];
                    hd_asm_ref_sq[off] = sq;

                    if (!(sq > 0.0f))
                        logger->warn("LDPC %s: empty ASM reference for bit phase %d", d_constellation_str.c_str(), off);
                }
            }

            void CCSDSLDPCDecoderModule::hd_update_llr_gain(const int8_t *llr, int count)
            {
                // Mean-|LLR| controller, slew limited to 2x per chunk.
                long sum = 0;
                for (int i = 0; i < count; i++)
                {
                    int v = llr[i];
                    sum += v < 0 ? -v : v;
                }
                float mean = count > 0 ? (float)sum / (float)count : 0.0f;
                if (!(mean > 1e-2f))
                    return;

                float ratio = std::clamp(d_llr_target / mean, 0.5f, 2.0f);
                d_llr_gain = std::clamp(d_llr_gain * powf(ratio, 0.25f), 0.05f, 64.0f);
                hd_constellation->set_scale(d_llr_gain);
            }

            void CCSDSLDPCDecoderModule::demap_higher_order(const int8_t *sym, int nsyms, int phase, bool conjugate, int8_t *out)
            {
                const float c = hd_phase_cos[phase];
                const float s = hd_phase_sin[phase];

                for (int i = 0; i < nsyms; i++)
                {
                    // int8 ~100 RMS -> unit RMS.
                    float r = (float)sym[2 * i + 0] * 0.01f;
                    float q = (float)sym[2 * i + 1] * (conjugate ? -0.01f : 0.01f);
                    hd_constellation->demod_soft_calc(complex_t(r * c - q * s, q * c + r * s), &out[i * d_bits_per_sym], nullptr, d_demap_npwr);
                }
            }

            float CCSDSLDPCDecoderModule::correlate_asm(const int8_t *bits, int pos, int nbits, const float *ref, float ref_sq, float &raw_out)
            {
                const int n = (int)hd_asm_bits.size();
                if (pos < 0 || pos + n > nbits)
                {
                    raw_out = 0.0f;
                    return 0.0f;
                }

                float dot = 0.0f, sq = 0.0f;
                for (int k = 0; k < n; k++)
                {
                    float v = (float)bits[pos + k];
                    dot += v * ref[k];
                    sq += v * v;
                }

                float corr = dot / (sqrtf(sq * ref_sq) + 1e-6f);
                raw_out = corr * 32.0f; // plot scale
                return corr;
            }

            void CCSDSLDPCDecoderModule::hd_append_llrs(const int8_t *llr, int count)
            {
                if (count <= 0)
                    return;
                if (hd_fifo_len + count > hd_fifo_cap)
                {
                    // Size is guaranteed; drop and re-acquire rather than misalign.
                    logger->warn("LDPC %s FIFO overflow (%d + %d > %d), re-acquiring", d_constellation_str.c_str(), hd_fifo_len, count, hd_fifo_cap);
                    hd_fifo_len = 0;
                    hd_tracking = false;
                    hd_syncing = false;
                    count = 0;
                }
                if (count <= 0)
                    return;

                memcpy(&hd_llr_fifo[hd_fifo_len], llr, count);
                hd_fifo_len += count;
            }

            bool CCSDSLDPCDecoderModule::find_asm_window(const int8_t *bits, int center, int half, int nbits, int bit_phase, int &pos, float &corr, float &raw)
            {
                pos = -1;
                corr = -1e9f;
                raw = 0.0f;
                const int n = (int)hd_asm_bits.size();
                int lo = std::max(0, center - half);
                int hi = std::min(nbits - n, center + half);
                for (int p = lo; p <= hi; p++)
                {
                    const int off = (bit_phase + p) % d_bits_per_sym;
                    float r;
                    float c = correlate_asm(bits, p, nbits, hd_asm_ref[off], hd_asm_ref_sq[off], r);
                    if (c > corr)
                    {
                        corr = c;
                        raw = r;
                        pos = p;
                    }
                }
                return pos >= 0;
            }

            void CCSDSLDPCDecoderModule::process_higher_order()
            {
                const int chunk_bits = d_bits_per_sym * HD_CHUNK_SYMS;
                const int asm_bits = (int)hd_asm_bits.size();
                // Clean ASM ~0.99, acquisition noise max ~0.83.
                const float acq_threshold = std::max(d_correlator_threshold, 0.70f);
                const float verify_threshold = std::max(d_correlator_threshold, 0.80f);

                while (should_run())
                {
                    if (!hd_tracking && !hd_syncing)
                    {
                        const size_t got = read_data((uint8_t *)hd_sym_buffer, HD_CHUNK_SYMS * 2);
                        if (got < 2)
                            break; // end of file: go drain what is already in the FIFO

                        // Take npwr here, where the FIFO resets: ref and LLRs share a shape.
                        if (d_llr_noise_cal && d_mer_valid)
                        {
                            float npwr = std::clamp(d_npwr_smooth, 1e-6f, 1.0f);
                            if (npwr > d_demap_npwr * 1.25f || npwr < d_demap_npwr * 0.8f)
                            {
                                d_demap_npwr = npwr;
                                hd_build_asm_ref(d_demap_npwr);
                                logger->debug("16APSK demapper npwr -> %.3g (MER %.1f dB)", d_demap_npwr, mer_db);
                            }
                        }

                        float best_corr = 0.0f, best_raw = 0.0f;
                        int best_phase = -1, best_pos = 0, best_conj = 0;

                        // Best correlation per (phase, conj), for the two-ASM re-score.
                        float cand_corr[16][2];
                        int cand_pos[16][2];
                        for (int p = 0; p < 16; p++)
                            for (int c = 0; c < 2; c++)
                            {
                                cand_corr[p][c] = 0.0f;
                                cand_pos[p][c] = -1;
                            }

                        for (int p = 0; p < d_num_phases; p++)
                        {
                            for (int conj = 0; conj < 2; conj++)
                            {
                                demap_higher_order(hd_sym_buffer, HD_CHUNK_SYMS, p, conj == 1, hd_llr_buffer);
                                for (int pos = 0; pos + asm_bits <= chunk_bits; pos++)
                                {
                                    float raw;
                                    const int off = pos % d_bits_per_sym; // chunk starts on a symbol boundary
                                    float corr = correlate_asm(hd_llr_buffer, pos, chunk_bits, hd_asm_ref[off], hd_asm_ref_sq[off], raw);
                                    if (corr > cand_corr[p][conj])
                                    {
                                        cand_corr[p][conj] = corr;
                                        cand_pos[p][conj] = pos;
                                    }
                                    if (corr > best_corr)
                                    {
                                        best_corr = corr;
                                        best_raw = raw;
                                        best_phase = p;
                                        best_pos = pos;
                                        best_conj = conj;
                                    }
                                }
                            }
                        }

                        // Re-score the plausible phases on two ASMs: one 32-bit ASM cannot
                        // separate the true rotation from a half-step one at low SNR (8PSK
                        // at 4 dB picked the half-step and lost every frame).
                        if (best_phase >= 0 && best_corr >= acq_threshold &&
                            best_pos + (int)d_ldpc_frame_size + asm_bits <= chunk_bits)
                        {
                            float best_score = -1e9f;
                            int bp = best_phase, bc = best_conj, bpos = best_pos;
                            for (int p = 0; p < (int)d_num_phases; p++)
                            {
                                for (int c = 0; c < 2; c++)
                                {
                                    if (cand_pos[p][c] < 0 || cand_corr[p][c] < best_corr - 0.25f)
                                        continue;
                                    const int pos2 = cand_pos[p][c] + (int)d_ldpc_frame_size;
                                    if (pos2 + asm_bits > chunk_bits)
                                        continue;
                                    demap_higher_order(hd_sym_buffer, HD_CHUNK_SYMS, p, c == 1, hd_llr_buffer);
                                    float raw2;
                                    const int off2 = pos2 % d_bits_per_sym;
                                    const float score = cand_corr[p][c] +
                                                        correlate_asm(hd_llr_buffer, pos2, chunk_bits, hd_asm_ref[off2], hd_asm_ref_sq[off2], raw2);
                                    if (score > best_score)
                                    {
                                        best_score = score;
                                        bp = p;
                                        bc = c;
                                        bpos = cand_pos[p][c];
                                    }
                                }
                            }
                            if (best_score > -1e8f && (bp != best_phase || bc != best_conj || bpos != best_pos))
                            {
                                logger->debug("phase re-score chose %d/%d at %d (two-frame score %.3f vs %.3f)",
                                              bp, bc, bpos, best_score, best_corr);
                                best_phase = bp;
                                best_conj = bc;
                                best_pos = bpos;
                                best_corr = best_score * 0.5f;
                            }
                        }

                        correlator_cor = best_raw;
                        correlator_corr_norm = best_corr;

                        if (best_phase >= 0 && best_corr >= acq_threshold)
                        {
                            demap_higher_order(hd_sym_buffer, HD_CHUNK_SYMS, best_phase, best_conj == 1, hd_llr_buffer);
                            hd_fifo_len = 0;
                            hd_fifo_bit_phase = best_pos % d_bits_per_sym;
                            hd_append_llrs(&hd_llr_buffer[best_pos], chunk_bits - best_pos);

                            // Do not drop queued frames on a re-lock: each is a complete
                            // codeword and the LDPC lanes are independent, so a batch may
                            // mix phases (resetting cost up to 15 good frames per slip).
                            d_current_phase = best_phase;
                            d_conjugate = (best_conj == 1);
                            hd_grid_ok = false; // new alignment: the grid must be re-earned
                            hd_syncing = true;
                            hd_sync_fail_count = 0;
                            correlator_locked = false;
                            correlator_good_count = 0;
                            correlator_bad_count = 0;
                        }
                        continue;
                    }

                    const int half = 256;
                    const int need = d_ldpc_frame_size + asm_bits + half;
                    if (hd_fifo_len < need)
                    {
                        // Demap only what was read: at EOF the rest of hd_sym_buffer is stale.
                        const size_t got = read_data((uint8_t *)hd_sym_buffer, HD_CHUNK_SYMS * 2);
                        if (got < 2)
                            break;
                        const int nsyms = (int)(got / 2);
                        demap_higher_order(hd_sym_buffer, nsyms, d_current_phase, d_conjugate, hd_llr_buffer);
                        hd_append_llrs(hd_llr_buffer, nsyms * d_bits_per_sym);

                        static thread_local std::vector<complex_t> mer_buf;
                        if ((int)mer_buf.size() < HD_CHUNK_SYMS)
                            mer_buf.resize(HD_CHUNK_SYMS);
                        for (int i = 0; i < HD_CHUNK_SYMS; i++)
                            mer_buf[i] = complex_t(hd_sym_buffer[i * 2 + 0] / 127.0f, hd_sym_buffer[i * 2 + 1] / 127.0f);
                        mer_estimator.update(mer_buf.data(), HD_CHUNK_SYMS);
                        mer_db = mer_estimator.snr();
                        llr_snr = mer_db;
                        if (mer_db > peak_mer)
                            peak_mer = mer_db;
                        avg_mer = avg_mer * 0.99f + mer_db * 0.01f;

                        if (d_llr_noise_cal)
                        {
                            // npwr = PER-AXIS variance in demap units (int8/100): scale by
                            // (127/100)^2 from the EVM units and halve for per-axis.
                            float pn = powf(10.0f, mer_estimator.noise() / 10.0f);
                            d_npwr_implied = std::clamp(0.5f * pn * (127.0f * 127.0f) / (100.0f * 100.0f), 1e-6f, 1.0f);

                            // The estimator starts from zero power, so its first chunks read
                            // several dB low; average before trusting it.
                            // Skip the biased first chunks after a reset.
                            if (d_mer_settle > 0)
                                d_mer_settle--;
                            else if (++d_mer_updates == 1)
                                d_npwr_smooth = d_npwr_implied;
                            else
                                d_npwr_smooth += 0.1f * (d_npwr_implied - d_npwr_smooth);

                            // Also wait out the amplitude drift at the head of a recording.
                            d_mer_valid = (d_mer_updates >= 6);

                            hd_update_llr_gain(hd_llr_buffer, chunk_bits);
                            llr_scale = d_llr_gain;

                            // Apply npwr at (re)acquisition only: a mid-lock change would
                            // score buffered frames against a different filter shape, so a
                            // big SNR move re-acquires instead.
                            if (d_mer_valid && (d_npwr_smooth > d_demap_npwr * 2.0f || d_npwr_smooth < d_demap_npwr * 0.5f))
                            {
                                logger->debug("16APSK MER %.1f dB implies npwr %.3g vs %.3g in use, re-acquiring", mer_db, d_npwr_smooth, d_demap_npwr);
                                hd_tracking = false;
                                hd_syncing = false;
                                hd_fifo_len = 0;
                                correlator_bad_count = 0;
                                correlator_good_count = 0;
                                continue;
                            }
                        }
                        continue;
                    }

                    if (hd_syncing)
                    {
                        int npos;
                        float ncorr, nraw;
                        find_asm_window(hd_llr_fifo, d_ldpc_frame_size, half, hd_fifo_len, hd_fifo_bit_phase, npos, ncorr, nraw);
                        correlator_cor = nraw;
                        correlator_corr_norm = ncorr;

                        if (npos == d_ldpc_frame_size && ncorr >= verify_threshold)
                        {
                            hd_syncing = false;
                            hd_tracking = true;
                            hd_grid_ok = true; // the ASM exactly one frame later verified
                            hd_sync_fail_count = 0;
                            correlator_good_count = 1;
                            correlator_locked = (correlator_good_count >= correlator_lock_after);
                        }
                        else if (npos >= 0 && ncorr >= verify_threshold && ++hd_sync_fail_count <= 4)
                        {
                            memmove(hd_llr_fifo, &hd_llr_fifo[npos], hd_fifo_len - npos);
                            hd_fifo_len -= npos;
                            hd_fifo_bit_phase = (hd_fifo_bit_phase + npos) % d_bits_per_sym;
                        }
                        else
                        {
                            hd_syncing = false;
                            hd_sync_fail_count = 0;
                            hd_fifo_len = 0;
                        }
                        continue;
                    }

                    int best_pos;
                    float best_corr, best_raw;
                    find_asm_window(hd_llr_fifo, 0, 2048, hd_fifo_len, hd_fifo_bit_phase, best_pos, best_corr, best_raw);
                    correlator_cor = best_raw;
                    correlator_corr_norm = best_corr;

                    const float track_threshold = std::max(d_correlator_threshold, 0.80f);
                    if (best_pos >= 0 && best_corr >= track_threshold)
                    {
                        if (best_pos > 0)
                        {
                            memmove(hd_llr_fifo, &hd_llr_fifo[best_pos], hd_fifo_len - best_pos);
                            hd_fifo_len -= best_pos;
                            hd_fifo_bit_phase = (hd_fifo_bit_phase + best_pos) % d_bits_per_sym;
                            // refill before next-ASM search
                            if (hd_fifo_len < need)
                                continue;
                        }

                        int npos;
                        float ncorr, nraw;
                        find_asm_window(hd_llr_fifo, d_ldpc_frame_size, half, hd_fifo_len, hd_fifo_bit_phase, npos, ncorr, nraw);

                        // A weak next-ASM says nothing about the position (the peak wanders
                        // 0..1 bits), and the ASM is not part of the codeword: with a trusted
                        // grid the boundary is exactly one frame ahead, so feed the frame.
                        const bool asm_strong = (ncorr >= track_threshold);
                        const bool at_expected = asm_strong ? (npos == d_ldpc_frame_size) : hd_grid_ok;

                        if (at_expected)
                        {
                            if (asm_strong)
                            {
                                correlator_good_count++;
                                correlator_bad_count = 0;
                            }
                            else
                            {
                                logger->trace("LDPC %s weak next ASM (npos %d corr %.2f), trusting the grid and feeding",
                                              d_constellation_str.c_str(), npos, ncorr);
                                correlator_bad_count++;
                            }

                            if (!correlator_locked && correlator_good_count >= correlator_lock_after)
                            {
                                correlator_locked = true;
                                logger->info("LDPC %s locked (phase %d/%d, %s)", d_constellation_str.c_str(), d_current_phase, d_num_phases,
                                             d_conjugate ? "conjugated" : "normal");
                            }

                            feed_frame(hd_llr_fifo);
                            hd_frames_fed++;
                            if (!asm_strong)
                                hd_frames_weak_fed++;
                            hd_grid_ok = true;
                            memmove(hd_llr_fifo, &hd_llr_fifo[d_ldpc_frame_size], hd_fifo_len - d_ldpc_frame_size);
                            hd_fifo_len -= d_ldpc_frame_size;
                            hd_fifo_bit_phase = (hd_fifo_bit_phase + d_ldpc_frame_size) % d_bits_per_sym;
                            continue;
                        }

                        if (asm_strong && npos >= 0)
                        {
                            logger->trace("LDPC %s frame slip (%d bits), dropping frame", d_constellation_str.c_str(), npos - d_ldpc_frame_size);
                            hd_frames_dropped++;
                            correlator_bad_count = 0;
                            hd_grid_ok = true;
                            memmove(hd_llr_fifo, &hd_llr_fifo[npos], hd_fifo_len - npos);
                            hd_fifo_len -= npos;
                            hd_fifo_bit_phase = (hd_fifo_bit_phase + npos) % d_bits_per_sym;
                            continue;
                        }

                        // Current ASM good, next one damaged: step exactly one frame.
                        // Sliding a bit would move the good ASM out of its window.
                        if (!asm_strong)
                        {
                            logger->trace("LDPC %s weak next ASM (npos %d, corr %.2f), no trusted grid: dropping frame", d_constellation_str.c_str(), npos, ncorr);
                            hd_frames_dropped++;
                        }
                    }

                    correlator_bad_count++;
                    correlator_good_count = 0;

                    if (correlator_bad_count >= correlator_drop_after)
                    {
                        logger->warn("LDPC %s lost lock (corr %.2f, pos %d), re-acquiring", d_constellation_str.c_str(), best_corr, best_pos);
                        correlator_locked = false;
                        hd_tracking = false;
                        hd_grid_ok = false;
                        correlator_bad_count = 0;
                        hd_fifo_len = 0;
                        // keep partial batch; only a phase change resets it
                        mer_db = 0;
                        avg_mer = 0;
                        mer_estimator.reset();
                        d_mer_settle = 2;
                        continue;
                    }

                    // Grid known if a frame was fed or the current ASM correlated.
                    if (hd_grid_ok || (best_pos >= 0 && best_corr >= track_threshold))
                    {
                        if (hd_fifo_len < d_ldpc_frame_size + asm_bits)
                        {
                            // Cannot happen (the refill guarantees need bits).
                            hd_tracking = false;
                            hd_syncing = false;
                            hd_grid_ok = false;
                            hd_fifo_len = 0;
                            correlator_bad_count = 0;
                            continue;
                        }
                        // Trusted grid: skip exactly one frame and keep the alignment.
                        memmove(hd_llr_fifo, &hd_llr_fifo[d_ldpc_frame_size], hd_fifo_len - d_ldpc_frame_size);
                        hd_fifo_len -= d_ldpc_frame_size;
                        hd_fifo_bit_phase = (hd_fifo_bit_phase + d_ldpc_frame_size) % d_bits_per_sym;
                    }
                    else
                    {
                        // No trusted grid yet: slide one bit and keep looking.
                        memmove(hd_llr_fifo, &hd_llr_fifo[1], hd_fifo_len - 1);
                        hd_fifo_len--;
                        hd_fifo_bit_phase = (hd_fifo_bit_phase + 1) % d_bits_per_sym;
                    }
                }

                // Drain the FIFO at EOF: up to ~15 demapped frames were left unemitted.
                // Require the ASM at position 0 with a good correlation.
                if (!d_is_streaming_input)
                {
                    int drained = 0;
                    // Stricter than tracking: no next-ASM bracket to confirm it.
                    const float drain_threshold = std::max(d_correlator_threshold, 0.90f);
                    while (hd_fifo_len >= d_ldpc_frame_size + asm_bits)
                    {
                        int pos;
                        float corr, raw;
                        find_asm_window(hd_llr_fifo, 0, 256, hd_fifo_len, hd_fifo_bit_phase, pos, corr, raw);
                        if (pos != 0 || corr < drain_threshold)
                            break;

                        feed_frame(hd_llr_fifo);
                        hd_frames_fed++;
                        drained++;
                        memmove(hd_llr_fifo, &hd_llr_fifo[d_ldpc_frame_size], hd_fifo_len - d_ldpc_frame_size);
                        hd_fifo_len -= d_ldpc_frame_size;
                        hd_fifo_bit_phase = (hd_fifo_bit_phase + d_ldpc_frame_size) % d_bits_per_sym;

                        if (frames_in_ldpc_buffer == d_ldpc_simd)
                        {
                            decode_batch();
                            frames_in_ldpc_buffer = 0;
                        }
                    }
                    if (drained > 0)
                        logger->info("%s: drained %d frames left in the FIFO at EOF", d_constellation_str.c_str(), drained);
                }

                // Flush the last partial batch (up to 15 frames) at EOF.
                if (frames_in_ldpc_buffer > 0 && !d_is_streaming_input)
                {
                    logger->info("%s: flushing the final batch (%d frames)", d_constellation_str.c_str(), frames_in_ldpc_buffer);
                    decode_batch(frames_in_ldpc_buffer);
                    frames_in_ldpc_buffer = 0;
                }

                logger->info("%s front-end: %llu frames fed (%llu with a weak ASM), %llu dropped",
                             d_constellation_str.c_str(), (unsigned long long)hd_frames_fed, (unsigned long long)hd_frames_weak_fed,
                             (unsigned long long)hd_frames_dropped);
            }

            nlohmann::json CCSDSLDPCDecoderModule::getModuleStats()
            {
                auto v = satdump::pipeline::base::FileStreamToFileStreamModule::getModuleStats();
                if (d_internal_stream)
                    v["deframer_lock"] = deframer->getState() == deframer->STATE_SYNCED;
                v["constellation"] = d_constellation_str;
                if (d_is_higher_order)
                    v["phase_index"] = d_current_phase;
                v["correlator_lock"] = correlator_locked;
                v["correlator_corr"] = correlator_cor;
                v["correlator_corr_norm"] = correlator_corr_norm;
                v["ldpc_corr"] = ldpc_corr;
                v["ldpc_iterations"] = ldpc_iterations_used;
                v["ldpc_algorithm"] = codings::ldpc::ldpc_algorithm_to_string(d_ldpc_algorithm);
                v["ldpc_nms_alpha"] = d_ldpc_nms_alpha_q8 / 256.0f;
                v["ldpc_offset_beta"] = d_ldpc_offset_beta_q8 / 256.0f;
                v["ldpc_converged"] = ldpc_last_converged;
                v["llr_snr"] = llr_snr;
                v["llr_scale"] = llr_scale;
                if (d_llr_noise_cal)
                {
                    v["apsk_gamma"] = d_gamma;
                    v["demap_npwr"] = d_demap_npwr;
                    v["llr_gain"] = d_llr_gain;
                }
                if (d_is_higher_order)
                {
                    v["frames_fed"] = (uint64_t)hd_frames_fed;
                    v["frames_dropped"] = (uint64_t)hd_frames_dropped;
                    v["frames_weak_fed"] = (uint64_t)hd_frames_weak_fed;
                }
                v["llr_sigma2"] = llr_sigma2;
                v["llr_scaling"] = d_llr_calibrated ? "calibrated" : "heuristic";
                if (correlator_locked)
                    v["mer_db"] = mer_db;
                std::string lock_state = correlator_locked ? "SYNCED" : "NOSYNC";
                std::string deframer_state;
                v["lock_state"] = lock_state;
                if (d_internal_stream)
                    deframer_state = deframer->getState() == deframer->STATE_NOSYNC ? "NOSYNC" : (deframer->getState() == deframer->STATE_SYNCING ? "SYNCING" : "SYNCED");
                if (d_internal_stream)
                    v["deframer_state"] = deframer_state;
                return v;
            }

            void CCSDSLDPCDecoderModule::drawUI(bool window)
            {
                if (!is_started)
                    return;

                std::string algo;
                if (d_ldpc_algorithm == codings::ldpc::LDPC_NORMALIZED_MIN_SUM)
                {
                    char buf[32];
                    snprintf(buf, sizeof(buf), "%.2f", d_ldpc_nms_alpha_q8 / 256.0f);
                    algo = "NMS a=" + std::string(buf);
                }
                else if (d_ldpc_algorithm == codings::ldpc::LDPC_SELF_CORRECTED_MIN_SUM)
                    algo = "SCMS";
                else if (d_ldpc_algorithm == codings::ldpc::LDPC_SUM_PRODUCT)
                    algo = "Sum-Product";
                else if (d_ldpc_algorithm == codings::ldpc::LDPC_LAYERED_MIN_SUM)
                    algo = "Layered MS";
                else if (d_ldpc_algorithm == codings::ldpc::LDPC_LAYERED_NORMALIZED_MIN_SUM)
                {
                    char buf[32];
                    snprintf(buf, sizeof(buf), "%.2f", d_ldpc_nms_alpha_q8 / 256.0f);
                    algo = "Layered NMS a=" + std::string(buf);
                }
                else
                    algo = "Min-Sum";
                std::string window_title = "CCSDS LDPC Decoder (" + algo + ")";
                ImGui::Begin(window_title.c_str(), NULL, window ? 0 : NOWINDOW_FLAGS);

                float avail_x = ImGui::GetContentRegionAvail().x;
                float spacing = ImGui::GetStyle().ItemSpacing.x;
                int num_columns = d_is_streaming_input ? 2 : 3;
                float col_w = (avail_x - spacing * (num_columns - 1)) / num_columns;
                if (col_w < 50.0f * ui_scale) col_w = 50.0f * ui_scale;

                ImGui::Dummy({0, 0}); // Stupid ImGui stuff?

                ImGui::BeginGroup();
                if (!d_is_streaming_input)
                {
                    // Constellation
                    ImDrawList *draw_list = ImGui::GetWindowDrawList();
                    ImVec2 rect_min = ImGui::GetCursorScreenPos();
                    ImVec2 rect_max = {rect_min.x + col_w, rect_min.y + col_w};
                    draw_list->AddRectFilled(rect_min, rect_max, style::theme.widget_bg);
                    draw_list->PushClipRect(rect_min, rect_max);

                    // Bounded by the frame size, not a fixed 2048.
                    if (d_is_higher_order)
                        widgets::drawSoftConstellation(draw_list, rect_min, rect_max, hd_sym_buffer, std::min(HD_CHUNK_SYMS, 2048), 2,
                                                       &rng, 0.8f, std::max(1.0f, col_w * 0.01f));
                    else if (d_constellation == dsp::BPSK)
                        widgets::drawSoftConstellation(draw_list, rect_min, rect_max, soft_buffer, std::min(d_ldpc_frame_size, 2048), 1,
                                                       &rng, 0.8f, std::max(1.0f, col_w * 0.01f));
                    else
                        widgets::drawSoftConstellation(draw_list, rect_min, rect_max, soft_buffer, std::min(d_ldpc_frame_size / 2, 2048), 2,
                                                       &rng, 0.8f, std::max(1.0f, col_w * 0.01f));

                    draw_list->PopClipRect();
                    ImGui::Dummy(ImVec2(col_w, col_w));
                }
                ImGui::EndGroup();

                ImGui::SameLine();

                ImGui::BeginGroup();
                {
                    ImGui::Button("Correlator", {col_w, 20 * ui_scale});
                    {
                        ImGui::Text("State : ");
                        ImGui::SameLine();
                        ImGui::TextColored(correlator_locked ? style::theme.green : style::theme.orange, correlator_locked ? "SYNCED" : "NOSYNC");

                        ImGui::Text("Corr  : ");
                        ImGui::SameLine();
                        ImGui::TextColored(correlator_locked ? style::theme.green : style::theme.orange, UITO_C_STR(correlator_cor));

                        std::memmove(&cor_history[0], &cor_history[1], (200 - 1) * sizeof(float));
                        cor_history[200 - 1] = correlator_cor;

                        if (d_ldpc_asm_size == 32)
                            widgets::ThemedPlotLines(style::theme.plot_bg.Value, "##cor", cor_history, IM_ARRAYSIZE(cor_history), 0, "", 15.0f, 35.0f, ImVec2(col_w, 50 * ui_scale));
                        else
                            widgets::ThemedPlotLines(style::theme.plot_bg.Value, "##cor", cor_history, IM_ARRAYSIZE(cor_history), 0, "", 25.0f, 70.0f, ImVec2(col_w, 50 * ui_scale));
                    }

                    ImGui::Spacing();

                    ImGui::Button("LDPC", {col_w, 20 * ui_scale});
                    {
                        ImGui::Text("Iter  : ");
                        ImGui::SameLine();
                        ImGui::TextColored(ldpc_iterations_used >= d_ldpc_cap_iterations ? style::theme.orange : style::theme.green, "%d / %d", ldpc_iterations_used, d_ldpc_cap_iterations);

                        ImGui::Text("Diff  : ");
                        ImGui::SameLine();
                        ImGui::TextColored(ldpc_corr > 10 ? style::theme.orange : style::theme.green, UITO_C_STR(ldpc_corr));

                        ImGui::Text("Sync  : ");
                        ImGui::SameLine();
                        ImGui::TextColored(ldpc_last_converged ? style::theme.green : style::theme.orange, ldpc_last_converged ? "Converged" : "Not conv");
                    }

                    if (d_internal_stream)
                    {
                        ImGui::Spacing();

                        ImGui::Button("Deframer", {col_w, 20 * ui_scale});
                        {
                            ImGui::Text("State : ");
                            ImGui::SameLine();
                            if (!correlator_locked)
                                ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), "NOSYNC");
                            else
                            {
                                if (deframer->getState() == deframer->STATE_NOSYNC)
                                    ImGui::TextColored(style::theme.red, "NOSYNC");
                                else if (deframer->getState() == deframer->STATE_SYNCING)
                                    ImGui::TextColored(style::theme.orange, "SYNCING");
                                else
                                    ImGui::TextColored(style::theme.green, "SYNCED");
                            }
                        }
                    }
                }
                ImGui::EndGroup();

                ImGui::SameLine();

                ImGui::BeginGroup();
                {
                    ImGui::Button("LLR", {col_w, 20 * ui_scale});
                    {
                        ImGui::Text("SNR   : ");
                        ImGui::SameLine();
                        ImGui::TextColored(style::theme.green, "%.1f dB", llr_snr);

                        ImGui::Text("Scale : ");
                        ImGui::SameLine();
                        ImGui::TextColored(style::theme.green, "%.2fx", llr_scale);
                    }

                    ImGui::Spacing();

                    ImGui::Button("MER", {col_w, 20 * ui_scale});
                    {
                        ImGui::Text("MER     : ");
                        ImGui::SameLine();
                        if (!correlator_locked)
                            ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), "---");
                        else
                            ImGui::TextColored(style::theme.green, "%.2f dB", mer_db);

                        ImGui::Text("Peak MER: ");
                        ImGui::SameLine();
                        if (peak_mer <= 0.0f)
                            ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), "---");
                        else
                            ImGui::TextColored(style::theme.green, "%.2f dB", peak_mer);

                        ImGui::Text("Avg MER : ");
                        ImGui::SameLine();
                        if (avg_mer <= 0.0f)
                            ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), "---");
                        else
                            ImGui::TextColored(style::theme.green, "%.2f dB", avg_mer);

                        std::memmove(&mer_history[0], &mer_history[1], (200 - 1) * sizeof(float));
                        mer_history[200 - 1] = correlator_locked ? mer_db : 0.0f;

                        widgets::ThemedPlotLines(style::theme.plot_bg.Value, "##mer", mer_history, IM_ARRAYSIZE(mer_history), 0, "", 0.0f, 30.0f, ImVec2(col_w, 50 * ui_scale));
                    }
                }
                ImGui::EndGroup();

                drawProgressBar();

                ImGui::End();
            }

            std::string CCSDSLDPCDecoderModule::getID() { return "ccsds_ldpc_decoder"; }

            std::shared_ptr<ProcessingModule> CCSDSLDPCDecoderModule::getInstance(std::string input_file, std::string output_file_hint, nlohmann::json parameters)
            { return std::make_shared<CCSDSLDPCDecoderModule>(input_file, output_file_hint, parameters); }
        } // namespace ccsds
    } // namespace pipeline
} // namespace satdump

