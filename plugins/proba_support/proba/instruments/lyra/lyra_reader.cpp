#include "lyra_reader.h"
#include "image/io.h"
#include "image/text.h"
#include "logger.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>

namespace proba
{
    namespace lyra
    {
        namespace
        {
            constexpr int CHANNELS = 4;
            constexpr int SAMPLES = 501;
            constexpr int SAMPLE_BYTES = 3;
            constexpr int SAMPLE_SECTION = CHANNELS * SAMPLES * SAMPLE_BYTES;
            constexpr int MARKS_OFFSET = SAMPLE_SECTION + 4 + 500 * 4;
            constexpr int MARK_BYTES = 7;
            constexpr int COUNTER_RATE = 20;
            constexpr int PLOT_SCALE = 2;
            constexpr int CLEAR_CODE = 256;
            constexpr int CODES_PER_GROUP = 8;

            constexpr double INTEGRATION_MS = 49.9894193;
            constexpr double DEFAULT_TEMPERATURE = 50.5;

            const char *const CHANNEL_NAMES[CHANNELS] = {"Lyman alpha", "Herzberg", "Aluminium filter", "Zirconium filter"};

            struct ChannelCalibration
            {
                double k;
                double d0;
                double a;
                double b;
            };

            constexpr ChannelCalibration CHANNEL_CALIBRATION[CHANNELS] = {
                {1.28113848e-05, 7.000848, 1.272312e-05, 0.240884},
                {1.02583741e-03, 6.455, 0.0, 0.0},
                {1.41720406e-04, 6.32463, 2.820584e-04, 0.138467},
                {2.16453158e-05, 6.299687, 4.799281e-04, 0.168691},
            };

            constexpr double TEMPERATURE_POLY[3] = {5.876600936895061e-08, -0.0029869838697916666, 66.51950307884134};

            struct DegradationRow
            {
                int day;
                double g[CHANNELS];
            };

            constexpr DegradationRow DEGRADATION[] = {
                {0, {6.2907691160e-03, 6.9633575980e-01, 2.2595608600e-03, 6.6969715320e-04}},
                {11, {6.2901006210e-03, 6.9633575980e-01, 2.2603452000e-03, 6.7068469860e-04}},
                {21, {6.2903520520e-03, 6.9633575980e-01, 2.2585938330e-03, 6.6817811740e-04}},
                {31, {6.2905605740e-03, 6.9633575980e-01, 2.2609632090e-03, 6.7138923710e-04}},
                {41, {6.2907829870e-03, 6.9633575980e-01, 2.2612575520e-03, 6.7172810080e-04}},
                {51, {6.2913148570e-03, 6.9633575980e-01, 2.2596818810e-03, 6.6954763620e-04}},
                {61, {6.2911985510e-03, 6.9633575980e-01, 2.2618645450e-03, 6.7242195630e-04}},
                {72, {6.2912234180e-03, 6.9633575980e-01, 2.2617143060e-03, 6.7223807910e-04}},
                {82, {6.2913804430e-03, 6.9633575980e-01, 2.2618866610e-03, 6.7245117990e-04}},
                {92, {6.2894867460e-03, 6.9633575980e-01, 2.2612925410e-03, 6.6812726390e-04}},
                {102, {6.2896355800e-03, 6.9633575980e-01, 2.2614157760e-03, 6.6832052430e-04}},
                {112, {6.2910151390e-03, 6.9633575980e-01, 2.2615145290e-03, 6.6849238640e-04}},
                {122, {6.2911663370e-03, 6.9633575980e-01, 2.2616289500e-03, 6.6870085550e-04}},
                {133, {6.2912824400e-03, 6.9633575980e-01, 2.2617086070e-03, 6.6888436260e-04}},
                {143, {6.2910304300e-03, 6.9633575980e-01, 2.2614805910e-03, 6.6859309310e-04}},
                {153, {6.2914511930e-03, 6.9633575980e-01, 2.2618103810e-03, 6.6918662170e-04}},
                {164, {6.2916184790e-03, 6.9633575980e-01, 2.2619367310e-03, 6.6949752300e-04}},
                {174, {6.2915037530e-03, 6.9633575980e-01, 2.2618180750e-03, 6.6940395460e-04}},
                {184, {6.2915178850e-03, 6.9633575980e-01, 2.2618104890e-03, 6.6948282520e-04}},
                {194, {6.2912776420e-03, 6.9633575980e-01, 2.2615312900e-03, 6.6911643060e-04}},
                {204, {6.2916832740e-03, 6.9633575980e-01, 2.2618910060e-03, 6.6980803190e-04}},
                {214, {6.2917606400e-03, 6.9633575980e-01, 2.2619453580e-03, 6.6998198050e-04}},
                {225, {6.2919483860e-03, 6.9633575980e-01, 2.2620447350e-03, 6.7018888010e-04}},
                {235, {6.2922483680e-03, 6.9633575980e-01, 2.2623248560e-03, 6.7083490510e-04}},
                {245, {6.2926036740e-03, 6.9633575980e-01, 2.2626621930e-03, 6.7153605820e-04}},
                {255, {6.2926570690e-03, 6.9633575980e-01, 2.2627532880e-03, 6.7175425800e-04}},
                {265, {6.2927918570e-03, 6.9633575980e-01, 2.2628753470e-03, 6.7200353290e-04}},
                {275, {6.2928096030e-03, 6.9633575980e-01, 2.2629042090e-03, 6.7208287760e-04}},
                {286, {6.2928748600e-03, 6.9633575980e-01, 2.2629036460e-03, 6.7209628810e-04}},
                {296, {6.2929356510e-03, 6.9633575980e-01, 2.2629581250e-03, 6.7217362900e-04}},
                {306, {6.2930118240e-03, 6.9633575980e-01, 2.2629935380e-03, 6.7219869080e-04}},
                {317, {6.2946250750e-03, 6.9633575980e-01, 2.2643569320e-03, 6.7374614260e-04}},
                {327, {6.2940594620e-03, 6.9633575980e-01, 2.2637966050e-03, 6.7301499760e-04}},
                {337, {6.2941779360e-03, 6.9633575980e-01, 2.2638990470e-03, 6.7311867860e-04}},
                {345, {6.2942447700e-03, 6.9633575980e-01, 2.2639546430e-03, 6.7316682820e-04}},
                {355, {6.2942466680e-03, 6.9633575980e-01, 2.2639547130e-03, 6.7313245010e-04}},
                {365, {6.2943865880e-03, 6.9633575980e-01, 2.2640915080e-03, 6.7325079910e-04}},
                {376, {6.2944224510e-03, 6.9633575980e-01, 2.2641313540e-03, 6.7324717460e-04}},
                {386, {6.2945846080e-03, 6.9633575980e-01, 2.2641883450e-03, 6.7326602660e-04}},
                {396, {6.2952876880e-03, 6.9633575980e-01, 2.2649101510e-03, 6.7410441690e-04}},
                {406, {6.2946696200e-03, 6.9633575980e-01, 2.2643873390e-03, 6.7340837730e-04}},
                {416, {6.2948299710e-03, 6.9633575980e-01, 2.2645358550e-03, 6.7355303090e-04}},
                {426, {6.2949921160e-03, 6.9633575980e-01, 2.2646789820e-03, 6.7369674030e-04}},
                {437, {6.2959978740e-03, 6.9633575980e-01, 2.2655353680e-03, 6.7476853880e-04}},
                {447, {6.2951961670e-03, 6.9633575980e-01, 2.2648682550e-03, 6.7390640000e-04}},
                {457, {6.2952218120e-03, 6.9633575980e-01, 2.2649060840e-03, 6.7395747140e-04}},
                {467, {6.2954913830e-03, 6.9633575980e-01, 2.2646381410e-03, 6.7525515630e-04}},
                {477, {6.2953286250e-03, 6.9633575980e-01, 2.2641064580e-03, 6.7464727570e-04}},
                {487, {6.2952839390e-03, 6.9633575980e-01, 2.2640362250e-03, 6.7458449380e-04}},
                {498, {6.2954755750e-03, 6.9633575980e-01, 2.2642441550e-03, 6.7491444920e-04}},
                {508, {6.2955304370e-03, 6.9633575980e-01, 2.2642901150e-03, 6.7503649830e-04}},
                {518, {6.2956772600e-03, 6.9633575980e-01, 2.2644446120e-03, 6.7530970630e-04}},
                {529, {6.2957657960e-03, 6.9633575980e-01, 2.2645359120e-03, 6.7553088750e-04}},
                {539, {6.2957790290e-03, 6.9633575980e-01, 2.2645271860e-03, 6.7561412380e-04}},
                {549, {6.2960475720e-03, 6.9633575980e-01, 2.2648506760e-03, 6.7614069440e-04}},
                {559, {6.2962887080e-03, 6.9633575980e-01, 2.2651569360e-03, 6.7664522410e-04}},
                {569, {6.2966103740e-03, 6.9633575980e-01, 2.2656314080e-03, 6.7736824580e-04}},
            };

            bool uncompressUnix(const uint8_t *data, size_t length, std::vector<uint8_t> &out)
            {
                if (length < 3 || data[0] != 0x1F || data[1] != 0x9D)
                    return false;

                int max_bits = data[2] & 0x1F;
                bool block_mode = (data[2] & 0x80) != 0;
                if (max_bits < 9 || max_bits > 16)
                    return false;

                const uint8_t *body = data + 3;
                size_t body_size = length - 3;
                uint64_t total_bits = (uint64_t)body_size * 8;
                const int max_codes = 1 << max_bits;

                std::vector<std::vector<uint8_t>> table(max_codes);
                std::vector<uint8_t> defined(max_codes, 0);
                std::vector<uint8_t> previous;
                int free_code = 257;
                bool have_previous = false;

                auto reset = [&]()
                {
                    for (int i = 0; i < 256; i++)
                    {
                        table[i].assign(1, (uint8_t)i);
                        defined[i] = 1;
                    }
                    for (int i = 256; i < max_codes; i++)
                    {
                        table[i].clear();
                        defined[i] = 0;
                    }
                    free_code = 257;
                    have_previous = false;
                    previous.clear();
                };
                reset();

                int bits = 9, index = 0;
                uint64_t group_start = 0;
                out.clear();

                while (true)
                {
                    uint64_t position = group_start + (uint64_t)index * bits;
                    if (position + bits > total_bits)
                        break;

                    size_t byte_pos = (size_t)(position >> 3);
                    uint32_t chunk = 0;
                    for (int i = 0; i < 3 && byte_pos + i < body_size; i++)
                        chunk |= (uint32_t)body[byte_pos + i] << (8 * i);
                    int code = (chunk >> (position & 7)) & ((1 << bits) - 1);

                    index++;
                    if (index == CODES_PER_GROUP)
                    {
                        group_start += (uint64_t)CODES_PER_GROUP * bits;
                        index = 0;
                    }

                    if (block_mode && code == CLEAR_CODE)
                    {
                        reset();
                        if (index)
                            group_start += (uint64_t)CODES_PER_GROUP * bits;
                        bits = 9;
                        index = 0;
                        continue;
                    }

                    std::vector<uint8_t> entry;
                    if (code < max_codes && defined[code])
                        entry = table[code];
                    else if (code == free_code && have_previous)
                    {
                        entry = previous;
                        entry.push_back(previous[0]);
                    }
                    else
                        return false;

                    out.insert(out.end(), entry.begin(), entry.end());

                    if (have_previous && free_code < max_codes)
                    {
                        table[free_code] = previous;
                        table[free_code].push_back(entry[0]);
                        defined[free_code] = 1;
                        free_code++;
                    }
                    previous = entry;
                    have_previous = true;

                    if (free_code > (1 << bits) - 1 && bits < max_bits)
                    {
                        if (index)
                        {
                            group_start += (uint64_t)CODES_PER_GROUP * bits;
                            index = 0;
                        }
                        bits++;
                    }
                }

                return true;
            }

            double unitTemperatureC(uint16_t raw)
            {
                return (TEMPERATURE_POLY[0] * raw + TEMPERATURE_POLY[1]) * raw + TEMPERATURE_POLY[2];
            }

            void degradationFor(int day, double g[CHANNELS], std::string &source)
            {
                constexpr int rows = (int)(sizeof(DEGRADATION) / sizeof(DEGRADATION[0]));

                if (day <= DEGRADATION[0].day)
                {
                    for (int c = 0; c < CHANNELS; c++)
                        g[c] = DEGRADATION[0].g[c];
                    source = day < DEGRADATION[0].day ? "before the table (2025-03-25), first value" : "table";
                    return;
                }
                if (day >= DEGRADATION[rows - 1].day)
                {
                    for (int c = 0; c < CHANNELS; c++)
                        g[c] = DEGRADATION[rows - 1].g[c];
                    source = "after the table, last value";
                    return;
                }

                for (int i = 1; i < rows; i++)
                {
                    if (day > DEGRADATION[i].day)
                        continue;
                    double f = (double)(day - DEGRADATION[i - 1].day) / (double)(DEGRADATION[i].day - DEGRADATION[i - 1].day);
                    for (int c = 0; c < CHANNELS; c++)
                        g[c] = DEGRADATION[i - 1].g[c] + f * (DEGRADATION[i].g[c] - DEGRADATION[i - 1].g[c]);
                    source = "interpolated";
                    return;
                }

                for (int c = 0; c < CHANNELS; c++)
                    g[c] = DEGRADATION[0].g[c];
                source = "table";
            }

            std::string formatScience(double value)
            {
                char buffer[32];
                std::snprintf(buffer, sizeof(buffer), "%.4e", value);
                return buffer;
            }

            std::string uniqueStem(const std::string &folder, const std::string &stem, const std::string &extension)
            {
                if (!std::filesystem::exists(folder + "/" + stem + extension))
                    return stem;
                int index = 0;
                while (std::filesystem::exists(folder + "/" + stem + "-" + std::to_string(index) + extension))
                    index++;
                return stem + "-" + std::to_string(index);
            }
        }

        LYRAReader::LYRAReader(std::string &outputfolder, proba2::GPSClock &gps_clock) : clock(gps_clock) { output_folder = outputfolder; }

        bool LYRAReader::decodeSamples(ccsds::CCSDSPacket &packet, std::vector<Sample> &out)
        {
            const auto &payload = packet.payload;

            if (payload.size() <= 95)
                return false;
            if (!(payload[0] == 0x10 && payload[1] == 0x08 && payload[2] == 0x8B))
                return false;
            if (proba2::readBE(payload, 34, 2) != 0)
                return false;
            if (!(payload[90] == 0x1F && payload[91] == 0x9D))
                return false;
            if (!proba2::crc16OK(packet))
                return false;

            std::vector<uint8_t> block;
            if (!uncompressUnix(payload.data() + 90, payload.size() - 90 - 2, block))
                return false;

            if (block.size() < (size_t)MARKS_OFFSET)
                return false;
            size_t tail_size = block.size() - MARKS_OFFSET;
            size_t marks = tail_size / (MARK_BYTES + 4);
            if (marks == 0 || tail_size != marks * (MARK_BYTES + 4))
                return false;

            std::vector<uint64_t> mark_stamps(marks);
            std::vector<uint64_t> mark_counters(marks);
            for (size_t i = 0; i < marks; i++)
            {
                mark_stamps[i] = proba2::readBE(&block[MARKS_OFFSET + i * MARK_BYTES], 6);
                mark_counters[i] = proba2::readBE(&block[MARKS_OFFSET + marks * MARK_BYTES + i * 4], 4);
            }

            int32_t counts[CHANNELS][SAMPLES];
            for (int channel = 0; channel < CHANNELS; channel++)
            {
                for (int sample = 0; sample < SAMPLES; sample++)
                {
                    const uint8_t *v = &block[(size_t)(channel * SAMPLES + sample) * SAMPLE_BYTES];
                    int32_t value = (v[0] << 16) | (v[1] << 8) | v[2];
                    if (value >= (1 << 23))
                        value -= (1 << 24);
                    counts[channel][sample] = sample == 0 ? value : counts[channel][sample - 1] + value;
                }
            }

            uint64_t first_counter = proba2::readBE(payload, 13, 3);
            const double stamp_step = proba2::TICK_FRACTION / COUNTER_RATE;

            out.reserve(SAMPLES);
            for (int sample = 0; sample < SAMPLES; sample++)
            {
                int64_t counter = (int64_t)first_counter + 2 * sample;
                double stamp = (double)mark_stamps[0] + (double)(counter - (int64_t)mark_counters[0]) * stamp_step;

                bool at_mark = false;
                for (size_t i = 0; i < marks; i++)
                    if ((int64_t)mark_counters[i] == counter)
                    {
                        at_mark = true;
                        break;
                    }

                Sample value;
                value.counter = counter;
                value.stamp = stamp;
                value.qfactor = at_mark ? 4 : 3;
                for (int channel = 0; channel < CHANNELS; channel++)
                    value.counts[channel] = counts[channel][sample];
                out.push_back(value);
            }

            return true;
        }

        void LYRAReader::work(ccsds::CCSDSPacket &packet)
        {
            packet_count++;
            std::vector<Sample> decoded;
            if (!decodeSamples(packet, decoded))
            {
                skipped++;
                return;
            }
            samples.insert(samples.end(), decoded.begin(), decoded.end());
        }

        void LYRAReader::workTemperature(ccsds::CCSDSPacket &packet)
        {
            const auto &payload = packet.payload;

            if (payload.size() != 144 || payload[1] != 0x03 || payload[2] != 0x19 || payload[15] != 70)
                return;
            if (!proba2::crc16OK(packet))
                return;

            TemperatureReport report;
            report.stamp = (double)proba2::readBE(payload, 5, 6);
            report.raw = (uint16_t)proba2::readBE(payload, 49, 2);
            reports.push_back(report);
            temperature_reports++;
        }

        void LYRAReader::save()
        {
            if (samples.empty())
            {
                logger->info("LYRA: no samples in this dump");
                return;
            }

            clock.finalize();

            std::sort(samples.begin(), samples.end(), [](const Sample &a, const Sample &b) { return a.counter < b.counter; });
            samples.erase(std::unique(samples.begin(), samples.end(), [](const Sample &a, const Sample &b) { return a.counter == b.counter; }), samples.end());

            double utc0 = clock.utcFromStamp(samples.front().stamp);
            double utc_end = clock.utcFromStamp(samples.back().stamp);
            double midnight = std::floor(utc0 / 86400.0) * 86400.0;
            int day_number = (int)std::floor((midnight - proba2::LYRA_EPOCH_UNIX) / 86400.0 + 0.5);

            std::vector<std::pair<double, double>> temperatures;
            temperatures.reserve(reports.size());
            for (auto &report : reports)
                temperatures.push_back({clock.utcFromStamp(report.stamp) - midnight, unitTemperatureC(report.raw)});
            std::sort(temperatures.begin(), temperatures.end());

            std::string temperature_source = temperatures.empty() ? "assumed" : "housekeeping";
            double temperature_mean = DEFAULT_TEMPERATURE;
            bool temperature_assumed = temperatures.empty();

            auto temperatureAt = [&](double seconds)
            {
                if (temperatures.empty())
                    return DEFAULT_TEMPERATURE;
                if (seconds <= temperatures.front().first)
                    return temperatures.front().second;
                if (seconds >= temperatures.back().first)
                    return temperatures.back().second;
                for (size_t i = 1; i < temperatures.size(); i++)
                {
                    if (seconds > temperatures[i].first)
                        continue;
                    double span = temperatures[i].first - temperatures[i - 1].first;
                    if (span <= 0)
                        return temperatures[i].second;
                    double f = (seconds - temperatures[i - 1].first) / span;
                    return temperatures[i - 1].second + f * (temperatures[i].second - temperatures[i - 1].second);
                }
                return temperatures.back().second;
            };

            double degradation[CHANNELS] = {0, 0, 0, 0};
            std::string degradation_source;
            degradationFor(day_number, degradation, degradation_source);

            std::string stem = "lyra_" + proba2::formatUTC(utc0, "%Y%m%d-%H%M%S");
            stem = uniqueStem(output_folder, stem, ".csv");
            std::string csv_path = output_folder + "/" + stem + ".csv";

            std::ofstream file(csv_path);
            file << "# PROBA-2 LYRA, from a raw .cadu dump\n";
            file << "# level 0: counts per integration; level 1: counts / 49.9894193 ms (kHz); level 2: irradiance (W/m^2)\n";
            file << "# clock: " << (clock.isMeasured() ? "GPS records (APID 18)" : "fixed offset, no GPS records") << "\n";
            file << "# unit 2 temperature: " << temperature_source << "\n";
            file << "# degradation G: " << degradation_source << "\n";
            file << "utc,time_s,counter,qfactor";
            for (int channel = 0; channel < CHANNELS; channel++)
                file << ",ch" << (channel + 1) << "_counts";
            for (int channel = 0; channel < CHANNELS; channel++)
                file << ",ch" << (channel + 1) << "_khz";
            for (int channel = 0; channel < CHANNELS; channel++)
                file << ",ch" << (channel + 1) << "_wm2";
            file << ",temp_c\n";

            std::vector<double> seconds(samples.size());
            std::vector<std::array<double, CHANNELS>> irradiance(samples.size());
            double temperature_sum = 0;

            for (size_t i = 0; i < samples.size(); i++)
            {
                const Sample &sample = samples[i];
                double utc = clock.utcFromStamp(sample.stamp);
                double seconds_of_day = utc - midnight;
                double temperature = temperatureAt(seconds_of_day);
                double d2 = proba2::sunDistanceSquaredAU(utc);
                int millis = (int)std::lround((utc - std::floor(utc)) * 1000.0);
                if (millis > 999)
                    millis = 999;
                seconds[i] = seconds_of_day;
                temperature_sum += temperature;

                double khz[CHANNELS], wm2[CHANNELS];
                for (int channel = 0; channel < CHANNELS; channel++)
                {
                    khz[channel] = std::round((double)sample.counts[channel] / INTEGRATION_MS * 1e6) / 1e6;
                    const ChannelCalibration &cal = CHANNEL_CALIBRATION[channel];
                    double dark = cal.d0 + cal.a * std::exp(cal.b * temperature);
                    wm2[channel] = cal.k * d2 * (khz[channel] - dark) + degradation[channel];
                    irradiance[i][channel] = wm2[channel];
                }

                file << proba2::formatUTC(utc, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3) << std::setfill('0') << millis << std::setfill(' ') << "Z," << std::fixed
                     << std::setprecision(3) << seconds_of_day << ',' << sample.counter << ',' << (int)sample.qfactor;

                for (int channel = 0; channel < CHANNELS; channel++)
                    file << ',' << sample.counts[channel];
                file << std::setprecision(6);
                for (int channel = 0; channel < CHANNELS; channel++)
                    file << ',' << khz[channel];
                file << std::scientific << std::setprecision(10);
                for (int channel = 0; channel < CHANNELS; channel++)
                    file << ',' << wm2[channel];
                file << std::fixed << std::setprecision(3) << ',' << temperature << '\n';
            }
            file.close();

            temperature_mean = temperature_assumed ? DEFAULT_TEMPERATURE : temperature_sum / (double)samples.size();

            std::string plot_path = output_folder + "/" + stem + "_lev2.png";
            writePlot(plot_path, seconds, irradiance, proba2::formatUTC(utc0, "%Y-%m-%d %H:%M:%S"), proba2::formatUTC(utc_end, "%H:%M:%S"));

            logger->info("LYRA: %d samples (%s to %s UTC), level 0/1/2 written to %s", (int)samples.size(), proba2::formatUTC(utc0, "%Y-%m-%d %H:%M:%S").c_str(),
                         proba2::formatUTC(utc_end, "%H:%M:%S").c_str(), (stem + ".csv").c_str());
            logger->info("LYRA: unit 2 %.2f C (%s), G %s", temperature_mean, temperature_source.c_str(), degradation_source.c_str());
        }

        void LYRAReader::writePlot(const std::string &path, const std::vector<double> &seconds, const std::vector<std::array<double, CHANNELS>> &values, const std::string &start_text,
                                   const std::string &end_text)
        {
            const int width = 1100 * PLOT_SCALE;
            const int header_height = 46 * PLOT_SCALE;
            const int panel_height = 210 * PLOT_SCALE;
            const int panel_gap = 10 * PLOT_SCALE;
            const int footer_height = 30 * PLOT_SCALE;
            const int left = 110 * PLOT_SCALE;
            const int right = 24 * PLOT_SCALE;
            const int plot_top_offset = 26 * PLOT_SCALE;
            const int plot_bottom_offset = 16 * PLOT_SCALE;

            int height = header_height + CHANNELS * (panel_height + panel_gap) + footer_height;
            image::Image img(8, width, height, 3);
            img.fill_color({0.04, 0.04, 0.06});

            auto thickLine = [&img](int x0, int y0, int x1, int y1, const std::vector<double> &color)
            {
                for (int t = 0; t < PLOT_SCALE; t++)
                {
                    img.draw_line(x0 + t, y0, x1 + t, y1, color);
                    img.draw_line(x0, y0 + t, x1, y1 + t, color);
                }
            };

            image::TextDrawer text;
            text.init_font();
            if (text.font_ready())
                text.draw_text(img, 14 * PLOT_SCALE, 12 * PLOT_SCALE, {0.92, 0.92, 0.95}, 22 * PLOT_SCALE, "PROBA-2 LYRA - level 2 irradiance");

            const std::vector<double> colors[CHANNELS] = {
                {1.0, 0.82, 0.25},
                {0.45, 0.78, 1.0},
                {1.0, 0.52, 0.45},
                {0.55, 1.0, 0.62},
            };

            size_t n = values.size();
            if (n < 2 || seconds.size() != n)
                return;

            for (int channel = 0; channel < CHANNELS; channel++)
            {
                int top = header_height + channel * (panel_height + panel_gap);
                int plot_top = top + plot_top_offset;
                int plot_bottom = top + panel_height - plot_bottom_offset;

                for (int t = 0; t < PLOT_SCALE; t++)
                    img.draw_rectangle(left + t, plot_top + t, width - right - t, plot_bottom - t, {0.20, 0.20, 0.26}, false);
                for (int grid = 1; grid < 4; grid++)
                {
                    int y = plot_top + (plot_bottom - plot_top) * grid / 4;
                    for (int t = 0; t < PLOT_SCALE; t++)
                        img.draw_line(left + 1, y + t, width - right - 1, y + t, {0.12, 0.12, 0.16});
                }

                double minimum = values[0][channel], maximum = values[0][channel];
                for (size_t i = 0; i < n; i++)
                {
                    minimum = std::min(minimum, values[i][channel]);
                    maximum = std::max(maximum, values[i][channel]);
                }
                if (!(maximum > minimum))
                {
                    maximum = minimum + 1e-12;
                    minimum -= 1e-12;
                }
                double margin = (maximum - minimum) * 0.05;
                minimum -= margin;
                maximum += margin;

                int prev_x = -1, prev_y = -1;
                for (size_t i = 0; i < n; i++)
                {
                    int x = left + 1 + (int)((width - right - left - 2) * (double)i / (double)(n - 1));
                    int y = plot_bottom - 1 - (int)((plot_bottom - plot_top - 2) * (values[i][channel] - minimum) / (maximum - minimum));
                    if (i > 0 && seconds[i] - seconds[i - 1] > 1.0)
                        prev_x = -1;
                    if (prev_x >= 0)
                        thickLine(prev_x, prev_y, x, y, colors[channel]);
                    prev_x = x;
                    prev_y = y;
                }

                if (text.font_ready())
                {
                    text.draw_text(img, 10 * PLOT_SCALE, plot_top + 2 * PLOT_SCALE, colors[channel], 15 * PLOT_SCALE, CHANNEL_NAMES[channel]);
                    text.draw_text(img, 10 * PLOT_SCALE, plot_top + 20 * PLOT_SCALE, {0.65, 0.65, 0.7}, 12 * PLOT_SCALE, formatScience(maximum));
                    text.draw_text(img, 10 * PLOT_SCALE, plot_bottom - 12 * PLOT_SCALE, {0.65, 0.65, 0.7}, 12 * PLOT_SCALE, formatScience(minimum));
                    text.draw_text(img, left, plot_top - 18 * PLOT_SCALE, {0.65, 0.65, 0.7}, 12 * PLOT_SCALE, "W/m2");
                }
            }

            if (text.font_ready())
            {
                text.draw_text(img, left, height - 20 * PLOT_SCALE, {0.75, 0.75, 0.8}, 13 * PLOT_SCALE, start_text + " UTC");
                int text_width = (int)end_text.size() * 7 * PLOT_SCALE;
                text.draw_text(img, width - right - text_width, height - 20 * PLOT_SCALE, {0.75, 0.75, 0.8}, 13 * PLOT_SCALE, end_text + " UTC");
            }

            image::save_img(img, path);
        }
    }
}
