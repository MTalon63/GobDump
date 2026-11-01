#include "module_dvbs2_demod.h"
#include <array>
#include <cmath>
#include "codings/dvb-s2/modcod_to_cfg.h"
#include "common/dsp/filter/firdes.h"
#include "imgui/imgui.h"
#include "logger.h"
#if defined(__SSE2__)
#include <emmintrin.h>
#endif

#if defined(__SSE2__)
// Scalar repack is MSB-first, _mm_movemask_epi8 is LSB-first.
static inline const unsigned char *rev8_table()
{
    static const std::array<unsigned char, 256> t = [] {
        std::array<unsigned char, 256> r = {};
        for (int i = 0; i < 256; i++)
            for (int b = 0; b < 8; b++)
                if (i & (1 << b))
                    r[i] |= (unsigned char)(1 << (7 - b));
        return r;
    }();
    return t.data();
}
#endif

namespace satdump
{
    namespace pipeline
    {
        namespace dvb
        {
            DVBS2DemodModule::DVBS2DemodModule(std::string input_file, std::string output_file_hint, nlohmann::json parameters)
                : BaseDemodModule(input_file, output_file_hint, parameters), constellation_s2(90.0f / 127.0f, 90.0f / 127.0f, demod_constellation_size)
            {
                // Parse params
                if (parameters.count("rrc_alpha") > 0)
                    d_rrc_alpha = parameters["rrc_alpha"].get<float>();
                else
                    throw satdump_exception("RRC Alpha parameter must be present!");

                if (parameters.count("rrc_taps") > 0)
                    d_rrc_taps = parameters["rrc_taps"].get<int>();

                if (parameters.count("pll_bw") > 0)
                    d_loop_bw = parameters["pll_bw"].get<float>();
                else
                    throw satdump_exception("PLL BW parameter must be present!");

                if (parameters.count("freq_prop_factor") > 0)
                    freq_propagation_factor = parameters["freq_prop_factor"].get<float>();

                if (parameters.count("clock_alpha") > 0)
                {
                    float clock_alpha = parameters["clock_alpha"].get<float>();
                    d_clock_gain_omega = pow(clock_alpha, 2) / 4.0;
                    d_clock_gain_mu = clock_alpha;
                }

                if (parameters.count("clock_gain_omega") > 0)
                    d_clock_gain_omega = parameters["clock_gain_omega"].get<float>();

                if (parameters.count("clock_mu") > 0)
                    d_clock_mu = parameters["clock_mu"].get<float>();

                if (parameters.count("clock_gain_mu") > 0)
                    d_clock_gain_mu = parameters["clock_gain_mu"].get<float>();

                if (parameters.count("clock_omega_relative_limit") > 0)
                    d_clock_omega_relative_limit = parameters["clock_omega_relative_limit"].get<float>();

                if (parameters.count("modcod") > 0)
                    d_modcod = parameters["modcod"].get<int>();
                else
                    throw satdump_exception("MODCOD parameter must be present!");

                if (parameters.count("shortframes") > 0)
                    d_shortframes = parameters["shortframes"].get<bool>();

                if (parameters.count("pilots") > 0)
                    d_pilots = parameters["pilots"].get<bool>();

                if (parameters.count("sof_thresold") > 0)
                    d_sof_thresold = parameters["sof_thresold"].get<float>();

                if (parameters.count("ldpc_trials") > 0)
                    d_max_ldpc_trials = parameters["ldpc_trials"].get<int>();

                if (parameters.count("ldpc_early_term") > 0)
                    d_ldpc_early_term = parameters["ldpc_early_term"].get<bool>();

                if (parameters.count("ldpc_min_iterations") > 0)
                    d_ldpc_min_iterations = parameters["ldpc_min_iterations"].get<int>();

                if (parameters.count("ldpc_adaptive") > 0)
                    d_ldpc_adaptive = parameters["ldpc_adaptive"].get<bool>();
                if (parameters.count("ldpc_trials_good") > 0)
                    d_ldpc_trials_good = parameters["ldpc_trials_good"].get<int>();
                if (parameters.count("ldpc_trials_bad") > 0)
                    d_ldpc_trials_bad = parameters["ldpc_trials_bad"].get<int>();
                if (parameters.count("ldpc_snr_good") > 0)
                    d_ldpc_snr_good = parameters["ldpc_snr_good"].get<float>();
                if (parameters.count("ldpc_snr_bad") > 0)
                    d_ldpc_snr_bad = parameters["ldpc_snr_bad"].get<float>();

                if (parameters.count("mt_bch") > 0)
                    d_multithread_bch = parameters["mt_bch"].get<bool>();

                // Window Name in the UI
                name = "DVB-S2 Demodulator";

                ring_buffer = std::make_unique<dsp::RingBuffer<int8_t>>();
                ring_buffer2 = std::make_unique<dsp::RingBuffer<uint8_t>>();
            }

            void DVBS2DemodModule::init()
            {
                BaseDemodModule::initb();

                float g1 = 0, g2 = 0;

                // Parse modcod number
                auto cfg = dvbs2::get_dvbs2_cfg(d_modcod, d_shortframes, d_pilots);

                frame_slot_count = cfg.frame_slot_count;
                s2_constellation = cfg.constellation;
                s2_constel_obj_type = cfg.constel_obj_type;
                s2_framesize = cfg.framesize;
                s2_coderate = cfg.coderate;
                g1 = cfg.g1;
                g2 = cfg.g2;

                // RRC
                rrc = std::make_shared<dsp::FIRBlock<complex_t>>(agc->output_stream, dsp::firdes::root_raised_cosine(1, final_samplerate, d_symbolrate, d_rrc_alpha, d_rrc_taps));

                // Clock recovery
                rec = std::make_shared<dsp::MMClockRecoveryBlock<complex_t>>(rrc->output_stream, final_sps, d_clock_gain_omega, d_clock_mu, d_clock_gain_mu, d_clock_omega_relative_limit);

                // Freq correction
                freq_sh = std::make_shared<dsp::FreqShiftBlock>(rec->output_stream, 1, 0);

                // PL (SOF) Synchronization
                pl_sync = std::make_shared<dvbs2::S2PLSyncBlock>(freq_sh->output_stream, frame_slot_count, d_pilots);
                pl_sync->thresold = d_sof_thresold;

                // PLL
                s2_pll = std::make_shared<dvbs2::S2PLLBlock>(pl_sync->output_stream, d_loop_bw);
                s2_pll->pilots = d_pilots;
                s2_pll->constellation = std::make_shared<dsp::constellation_t>(s2_constel_obj_type, g1, g2);
                s2_pll->constellation->make_lut(256);
                s2_pll->frame_slot_count = frame_slot_count;
                s2_pll->pls_code = d_modcod << 2 | d_shortframes << 1 | d_pilots;
                s2_pll->update();

                // BB to soft syms
                s2_bb_to_soft = std::make_shared<dvbs2::S2BBToSoft>(s2_pll->output_stream);
                s2_bb_to_soft->pilots = d_pilots;
                s2_bb_to_soft->constellation = std::make_shared<dsp::constellation_t>(s2_constel_obj_type, g1, g2);
                s2_bb_to_soft->constellation->make_lut(256);
                s2_bb_to_soft->frame_slot_count = frame_slot_count;
                s2_bb_to_soft->deinterleaver = std::make_shared<dvbs2::S2Deinterleaver>(s2_constellation, s2_framesize, s2_coderate);

                // Init the rest
                ldpc_decoder = std::make_unique<dvbs2::BBFrameLDPC>(s2_framesize, s2_coderate);
                ldpc_decoder->setEarlyTermination(d_ldpc_early_term);
                ldpc_decoder->setMinIterations(d_ldpc_min_iterations);
                bch_decoder = std::make_unique<dvbs2::BBFrameBCH>(s2_framesize, s2_coderate);
                descramber = std::make_unique<dvbs2::BBFrameDescrambler>(s2_framesize, s2_coderate);

                // Info
                logger->info("Output bbframe bits : %d", bch_decoder->dataSize());
            }

            DVBS2DemodModule::~DVBS2DemodModule() {}

            void DVBS2DemodModule::process()
            {
                if (input_data_type == DATA_FILE)
                    filesize = file_source->getFilesize();
                else
                    filesize = 0;

                if (output_data_type == DATA_FILE)
                {
                    data_out = std::ofstream(d_output_file_hint + ".bbframe", std::ios::binary);
                    d_output_file = d_output_file_hint + ".bbframe";
                }

                logger->info("MODCOD : %d", d_modcod);
                logger->info("Using input baseband " + d_input_file);
                logger->info("Demodulating to " + d_output_file_hint + ".bbframe");
                logger->info("Buffer size : %d", d_buffer_size);

                time_t lastTime = 0;

                // Stage the frame locally so a blocked ring write does not hold bb_to_soft's buffer.
                std::vector<int8_t> frame_copy(64800);

                // Start
                BaseDemodModule::start();
                rrc->start();
                rec->start();
                freq_sh->start();
                pl_sync->start();
                s2_pll->start();
                s2_bb_to_soft->start();

                ring_buffer->init(1000000);
                if (d_multithread_bch)
                    ring_buffer2->init(1000000);

                process_s2_th = std::thread(&DVBS2DemodModule::process_s2, this);
                if (d_multithread_bch)
                    process_bch_th = std::thread(&DVBS2DemodModule::process_s2_bch, this);

                // Drain bb_to_soft even after the file ends, or the tail never reaches the FEC.
                while (demod_should_run() || !s2_bb_to_soft->output_stream->exhausted())
                {
                    int dat_size = s2_bb_to_soft->output_stream->read();

                    if (dat_size <= 0)
                    {
                        s2_bb_to_soft->output_stream->flush();
                        continue;
                    }

                    constellation_s2.pushComplexPL(&s2_pll->output_stream->readBuf[0], 90);
                    constellation_s2.pushComplexSlots(&s2_pll->output_stream->readBuf[90], frame_slot_count * 90);

                    snr_estimator.update(&s2_pll->output_stream->readBuf[90], frame_slot_count * 90);
                    snr = snr_estimator.snr();
                    d_snr_shared.store(snr, std::memory_order_relaxed);

                    if (snr > peak_snr)
                        peak_snr = snr;

                    pushSNRAudioFeedback(frame_slot_count * 90, d_symbolrate);

                    // Adaptive LLR scaling: update constellation LUTs based on measured SNR
                    frame_counter++;
                    if (frame_counter >= LLR_UPDATE_INTERVAL)
                    {
                        frame_counter = 0;
                        // npwr = 2 * scale * 10^(-SNR/20), matching demod_soft_calc.
                        float scale_pll = s2_pll->constellation->get_scale_factor();
                        float scale_bb = s2_bb_to_soft->constellation->get_scale_factor();
                        float npwr_pll = 2.0f * scale_pll * powf(10.0f, -snr / 20.0f);
                        float npwr_bb = 2.0f * scale_bb * powf(10.0f, -snr / 20.0f);
                        // Rebuild off-thread; the LUT is published atomically.
                        auto cons_pll = s2_pll->constellation;
                        auto cons_bb = s2_bb_to_soft->constellation;
                        std::thread([cons_pll, cons_bb, npwr_pll, npwr_bb]() {
                            cons_pll->set_noise_power(npwr_pll);
                            cons_bb->set_noise_power(npwr_bb);
                        }).detach();
                    }

                    display_freq = dsp::rad_to_hz(current_freq / final_sps, final_samplerate);

                    detected_modcod = s2_bb_to_soft->detect_modcod;
                    detected_shortframes = s2_bb_to_soft->detect_shortframes;
                    detected_pilots = s2_bb_to_soft->detect_pilots;

                    const int frame_sz = d_shortframes ? 16200 : 64800;
                    memcpy(frame_copy.data(), s2_bb_to_soft->output_stream->readBuf, frame_sz);
                    s2_bb_to_soft->output_stream->flush();
                    ring_buffer->write(frame_copy.data(), frame_sz);

                    // Propagate frequency to an earlier rotator, slowly
                    current_freq -= s2_pll->getFreq() * freq_propagation_factor;
                    freq_sh->set_freq_raw(current_freq);

                    if (input_data_type == DATA_FILE)
                        progress = file_source->getPosition();
                    if (time(NULL) % 10 == 0 && lastTime != time(NULL))
                    {
                        lastTime = time(NULL);
                        logger->info("Progress " + std::to_string(round(((double)progress / (double)filesize) * 1000.0) / 10.0) + "%%, SNR : " + std::to_string(snr) + "dB," +
                                     " Peak SNR: " + std::to_string(peak_snr) + "dB");
                    }
                }

                should_stop = true;

                if (input_data_type == DATA_FILE)
                    stop();
            }

            nlohmann::json DVBS2DemodModule::getModuleStats()
            {
                nlohmann::json v;
                v["progress"] = ((double)progress / (double)filesize);
                v["snr"] = snr;
                v["peak_snr"] = peak_snr;
                v["freq"] = display_freq;
                v["ldpc_trials"] = ldpc_trials;
                v["bch_corrections"] = bch_corrections;
                return v;
            }

            void DVBS2DemodModule::process_s2()
            {
                int8_t *sym_buffer = new int8_t[64800 * 32];
                uint8_t *repacker_buffer = new uint8_t[64800 * 32];

                while (!ring_buffer->exhausted())
                {
                    // Read frame by frame so a short final batch is not discarded.
                    const int frame_bytes = d_shortframes ? 16200 : 64800;
                    int got = 0;
                    for (int f = 0; f < dvbs2::simd_type::SIZE; f++)
                    {
                        int r = ring_buffer->read(sym_buffer + (size_t)f * frame_bytes, frame_bytes);
                        if (r <= 0)
                            break;
                        got++;
                    }

                    if (got <= 0)
                        continue;

                    // Idle lanes: zeroed soft bits are a valid all-zero codeword, so the
                    // batch can still exit early instead of decoding stale/uninit data.
                    if (got < dvbs2::simd_type::SIZE)
                        memset(sym_buffer + (size_t)got * frame_bytes, 0,
                               (size_t)(dvbs2::simd_type::SIZE - got) * frame_bytes);

                    {
                        int trials_cap = d_max_ldpc_trials;
                        if (d_ldpc_adaptive)
                        {
                            // 0 means the SNR estimator has not settled; keep the full cap.
                            float s = d_snr_shared.load(std::memory_order_relaxed);
                            if (s > 0.0f)
                            {
                                float t = (s - d_ldpc_snr_bad) / (d_ldpc_snr_good - d_ldpc_snr_bad);
                                t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
                                trials_cap = (int)std::lround(d_ldpc_trials_bad + t * (d_ldpc_trials_good - d_ldpc_trials_bad));
                                if (trials_cap < d_ldpc_min_iterations)
                                    trials_cap = d_ldpc_min_iterations;
                            }
                        }

                        ldpc_trials = ldpc_decoder->decode(sym_buffer, trials_cap);
                        if (ldpc_trials < 0)
                            ldpc_trials = trials_cap;
                    }

                    // Only forward lanes that arrived; the rest hold stale data.
                    for (int ff = 0; ff < got; ff++)
                    {
                        int8_t *buf = &sym_buffer[frame_bytes * ff];

#if defined(__SSE2__)
                        // 16 sign bits per movemask instead of byte-at-a-time shifts.
                        const int nbits = ldpc_decoder->dataSize();
                        memset(repacker_buffer, 0, nbits / 8);
                        const unsigned char *rev = rev8_table();
                        const __m128i zero = _mm_setzero_si128();
                        int i = 0;
                        for (; i + 16 <= nbits; i += 16)
                        {
                            __m128i v = _mm_loadu_si128((const __m128i *)(buf + i));
                            int m = _mm_movemask_epi8(_mm_cmpgt_epi8(zero, v));
                            repacker_buffer[i / 8] = rev[m & 0xFF];
                            repacker_buffer[i / 8 + 1] = rev[(m >> 8) & 0xFF];
                        }
                        for (; i < nbits; i++)
                            repacker_buffer[i / 8] = (uint8_t)(repacker_buffer[i / 8] << 1) | (buf[i] < 0);
#else
                        memset(repacker_buffer, 0, ldpc_decoder->dataSize());
                        for (int i = 0; i < ldpc_decoder->dataSize(); i++)
                            repacker_buffer[i / 8] = repacker_buffer[i / 8] << 1 | (buf[i] < 0);
#endif

                        if (!d_multithread_bch)
                        {
                            bch_corrections = bch_decoder->decode(repacker_buffer);
                            descramber->work(repacker_buffer);

                            if (output_data_type == DATA_FILE)
                                data_out.write((char *)repacker_buffer, bch_decoder->dataSize() / 8);
                            else
                                output_fifo->write((uint8_t *)repacker_buffer, bch_decoder->dataSize() / 8);
                        }
                        else
                        {
                            ring_buffer2->write((uint8_t *)repacker_buffer, ldpc_decoder->dataSize() / 8);
                        }
                    }
                }

                logger->info("Exit FEC Thead!");

                delete[] sym_buffer;
                delete[] repacker_buffer;
            }

            void DVBS2DemodModule::process_s2_bch()
            {
                logger->info("Starting BCH Thead!");

                uint8_t *repacker_buffer = new uint8_t[64800];

                while (!ring_buffer2->exhausted())
                {
                    int bch_fsize = ldpc_decoder->dataSize() / 8;
                    int bch_dsize = bch_decoder->dataSize() / 8;
                    int read = ring_buffer2->read(repacker_buffer, bch_fsize);

                    if (read <= 0)
                        continue;

                    bch_corrections = bch_decoder->decode(repacker_buffer);
                    descramber->work(repacker_buffer);

                    if (output_data_type == DATA_FILE)
                        data_out.write((char *)repacker_buffer, bch_dsize);
                    else
                        output_fifo->write((uint8_t *)repacker_buffer, bch_dsize);
                }

                logger->info("Exit BCH Thead!");

                delete[] repacker_buffer;
            }

            void DVBS2DemodModule::stop()
            {
                // Stop
                BaseDemodModule::stop();

                rrc->stop();
                rec->stop();
                freq_sh->stop();
                pl_sync->stop();
                s2_pll->stop();
                s2_bb_to_soft->stop();
                s2_bb_to_soft->output_stream->stopReader();
                ring_buffer->stopWriter();
                ring_buffer->stopReader();
                if (d_multithread_bch)
                {
                    ring_buffer2->stopWriter();
                    ring_buffer2->stopReader();
                }

                if (process_s2_th.joinable())
                    process_s2_th.join();
                if (d_multithread_bch)
                    if (process_bch_th.joinable())
                        process_bch_th.join();

                if (output_data_type == DATA_FILE)
                    data_out.close();
            }

            void DVBS2DemodModule::drawUI(bool window)
            {
                ImGui::Begin(name.c_str(), NULL, window ? 0 : NOWINDOW_FLAGS);

                ImGui::BeginGroup();
                constellation_s2.draw(); // Constellation
                ImGui::EndGroup();

                ImGui::SameLine();

                ImGui::BeginGroup();
                {
                    // Show SNR information
                    ImGui::Button("Signal", {200 * ui_scale, 20 * ui_scale});
                    ImGui::Text("Freq : ");
                    ImGui::SameLine();
                    ImGui::TextColored(style::theme.orange, "%.0f Hz", display_freq);
                    snr_plot.draw(snr, peak_snr);
                    if (!d_is_streaming_input)
                        if (ImGui::Checkbox("Show FFT", &show_fft))
                            fft_splitter->set_enabled("fft", show_fft);

                    // Header
                    ImGui::Button("Header", {200 * ui_scale, 20 * ui_scale});
                    ImGui::Text("MODCOD : ");
                    ImGui::SameLine();
                    ImGui::TextColored(style::theme.green, UITO_C_STR(detected_modcod));
                    ImGui::Text("Frames : ");
                    ImGui::SameLine();
                    ImGui::TextColored(style::theme.green, detected_shortframes ? "Short" : "Normal");
                    ImGui::Text("Pilots : ");
                    ImGui::SameLine();
                    ImGui::TextColored(detected_pilots ? style::theme.green : style::theme.red, detected_pilots ? "ON" : "OFF");
                }
                ImGui::EndGroup();

                ImGui::SameLine();

                ImGui::BeginGroup();
                {

                    // Show FEC information
                    ImGui::Button("FEC", {200 * ui_scale, 20 * ui_scale});
                    ldpc_viewer.draw(ldpc_trials, 5, 0, "LDPC Trials :");
                    bch_viewer.draw(bch_corrections, 10, 0, "BCH Corrections :");
                }
                ImGui::EndGroup();

                if (!d_is_streaming_input)
                    ImGui::ProgressBar((double)progress / (double)filesize, ImVec2(ImGui::GetContentRegionAvail().x, 20 * ui_scale));

                drawStopButton();

                ImGui::End();

                drawFFT();
            }

            std::string DVBS2DemodModule::getID() { return "dvbs2_demod"; }

            std::shared_ptr<ProcessingModule> DVBS2DemodModule::getInstance(std::string input_file, std::string output_file_hint, nlohmann::json parameters)
            {
                return std::make_shared<DVBS2DemodModule>(input_file, output_file_hint, parameters);
            }
        } // namespace dvb
    } // namespace pipeline
} // namespace satdump