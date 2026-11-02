#ifdef BUILD_ZIQ
#include "ziq.h"
#include "logger.h"
#include "dsp/buffer.h"
#include <volk/volk.h>

#define ZIQ_DECOMPRESS_BUFSIZE 8192

namespace ziq
{
    static std::string annotate_quant(const std::string &in, int bits)
    {
        std::string kv = "\"quant_bits\":" + std::to_string(bits);
        if (in.empty())
            return "{" + kv + "}";
        if (in.find("quant_bits") != std::string::npos)
            return in;
        size_t p = in.find('{');
        if (p == std::string::npos)
            return in;
        std::string out = in;
        size_t q = out.find_first_not_of(" \t\r\n", p + 1);
        if (q != std::string::npos && out[q] == '}')
            out.insert(q, kv);
        else
            out.insert(p + 1, kv + ",");
        return out;
    }

    // Writer
    ziq_writer::ziq_writer(ziq_cfg cfg, std::ofstream &stream) : cfg(cfg), stream(stream)
    {
        std::string annotation = cfg.annotation;
        if (cfg.quant_bits > 0 && cfg.quant_bits < cfg.bits_per_sample)
            annotation = annotate_quant(annotation, cfg.quant_bits);

        // Write header
        stream.write((char *)ZIQ_SIGNATURE, 4);
        stream.write((char *)&cfg.is_compressed, 1);
        stream.write((char *)&cfg.bits_per_sample, 1);
        stream.write((char *)&cfg.samplerate, 8);
        uint64_t string_size = annotation.size();
        stream.write((char *)&string_size, 8);
        stream.write((char *)annotation.c_str(), string_size);

        max_buffer_size = dsp::STREAM_BUFFER_SIZE; // Abolute max size. Show never be reached

        // If compresssed, init compression
        if (cfg.is_compressed)
        {
            zstd_ctx = ZSTD_createCCtx();
            ZSTD_CCtx_setParameter(zstd_ctx, ZSTD_c_compressionLevel, zst_level);
            ZSTD_CCtx_setParameter(zstd_ctx, ZSTD_c_checksumFlag, 1);
            ZSTD_CCtx_setParameter(zstd_ctx, ZSTD_c_nbWorkers, zst_workers);

            // Init buffer
            output_compressed = new uint8_t[max_buffer_size * sizeof(complex_t)];
        }

        if (cfg.bits_per_sample == 8)
            buffer_i8 = new int8_t[max_buffer_size * 2];
        else if (cfg.bits_per_sample == 16)
            buffer_i16 = new int16_t[max_buffer_size * 2];
    }

    void ziq_writer::finish()
    {
        if (!cfg.is_compressed || finished || zstd_ctx == nullptr)
            return;
        finished = true;

        ZSTD_inBuffer in = {nullptr, 0, 0};
        size_t ret = 1;
        while (ret != 0)
        {
            ZSTD_outBuffer out = {output_compressed, max_buffer_size, 0};
            ret = ZSTD_compressStream2(zstd_ctx, &out, &in, ZSTD_e_end);
            if (ZSTD_isError(ret))
            {
                logger->error("ZIQ flush failed: %s", ZSTD_getErrorName(ret));
                break;
            }
            if (out.pos)
                stream.write((char *)output_compressed, out.pos);
        }
        stream.flush();
    }

    ziq_writer::~ziq_writer()
    {
        finish();
        ZSTD_freeCCtx(zstd_ctx);

        if (cfg.is_compressed)
            delete[] output_compressed;

        if (cfg.bits_per_sample == 8)
            delete[] buffer_i8;
        else if (cfg.bits_per_sample == 16)
            delete[] buffer_i16;
    }

    int ziq_writer::compress_and_write(uint8_t *input, size_t size)
    {
        zstd_input = {input, size, 0};
        zstd_output = {output_compressed, max_buffer_size, 0};

        while (zstd_input.pos < zstd_input.size)
            ZSTD_compressStream2(zstd_ctx, &zstd_output, &zstd_input, ZSTD_e_continue);

        stream.write((char *)output_compressed, zstd_output.pos);

        return zstd_output.pos;
    }

    int ziq_writer::write(complex_t *input, int size)
    {
        if (cfg.bits_per_sample == 8)
        {
            volk_32f_s32f_convert_8i(buffer_i8, (float *)input, 127, size * 2);

            if (cfg.quant_bits > 0 && cfg.quant_bits < 8)
            {
                const int drop = 8 - cfg.quant_bits;
                const uint8_t half = (uint8_t)(1 << (drop - 1));
                const uint8_t mask = (uint8_t)(0xFF & ~((1 << drop) - 1));
                for (int i = 0; i < size * 2; i++)
                    buffer_i8[i] = (int8_t)(uint8_t)(((uint8_t)buffer_i8[i] + half) & mask);
            }

            if (cfg.is_compressed)
            {
                return compress_and_write((uint8_t *)buffer_i8, size * 2 * sizeof(int8_t));
            }
            else
            {
                stream.write((char *)buffer_i8, size * 2 * sizeof(int8_t));
                return size * 2 * sizeof(int8_t);
            }
        }
        else if (cfg.bits_per_sample == 16)
        {
            volk_32f_s32f_convert_16i(buffer_i16, (float *)input, 32767, size * 2);

            if (cfg.is_compressed)
            {
                return compress_and_write((uint8_t *)buffer_i16, size * 2 * sizeof(int16_t));
            }
            else
            {
                stream.write((char *)buffer_i16, size * 2 * sizeof(int16_t));
                return size * 2 * sizeof(int16_t);
            }
        }
        else if (cfg.bits_per_sample == 32)
        {
            if (cfg.is_compressed)
            {
                return compress_and_write((uint8_t *)input, size * sizeof(complex_t));
            }
            else
            {
                stream.write((char *)input, size * sizeof(complex_t));
                return size * sizeof(complex_t);
            }
        }

        return 0;
    }

    // Reader
    ziq_reader::ziq_reader(std::ifstream &stream) : stream(stream)
    {
        // Read header
        char signature[4];
        annotation_size = 0;
        stream.read((char *)signature, 4);
        stream.read((char *)&cfg.is_compressed, 1);
        stream.read((char *)&cfg.bits_per_sample, 1);
        stream.read((char *)&cfg.samplerate, 8);
        stream.read((char *)&annotation_size, 8);
        cfg.annotation.resize(annotation_size);
        stream.read((char *)cfg.annotation.c_str(), annotation_size);

        if (std::string(&signature[0], &signature[4]) != ZIQ_SIGNATURE)
        {
            logger->critical("This file is not a valid ZIQ file!");
            isValid = false;
        }

        // If compresssed, init compression
        max_buffer_size = dsp::STREAM_BUFFER_SIZE; // Abolute max size. Show never be reached

        if (cfg.is_compressed)
        {
            zstd_ctx = ZSTD_createDCtx();

            // Init buffer
            output_decompressed = new uint8_t[max_buffer_size * sizeof(complex_t)];
            compressed_buffer = new uint8_t[ZIQ_DECOMPRESS_BUFSIZE];
        }

        if (cfg.bits_per_sample == 8)
            buffer_i8 = new int8_t[max_buffer_size * 2];
        else if (cfg.bits_per_sample == 16)
            buffer_i16 = new int16_t[max_buffer_size * 2];

        decompressed_cnt = 0;

        isValid = true;
    }

    ziq_reader::~ziq_reader()
    {
        ZSTD_freeDCtx(zstd_ctx);

        if (cfg.is_compressed)
        {
            delete[] output_decompressed;
            delete[] compressed_buffer;
        }

        if (cfg.bits_per_sample == 8)
            delete[] buffer_i8;
        else if (cfg.bits_per_sample == 16)
            delete[] buffer_i16;
    }

    bool ziq_reader::seekg(size_t pos)
    {
        // Move to location in file
        if (cfg.is_compressed)
        {
            size_t err;
            decompressed_cnt = 0;
            if (pos + annotation_size + 22 < (size_t)stream.tellg())
            {
                err = ZSTD_DCtx_reset(zstd_ctx, ZSTD_reset_session_only);
                if (ZSTD_isError(err))
                    return false;
                stream.seekg(annotation_size + 22);
            }
            while ((size_t)stream.tellg() < pos + annotation_size + 22)
            {
                stream.read((char *)compressed_buffer, ZIQ_DECOMPRESS_BUFSIZE);
                zstd_input = {compressed_buffer, (unsigned long)ZIQ_DECOMPRESS_BUFSIZE, 0};
                zstd_output = {output_decompressed, max_buffer_size, 0};
                while (zstd_input.pos < zstd_input.size)
                {
                    err = ZSTD_decompressStream(zstd_ctx, &zstd_output, &zstd_input);
                    if (ZSTD_isError(err))
                        return false;
                }
            }
        }
        else
        {
            stream.seekg(pos + annotation_size + 22);
            return true;
        }

        return true;
    }

    int ziq_reader::decompress_at_least(int size)
    {
        while (decompressed_cnt <= size && !stream.eof())
        {
            stream.read((char *)compressed_buffer, ZIQ_DECOMPRESS_BUFSIZE);

            zstd_input = {compressed_buffer, (unsigned long)ZIQ_DECOMPRESS_BUFSIZE, 0};
            zstd_output = {&output_decompressed[decompressed_cnt], (unsigned long)(max_buffer_size - decompressed_cnt), 0};

            while (zstd_input.pos < zstd_input.size)
            {
                size_t err = ZSTD_decompressStream(zstd_ctx, &zstd_output, &zstd_input);
                if (ZSTD_isError(err))
                {
                    ZSTD_DCtx_reset(zstd_ctx, ZSTD_reset_session_only);
                    break;
                }
            }

            decompressed_cnt += zstd_output.pos;
        }

        if (decompressed_cnt < size)
            return 1;

        return 0;
    }

    int ziq_reader::read_decompressed(uint8_t *buffer, int size)
    {
        if (size <= decompressed_cnt)
        {
            memcpy(buffer, output_decompressed, size);

            if (size < decompressed_cnt)
            {
                memmove(output_decompressed, &output_decompressed[size], decompressed_cnt - size);
                decompressed_cnt -= size;
            }
            else
            {
                decompressed_cnt = 0;
            }
            return 0;
        }
        else
        {
            return 1;
        }
    }

    int ziq_reader::read(complex_t *output, int size)
    {
        if (!isValid)
            return 1;

        if (cfg.bits_per_sample == 8)
        {
            if (cfg.is_compressed)
            {
                decompress_at_least(size * 2 * sizeof(int8_t));
                read_decompressed((uint8_t *)buffer_i8, size * 2 * sizeof(int8_t));
            }
            else
            {
                stream.read((char *)buffer_i8, size * 2 * sizeof(int8_t));
            }

            volk_8i_s32f_convert_32f_u((float *)output, (const int8_t *)buffer_i8, 127, size * 2);
        }
        else if (cfg.bits_per_sample == 16)
        {
            if (cfg.is_compressed)
            {
                decompress_at_least(size * 2 * sizeof(int16_t));
                read_decompressed((uint8_t *)buffer_i16, size * 2 * sizeof(int16_t));
            }
            else
            {
                stream.read((char *)buffer_i16, size * 2 * sizeof(int16_t));
            }

            volk_16i_s32f_convert_32f_u((float *)output, (const int16_t *)buffer_i16, 32767, size * 2);
        }
        else if (cfg.bits_per_sample == 32)
        {
            if (cfg.is_compressed)
            {
                // return compress_and_write((uint8_t *)input, size * sizeof(complex_t));
                decompress_at_least(size * sizeof(complex_t));
                read_decompressed((uint8_t *)output, size * sizeof(complex_t));
            }
            else
            {
                stream.read((char *)output, size * sizeof(complex_t));
            }
        }

        return 0;
    }

    // Util functions
    bool isValidZIQ(std::string file)
    {
        std::ifstream stream(file, std::ios::binary);
        char signature[4];
        stream.read((char *)signature, 4);
        stream.close();

        return std::string(&signature[0], &signature[4]) == ZIQ_SIGNATURE;
    }

    ziq_cfg getCfgFromFile(std::string file)
    {
        ziq_cfg cfg;

        std::ifstream stream(file, std::ios::binary);

        char signature[4];
        stream.read((char *)signature, 4);
        stream.read((char *)&cfg.is_compressed, 1);
        stream.read((char *)&cfg.bits_per_sample, 1);
        stream.read((char *)&cfg.samplerate, 8);
        uint64_t string_size = 0;
        stream.read((char *)&string_size, 8);
        cfg.annotation.resize(string_size);
        stream.read((char *)cfg.annotation.c_str(), string_size);

        stream.close();

        return cfg;
    }
};
#endif