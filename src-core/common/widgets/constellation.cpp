#include "constellation.h"
#include "common/dsp/block.h"
#include "core/style.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace satdump
{
    namespace widgets
    {
        ConstellationViewer::ConstellationViewer(float hscale, float vscale, int constellation_size) : d_constellation_size(constellation_size), d_hscale(hscale), d_vscale(vscale) {}

        ConstellationViewer::~ConstellationViewer() {}

        void ConstellationViewer::pushComplexScaled(complex_t *buffer, int size, float scale)
        {
            mtx.lock();
            int to_copy = std::min<int>(CONST_SIZE, size);
            int to_move = size >= CONST_SIZE ? 0 : CONST_SIZE - size;

            if (to_move > 0)
                std::memmove(&sample_buffer_complex_float[size], sample_buffer_complex_float, to_move * sizeof(complex_t));

            for (int i = 0; i < to_copy; i++)
                sample_buffer_complex_float[i] = buffer[i] * scale;
            mtx.unlock();
        }

        void ConstellationViewer::pushComplex(complex_t *buffer, int size)
        {
            mtx.lock();
            int to_copy = std::min<int>(CONST_SIZE, size);
            int to_move = size >= CONST_SIZE ? 0 : CONST_SIZE - size;

            if (to_move > 0)
                std::memmove(&sample_buffer_complex_float[size], sample_buffer_complex_float, to_move * sizeof(complex_t));

            std::memcpy(sample_buffer_complex_float, buffer, to_copy * sizeof(complex_t));
            mtx.unlock();
        }

        void ConstellationViewer::pushFloatAndGaussian(float *buffer, int size)
        {
            mtx.lock();
            int to_copy = std::min<int>(CONST_SIZE, size);
            int to_move = size >= CONST_SIZE ? 0 : CONST_SIZE - size;

            if (to_move > 0)
                std::memmove(&sample_buffer_complex_float[size], sample_buffer_complex_float, to_move * sizeof(complex_t));

            for (int i = 0; i < to_copy; i++)
                sample_buffer_complex_float[i] = complex_t(buffer[i], rng.gasdev());
            mtx.unlock();
        }

        void ConstellationViewer::pushSofttAndGaussian(int8_t *buffer, float scale, int size)
        {
            mtx.lock();
            int to_copy = std::min<int>(CONST_SIZE, size);
            int to_move = size >= CONST_SIZE ? 0 : CONST_SIZE - size;

            if (to_move > 0)
                std::memmove(&sample_buffer_complex_float[size], sample_buffer_complex_float, to_move * sizeof(complex_t));

            for (int i = 0; i < to_copy; i++)
                sample_buffer_complex_float[i] = complex_t(buffer[i] / scale, rng.gasdev());
            mtx.unlock();
        }

        void ConstellationViewer::draw()
        {
            mtx.lock();
            ImDrawList *draw_list = ImGui::GetWindowDrawList();
            ImVec2 rect_min = ImGui::GetCursorScreenPos();
            ImVec2 rect_max = {rect_min.x + d_constellation_size * ui_scale, rect_min.y + d_constellation_size * ui_scale};
            draw_list->AddRectFilled(rect_min, rect_max, style::theme.widget_bg);
            draw_list->PushClipRect(rect_min, rect_max);

            for (int i = 0; i < CONST_SIZE; i++)
            {
                draw_list->AddCircleFilled(
                    ImVec2(ImGui::GetCursorScreenPos().x + ((d_constellation_size / 2) * ui_scale + sample_buffer_complex_float[i].real * (d_constellation_size / 2) * d_hscale * ui_scale),
                           ImGui::GetCursorScreenPos().y + ((d_constellation_size / 2) * ui_scale + sample_buffer_complex_float[i].imag * (d_constellation_size / 2) * d_vscale * ui_scale)),
                    2 * ui_scale * (d_constellation_size / 200.0f), style::theme.constellation);
            }

            draw_list->PopClipRect();
            ImGui::Dummy(ImVec2(d_constellation_size * ui_scale + 3, d_constellation_size * ui_scale + 3));
            mtx.unlock();
        }

        namespace
        {
            constexpr float SOFT_SIGMA_FALLBACK = 6.0f;

            float estimatePeakSigma(const int8_t *soft, int num_symbols)
            {
                float max_a = 0.0f;
                for (int i = 0; i < num_symbols; i++)
                {
                    float a = std::fabs((float)soft[i]);
                    if (a > max_a)
                        max_a = a;
                }

                if (max_a < 1.0f)
                    return SOFT_SIGMA_FALLBACK;

                const float threshold = max_a * 0.45f;

                int count = 0;
                float mean = 0.0f, m2 = 0.0f;
                for (int i = 0; i < num_symbols; i++)
                {
                    float a = std::fabs((float)soft[i]);
                    if (a <= threshold)
                        continue;

                    count++;
                    float delta = a - mean;
                    mean += delta / (float)count;
                    m2 += delta * (a - mean);
                }

                if (count < 16)
                    return SOFT_SIGMA_FALLBACK;

                return std::sqrt(m2 / (float)count);
            }
        } // namespace

        void drawSoftConstellation(ImDrawList *draw_list, const ImVec2 &rect_min, const ImVec2 &rect_max,
                                   const int8_t *soft, int num_symbols, int components,
                                   dsp::Random *rng, float fill, float dot_radius)
        {
            if (draw_list == nullptr || soft == nullptr || num_symbols <= 0 || components < 1)
                return;

            static dsp::Random fallback_rng; // only the UI thread plots
            dsp::Random &r = rng != nullptr ? *rng : fallback_rng;

            float half_w = (rect_max.x - rect_min.x) * 0.5f;
            float half_h = (rect_max.y - rect_min.y) * 0.5f;
            if (half_w <= 0.0f || half_h <= 0.0f)
                return;

            float center_x = rect_min.x + half_w;
            float center_y = rect_min.y + half_h;
            float scale = std::min(half_w, half_h) * fill / 127.0f;

            float sigma_y = 0.0f;
            if (components == 1)
            {
                float sigma_soft = estimatePeakSigma(soft, num_symbols);
                sigma_y = std::clamp(sigma_soft * scale, half_h * 0.02f, half_h * 0.2f);
            }

            for (int i = 0; i < num_symbols; i++)
            {
                if (components == 1)
                {
                    int8_t val = soft[i];
                    if (val == 0)
                        continue;

                    float px = center_x + ((float)val + r.ran1() - 0.5f) * scale;
                    float py = center_y + r.gasdev() * sigma_y;
                    draw_list->AddCircleFilled(ImVec2(px, py), dot_radius, style::theme.constellation);
                }
                else
                {
                    int8_t val_i = soft[i * 2 + 0];
                    int8_t val_q = soft[i * 2 + 1];
                    if (val_i == 0 && val_q == 0)
                        continue;

                    float px = center_x + ((float)val_i + r.ran1() - 0.5f) * scale;
                    float py = center_y + ((float)val_q + r.ran1() - 0.5f) * scale;
                    draw_list->AddCircleFilled(ImVec2(px, py), dot_radius, style::theme.constellation);
                }
            }
        }
    } // namespace widgets
} // namespace satdump