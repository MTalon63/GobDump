/*
F5OEO FixMe :
- RX1 and RX2 should be available only if pluto is Rev C
- CS8/CS16 selection shoudl be available only if firmware text contains tezuka
- Not tested with CLI
*/
#include "plutosdr_sdr.h"
#include "core/config.h"
#include "imgui/imgui.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cmath>
#include <cctype>
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
    start_temp_worker();
    start_thread();
}

void PlutoSDRSource::stop()
{
    stop_thread();
    // The readout shares our context, so it must be quiesced before the context is destroyed; if it
    // cannot be (wedged read) stop_temp_worker abandons the context rather than freeing it.
    stop_temp_worker();
    if (is_started && ctx)
        iio_context_destroy(ctx);
    is_started = false;

    // Everything looked up from the context (phy, dev, channels) dies with it. Leaving these
    // dangling made the temperature readout touch freed memory on the next frame after a stop,
    // and again when the device was unplugged before stopping.
    ctx = nullptr;
    phy = nullptr;
    dev = nullptr;
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

// Parse a millidegree-Celsius attribute value. Rejects empty/garbage text and implausible
// readings rather than displaying a bogus 0.0 C, since a wedged attribute read can hand back an
// empty string without reporting an error.
static bool parse_millidegrees(const char *text, float &out_celsius)
{
    if (!text || !*text)
        return false;

    char *end = nullptr;
    double v = strtod(text, &end);
    if (end == text || !std::isfinite(v))
        return false;
    if (v < -200000.0 || v > 200000.0) // -200..200 C, far outside any sane die temperature
        return false;

    out_celsius = (float)(v / 1000.0);
    return true;
}

// Reads both die temperatures. A channel that is simply absent is not a failure - firmware may not
// expose it - but a channel that exists and will not read means the link is gone.
static void read_temp_values(iio_context *ctx, iio_device *phy, bool &has_ad, float &ad,
                             bool &has_zy, float &zy, bool &failed)
{
    has_ad = has_zy = failed = false;
    ad = zy = 0;

    char buf[128];

    // AD936x: the driver publishes the die sensor as a processed channel, already in m C.
    if (phy)
    {
        iio_channel *temp_ch = iio_device_find_channel(phy, "temp0", false);
        if (temp_ch)
        {
            if (iio_channel_attr_read(temp_ch, "input", buf, sizeof(buf)) > 0 && parse_millidegrees(buf, ad))
                has_ad = true;
            else
                failed = true;
        }
    }

    // Zynq: xadc exposes raw/scale/offset, which combine to m C. Read it even if the PHY sensor
    // failed, so one broken sensor cannot hide the other.
    iio_device *xadc = iio_context_find_device(ctx, "xadc");
    if (xadc)
    {
        iio_channel *temp_ch = iio_device_find_channel(xadc, "temp0", false);
        if (temp_ch)
        {
            char raw_s[64], scale_s[64], offset_s[64];
            if (iio_channel_attr_read(temp_ch, "raw", raw_s, sizeof(raw_s)) > 0 &&
                iio_channel_attr_read(temp_ch, "scale", scale_s, sizeof(scale_s)) > 0 &&
                iio_channel_attr_read(temp_ch, "offset", offset_s, sizeof(offset_s)) > 0)
            {
                char *er = nullptr, *es = nullptr, *eo = nullptr;
                double raw = strtod(raw_s, &er), scale = strtod(scale_s, &es), offset = strtod(offset_s, &eo);
                if (er != raw_s && es != scale_s && eo != offset_s &&
                    std::isfinite(raw) && std::isfinite(scale) && std::isfinite(offset))
                {
                    float c = (float)((raw + offset) * scale / 1000.0);
                    if (c > -200.0f && c < 200.0f)
                    {
                        zy = c;
                        has_zy = true;
                    }
                    else
                        failed = true;
                }
                else
                    failed = true;
            }
            else
                failed = true;
        }
    }
}

// Board/silicon identity from the context attributes: hw_model carries board revision, Zynq part
// and AD936x variant ("Analog Devices PlutoSDR Rev.B (Z7010-AD9364)"). The parenthetical is
// cosmetic on some builds, so the PHY's own model attribute wins when present.
static void read_identity(iio_context *ctx, std::string &hw, std::string &fw,
                          std::string &ad_model, std::string &fpga, std::string &xo_correction)
{
    hw.clear();
    fw.clear();
    ad_model.clear();
    fpga.clear();
    xo_correction.clear();

    const char *h = iio_context_get_attr_value(ctx, "hw_model");
    const char *f = iio_context_get_attr_value(ctx, "fw_version");
    if (h)
        hw = h;
    if (f)
        fw = f;

    size_t lp = hw.find('('), rp = hw.find(')');
    if (lp != std::string::npos && rp != std::string::npos && rp > lp)
    {
        std::string parts = hw.substr(lp + 1, rp - lp - 1);
        size_t dash = parts.find('-');
        if (dash != std::string::npos)
        {
            fpga = parts.substr(0, dash);
            ad_model = parts.substr(dash + 1);
        }
        else
            ad_model = parts;
    }

    const char *am = iio_context_get_attr_value(ctx, "ad9361-phy,model");
    if (am && *am)
    {
        ad_model = am;
        for (char &ch : ad_model)
            ch = (char)toupper((unsigned char)ch);
    }

    const char *xo = iio_context_get_attr_value(ctx, "ad9361-phy,xo_correction");
    if (xo && *xo)
        xo_correction = xo;
}

void PlutoSDRSource::start_temp_worker()
{
    if (temp_thread.joinable())
        return;

    if (!temp_state)
        temp_state = std::make_shared<TempState>();

    // sdr_startup() runs before this on a first start, so hand the streaming handles over here as
    // well as there - otherwise the worker would have seen a null context and done nothing.
    {
        std::lock_guard<std::timed_mutex> lk(temp_state->iio_mutex);
        temp_state->ctx = ctx;
        temp_state->phy = phy;
        temp_state->identity_done = false;
    }

    temp_state->stop = false;
    temp_state->done = false;
    temp_state->enabled = false;
    temp_state->identity_done = false;

    auto st = temp_state;

    temp_thread = std::thread([st]()
    {
        while (!st->stop)
        {
            if (st->enabled)
            {
                // Timed lock: if the UI is tearing the context down we skip this tick instead of
                // blocking, and the reverse case (we hold it) is what stop() watches for.
                if (st->iio_mutex.try_lock_for(std::chrono::milliseconds(500)))
                {
                    if (st->ctx)
                    {
                        bool has_ad = false, has_zy = false, failed = false;
                        float ad = 0, zy = 0;
                        read_temp_values(st->ctx, st->phy, has_ad, ad, has_zy, zy, failed);

                        std::string hw, fw, ad_model, fpga, xo;
                        bool read_ident = !st->identity_done;
                        if (read_ident)
                            read_identity(st->ctx, hw, fw, ad_model, fpga, xo);

                        {
                            std::lock_guard<std::mutex> lk(st->mutex);
                            st->has_ad = has_ad;
                            st->ad = ad;
                            st->has_zy = has_zy;
                            st->zy = zy;
                            if (read_ident && !hw.empty())
                            {
                                st->hw_model = hw;
                                st->fw_version = fw;
                                st->ad_model = ad_model;
                                st->fpga_model = fpga;
                                st->identity_done = true;
                            }
                        }

                        if (read_ident && !hw.empty())
                        {
                            logger->info("PlutoSDR hardware: %s (fw %s)", hw.c_str(),
                                         fw.empty() ? "unknown" : fw.c_str());
                            // XO trim is a diagnostic, not something the panel needs: log only.
                            if (!xo.empty())
                                logger->info("PlutoSDR XO correction: %s", xo.c_str());
                        }

                        // Log the transition, not every tick: a source left running with the plug
                        // pulled would otherwise print this once a second forever.
                        if (failed && !st->read_error_logged)
                        {
                            logger->debug("PlutoSDR temperature read failed, link may be gone");
                            st->read_error_logged = true;
                        }
                        else if (!failed && (has_ad || has_zy))
                            st->read_error_logged = false;
                    }
                    st->iio_mutex.unlock();
                }
            }
            else
            {
                // Readout off: keep the last values out of the way, no polling at all.
                std::lock_guard<std::mutex> lk(st->mutex);
                st->has_ad = st->has_zy = false;
            }

            // Poll at 1 Hz normally, but only every 5 s once reads are failing: nothing changes on a
            // link that is gone, and USB traffic on a yanked device is pointless.
            bool backing_off = st->read_error_logged;
            std::unique_lock<std::mutex> lk(st->mutex);
            st->cv.wait_for(lk, std::chrono::seconds(backing_off ? 5 : 1), [st] { return (bool)st->stop; });
        }

        st->done = true;
    });
}

void PlutoSDRSource::stop_temp_worker()
{
    if (temp_state)
    {
        temp_state->enabled = false;
        temp_state->stop = true;
        temp_state->cv.notify_all();

        if (temp_thread.joinable())
        {
            // Give the worker a moment to leave its read; never block the UI on a wedged transport.
            for (int i = 0; i < 30 && !temp_state->done; i++)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));

            if (temp_state->done)
                temp_thread.join();
            else
                temp_thread.detach();
        }
    }

    if (!temp_state)
        return;

    // Hand the context over for destruction, or abandon it if the worker is still inside a read:
    // leaking one context is preferable to freeing it under a waiting thread.
    if (temp_state->iio_mutex.try_lock_for(std::chrono::milliseconds(200)))
    {
        temp_state->iio_mutex.unlock();
        if (temp_state->ctx)
        {
            iio_context_destroy(temp_state->ctx);
            temp_state->ctx = nullptr;
            temp_state->phy = nullptr;
        }
    }
    else
    {
        logger->warn("PlutoSDR: temperature read still in flight, abandoning its context");
        temp_state->ctx = nullptr;
        temp_state->phy = nullptr;
    }

    // The source's raw handles alias the ones above; clear them so stop() does not free twice.
    ctx = nullptr;
    phy = nullptr;
    dev = nullptr;
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

    // Board identity and die temperatures: advanced mode only, and serviced entirely by the worker
    // thread. This block only copies cached values under a lock, so it can never block on the link.
    bool advanced = false;
    if (satdump::satdump_cfg.main_cfg.contains("user_interface") &&
        satdump::satdump_cfg.main_cfg["user_interface"].contains("advanced_mode") &&
        satdump::satdump_cfg.main_cfg["user_interface"]["advanced_mode"].contains("value") &&
        satdump::satdump_cfg.main_cfg["user_interface"]["advanced_mode"]["value"].is_boolean())
        advanced = satdump::satdump_cfg.main_cfg["user_interface"]["advanced_mode"]["value"].get<bool>();

    if (temp_state)
    {
        temp_state->enabled = advanced; // worker polls only while the readout is on screen

        if (advanced)
        {
            bool has_ad = false, has_zy = false;
            float ad = 0, zy = 0;
            std::string ad_model, fpga_model;
            {
                std::lock_guard<std::mutex> lk(temp_state->mutex);
                has_ad = temp_state->has_ad;
                ad = temp_state->ad;
                has_zy = temp_state->has_zy;
                zy = temp_state->zy;
                ad_model = temp_state->ad_model;
                fpga_model = temp_state->fpga_model;
            }

            if (has_ad || has_zy)
            {
                const char *ad_name = ad_model.empty() ? "AD936x" : ad_model.c_str();
                const char *fp_name = fpga_model.empty() ? "Zynq" : fpga_model.c_str();
                char line[128] = {0};
                if (has_ad && has_zy)
                    snprintf(line, sizeof(line), "%s: %.1f\xC2\xB0" "C  %s: %.1f\xC2\xB0" "C", ad_name, ad, fp_name, zy);
                else if (has_ad)
                    snprintf(line, sizeof(line), "%s: %.1f\xC2\xB0" "C", ad_name, ad);
                else
                    snprintf(line, sizeof(line), "%s: %.1f\xC2\xB0" "C", fp_name, zy);
                ImGui::TextUnformatted(line);
            }
        }
    }
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

    // Manual-entry placeholder; not auto-selected (see SourceDescriptor::auto_select_ok).
    results.push_back({"plutosdr", "PlutoSDR IP", "0", false, false});

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

    // Bound every IIO operation on this context. Without it, a link that dies mid-stream leaves the
    // reader blocked in iio_buffer_refill() until TCP gives up - observed at ~7 s when the Pluto was
    // unplugged while running, during which the UI sits in "Waiting for the thread...". 2 s caps
    // that while staying well clear of legitimate transfers (a 1 MB buffer at 53 Msps is ~16 ms).
    iio_context_set_timeout(ctx, 2000);

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

    // Hand the streaming handles to the readout worker (advanced mode only). Pluto's USB interface
    // can only be claimed once, so the worker cannot open its own context.
    if (temp_state)
    {
        temp_state->iio_mutex.lock();
        temp_state->ctx = ctx;
        temp_state->phy = phy;
        temp_state->identity_done = false;
        temp_state->iio_mutex.unlock();
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
