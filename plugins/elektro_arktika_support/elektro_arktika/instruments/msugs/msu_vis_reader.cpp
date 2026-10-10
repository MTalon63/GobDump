#include "msu_vis_reader.h"
#include "logger.h"
#include "utils/binary.h"
#include <cstring>
#include <string>

namespace elektro_arktika
{
    namespace msugs
    {
        MSUVISReader::MSUVISReader()
        {
            imageBuffer1 = new unsigned short[17200 * 6004];
            imageBuffer2 = new unsigned short[17200 * 6004];
            timestamps.resize(17200, -1);
            line_filled.resize(17200, 0);
            frames = 0;
        }

        MSUVISReader::~MSUVISReader()
        {
            delete[] imageBuffer1;
            delete[] imageBuffer2;
        }

        void MSUVISReader::writeFrame(uint8_t *data, int counter)
        {
            // Offset to start reading from
            int pos = 5 * 38;

            // Convert to 10 bits values
            for (int i = 0; i < 12044; i += 4)
            {
                msuLineBuffer[i] = (data[pos + 0] << 2) | (data[pos + 1] >> 6);
                msuLineBuffer[i + 1] = ((data[pos + 1] % 64) << 4) | (data[pos + 2] >> 4);
                msuLineBuffer[i + 2] = ((data[pos + 2] % 16) << 6) | (data[pos + 3] >> 2);
                msuLineBuffer[i + 3] = ((data[pos + 3] % 4) << 8) | data[pos + 4];
                pos += 5;
            }

            // Deinterleave and load into our image buffer
            for (int i = 0; i < 6004; i++)
            {
                imageBuffer1[counter * 6004 + i] = msuLineBuffer[i * 2 + 0] << 6;
                imageBuffer2[counter * 6004 + i] = msuLineBuffer[i * 2 + 1] << 6;
            }

            uint64_t data_time = data[10] << 32 | data[11] << 24 | data[12] << 16 | data[13] << 8 | data[14];
            //  data_time = 0;
            double timestamp = data_time / 256.0; //.56570155902006;
            timestamp += 1735204808.2837029;
            timestamp -= 1800 + 88;
            timestamps[counter] = timestamp;

            uint8_t vals[7];
            for (int i = 0; i < 7; i++)
                vals[6 - i] = satdump::reverseBits(data[15200 + i]);
            for (int i = 0; i < 7; i++)
                data[15200 + i] = vals[i];

            data[15208] = satdump::reverseBits(data[15208]);

            double val = (uint64_t)data[12] << 16 | (uint64_t)data[13] << 8 | (uint64_t)data[14]; // (uint64_t)data[15202] << 16 | (uint64_t)data[15203] << 8 | (uint64_t)data[15204];
            // val = (val / 16777215.0) * 360;

            if (val > 1)
            {
                angle_points.push_back({counter, val});
            }

            line_filled[counter] = 1;
            frames++;
        }

        void MSUVISReader::pushFrame(uint8_t *data, bool apply_correction)
        {

            // First bit is never set, mask it
            int counter = (data[8] << 8 | data[9]) & 0x7fff;

            // Does correction logic if specified by the user
            if (apply_correction)
            {
                // Unlocks if we are fstarting a new image
                // Not implemented in RDAS!
                /*
                if (counter_locked) && (counter == 1 || counter == 2)) {
                    counter_locked = false;

                } else*/

                if (!counter_locked)
                {
                    if (counter == global_counter + 1)
                    {
                        // LOCKED!
                        logger->debug("Counter correction LOCKED! Counter: " + std::to_string(counter));
                        counter_locked = true;
                    }
                    else
                    {
                        // We can't lock, save this counter for a check on the next one
                        global_counter = counter;
                    }
                }
                if (counter_locked)
                {
                    // Makes sure dropped frames don't throw us off, a few skipped lines are fine
                    // Corrector therefore doesn't fix three LSB flips, but this improves reliability
                    // with projections and such
                    if (abs(counter - global_counter) > 7)
                    {
                        counter = global_counter + 1;
                    }

                    global_counter = counter;
                }
            }
            // Warning: The above code MUST have an unlock in the FD end! Since the code doesn't have
            // handling for more than one FD at a time right now, I didn't add this. Just keep it in mind!

            // Sanity check
            if (counter >= 17200)
            {
                rejected_frames++;
                return;
            }

            // Resolve the frame we held on the previous call: it was a real line only if this
            // counter continues it. Otherwise it was a bit-flip in the counter field.
            if (pending_valid)
            {
                if (counter == pending_counter + 1 && !line_filled[pending_counter])
                {
                    writeFrame(pending_data.data(), pending_counter);
                    if (pending_counter > last_counter)
                        last_counter = pending_counter;
                }
                else
                    rejected_frames++;
                pending_valid = false;
            }

            // Never overwrite a line we already have (that is what corrupted counters used to do)
            if (line_filled[counter])
            {
                rejected_frames++;
                return;
            }

            // A large forward jump is ambiguous: hold it and let the next frame tell us. A bit-flip
            // is followed by the true next line, a real gap by the held counter + 1.
            if (counter > last_counter && counter - last_counter > MAX_LINE_GAP)
            {
                if ((int)pending_data.size() != FRAME_BYTES)
                    pending_data.resize(FRAME_BYTES);
                memcpy(pending_data.data(), data, FRAME_BYTES);
                pending_counter = counter;
                pending_valid = true;
                return;
            }

            // Sequential line, or a late-delivered line filling a row we don't have yet
            writeFrame(data, counter);
            if (counter > last_counter)
                last_counter = counter;
        }

        // Cosmetic fill, same idea as the Meteor-M LRPT "fill missing data": interpolate each
        // missing line between its neighbours. Only gaps of at most max_lines are filled, and
        // the line timestamps are interpolated too so the projection can still use those rows.
        size_t MSUVISReader::fillMissing(size_t max_lines)
        {
            if (max_lines == 0)
                return 0;

            const int W = 6004;
            size_t filled = 0;
            int start = -1;

            for (int y = 0; y <= 17200; y++)
            {
                bool missing = (y < 17200) && !line_filled[y];

                if (missing)
                {
                    if (start < 0)
                        start = y;
                    continue;
                }

                if (start < 0)
                    continue;

                int end = y - 1;
                int len = end - start + 1;

                // Can only bridge a gap with a good line on either side
                if (len <= (int)max_lines && start > 0 && y < 17200 && line_filled[start - 1] && line_filled[y])
                {
                    const unsigned short *top1 = &imageBuffer1[(start - 1) * W];
                    const unsigned short *bot1 = &imageBuffer1[y * W];
                    const unsigned short *top2 = &imageBuffer2[(start - 1) * W];
                    const unsigned short *bot2 = &imageBuffer2[y * W];

#pragma omp parallel for
                    for (int x = 0; x < W; x++)
                    {
                        float t1 = top1[x], b1 = bot1[x], t2 = top2[x], b2 = bot2[x];
                        for (int k = 0; k < len; k++)
                        {
                            float p = (float)(k + 1) / (float)(len + 1);
                            imageBuffer1[(start + k) * W + x] = (unsigned short)((1.0f - p) * t1 + p * b1);
                            imageBuffer2[(start + k) * W + x] = (unsigned short)((1.0f - p) * t2 + p * b2);
                        }
                    }

                    double above = timestamps[start - 1], below = timestamps[y];
                    for (int k = 0; k < len; k++)
                    {
                        float p = (float)(k + 1) / (float)(len + 1);
                        if (above > 0 && below > 0)
                            timestamps[start + k] = above + (below - above) * p;
                        line_filled[start + k] = 1;
                    }

                    filled += len;
                }

                start = -1;
            }

            return filled;
        }

        image::Image MSUVISReader::getImage1() { return image::Image(&imageBuffer1[0], 16, 6004, 17200, 1); }

        image::Image MSUVISReader::getImage2() { return image::Image(&imageBuffer2[0], 16, 6004, 17200, 1); }
    } // namespace msugs
} // namespace elektro_arktika
