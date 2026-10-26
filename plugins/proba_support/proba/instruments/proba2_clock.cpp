#include "proba2_clock.h"
#include "logger.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace proba
{
    namespace proba2
    {
        uint64_t readBE(const uint8_t *data, int size)
        {
            uint64_t value = 0;
            for (int i = 0; i < size; i++)
                value = (value << 8) | data[i];
            return value;
        }

        double readBEDouble(const uint8_t *data)
        {
            uint64_t bits = readBE(data, 8);
            double value = 0;
            std::memcpy(&value, &bits, sizeof(value));
            return value;
        }

        uint16_t crc16CCITT(const uint8_t *data, size_t len)
        {
            uint16_t crc = 0xFFFF;
            for (size_t i = 0; i < len; i++)
            {
                crc ^= (uint16_t)data[i] << 8;
                for (int bit = 0; bit < 8; bit++)
                    crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
            }
            return crc;
        }

        bool crc16OK(ccsds::CCSDSPacket &packet)
        {
            if (packet.payload.size() < 2)
                return false;

            uint16_t expected = (uint16_t)(packet.payload[packet.payload.size() - 2] << 8 | packet.payload[packet.payload.size() - 1]);

            uint16_t crc = 0xFFFF;
            auto feed = [&crc](uint8_t value)
            {
                crc ^= (uint16_t)value << 8;
                for (int bit = 0; bit < 8; bit++)
                    crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
            };

            for (int i = 0; i < 6; i++)
                feed(packet.header.raw[i]);
            for (size_t i = 0; i < packet.payload.size() - 2; i++)
                feed(packet.payload[i]);

            return crc == expected;
        }

        std::string formatUTC(double utc, const std::string &format)
        {
            time_t seconds = (time_t)std::floor(utc);
            std::tm tm = *gmtime(&seconds);
            char buffer[64];
            size_t len = std::strftime(buffer, sizeof(buffer), format.c_str(), &tm);
            return std::string(buffer, len);
        }

        double sunDistanceSquaredAU(double utc)
        {

            double n = (utc - 946728000.0) / 86400.0;
            constexpr double PI = 3.14159265358979323846;
            double g = (357.528 + 0.9856003 * n) * PI / 180.0;
            double distance = 1.00014 - 0.01671 * std::cos(g) - 0.00014 * std::cos(2 * g);
            return distance * distance;
        }

        double sunAngularRadiusArcsec(double utc)
        {
            constexpr double PI = 3.14159265358979323846;
            constexpr double SOLAR_RADIUS_KM = 696000.0;
            constexpr double AU_KM = 149597870.7;

            double distance = std::sqrt(sunDistanceSquaredAU(utc));
            double ratio = SOLAR_RADIUS_KM / (distance * AU_KM);
            if (ratio > 1.0)
                ratio = 1.0;
            return std::asin(ratio) * (180.0 / PI) * 3600.0;
        }

        double solarEpDeg(double utc)
        {
            constexpr double PI = 3.14159265358979323846;
            constexpr double D2R = PI / 180.0;
            constexpr double R2D = 180.0 / PI;
            constexpr double ECLIPTIC_OBLIQUITY = 23.4392911 * D2R;

            struct Vector
            {
                double x, y, z;
            };
            auto dot = [](const Vector &a, const Vector &b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
            auto cross = [](const Vector &a, const Vector &b) { return Vector{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; };
            auto normalize = [](Vector &v)
            {
                double length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
                v.x /= length;
                v.y /= length;
                v.z /= length;
            };

            double n = (utc - 946728000.0) / 86400.0;
            double T = n / 36525.0;
            double L = 280.460 + 0.9856474 * n;
            double g = (357.528 + 0.9856003 * n) * D2R;
            double longitude = L + 1.915 * std::sin(g) + 0.020 * std::sin(2 * g);
            double precession = (5029.0966 * T + 1.11113 * T * T - 0.000006 * T * T * T) / 3600.0;
            double longitude_j2000 = (longitude - precession) * D2R;

            // J2000 equator: solar longitude, IAU solar pole (286.13, 63.87), ecliptic pole.
            Vector sun{std::cos(longitude_j2000), std::sin(longitude_j2000) * std::cos(ECLIPTIC_OBLIQUITY), std::sin(longitude_j2000) * std::sin(ECLIPTIC_OBLIQUITY)};
            Vector pole{std::cos(63.87 * D2R) * std::cos(286.13 * D2R), std::cos(63.87 * D2R) * std::sin(286.13 * D2R), std::sin(63.87 * D2R)};
            Vector ecliptic{0.0, -std::sin(ECLIPTIC_OBLIQUITY), std::cos(ECLIPTIC_OBLIQUITY)};

            auto positionAngle = [&](const Vector &point)
            {
                Vector north{0.0, 0.0, 1.0};
                double along = dot(north, sun);
                north = Vector{north.x - along * sun.x, north.y - along * sun.y, north.z - along * sun.z};
                normalize(north);
                Vector east = cross(north, sun);
                normalize(east);

                along = dot(point, sun);
                Vector visible{point.x - along * sun.x, point.y - along * sun.y, point.z - along * sun.z};
                return std::atan2(dot(visible, east), dot(visible, north)) * R2D;
            };

            double angle = positionAngle(ecliptic) - positionAngle(pole);
            angle = std::fmod(angle, 360.0);
            if (angle < 0)
                angle += 360.0;
            return angle;
        }

        bool GPSClock::addRecord(ccsds::CCSDSPacket &packet)
        {
            const auto &payload = packet.payload;

            if (payload.size() != 18 + 44 + 5)
                return false;
            if (!(payload[0] == 0x10 && payload[1] == 0x08 && payload[2] == 0x89))
                return false;
            if (!crc16OK(packet))
                return false;

            std::string text((const char *)&payload[18], 44);
            if (text.compare(0, 2, "F4") != 0)
                return false;

            int week = 0;
            double seconds_of_week = 0;
            try
            {
                week = std::stoi(text.substr(3, 4));
                seconds_of_week = std::stod(text.substr(7, 8));
            }
            catch (std::exception &)
            {
                return false;
            }

            double tick = (double)readBE(payload, 5, 4) + (double)readBE(payload, 9, 2) / TICK_FRACTION;
            double gps = week * 604800.0 + seconds_of_week;
            records.push_back({tick, gps - tick});
            finalized = false;
            return true;
        }

        double GPSClock::median(std::vector<double> values)
        {
            if (values.empty())
                return 0;
            std::sort(values.begin(), values.end());
            size_t n = values.size();
            return (n % 2) ? values[n / 2] : 0.5 * (values[n / 2 - 1] + values[n / 2]);
        }

        void GPSClock::finalize()
        {
            if (finalized)
                return;
            finalized = true;

            if (records.empty())
            {
                measured = false;
                logger->warn("PROBA-2: no GPS records in this dump, image times may be off by a minute or more!");
                return;
            }

            std::vector<double> ticks, offsets;
            ticks.reserve(records.size());
            offsets.reserve(records.size());
            for (auto &record : records)
            {
                ticks.push_back(record.tick);
                offsets.push_back(record.offset);
            }

            reference_tick = median(ticks);
            gps_minus_tick = median(offsets);
            measured = true;
            logger->info("PROBA-2: clock from %d GPS records, GPS - tick = %.3f s at tick %.3f", (int)records.size(), gps_minus_tick, reference_tick);
        }

        double GPSClock::utcFromStamp(double stamp) const
        {
            double tick = stamp / TICK_FRACTION;
            double offset = gps_minus_tick + (measured ? TICK_DRIFT * (tick - reference_tick) : 0.0);
            return GPS_EPOCH_UNIX + tick + offset - GPS_LEAP_SECONDS;
        }
    }
}
