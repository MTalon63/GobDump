#pragma once

#include "image/image.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace proba
{
    namespace swap
    {

        class GIFWriter
        {
        public:
            using Palette = std::array<std::array<uint8_t, 3>, 256>;

            static Palette tintPalette(uint8_t red = 237, uint8_t green = 177, uint8_t blue = 3);

            GIFWriter() = default;
            ~GIFWriter();

            bool open(const std::string &path, int width, int height, const Palette &palette, int delay_centiseconds = 10);
            bool addFrame(image::Image &img);
            void close();

            bool isOpen() const { return file != nullptr; }
            int frameCount() const { return frames; }

        private:
            std::FILE *file = nullptr;
            int width = 0, height = 0, delay = 10, frames = 0;

            std::vector<uint8_t> indices;
            std::vector<int32_t> table;
            std::vector<uint32_t> generation;
            uint32_t current_generation = 0;

            uint32_t bit_buffer = 0;
            int bit_count = 0;
            uint8_t block[255] = {};
            int block_length = 0;

            void lzwByte(uint8_t value);
            void flushBits();
            void flushBlock();
            void emitCode(uint32_t code, int bits);
            void writeLZW(const std::vector<uint8_t> &pixels);
        };
    }
}
