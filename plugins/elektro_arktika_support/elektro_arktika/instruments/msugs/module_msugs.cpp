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

#ifdef ENABLE_RDAS_ALIGNER
#include "core/config.h"
#include "satdump_vars.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif
#endif

namespace elektro_arktika
{
    namespace msugs
    {
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

#ifdef ENABLE_RDAS_ALIGNER
        // The user config dir differs depending on the working directory (see init.cpp), so the
        // aligner calibration has to be searched for in each of the places it can land.
        static std::string getDefaultUserDir()
        {
#ifdef _WIN32
            const char *appdata = getenv("APPDATA");
            return appdata ? std::string(appdata) + "/gobdump" : std::string(".");
#else
            const char *home = getenv("HOME");
            return home ? std::string(home) + "/.config/gobdump" : std::string(".");
#endif
        }

        static void logRDASAlignerOutput(std::string &pending)
        {
            size_t nl;
            while ((nl = pending.find('\n')) != std::string::npos)
            {
                std::string line = pending.substr(0, nl);
                pending.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (!line.empty())
                    logger->info("RDAS aligner: " + line);
            }
        }

        // The aligner reads/writes ./config.json, so it is always started in its own calibration directory
        // with its output piped back into the GobDump log.
        static int runRDASAligner(const std::string &exe, const std::string &workdir, const std::string &sat, const std::string &rdas_dir, const std::string &mode)
        {
#ifdef _WIN32
            std::string cmd = "\"" + exe + "\" -s " + sat + " -d \"" + rdas_dir + "\" -m " + mode;
            std::vector<char> cmdline(cmd.begin(), cmd.end());
            cmdline.push_back('\0');

            SECURITY_ATTRIBUTES sa{};
            sa.nLength = sizeof(sa);
            sa.bInheritHandle = TRUE;
            HANDLE rd = NULL, wr = NULL;
            if (!CreatePipe(&rd, &wr, &sa, 0))
                return -1;
            SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

            STARTUPINFOA si{};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdOutput = wr;
            si.hStdError = wr;
            si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
            PROCESS_INFORMATION pi{};
            if (!CreateProcessA(NULL, cmdline.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, workdir.c_str(), &si, &pi))
            {
                CloseHandle(rd);
                CloseHandle(wr);
                return -1;
            }
            CloseHandle(wr);

            std::string pending;
            char buf[4096];
            DWORD n = 0;
            while (ReadFile(rd, buf, sizeof(buf), &n, NULL) && n > 0)
            {
                pending.append(buf, n);
                logRDASAlignerOutput(pending);
            }
            if (!pending.empty())
                logger->info("RDAS aligner: " + pending);
            CloseHandle(rd);

            WaitForSingleObject(pi.hProcess, INFINITE);
            DWORD rc = 1;
            GetExitCodeProcess(pi.hProcess, &rc);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            return (int)rc;
#else
            int fds[2];
            if (pipe(fds) != 0)
                return -1;

            pid_t pid = fork();
            if (pid == 0)
            {
                dup2(fds[1], STDOUT_FILENO);
                dup2(fds[1], STDERR_FILENO);
                close(fds[0]);
                close(fds[1]);
                if (chdir(workdir.c_str()) != 0)
                    _exit(127);
                execl(exe.c_str(), exe.c_str(), "-s", sat.c_str(), "-d", rdas_dir.c_str(), "-m", mode.c_str(), (char *)nullptr);
                _exit(127);
            }
            close(fds[1]);
            if (pid < 0)
            {
                close(fds[0]);
                return -1;
            }

            std::string pending;
            char buf[4096];
            ssize_t n = 0;
            while ((n = read(fds[0], buf, sizeof(buf))) > 0)
            {
                pending.append(buf, n);
                logRDASAlignerOutput(pending);
            }
            if (!pending.empty())
                logger->info("RDAS aligner: " + pending);
            close(fds[0]);

            int status = 0;
            waitpid(pid, &status, 0);
            return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
        }

        static bool runRDASLayerAligner(bool is_arktika, int sat_num, bool recalibrate, const std::string &rdas_dir)
        {
            const std::string dir = satdump::getExecutableDir();
#ifdef _WIN32
            const std::string exe = dir + "/rdas_aligner.exe";
#else
            const std::string exe = dir + "/rdas_aligner";
#endif
            if (!std::filesystem::exists(exe))
            {
                logger->warn("RDAS layer aligner not found (" + exe + "), skipping alignment");
                return false;
            }

            const std::string sat = std::string(is_arktika ? "M" : "L") + std::to_string(sat_num);

            // Calibration is stored next to settings.json, exactly like the rest of the user config, so
            // it persists between runs. Where settings.json lives depends on how GobDump was started
            // (portable ./config when gobdump_cfg.json is in the working directory, otherwise
            // %APPDATA%/gobdump on Windows or $HOME/.config/gobdump elsewhere), so reuse a valid
            // calibration from any of those places and only calibrate when none of them has one.
            auto calibrationOk = [&](const std::filesystem::path &p) {
                if (!std::filesystem::exists(p))
                    return false;
                try
                {
                    nlohmann::ordered_json cfg = loadJsonFile(p.string());
                    return cfg.contains(sat + "VIS1") && cfg[sat + "VIS1"].contains("CH2") &&
                           cfg.contains(sat + "VIS2") && cfg[sat + "VIS2"].contains("CH2");
                }
                catch (std::exception &)
                {
                    return false;
                }
            };

            std::filesystem::path settings_dir = std::filesystem::path(satdump::satdump_cfg.user_cfg_path).parent_path();
            if (settings_dir.empty()) // settings.json sits directly in the working directory
                settings_dir = std::filesystem::current_path();

            const std::filesystem::path exe_dir = satdump::getExecutableDir();
            std::vector<std::filesystem::path> cal_bases = {
                settings_dir,                    // next to settings.json
                getDefaultUserDir(),             // %APPDATA%/gobdump or $HOME/.config/gobdump
                exe_dir / "config",              // portable install
                exe_dir.parent_path() / "config" // source tree
            };

            std::filesystem::path cal_dir;
            bool calibrated = false;
            for (const std::filesystem::path &base : cal_bases)
            {
                std::filesystem::path candidate = base / "rdas_aligner" / sat;
                if (calibrationOk(candidate / "config.json"))
                {
                    cal_dir = candidate;
                    calibrated = true;
                    break;
                }
            }
            if (cal_dir.empty())
                cal_dir = cal_bases[0] / "rdas_aligner" / sat;

            std::error_code ec;
            std::filesystem::create_directories(cal_dir, ec);
            const std::filesystem::path cfg_path = cal_dir / "config.json";
            const std::string workdir = cal_dir.string();

            // The transforms decide whether a calibration exists - a file left behind by a failed run has only ROIs.
            if (calibrated)
                logger->log(slog::LOG_INFO, "Reusing RDAS layer aligner calibration from " + cfg_path.string());

            // Generate the missing calibration: start from the shipped merge parameters (ROIs) only,
            // the transforms are always derived from this decode.
            if (!calibrated)
            {
                nlohmann::ordered_json seed = nlohmann::ordered_json::object();
                if (resources::resourceExists("elektro/rdas_aligner.json"))
                {
                    nlohmann::ordered_json defaults = loadJsonFile(resources::getResourcePath("elektro/rdas_aligner.json"));
                    for (const std::string &key : {sat + "VIS1", sat + "VIS2"})
                        if (defaults.contains(key) && defaults[key].contains("ROI"))
                            seed[key]["ROI"] = defaults[key]["ROI"];
                }
                saveJsonFile(cfg_path.string(), seed);
            }

            if (!calibrated || recalibrate)
            {
                logger->info("Calibrating RDAS layer aligner for " + sat + (recalibrate ? " (forced)" : " (first decode)") + ", this takes a couple of minutes...");
                int rc = runRDASAligner(exe, workdir, sat, rdas_dir, "calibrate");
                if (rc != 0)
                {
                    logger->error("RDAS layer aligner calibration failed with code " + std::to_string(rc));
                    return false;
                }
            }

            logger->info("Aligning RDAS MSU-GS layers for " + sat + "...");
            int rc = runRDASAligner(exe, workdir, sat, rdas_dir, "generate");
            if (rc != 0)
                logger->error("RDAS layer aligner exited with code " + std::to_string(rc));
            return rc == 0;
        }
#endif

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

            double last_val = 0;

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

                    double orign = p.second;
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

                // Row-major equivalent of the column-major loop in the else branch: same reverse()
                // call and same pixel written once, but each source row is resampled once and then
                // stride-copied into every destination column.
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

                // Off by default: the GUI projection pass on the full 6004x17200 VIS2 image is slow.
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

#ifdef ENABLE_RDAS_ALIGNER
            if (runRDASLayerAligner(is_arktika, sat_num, recalibrate_aligner, dataset_dir))
                for (const char *aligned : {"MSUGS_VIS_Aligned", "MSUGS_VIS-1_Aligned", "MSUGS_VIS-2_Aligned"})
                    if (std::filesystem::exists(dataset_dir + "/" + aligned))
                        dataset.products_list.push_back(aligned);
#endif

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

            ImGui::End();
        }

        std::string MSUGSDecoderModule::getID() { return "elektro_arktika_msugs"; }

        std::shared_ptr<satdump::pipeline::ProcessingModule> MSUGSDecoderModule::getInstance(std::string input_file, std::string output_file_hint, nlohmann::json parameters)
        {
            return std::make_shared<MSUGSDecoderModule>(input_file, output_file_hint, parameters);
        }
    } // namespace msugs
} // namespace elektro_arktika