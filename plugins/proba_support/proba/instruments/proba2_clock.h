#pragma once

#include "common/ccsds/ccsds.h"
#include <cstdint>
#include <string>
#include <vector>

namespace proba
{
    namespace proba2
    {
        constexpr double GPS_EPOCH_UNIX = 315964800.0;
        constexpr double GPS_LEAP_SECONDS = 18.0;
        constexpr double TICK_FRACTION = 65536.0;
        constexpr double TICK_DRIFT = 1.064 / 86400.0;
        constexpr double GPS_MINUS_TICK_FALLBACK = 1209613891.0;
        constexpr double LYRA_EPOCH_UNIX = 1742860800.0;

        uint64_t readBE(const uint8_t *data, int size);

        inline uint64_t readBE(const std::vector<uint8_t> &data, size_t offset, int size) { return readBE(data.data() + offset, size); }

        double readBEDouble(const uint8_t *data);

        uint16_t crc16CCITT(const uint8_t *data, size_t len);

        bool crc16OK(ccsds::CCSDSPacket &packet);

        std::string formatUTC(double utc, const std::string &format);

        double sunDistanceSquaredAU(double utc);

        double sunAngularRadiusArcsec(double utc);

        double solarEpDeg(double utc);

        class GPSClock
        {
        private:
            struct Record
            {
                double tick;
                double offset;
            };

            std::vector<Record> records;
            double reference_tick = 0;
            double gps_minus_tick = GPS_MINUS_TICK_FALLBACK;
            bool measured = false;
            bool finalized = false;

            static double median(std::vector<double> values);

        public:
            bool addRecord(ccsds::CCSDSPacket &packet);

            void finalize();

            double utcFromStamp(double stamp) const;

            bool isMeasured() const { return measured; }
            size_t recordCount() const { return records.size(); }
        };
    }
}
