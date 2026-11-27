#include "usrp_sdr.h"
#include "init.h"
#include "core/config.h"

void USRPSource::set_gains()
{
    if (!is_started || device_lost)
        return;

    try
    {
        usrp_device->set_rx_gain(gain, channel);
        logger->debug("Set USRP gain to %f", gain);
    }
    catch (const std::exception &e)
    {
        if (!thread_should_run)
            throw;
        device_lost = true;
        logger->error("USRP gain change failed: %s", e.what());
    }
}

void USRPSource::open_sdr()
{
    uhd::device_addrs_t devlist;
    try
    {
        devlist = uhd::device::find(uhd::device_addr_t());
    }
    catch (const std::exception &e)
    {
        throw satdump_exception("Could not enumerate USRP devices: " + std::string(e.what()));
    }

    int device_id = 0;
    try
    {
        device_id = std::stoi(d_sdr_id);
    }
    catch (const std::exception &)
    {
        throw satdump_exception("Invalid USRP device index!");
    }

    if (device_id < 0 || device_id >= (int)devlist.size())
        throw satdump_exception("USRP device not found!");

    uhd::device_addr_t addr = devlist[device_id];

    // Omit custom transport frame settings when UHD defaults are enabled, so UHD auto-selects per-interface settings

    if (satdump::db->get_user("usrp_sdr_support/use_uhd_defaults", "") == "0")
    {
        addr["recv_frame_size"] = satdump::db->get_user("usrp_sdr_support/recv_frame_size", "");
        addr["num_recv_frames"] = satdump::db->get_user("usrp_sdr_support/num_recv_frames", "");
        addr["send_frame_size"] = satdump::db->get_user("usrp_sdr_support/send_frame_size", "");
        addr["num_send_frames"] = satdump::db->get_user("usrp_sdr_support/num_send_frames", "");
    }

    usrp_device = uhd::usrp::multi_usrp::make(addr);

    // uhd::meta_range_t master_clock_range = usrp_device->get_master_clock_rate_range();
    // usrp_device->set_master_clock_rate(master_clock_range.stop());

    uhd::usrp::subdev_spec_t sub_boards = usrp_device->get_rx_subdev_spec();
    channel_option_str = "";
    for (int i = 0; i < (int)sub_boards.size(); i++)
    {
        logger->trace("USRP has " + usrp_device->get_rx_subdev_name(i) + " in slot " + sub_boards[i].db_name);
        channel_option_str += usrp_device->get_rx_subdev_name(i) + " (" + sub_boards[i].db_name + ")" + '\0';
    }
}

void USRPSource::open_channel()
{
    if (channel >= (int)usrp_device->get_rx_num_channels())
        throw satdump_exception("Channel " + std::to_string(channel) + " is invalid!");

    logger->info("Using USRP channel %d", channel);

    if (usrp_device->get_master_clock_rate_range().start() != usrp_device->get_master_clock_rate_range().stop())
        use_device_rates = true;

    std::vector<double> available_samplerates;

    // Preserve current samplerate selection before updating the list
    double current_samplerate = samplerate_widget.get_value();

    if (use_device_rates)
    {
        // Get samplerates
        uhd::meta_range_t dev_samplerates = usrp_device->get_master_clock_rate_range();
        available_samplerates.clear();
        for (auto &sr : dev_samplerates)
        {
            if (sr.step() == 0 && sr.start() == sr.stop())
            {
                available_samplerates.push_back(sr.start());
            }
            else if (sr.step() == 0)
            {
                for (double s = std::max(sr.start(), 1e6); s < sr.stop(); s += 1e6)
                    available_samplerates.push_back(s);
                available_samplerates.push_back(sr.stop());
            }
            else
            {
                for (double s = sr.start(); s <= sr.stop(); s += sr.step())
                    available_samplerates.push_back(s);
            }
        }
    }
    else
    {
        // Get samplerates
        uhd::meta_range_t dev_samplerates = usrp_device->get_rx_rates(channel);
        available_samplerates.clear();
        for (auto &sr : dev_samplerates)
        {
            if (sr.step() == 0 || sr.start() == sr.stop())
            {
                available_samplerates.push_back(sr.start());
            }
            else
            {
                for (double s = sr.start(); s <= sr.stop(); s += sr.step())
                    available_samplerates.push_back(s);
            }
        }
    }

    samplerate_widget.set_list(available_samplerates, true);

    // Restore the previously selected samplerate if it's still available
    samplerate_widget.set_value(current_samplerate, 0);

    // Get gain range
    gain_range = usrp_device->get_rx_gain_range(channel);

    usrp_antennas = usrp_device->get_rx_antennas(channel);
    antenna_option_str = "";
    for (int i = 0; i < (int)usrp_antennas.size(); i++)
    {
        antenna_option_str += usrp_antennas[i] + '\0';
        logger->trace("USRP has antenna option %s", usrp_antennas[i].c_str());
    }
}

void USRPSource::set_settings(nlohmann::json settings)
{
    d_settings = settings;

    channel = getValueOrDefault(d_settings["channel"], channel);
    antenna = getValueOrDefault(d_settings["antenna"], antenna);
    gain = getValueOrDefault(d_settings["gain"], gain);
    bit_depth = getValueOrDefault(d_settings["bit_depth"], bit_depth);
    clock_source = getValueOrDefault(d_settings["clock_source"], clock_source);
    if (clock_source == "internal")
        selected_clock_source = 0;
    else if (clock_source == "external")
        selected_clock_source = 1;
    else if (clock_source == "gpsdo")
        selected_clock_source = 2;

    // Combo order is 8 / 12 / 16, so the restore mapping must match it exactly. It previously
    // mapped 16 to slot 1 and had no case for 12, so a saved 12-bit setting came back displayed as
    // 16-bit, and a saved 16-bit one as 12-bit.
    if (bit_depth == 8)
        selected_bit_depth = 0;
    else if (bit_depth == 12)
        selected_bit_depth = 1;
    else
        selected_bit_depth = 2;

    if (is_started)
    {
        set_gains();
    }
}

nlohmann::json USRPSource::get_settings()
{

    d_settings["channel"] = channel;
    d_settings["antenna"] = antenna;
    d_settings["gain"] = gain;
    d_settings["bit_depth"] = bit_depth;
    d_settings["clock_source"] = clock_source;

    return d_settings;
}

void USRPSource::open()
{
    open_sdr();
    is_open = true;
    device_lost = false;
    open_channel();
    usrp_device.reset();
}

void USRPSource::start_status_worker(uhd::usrp::multi_usrp::sptr dev, int chan)
{
    if (status_thread.joinable())
        return;

    if (!status_state)
        status_state = std::make_shared<StatusState>();

    status_state->stop = false;
    status_state->done = false;
    status_state->enabled = false;
    status_state->logged_once = false;

    auto st = status_state;

    // The lambda holds its own sptr, so the device object stays alive for as long as the worker
    // touches it - even if stop() resets the source's handle.
    status_thread = std::thread([st, dev, chan]()
    {
        while (!st->stop)
        {
            if (st->enabled)
            {
                std::string board;
                bool has_temp = false, has_rssi = false;
                float temp_c = 0, rssi_db = 0;
                std::string temp_unit, rssi_unit, err, available;

                try
                {
                    board = dev->get_mboard_name();

                    // Both readings come from the channel actually streaming, not always channel 0.
                    for (const std::string &name : dev->get_rx_sensor_names(chan))
                    {
                        if (!available.empty())
                            available += ",";
                        available += name;

                        if (name == "temp")
                        {
                            auto v = dev->get_rx_sensor("temp", chan);
                            temp_c = std::stof(v.value);
                            temp_unit = v.unit;
                            has_temp = true;
                        }
                        else if (name == "rssi")
                        {
                            auto v = dev->get_rx_sensor("rssi", chan);
                            rssi_db = std::stof(v.value);
                            rssi_unit = v.unit;
                            has_rssi = true;
                        }
                    }
                }
                catch (const std::exception &e)
                {
                    err = e.what();
                }

                {
                    std::lock_guard<std::mutex> lk(st->mutex);
                    st->board = board;
                    st->has_temp = has_temp;
                    st->temp_c = temp_c;
                    st->has_rssi = has_rssi;
                    st->rssi_db = rssi_db;
                    st->temp_unit = temp_unit;
                    st->rssi_unit = rssi_unit;
                    st->error = err;
                }

                // One line per start recording what the board advertises and what we took from it, so
                // "nothing appears in the panel" can be told apart from "this board has no sensors".
                // 1 Hz value changes belong on screen, not in the log.
                if (err.empty() && !st->logged_once)
                {
                    if (has_temp || has_rssi)
                        logger->debug("USRP sensors (chan %d): using%s%s of [%s]", chan,
                                      has_temp ? (" temp=" + std::to_string(temp_c) + " " + (temp_unit.empty() ? "C" : temp_unit)).c_str() : "",
                                      has_rssi ? (" rssi=" + std::to_string(rssi_db) + " " + (rssi_unit.empty() ? "dB" : rssi_unit)).c_str() : "",
                                      available.c_str());
                    else
                        logger->debug("USRP sensors (chan %d): board reports no temperature or RSSI sensor ([%s])",
                                      chan, available.empty() ? "none" : available.c_str());
                    st->logged_once = true;
                }

                if (!err.empty() && !st->error_logged)
                {
                    logger->debug("USRP status read failed: %s", err.c_str());
                    st->error_logged = true;
                }
                else if (err.empty())
                    st->error_logged = false;
            }
            else
            {
                std::lock_guard<std::mutex> lk(st->mutex);
                st->has_temp = st->has_rssi = false;
            }

            // 1 Hz normally, 5 s once the device stops answering.
            std::unique_lock<std::mutex> lk(st->mutex);
            st->cv.wait_for(lk, std::chrono::seconds(st->error_logged ? 5 : 1), [st] { return (bool)st->stop; });
        }

        st->done = true;
    });
}

void USRPSource::stop_status_worker()
{
    if (!status_state)
        return;

    status_state->enabled = false;
    status_state->stop = true;
    status_state->cv.notify_all();

    if (status_thread.joinable())
    {
        // Never block the UI on a device that has stopped answering; the worker keeps the device
        // alive through its own reference, so detaching is safe.
        for (int i = 0; i < 30 && !status_state->done; i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

        if (status_state->done)
            status_thread.join();
        else
            status_thread.detach();
    }
}

void USRPSource::start()
{
    DSPSampleSource::start();
    thread_should_run = false;
    device_lost = false;

    usrp_streamer.reset();
    usrp_device.reset();

    open_sdr();
    open_channel();
    // Reference clock selection before starting a stream
    usrp_device->set_clock_source(clock_source);
    usrp_device->set_time_source(clock_source);

    uint64_t current_samplerate = samplerate_widget.get_value();

    logger->debug("Set USRP samplerate to " + std::to_string(current_samplerate));
    if (use_device_rates)
        usrp_device->set_master_clock_rate(current_samplerate);
    usrp_device->set_rx_rate(current_samplerate, channel);
    usrp_device->set_rx_bandwidth(current_samplerate, channel);

    if (antenna >= (int)usrp_device->get_rx_antennas(channel).size())
        throw satdump_exception("Antenna " + std::to_string(antenna) + " is invalid!");

    usrp_device->set_rx_antenna(usrp_antennas[antenna], channel);

    is_started = true;

    set_frequency(d_frequency);

    set_gains();

    uhd::stream_args_t sargs;
    sargs.channels.clear();
    sargs.channels.push_back(channel);
    sargs.cpu_format = "fc32";
    if (bit_depth == 8)
        sargs.otw_format = "sc8";
    // Temporary sc12 for testing purposes, will get a compatibility check soon
    else if (bit_depth == 12)
        sargs.otw_format = "sc12";
    else if (bit_depth == 16)
        sargs.otw_format = "sc16";

    logger->debug("USRP bit depth %d, wire format %s", bit_depth,
                  sargs.otw_format.empty() ? "default" : sargs.otw_format.c_str());

    usrp_streamer = usrp_device->get_rx_stream(sargs);
    if (!usrp_streamer)
        throw satdump_exception("Could not create USRP RX stream!");

    usrp_streamer->issue_stream_cmd(uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);

    start_status_worker(usrp_device, channel);

    thread_should_run = true;
    work_thread = std::thread(&USRPSource::mainThread, this);
}

void USRPSource::stop()
{
    stop_status_worker(); // released before the device handle goes away

    thread_should_run = false;
    logger->info("Waiting for the thread...");
    if (output_stream)
        output_stream->stopWriter();
    if (work_thread.joinable())
        work_thread.join();
    logger->info("Thread stopped");

    if (!device_lost && usrp_streamer)
    {
        try
        {
            usrp_streamer->issue_stream_cmd(uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS);
        }
        catch (const std::exception &e)
        {
            logger->error("Could not stop USRP stream: %s", e.what());
            device_lost = true;
        }
    }

    usrp_streamer.reset();
    usrp_device.reset();

    if (device_lost)
        logger->error("USRP device disconnected. Reconnect it and press Start again.");

    is_started = false;
}

void USRPSource::close() { is_open = false; }

void USRPSource::set_frequency(uint64_t frequency)
{
    if (is_started && !device_lost)
    {
        try
        {
            usrp_device->set_rx_freq(frequency, channel);
            logger->debug("Set USRP frequency to %d", frequency);
        }
        catch (const std::exception &e)
        {
            if (!thread_should_run)
                throw;
            device_lost = true;
            logger->error("USRP frequency change failed: %s", e.what());
        }
    }
    DSPSampleSource::set_frequency(frequency);
}

void USRPSource::drawControlUI()
{
    if (is_started)
        RImGui::beginDisabled();

    if (RImGui::Combo("Channel", &channel, channel_option_str.c_str()) && is_open)
    {
        try
        {
            open_sdr();
            open_channel();
        }
        catch (const std::exception &e)
        {
            logger->error("Could not switch USRP channel: %s", e.what());
        }
        usrp_streamer.reset();
        usrp_device.reset();
    }

    RImGui::Combo("Antenna", &antenna, antenna_option_str.c_str());

    samplerate_widget.render();

    if (RImGui::Combo("Bit depth", &selected_bit_depth,
                      "8-bits\0"
                      "12-bits\0"
                      "16-bits\0"))
    {
        if (selected_bit_depth == 0)
            bit_depth = 8;
        else if (selected_bit_depth == 1)
            bit_depth = 12;
        else if (selected_bit_depth == 2)
            bit_depth = 16;
    }
    // Reference clock source selection
    if (RImGui::Combo("Clock Source", &selected_clock_source,
                      "Internal\0"
                      "External\0"
                      "GPSDO\0"))
    {
        if (selected_clock_source == 0)
            clock_source = "internal";
        if (selected_clock_source == 1)
            clock_source = "external";
        if (selected_clock_source == 2)
            clock_source = "gpsdo";

        // Apply immediately: previously this was only pushed in start(), so changing the source
        // while running did nothing until the stream was restarted, which made the ref_locked
        // readout look stuck.
        if (is_started && usrp_device)
        {
            try
            {
                usrp_device->set_clock_source(clock_source);
                usrp_device->set_time_source(clock_source);
                logger->info("USRP clock/time source set to %s", clock_source.c_str());
            }
            catch (const std::exception &e)
            {
                logger->error("Could not set USRP clock source to %s: %s", clock_source.c_str(), e.what());
            }
        }
    }

    if (is_started)
        RImGui::endDisabled();

    // Gain settings
    if (is_open)
    {
        if (RImGui::SteppedSliderFloat("Gain", &gain, gain_range.start(), gain_range.stop()))
            set_gains();
    }
    else // Defaults for using UI when device is not attached (config export)
        RImGui::SteppedSliderFloat("Gain", &gain, 0, 60);

    // RFIC die temperature and RSSI: advanced mode only, for both polling and display. Values come
    // from the RX channel in use and are only copied under a lock here, so the UI never waits on
    // the device.
    bool advanced = false;
    if (satdump::satdump_cfg.main_cfg.contains("user_interface") &&
        satdump::satdump_cfg.main_cfg["user_interface"].contains("advanced_mode") &&
        satdump::satdump_cfg.main_cfg["user_interface"]["advanced_mode"].contains("value") &&
        satdump::satdump_cfg.main_cfg["user_interface"]["advanced_mode"]["value"].is_boolean())
        advanced = satdump::satdump_cfg.main_cfg["user_interface"]["advanced_mode"]["value"].get<bool>();

    if (status_state)
    {
        status_state->enabled = advanced && is_started;

        if (advanced && is_started)
        {
            bool has_temp = false, has_rssi = false;
            float temp_c = 0, rssi_db = 0;
            std::string board, temp_unit, rssi_unit;
            {
                std::lock_guard<std::mutex> lk(status_state->mutex);
                has_temp = status_state->has_temp;
                temp_c = status_state->temp_c;
                has_rssi = status_state->has_rssi;
                rssi_db = status_state->rssi_db;
                temp_unit = status_state->temp_unit;
                rssi_unit = status_state->rssi_unit;
                board = status_state->board;
            }

            if (has_temp || has_rssi)
            {
                // B2xx frontends report the AD936x die sensor; other boards differ, so fall back to
                // a generic label rather than claiming a part number we have not verified.
                const char *chip = "RFIC";
                if (board == "B210")
                    chip = "AD9361";
                else if (board == "B200" || board == "B205mini")
                    chip = "AD9364";

                // Units come from UHD rather than being assumed: "dB" for RSSI (relative, not dBm),
                // and "C" for the die sensor, which is rendered with the degree glyph.
                const char *tu = (temp_unit.empty() || temp_unit == "C") ? "\xC2\xB0" "C" : temp_unit.c_str();
                const char *ru = rssi_unit.empty() ? "dB" : rssi_unit.c_str();

                char line[160] = {0};
                int n = 0;
                if (has_temp)
                    n += snprintf(line + n, sizeof(line) - n, "%s: %.1f%s", chip, temp_c, tu);
                if (has_rssi)
                    n += snprintf(line + n, sizeof(line) - n, "%sRSSI: %.1f %s", n ? "  " : "", rssi_db, ru);
                ImGui::TextUnformatted(line);
            }
        }
    }
}

void USRPSource::set_samplerate(uint64_t samplerate)
{
    if (!samplerate_widget.set_value(samplerate, 0))
        throw satdump_exception("Unsupported samplerate : " + std::to_string(samplerate) + "!");
}

uint64_t USRPSource::get_samplerate() { return samplerate_widget.get_value(); }

std::vector<dsp::SourceDescriptor> USRPSource::getAvailableSources()
{
    std::vector<dsp::SourceDescriptor> results;

    uhd::device_addrs_t devlist;
    try
    {
        devlist = uhd::device::find(uhd::device_addr_t());
    }
    catch (const std::exception &e)
    {
        logger->error("Could not enumerate USRP devices: %s", e.what());
        return results;
    }

    uint64_t i = 0;
    for (const uhd::device_addr_t &dev : devlist)
    {
        std::string type = dev.has_key("product") ? dev["product"] : dev["type"];
        std::string serial = dev.has_key("serial") ? dev["serial"] : "unknown";
        results.push_back({"usrp", "USRP " + type + " " + serial, std::to_string(i)});
        i++;
    }

    return results;
}
