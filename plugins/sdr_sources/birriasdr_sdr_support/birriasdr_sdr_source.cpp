#include "birriasdr_sdr_source.h"

// 14-bit codes in a 32-bit word, scaled to the DSP chain's unit range.
void BirriaSDRSource::_read_callback(const int16_t *iq, uint32_t count, uint64_t, void *context)
{
    BirriaSDRSource *self = (BirriaSDRSource *)context;
    std::shared_ptr<dsp::stream<complex_t>> stream = self->output_stream;
    if (!stream)
        return;

    for (uint32_t done = 0; done < count;)
    {
        uint32_t n = std::min<uint32_t>(count - done, (uint32_t)dsp::STREAM_BUFFER_SIZE);
        for (uint32_t i = 0; i < n; i++)
        {
            float re = iq[(done + i) * 2 + 0] / 8192.0f;
            float im = iq[(done + i) * 2 + 1] / 8192.0f;
            if (self->dc_block)
            {
                self->dc_i += (re - self->dc_i) * self->dc_alpha;
                self->dc_q += (im - self->dc_q) * self->dc_alpha;
                re -= (float)self->dc_i;
                im -= (float)self->dc_q;
            }
            stream->writeBuf[i].real = re;
            stream->writeBuf[i].imag = im;
        }
        if (!stream->swap(n))
            return;
        done += n;
    }
}

void BirriaSDRSource::_recovery_callback(const struct birriasdr_recovery_event *event, void *)
{
    logger->warn("BirriaSDR gap #%u at %llu samples: %s", event->attempt,
                 (unsigned long long)event->next_sample, birriasdr_strerror(event->error));
}

bool BirriaSDRSource::in_tuning_range(uint64_t frequency)
{
    return tuning_distance(frequency) == 0;
}

// Distance to the nearest covered band, zero when inside one.
uint64_t BirriaSDRSource::tuning_distance(uint64_t frequency)
{
    uint64_t minimum = 0, maximum = 0, nearest = UINT64_MAX;
    for (uint32_t band = 0; band < 4; band++)
    {
        if (birriasdr_get_tuning_band(sdr_dev, band, &minimum, &maximum) < 0)
            break;
        if (frequency >= minimum && frequency <= maximum)
            return 0;
        uint64_t distance = frequency < minimum ? minimum - frequency : frequency - maximum;
        if (distance < nearest)
            nearest = distance;
    }
    return nearest;
}

void BirriaSDRSource::apply_gains(bool force)
{
    if (!is_started)
        return;

    int mode = manual_gain ? 1 : 0;

    // GobDump re-applies settings repeatedly; skip writes that would not change the hardware.
    if (!force && mode == applied_manual &&
        (!manual_gain || (lna == applied_lna && lpf == applied_lpf && digital == applied_digital)))
        return;

    // Live writes cost milliseconds at 80 MSps; coalesce slider drags instead of one write per frame.
    auto now = std::chrono::steady_clock::now();
    if (!force && now - last_gain_write < std::chrono::milliseconds(200))
        return;
    last_gain_write = now;

    int err = birriasdr_set_tuner_gain_mode(sdr_dev, mode);
    if (err == BIRRIASDR_ERROR_BUSY)
        return;
    if (err < 0)
    {
        logger->error("BirriaSDR gain mode failed: %s", birriasdr_strerror(err));
        return;
    }

    if (manual_gain)
    {
        struct birriasdr_gain gain = {(uint8_t)lna, (uint8_t)lpf, (uint8_t)digital};
        err = birriasdr_set_gain_codes(sdr_dev, &gain);
        if (err == BIRRIASDR_ERROR_BUSY)
            return;
        if (err < 0)
        {
            logger->error("BirriaSDR gain codes failed: %s", birriasdr_strerror(err));
            return;
        }
        logger->debug("Set BirriaSDR gain LNA=%d LPF=%d DIG=%d", lna, lpf, digital);
        applied_lna = lna;
        applied_lpf = lpf;
        applied_digital = digital;
    }

    applied_manual = mode;
}

void BirriaSDRSource::set_settings(nlohmann::json settings)
{
    d_settings = settings;

    manual_gain = getValueOrDefault(d_settings["manual_gain"], manual_gain);
    lna = getValueOrDefault(d_settings["lna"], lna);
    lpf = getValueOrDefault(d_settings["lpf"], lpf);
    digital = getValueOrDefault(d_settings["digital"], digital);
    dc_block = getValueOrDefault(d_settings["dc_block"], dc_block);

    if (is_open)
        apply_gains();
}

nlohmann::json BirriaSDRSource::get_settings()
{
    d_settings["manual_gain"] = manual_gain;
    d_settings["lna"] = lna;
    d_settings["lpf"] = lpf;
    d_settings["digital"] = digital;
    d_settings["dc_block"] = dc_block;
    return d_settings;
}

void BirriaSDRSource::open()
{
    is_open = true;

    // The ICAP capture path offers exactly these four rates.
    std::vector<double> available_samplerates = {1e7, 2e7, 4e7, 8e7};
    samplerate_widget.set_list(available_samplerates, false);
}

void BirriaSDRSource::start()
{
    DSPSampleSource::start();

    device_index = 0;
    try
    {
        device_index = (uint32_t)std::stoul(d_sdr_id);
    }
    catch (...)
    {
        logger->warn("BirriaSDR: unexpected device id '%s', using index 0", d_sdr_id.c_str());
    }

    int err = birriasdr_open(&sdr_dev, device_index);
    if (err < 0)
        throw satdump_exception("Could not open BirriaSDR device: " + std::string(birriasdr_strerror(err)));

    uint32_t rate = (uint32_t)samplerate_widget.get_value();
    err = birriasdr_set_sample_rate(sdr_dev, rate);
    if (err < 0)
        throw satdump_exception("BirriaSDR samplerate rejected: " + std::string(birriasdr_strerror(err)));

    is_started = true;
    applied_manual = -1;

    // Measured on this host: the libusbK bulk path sustains ~160 MB/s, which is exactly 40 MSps.
    if (rate >= 80000000)
        logger->warn("BirriaSDR: %u MSps needs 320 MB/s, this USB path tops out near 160 MB/s - expect overruns", rate / 1000000);

    // ~0.1 s leaky-mean time constant: kills the LO spur, leaves everything above a few Hz.
    dc_alpha = rate ? 1.0 / ((double)rate * 0.1) : 0.0;
    dc_i = dc_q = 0.0;

    set_frequency(d_frequency);
    apply_gains(true);

    // Keep the stream alive across an overrun instead of ending the capture.
    if (birriasdr_set_recovery(sdr_dev, 4, &BirriaSDRSource::_recovery_callback, this) < 0)
        logger->warn("BirriaSDR: automatic recovery unavailable");

    // read_async blocks until cancel, so the capture runs on its own thread.
    stopping = false;
    worker = std::thread([this]()
                         {
        int result = birriasdr_read_async(sdr_dev, &BirriaSDRSource::_read_callback, this, 0, 0);
        if (result < 0 && !stopping)
            logger->error("BirriaSDR stream ended: %s", birriasdr_strerror(result)); });
}

void BirriaSDRSource::stop()
{
    if (is_started)
    {
        // swap() parks the callback when the chain stops reading, so release it before joining.
        stopping = true;
        if (output_stream)
            output_stream->stopWriter();
        birriasdr_cancel_async(sdr_dev);
        if (worker.joinable())
            worker.join();
        birriasdr_close(sdr_dev);
        sdr_dev = nullptr;
        is_started = false;
    }
}

void BirriaSDRSource::close()
{
    is_open = false;
}

void BirriaSDRSource::set_frequency(uint64_t frequency)
{
    DSPSampleSource::set_frequency(frequency);

    if (!is_open || !is_started)
        return;

    // Dragging past a band edge is not an error; only warn when nothing is in reach.
    if (!in_tuning_range(frequency))
    {
        if (tuning_distance(frequency) > 20000000ull && frequency != last_rejected)
        {
            logger->warn("BirriaSDR cannot tune %llu Hz: bands are 2156-2610 and 4500-6200 MHz, "
                         "radio stays at its previous frequency",
                         (unsigned long long)frequency);
            last_rejected = frequency;
        }
        else
            logger->debug("BirriaSDR ignoring %llu Hz, outside the tuning bands", (unsigned long long)frequency);
        return;
    }

    int err = birriasdr_set_center_freq(sdr_dev, frequency);
    if (err == BIRRIASDR_ERROR_BUSY)
        return;
    if (err < 0)
        logger->error("BirriaSDR tune to %llu Hz failed: %s", (unsigned long long)frequency, birriasdr_strerror(err));
    else
        logger->debug("Set BirriaSDR frequency to %llu", (unsigned long long)frequency);
}

void BirriaSDRSource::drawControlUI()
{
    if (is_started)
        RImGui::beginDisabled();
    samplerate_widget.render();
    if (is_started)
        RImGui::endDisabled();

    bool changed = RImGui::Checkbox("Manual gain", &manual_gain);
    if (manual_gain)
    {
        changed |= RImGui::SteppedSliderInt("LNA", (int *)&lna, 0, 5, 1, "%d", ImGuiSliderFlags_AlwaysClamp);
        changed |= RImGui::SteppedSliderInt("LPF", (int *)&lpf, 0, 8, 1, "%d", ImGuiSliderFlags_AlwaysClamp);
        changed |= RImGui::SteppedSliderInt("Digital", (int *)&digital, 0, 12, 1, "%d", ImGuiSliderFlags_AlwaysClamp);
        RImGui::Text("%s", "Raw gain indices, not dB. 5 GHz needs LNA 4..5.");
    }

    RImGui::Checkbox("DC block", &dc_block);

    if (changed)
        apply_gains();
}

void BirriaSDRSource::set_samplerate(uint64_t samplerate)
{
    if (!samplerate_widget.set_value((double)samplerate))
        throw satdump_exception("BirriaSDR supports 10000000, 20000000, 40000000 or 80000000 sps, got " + std::to_string(samplerate));
}

uint64_t BirriaSDRSource::get_samplerate()
{
    double rate = samplerate_widget.get_value();
    return rate > 0 ? (uint64_t)rate : 40000000;
}

std::vector<dsp::SourceDescriptor> BirriaSDRSource::getAvailableSources()
{
    std::vector<dsp::SourceDescriptor> results;

    int count = birriasdr_get_device_count();
    for (int i = 0; i < count; i++)
    {
        struct birriasdr_device_info info;
        if (birriasdr_get_device_info(i, &info) < 0)
            continue;

        char name[160];
        snprintf(name, sizeof(name), "BirriaSDR %s [%04x:%04x]", info.name, info.vid, info.pid);
        results.push_back({"birriasdr", name, std::to_string(i)});
    }

    return results;
}
