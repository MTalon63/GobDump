#include "dvbs_ts_deframer.h"
#include <cstdint>
#include <cstring>

namespace deframing
{
#define TS_BYTE_SIZE (204 * 8 + 1)

    DVBS_TS_Deframer::DVBS_TS_Deframer()
    {
        full_frame_shifter = new uint8_t[TS_SIZE * 2];
        buf_bytes = new uint8_t[TS_BYTE_SIZE];
    }

    DVBS_TS_Deframer::~DVBS_TS_Deframer()
    {
        delete[] full_frame_shifter;
        delete[] buf_bytes;
    }

    int DVBS_TS_Deframer::work(uint8_t *input, int size, uint8_t *output)
    {
        int frame_count = 0;

        int bytes = repack.work(input, size, buf_bytes);

        for (int ib = 0; ib < bytes; ib++)
        {
            memmove(&full_frame_shifter[0], &full_frame_shifter[1], TS_BYTE_SIZE - 1);
            full_frame_shifter[TS_BYTE_SIZE - 1] = buf_bytes[ib];

            for (int s = 0; s < 8; s++)
            {
                int errors_nor = 0;
                int errors_inv = 0;

                // 8 Sync bytes. First one should be inverted
                for (int i = 0; i < 8; i++)
                {
                    sync_bytes[i] = ((full_frame_shifter[204 * i + 0] << s) | (full_frame_shifter[204 * i + 1] >> (8 - s))) & 0xFF;
                    errors_nor += compare_8(i == 0 ? 0xB8 : 0x47, sync_bytes[i]);
                    errors_inv += compare_8(i == 0 ? 0x47 : 0xB8, sync_bytes[i]);
                }

                if (errors_nor <= 8)
                {
                    uint8_t *outfrm = &output[frame_count * 204 * 8];
                    for (int i = 0; i < 1632; i++)
                        outfrm[i] = (((full_frame_shifter[i + 0] << s) | (full_frame_shifter[i + 1] >> (8 - s))) & 0xFF);
                    frame_count++;
                }
                else if (errors_inv <= 8)
                {
                    uint8_t *outfrm = &output[frame_count * 204 * 8];
                    for (int i = 0; i < 1632; i++)
                        outfrm[i] = (((full_frame_shifter[i + 0] << s) | (full_frame_shifter[i + 1] >> (8 - s))) & 0xFF) ^ 0xFF;
                    frame_count++;
                }
            }
        }

        return frame_count;
    }
}