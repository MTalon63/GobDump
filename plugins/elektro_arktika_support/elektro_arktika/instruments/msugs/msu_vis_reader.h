#pragma once

#include "image/image.h"
#include <cstdint>
#include <vector>

namespace elektro_arktika
{
    namespace msugs
    {
        class MSUVISReader
        {
        private:
            unsigned short *imageBuffer1, *imageBuffer2;
            unsigned short msuLineBuffer[12044];
            void writeFrame(uint8_t *data, int counter);

        public:
            int frames;
            std::vector<double> timestamps;

            std::vector<std::pair<int, double>> angle_points;

            // Related to counter correction
            int global_counter = -1;
            bool counter_locked = false;

            static constexpr int MAX_LINE_GAP = 16;
            static constexpr int FRAME_BYTES = 15210;
            int last_counter = -1;
            int rejected_frames = 0;
            std::vector<uint8_t> line_filled;
            bool pending_valid = false;
            int pending_counter = -1;
            std::vector<uint8_t> pending_data;

        public:
            MSUVISReader();
            ~MSUVISReader();
            void pushFrame(uint8_t *data, bool apply_correction);
            size_t fillMissing(size_t max_lines);
            image::Image getImage1();
            image::Image getImage2();
        };
    } // namespace msugs
} // namespace elektro_arktika
