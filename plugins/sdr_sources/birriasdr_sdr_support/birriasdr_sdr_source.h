#pragma once

#include "common/dsp_source_sink/dsp_sample_source.h"
#include "birriasdr.h"
#include "logger.h"
#include "common/rimgui.h"
#include "common/widgets/double_list.h"
#include <atomic>
#include <chrono>
#include <thread>

class BirriaSDRSource : public dsp::DSPSampleSource
{
protected:
    bool is_open = false, is_started = false;
    birriasdr_dev_t *sdr_dev = nullptr;
    std::thread worker;
    std::atomic<bool> stopping{false};
    uint32_t device_index = 0;

    satdump::widgets::DoubleList samplerate_widget;

    static void _read_callback(const int16_t *iq, uint32_t count, uint64_t first_sample, void *context);
    static void _recovery_callback(const struct birriasdr_recovery_event *event, void *context);

    bool manual_gain = true;
    // ImGui writes a 4-byte int through the pointer, so these cannot be uint8_t.
    int lna = 5, lpf = 8, digital = 6;
    int applied_lna = -1, applied_lpf = -1, applied_digital = -1;
    int applied_manual = -1;

    // Zero-IF LO leakage sits on the tuned centre; a leaky-mean high-pass removes it.
    bool dc_block = true;
    double dc_alpha = 0.0, dc_i = 0.0, dc_q = 0.0;

    void apply_gains(bool force = false);
    bool in_tuning_range(uint64_t frequency);
    uint64_t tuning_distance(uint64_t frequency);

    // Live control writes are expensive at 80 MSps and BUSY during a restart, so coalesce them.
    std::chrono::steady_clock::time_point last_gain_write;
    std::chrono::steady_clock::time_point last_tune_write;
    uint64_t last_rejected = 0;

public:
    BirriaSDRSource(dsp::SourceDescriptor source) : DSPSampleSource(source), samplerate_widget("Samplerate") {}

    ~BirriaSDRSource()
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

    static std::string getID() { return "birriasdr"; }
    static std::shared_ptr<dsp::DSPSampleSource> getInstance(dsp::SourceDescriptor source) { return std::make_shared<BirriaSDRSource>(source); }
    static std::vector<dsp::SourceDescriptor> getAvailableSources();
};
