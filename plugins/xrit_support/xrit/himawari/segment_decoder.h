#pragma once

/**
 * @file segment_decoder.h
 * @brief Himawari Segmented decoder
 */

#include "../segment_decoder.h"
#include "image/image.h"
#include "utils/string.h"
#include "xrit/identify.h"
#include "xrit/processor/get_img.h"

namespace satdump
{
    namespace xrit
    {
        /**
         * @brief Himawari(cast)-specific Segmented Decoder
         */
        class HimawariSegmentedImageDecoder : public SegmentedImageDecoder
        {
        private:
            int seg_count = 0;
            std::vector<bool> segments_done;
            int seg_height = 0, seg_width = 0;

        public:
            void init(int bit_depth, int max_seg, int segment_width, int segment_height)
            {
                seg_count = max_seg;
                segments_done.resize(seg_count, false);

                image = image::Image(bit_depth, segment_width, segment_height * max_seg, 1);
                seg_height = segment_height;
                seg_width = segment_width;

                image.fill(0);
            }

            HimawariSegmentedImageDecoder(XRITFile &file)
            {
                ImageStructureRecord image_structure_record = file.getHeader<ImageStructureRecord>();

                init(image_structure_record.bit_per_pixel > 8 ? 16 : 8, //
                     10,                                                //
                     image_structure_record.columns_count,              //
                     image_structure_record.lines_count);
            }

            void pushSegment(image::Image &data, int segc)
            {
                if (segc >= seg_count || segc < 0)
                    return;
                if (data.size() != seg_height * seg_width)
                {
                    logger->error("Image of the wrong size! (%s) %dx%d Got %dx%d", info.channel.c_str(), seg_width, seg_height, data.width(), data.height());
                    return;
                }
                image::imemcpy(image, (seg_height * seg_width) * segc, data, 0, seg_height * seg_width);
                segments_done[segc] = true;
            }

            void pushSegment(XRITFile &file)
            {
                auto img = getImageFromXRITFile(XRIT_HIMAWARI_AHI, file);

                // filename comes straight off the downlink: std::stoi throws on a non-numeric field, and
                // size()-3 wraps on a filename shorter than 3 chars, making substr throw too. Neither was
                // caught anywhere up the call chain, so one corrupted record killed the whole capture.
                int segment = 0;
                try
                {
                    if (file.filename.size() < 3)
                        throw std::invalid_argument("filename too short");
                    segment = std::stoi(file.filename.substr(file.filename.size() - 3)) - 1;
                }
                catch (std::exception &e)
                {
                    logger->error("Could not parse Himawari segment number from '%s' : %s", file.filename.c_str(), e.what());
                    return;
                }

                pushSegment(img, segment);
            }

            bool isComplete()
            {
                bool complete = true;
                for (int i = 0; i < seg_count; i++)
                    complete = complete && segments_done[i];
                return complete;
            }

            void reset()
            {
                for (int i = 0; i < seg_count; i++)
                    segments_done[i] = false;
                image.clear();
            }

            bool hasData()
            {
                for (int i = 0; i < seg_count; i++)
                    if (segments_done[i])
                        return true;
                return false;
            }
        };
    } // namespace xrit
} // namespace satdump