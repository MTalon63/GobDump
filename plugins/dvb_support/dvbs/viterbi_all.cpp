#include "viterbi_all.h"
#include <cstring>
#include "common/codings/viterbi/utils.h"

#define ST_IDLE 0
#define ST_SYNCED 1

namespace viterbi
{
    // Fused rotate_soft + signed_soft_to_unsigned for the SYNCED path.
    // -128 is clamped to -127 and 128 becomes 127 (the erasure marker).
    static inline void rotate_convert_fused(int8_t *in, uint8_t *out, int size, phase_t phase)
    {
        switch (phase)
        {
        case PHASE_90:
            for (int i = 0; i + 1 < size; i += 2)
            {
                float a = in[i], b = in[i + 1];
                if (a == -128) a = -127;
                if (b == -128) b = -127;
                int8_t r = (int8_t)b, im = (int8_t)(-a);
                in[i] = r;
                in[i + 1] = im;
                uint8_t o0 = (uint8_t)(r + 127);
                uint8_t o1 = (uint8_t)(im + 127);
                out[i] = (o0 == 128) ? 127 : o0;
                out[i + 1] = (o1 == 128) ? 127 : o1;
            }
            if (size & 1)
            {
                int a = in[size - 1];
                if (a == -128) a = -127;
                uint8_t o = (uint8_t)(a + 127);
                out[size - 1] = (o == 128) ? 127 : o;
            }
            break;
        case PHASE_180:
            for (int i = 0; i < size; i++)
            {
                int a = in[i];
                if (a == -128) a = -127;
                int8_t v = (int8_t)(-a);
                in[i] = v;
                uint8_t o = (uint8_t)(v + 127);
                out[i] = (o == 128) ? 127 : o;
            }
            break;
        case PHASE_270:
            for (int i = 0; i + 1 < size; i += 2)
            {
                float a = in[i], b = in[i + 1];
                if (a == -128) a = -127;
                if (b == -128) b = -127;
                int8_t r = (int8_t)(-b), im = (int8_t)a;
                in[i] = r;
                in[i + 1] = im;
                uint8_t o0 = (uint8_t)(r + 127);
                uint8_t o1 = (uint8_t)(im + 127);
                out[i] = (o0 == 128) ? 127 : o0;
                out[i + 1] = (o1 == 128) ? 127 : o1;
            }
            if (size & 1)
            {
                int a = in[size - 1];
                if (a == -128) a = -127;
                uint8_t o = (uint8_t)(a + 127);
                out[size - 1] = (o == 128) ? 127 : o;
            }
            break;
        default: // PHASE_0
            for (int i = 0; i < size; i++)
            {
                int a = in[i];
                if (a == -128) a = -127;
                in[i] = (int8_t)a;
                uint8_t o = (uint8_t)(a + 127);
                out[i] = (o == 128) ? 127 : o;
            }
            break;
        }
    }

    Viterbi_DVBS::Viterbi_DVBS(float ber_threshold, int max_outsync, int buffer_size, std::vector<phase_t> phases)
        : d_ber_thresold(ber_threshold),
          d_max_outsync(max_outsync),
          d_buffer_size(buffer_size),
          d_phases_to_check(phases),
          d_state(ST_IDLE),

          cc_decoder_ber_12(TEST_BITS_LENGTH / 2, 7, 2, {79, 109}),
          cc_encoder_ber_12(TEST_BITS_LENGTH / 2, 7, 2, {79, 109}),
          cc_decoder_ber_23(TEST_BITS_LENGTH * 1.334 / 2, 7, 2, {79, 109}),
          cc_encoder_ber_23(TEST_BITS_LENGTH * 1.334 / 2, 7, 2, {79, 109}),
          cc_decoder_ber_34(TEST_BITS_LENGTH * 1.5 / 2, 7, 2, {79, 109}),
          cc_encoder_ber_34(TEST_BITS_LENGTH * 1.5 / 2, 7, 2, {79, 109}),
          cc_decoder_ber_56(TEST_BITS_LENGTH * 1.66 / 2, 7, 2, {79, 109}),
          cc_encoder_ber_56(TEST_BITS_LENGTH * 1.66 / 2, 7, 2, {79, 109}),
          cc_decoder_ber_78(TEST_BITS_LENGTH * 1.75 / 2, 7, 2, {79, 109}),
          cc_encoder_ber_78(TEST_BITS_LENGTH * 1.75 / 2, 7, 2, {79, 109}),

          vit_bufsize_23(ensureIs2Multiple(buffer_size * 1.33)),
          vit_bufsize_56(ensureIs2Multiple(buffer_size * 1.66)),

          cc_decoder_12(buffer_size / 2, 7, 2, {79, 109}),
          cc_decoder_23(vit_bufsize_23 / 2, 7, 2, {79, 109}),
          cc_decoder_34(buffer_size * 1.5 / 2, 7, 2, {79, 109}),
          cc_decoder_56(vit_bufsize_56 / 2, 7, 2, {79, 109}),
          cc_decoder_78(buffer_size * 1.75 / 2, 7, 2, {79, 109}),

          vit_buffer_23(vit_bufsize_23 * 4),
          vit_buffer_56(vit_bufsize_56 * 4)

    {
        soft_buffer = new uint8_t[d_buffer_size * 4];
        depunc_buffer = new uint8_t[d_buffer_size * 4];

        for (int i = 0; i < 12; i++)
        {
            for (int y = 0; y < 4; y++)
            {
                d_bers_12[y][i] = 10;
                d_bers_23[y][i] = 10;
                d_bers_34[y][i] = 10;
                d_bers_56[y][i] = 10;
                d_bers_78[y][i] = 10;
            }
        }
    }

    Viterbi_DVBS::~Viterbi_DVBS()
    {
        delete[] soft_buffer;
        delete[] depunc_buffer;
    }

    float Viterbi_DVBS::get_ber(uint8_t *raw, uint8_t *rencoded, int len, float ratio)
    {
        float errors = 0, total = 0;
        for (int i = 0; i < len; i++)
        {
            if (raw[i] != 128)
            {
                errors += (raw[i] > 127) != rencoded[i];
                total++;
            }
        }

        return (errors / total) * ratio;
    }

    int Viterbi_DVBS::work(int8_t *input, int size, uint8_t *output)
    {
        if (d_state == ST_IDLE) // Search for a lock
        {
            d_ber = 10;
            for (phase_t phase : d_phases_to_check)
            {
                memcpy(ber_test_buffer, input, TEST_BITS_LENGTH);                            // Copy over small buffer
                rotate_soft(ber_test_buffer, TEST_BITS_LENGTH, phase, false);                // Phase shift
                signed_soft_to_unsigned(ber_test_buffer, ber_soft_buffer, TEST_BITS_LENGTH); // Convert to softs for the viterbi

                // Rate 1/2
                for (int shift = 0; shift < 2; shift++) // Test 2 puncturing shifts
                {
                    cc_decoder_ber_12.work(ber_soft_buffer + shift, ber_decoded_buffer); // Decode....
                    cc_encoder_ber_12.work(ber_decoded_buffer, ber_encoded_buffer);      // ....then reencode for comparison

                    d_bers_12[phase][shift] = get_ber(ber_soft_buffer + shift, ber_encoded_buffer, TEST_BITS_LENGTH, 2.5); // Compute BER between initial buffer and re-encoded

                    if (d_bers_12[phase][shift] < d_ber_thresold) // Check for a lock
                    {
                        d_ber = d_bers_12[phase][shift]; // Set current BER
                        d_state = ST_SYNCED;             // Set the decoder state to SYNCED so we start decoding
                        d_phase = (phase_t)phase;        // Set current phase
                        d_shift = shift;                 // Set current puncturing shift
                        d_invalid = 0;                   // Reset invalid BER count
                        d_rate = RATE_1_2;               // Set rate

                        memset(soft_buffer, 128, d_buffer_size * 4);
                        memset(depunc_buffer, 128, d_buffer_size * 4);
                    }
                }

                // Rate 2/3
                for (int shift = 0; shift < 6; shift++) // Test 3 puncturing shifts
                {
                    depunc_23.depunc_static(ber_soft_buffer, ber_depunc_buffer, TEST_BITS_LENGTH, shift); // Depuncture

                    cc_decoder_ber_23.work(ber_depunc_buffer, ber_decoded_buffer);  // Decode....
                    cc_encoder_ber_23.work(ber_decoded_buffer, ber_encoded_buffer); // ....then reencode for comparison

                    d_bers_23[phase][shift] = get_ber(ber_depunc_buffer, ber_encoded_buffer, TEST_BITS_LENGTH * 1.25, 3.5); // Compute BER between initial buffer and re-encoded

                    if (d_bers_23[phase][shift] < d_ber_thresold) // Check for a lock
                    {
                        d_ber = d_bers_23[phase][shift]; // Set current BER
                        d_state = ST_SYNCED;             // Set the decoder state to SYNCED so we start decoding
                        d_phase = (phase_t)phase;        // Set current phase
                        d_shift = shift;                 // Set current puncturing shift
                        d_invalid = 0;                   // Reset invalid BER count
                        d_rate = RATE_2_3;               // Set rate
                        depunc_23.set_shift(d_shift);

                        memset(soft_buffer, 128, d_buffer_size * 4);
                        memset(depunc_buffer, 128, d_buffer_size * 4);
                    }
                }

                // Rate 3/4
                for (int shift = 0; shift < 2; shift++) // Test 2 puncturing shifts
                {
                    depuncture_34(ber_soft_buffer, ber_depunc_buffer, TEST_BITS_LENGTH, shift); // Depuncture

                    cc_decoder_ber_34.work(ber_depunc_buffer, ber_decoded_buffer);  // Decode....
                    cc_encoder_ber_34.work(ber_decoded_buffer, ber_encoded_buffer); // ....then reencode for comparison

                    d_bers_34[phase][shift] = get_ber(ber_depunc_buffer, ber_encoded_buffer, TEST_BITS_LENGTH * 1.5, 5); // Compute BER between initial buffer and re-encoded

                    if (d_bers_34[phase][shift] < d_ber_thresold) // Check for a lock
                    {
                        d_ber = d_bers_34[phase][shift]; // Set current BER
                        d_state = ST_SYNCED;             // Set the decoder state to SYNCED so we start decoding
                        d_phase = (phase_t)phase;        // Set current phase
                        d_shift = shift;                 // Set current puncturing shift
                        d_invalid = 0;                   // Reset invalid BER count
                        d_rate = RATE_3_4;               // Set rate

                        memset(soft_buffer, 128, d_buffer_size * 4);
                        memset(depunc_buffer, 128, d_buffer_size * 4);
                    }
                }

                // Rate 5/6
                for (int shift = 0; shift < 12; shift++) // Test 3 puncturing shifts
                {
                    depunc_56.depunc_static(ber_soft_buffer, ber_depunc_buffer, TEST_BITS_LENGTH, shift); // Depuncture

                    cc_decoder_ber_56.work(ber_depunc_buffer, ber_decoded_buffer);  // Decode....
                    cc_encoder_ber_56.work(ber_decoded_buffer, ber_encoded_buffer); // ....then reencode for comparison

                    d_bers_56[phase][shift] = get_ber(ber_depunc_buffer, ber_encoded_buffer, TEST_BITS_LENGTH * 1.66, 8); // Compute BER between initial buffer and re-encoded

                    if (d_bers_56[phase][shift] < d_ber_thresold) // Check for a lock
                    {
                        d_ber = d_bers_56[phase][shift]; // Set current BER
                        d_state = ST_SYNCED;             // Set the decoder state to SYNCED so we start decoding
                        d_phase = (phase_t)phase;        // Set current phase
                        d_shift = shift;                 // Set current puncturing shift
                        d_invalid = 0;                   // Reset invalid BER count
                        d_rate = RATE_5_6;               // Set rate
                        depunc_56.set_shift(d_shift);

                        memset(soft_buffer, 128, d_buffer_size * 4);
                        memset(depunc_buffer, 128, d_buffer_size * 4);
                    }
                }

                // Rate 7/8
                for (int shift = 0; shift < 4; shift++) // Test 3 puncturing shifts
                {
                    depuncture_78(ber_soft_buffer, ber_depunc_buffer, TEST_BITS_LENGTH, shift); // Depuncture

                    cc_decoder_ber_78.work(ber_depunc_buffer, ber_decoded_buffer);  // Decode....
                    cc_encoder_ber_78.work(ber_decoded_buffer, ber_encoded_buffer); // ....then reencode for comparison

                    d_bers_78[phase][shift] = get_ber(ber_depunc_buffer, ber_encoded_buffer, TEST_BITS_LENGTH * 1.75, 10); // Compute BER between initial buffer and re-encoded

                    if (d_bers_78[phase][shift] < d_ber_thresold) // Check for a lock
                    {
                        d_ber = d_bers_78[phase][shift]; // Set current BER
                        d_state = ST_SYNCED;             // Set the decoder state to SYNCED so we start decoding
                        d_phase = (phase_t)phase;        // Set current phase
                        d_shift = shift;                 // Set current puncturing shift
                        d_invalid = 0;                   // Reset invalid BER count
                        d_rate = RATE_7_8;               // Set rate

                        memset(soft_buffer, 128, d_buffer_size * 4);
                        memset(depunc_buffer, 128, d_buffer_size * 4);
                    }
                }
            }
        }

        int out_n = 0; // Output bytes count

        if (d_state == ST_SYNCED) // Decode
        {
            rotate_convert_fused((int8_t *)input, soft_buffer, size, d_phase);

            if (d_rate == RATE_1_2)
            {
                cc_decoder_12.work(soft_buffer + d_shift, output, size); // Decode entire buffer
                out_n = size / 2;

                cc_encoder_ber_12.work(output, ber_encoded_buffer);                                // Re-encoded for a BER check
                d_ber = get_ber(soft_buffer + d_shift, ber_encoded_buffer, TEST_BITS_LENGTH, 2.5); // Compute BER
            }
            else if (d_rate == RATE_2_3)
            {
                int sz = depunc_23.depunc_cont(soft_buffer, depunc_buffer, size); // Depuncturing

                vit_buffer_23.add(depunc_buffer, sz);

                out_n = 0;
                while (vit_buffer_23.in_buffer > vit_bufsize_23)
                {
                    cc_decoder_23.work(vit_buffer_23.buffer_ptr, output + out_n); // Decode entire buffer

                    cc_encoder_ber_23.work(output + out_n, ber_encoded_buffer);                                  // Re-encoded for a BER check
                    d_ber = get_ber(vit_buffer_23.buffer_ptr, ber_encoded_buffer, TEST_BITS_LENGTH * 1.25, 3.5); // Compute BER

                    out_n += vit_bufsize_23 / 2;
                    vit_buffer_23.del(vit_bufsize_23);
                }
            }
            else if (d_rate == RATE_3_4)
            {
                int sz = depuncture_34(soft_buffer, depunc_buffer, size, d_shift); // Depuncturing

                cc_decoder_34.work(depunc_buffer, output, sz); // Decode entire buffer
                out_n = sz / 2;

                cc_encoder_ber_34.work(output, ber_encoded_buffer);                            // Re-encoded for a BER check
                d_ber = get_ber(depunc_buffer, ber_encoded_buffer, TEST_BITS_LENGTH * 1.5, 5); // Compute BER
            }
            else if (d_rate == RATE_5_6)
            {
                int sz = depunc_56.depunc_cont(soft_buffer, depunc_buffer, size); // Depuncturing

                vit_buffer_56.add(depunc_buffer, sz);

                out_n = 0;
                while (vit_buffer_56.in_buffer > vit_bufsize_56)
                {
                    cc_decoder_56.work(vit_buffer_56.buffer_ptr, output + out_n); // Decode entire buffer

                    cc_encoder_ber_56.work(output + out_n, ber_encoded_buffer);                                // Re-encoded for a BER check
                    d_ber = get_ber(vit_buffer_56.buffer_ptr, ber_encoded_buffer, TEST_BITS_LENGTH * 1.66, 8); // Compute BER

                    out_n += vit_bufsize_56 / 2;
                    vit_buffer_56.del(vit_bufsize_56);
                }
            }
            else if (d_rate == RATE_7_8)
            {
                int sz = depuncture_78(soft_buffer, depunc_buffer, size, d_shift); // Depuncturing

                cc_decoder_78.work(depunc_buffer, output, sz); // Decode entire buffer
                out_n = sz / 2;

                cc_encoder_ber_78.work(output, ber_encoded_buffer);                              // Re-encoded for a BER check
                d_ber = get_ber(depunc_buffer, ber_encoded_buffer, TEST_BITS_LENGTH * 1.75, 10); // Compute BER
            }

            if (d_ber > d_ber_thresold) // Check current BER
            {
                d_invalid++;
                if (d_invalid > d_max_outsync) // If we get over out max unsynced thresold...
                    d_state = ST_IDLE;         // ...reset the decoder
            }
            else
            {
                d_invalid = 0; // Otherwise, reset current count
            }
        }

        return out_n;
    }

    float Viterbi_DVBS::ber()
    {
        if (d_state == ST_SYNCED)
            return d_ber;
        else
        {
            float ber = 10;

            for (phase_t phase : d_phases_to_check)
                for (int o = 0; o < 2; o++)
                    if (ber > d_bers_12[phase][o])
                        ber = d_bers_12[phase][o];

            for (phase_t phase : d_phases_to_check)
                for (int o = 0; o < 6; o++)
                    if (ber > d_bers_23[phase][o])
                        ber = d_bers_23[phase][o];

            for (phase_t phase : d_phases_to_check)
                for (int o = 0; o < 2; o++)
                    if (ber > d_bers_34[phase][o])
                        ber = d_bers_34[phase][o];

            for (phase_t phase : d_phases_to_check)
                for (int o = 0; o < 12; o++)
                    if (ber > d_bers_56[phase][o])
                        ber = d_bers_56[phase][o];

            for (phase_t phase : d_phases_to_check)
                for (int o = 0; o < 4; o++)
                    if (ber > d_bers_78[phase][o])
                        ber = d_bers_78[phase][o];

            return ber;
        }
    }

    int Viterbi_DVBS::getState()
    {
        return d_state;
    }
}
