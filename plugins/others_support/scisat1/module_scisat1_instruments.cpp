#include "module_scisat1_instruments.h"
#include "common/ccsds/ccsds_tm/demuxer.h"
#include "common/ccsds/ccsds_tm/vcdu.h"
#include "common/utils.h"
#include "image/io.h"
#include "imgui/imgui.h"
#include "logger.h"
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>

namespace scisat1
{
    namespace instruments
    {
        SciSat1InstrumentsDecoderModule::SciSat1InstrumentsDecoderModule(std::string input_file, std::string output_file_hint, nlohmann::json parameters)
            : satdump::pipeline::base::FileStreamToFileStreamModule(input_file, output_file_hint, parameters)
        {
            fsfsm_enable_output = false;
        }

        void SciSat1InstrumentsDecoderModule::process()
        {
            uint8_t cadu[1199];

            // One demuxer per VCID, rebuilt when its VC frame count skips
            std::unique_ptr<ccsds::ccsds_tm::Demuxer> demuxers[8];
            int last_vc_count[8];
            for (int v = 0; v < 8; v++)
            {
                demuxers[v] = std::make_unique<ccsds::ccsds_tm::Demuxer>(1016, false, 2, 7);
                last_vc_count[v] = -1;
            }

            while (should_run())
            {
                // Read buffer
                read_data((uint8_t *)&cadu, 1199);

                // FECF (CRC-16/CCITT over the 1033 bytes after the ASM)
                {
                    uint16_t crc = 0xFFFF;
                    for (int i = 4; i < 4 + 1033; i++)
                    {
                        crc ^= cadu[i] << 8;
                        for (int b = 0; b < 8; b++)
                            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
                    }
                    if (crc != (cadu[4 + 1033] << 8 | cadu[4 + 1034]))
                        continue;
                }

                // Parse this transport frame
                ccsds::ccsds_tm::VCDU vcdu = ccsds::ccsds_tm::parseVCDU(cadu);
                if (vcdu.spacecraft_id != 237)
                    continue;

                // VC frame count is cadu[7]
                {
                    int vc_count = cadu[7];
                    int &last = last_vc_count[vcdu.vcid];
                    if (last != -1 && ((vc_count - last) & 0xFF) != 1)
                        demuxers[vcdu.vcid] = std::make_unique<ccsds::ccsds_tm::Demuxer>(1016, false, 2, 7);
                    last = vc_count;
                }

                if (vcdu.vcid == 0) // ACE-FTS
                {
                    for (ccsds::CCSDSPacket &pkt : demuxers[0]->work(cadu))
                        if (pkt.header.apid == 101)
                            fts_reader.work(pkt);
                }
                else if (vcdu.vcid == 1) // MAESTRO
                {
                    for (ccsds::CCSDSPacket &pkt : demuxers[1]->work(cadu))
                        if (pkt.header.apid == 102)
                            maestro_reader.work(pkt);
                }
            }

            cleanup();

            // FTS
            {
                fts_status = SAVING;

                logger->info("----------- FTS");
                logger->info("Lines : %d", fts_reader.lines);

                std::string fts_directory = d_output_file_hint.substr(0, d_output_file_hint.rfind('/')) + "/FTS";

                if (!std::filesystem::exists(fts_directory))
                    std::filesystem::create_directory(fts_directory);

                auto img = fts_reader.getImg();
                image::save_img(img, fts_directory + "/FTS");

                img.resize_bilinear(img.width() / 10, img.height() * 10);
                image::save_img(img, fts_directory + "/FTS_scaled");

                fts_status = DONE;
            }

            // MAESTRO
            {
                maestro_status = SAVING;

                logger->info("----------- MAESTRO");
                logger->info("Lines (1) : %d", maestro_reader.lines_1);
                logger->info("Lines (2) : %d", maestro_reader.lines_2);

                std::string maestro_directory = d_output_file_hint.substr(0, d_output_file_hint.rfind('/')) + "/MAESTRO";

                if (!std::filesystem::exists(maestro_directory))
                    std::filesystem::create_directory(maestro_directory);

                auto img = maestro_reader.getImg1();
                image::save_img(img, maestro_directory + "/MAESTRO_1");
                img = maestro_reader.getImg2();
                image::save_img(img, maestro_directory + "/MAESTRO_2");

                maestro_status = DONE;
            }
        }

        void SciSat1InstrumentsDecoderModule::drawUI(bool window)
        {
            ImGui::Begin("SciSat-1 Instruments Decoder", NULL, window ? 0 : NOWINDOW_FLAGS);

            if (ImGui::BeginTable("##scisat1instrumentstable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("Instrument");
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("Lines / Frames");
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("Status");

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("FTS");
                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(style::theme.green, "%d", fts_reader.lines);
                ImGui::TableSetColumnIndex(2);
                drawStatus(fts_status);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("MAESTRO Mode 1");
                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(style::theme.green, "%d", maestro_reader.lines_1);
                ImGui::TableSetColumnIndex(2);
                drawStatus(maestro_status);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("MAESTRO Mode 2");
                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(style::theme.green, "%d", maestro_reader.lines_2);
                ImGui::TableSetColumnIndex(2);
                drawStatus(maestro_status);

                ImGui::EndTable();
            }

            drawProgressBar();

            ImGui::End();
        }

        std::string SciSat1InstrumentsDecoderModule::getID() { return "scisat1_instruments"; }

        std::shared_ptr<satdump::pipeline::ProcessingModule> SciSat1InstrumentsDecoderModule::getInstance(std::string input_file, std::string output_file_hint, nlohmann::json parameters)
        {
            return std::make_shared<SciSat1InstrumentsDecoderModule>(input_file, output_file_hint, parameters);
        }
    } // namespace instruments
} // namespace scisat1