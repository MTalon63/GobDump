#pragma once

#include "common/ccsds/ccsds.h"
#include "../proba2_clock.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace proba
{
    namespace lyra
    {

        class LYRAReader
        {
        private:
            struct Sample
            {
                int64_t counter = 0;
                double stamp = 0;
                int32_t counts[4] = {0, 0, 0, 0};
                uint8_t qfactor = 3;
            };

            struct TemperatureReport
            {
                double stamp = 0;
                uint16_t raw = 0;
            };

            std::string output_folder;
            proba2::GPSClock &clock;
            std::vector<Sample> samples;
            std::vector<TemperatureReport> reports;

            bool decodeSamples(ccsds::CCSDSPacket &packet, std::vector<Sample> &out);

            void writePlot(const std::string &path, const std::vector<double> &seconds, const std::vector<std::array<double, 4>> &values, const std::string &start_text,
                           const std::string &end_text);

        public:
            LYRAReader(std::string &outputfolder, proba2::GPSClock &gps_clock);

            int packet_count = 0;
            int skipped = 0;
            int temperature_reports = 0;

            void work(ccsds::CCSDSPacket &packet);

            void workTemperature(ccsds::CCSDSPacket &packet);

            void save();
        };
    }
}
