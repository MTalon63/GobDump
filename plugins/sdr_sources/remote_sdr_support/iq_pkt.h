#pragma once

#include <cstdint>
#include "common/dsp/buffer.h"
#include <volk/volk.h>
#include "common/dsp/complex.h"

// Copied from ZIQ2 for now... Expected to change...
namespace remote_sdr
{
    inline int encode_iq_pkt(uint8_t *output, complex_t *input, float *mag_buf, int nsamples, int bit_depth)
    {
        uint32_t max_index = 0;
        float scale = 0;
        if (bit_depth != 32)
        {
            volk_32fc_magnitude_32f(mag_buf, (lv_32fc_t *)input, nsamples);
            volk_32f_index_max_32u(&max_index, mag_buf, nsamples);

            if (bit_depth == 8)
                scale = 127.0f / mag_buf[max_index];
            else if (bit_depth == 16)
                scale = 32767.0f / mag_buf[max_index];
        }

        output[0] = bit_depth;
        memcpy(output + 1, &scale, sizeof(scale));
        memcpy(output + 5, &nsamples, sizeof(nsamples));

        int final_size = 9;

        if (bit_depth == 8)
        {
            volk_32f_s32f_convert_8i((int8_t *)&output[final_size], (float *)input, scale, nsamples * 2);
            final_size += nsamples * sizeof(int8_t) * 2;
        }
        else if (bit_depth == 16)
        {
            volk_32f_s32f_convert_16i((int16_t *)&output[final_size], (float *)input, scale, nsamples * 2);
            final_size += nsamples * sizeof(int16_t) * 2;
        }
        else if (bit_depth == 32)
        {
            memcpy(&output[final_size], input, nsamples * sizeof(complex_t));
            final_size += nsamples * sizeof(complex_t);
        }

        // printf("Final Size %d\n", final_size);

        return final_size;
    }

    inline bool decode_iq_pkt(const uint8_t *input, size_t input_len, complex_t *output,
                              size_t output_capacity, int *nsamples)
    {
        constexpr size_t header_size = 9;
        if (input == nullptr || output == nullptr || nsamples == nullptr || input_len < header_size)
            return false;

        int bit_depth = input[0];
        float scale = 0.0f;
        int decoded_samples = 0;
        memcpy(&scale, input + 1, sizeof(scale));
        memcpy(&decoded_samples, input + 5, sizeof(decoded_samples));

        if (decoded_samples <= 0 || static_cast<size_t>(decoded_samples) > output_capacity)
            return false;

        size_t bytes_per_sample = 0;
        if (bit_depth == 8)
            bytes_per_sample = 2 * sizeof(int8_t);
        else if (bit_depth == 16)
            bytes_per_sample = 2 * sizeof(int16_t);
        else if (bit_depth == 32)
            bytes_per_sample = sizeof(complex_t);
        else
            return false;

        const size_t sample_count = static_cast<size_t>(decoded_samples);
        if (sample_count > (SIZE_MAX - header_size) / bytes_per_sample)
            return false;
        if (header_size + sample_count * bytes_per_sample > input_len)
            return false;

        if (bit_depth == 8)
            volk_8i_s32f_convert_32f((float *)output, (const int8_t *)&input[header_size], scale, decoded_samples * 2);
        else if (bit_depth == 16)
            volk_16i_s32f_convert_32f((float *)output, (const int16_t *)&input[header_size], scale, decoded_samples * 2);
        else
            memcpy(output, &input[header_size], sample_count * sizeof(complex_t));

        *nsamples = decoded_samples;
        return true;
    }
};
