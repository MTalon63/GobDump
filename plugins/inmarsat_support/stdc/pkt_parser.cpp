#include "pkt_parser.h"
#include "common/buf_bounds.h"
#include "logger.h"

namespace inmarsat
{
    namespace stdc
    {
        // The destination size (mpkt_len, from pkt[2..3]) and the copy length (from descriptor.length via
        // pkt_len) are unrelated frame fields, so nothing made the copy fit. Both are now tied together.
        void STDPacketParser::parse_pkt_bd(uint8_t *pkt, int pkt_len, nlohmann::json &)
        {
            wip_multi_frame_has_start = false;

            if (pkt_len < 4) // Otherwise pkt[3] over-reads and pkt_len-4 goes negative
                return;

            uint8_t mid = pkt[2] & 0xFF;

            int mpkt_len = 0;
            if (mid >> 7 == 0)
                mpkt_len = (mid & 0x0F) + 1;
            else if (mid >> 6 == 0x02)
                mpkt_len = pkt[3] + 2;
            else // mid >= 0xC0 assigned neither branch, leaving mpkt_len 0 and copying into an empty vector
                return;

            size_t copy_len = satdump::buf_len_nonneg(pkt_len - 4);
            if (mpkt_len <= 0 || !satdump::buf_fits((size_t)mpkt_len, 0, copy_len))
                return;

            wip_multi_frame_pkt.assign(mpkt_len, 0);
            wip_multi_frame_gotten_size = (int)copy_len;
            memcpy(wip_multi_frame_pkt.data(), &pkt[2], copy_len);

            wip_multi_frame_has_start = true;
        }

        void STDPacketParser::parse_pkt_be(uint8_t *pkt, int pkt_len, nlohmann::json &)
        {
            if (!wip_multi_frame_has_start || pkt_len < 4)
                return;

            size_t actual_length = satdump::buf_len_nonneg(pkt_len - 4);
            // Continuation frames appended with no bound at all, so they walked the heap indefinitely.
            if (wip_multi_frame_gotten_size < 0 || !satdump::buf_fits(wip_multi_frame_pkt.size(), (size_t)wip_multi_frame_gotten_size, actual_length))
            {
                wip_multi_frame_has_start = false; // Desynced from the start frame - drop, don't write
                return;
            }

            memcpy(&wip_multi_frame_pkt[wip_multi_frame_gotten_size], &pkt[2], actual_length);
            wip_multi_frame_gotten_size += (int)actual_length;
        }

        void STDPacketParser::parse_main_pkt(uint8_t *main_pkt, int main_pkt_len)
        {
            double timestamp = time(0);

            int pos = 0;
            while (pos < main_pkt_len)
            {
                // Setup utils vars
                uint8_t *pkt = &main_pkt[pos];
                int pkt_len_max = main_pkt_len - pos;

                if (pkt[0] == 0x00) // No more packets
                    return;

                pkts::PacketBase pkt_b(&pkt[0], pkt_len_max);

                // Clear output
                output_meta.clear();

                switch (pkt_b.descriptor.type)
                {
                case pkts::PacketBulletinBoard::FRM_ID:
                    output_meta = pkts::PacketBulletinBoard(pkt, pkt_len_max);
                    break;
                case pkts::PacketSignallingChannel::FRM_ID:
                    output_meta = pkts::PacketSignallingChannel(pkt, pkt_len_max);
                    break;
                case pkts::PacketAcknowledgement::FRM_ID:
                    output_meta = pkts::PacketAcknowledgement(pkt, pkt_len_max);
                    break;
                case pkts::PacketAcknowledgementRequest::FRM_ID:
                    output_meta = pkts::PacketAcknowledgementRequest(pkt, pkt_len_max);
                    break;
                case pkts::PacketAnnouncement::FRM_ID:
                    output_meta = pkts::PacketAnnouncement(pkt, pkt_len_max);
                    break;
                case pkts::PacketLESForcedClear::FRM_ID:
                    output_meta = pkts::PacketLESForcedClear(pkt, pkt_len_max);
                    break;
                case pkts::PacketClear::FRM_ID:
                    output_meta = pkts::PacketClear(pkt, pkt_len_max);
                    break;
                case pkts::PacketConfirmation::FRM_ID:
                    output_meta = pkts::PacketConfirmation(pkt, pkt_len_max);
                    break;
                case pkts::PacketDistressAlertAcknowledgement::FRM_ID:
                    output_meta = pkts::PacketDistressAlertAcknowledgement(pkt, pkt_len_max);
                    break;
                case pkts::PacketDistressTestRequest::FRM_ID:
                    output_meta = pkts::PacketDistressTestRequest(pkt, pkt_len_max);
                    break;
                case pkts::PacketEGCSingleHeader::FRM_ID:
                    output_meta = pkts::PacketEGCSingleHeader(pkt, pkt_len_max);
                    break;
                case pkts::PacketEGCDoubleHeader1::FRM_ID:
                    output_meta = pkts::PacketEGCDoubleHeader1(pkt, pkt_len_max);
                    break;
                case pkts::PacketEGCDoubleHeader2::FRM_ID:
                    output_meta = pkts::PacketEGCDoubleHeader2(pkt, pkt_len_max);
                    break;
                case pkts::PacketLogicalChannelAssignement::FRM_ID:
                    output_meta = pkts::PacketLogicalChannelAssignement(pkt, pkt_len_max);
                    break;
                case pkts::PacketLoginAcknowledgment::FRM_ID:
                    output_meta = pkts::PacketLoginAcknowledgment(pkt, pkt_len_max);
                    break;
                case pkts::PacketLogoutAcknowledgment::FRM_ID:
                    output_meta = pkts::PacketLogoutAcknowledgment(pkt, pkt_len_max);
                    break;
                case pkts::PacketMessageData::FRM_ID:
                    output_meta = pkts::PacketMessageData(pkt, pkt_len_max);
                    break;
                case pkts::PacketMessageStatus::FRM_ID:
                    output_meta = pkts::PacketMessageStatus(pkt, pkt_len_max);
                    break;
                case pkts::PacketNetworkUpdate::FRM_ID:
                    output_meta = pkts::PacketNetworkUpdate(pkt, pkt_len_max);
                    break;
                case pkts::PacketRequestStatus::FRM_ID:
                    output_meta = pkts::PacketRequestStatus(pkt, pkt_len_max);
                    break;
                case pkts::PacketTestResult::FRM_ID:
                    output_meta = pkts::PacketTestResult(pkt, pkt_len_max);
                    break;
                case pkts::PacketNetworkMonitor::FRM_ID:
                    output_meta = pkts::PacketNetworkMonitor(pkt, pkt_len_max);
                    break;
                    ///////////////////////////////////////////////
                case 0x3d: // Multiframe start
                    parse_pkt_bd(pkt, pkt_b.descriptor.length, output_meta);
                    break;
                case 0x3e: // Multiframe end
                    parse_pkt_be(pkt, pkt_b.descriptor.length, output_meta);

                    // Check packet is usable
                    if (wip_multi_frame_has_start && (wip_multi_frame_gotten_size == (int)wip_multi_frame_pkt.size() - 2))
                    {
                        STDPacketParser dec;
                        dec.on_packet = on_packet;
                        dec.parse_main_pkt(wip_multi_frame_pkt.data(), wip_multi_frame_pkt.size());
                    }

                    wip_multi_frame_has_start = false;
                    wip_multi_frame_gotten_size = 0;
                    wip_multi_frame_pkt.clear();

                    break;
                default:
                    output_meta["descriptor"] = pkt_b.descriptor;
                }

                // If this is 0x7D and first frame, use better timestamp
                if (pkt_b.descriptor.type == pkts::PacketBulletinBoard::FRM_ID && pos == 0)
                {
                    time_t currentDay = time(0);
                    time_t dayValue = currentDay - (currentDay % 86400);
                    timestamp = dayValue + output_meta.get<pkts::PacketBulletinBoard>().seconds_of_day;
                }

                // Timestamp packet
                output_meta["timestamp"] = timestamp + ((double)pos / (double)main_pkt_len) * 8.64;

                // Not a multiframe PKT, process it now.
                if (pkt_b.descriptor.type != 0x3d && pkt_b.descriptor.type != 0x3e)
                {
                    on_packet(output_meta);
                    output_meta.clear();
                }

                pos += pkt_b.descriptor.length;
            }
        }
    }
}