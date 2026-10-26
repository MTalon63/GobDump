#pragma once

#include "common/ccsds/ccsds.h"
#include "gif_writer.h"
#include "image/image.h"
#include "../proba2_clock.h"
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace proba
{
    namespace swap
    {

        class SWAPReader
        {
        public:
            struct ImageHeader
            {
                uint64_t end = 0;
                uint64_t start = 0;
                uint8_t obs_mode = 0;
                uint8_t capture_mode = 0;
                uint8_t priority = 0;
                double lang_rot = 0;
                double pav_rot0 = 0;
                double pav_rot1 = 0;
                uint32_t exposure = 0;
                uint32_t compressed_size = 0;
                uint32_t jpeg_length = 0;
                bool valid = false;

                int packetCount() const;

                int packetLength(int index) const;
            };

        private:
            struct Packet
            {
                std::vector<uint8_t> data;
                bool crc_ok = false;
            };

            struct WIP_Swap
            {
                std::map<uint16_t, Packet> packets;
            };

            struct Assembled
            {
                ImageHeader header;
                std::vector<uint8_t> jpeg;
                int packets = 0;
                int packets_expected = 0;
                int bad_crc = 0;
                bool header_ok = false;

                bool complete() const { return packets == packets_expected; }
            };

            std::map<uint64_t, WIP_Swap> currentOuts;
            std::string output_folder;
            proba2::GPSClock &clock;
            bool align_sun = false;
            bool make_gif = false;
            GIFWriter gif;

            bool findFirstPacket(const WIP_Swap &image, uint16_t &sequence, const Packet *&packet) const;
            bool assemble(const WIP_Swap &image, bool keep_bit_errors, Assembled &out) const;
            static double imageQuality(image::Image &img);
            static int rollDegrees(double lang_rot);
            static int northUpSteps(double lang_rot);
            static image::Image rotate90(image::Image &img, int steps);
            bool alignImage(image::Image &img, const ImageHeader &header, double utc);
            std::string uniqueName(const std::string &filename) const;

        public:
            std::vector<std::string> all_images;
            int count = 0;

            SWAPReader(std::string &outputfolder, proba2::GPSClock &gps_clock, bool align, bool gif_animation);

            void work(ccsds::CCSDSPacket &packet);
            void save();
        };
    }
}
