#pragma once

#include "common/dsp/complex.h"
#include "common/dsp/utils/random.h"
#include "imgui/imgui.h"
#include <mutex>

namespace satdump
{
    namespace widgets
    {
#define CONST_SIZE 2048

        class ConstellationViewer
        {
        private:
            complex_t sample_buffer_complex_float[CONST_SIZE];
            const int d_constellation_size;
            dsp::Random rng;
            std::mutex mtx;

        public:
            float d_hscale, d_vscale;

        public:
            ConstellationViewer(float hscale = 1, float vscale = 1, int constellation_size = 200);
            ~ConstellationViewer();
            void pushComplexScaled(complex_t *buffer, int size, float scale);
            void pushComplex(complex_t *buffer, int size);
            void pushFloatAndGaussian(float *buffer, int size);
            void pushSofttAndGaussian(int8_t *buffer, float scale, int size);
            void draw();
        };

        // Plots int8 soft symbols in rect_min..rect_max; components is 1 (BPSK) or 2 (I/Q).
        void drawSoftConstellation(ImDrawList *draw_list, const ImVec2 &rect_min, const ImVec2 &rect_max,
                                   const int8_t *soft, int num_symbols, int components,
                                   dsp::Random *rng = nullptr, float fill = 0.8f, float dot_radius = 2.0f);
    } // namespace widgets
} // namespace satdump