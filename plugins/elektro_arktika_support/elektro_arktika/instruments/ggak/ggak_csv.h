#pragma once

/**
 * @file ggak_csv.h
 * @brief Compact streaming CSV writer for GGAK frames and decoded samples.
 *
 * The decoder writes one file per instrument group instead of one row per
 * scalar. This keeps the repeated metadata out of every row and makes the
 * files directly graphable:
 *
 *   ggak_mag_YYYY-MM-DD.csv       - FM-VE magnetometer, one row per reading
 *   ggak_particle_YYYY-MM-DD.csv  - GALS-VE + MIP, one row per reading
 *   ggak_esa_YYYY-MM-DD.csv       - SKIF-VE V/G ESA, one row per packet
 *   ggak_skl_YYYY-MM-DD.csv       - SKL-E spectral, one row per packet (values are ;-separated)
 *   ggak_hk_YYYY-MM-DD.csv        - housekeeping, one row per block
 *   ggak_frames_YYYY-MM-DD.csv    - optional raw frames, one row per CADU
 *
 * Files roll at UTC midnight. Within a day, a file that reaches max_bytes is
 * continued as ..._0002.csv, ..._0003.csv, etc. A restart continues the
 * current UTC day's newest part if it is not full.
 */

#include "ggak_reader.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace elektro_arktika
{
    namespace ggak
    {
        class GGAKCSVWriter
        {
        public:
            GGAKCSVWriter() = default;
            ~GGAKCSVWriter();

            bool open(const std::string &directory, const std::string &prefix = "ggak",
                      uint64_t max_bytes = 64ull * 1024ull * 1024ull,
                      bool write_raw_frames = false);
            void close();

            bool isOpen() const;
            std::vector<std::string> getPaths() const;

            /**
             * @brief Write any raw frame row and all samples decoded from it.
             *
             * Call this immediately after GGAKReader::pushFrame() returns true.
             */
            void writeFrame(const uint8_t *cadu, const GGAKReader &reader);

        private:
            enum StreamId
            {
                S_FRAMES,
                S_MAG,
                S_PARTICLE,
                S_ESA,
                S_SKL,
                S_HK,
                S_COUNT
            };

            struct Stream
            {
                std::ofstream file;
                std::string kind;
                std::string header;
                std::string path;
                std::string day;
                size_t part = 1;
                uint64_t bytes = 0;
                bool enabled = false;
            };

            struct PartInfo
            {
                std::string day; // YYYY-MM-DD, UTC
                size_t index = 0;
            };

            void initStreams();
            bool openStream(Stream &s);
            std::string partPath(const Stream &s, const std::string &day, size_t index) const;
            PartInfo findLatestPart(const Stream &s) const;
            PartInfo choosePart(const Stream &s, const std::string &day) const;
            bool ensureDay(Stream &s, const std::string &day);
            void rotate(Stream &s, const std::string &day, bool new_day);
            void checkRotate(Stream &s);

            void writeMag(Stream &s, uint16_t master_counter, const std::string &host_utc,
                          const std::string &day, double elapsed_s, bool checksum_ok,
                          const GGAKReader &reader);
            void writeParticle(Stream &s, uint16_t master_counter, const std::string &host_utc,
                               const std::string &day, double elapsed_s, bool checksum_ok,
                               const GGAKReader &reader);
            void writeEsa(Stream &s, const char *direction, uint16_t master_counter,
                          const std::string &host_utc, const std::string &day, double elapsed_s,
                          bool checksum_ok, const std::vector<SubPacket> &packets, size_t &written);
            void writeSkl(Stream &s, uint8_t frame_type, uint16_t master_counter,
                          const std::string &host_utc, const std::string &day, double elapsed_s,
                          bool checksum_ok, const GGAKReader &reader);
            void writeHk(Stream &s, const char *record_type, uint16_t master_counter,
                         const std::string &host_utc, const std::string &day, double elapsed_s,
                         bool checksum_ok, const std::vector<SubPacket> &packets, size_t &written);

        private:
            std::array<Stream, S_COUNT> streams;
            std::string directory;
            std::string prefix = "ggak";
            uint64_t max_bytes = 64ull * 1024ull * 1024ull;

            size_t mag_written = 0;
            size_t particle_written = 0;
            size_t esa20_written = 0;
            size_t esa30_written = 0;
            size_t skl_written = 0;
            size_t hk00_written = 0;
            size_t hk10_written = 0;
        };
    } // namespace ggak
} // namespace elektro_arktika
