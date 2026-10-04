#pragma once

#include "imgui/implot/implot.h"
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace satdump
{
    class BitContainer
    {
    private:
        const std::string d_name;
        const std::string d_filepath;
        size_t d_chunk_size = 512;
        size_t d_file_memory_size;
        uint8_t *d_file_memory_ptr;
        int fd;
        char unique_id[16] = {0};

    public:
        struct HighlightDef
        {
            size_t ptr;
            size_t size;
            uint8_t r, g, b;
        };

        std::vector<HighlightDef> highlights;

    private:
        struct PartImage
        {
            unsigned int image_id = 0;
            double pos1_x = 0;
            double pos1_y = 0;
            double pos2_x = 0;
            double pos2_y = 0;
            int i = -1;
            bool visible = false;

            bool need_update = true;

            std::vector<HighlightDef> highlights;
        };

        size_t img_parts_y = 0;
        size_t img_parts_x = 0;
        std::vector<PartImage> image_parts;

        // init_display() rebuilds image_parts and is called both from the UI thread (any display
        // setting) and from a worker (Find Sync), while the renderer walks it every frame.
        std::mutex parts_mtx;

    private:
        uint32_t *wip_texture_buffer = nullptr;

        bool force_update_all = false;
        bool update = false;

    public:
        struct FrameDef
        {
            size_t ptr;
            size_t size;
        };

        bool d_frame_mode = false;
        std::vector<FrameDef> frames;

    public:
        static constexpr size_t max_bit_period = 1 << 24;

        int d_display_mode = 0;
        size_t d_bitperiod = 256;
        int d_display_bits = 1;

        uint8_t *get_ptr() { return d_file_memory_ptr; }
        size_t get_ptr_size() { return d_file_memory_size; }

        bool d_is_temporary = false;

    public:
        BitContainer(std::string name, std::string file, std::vector<FrameDef> frms = {});
        ~BitContainer();

        std::string getName() { return d_name; }
        std::string getFilePath() { return d_filepath; }
        std::string getID() { return std::string(unique_id); }

        void init_display();

        // Same lock init_display() takes, so highlight edits cannot race the rebuild that
        // reads highlights while walking the parts.
        void setHighlights(std::vector<HighlightDef> h);
        void clearHighlights();

        void doUpdateTextures();
        void doDrawPlotTextures(ImPlotRect c);

        void renderSegment(PartImage &part, size_t &ii, size_t &iii, size_t &xoffset, size_t &offset);
        void renderSegmentText(PartImage &part, size_t &ii, size_t &iii, size_t &xoffset, size_t &offset);

    public:
        void *bitview = nullptr;
    };
} // namespace satdump