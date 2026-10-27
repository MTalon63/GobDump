#include "module_msugs.h"
#include "common/simple_deframer.h"
#include "common/utils.h"
#include "image/io.h"
#include "imgui/imgui.h"
#include "logger.h"
#include "products/image/channel_transform.h"
#include "utils/binary.h"
#include "utils/stats.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "products/dataset.h"
#include "products/image_product.h"

#include "common/tracking/tle.h"
#include "core/resources.h"
#include "init.h"
#include "nlohmann/json_utils.h"
#include "rdas_aligner.h"

namespace elektro_arktika
{
    namespace msugs
    {
        static double meanValidTimestamp(const std::vector<double> &timestamps)
        {
            double sum = 0;
            size_t count = 0;
            for (double t : timestamps)
                if (t > 0)
                {
                    sum += t;
                    count++;
                }
            return count > 0 ? sum / (double)count : 0;
        }

        static int getMSUGSNorad(bool is_arktika, int sat_num)
        {
            if (is_arktika)
                return sat_num == 2 ? 58584 : (sat_num == 1 ? 47719 : -1); // Arktika-M N1 / N2

            switch (sat_num)
            {
            case 2:
                return 41105; // Elektro-L 2
            case 3:
                return 44903; // Elektro-L 3
            case 4:
                return 55506; // Elektro-L 4
            default:
                return -1;
            }
        }

        MSUGSDecoderModule::MSUGSDecoderModule(std::string input_file, std::string output_file_hint, nlohmann::json parameters)
            : satdump::pipeline::base::FileStreamToFileStreamModule(input_file, output_file_hint, parameters)
        {
            fsfsm_enable_output = false;
            apply_correction = parameters.contains("apply_correction") ? parameters["apply_correction"].get<bool>() : false;

            is_arktika = parameters.contains("is_arktika") ? parameters["is_arktika"].get<bool>() : false;
            if (parameters.contains("satellite_number"))
                sat_num = parameters["satellite_number"].is_string() ? std::stoi(parameters["satellite_number"].get<std::string>()) : parameters["satellite_number"].get<int>();
            else
                sat_num = 0;

            recalibrate_aligner = parameters.contains("recalibrate_aligner") ? parameters["recalibrate_aligner"].get<bool>() : false;

            decode_ir = parameters.contains("decode_ir") ? parameters["decode_ir"].get<bool>() : false;
            project_vis2 = parameters.contains("project_vis2") ? parameters["project_vis2"].get<bool>() : false;
            fill_missing = parameters.contains("fill_missing") ? parameters["fill_missing"].get<bool>() : false;
            max_fill_lines = parameters.contains("max_fill_lines") ? parameters["max_fill_lines"].get<size_t>() : 50;
            if (decode_ir)
                infr_reader = std::make_unique<MSUIRReader>();
        }

        void MSUGSDecoderModule::process()
        {
            std::string directory = d_output_file_hint.substr(0, d_output_file_hint.rfind('/')) + "/MSU-GS";

            uint8_t cadu[1024];

            def::SimpleDeframer deframerVIS1(0x0218a7a392dd9abf, 64, 121680, 10, true);
            def::SimpleDeframer deframerVIS2(0x0218a7a392dd9abf, 64, 121680, 10, true);
            def::SimpleDeframer deframerVIS3(0x0218a7a392dd9abf, 64, 121680, 10, true);
            def::SimpleDeframer deframerIR(0x0218a7a392dd9abf, 64, 14560, 10, true);
            // def::SimpleDeframer deframerUnknown(0xa6007c, 24, 1680, 0, false);

            std::ofstream data_unknown(directory + "/data_unknown.bin", std::ios::binary);

            logger->info("Demultiplexing and deframing...");

            while (should_run())
            {
                // Read buffer
                read_data((uint8_t *)cadu, 1024);

                int vcid = (cadu[5] >> 1) & 7;
                int vcid_2 = (cadu[11] >> 1) & 7;

                if ((vcid == 2) || (vcid_2 == 2))
                {
                    std::vector<std::vector<uint8_t>> frames = deframerVIS1.work(&cadu[24], 1024 - 24);
                    for (std::vector<uint8_t> &frame : frames)
                    {
                        vis1_reader.pushFrame(&frame[0], apply_correction);

                        uint8_t vals[7];
                        for (int i = 0; i < 7; i++)
                            vals[6 - i] = satdump::reverseBits(frame[15200 + i]);
                        for (int i = 0; i < 7; i++)
                            frame[15200 + i] = vals[i];

#if 0
                        double val = (uint64_t)frame[15202] << 16 | (uint64_t)frame[15203] << 8 | (uint64_t)frame[15204];
                        val = (val / 16777215.0) * 360;
                        printf("%4.4f %4.4f\n", val, val - last_val);
                        last_val = val;
#endif

                        data_unknown.write((char *)frame.data(), frame.size());
                    }
                }
                else if ((vcid == 5) || (vcid_2 == 5))
                {
                    std::vector<std::vector<uint8_t>> frames = deframerVIS2.work(&cadu[24], 1024 - 24);
                    for (std::vector<uint8_t> &frame : frames)
                        vis2_reader.pushFrame(&frame[0], apply_correction);
                }
                else if ((vcid == 3) || (vcid_2 == 3))
                {
                    std::vector<std::vector<uint8_t>> frames = deframerVIS3.work(&cadu[24], 1024 - 24);
                    for (std::vector<uint8_t> &frame : frames)
                        vis3_reader.pushFrame(&frame[0], apply_correction);
                }
                else if (decode_ir && infr_reader && ((vcid == 4) || (vcid_2 == 4)))
                {
                    std::vector<std::vector<uint8_t>> frames = deframerIR.work(&cadu[24], 1024 - 24);
                    for (std::vector<uint8_t> &frame : frames)
                        infr_reader->pushFrame(&frame[0]);
                }

                // Unknown additional data, 0xa6007c ASM 1680-bits frames
                /*{
                    // datatest.write((char *)&cadu[12], 10); // , no idea what that is yet
                    std::vector<std::vector<uint8_t>> frames = deframerUnknown.work(&cadu[12], 10);
                    infr_frames += frames.size();
                    for (std::vector<uint8_t> &frame : frames)
                    {
                        int marker = (frame[3] >> 1) & 0b111;
                        // logger->error(marker);
                        if (marker == 5) // 0, 2, 3, 4, 5
                            data_unknown.write((char *)frame.data(), frame.size());
                    }
                }*/
            }

            // data_unknown.close();
            cleanup();

            // TODOREWORK satelliteID
            std::string sat_name = "ELEKTRO-L";

            // Products dataset
            satdump::products::DataSet dataset;
            dataset.satellite_name = sat_name;
            dataset.timestamp = time(0); // satdump::get_median(vis1_reader.timestamps);

            logger->info("----------- MSU-GS");
            logger->info("MSU-GS CH1 Lines        : " + std::to_string(vis1_reader.frames) + " (dropped " + std::to_string(vis1_reader.rejected_frames) + ")");
            logger->info("MSU-GS CH2 Lines        : " + std::to_string(vis2_reader.frames) + " (dropped " + std::to_string(vis2_reader.rejected_frames) + ")");
            logger->info("MSU-GS CH3 Lines        : " + std::to_string(vis3_reader.frames) + " (dropped " + std::to_string(vis3_reader.rejected_frames) + ")");
            if (decode_ir)
                logger->info("MSU-GS IR Frames        : " + std::to_string(infr_reader->frames));

            if (fill_missing)
            {
                size_t filled = vis1_reader.fillMissing(max_fill_lines) + vis2_reader.fillMissing(max_fill_lines) + vis3_reader.fillMissing(max_fill_lines);
                logger->info("Filled " + std::to_string(filled) + " missing MSU-GS lines (max gap " + std::to_string(max_fill_lines) + ")");
            }

            logger->info("Writing images.... (Can take a while)");

            if (!std::filesystem::exists(directory))
                std::filesystem::create_directory(directory);

            // Raw channels are staggered; these transforms cancel it in the viewer.
            nlohmann::json sat_cfg;
            if (is_arktika && resources::resourceExists("elektro/m" + std::to_string(sat_num) + "_cfg.json"))
                sat_cfg = loadJsonFile(resources::getResourcePath("elektro/m" + std::to_string(sat_num) + "_cfg.json"));
            else if (resources::resourceExists("elektro/l" + std::to_string(sat_num) + "_cfg.json"))
                sat_cfg = loadJsonFile(resources::getResourcePath("elektro/l" + std::to_string(sat_num) + "_cfg.json"));
            else
                logger->error("No further MSU-GS processing will be performed for this ELEKTRO sat!");

            {
                std::vector<std::pair<double, double>> points;

                int is = 0;
                for (auto &p : vis3_reader.angle_points)
                {
                    // printf("%d %4.4f\n", p.first, p.second);

                    p.second -= 60509 + 15597568; // 51686860;
                    p.second *= 17200. / 44065.;  // 17200. / 20.;
                    //  p.second =

                    // if (p.first < 1000)
                    //     printf("%d %f %d\n", (int)p.first, orign, (int)p.second);

                    if (is++ % 10 == 0)
                        points.push_back({(double)p.first, p.second});
                }

                logger->critical(points.size());

                auto img_o = vis3_reader.getImage2();
                auto img_m = img_o;
                img_m.fill(0);

                logger->info("Calculating!");

                satdump::ChannelTransform t;
                t = t.init_affine_interpx(1, 1, 0, 0, points);

                logger->info("Done calculating!");

                // Row-major equivalent of the else branch: resample each row once.
                if (!points.empty() && t.is_reverse_x_separable())
                {
                    const int W = (int)img_o.width(), H = (int)img_o.height();
                    std::vector<double> src_y(H);
                    std::vector<char> src_ok(H);
                    for (int y = 0; y < H; y++)
                    {
                        double xx = 0, yy = y;
                        t.reverse(&yy, &xx);
                        src_y[y] = yy;
                        src_ok[y] = (xx >= 0.0 && xx < (double)W && yy >= 0.0 && yy < (double)W) ? 1 : 0;
                    }

                    uint16_t *dst = (uint16_t *)img_m.raw_data();

#pragma omp parallel
                    {
                        std::vector<int> rowbuf(W);

#pragma omp for schedule(dynamic, 16)
                        for (int y = 0; y < H; y++)
                        {
                            if (!src_ok[y])
                                continue;

                            int y0 = (int)src_y[y];
                            if (y0 > W - 2 || y0 < 0) // get_pixel_bilinear() returns a for its last row
                                y0 = W - 2;

                            img_o.get_pixel_bilinear_row(0, (size_t)y0, src_y[y] - (double)y0, rowbuf.data(), (size_t)W);

                            uint16_t *out = dst + y;
                            for (int x = 0; x < W; x++)
                                out[(size_t)x * H] = (uint16_t)rowbuf[x];
                        }
                    }
                }
                else
                {
                    for (int x = 0; x < 6004; x++)
                    {
                        for (int y = 0; y < 17200; y++)
                        {
                            double xx = 0, yy = y;

                            t.reverse(&yy, &xx);

                            if (yy >= 0 && yy < 17200)
                                img_m.set(0, x, y, img_o.get_pixel_bilinear(0, x, yy));
                        }
                    }
                }
            }

            // MSUVIS1 TODOREWORK
            {
                ////////////////////////////////////////// mtvza_status = SAVING;
                std::string directory = d_output_file_hint.substr(0, d_output_file_hint.rfind('/')) + "/MSUGS_VIS1";

                if (!std::filesystem::exists(directory))
                    std::filesystem::create_directory(directory);

                //   logger->info("----------- KMSS MSU-100 1");
                //   logger->info("Lines : " + std::to_string(kmss_lines));

                satdump::products::ImageProduct msuvis_product;
                msuvis_product.instrument_name = "msugs_vis";
                //                    msuvis_products.has_timestamps = true; // TODOREWORK
                //                    msuvis_products.set_tle(satdump::general_tle_registry.get_from_norad(norad));
                //                    msuvis_products.set_timestamps(timestamps);
                //     msuvis_product.set_proj_cfg(loadJsonFile(resources::getResourcePath("projections_settings/meteor_m2-2_kmss_msu100_1.json")));

                satdump::ChannelTransform t1, t2, t3;
                t1.init_none(), t2.init_none(), t3.init_none();
                if (!sat_cfg["msu_vis_1"]["1"].is_null())
                    t1 = sat_cfg["msu_vis_1"]["1"];
                if (!sat_cfg["msu_vis_1"]["2"].is_null())
                    t2 = sat_cfg["msu_vis_1"]["2"];
                if (!sat_cfg["msu_vis_1"]["3"].is_null())
                    t3 = sat_cfg["msu_vis_1"]["3"];

                msuvis_product.images.push_back({0, "MSUGS-VIS-1", "1", vis1_reader.getImage1(), 10, t1});
                msuvis_product.images.push_back({1, "MSUGS-VIS-2", "2", vis2_reader.getImage1(), 10, t2});
                msuvis_product.images.push_back({2, "MSUGS-VIS-3", "3", vis3_reader.getImage1(), 10, t3});

                msuvis_product.save(directory);
                dataset.products_list.push_back("MSUGS_VIS1");

                ////////////////////////////////////////////////     mtvza_status = DONE;
            }

            // MSUVIS2 TODOREWORK
            {
                ////////////////////////////////////////// mtvza_status = SAVING;
                std::string directory = d_output_file_hint.substr(0, d_output_file_hint.rfind('/')) + "/MSUGS_VIS2";

                if (!std::filesystem::exists(directory))
                    std::filesystem::create_directory(directory);

                //   logger->info("----------- KMSS MSU-100 1");
                //   logger->info("Lines : " + std::to_string(kmss_lines));

                satdump::products::ImageProduct msuvis_product;
                msuvis_product.instrument_name = "msugs_vis";
                //                    msuvis_products.has_timestamps = true; // TODOREWORK
                //                    msuvis_products.set_tle(satdump::db_tle.get_from_norad(norad));
                //                    msuvis_products.set_timestamps(timestamps);
                // msuvis_product.set_proj_cfg_tle_timestamps(loadJsonFile(resources::getResourcePath("projections_settings/elektro_l3_msugs_vis2.json")),
                //                                            satdump::db_tle->get_from_norad(44903), vis1_reader.timestamps);

                satdump::ChannelTransform t1, t2, t3;
                t1.init_none(), t2.init_none(), t3.init_none();
                if (!sat_cfg["msu_vis_2"]["1"].is_null())
                    t1 = sat_cfg["msu_vis_2"]["1"];
                if (!sat_cfg["msu_vis_2"]["2"].is_null())
                    t2 = sat_cfg["msu_vis_2"]["2"];
                if (!sat_cfg["msu_vis_2"]["3"].is_null())
                    t3 = sat_cfg["msu_vis_2"]["3"];

                msuvis_product.images.push_back({0, "MSUGS-VIS-1", "1", vis1_reader.getImage2(), 10, t1});
                msuvis_product.images.push_back({1, "MSUGS-VIS-2", "2", vis2_reader.getImage2(), 10, t2});
                msuvis_product.images.push_back({2, "MSUGS-VIS-3", "3", vis3_reader.getImage2(), 10, t3});

                if (project_vis2)
                {
                const std::string proj_cfg_path = std::string("projections_settings/") + (is_arktika ? "arktika_m" : "elektro_l") + std::to_string(sat_num) + "_msugs_vis2.json";
                if (resources::resourceExists(proj_cfg_path))
                {
                    int norad = getMSUGSNorad(is_arktika, sat_num);
                    double acquisition = 0;
                    int count = 0;
                    for (double t : vis1_reader.timestamps)
                        if (t > 0)
                        {
                            acquisition += t;
                            count++;
                        }
                    if (count > 0)
                        acquisition /= count;

                    std::optional<satdump::TLE> tle;
                    if (norad > 0 && acquisition > 0)
                        tle = satdump::db_keplers->get_from_norad_time(norad, (time_t)acquisition);

                    if (tle.has_value())
                    {
                        msuvis_product.set_product_timestamp(acquisition);
                        msuvis_product.set_proj_cfg_tle_timestamps(loadJsonFile(resources::getResourcePath(proj_cfg_path)), tle, vis1_reader.timestamps);
                        logger->info("MSU-GS VIS2 projection enabled (NORAD " + std::to_string(norad) + ")");
                    }
                    else
                        logger->warn("MSU-GS VIS2 projection skipped: no TLE for NORAD " + std::to_string(norad));
                }
                }

                msuvis_product.save(directory);
                dataset.products_list.push_back("MSUGS_VIS2");

                ////////////////////////////////////////////////     mtvza_status = DONE;
            }

#if 0
            channels_statuses[0] = channels_statuses[1] = channels_statuses[2] = PROCESSING;
            image::Image image1 = vis1_reader.getImage();
            image::Image image2 = vis2_reader.getImage();
            image::Image image3 = vis3_reader.getImage();

            //     image1.crop(0, 1421, 12008, 1421 + 12008);
            channels_statuses[0] = SAVING;
            image::save_img(image1, directory + "/MSU-GS-1");
            channels_statuses[0] = DONE;

            //     image2.crop(0, 1421 + 1804, 12008, 1421 + 1804 + 12008);
            channels_statuses[1] = SAVING;
            image::save_img(image2, directory + "/MSU-GS-2");
            channels_statuses[1] = DONE;

            //    image3.crop(0, 1421 + 3606, 12008, 1421 + 3606 + 12008);
            channels_statuses[2] = SAVING;
            image::save_img(image3, directory + "/MSU-GS-3");
            channels_statuses[2] = DONE;
#endif

            if (decode_ir)
                for (int i = 0; i < 7; i++)
                {
                    channels_statuses[3 + i] = PROCESSING;
                    logger->info("Channel IR " + std::to_string(i + 4) + "...");
                    image::Image img = infr_reader->getImage(i);
                    channels_statuses[3 + i] = SAVING;
                    image::save_img(img, directory + "/MSU-GS-" + std::to_string(i + 4));
                    channels_statuses[3 + i] = DONE;
                }

            const std::string dataset_dir = d_output_file_hint.substr(0, d_output_file_hint.rfind('/'));

            // In-process RDAS alignment, crop/warp/merge into regular products.
            try
            {
                RDASImages rdas_images;
                image::Image vis1_ch1 = vis1_reader.getImage1();
                image::Image vis1_ch2 = vis2_reader.getImage1();
                image::Image vis1_ch3 = vis3_reader.getImage1();
                image::Image vis2_ch1 = vis1_reader.getImage2();
                image::Image vis2_ch2 = vis2_reader.getImage2();
                image::Image vis2_ch3 = vis3_reader.getImage2();

                rdas_images.vis1_ch1 = &vis1_ch1;
                rdas_images.vis1_ch2 = &vis1_ch2;
                rdas_images.vis1_ch3 = &vis1_ch3;
                rdas_images.vis2_ch1 = &vis2_ch1;
                rdas_images.vis2_ch2 = &vis2_ch2;
                rdas_images.vis2_ch3 = &vis2_ch3;

                RDASProcessOptions rdas_options;
                rdas_options.satellite = std::string(is_arktika ? "M" : "L") + std::to_string(sat_num);
                rdas_options.recalibrate = recalibrate_aligner.load();
                rdas_options.output_directory = dataset_dir;
                rdas_options.product_source = sat_name;
                rdas_options.vis1_timestamp = meanValidTimestamp(vis1_reader.timestamps);
                rdas_options.vis2_timestamp = meanValidTimestamp(vis2_reader.timestamps);
                rdas_options.dataset = &dataset;

                const RDASProcessResult rdas_result = processRDASLayers(rdas_images, rdas_options);
                if (rdas_result.success)
                    logger->info("RDAS layer alignment done (" + std::to_string(rdas_result.products.size()) +
                                 (rdas_result.products.size() == 1 ? " product)" : " products)"));
                else
                    logger->warn(std::string("RDAS layer alignment skipped: ") + rdas_result.error);
            }
            catch (std::exception &e)
            {
                logger->error(std::string("RDAS layer alignment failed: ") + e.what());
            }

            dataset.save(dataset_dir);
        }

        void MSUGSDecoderModule::drawUI(bool window)
        {
            ImGui::Begin("ELEKTRO / ARKTIKA MSU-GS Decoder", NULL, window ? 0 : NOWINDOW_FLAGS);

            if (ImGui::BeginTable("##msugstable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("MSU-GS Channel");
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("Frames");
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("Status");

                for (int i = 0; i < 10; i++)
                {
                    int frames = 0;
                    if (i == 0)
                        frames = vis1_reader.frames;
                    else if (i == 1)
                        frames = vis2_reader.frames;
                    else if (i == 2)
                        frames = vis3_reader.frames;
                    else if (infr_reader)
                        frames = infr_reader->frames;

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("Channel %d", i + 1);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextColored(style::theme.green, "%d", frames);
                    ImGui::TableSetColumnIndex(2);
                    drawStatus(channels_statuses[i]);
                }

                ImGui::EndTable();
            }

            drawProgressBar();

            bool recalibrate = recalibrate_aligner.load();
            if (ImGui::Checkbox("Recalibrate RDAS layers on this run##rdas_recalibrate", &recalibrate))
                recalibrate_aligner.store(recalibrate);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Redo the MSU-GS layer calibration at the end of this decode.\n"
                                  "Otherwise the saved calibration is reused.");

            ImGui::End();
        }

        std::string MSUGSDecoderModule::getID() { return "elektro_arktika_msugs"; }

        std::shared_ptr<satdump::pipeline::ProcessingModule> MSUGSDecoderModule::getInstance(std::string input_file, std::string output_file_hint, nlohmann::json parameters)
        {
            return std::make_shared<MSUGSDecoderModule>(input_file, output_file_hint, parameters);
        }
    } // namespace msugs
} // namespace elektro_arktika