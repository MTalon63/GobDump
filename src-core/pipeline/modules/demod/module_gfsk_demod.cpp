#include "module_gfsk_demod.h"
#include "common/dsp/filter/firdes.h"
#include "imgui/imgui.h"
#include "logger.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <numeric>

namespace satdump
{
    namespace pipeline
    {
        namespace demod
        {
            GFSKDemodModule::GFSKDemodModule(std::string input_file, std::string output_file_hint, nlohmann::json parameters) : BaseDemodModule(input_file, output_file_hint, parameters)
            {
                // Buffers
                sym_buffer = new int8_t[d_buffer_size * 4];

                // Parse params
                if (parameters.count("fsk_deviation") > 0)
                    d_deviation = parameters["fsk_deviation"].get<float>();

                if (parameters.count("rrc_alpha") > 0)
                    d_rrc_alpha = parameters["rrc_alpha"].get<float>();

                if (parameters.count("rrc_taps") > 0)
                    d_rrc_taps = parameters["rrc_taps"].get<int>();

                if (parameters.count("lpf_transition_width") > 0)
                    d_lpf_transition_width = parameters["lpf_transition_width"].get<float>();

                if (parameters.count("zc_sps") > 0)
                    d_zc_sps = parameters["zc_sps"].get<int>();

                if (parameters.count("zc_alpha") > 0)
                    d_zc_alpha = parameters["zc_alpha"].get<float>();

                if (parameters.count("soft_scale") > 0)
                    d_soft_scale = parameters["soft_scale"].get<float>();

                if (parameters.count("agc_target") > 0)
                    d_agc_target = parameters["agc_target"].get<float>();

                if (parameters.count("agc_bias") > 0)
                    d_agc_bias = parameters["agc_bias"].get<float>();

                if (parameters.count("agc_gain") > 0)
                    d_agc_gain = parameters["agc_gain"].get<float>();

                name = "GFSK Demodulator";
                show_freq = false;
                constellation.d_hscale = (80.0 / 10.0) / 100.0;
                constellation.d_vscale = 20.0 / 100.0;
            }

            GFSKDemodModule::~GFSKDemodModule() { delete[] sym_buffer; }

            void GFSKDemodModule::init()
            {
                // false : do NOT let the base create its SmartResampler; we resample explicitly below
                BaseDemodModule::initb(false);

                if (d_symbolrate <= 0)
                    throw satdump_exception("gfsk_demod : symbolrate must be > 0!");

                const double Fs_in = (double)d_samplerate;
                const double Rs = (double)d_symbolrate;

                // RRC (on the complex baseband, before quadrature demod), at the true input rate
                rrc = std::make_shared<dsp::FIRBlock<complex_t>>(agc->output_stream, dsp::firdes::root_raised_cosine(1, Fs_in, Rs, d_rrc_alpha, d_rrc_taps));

                // Derive the working sample-rate / sps the clock recovery will run at
                d_sps = d_zc_sps > 0 ? d_zc_sps : std::max<int>(2, (int)std::lround(Fs_in / Rs));
                d_resampled_rate = Rs * (double)d_sps;
                const double Fs_rs = d_resampled_rate;

                long long a = (long long)std::llround(Fs_rs);
                long long b = (long long)std::llround(Fs_in);
                long long g = std::gcd(a, b);
                d_interp = (int)(a / g);
                d_decim = (int)(b / g);

                logger->info("gfsk_demod : Fs_in=%f Rs=%d -> Fs_rs=%f sps=%d interp=%d decim=%d", Fs_in, d_symbolrate, Fs_rs, d_sps, d_interp, d_decim);

                // Quadrature demod
                const float qg = d_deviation > 0 ? (float)(Fs_in / (2.0 * M_PI * (double)d_deviation)) : 1.0f;
                qua = std::make_shared<dsp::QuadratureDemodBlock>(rrc->output_stream, qg);

                // Optional exact rational resampler down to Fs_rs, then AGC (with bias/DC removal)
                if (d_interp != d_decim)
                {
                    resamp = std::make_shared<dsp::RationalResamplerBlock<float>>(qua->output_stream, (unsigned)d_interp, (unsigned)d_decim);
                    agc2 = std::make_shared<dsp::AGC2Block<float>>(resamp->output_stream, d_agc_target, d_agc_bias, d_agc_gain);
                }
                else
                {
                    resamp.reset();
                    agc2 = std::make_shared<dsp::AGC2Block<float>>(qua->output_stream, d_agc_target, d_agc_bias, d_agc_gain);
                }

                // LPF
                const double lpf_tw = d_lpf_transition_width > 0 ? (double)d_lpf_transition_width : std::max(1000.0, Rs / 2.0);
                lpf = std::make_shared<dsp::FIRBlock<float>>(agc2->output_stream, dsp::firdes::low_pass(1, Fs_rs, Rs / 2.0, lpf_tw));

                // Clock recovery
                rec = std::make_shared<dsp::ZeroCrossingRecoveryBlock>(lpf->output_stream, d_sps, d_zc_alpha);
            }

            void GFSKDemodModule::process()
            {
                if (input_data_type == DATA_FILE)
                    filesize = file_source->getFilesize();
                else
                    filesize = 0;

                data_out = std::ofstream(d_output_file_hint + ".soft", std::ios::binary);
                d_output_file = d_output_file_hint + ".soft";

                logger->info("Using input baseband " + d_input_file);
                logger->info("Demodulating to " + d_output_file);
                logger->info("Buffer size : " + std::to_string(d_buffer_size));

                time_t lastTime = 0;

                // Start
                BaseDemodModule::start();
                rrc->start();
                qua->start();
                if (resamp)
                    resamp->start();
                agc2->start();
                lpf->start();
                rec->start();

                int dat_size = 0;
                while (demod_should_run())
                {
                    dat_size = rec->output_stream->read();

                    if (dat_size <= 0)
                    {
                        rec->output_stream->flush();
                        continue;
                    }

                    // Into constellation (real samples : X = value, Y = Gaussian)
                    constellation.pushFloatAndGaussian(rec->output_stream->readBuf, dat_size);

                    // Estimate SNR from the current buffer
                    snr_update((complex_t *)rec->output_stream->readBuf, dat_size / 2);
                    float snr_now = snr_read();
                    if (!std::isfinite(snr_now))
                        snr_now = 0.0f;

                    // Continuous stream : no burst/packet boundary exists, so average the
                    // per-buffer estimate and only publish snr/peak_snr at a coarse interval.
                    // drawUI() reads snr/peak_snr; snr_plot is fed by BaseDemodModule.
                    d_snr_smoothed = (d_snr_smoothed == 0.0f)
                                         ? snr_now
                                         : d_snr_smoothed * (1.0f - k_snr_ema) + snr_now * k_snr_ema;

                    if (++d_snr_publish_count >= k_snr_publish_buffers)
                    {
                        d_snr_publish_count = 0;
                        snr = d_snr_smoothed;
                        if (snr > peak_snr)
                            peak_snr = snr;
                    }

                    pushSNRAudioFeedback(dat_size, d_symbolrate);

                    for (int i = 0; i < dat_size; i++)
                        sym_buffer[i] = clamp(rec->output_stream->readBuf[i] * d_soft_scale);

                    rec->output_stream->flush();

                    if (output_data_type == DATA_FILE)
                        data_out.write((char *)sym_buffer, dat_size);
                    else
                        output_fifo->write((uint8_t *)sym_buffer, dat_size);

                    if (input_data_type == DATA_FILE)
                        progress = file_source->getPosition();

                    if (time(NULL) % 10 == 0 && lastTime != time(NULL))
                    {
                        lastTime = time(NULL);
                        if (filesize > 0)
                            logger->info("Progress " + std::to_string(round(((double)progress / (double)filesize) * 1000.0) / 10.0) + "%%, SNR : " + std::to_string(snr) + "dB," +
                                         " Peak SNR: " + std::to_string(peak_snr) + "dB");
                        else
                            logger->info("SNR : " + std::to_string(snr) + "dB, Peak SNR: " + std::to_string(peak_snr) + "dB");
                    }
                }

                logger->info("Demodulation finished");

                if (input_data_type == DATA_FILE)
                    stop();
            }

            void GFSKDemodModule::stop()
            {
                // Stop
                BaseDemodModule::stop();
                rrc->stop();
                qua->stop();
                if (resamp)
                    resamp->stop();
                agc2->stop();
                lpf->stop();
                rec->stop();
                rec->output_stream->stopReader();

                if (output_data_type == DATA_FILE && data_out.is_open())
                    data_out.close();
            }

            nlohmann::json GFSKDemodModule::getModuleStats()
            {
                nlohmann::json v;
                v["progress"] = filesize > 0 ? ((double)progress / (double)filesize) : 0.0;
                return v;
            }

            nlohmann::json GFSKDemodModule::getParams()
            {
                nlohmann::json v = BaseDemodModule::getParams();
                v["fsk_deviation"] = 5000;
                v["rrc_alpha"] = 0.35;
                v["rrc_taps"] = 31;
                v["lpf_transition_width"] = 0;
                v["zc_sps"] = 0;
                v["zc_alpha"] = 0.1;
                v["soft_scale"] = 10;
                v["agc_target"] = 5.0;
                v["agc_bias"] = 0.01;
                v["agc_gain"] = 0.001;
                return v;
            }

            std::shared_ptr<ProcessingModule> GFSKDemodModule::getInstance(std::string input_file, std::string output_file_hint, nlohmann::json parameters)
            {
                return std::make_shared<GFSKDemodModule>(input_file, output_file_hint, parameters);
            }
        } // namespace demod
    } // namespace pipeline
} // namespace satdump