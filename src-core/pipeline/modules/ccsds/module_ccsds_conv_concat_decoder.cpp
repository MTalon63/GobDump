#include "module_ccsds_conv_concat_decoder.h"
#include "common/codings/differential/nrzm.h"
#include "common/codings/randomization.h"
#include "common/widgets/themed_widgets.h"
#include "core/exception.h"
#include "imgui/imgui.h"
#include "logger.h"
#include <chrono>
#include <cstdint>

namespace satdump
{
    namespace pipeline
    {
        namespace ccsds
        {
            CCSDSConvConcatDecoderModule::CCSDSConvConcatDecoderModule(std::string input_file, std::string output_file_hint, nlohmann::json parameters)
                : base::FileStreamToFileStreamModule(input_file, output_file_hint, parameters), is_ccsds(parameters.count("ccsds") > 0 ? parameters["ccsds"].get<bool>() : true),

                  d_constellation_str(parameters["constellation"].get<std::string>()),

                  d_iq_invert(parameters.count("iq_invert") > 0 ? parameters["iq_invert"].get<bool>() : false), d_cadu_size(parameters["cadu_size"].get<int>()),
                  d_cadu_bytes(ceil(d_cadu_size / 8.0)), // If we can't use complete bytes, add one and padding
                  d_buffer_size(std::max<int>(d_cadu_size, 8192)),

                  d_viterbi_outsync_after(parameters["viterbi_outsync_after"].get<int>()), d_viterbi_ber_threasold(parameters["viterbi_ber_thresold"].get<float>()),

                  d_deframer_nosync_timeout(parameters.count("deframer_nosync_timeout") > 0 ? parameters["deframer_nosync_timeout"].get<double>() : 0.0),

                  d_diff_decode(parameters.count("nrzm") > 0 ? parameters["nrzm"].get<bool>() : false),

                  d_derand(parameters.count("derandomize") > 0 ? parameters["derandomize"].get<bool>() : true),
                  d_derand_after_rs(parameters.count("derand_after_rs") > 0 ? parameters["derand_after_rs"].get<bool>() : false),
                  d_derand_from(parameters.count("derand_start") > 0 ? parameters["derand_start"].get<int>() : 4),

                  d_conv_type(parameters.count("conv_rate") > 0 ? parameters["conv_rate"].get<std::string>() : "1/2"),

                  d_rs_interleaving_depth(parameters["rs_i"].get<int>()), d_rs_fill_bytes(parameters.count("rs_fill_bytes") > 0 ? parameters["rs_fill_bytes"].get<int>() : -1),
                  d_rs_dualbasis(parameters.count("rs_dualbasis") > 0 ? parameters["rs_dualbasis"].get<bool>() : true),
                  d_rs_type(parameters.count("rs_type") > 0 ? parameters["rs_type"].get<std::string>() : "none"),
                  d_rs_usecheck(parameters.count("rs_usecheck") > 0 ? parameters["rs_usecheck"].get<bool>() : false)
            {
                soft_buffer = new int8_t[d_buffer_size];
                frame_buffer = new uint8_t[d_buffer_size * 8]; // Larger by safety
                d_bpsk_90 = false;
                d_oqpsk_mode = false;

                // Get constellation
                if (d_constellation_str == "bpsk")
                {
                    d_constellation = dsp::BPSK;
                    d_bpsk_90 = false;
                }
                else if (d_constellation_str == "bpsk_90")
                {
                    d_constellation = dsp::BPSK;
                    d_bpsk_90 = true;
                }
                else if (d_constellation_str == "qpsk")
                    d_constellation = dsp::QPSK;
                else if (d_constellation_str == "oqpsk")
                {
                    d_constellation = dsp::QPSK;
                    d_oqpsk_mode = true;
                }
                else
                    throw satdump_exception("CCSDS Concatenated 1/2 Decoder : invalid constellation type!");

                std::vector<phase_t> d_phases;

                // Get phases for the viterbi decoder to check
                if (d_constellation == dsp::BPSK && !d_bpsk_90)
                    d_phases = {PHASE_0};
                else if (d_constellation == dsp::BPSK && d_bpsk_90)
                    d_phases = {PHASE_90};
                else if (d_constellation == dsp::QPSK)
                    d_phases = {PHASE_0, PHASE_90};

                // Parse RS
                reedsolomon::RS_TYPE rstype = reedsolomon::RS223;
                if (d_rs_interleaving_depth != 0)
                {
                    if (d_rs_type == "rs223")
                        rstype = reedsolomon::RS223;
                    else if (d_rs_type == "rs239")
                        rstype = reedsolomon::RS239;
                    else
                        throw satdump_exception("CCSDS Concatenated 1/2 Decoder : invalid Reed-Solomon type!");
                }

                // Parse sync marker if set
                uint32_t asm_sync = 0x1acffc1d;
                if (parameters.count("asm") > 0)
                    asm_sync = std::stoul(parameters["asm"].get<std::string>(), nullptr, 16);

                // Build viterbi decoder pool.
                // conv_rate "auto" creates all 5 rate slots in parallel;
                // a specific rate ("1/2" etc.) creates a single slot (zero overhead vs old code).
                auto makeSlot = [&](vitrate_t rate, const char *sname) -> ViterbiSlot {
                    ViterbiSlot s;
                    s.rate = rate;
                    s.name = sname;
                    s.out  = new uint8_t[d_buffer_size * 8];
                    // Store params so reinit() can recreate the object with a clean trellis
                    s.ber_threshold = d_viterbi_ber_threasold;
                    s.outsync_after = d_viterbi_outsync_after;
                    s.buffer_size   = d_buffer_size;
                    s.phases        = d_phases;
                    s.oqpsk_mode    = d_oqpsk_mode;
                    if (rate == PUNCRATE_1_2)
                        s.v12 = std::make_shared<viterbi::Viterbi1_2>(d_viterbi_ber_threasold, d_viterbi_outsync_after, d_buffer_size, d_phases, d_oqpsk_mode);
                    else if (rate == PUNCRATE_2_3)
                        s.vp = std::make_shared<viterbi::Viterbi_Depunc>(std::make_shared<viterbi::puncturing::Depunc23>(), d_viterbi_ber_threasold, d_viterbi_outsync_after, d_buffer_size, d_phases, d_oqpsk_mode);
                    else if (rate == PUNCRATE_3_4)
                        s.vp = std::make_shared<viterbi::Viterbi_Depunc>(std::make_shared<viterbi::puncturing::Depunc34>(), d_viterbi_ber_threasold, d_viterbi_outsync_after, d_buffer_size, d_phases, d_oqpsk_mode);
                    else if (rate == PUNCRATE_5_6)
                        s.vp = std::make_shared<viterbi::Viterbi_Depunc>(std::make_shared<viterbi::puncturing::Depunc56>(), d_viterbi_ber_threasold, d_viterbi_outsync_after, d_buffer_size, d_phases, d_oqpsk_mode);
                    else if (rate == PUNCRATE_7_8)
                        s.vp = std::make_shared<viterbi::Viterbi_Depunc>(std::make_shared<viterbi::puncturing::Depunc78>(), d_viterbi_ber_threasold, d_viterbi_outsync_after, d_buffer_size, d_phases, d_oqpsk_mode);
                    return s;
                };

                if (d_conv_type == "auto")
                {
                    d_auto_rate = true;
                    d_rate_pool.push_back(makeSlot(PUNCRATE_1_2, "1/2"));
                    d_rate_pool.push_back(makeSlot(PUNCRATE_2_3, "2/3"));
                    d_rate_pool.push_back(makeSlot(PUNCRATE_3_4, "3/4"));
                    d_rate_pool.push_back(makeSlot(PUNCRATE_5_6, "5/6"));
                    d_rate_pool.push_back(makeSlot(PUNCRATE_7_8, "7/8"));
                }
                else
                {
                    d_auto_rate = false;
                    vitrate_t rate;
                    const char *rname;
                    if (d_conv_type == "1/2")      { rate = PUNCRATE_1_2; rname = "1/2"; }
                    else if (d_conv_type == "2/3") { rate = PUNCRATE_2_3; rname = "2/3"; }
                    else if (d_conv_type == "3/4") { rate = PUNCRATE_3_4; rname = "3/4"; }
                    else if (d_conv_type == "5/6") { rate = PUNCRATE_5_6; rname = "5/6"; }
                    else if (d_conv_type == "7/8") { rate = PUNCRATE_7_8; rname = "7/8"; }
                    else throw satdump_exception("CCSDS Concatenated Decoder : invalid conv_rate!");
                    d_rate_pool.push_back(makeSlot(rate, rname));
                }

                d_active_rate_idx = 0;
                viterbi_rate_str  = d_rate_pool[0].name;
                viterbi_out       = d_rate_pool[0].out;

                deframer = std::make_shared<deframing::BPSK_CCSDS_Deframer>(d_cadu_size, asm_sync);
                if (d_rs_interleaving_depth != 0)
                    reed_solomon = std::make_shared<reedsolomon::ReedSolomon>(rstype, d_rs_fill_bytes);

                if (d_cadu_size % 8 != 0) // If this is not a perfect byte length match, pad the frames
                {
                    deframer->CADU_PADDING = d_cadu_size % 8;
                    logger->info("Frames will be padded!");
                }

                fsfsm_file_ext = is_ccsds ? ".cadu" : ".frm";
            }

            CCSDSConvConcatDecoderModule::~CCSDSConvConcatDecoderModule()
            {
                for (auto &slot : d_rate_pool)
                    delete[] slot.out; // viterbi_out is a non-owning alias; slots own their buffers
                delete[] soft_buffer;
                delete[] frame_buffer;
            }

            void CCSDSConvConcatDecoderModule::process()
            {
                diff::NRZMDiff diff;
                // Measured at the TOP of each iteration so elapsed reflects real wall-clock
                // time between buffer reads, not decode time.
                auto d_last_buffer_time = std::chrono::steady_clock::now();

                while (should_run())
                {
                    // Read a buffer
                    read_data((uint8_t *)soft_buffer, d_buffer_size);

                    if (d_bpsk_90 || d_iq_invert) // Symbols are swapped for some Q/BPSK sats
                        rotate_soft((int8_t *)soft_buffer, d_buffer_size, PHASE_0, true);

                    // Run all viterbi decoders in the pool (1 slot for fixed rate, 5 for auto)
                    for (auto &slot : d_rate_pool)
                        slot.work((int8_t *)soft_buffer, d_buffer_size);

                    // Rate selection — Option A (hysteresis):
                    // Keep the active slot while it stays locked.
                    // Only switch when it drops lock; pick lowest-BER locked alternative.
                    if (d_auto_rate && d_rate_pool[d_active_rate_idx].getState() == 0)
                    {
                        int best_idx  = -1;
                        float best_ber = 999.0f;
                        for (int i = 0; i < (int)d_rate_pool.size(); i++)
                        {
                            if (d_rate_pool[i].getState() != 0)
                            {
                                float b = d_rate_pool[i].ber();
                                if (b < best_ber) { best_ber = b; best_idx = i; }
                            }
                        }
                        if (best_idx >= 0)
                        {
                            logger->info("Puncture rate switch: %s -> %s",
                                         d_rate_pool[d_active_rate_idx].name.c_str(),
                                         d_rate_pool[best_idx].name.c_str());
                            d_active_rate_idx = best_idx;
                        }
                    }

                    // Update active-slot aliases used by the rest of the loop.
                    // Cache ber()/getState() to avoid calling virtual dispatch twice.
                    {
                        ViterbiSlot &active_slot = d_rate_pool[d_active_rate_idx];
                        int vitout_cur       = active_slot.last_vitout;
                        uint8_t *vout_cur    = active_slot.out;
                        float    ber_cur     = active_slot.ber();
                        int      state_cur   = active_slot.getState();

                        viterbi_out      = vout_cur;
                        viterbi_ber      = ber_cur;
                        viterbi_lock     = state_cur;
                        // Only copy string when the active slot actually changed
                        if (viterbi_rate_str != active_slot.name)
                            viterbi_rate_str = active_slot.name;

                        if (d_diff_decode) // Diff decoding if required
                            diff.decode_bits(viterbi_out, vitout_cur);

                        // Run deframer
                        int frames = deframer->work(viterbi_out, vitout_cur, frame_buffer);

                        // Deframer nosync timeout with exponential backoff.
                        //
                        // d_deframer_nosync_timer is used as a signed accumulator:
                        //   >= d_deframer_nosync_timeout  → fire forced reset
                        //   0 .. threshold                → counting toward next reset
                        //   < 0                           → post-reset cooldown; can't fire yet
                        //
                        // After each reset the timer is pre-charged to a negative cooldown equal to
                        // timeout × min(2^reset_count, 8), so consecutive failures back off
                        // exponentially:  1s timeout → 1s, 2s, 4s, 8s, 8s, ... cooldown before retry.
                        // The counter resets to 0 the moment the deframer actually syncs.
                        if (d_deframer_nosync_timeout > 0.0)
                        {
                            auto now = std::chrono::steady_clock::now();
                            double elapsed = std::chrono::duration<double>(now - d_last_buffer_time).count();
                            d_last_buffer_time = now;

                            bool deframer_synced = deframer->getState() == deframer->STATE_SYNCED;

                            if (deframer_synced)
                            {
                                // Deframer reached lock — clear everything
                                d_deframer_nosync_timer      = 0;
                                d_deframer_nosync_reset_count = 0;
                            }
                            else
                            {
                                // Count time only while viterbi has a lock.
                                // Timer may be negative (in cooldown); we still advance it
                                // so it naturally climbs toward 0 and then toward the threshold.
                                if (state_cur != 0)
                                    d_deframer_nosync_timer += elapsed;

                                if (d_deframer_nosync_timer >= d_deframer_nosync_timeout)
                                {
                                    d_deframer_nosync_reset_count++;
                                    // Cooldown = timeout × clamped power-of-two backoff (max 8×)
                                    int backoff_mult = 1 << std::min(d_deframer_nosync_reset_count - 1, 3); // 1, 2, 4, 8
                                    double cooldown  = d_deframer_nosync_timeout * backoff_mult;

                                    logger->warn("Deframer failed to sync for %.1f s — forcing outsync (attempt %d, cooldown %.1f s)",
                                                 d_deframer_nosync_timeout, d_deframer_nosync_reset_count, cooldown);

                                    // Hard reset: recreate decoder objects from scratch so all
                                    // trellis path metrics and survivor paths are zeroed — this
                                    // prevents re-convergence to the same wrong path that caused
                                    // the deframer not to sync in the first place.
                                    for (auto &slot : d_rate_pool)
                                        slot.reinit();
                                    d_active_rate_idx = 0;
                                    deframer->reset();
                                    viterbi_lock = d_rate_pool[0].getState();

                                    // Pre-charge the timer with a negative cooldown debt.
                                    // The next reset cannot fire until the timer has climbed
                                    // through the entire cooldown and then the full timeout period.
                                    d_deframer_nosync_timer = -cooldown;
                                }
                            }
                        }


                        for (int i = 0; i < frames; i++)
                        {
                            uint8_t *cadu = &frame_buffer[i * d_cadu_bytes];

                            if (d_derand && !d_derand_after_rs) // Derand if required, before RS
                                derand_ccsds(&cadu[d_derand_from], d_cadu_bytes - d_derand_from);

                            if (d_rs_interleaving_depth != 0) // RS Correction
                                reed_solomon->decode_interlaved(&cadu[4], d_rs_dualbasis, d_rs_interleaving_depth, errors);

                            bool valid = true;
                            for (int j = 0; j < d_rs_interleaving_depth; j++)
                                if (errors[j] == -1)
                                    valid = false;

                            if (d_derand && d_derand_after_rs) // Derand if required, after RS
                                derand_ccsds(&cadu[d_derand_from], d_cadu_bytes - d_derand_from);

                            if (!d_rs_usecheck || valid)
                            {
                                // Write it out
                                write_data(cadu, d_cadu_bytes);
                            }
                        }
                    }
                }

                cleanup();
            }

            nlohmann::json CCSDSConvConcatDecoderModule::getModuleStats()
            {
                auto v = satdump::pipeline::base::FileStreamToFileStreamModule::getModuleStats();
                v["deframer_lock"] = deframer->getState() == deframer->STATE_SYNCED;
                v["viterbi_ber"] = viterbi_ber;
                v["viterbi_lock"] = viterbi_lock;
                v["viterbi_rate"] = viterbi_rate_str;
                if (d_rs_interleaving_depth != 0)
                {
                    int rs_sum = 0;
                    for (int i = 0; i < d_rs_interleaving_depth; i++) rs_sum += errors[i];
                    v["rs_avg"] = rs_sum / d_rs_interleaving_depth;
                }
                std::string viterbi_state = viterbi_lock == 0 ? "NOSYNC" : "SYNCED";
                std::string deframer_state = deframer->getState() == deframer->STATE_NOSYNC ? "NOSYNC" : (deframer->getState() == deframer->STATE_SYNCING ? "SYNCING" : "SYNCED");
                v["viterbi_state"] = viterbi_state;
                v["deframer_state"] = deframer_state;
                return v;
            }

            void CCSDSConvConcatDecoderModule::drawUI(bool window)
            {
                // Build window title once; only recompute if rate changed (auto mode)
                static std::string s_window_title;
                if (s_window_title.empty() || d_auto_rate)
                {
                    s_window_title = d_auto_rate
                        ? "CCSDS Auto-Rate Concatenated Decoder"
                        : ("CCSDS r=" + d_rate_pool[0].name + " Concatenated Decoder");
                }
                ImGui::Begin(s_window_title.c_str(), NULL, window ? 0 : NOWINDOW_FLAGS);
                float &ber = viterbi_ber;

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
                    ImGui::Button("Viterbi", {200 * ui_scale, 20 * ui_scale});
                    {
                        ImGui::Text("State : ");

                        ImGui::SameLine();

                        if (viterbi_lock == 0)
                            ImGui::TextColored(style::theme.red, "NOSYNC");
                        else
                            ImGui::TextColored(style::theme.green, "SYNCED");

                        ImGui::Text("BER   : ");
                        ImGui::SameLine();
                        ImGui::TextColored(viterbi_lock == 0 ? style::theme.red : style::theme.green, UITO_C_STR(ber));

                        std::memmove(&ber_history[0], &ber_history[1], (200 - 1) * sizeof(float));
                        ber_history[200 - 1] = ber;

                        widgets::ThemedPlotLines(style::theme.plot_bg.Value, "##", ber_history, IM_ARRAYSIZE(ber_history), 0, "", 0.0f, 1.0f, ImVec2(200 * ui_scale, 50 * ui_scale));

                        if (d_auto_rate)
                        {
                            ImGui::Text("Rate  : ");
                            ImGui::SameLine();
                            ImGui::TextColored(viterbi_lock == 0 ? style::theme.red : style::theme.green, "%s", viterbi_rate_str.c_str());
                        }
                    }

                    ImGui::Spacing();

                    ImGui::Button("Deframer", {200 * ui_scale, 20 * ui_scale});
                    {
                        ImGui::Text("State : ");

                        ImGui::SameLine();

                        if (viterbi_lock == 0)
                        {
                            ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), "NOSYNC");
                        }
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

                    ImGui::Spacing();

                    if (d_rs_interleaving_depth != 0)
                    {
                        ImGui::Button("Reed-Solomon", {200 * ui_scale, 20 * ui_scale});
                        {
                            ImGui::Text("RS    : ");
                            for (int i = 0; i < d_rs_interleaving_depth; i++)
                            {
                                ImGui::SameLine();

                                if (viterbi_lock == 0 || deframer->getState() == deframer->STATE_NOSYNC)
                                {
                                    ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), "%i ", i);
                                }
                                else
                                {
                                    if (errors[i] == -1)
                                        ImGui::TextColored(style::theme.red, "%i ", i);
                                    else if (errors[i] > 0)
                                        ImGui::TextColored(style::theme.orange, "%i ", i);
                                    else
                                        ImGui::TextColored(style::theme.green, "%i ", i);
                                }
                            }
                        }
                    }
                }
                ImGui::EndGroup();

                drawProgressBar();

                ImGui::End();
            }

            std::string CCSDSConvConcatDecoderModule::getID() { return "ccsds_conv_concat_decoder"; }

            std::shared_ptr<ProcessingModule> CCSDSConvConcatDecoderModule::getInstance(std::string input_file, std::string output_file_hint, nlohmann::json parameters)
            {
                return std::make_shared<CCSDSConvConcatDecoderModule>(input_file, output_file_hint, parameters);
            }
        } // namespace ccsds
    } // namespace pipeline
} // namespace satdump