#pragma once

#include "common/dsp_source_sink/dsp_sample_source.h"
#include "common/rimgui.h"
#include "common/widgets/double_list.h"
#include "logger.h"
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <uhd.h>
#include <uhd/device.hpp>
#include <uhd/usrp/multi_usrp.hpp>

class USRPSource : public dsp::DSPSampleSource
{
protected:
    bool is_open = false, is_started = false;

    uhd::usrp::multi_usrp::sptr usrp_device;
    uhd::rx_streamer::sptr usrp_streamer;

    // Board status: RFIC die temperature and RSSI, read from the RX channel actually in use and
    // polled by a worker thread that keeps its own reference to the device, so it can never outlive
    // the streaming path. Advanced mode only - nothing is polled while the readout is off screen.
    // Sensors are discovered by name, since which ones exist is a board property rather than
    // something we can assume.
    struct StatusState
    {
        std::mutex mutex;
        std::string board;
        bool has_temp = false, has_rssi = false;
        float temp_c = 0, rssi_db = 0;
        std::string temp_unit, rssi_unit; // as reported by UHD, shown verbatim
        std::string error;
        bool error_logged = false;
        bool logged_once = false;
        std::atomic<bool> enabled{false}, stop{false}, done{false};
        std::condition_variable cv;
    };
    std::shared_ptr<StatusState> status_state;
    std::thread status_thread;
    void start_status_worker(uhd::usrp::multi_usrp::sptr dev, int chan);
    void stop_status_worker();
    uhd::gain_range_t gain_range;

    bool use_device_rates = false;

    satdump::widgets::DoubleList samplerate_widget;

    std::string channel_option_str;
    std::vector<std::string> usrp_antennas;
    std::string antenna_option_str;
    int selected_bit_depth = 2;
    int selected_clock_source = 0;
    std::string clock_source = "internal";

    int channel = 0;
    int antenna = 0;
    float gain = 0;
    int bit_depth = 16;

    void set_gains();

    void open_sdr();
    void open_channel();

    std::thread work_thread;
    std::atomic<bool> thread_should_run{false};
    std::atomic<bool> device_lost{false};
    void mainThread()
    {
        uhd::rx_metadata_t meta;

        int buffer_size = std::min<int>(samplerate_widget.get_value() / 250, dsp::STREAM_BUFFER_SIZE);
        logger->trace("USRP Buffer size %d", buffer_size);

        while (thread_should_run)
        {
            try
            {
                int cnt = usrp_streamer->recv(output_stream->writeBuf, buffer_size, meta, 1.0);
                if (cnt > 0)
                    output_stream->swap(cnt);
                if (meta.error_code != meta.ERROR_CODE_NONE && meta.error_code != meta.ERROR_CODE_TIMEOUT)
                    logger->error("USRP Stream error %s", meta.strerror().c_str());
            }
            catch (const std::exception &e)
            {
                logger->error("USRP stream stopped: %s", e.what());
                device_lost = true;
                break;
            }
        }

        if (output_stream)
            output_stream->stopWriter();
    }

public:
    USRPSource(dsp::SourceDescriptor source) : DSPSampleSource(source), samplerate_widget("Samplerate") {}

    ~USRPSource()
    {
        stop();
        close();
    }

    void set_settings(nlohmann::json settings);
    nlohmann::json get_settings();

    void open();
    void start();
    void stop();
    void close();

    void set_frequency(uint64_t frequency);

    void drawControlUI();

    void set_samplerate(uint64_t samplerate);
    uint64_t get_samplerate();

    static std::string getID() { return "usrp"; }
    static std::shared_ptr<dsp::DSPSampleSource> getInstance(dsp::SourceDescriptor source) { return std::make_shared<USRPSource>(source); }
    static std::vector<dsp::SourceDescriptor> getAvailableSources();
};
