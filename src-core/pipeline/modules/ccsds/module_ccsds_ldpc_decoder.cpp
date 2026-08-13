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

                correlator = std::make_unique<CorrelatorGeneric>(
                    d_constellation, d_ldpc_rate == codings::ldpc::RATE_7_8 ? satdump::unsigned_to_bitvec<uint32_t>(0x1acffc1d) : satdump::unsigned_to_bitvec<uint64_t>(0x034776c7272895b0),
                    d_ldpc_frame_size);

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

                // Offset Min-Sum beta (0 = plain min-sum, i.e. current behaviour)
                if (d_parameters.contains("ldpc_oms_beta"))
                    ldpc_dec->set_oms_beta((int16_t)d_parameters["ldpc_oms_beta"].get<int>());

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

                    int pos = correlator->correlate((int8_t *)soft_buffer, phase, swap, correlator_cor, d_ldpc_frame_size);

                    correlator_locked = pos == 0; // Update locking state

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

                            if (d_constellation == dsp::BPSK)
                            {
                                std::vector<complex_t> tmp(total_soft);
                                for (int i = 0; i < total_soft; i++)
                                    tmp[i] = complex_t(ldpc_input_buffer[i] / 127.0f, 0.0f);
                                snr_estimator.update(tmp.data(), total_soft);
                            }
                            else // QPSK / OQPSK: interleaved I, Q soft bits
                            {
                                int n = total_soft / 2;
                                std::vector<complex_t> tmp(n);
                                for (int i = 0; i < n; i++)
                                    tmp[i] = complex_t(ldpc_input_buffer[i * 2 + 0] / 127.0f,
                                                       ldpc_input_buffer[i * 2 + 1] / 127.0f);
                                snr_estimator.update(tmp.data(), n);
                            }

                            llr_snr = snr_estimator.snr();

                            // npwr = 2 * 10^(-SNR/20), scale = 1/npwr — same as DVB-S2 demod
                            float npwr = 2.0f * powf(10.0f, -llr_snr / 20.0f);
                            llr_scale = std::clamp(1.0f / npwr, 0.25f, 8.0f);

                            for (int i = 0; i < total_soft; i++)
                            {
                                float v = ldpc_input_buffer[i] * llr_scale;
                                ldpc_input_buffer[i] = (int8_t)std::clamp(v, -127.0f, 127.0f);
                            }
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
                v["ldpc_corr"] = ldpc_corr;
                v["llr_snr"] = llr_snr;
                v["llr_scale"] = llr_scale;
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

                ImGui::BeginGroup();
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
                ImGui::EndGroup();

                ImGui::SameLine();

                ImGui::BeginGroup();
                {
                    ImGui::Button("Correlator", {200 * ui_scale, 20 * ui_scale});
                    {
                        ImGui::Text("Corr  : ");
                        ImGui::SameLine();
                        ImGui::TextColored(correlator_locked ? style::theme.green : style::theme.orange, UITO_C_STR(correlator_cor));

                        std::memmove(&cor_history[0], &cor_history[1], (200 - 1) * sizeof(float));
                        cor_history[200 - 1] = correlator_cor;

                        if (d_ldpc_asm_size == 32)
                            widgets::ThemedPlotLines(style::theme.plot_bg.Value, "##", cor_history, IM_ARRAYSIZE(cor_history), 0, "", 15.0f, 35.0f, ImVec2(200 * ui_scale, 50 * ui_scale));
                        else
                            widgets::ThemedPlotLines(style::theme.plot_bg.Value, "##", cor_history, IM_ARRAYSIZE(cor_history), 0, "", 25.0f, 70.0f, ImVec2(200 * ui_scale, 50 * ui_scale));
                    }

                    ImGui::Button("LDPC", {200 * ui_scale, 20 * ui_scale});
                    {
                        ImGui::Text("Diff  : ");
                        ImGui::SameLine();
                        ImGui::TextColored(ldpc_corr > 10 ? style::theme.orange : style::theme.green, UITO_C_STR(ldpc_corr));

                        std::memmove(&ldpc_history[0], &ldpc_history[1], (200 - 1) * sizeof(float));
                        ldpc_history[200 - 1] = ldpc_corr;

                        widgets::ThemedPlotLines(style::theme.plot_bg.Value, "##", ldpc_history, IM_ARRAYSIZE(ldpc_history), 0, "", 0.0f, d_ldpc_codeword_size / 20,
                                                 ImVec2(200 * ui_scale, 50 * ui_scale));
                    }

                    ImGui::Button("LLR Scaling", {200 * ui_scale, 20 * ui_scale});
                    {
                        ImGui::Text("SNR   : ");
                        ImGui::SameLine();
                        ImGui::TextColored(style::theme.green, "%.1f dB", llr_snr);
                        ImGui::Text("Scale : ");
                        ImGui::SameLine();
                        ImGui::TextColored(style::theme.green, "%.2fx", llr_scale);
                        ImGui::Text("OMS β: ");
                        ImGui::SameLine();
                        int16_t cur_beta = d_parameters.contains("ldpc_oms_beta") ? (int16_t)d_parameters["ldpc_oms_beta"].get<int>() : 0;
                        ImGui::TextColored(cur_beta > 0 ? style::theme.green : style::theme.orange, "%d", (int)cur_beta);

                        std::memmove(&llr_scale_history[0], &llr_scale_history[1], (200 - 1) * sizeof(float));
                        llr_scale_history[200 - 1] = llr_scale;

                        widgets::ThemedPlotLines(style::theme.plot_bg.Value, "##llrscale", llr_scale_history, IM_ARRAYSIZE(llr_scale_history), 0, "", 0.0f, 8.0f,
                                                 ImVec2(200 * ui_scale, 50 * ui_scale));
                    }

                    if (d_internal_stream)
                    {
                        ImGui::Spacing();

                        ImGui::Button("Deframer", {200 * ui_scale, 20 * ui_scale});
                        {
                            ImGui::Text("State : ");

                            ImGui::SameLine();

                            if (deframer->getState() == deframer->STATE_NOSYNC)
                                ImGui::TextColored(style::theme.red, "NOSYNC");
                            else if (deframer->getState() == deframer->STATE_SYNCING)
                                ImGui::TextColored(style::theme.orange, "SYNCING");
                            else
                                ImGui::TextColored(style::theme.green, "SYNCED");
                        }
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
