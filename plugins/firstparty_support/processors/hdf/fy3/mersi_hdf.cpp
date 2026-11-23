#include "mersi_hdf.h"
#include "common/utils.h"
#include "core/resources.h"
#include "image/image.h"
#include "logger.h"
#include "nlohmann/json_utils.h"
#include "utils/time.h"
#include <H5Cpp.h>
#include <H5LTpublic.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace satdump
{
    namespace firstparty
    {
        std::vector<image::Image> get_multi_img_from_hdf(H5::H5File &file, std::string path)
        {
            H5::DataSet s = file.openDataSet(path);

            if (s.getSpace().getSimpleExtentNdims() != 3)
            {
                logger->error("Rank is not 3!");
                return {};
            }

            std::vector<hsize_t> image_dims(3);
            s.getSpace().getSimpleExtentDims(image_dims.data());

            std::vector<uint16_t> tmp_all_channels(image_dims.at(0) * image_dims.at(1) * image_dims.at(2), 0);
            s.read(tmp_all_channels.data(), H5::PredType::NATIVE_UINT16);

            std::vector<image::Image> imgs;
            for (hsize_t i = 0; i < image_dims.at(0); i++)
                imgs.push_back(image::Image(tmp_all_channels.data() + image_dims.at(2) * image_dims.at(1) * i, 16, image_dims.at(2), image_dims.at(1), 1));

            return imgs;
        }

        image::Image get_img_from_hdf(H5::H5File &file, std::string path);

        void MERSIHdfProcessor::ingestFile(std::vector<uint8_t> vec)
        {
            H5::H5File file(H5LTopen_file_image(vec.data(), vec.size(), H5F_ACC_RDONLY));

            double timestamp = -1;
            {
                std::string date_str;
                auto att = file.openAttribute("Observing Beginning Date");
                att.read(att.getStrType(), date_str);

                std::string time_str;
                auto att2 = file.openAttribute("Observing Beginning Time");
                att2.read(att2.getStrType(), time_str);

                std::tm timeS;
                memset(&timeS, 0, sizeof(std::tm));
                if (sscanf(date_str.c_str(), "%4d-%2d-%2d", &timeS.tm_year, &timeS.tm_mon, &timeS.tm_mday) != 3)
                    logger->error("Parse Date Error!");
                if (sscanf(time_str.c_str(), "%2d:%2d:%2d", &timeS.tm_hour, &timeS.tm_min, &timeS.tm_sec) != 3)
                    logger->error("Parse Time Error!");
                timeS.tm_year -= 1900;
                timeS.tm_mon -= 1;
                timestamp = timegm(&timeS);
            }

            std::string name_str;
            auto att = file.openAttribute("Satellite Name");
            att.read(att.getStrType(), name_str);

            if (name_str == "FY-3D")
                mersi_type = MERSI_2;
            else if (name_str == "FY-3E")
                mersi_type = MERSI_LL;
            else if (name_str == "FY-3F")
                mersi_type = MERSI_3;
            else if (name_str == "FY-3G")
                mersi_type = MERSI_RM;
            else if (name_str == "FY-3H")
                mersi_type = MERSI_3;

            // Accumulate EUMETCast 1-minute MERSI-2 L1B granules across a contiguous pass
            if (mersi_type == MERSI_2)
            {
                mersi2_sat_name = name_str;

                if (file.nameExists("Data"))
                {
                    int gran_lines = 0;
                    int gran_width = 0;

                    auto append_band = [&](int ch_idx, image::Image &img)
                    {
                        if (img.size() == 0)
                            return;
                        gran_lines = img.height();
                        gran_width = img.width();
                        mersi2_width = gran_width;

                        size_t old_offset = size_t(mersi2_lines) * size_t(gran_width);
                        mersi2_data.at(ch_idx).resize(old_offset + img.size());
                        for (size_t p = 0; p < img.size(); p++)
                        {
                            uint16_t v = img.get(p);
                            mersi2_data.at(ch_idx).at(old_offset + p) = (v >= 4096) ? 0 : static_cast<uint16_t>(v << 4);
                        }
                    };

                    if (file.nameExists("Data/EV_250_Aggr.1KM_RefSB"))
                    {
                        auto b1_4 = get_multi_img_from_hdf(file, "Data/EV_250_Aggr.1KM_RefSB");
                        for (size_t k = 0; k < b1_4.size() && k < 4; k++)
                            append_band(int(k), b1_4.at(k));
                    }
                    if (file.nameExists("Data/EV_250_Aggr.1KM_Emissive"))
                    {
                        auto b5_6 = get_multi_img_from_hdf(file, "Data/EV_250_Aggr.1KM_Emissive");
                        for (size_t k = 0; k < b5_6.size() && k < 2; k++)
                            append_band(4 + int(k), b5_6.at(k));
                    }
                    if (file.nameExists("Data/EV_1KM_RefSB"))
                    {
                        auto b7_21 = get_multi_img_from_hdf(file, "Data/EV_1KM_RefSB");
                        for (size_t k = 0; k < b7_21.size() && k < 15; k++)
                            append_band(6 + int(k), b7_21.at(k));
                    }
                    if (file.nameExists("Data/EV_1KM_Emissive"))
                    {
                        auto b22_25 = get_multi_img_from_hdf(file, "Data/EV_1KM_Emissive");
                        for (size_t k = 0; k < b22_25.size() && k < 4; k++)
                            append_band(21 + int(k), b22_25.at(k));
                    }

                    if (gran_lines > 0)
                    {
                        for (int l = 0; l < gran_lines; l++)
                            mersi2_timestamps.push_back(timestamp + (double(l) / double(gran_lines)) * 60.0);

                        if (file.nameExists("Geolocation/Latitude") && file.nameExists("Geolocation/Longitude"))
                        {
                            H5::DataSet lat_ds = file.openDataSet("Geolocation/Latitude");
                            H5::DataSet lon_ds = file.openDataSet("Geolocation/Longitude");
                            if (lat_ds.getSpace().getSimpleExtentNdims() == 2 && lon_ds.getSpace().getSimpleExtentNdims() == 2)
                            {
                                std::vector<hsize_t> dims(2);
                                lat_ds.getSpace().getSimpleExtentDims(dims.data());
                                size_t rows = dims.at(0);
                                size_t cols = dims.at(1);

                                std::vector<float> lats(rows * cols);
                                std::vector<float> lons(rows * cols);
                                lat_ds.read(lats.data(), H5::PredType::NATIVE_FLOAT);
                                lon_ds.read(lons.data(), H5::PredType::NATIVE_FLOAT);

                                int step_x = (cols > 1) ? std::max(1, (gran_width - 1) / int(cols - 1)) : 5;

                                if (rows >= 2 && cols >= 2)
                                {
                                    for (int l = 0; l < gran_lines; l++)
                                    {
                                        double pos = (double(l) / double(std::max(1, gran_lines - 1))) * double(rows - 1);
                                        size_t r0 = std::min(rows - 2, size_t(pos));
                                        size_t r1 = r0 + 1;
                                        double t = pos - double(r0);

                                        auto interp_deg = [&](size_t col, float &out_lat, float &out_lon) -> bool
                                        {
                                            float lat0 = lats.at(r0 * cols + col);
                                            float lon0 = lons.at(r0 * cols + col);
                                            float lat1 = lats.at(r1 * cols + col);
                                            float lon1 = lons.at(r1 * cols + col);
                                            if (!std::isfinite(lat0) || !std::isfinite(lon0) || !std::isfinite(lat1) || !std::isfinite(lon1))
                                                return false;
                                            if (std::abs(lat0) > 90.0f || std::abs(lat1) > 90.0f || std::abs(lon0) > 180.0f || std::abs(lon1) > 180.0f)
                                                return false;

                                            double dlon = double(lon1) - double(lon0);
                                            if (dlon > 180.0)
                                                dlon -= 360.0;
                                            else if (dlon < -180.0)
                                                dlon += 360.0;

                                            out_lat = float(double(lat0) + t * (double(lat1) - double(lat0)));
                                            out_lon = float(double(lon0) + t * dlon);
                                            if (out_lon > 180.0f)
                                                out_lon -= 360.0f;
                                            else if (out_lon < -180.0f)
                                                out_lon += 360.0f;
                                            return true;
                                        };

                                        // Interpolate 5-km Geolocation grid onto every scanline for raytraced projection
                                        int y = mersi2_lines + l;
                                        float lat_l = 0, lon_l = 0, lat_r = 0, lon_r = 0;
                                        if (interp_deg(0, lat_l, lon_l) && interp_deg(cols - 1, lat_r, lon_r))
                                        {
                                            mersi2_gcps[mersi2_gcps_cnt]["x"] = 0;
                                            mersi2_gcps[mersi2_gcps_cnt]["y"] = y;
                                            mersi2_gcps[mersi2_gcps_cnt]["lat"] = lat_l;
                                            mersi2_gcps[mersi2_gcps_cnt]["lon"] = lon_l;
                                            mersi2_gcps_cnt++;

                                            mersi2_gcps[mersi2_gcps_cnt]["x"] = gran_width - 1;
                                            mersi2_gcps[mersi2_gcps_cnt]["y"] = y;
                                            mersi2_gcps[mersi2_gcps_cnt]["lat"] = lat_r;
                                            mersi2_gcps[mersi2_gcps_cnt]["lon"] = lon_r;
                                            mersi2_gcps_cnt++;
                                        }

                                        if (l % 20 == 0)
                                        {
                                            for (size_t c = 4; c + 1 < cols; c += 4)
                                            {
                                                float lat_m = 0, lon_m = 0;
                                                if (interp_deg(c, lat_m, lon_m))
                                                {
                                                    mersi2_gcps[mersi2_gcps_cnt]["x"] = std::min(gran_width - 1, int(c) * step_x);
                                                    mersi2_gcps[mersi2_gcps_cnt]["y"] = y;
                                                    mersi2_gcps[mersi2_gcps_cnt]["lat"] = lat_m;
                                                    mersi2_gcps[mersi2_gcps_cnt]["lon"] = lon_m;
                                                    mersi2_gcps_cnt++;
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        mersi2_lines += gran_lines;
                    }
                }
                return;
            }
            if (!mersi_products)
            {
                mersi_products = std::make_shared<satdump::products::ImageProduct>();
                if (mersi_type == MERSI_RM)
                    mersi_products->instrument_name = "mersirm";
                else if (mersi_type == MERSI_3)
                    mersi_products->instrument_name = "mersi3";
                mersi_products->set_product_timestamp(timestamp);
                mersi_products->set_product_source(name_str);
            }

            if (file.nameExists("Data"))
            {
                if (mersi_type == MERSI_LL)
                {
                    if (file.nameExists("Data/EV_250_Emissive_b6"))
                    {
                        auto ch1 = get_img_from_hdf(file, "Data/EV_250_Emissive_b6");
                        mersi_products->images.push_back({0, "MERSILL-1", "1", ch1, 16, satdump::ChannelTransform().init_none()});
                    }
                    if (file.nameExists("Data/EV_250_Emissive_b7"))
                    {
                        auto ch2 = get_img_from_hdf(file, "Data/EV_250_Emissive_b7");
                        mersi_products->images.push_back({1, "MERSILL-2", "2", ch2, 16, satdump::ChannelTransform().init_none()});
                    }
                    if (file.nameExists("Data/EV_1KM_Emissive"))
                    {
                        auto ch3_6 = get_multi_img_from_hdf(file, "Data/EV_1KM_Emissive");
                        for (size_t i = 2; i < 2 + ch3_6.size(); i++)
                            mersi_products->images.push_back({int(i), "MERSILL-" + std::to_string(i + 1), std::to_string(i + 1), ch3_6.at(i - 2), 16, satdump::ChannelTransform().init_none()});
                    }
                    if (file.nameExists("Data/EV_1KM_SWIR"))
                    {
                        auto img = get_img_from_hdf(file, "Data/EV_1KM_SWIR");
                        for (size_t i = 0; i < img.size(); i++)
                            img.set(i, img.get(i) << 4);
                        mersi_products->images.push_back({6, "MERSILL-7", "7", img, 16, satdump::ChannelTransform().init_none()});
                    }
                    if (file.nameExists("Data/EV_1KM_HGS"))
                    {
                        auto img = get_img_from_hdf(file, "Data/EV_1KM_HGS");
                        for (size_t i = 0; i < img.size(); i++)
                            img.set(i, img.get(i) << 4);
                        mersi_products->images.push_back({7, "MERSILL-8", "8", img, 16, satdump::ChannelTransform().init_none()});
                    }
                    if (file.nameExists("Data/EV_1KM_HGS_ACC"))
                    {
                        auto img = get_img_from_hdf(file, "Data/EV_1KM_HGS_ACC");
                        for (size_t i = 0; i < img.size(); i++)
                            img.set(i, img.get(i) << 4);
                        mersi_products->images.push_back({8, "MERSILL-9", "9", img, 16, satdump::ChannelTransform().init_none()});
                    }
                    if (file.nameExists("Data/EV_1KM_LGS"))
                    {
                        auto img = get_img_from_hdf(file, "Data/EV_1KM_LGS");
                        for (size_t i = 0; i < img.size(); i++)
                            img.set(i, img.get(i) << 4);
                        mersi_products->images.push_back({9, "MERSILL-10", "10", img, 16, satdump::ChannelTransform().init_none()});
                    }
                    if (file.nameExists("Data/EV_1KM_MGS"))
                    {
                        auto img = get_img_from_hdf(file, "Data/EV_1KM_MGS");
                        for (size_t i = 0; i < img.size(); i++)
                            img.set(i, img.get(i) << 4);
                        mersi_products->images.push_back({10, "MERSILL-11", "11", img, 16, satdump::ChannelTransform().init_none()});
                    }
                }
                else if (mersi_type == MERSI_RM)
                {
                    if (file.nameExists("Data/EV_Reflectance"))
                    {
                        auto ch1_5 = get_multi_img_from_hdf(file, "Data/EV_Reflectance");
                        for (size_t i = 0; i < ch1_5.size(); i++)
                            mersi_products->images.push_back({int(i), "MERSIRM-" + std::to_string(i + 1), std::to_string(i + 1), ch1_5.at(i), 16, satdump::ChannelTransform().init_none()});
                    }
                    if (file.nameExists("Data/EV_Emissive"))
                    {
                        auto ch6_8 = get_multi_img_from_hdf(file, "Data/EV_Emissive");
                        for (size_t i = 5; i < 5 + ch6_8.size(); i++)
                            mersi_products->images.push_back({int(i), "MERSIRM-" + std::to_string(i + 1), std::to_string(i + 1), ch6_8.at(i - 5), 16, satdump::ChannelTransform().init_none()});
                    }
                }
                else if (mersi_type == MERSI_3)
                {
                    if (file.nameExists("Data/EV_250_RefSB_b1"))
                    {
                        auto img = get_img_from_hdf(file, "Data/EV_250_RefSB_b1");
                        for (size_t i = 0; i < img.size(); i++)
                            img.set(i, img.get(i) << 4);
                        mersi_products->images.push_back({0, "MERSI3-1", "1", img, 12, satdump::ChannelTransform().init_none()});
                    }
                    if (file.nameExists("Data/EV_250_RefSB_b2"))
                    {
                        auto img = get_img_from_hdf(file, "Data/EV_250_RefSB_b2");
                        for (size_t i = 0; i < img.size(); i++)
                            img.set(i, img.get(i) << 4);
                        mersi_products->images.push_back({1, "MERSI3-2", "2", img, 12, satdump::ChannelTransform().init_none()});
                    }
                    if (file.nameExists("Data/EV_250_RefSB_b3"))
                    {
                        auto img = get_img_from_hdf(file, "Data/EV_250_RefSB_b3");
                        for (size_t i = 0; i < img.size(); i++)
                            img.set(i, img.get(i) << 4);
                        mersi_products->images.push_back({2, "MERSI3-3", "3", img, 12, satdump::ChannelTransform().init_none()});
                    }
                    if (file.nameExists("Data/EV_250_RefSB_b4"))
                    {
                        auto img = get_img_from_hdf(file, "Data/EV_250_RefSB_b4");
                        for (size_t i = 0; i < img.size(); i++)
                            img.set(i, img.get(i) << 4);
                        mersi_products->images.push_back({3, "MERSI3-4", "4", img, 12, satdump::ChannelTransform().init_none()});
                    }
                    if (file.nameExists("Data/EV_250_Emissive_b24"))
                    {
                        auto img = get_img_from_hdf(file, "Data/EV_250_Emissive_b24");
                        mersi_products->images.push_back({4, "MERSI3-5", "5", img, 16, satdump::ChannelTransform().init_none()});
                    }
                    if (file.nameExists("Data/EV_250_Emissive_b25"))
                    {
                        auto img = get_img_from_hdf(file, "Data/EV_250_Emissive_b25");
                        mersi_products->images.push_back({5, "MERSI3-6", "6", img, 16, satdump::ChannelTransform().init_none()});
                    }
                    if (file.nameExists("Data/EV_1KM_RefSB"))
                    {
                        auto ch5_19 = get_multi_img_from_hdf(file, "Data/EV_1KM_RefSB");
                        for (size_t i = 6; i < 6 + ch5_19.size(); i++)
                        {
                            for (size_t i2 = 0; i2 < ch5_19.at(i - 6).size(); i2++)
                                ch5_19.at(i - 6).set(i2, ch5_19.at(i - 6).get(i2) << 4);
                            mersi_products->images.push_back(
                                {int(i), "MERSI3-" + std::to_string(i + 1), std::to_string(i + 1), ch5_19.at(i - 6), 12, satdump::ChannelTransform().init_affine(4, 4, 0, 0)});
                        }
                    }
                    if (file.nameExists("Data/EV_1KM_Emissive"))
                    {
                        auto ch20_23 = get_multi_img_from_hdf(file, "Data/EV_1KM_Emissive");
                        for (size_t i = 21; i < 21 + ch20_23.size(); i++)
                            mersi_products->images.push_back(
                                {int(i), "MERSI3-" + std::to_string(i + 1), std::to_string(i + 1), ch20_23.at(i - 21), 16, satdump::ChannelTransform().init_affine(4, 4, 0, 0)});
                    }
                }
            }
        }

        std::shared_ptr<satdump::products::Product> MERSIHdfProcessor::getProduct()
        {
            if (mersi_type == MERSI_2)
            {
                if (mersi2_lines == 0 || mersi2_width == 0)
                    return nullptr;
                auto pro = std::make_shared<satdump::products::ImageProduct>();
                pro->instrument_name = "mersi2";
                pro->set_product_source(mersi2_sat_name);
                pro->set_product_timestamp(satdump::get_median(mersi2_timestamps));
                if (mersi2_gcps_cnt > 0)
                {
                    nlohmann::json proj_cfg = loadJsonFile(resources::getResourcePath("projections_settings/fengyun_d_mersi2.json"));
                    proj_cfg["type"] = "gcps_timestamps_line";
                    proj_cfg["gcp_cnt"] = mersi2_gcps_cnt;
                    proj_cfg["gcps"] = mersi2_gcps;
                    proj_cfg["timestamps"] = mersi2_timestamps;
                    pro->set_proj_cfg(proj_cfg);
                }
                for (int i = 0; i < 25; i++)
                {
                    if (mersi2_data.at(i).empty())
                        continue;
                    // A granule that omitted this band leaves the channel shorter than the accumulated
                    // pass size; pad so image::Image() never reads past the vector end.
                    size_t expected = size_t(mersi2_width) * size_t(mersi2_lines);
                    if (mersi2_data.at(i).size() != expected)
                    {
                        logger->warn("MERSI-2 channel %d has %zu pixels, expected %zu; zero-padding", i + 1, mersi2_data.at(i).size(), expected);
                        mersi2_data.at(i).resize(expected, 0);
                    }
                    pro->images.push_back({i, "MERSI2-" + std::to_string(i + 1), std::to_string(i + 1), image::Image(mersi2_data.at(i).data(), 16, mersi2_width, mersi2_lines, 1), 12,
                                           satdump::ChannelTransform().init_none()});
                }
                return pro;
            }
            return mersi_products;
        }
    } // namespace firstparty
} // namespace satdump
