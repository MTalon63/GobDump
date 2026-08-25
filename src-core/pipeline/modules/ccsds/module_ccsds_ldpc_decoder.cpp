#include "module_ccsds_ldpc_decoder.h"
#include "common/codings/ldpc/ccsds_ldpc.h"
#include "common/codings/ldpc/labrador/decoder.h"
#include "common/codings/randomization.h"
#include "common/codings/rotation.h"
#include "common/dsp/complex.h"
#include "common/utils.h"
#include "common/widgets/themed_widgets.h"
#include "core/exception.h"
#include "logger.h"
#include "utils/binary.h"
#include <algorithm>
#include <cmath>
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
                  d_cadu_size(parameters.count("internal_stream") > 0 ? parameters["internal_cadu_size"].get<int>() : 0),
                  d_cadu_bytes(ceil(d_cadu_size / 8.0)) // If we can't use complete bytes, add one and padding
            {
                // Get constellation
                if (d_constellation_str == "bpsk")
                    d_constellation = dsp::BPSK;
                else if (d_constellation_str == "qpsk")
                    d_constellation = dsp::QPSK;
                else if (d_constellation_str == "oqpsk")
                    d_constellation = dsp::OQPSK;
                else
                    throw satdump_exception("CCSDS LDPC Decoder : invalid constellation type!");

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

                mer_estimator = EVMSNREstimator(d_constellation == dsp::BPSK ? 2 : 4, 0.001f);

                float corr_threshold = parameters.count("correlator_threshold") > 0 ? parameters["correlator_threshold"].get<float>() : 0.5f;

                correlator = std::make_unique<CorrelatorGeneric>(
                    d_constellation, d_ldpc_rate == codings::ldpc::RATE_7_8 ? satdump::unsigned_to_bitvec<uint32_t>(0x1acffc1d) : satdump::unsigned_to_bitvec<uint64_t>(0x034776c7272895b0),
                    d_ldpc_frame_size, corr_threshold);

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

                soft_buffer = new int8_t[d_ldpc_frame_size];
                frames_in_ldpc_buffer = 0;
                ldpc_input_buffer = new int8_t[(d_ldpc_frame_size - d_ldpc_asm_size) * d_ldpc_simd];
                ldpc_output_buffer = new uint8_t[(d_ldpc_frame_size - d_ldpc_asm_size) * d_ldpc_simd];
                deframer_buffer = new uint8_t[d_ldpc_frame_size * 64];

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

                ldpc_dec->set_nms_alpha(d_ldpc_nms_alpha_q8);
                ldpc_dec->set_algorithm(d_ldpc_algorithm);

                logger->info("LDPC algorithm: %s (alpha %.3f), %d iterations", codings::ldpc::ldpc_algorithm_to_string(d_ldpc_algorithm).c_str(),
                             d_ldpc_nms_alpha_q8 / 256.0f, d_ldpc_iterations);

                is_started = true;

                fsfsm_file_ext = is_ccsds ? ".cadu" : ".frm";
            }

            CCSDSLDPCDecoderModule::~CCSDSLDPCDecoderModule()
            {
                delete[] soft_buffer;
                delete[] deframer_buffer;
                delete[] ldpc_input_buffer;
                delete[] ldpc_output_buffer;
            }

            void CCSDSLDPCDecoderModule::process()
            {
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

                    // Derand
                    if (d_derand)
                    {
                        if (d_derand_long_poly)
                            derand_ccsds17_soft(&soft_buffer[d_ldpc_asm_size], d_ldpc_codeword_size);
                        else
                            derand_ccsds_soft(&soft_buffer[d_ldpc_asm_size], d_ldpc_codeword_size);
                    }

                    // LDPC Decoding
                    memcpy(&ldpc_input_buffer[frames_in_ldpc_buffer * d_ldpc_codeword_size], &soft_buffer[d_ldpc_asm_size], d_ldpc_codeword_size);
                    frames_in_ldpc_buffer++;

                    if (frames_in_ldpc_buffer == d_ldpc_simd)
                    {
                        // Adaptive LLR scaling: estimate SNR from soft buffer via M2M4,
                        // then apply scale = 1/npwr, mirroring the DVB-S2 demod formula.
                        // Works for BPSK (each int8_t → complex_t with Q=0) and
                        // QPSK/OQPSK (interleaved I/Q pairs → complex_t).
                        {
                            int total_soft = d_ldpc_simd * d_ldpc_codeword_size;

                            // The estimator is a slow EMA, so a strided subset per batch
                            // tracks the channel just as well at a fraction of the cost.
                            int navail = d_constellation == dsp::BPSK ? total_soft : total_soft / 2;
                            int nuse = std::min(navail, SNR_ESTIMATOR_SAMPLES);
                            int stride = navail / nuse;

                            if (d_constellation == dsp::BPSK)
                            {
                                for (int i = 0; i < nuse; i++)
                                    snr_sample_buffer[i] = complex_t(ldpc_input_buffer[i * stride] / 127.0f, 0.0f);
                            }
                            else // QPSK / OQPSK: interleaved I, Q soft bits
                            {
                                for (int i = 0; i < nuse; i++)
                                    snr_sample_buffer[i] = complex_t(ldpc_input_buffer[i * stride * 2 + 0] / 127.0f,
                                                                     ldpc_input_buffer[i * stride * 2 + 1] / 127.0f);
                            }

                            snr_estimator.update(snr_sample_buffer.data(), nuse);

                            llr_snr = snr_estimator.snr();

                            if (d_llr_calibrated)
                            {
                                // Calibrated LLR: for a BPSK/QPSK symbol, each soft
                                // sample is v = A*s + n (A = signal amplitude, n ~
                                // N(0, sigma^2)), and the LLR is
                                //      LLR = 2 * A * v / sigma^2.
                                // So the scale applied to the raw int8 sample v is
                                // 2*A/sigma^2. NOTE: omitting the amplitude A makes
                                // the scale ~1/100 too small, which quantizes every
                                // soft sample to 0 in the int8 LUT and breaks decoding.
                                //
                                // The estimator is fed samples v/127.0. signal() and
                                // noise() return 10*log10 of the TOTAL signal/noise
                                // power summed over the components in these normalized
                                // units. Convert to per-component amplitude and
                                // variance, then to int8 units:
                                //   amp_norm = sqrt(sig_pow/ncomp)          (per comp)
                                //   var_norm = noi_pow/ncomp             (per comp)
                                //   scale = 2*A_int8/sigma2_int8
                                //         = 2*(amp_norm*127) / (var_norm*127^2)
                                //         = 2*amp_norm / (var_norm*127).
                                int ncomp = d_constellation == dsp::BPSK ? 1 : 2;
                                float sig_pow = powf(10.0f, snr_estimator.signal() / 10.0f);
                                float noi_pow = powf(10.0f, snr_estimator.noise() / 10.0f);

                                float amp_norm = std::sqrt(std::max(sig_pow / ncomp, 1e-6f));
                                float var_norm = std::max(noi_pow / ncomp, 1e-6f);

                                float scale = (2.0f * amp_norm) / (var_norm * 127.0f);

                                // Report the per-component int8^2 noise variance.
                                llr_sigma2 = var_norm * 127.0f * 127.0f;

                                // Clamp to a range where the int8 LUT keeps usable
                                // dynamic range: too small zeroes the samples, too
                                // large saturates them all identically.
                                llr_scale = std::clamp(scale, 0.25f, 8.0f);
                            }
                            else
                            {
                                // Legacy heuristic: npwr = 2 * 10^(-SNR/20),
                                // scale = 1/npwr — same as DVB-S2 demod.
                                float npwr = 2.0f * powf(10.0f, -llr_snr / 20.0f);
                                llr_scale = std::clamp(1.0f / npwr, 0.25f, 8.0f);
                            }

                            // Scaling is a function of the int8 input only, so resolve it
                            // once per batch into a 256-entry table instead of per sample.
                            for (int v = -128; v < 128; v++)
                                llr_scale_lut[v + 128] = (int8_t)std::clamp(v * llr_scale, -127.0f, 127.0f);

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

                            // Write directly
                            if (d_ldpc_asm_size == 32)
                            {
                                const uint32_t sync = 0x1acffc1d;
                                write_data((uint8_t *)&sync, 4);
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
#if 1 // For debug if necessary
                            ldpc_corr = ldpc_dec->decode(ldpc_input_buffer, ldpc_output_buffer, d_ldpc_iterations);

                            // Track how many iterations this batch actually used (with
                            // early termination) and push it into the history buffer.
                            ldpc_iterations_used = ldpc_dec->last_iterations();
                            std::memmove(&ldpc_iter_history[0], &ldpc_iter_history[1], (200 - 1) * sizeof(float));
                            ldpc_iter_history[200 - 1] = (float)ldpc_iterations_used;
#else
                            for (int i = 0; i < d_ldpc_simd * d_ldpc_codeword_size; i++)
                                ldpc_output_buffer[i] = ldpc_input_buffer[i] > 0;
#endif

                            if (d_internal_stream)
                            {
                                for (int i = 0; i < d_ldpc_simd; i++)
                                {
                                    // Deframe
                                    int frames = deframer->work(&ldpc_output_buffer[i * d_ldpc_codeword_size], d_ldpc_data_size, deframer_buffer);
                                    for (int i = 0; i < frames; i++)
                                        write_data(&deframer_buffer[i * d_cadu_bytes], d_cadu_bytes);
                                }
                            }
                            else
                            {
                                // Repack
                                for (int i = 0; i < d_ldpc_simd * d_ldpc_codeword_size; i++)
                                    deframer_buffer[i / 8] = deframer_buffer[i / 8] << 1 | ldpc_output_buffer[i];

                                for (int i = 0; i < d_ldpc_simd; i++)
                                {
                                    // Write directly
                                    if (d_ldpc_asm_size == 32)
                                    {
                                        const uint32_t sync = 0x1acffc1d;
                                        write_data((uint8_t *)&sync, 4);
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

                        frames_in_ldpc_buffer = 0;
                    }
                }

                cleanup();
            }

            nlohmann::json CCSDSLDPCDecoderModule::getModuleStats()
            {
                auto v = satdump::pipeline::base::FileStreamToFileStreamModule::getModuleStats();
                if (d_internal_stream)
                    v["deframer_lock"] = deframer->getState() == deframer->STATE_SYNCED;
                v["correlator_lock"] = correlator_locked;
                v["correlator_corr"] = correlator_cor;
                v["correlator_corr_norm"] = correlator_corr_norm;
                v["ldpc_corr"] = ldpc_corr;
                v["ldpc_iterations"] = ldpc_iterations_used;
                v["ldpc_algorithm"] = codings::ldpc::ldpc_algorithm_to_string(d_ldpc_algorithm);
                v["ldpc_nms_alpha"] = d_ldpc_nms_alpha_q8 / 256.0f;
                v["llr_snr"] = llr_snr;
                v["llr_scale"] = llr_scale;
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

                ImGui::Begin("CCSDS LDPC Decoder", NULL, window ? 0 : NOWINDOW_FLAGS);

                ImGui::Dummy({0, 0}); // Stupid ImGui stuff?

                // ---- Column 1: Constellation + Correlator + LDPC ----
                ImGui::BeginGroup();
                {
                    if (!d_is_streaming_input)
                    {
                        // Constellation
                        ImDrawList *draw_list = ImGui::GetWindowDrawList();
                        ImVec2 rect_min = ImGui::GetCursorScreenPos();
                        ImVec2 rect_max = {rect_min.x + 200 * ui_scale, rect_min.y + 200 * ui_scale};
                        draw_list->AddRectFilled(rect_min, rect_max, style::theme.widget_bg);
                        draw_list->PushClipRect(rect_min, rect_max);

                        if (d_constellation == dsp::BPSK)
                        {
                            for (int i = 0; i < 2048; i++)
                            {
                                draw_list->AddCircleFilled(ImVec2(ImGui::GetCursorScreenPos().x + (int)(100 * ui_scale + (((int8_t *)soft_buffer)[i] / 127.0) * 130 * ui_scale) % int(200 * ui_scale),
                                                                  ImGui::GetCursorScreenPos().y + (int)(100 * ui_scale + rng.gasdev() * 14 * ui_scale) % int(200 * ui_scale)),
                                                           2 * ui_scale, style::theme.constellation);
                            }
                        }
                        else
                        {
                            for (int i = 0; i < 2048; i++)
                            {
                                draw_list->AddCircleFilled(
                                    ImVec2(ImGui::GetCursorScreenPos().x + (int)(100 * ui_scale + (((int8_t *)soft_buffer)[i * 2 + 0] / 127.0) * 100 * ui_scale) % int(200 * ui_scale),
                                           ImGui::GetCursorScreenPos().y + (int)(100 * ui_scale + (((int8_t *)soft_buffer)[i * 2 + 1] / 127.0) * 100 * ui_scale) % int(200 * ui_scale)),
                                    2 * ui_scale, style::theme.constellation);
                            }
                        }

                        draw_list->PopClipRect();
                        ImGui::Dummy(ImVec2(200 * ui_scale + 3, 200 * ui_scale + 3));
                    }

                    // ---- Correlator ----
                    ImGui::Text("Corr  : ");
                    ImGui::SameLine();
                    ImGui::TextColored(correlator_locked ? style::theme.green : style::theme.orange, UITO_C_STR(correlator_cor));
                    ImGui::Text("Norm  : ");
                    ImGui::SameLine();
                    ImGui::TextColored(correlator_locked ? style::theme.green : style::theme.orange, "%.2f", correlator_corr_norm);

                    std::memmove(&cor_history[0], &cor_history[1], (200 - 1) * sizeof(float));
                    cor_history[200 - 1] = correlator_cor;

                    if (d_ldpc_asm_size == 32)
                        widgets::ThemedPlotLines(style::theme.plot_bg.Value, "##", cor_history, IM_ARRAYSIZE(cor_history), 0, "", 15.0f, 35.0f, ImVec2(200 * ui_scale, 50 * ui_scale));
                    else
                        widgets::ThemedPlotLines(style::theme.plot_bg.Value, "##", cor_history, IM_ARRAYSIZE(cor_history), 0, "", 25.0f, 70.0f, ImVec2(200 * ui_scale, 50 * ui_scale));
                    ImGui::Separator();

                    // ---- LDPC ----
                    ImGui::Text("Diff  : ");
                    ImGui::SameLine();
                    ImGui::TextColored(ldpc_corr > 10 ? style::theme.orange : style::theme.green, UITO_C_STR(ldpc_corr));

                    std::memmove(&ldpc_history[0], &ldpc_history[1], (200 - 1) * sizeof(float));
                    ldpc_history[200 - 1] = ldpc_corr;

                    widgets::ThemedPlotLines(style::theme.plot_bg.Value, "##", ldpc_history, IM_ARRAYSIZE(ldpc_history), 0, "", 0.0f, d_ldpc_codeword_size / 20,
                                             ImVec2(200 * ui_scale, 50 * ui_scale));
                }
                ImGui::EndGroup();

                ImGui::SameLine();

                // ---- Column 2: LDPC Iterations + LLR Scaling + Deframer + Status ----
                ImGui::BeginGroup();
                {
                    // ---- LDPC Iterations ----
                    ImGui::Text("Iter  : ");
                    ImGui::SameLine();
                    ImGui::TextColored(ldpc_iterations_used >= d_ldpc_iterations ? style::theme.orange : style::theme.green, "%d / %d", ldpc_iterations_used, d_ldpc_iterations);

                    std::memmove(&ldpc_iter_history[0], &ldpc_iter_history[1], (200 - 1) * sizeof(float));
                    ldpc_iter_history[200 - 1] = ldpc_iterations_used;

                    widgets::ThemedPlotLines(style::theme.plot_bg.Value, "##ldpciter", ldpc_iter_history, IM_ARRAYSIZE(ldpc_iter_history), 0, "", 0.0f, (float)d_ldpc_iterations,
                                             ImVec2(200 * ui_scale, 50 * ui_scale));
                    ImGui::Separator();

                    // ---- LLR Scaling ----
                    ImGui::Text("SNR   : ");
                    ImGui::SameLine();
                    ImGui::TextColored(style::theme.green, "%.1f dB", llr_snr);
                    ImGui::Text("Scale : ");
                    ImGui::SameLine();
                    ImGui::TextColored(style::theme.green, "%.2fx", llr_scale);
                    if (d_llr_calibrated)
                    {
                        ImGui::Text("σ²    : ");
                        ImGui::SameLine();
                        ImGui::TextColored(style::theme.green, "%.2f", llr_sigma2);
                    }
                    std::memmove(&llr_scale_history[0], &llr_scale_history[1], (200 - 1) * sizeof(float));
                    llr_scale_history[200 - 1] = llr_scale;

                    widgets::ThemedPlotLines(style::theme.plot_bg.Value, "##llrscale", llr_scale_history, IM_ARRAYSIZE(llr_scale_history), 0, "", 0.0f, 8.0f,
                                             ImVec2(200 * ui_scale, 50 * ui_scale));

                    if (d_internal_stream)
                    {
                        ImGui::Separator();
                        ImGui::Text("State : ");
                        ImGui::SameLine();
                        if (deframer->getState() == deframer->STATE_NOSYNC)
                            ImGui::TextColored(style::theme.red, "NOSYNC");
                        else if (deframer->getState() == deframer->STATE_SYNCING)
                            ImGui::TextColored(style::theme.orange, "SYNCING");
                        else
                            ImGui::TextColored(style::theme.green, "SYNCED");
                    }

                    // ---- Status ----
                    ImGui::Separator();
                    ImGui::Text("Lock  : ");
                    ImGui::SameLine();
                    ImGui::TextColored(correlator_locked ? style::theme.green : style::theme.orange, correlator_locked ? "SYNCED" : "NOSYNC");
                    ImGui::Text("Algo  : ");
                    ImGui::SameLine();
                    if (d_ldpc_algorithm == codings::ldpc::LDPC_NORMALIZED_MIN_SUM)
                        ImGui::TextColored(style::theme.green, "NMS a=%.2f", d_ldpc_nms_alpha_q8 / 256.0f);
                    else if (d_ldpc_algorithm == codings::ldpc::LDPC_SELF_CORRECTED_MIN_SUM)
                        ImGui::TextColored(style::theme.green, "SCMS");
                    else if (d_ldpc_algorithm == codings::ldpc::LDPC_SUM_PRODUCT)
                        ImGui::TextColored(style::theme.green, "Sum-Prod (BP)");
                    else
                        ImGui::TextColored(style::theme.green, "Min-Sum");
                }
                ImGui::EndGroup();

                ImGui::SameLine();

                ImGui::BeginGroup();
                {
                    ImGui::Button("MER", {200 * ui_scale, 20 * ui_scale});
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

                        widgets::ThemedPlotLines(style::theme.plot_bg.Value, "##mer", mer_history, IM_ARRAYSIZE(mer_history), 0, "", 0.0f, 30.0f, ImVec2(200 * ui_scale, 50 * ui_scale));
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
