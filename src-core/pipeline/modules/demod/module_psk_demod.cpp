#include "module_psk_demod.h"
#include "common/dsp/filter/firdes.h"
#include "imgui/imgui.h"
#include "logger.h"
#include <algorithm>
#include <cmath>
#include <set>

namespace satdump
{
    namespace pipeline
    {
        namespace demod
        {
            PSKDemodModule::PSKDemodModule(std::string input_file, std::string output_file_hint, nlohmann::json parameters) : BaseDemodModule(input_file, output_file_hint, parameters)
            {
                // Buffers
                sym_buffer = new int8_t[d_buffer_size * 4];

                // Parse params
                if (parameters.count("constellation") > 0)
                    constellation_type = parameters["constellation"].get<std::string>();
                else
                    throw satdump_exception("Constellation type parameter must be present!");

                // Reject unknown constellations before init() derefs pll.
                if (constellation_type != "bpsk" && constellation_type != "qpsk" &&
                    constellation_type != "oqpsk" && constellation_type != "8psk" &&
                    constellation_type != "16apsk")
                    throw satdump_exception("PSKDemod : unknown constellation \"" + constellation_type +
                                            "\" (use bpsk, qpsk, oqpsk, 8psk or 16apsk)");

                if (parameters.count("rrc_alpha") > 0)
                    d_rrc_alpha = parameters["rrc_alpha"].get<float>();
                else
                    throw satdump_exception("RRC Alpha parameter must be present!");

                if (parameters.count("rrc_taps") > 0)
                    d_rrc_taps = parameters["rrc_taps"].get<int>();

                if (parameters.count("pll_bw") > 0)
                    d_loop_bw = parameters["pll_bw"].get<float>();
                else if (parameters.count("costas_bw") > 0)
                    d_loop_bw = parameters["costas_bw"].get<float>();
                else
                    throw satdump_exception("PLL BW parameter must be present!");

                if (parameters.count("post_costas_dc") > 0)
                    d_post_costas_dc_blocking = parameters["post_costas_dc"].get<bool>();

                if (parameters.count("has_carrier") > 0)
                    d_has_carrier = parameters["has_carrier"].get<bool>();

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

                // Table shared by the DD carrier loop, the M&M slicer and the EVM estimator.
                is_apsk16 = constellation_type == "16apsk";
                if (parameters.count("gamma") > 0)
                    d_gamma = parameters["gamma"].get<float>();
                if (parameters.count("apsk_ted") > 0)
                    d_apsk_ted = parameters["apsk_ted"].get<bool>();
                if (parameters.count("sym_pll_bw") > 0)
                    d_sym_pll_bw = parameters["sym_pll_bw"].get<float>();
                if (is_apsk16)
                    apsk16_const = std::make_unique<dsp::constellation_t>(dsp::APSK16_CCSDS, d_gamma);

                // BPSK Stuff
                is_bpsk = constellation_type == "bpsk";

                // OQPSK Stuff
                is_oqpsk = constellation_type == "oqpsk";
                if (is_oqpsk)
                {
                    MIN_SPS = 1.6;
                    MAX_SPS = 2.4;
                }

                // Window Name in the UI
                if (constellation_type == "bpsk")
                    name = "BPSK Demodulator";
                else if (constellation_type == "qpsk")
                    name = "QPSK Demodulator";
                else if (constellation_type == "oqpsk")
                    name = "OQPSK Demodulator";
                else if (constellation_type == "8psk")
                    name = "8PSK Demodulator";
                else if (constellation_type == "16apsk")
                    name = "16APSK Demodulator";

                // Show freq
                show_freq = true;

                int snr_order = 4;
                if (constellation_type == "bpsk")
                    snr_order = 2;
                else if (constellation_type == "qpsk" || constellation_type == "oqpsk")
                    snr_order = 4;
                else if (constellation_type == "8psk")
                    snr_order = 8;
                else if (constellation_type == "16apsk")
                    snr_order = 16; // only the order field; the APSK table is used below
                snr_evm = EVMSNREstimator(snr_order, 0.001f);
                if (is_apsk16)
                    snr_evm.set_apsk_points(apsk16_const->get_points(), apsk16_const->get_states());

                if (parameters.count("snr_estimator") > 0)
                    d_use_evm_snr = (parameters["snr_estimator"].get<std::string>() == "evm");

                // M2M4 assumes a constant modulus; force the EVM path.
                if (is_apsk16)
                    d_use_evm_snr = true;

                // Warn (non-fatally) about parameters this module does not understand, so silently
                // dropped operator tuning is visible.
                static const std::set<std::string> known_keys = {
                    // BaseDemodModule (module_demod_base.cpp + getParams())
                    "samplerate", "buffer_size", "symbolrate", "agc_rate", "dc_block", "freq_shift", "iq_swap",
                    "enable_doppler", "doppler_alpha", "dump_intermediate", "baseband_format", "custom_samplerate",
                    "satellite_frequency", "satellite_norad", "qth_lon", "qth_lat", "qth_alt", "start_timestamp",
                    "min_sps", "max_sps", "snr_audio_feedback",
                    // PSKDemodModule
                    "constellation", "rrc_alpha", "rrc_taps", "pll_bw", "costas_bw", "post_costas_dc", "has_carrier",
                    "clock_alpha", "clock_gain_omega", "clock_mu", "clock_gain_mu", "clock_omega_relative_limit",
                    "carrier_pll_bw", "carrier_pll_max_offset", "costas_max_offset", "pll_max_offset", "snr_estimator",
                    "gamma", "apsk_ted", "sym_pll_bw"};
                for (auto &kv : parameters.items())
                    if (known_keys.find(kv.key()) == known_keys.end())
                        logger->warn("%s", ("PSKDemod : unknown/ignored parameter \"" + kv.key() + "\"").c_str());
            }

            void PSKDemodModule::init()
            {
                BaseDemodModule::initb();

                // RRC
                rrc = std::make_shared<dsp::FIRBlock<complex_t>>(agc->output_stream, dsp::firdes::root_raised_cosine(1, final_samplerate, d_symbolrate, d_rrc_alpha, d_rrc_taps));

                if (d_has_carrier)
                {
                    if (constellation_type != "bpsk")
                        throw satdump_exception("For carrier mode, constellation must be BPSK!");

                    float d_carrier_pll_bw;
                    if (d_parameters.count("carrier_pll_bw") > 0)
                        d_carrier_pll_bw = d_parameters["carrier_pll_bw"].get<float>();
                    else
                        throw satdump_exception("Carrier PLL Bw parameter must be present!");

                    float d_carrier_pll_max_offset = 3.14;
                    if (d_parameters.count("carrier_pll_max_offset") > 0)
                        d_carrier_pll_max_offset = d_parameters["carrier_pll_max_offset"].get<float>();

                    // PLL
                    carrier_pll = std::make_shared<dsp::PLLCarrierTrackingBlock>(rrc->output_stream, d_carrier_pll_bw, d_carrier_pll_max_offset, -d_carrier_pll_max_offset);

                    // DC
                    carrier_dc = std::make_shared<dsp::CorrectIQBlock<complex_t>>(carrier_pll->output_stream);
                }

                // The DD detector slices all 4 sps samples (3 carry ISI), so the 8PSK
                // loop bandwidth makes it cycle-slip: 0.002 is the usable maximum.
                if (is_apsk16 && d_loop_bw > 0.002f)
                {
                    logger->info("16APSK: clamping PLL bandwidth %.4f -> 0.0020 (decision-directed loop); "
                                 "0.0010-0.0020 is the useful range, 0.0010 trades ~0.4 dB of offset tolerance for ~1 dB of MER",
                                 d_loop_bw);
                    d_loop_bw = 0.002f;
                }

                // PLL
                float costas_max_offset = d_has_carrier ? 0.2 : 1.0; // The offset in frequency should already be resolved on AM subcarriers
                if (d_parameters.count("costas_max_offset") > 0)
                    costas_max_offset = dsp::hz_to_rad(d_parameters["costas_max_offset"].get<float>(), final_samplerate);
                else if (d_parameters.count("pll_max_offset") > 0)
                    costas_max_offset = d_parameters["pll_max_offset"].get<float>();

                if (constellation_type == "bpsk")
                    pll = std::make_shared<dsp::CostasLoopBlock>(d_has_carrier ? carrier_dc->output_stream : rrc->output_stream, d_loop_bw, 2, costas_max_offset);
                else if (constellation_type == "qpsk" || constellation_type == "oqpsk")
                    pll = std::make_shared<dsp::CostasLoopBlock>(rrc->output_stream, d_loop_bw, 4, costas_max_offset);
                else if (constellation_type == "8psk")
                    pll = std::make_shared<dsp::CostasLoopBlock>(rrc->output_stream, d_loop_bw, 8, costas_max_offset);
                else if (constellation_type == "16apsk")
                {
                    // Order 16 = decision-directed on the APSK table; the decoder
                    // resolves the 4-fold rotation ambiguity.
                    pll = std::make_shared<dsp::CostasLoopBlock>(rrc->output_stream, d_loop_bw, 16, costas_max_offset);
                    pll->set_dd_constellation(apsk16_const.get());
                }

                if (d_post_costas_dc_blocking)
                    post_pll_dc = std::make_shared<dsp::CorrectIQBlock<complex_t>>(pll->output_stream);

                if (is_oqpsk)
                    delay = std::make_shared<dsp::DelayOneImagBlock>(d_post_costas_dc_blocking ? post_pll_dc->output_stream : pll->output_stream);

                // Clock recovery
                rec = std::make_shared<dsp::MMClockRecoveryBlock<complex_t>>(is_oqpsk ? delay->output_stream : (d_post_costas_dc_blocking ? post_pll_dc->output_stream : pll->output_stream), final_sps,
                                                                             d_clock_gain_omega, d_clock_mu, d_clock_gain_mu, d_clock_omega_relative_limit);
                // Real constellations only; OQPSK and the higher orders keep the complex-slicer TED.
                rec->bpsk_real_ted = is_bpsk;

                // Nearest-APSK slicer for the TED (the QPSK sign slice biases between rings).
                if (is_apsk16 && d_apsk_ted)
                    rec->dd_const = apsk16_const.get();

                // Optional symbol-rate phase loop after timing recovery: the 4-sps loop
                // only keeps the constellation upright, this one works on clean symbols.
                dsp::constellation_t *sym_const = nullptr;
                if (is_apsk16)
                    sym_const = apsk16_const.get();
                else if (constellation_type == "8psk")
                {
                    psk8_sym_const = std::make_unique<dsp::constellation_t>(dsp::PSK8);
                    sym_const = psk8_sym_const.get();
                }

                if (sym_const != nullptr && d_sym_pll_bw > 0)
                {
                    sym_pll = std::make_shared<dsp::CostasLoopBlock>(rec->output_stream, d_sym_pll_bw, 16, 0.05f);
                    sym_pll->set_dd_constellation(sym_const);
                    logger->info("%s: symbol-rate phase loop enabled (bw %.3f at high SNR, scaling to %.3f at low SNR, +-1.6 kHz)",
                                 constellation_type.c_str(), d_sym_pll_bw, d_sym_pll_bw * 0.2f);
                }

                sym_stream = sym_pll ? sym_pll->output_stream : rec->output_stream;
            }

            PSKDemodModule::~PSKDemodModule() { delete[] sym_buffer; }

            void PSKDemodModule::process()
            {
                if (input_data_type == DATA_FILE)
                    filesize = file_source->getFilesize();
                else
                    filesize = 0;

                if (output_data_type == DATA_FILE)
                {
                    data_out = std::ofstream(d_output_file_hint + ".soft", std::ios::binary);
                    d_output_file = d_output_file_hint + ".soft";
                }

                logger->info("Constellation : " + constellation_type);
                logger->info("Using input baseband " + d_input_file);
                logger->info("Demodulating to " + d_output_file_hint + ".soft");
                logger->info("Buffer size : %d", d_buffer_size);

                time_t lastTime = 0;

                // Start
                BaseDemodModule::start();
                rrc->start();
                if (d_has_carrier)
                {
                    carrier_pll->start();
                    carrier_dc->start();
                }
                pll->start();
                if (d_post_costas_dc_blocking)
                    post_pll_dc->start();
                if (is_oqpsk)
                    delay->start();
                rec->start();
                if (sym_pll)
                    sym_pll->start();

                int dat_size = 0;
                while (demod_should_run())
                {
                    dat_size = sym_stream->read();

                    if (dat_size <= 0)
                    {
                        sym_stream->flush();
                        continue;
                    }

                    // Push into constellation
                    constellation.pushComplex(sym_stream->readBuf, dat_size);

                    // Estimate SNR
                    snr_update(sym_stream->readBuf, dat_size);
                    snr = snr_read();

                    if (snr > peak_snr)
                        peak_snr = snr;

                    pushSNRAudioFeedback(dat_size, d_symbolrate);

                    // Optimum bandwidth follows SNR: wide slips into its own decision noise
                    // at low SNR, narrow cannot track offset. Scale it 1:5 over 17..25 dB.
                    if (sym_pll && d_sym_pll_bw > 0.0f)
                    {
                        float t = (snr - 17.0f) * 0.125f; // 17 dB -> 0, 25 dB -> 1
                        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
                        sym_pll->set_loop_bw(d_sym_pll_bw * (0.2f + 0.8f * t));
                    }

                    // Update freq
                    display_freq = dsp::rad_to_hz(pll->getFreq(), final_samplerate);

                    // Running-RMS normalization
                    constexpr float k_sym_pow_ema = 0.01f; // ~100-block time constant, smooth, AGC-drift tracking, no inter-block pumping
                    constexpr float k_sym_pow_eps = 1e-6f; // floor so a near-zero (weak) signal cannot blow up the scale

                    if (is_bpsk) // BPSK Only uses the Q branch... So don't output useless data
                    {
                        float pow_acc = 0.0f;
                        for (int i = 0; i < dat_size; i++)
                        {
                            float v = sym_stream->readBuf[i].real;
                            pow_acc += v * v;
                        }
                        pow_acc /= (float)dat_size;

                        float scale = 50.0f / std::max(std::sqrt(d_sym_pow), k_sym_pow_eps);
                        d_sym_pow += k_sym_pow_ema * (pow_acc - d_sym_pow); // update for the NEXT block
                        for (int i = 0; i < dat_size; i++)
                        {
                            sym_buffer[i] = clamp(sym_stream->readBuf[i].real * scale);
                        }
                    }
                    else // Otherwise, for all other consts, we need both
                    {
                        float pow_acc = 0.0f;
                        for (int i = 0; i < dat_size; i++)
                        {
                            float re = sym_stream->readBuf[i].real;
                            float im = sym_stream->readBuf[i].imag;
                            pow_acc += re * re + im * im;
                        }
                        pow_acc /= (float)dat_size;

                        float scale = 100.0f / std::max(std::sqrt(d_sym_pow), k_sym_pow_eps);
                        d_sym_pow += k_sym_pow_ema * (pow_acc - d_sym_pow); // update for the NEXT block
                        for (int i = 0; i < dat_size; i++)
                        {
                            sym_buffer[i * 2] = clamp(sym_stream->readBuf[i].real * scale);
                            sym_buffer[i * 2 + 1] = clamp(sym_stream->readBuf[i].imag * scale);
                        }
                    }

                    sym_stream->flush();

                    if (output_data_type == DATA_FILE)
                        data_out.write((char *)sym_buffer, is_bpsk ? dat_size : dat_size * 2);
                    else
                        output_fifo->write((uint8_t *)sym_buffer, is_bpsk ? dat_size : dat_size * 2);

                    if (input_data_type == DATA_FILE)
                        progress = file_source->getPosition();
                    if (input_data_type == DATA_FILE && time(NULL) % 10 == 0 && lastTime != time(NULL))
                    {
                        lastTime = time(NULL);
                        logger->info("Progress " + std::to_string(round(((double)progress / (double)filesize) * 1000.0) / 10.0) + "%%, SNR : " + std::to_string(snr) + "dB," +
                                     " Peak SNR: " + std::to_string(peak_snr) + "dB");
                    }
                }

                logger->info("Demodulation finished");

                if (input_data_type == DATA_FILE)
                    stop();
            }

            nlohmann::json PSKDemodModule::getModuleStats()
            {
                nlohmann::json v; // auto v = satdump::pipeline::base::FileStreamToFileStreamModule::getModuleStats();
                v["progress"] = ((double)progress / (double)filesize);
                v["snr"] = snr;
                v["peak_snr"] = peak_snr;
                v["freq"] = display_freq;
                return v;
            }

            void PSKDemodModule::stop()
            {
                // Stop
                BaseDemodModule::stop();

                rrc->stop();
                if (d_has_carrier)
                {
                    carrier_pll->stop();
                    carrier_dc->stop();
                }
                pll->stop();
                if (d_post_costas_dc_blocking)
                    post_pll_dc->stop();
                if (is_oqpsk)
                    delay->stop();
                if (sym_pll)
                {
                    sym_pll->stop();
                    sym_pll->output_stream->stopReader();
                }
                rec->stop();
                rec->output_stream->stopReader();

                if (output_data_type == DATA_FILE)
                    data_out.close();
            }

            std::string PSKDemodModule::getID() { return "psk_demod"; }

            // std::vector<std::string> PSKDemodModule::getParameters()
            // {
            //     std::vector<std::string> params = {"rrc_alpha", "rrc_taps", "pll_bw", "clock_gain_omega", "clock_mu", "clock_gain_mu", "clock_omega_relative_limit"};
            //     params.insert(params.end(), BaseDemodModule::getParameters().begin(), BaseDemodModule::getParameters().end());
            //     return params;
            // } // TODOREWORK

            std::shared_ptr<ProcessingModule> PSKDemodModule::getInstance(std::string input_file, std::string output_file_hint, nlohmann::json parameters)
            { return std::make_shared<PSKDemodModule>(input_file, output_file_hint, parameters); }
        } // namespace demod
    } // namespace pipeline
} // namespace satdump
