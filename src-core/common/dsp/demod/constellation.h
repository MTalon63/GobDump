#pragma once

#include "common/dsp/complex.h"
#include <vector>
#include <cstdint>
#include <memory>

namespace dsp
{
    enum constellation_type_t
    {
        BPSK,
        QPSK,
        OQPSK,
        PSK8,
        APSK16,
        APSK32,
        // CCSDS 131.2-B-2 / DVB-S2 16APSK, average energy 1 (see the constructor).
        APSK16_CCSDS
    };

    // Constellation parser; expects symbols around 1 and -1.
    class constellation_t
    {
    protected:
        const constellation_type_t const_type; // Constellation type
        int const_bits;                        // Number of bits transmitted per sample
        int const_states;                      // Number of possible states
        complex_t *constellation;              // LUT used for modulation
        float const_amp = 1.0f;                // Required for APSK
        float const_sca = 50.0f;               // Const scale for soft symbols
        float const_prescale = 1.0f;           // Pre-scaling for input symbols

        complex_t polar(float r, int n, float i);
        int8_t clamp(float x);

    public:
        // Rebuilt by make_lut() (called from the demod thread when tracking
        // noise power) while DSP block threads read it via demod_soft_lut().
        // The table is therefore immutable after construction and swapped
        // out atomically (copy-on-write), so readers can never observe a
        // half-built or already-destroyed table.
        struct SoftLUT
        {
            int resolution;
            int bits;
            std::vector<float> phase_error_lut;
            std::vector<int8_t> bits_lut;
        };
        std::shared_ptr<const SoftLUT> lut_ptr;

    public:
        void make_lut(int resolution, float npwr = 1.0f);
        void set_noise_power(float npwr);
        float get_scale_factor() const { return const_amp * const_prescale; }
        // int8 LLR scale; the 16APSK path adapts it to the measured noise.
        void set_scale(float s) { const_sca = s; }
        float get_scale() const { return const_sca; }
        void demod_soft_lut(complex_t sample, int8_t *bits, float *phase_error = nullptr);

        struct SoftLutView
        {
            std::shared_ptr<const SoftLUT> keep;
            int resolution = 0;
            int bits = 0;
            float scale = 1.0f;
            float bias = 0.0f;
            const float *phase_error_lut = nullptr;
            const int8_t *bits_lut = nullptr;
            bool valid() const { return resolution > 0; }
        };

        SoftLutView acquire_lut_view()
        {
            SoftLutView v;
            v.keep = std::atomic_load(&lut_ptr);
            if (v.keep)
            {
                v.resolution = v.keep->resolution;
                v.bits = v.keep->bits;
                v.scale = (float)v.keep->resolution / 1.5f;
                v.bias = (float)(v.keep->resolution / 2);
                v.phase_error_lut = v.keep->phase_error_lut.data();
                v.bits_lut = v.keep->bits_lut.data();
            }
            return v;
        }

        void demod_soft_lut_view(const SoftLutView &v, complex_t sample, int8_t *bits, float *phase_error = nullptr)
        {
            if (const_bits == 5 || !v.valid())
            {
                demod_soft_calc(sample, bits, phase_error);
                return;
            }
            int x = (int)(sample.real * v.scale + v.bias);
            if (x < 0)
                x = 0;
            else if (x >= v.resolution)
                x = v.resolution - 1;
            int y = (int)(sample.imag * v.scale + v.bias);
            if (y < 0)
                y = 0;
            else if (y >= v.resolution)
                y = v.resolution - 1;
            size_t idx = (size_t)x * v.resolution + y;
            if (bits != nullptr)
            {
                const int8_t *src = &v.bits_lut[idx * v.bits];
                for (int i = 0; i < v.bits; i++)
                    bits[i] = src[i];
            }
            if (phase_error != nullptr)
                *phase_error = v.phase_error_lut[idx];
        }

    protected:
        float current_npwr = 1.0f;

    public:
        constellation_t(constellation_type_t type, float g1 = 0, float g2 = 0);
        ~constellation_t();

        complex_t mod(uint8_t symbol);                                                                        // Modulate a raw symbol. Proper resampling and RRC filtering is still required!
        uint8_t demod(complex_t sample);                                                                      // Demodulate a complex sample to hard bits

        // Nearest-point helpers for decision-directed loops; same scaling as demod_soft_calc().
        int nearest_index(complex_t sample) const;                 // Index (== label) of the nearest point
        complex_t nearest(complex_t sample) const;                 // The nearest point itself
        complex_t nearest_scaled(complex_t sample, float amp) const; // amp * nearest(sample / amp)

        // Raw point table for external EVM/SNR estimators.
        const complex_t *get_points() const { return constellation; }
        int get_states() const { return const_states; }
        uint8_t soft_demod(int8_t *sample);                                                                   // Demodulate a complex sample stored as soft (I/Q) bits to hard bits
        void soft_demod(int8_t *samples, int size, uint8_t *bits);                                            // Demodulate a full buffer of softs
        void demod_soft_calc(complex_t sample, int8_t *bits, float *phase_error = nullptr, float npwr = 1.0); // Demodulate to soft symbols

        int getBitsCnt() { return const_bits; }
    };
};
