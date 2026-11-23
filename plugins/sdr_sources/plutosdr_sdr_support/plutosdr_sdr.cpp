/*
F5OEO FixMe :
- RX1 and RX2 should be available only if pluto is Rev C
- CS8/CS16 selection shoudl be available only if firmware text contains tezuka
- Not tested with CLI
*/
#include "plutosdr_sdr.h"
#include "imgui/imgui.h"
#include <array>
#include <cerrno>
#include <cstring>

// Adapted from altillimity/SatDump PR #111 and SDR++

const std::array<const char *, 4> pluto_gain_mode = {"manual", "fast_attack", "slow_attack", "hybrid"};
const std::array<const char *, 2> pluto_iq_mode = {"cs16", "cs8"};
const std::array<const char *, 2> pluto_rf_input = {"rx1", "rx2"};

void PlutoSDRSource::set_rfinput()
{
    if (is_open && is_started)
    {
        uint32_t val = 0;

        iio_device_reg_read(phy, 0x00000003, &val);
        val = (val & 0x3F) | ((rf_input + 1) << 6);
        iio_device_reg_write(phy, 0x00000003, val);
        logger->trace("RFInput %d", rf_input + 1l);
        iio_device_debug_attr_write_longlong(phy, "adi,1rx-1tx-mode-use-rx-num", rf_input + 1);
        // WorkAround because ad9361 driver doesnt reflect well rx change with gain
        iio_channel_attr_write(iio_device_find_channel(phy, "voltage0", false), "gain_control_mode", "fast_attack");
        // Update gain to be coherent between 2 channels : Fixme, should be 2 separate gains
        set_gains();
    }
}

void PlutoSDRSource::set_gains()
{
    if (is_open && is_started)
    {
        iio_channel_attr_write(iio_device_find_channel(phy, "voltage0", false), "gain_control_mode", pluto_gain_mode.at(gain_mode));
        // AD936x accepts -3 through 71
        iio_channel_attr_write_longlong(iio_device_find_channel(phy, "voltage0", false), "hardwaregain", round(gain - 2));
        logger->debug("Set PlutoSDR gain to %d, mode %s", gain, pluto_gain_mode.at(gain_mode));
    }
}

void PlutoSDRSource::set_bandwidth()
{
    int set_to = manual_bandwidth ? bandwidth_widget.get_value() : samplerate_widget.get_value();
    iio_channel_attr_write_longlong(iio_device_find_channel(phy, "voltage0", false), "rf_bandwidth", set_to);

    logger->debug("Set PlutoSDR filter bandwidth to %d", set_to);
}

void PlutoSDRSource::set_settings(nlohmann::json settings)
{
    d_settings = settings;

    gain = getValueOrDefault(d_settings["gain"], gain);
    gain_mode = getValueOrDefault(d_settings["gain_mode"], gain_mode);
    ip_address = getValueOrDefault(d_settings["ip_address"], ip_address);
    port = getValueOrDefault(d_settings["port"], port);
    auto_reconnect = getValueOrDefault(d_settings["auto_reconnect"], auto_reconnect);
    manual_bandwidth = getValueOrDefault(d_settings["manual_bw"], manual_bandwidth);
    bandwidth_widget.set_value(getValueOrDefault(d_settings["manual_bw_value"], samplerate_widget.get_value()));
    iq_mode = getValueOrDefault(d_settings["iq_mode"], iq_mode);
    rf_input = getValueOrDefault(d_settings["rf_input"], rf_input);
    if (is_open && is_started)
    {
        set_rfinput();
        set_gains();
        set_bandwidth();
    }
}

nlohmann::json PlutoSDRSource::get_settings()
{
    d_settings["gain"] = gain;
    d_settings["gain_mode"] = gain_mode;
    d_settings["ip_address"] = ip_address;
    d_settings["port"] = port;
    d_settings["auto_reconnect"] = auto_reconnect;
    d_settings["manual_bw_value"] = bandwidth_widget.get_value();
    d_settings["iq_mode"] = iq_mode;
    d_settings["rf_input"] = rf_input;
    return d_settings;
}

void PlutoSDRSource::open()
{
    if (d_sdr_id != "0")
        is_usb = true;

    if (!is_open) // We do nothing there!
        is_open = true;

    // Get available samplerates
    std::vector<double> available_samplerates;
    for (int sr = 1000000; sr <= 20000000; sr += 500000)
        available_samplerates.push_back(sr);

    samplerate_widget.set_list(available_samplerates, true);

    std::vector<double> available_bandwidths;
    for (int bw = 1000000; bw <= 52000000; bw += 500000)
    {
        available_bandwidths.push_back(bw);
    }

    bandwidth_widget.set_list(available_bandwidths, true, "Hz");
}

void PlutoSDRSource::start()
{
    DSPSampleSource::start();
    sdr_startup();
    start_thread();
}

void PlutoSDRSource::stop()
{
    stop_thread();
    if (is_started)
        iio_context_destroy(ctx);
    is_started = false;
}

void PlutoSDRSource::close() {}

void PlutoSDRSource::set_frequency(uint64_t frequency)
{
    if (is_open && is_started)
    {
        iio_channel_attr_write_longlong(iio_device_find_channel(phy, "altvoltage0", true), "frequency", round(frequency));
        logger->debug("Set PlutoSDR frequency to %d", frequency);
    }
    DSPSampleSource::set_frequency(frequency);
}

void PlutoSDRSource::drawControlUI()
{
    if (is_started)
        RImGui::beginDisabled();

    samplerate_widget.render();

    if (!is_usb)
    {
        RImGui::InputText("Address", &ip_address);
        if (ImGui::InputInt("Port", &port))
            port = std::clamp(port, 1, 65535);
        RImGui::Checkbox("Auto-Reconnect", &auto_reconnect);
    }
    if (RImGui::Combo("IQ Mode", &iq_mode, "CS16\0CS8\0")) {};

    if (is_started)
        RImGui::endDisabled();

    if (RImGui::Combo("RF input", &rf_input, "rx1\0rx2\0"))
    {
        set_rfinput();
    };

    if (gain_mode == 0)
    {
        // Gain settings
        if (RImGui::SteppedSliderInt("Gain", &gain, -1, 73))
            set_gains();
    }

    if (RImGui::Combo("Gain Mode", &gain_mode, "Manual\0Fast Attack\0Slow Attack\0Hybrid\0"))
        set_gains();

    // Bandwidth Filter
    bool bw_update = RImGui::Checkbox("Manual Bandwidth", &manual_bandwidth);
    if (manual_bandwidth)
        bw_update = bw_update || bandwidth_widget.render();

    if (bw_update && is_started)
        set_bandwidth();
}

void PlutoSDRSource::set_samplerate(uint64_t samplerate)
{
    if (!samplerate_widget.set_value(samplerate, 61.44e6))
        throw satdump_exception("Unsupported samplerate : " + std::to_string(samplerate) + "!");
}

uint64_t PlutoSDRSource::get_samplerate() { return samplerate_widget.get_value(); }

std::vector<dsp::SourceDescriptor> PlutoSDRSource::getAvailableSources()
{
    std::vector<dsp::SourceDescriptor> results;

    results.push_back({"plutosdr", "PlutoSDR IP", "0", false});

#ifdef __ANDROID__
    iio_scan_context *scan_ctx = iio_create_scan_context("usb", 0);
#else
    iio_scan_context *scan_ctx = iio_create_scan_context("usb:ip", 0);
#endif

    if (scan_ctx != nullptr)
    {
        struct iio_context_info **info;
        ssize_t ret = iio_scan_context_get_info_list(scan_ctx, &info);

        if (ret > 0)
        {
            for (int contextidx = 0; contextidx < ret; contextidx++)
            {
                const char *dev_id = iio_context_info_get_uri(info[contextidx]);
                logger->trace("Context %s", dev_id);
                std::string dev_str = "PlutoSDR " + std::string(dev_id);
                results.push_back({"plutosdr", dev_str, std::string(dev_id)});
            }
            iio_context_info_list_free(info);
        }

        iio_scan_context_destroy(scan_ctx);
    }

    return results;
}

void PlutoSDRSource::sdr_startup()
{
    uint64_t current_samplerate = samplerate_widget.get_value();

    if (is_usb)
    {
        logger->trace("Using PlutoSDR Device at " + d_sdr_id);
        ctx = iio_create_context_from_uri(d_sdr_id.c_str());
    }
    else
    {
        std::string uri = ip_address;
        // Accept a full IIO URI (ip:/usb:/local:) verbatim; otherwise build ip:host[:port].
        // A non-default port is appended only when the user set one, so a bare address keeps
        // working for local Plutos while remote/LibreSDR units can use a custom iiod port.
        if (uri.rfind("ip:", 0) != 0 && uri.rfind("usb:", 0) != 0 && uri.rfind("local:", 0) != 0)
        {
            if (uri.find(':') == std::string::npos && port > 0 && port != 30431)
                uri = "ip:" + uri + ":" + std::to_string(port);
            else
                uri = "ip:" + uri;
        }
        logger->trace("Using PlutoSDR URI " + uri);
        ctx = iio_create_context_from_uri(uri.c_str());
    }

    if (ctx == NULL)
        throw satdump_exception("Could not open PlutoSDR device! (" + std::string(strerror(errno)) + ")");
    phy = iio_context_find_device(ctx, "ad9361-phy");
    if (phy == NULL)
    {
        iio_context_destroy(ctx);
        throw satdump_exception("Could not connect to PlutoSDR PHY!");
    }
    dev = iio_context_find_device(ctx, "cf-ad9361-lpc");
    if (dev == NULL)
    {
        iio_context_destroy(ctx);
        throw satdump_exception("Could not connect to PlutoSDR device!");
    }

    iio_channel_attr_write_bool(iio_device_find_channel(phy, "altvoltage1", true), "powerdown", true);
    iio_channel_attr_write_bool(iio_device_find_channel(phy, "altvoltage0", true), "powerdown", false);
    iio_channel_attr_write(iio_device_find_channel(phy, "voltage0", false), "rf_port_select", "A_BALANCED");

    logger->debug("Set PlutoSDR samplerate to " + std::to_string(current_samplerate));
    iio_channel_attr_write_longlong(iio_device_find_channel(phy, "voltage0", false), "sampling_frequency", round(current_samplerate));
    ad9361_set_bb_rate(phy, current_samplerate);

    is_started = true;

    set_frequency(d_frequency);
    set_rfinput();
    set_gains();
    set_bandwidth();
}
