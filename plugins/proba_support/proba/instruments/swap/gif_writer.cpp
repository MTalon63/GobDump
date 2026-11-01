#include "gif_writer.h"
#include "logger.h"
#include <algorithm>
#include <cmath>

namespace proba
{
    namespace swap
    {
        namespace
        {
            void putU16(std::FILE *file, uint16_t value)
            {
                uint8_t bytes[2] = {(uint8_t)(value & 0xFF), (uint8_t)(value >> 8)};
                std::fwrite(bytes, 1, 2, file);
            }
        }

        GIFWriter::~GIFWriter() { close(); }

        GIFWriter::Palette GIFWriter::tintPalette(uint8_t red, uint8_t green, uint8_t blue)
        {

            Palette palette{};
            double fill[3] = {(double)red, (double)green, (double)blue};
            double luma = 0.2126 * fill[0] + 0.7152 * fill[1] + 0.0722 * fill[2];

            for (int grey = 0; grey < 256; grey++)
            {
                double position = grey / 255.0 - 0.5;
                double weight = 1.0 - 4.0 * position * position;
                for (int channel = 0; channel < 3; channel++)
                {
                    double value = grey + (fill[channel] - luma) * weight;
                    palette[grey][channel] = (uint8_t)std::clamp((int)std::nearbyint(value), 0, 255);
                }
            }
            return palette;
        }

        bool GIFWriter::open(const std::string &path, int frame_width, int frame_height, const Palette &palette, int delay_centiseconds)
        {
            close();

            if (frame_width <= 0 || frame_height <= 0 || frame_width > 65535 || frame_height > 65535)
                return false;

            file = std::fopen(path.c_str(), "wb");
            if (file == nullptr)
            {
                logger->error("GIF: could not open " + path + " for writing");
                return false;
            }

            width = frame_width;
            height = frame_height;
            delay = delay_centiseconds;
            frames = 0;
            current_generation = 0;
            table.assign(4096 * 256, 0);
            generation.assign(4096 * 256, 0);
            indices.clear();

            std::fwrite("GIF89a", 1, 6, file);
            putU16(file, (uint16_t)width);
            putU16(file, (uint16_t)height);
            std::fputc(0xF7, file);
            std::fputc(0, file);
            std::fputc(0, file);

            for (int i = 0; i < 256; i++)
                std::fwrite(palette[i].data(), 1, 3, file);

            const uint8_t loop[] = {0x21, 0xFF, 0x0B, 'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0', 0x03, 0x01, 0x00, 0x00, 0x00};
            std::fwrite(loop, 1, sizeof(loop), file);

            return true;
        }

        void GIFWriter::close()
        {
            if (file == nullptr)
                return;
            std::fputc(0x3B, file);
            std::fclose(file);
            file = nullptr;
        }

        bool GIFWriter::addFrame(image::Image &img)
        {
            if (file == nullptr)
                return false;
            if (img.size() == 0 || img.depth() != 8 || img.channels() < 1)
                return false;
            if ((int)img.width() != width || (int)img.height() != height)
            {
                logger->warn("GIF: skipping a frame of " + std::to_string(img.width()) + "x" + std::to_string(img.height()) + " (expected " + std::to_string(width) + "x" + std::to_string(height) + ")");
                return false;
            }

            indices.resize((size_t)width * height);
            for (int y = 0; y < height; y++)
                for (int x = 0; x < width; x++)
                    indices[(size_t)y * width + x] = (uint8_t)img.get(0, x, y);

            const uint8_t control[] = {0x21, 0xF9, 0x04, 0x04, (uint8_t)(delay & 0xFF), (uint8_t)((delay >> 8) & 0xFF), 0x00, 0x00};
            std::fwrite(control, 1, sizeof(control), file);

            std::fputc(0x2C, file);
            putU16(file, 0);
            putU16(file, 0);
            putU16(file, (uint16_t)width);
            putU16(file, (uint16_t)height);
            std::fputc(0x00, file);

            std::fputc(8, file);
            writeLZW(indices);
            std::fputc(0x00, file);

            frames++;
            return true;
        }

        void GIFWriter::writeLZW(const std::vector<uint8_t> &pixels)
        {
            constexpr int clear_code = 256;
            constexpr int end_code = 257;

            block_length = 0;
            bit_buffer = 0;
            bit_count = 0;

            int code_size = 9;
            int next_code = end_code + 1;

            auto newGeneration = [&]()
            {
                current_generation++;
                if (current_generation == 0)
                {
                    std::fill(generation.begin(), generation.end(), 0);
                    current_generation = 1;
                }
                return current_generation;
            };

            uint32_t generation_id = newGeneration();
            emitCode(clear_code, code_size);

            int prefix = pixels.empty() ? 0 : pixels[0];
            for (size_t i = 1; i < pixels.size(); i++)
            {
                uint8_t next = pixels[i];
                uint32_t key = ((uint32_t)prefix << 8) | next;

                if (generation[key] == generation_id)
                {
                    prefix = table[key];
                    continue;
                }

                emitCode((uint32_t)prefix, code_size);

                if (next_code < 4096)
                {
                    table[key] = next_code;
                    generation[key] = generation_id;
                    next_code++;

                    // The decoder adds its matching entry one code later, so widen after the boundary.
                    if (next_code == (1 << code_size) + 1 && code_size < 12)
                        code_size++;
                }
                else
                {
                    emitCode(clear_code, code_size);
                    generation_id = newGeneration();
                    next_code = end_code + 1;
                    code_size = 9;
                }

                prefix = next;
            }

            if (!pixels.empty())
                emitCode((uint32_t)prefix, code_size);
            emitCode(end_code, code_size);

            flushBits();
            flushBlock();
        }

        void GIFWriter::emitCode(uint32_t code, int bits)
        {
            bit_buffer |= (code & ((1u << bits) - 1u)) << bit_count;
            bit_count += bits;

            while (bit_count >= 8)
            {
                lzwByte((uint8_t)(bit_buffer & 0xFF));
                bit_buffer >>= 8;
                bit_count -= 8;
            }
        }

        void GIFWriter::flushBits()
        {
            if (bit_count > 0)
            {
                lzwByte((uint8_t)(bit_buffer & 0xFF));
                bit_buffer = 0;
                bit_count = 0;
            }
        }

        void GIFWriter::lzwByte(uint8_t value)
        {
            block[block_length++] = value;
            if (block_length == 255)
            {
                std::fputc(255, file);
                std::fwrite(block, 1, 255, file);
                block_length = 0;
            }
        }

        void GIFWriter::flushBlock()
        {
            if (block_length > 0)
            {
                std::fputc((uint8_t)block_length, file);
                std::fwrite(block, 1, (size_t)block_length, file);
                block_length = 0;
            }
        }
    }
}
