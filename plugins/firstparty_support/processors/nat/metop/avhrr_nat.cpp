#include "avhrr_nat.h"
#include "common/repack.h"
#include "core/exception.h"
#include "core/resources.h"
#include "libs/bzlib_utils.h"
#include "logger.h"
#include "metop_helper.h"
#include "metop_nat.h"
#include "nlohmann/json_utils.h"
#include "projection/standard/proj_json.h"
#include "utils/time.h"
#include <cstdint>

namespace satdump
{
    namespace firstparty
    {
        void AVHRRNatProcessor::ingestFile(std::vector<uint8_t> nat_file)
        {
            tryDecompressBzip2(nat_file);

            size_t current_ptr = 0;
            while (current_ptr + 20 <= nat_file.size())
            {
                GenericRecordHeader main_header(&nat_file.at(current_ptr));

                // Records are walked by record_size, so a truncated/garbage size would run off the
                // buffer; stop instead of reading past the end.
                if (main_header.record_size < 20 || current_ptr + main_header.record_size > nat_file.size())
                    break;

                if (main_header.record_class == 1 && main_header.record_size >= 696)
                {
                    std::string full_header(&nat_file.at(current_ptr + 20), &nat_file.at(current_ptr + 20) + main_header.record_size - 20);
                    sat_id = full_header.substr(664 - 20 + 32, 3);
                }

                // Record class 8 = AVHRR scanline. The >=26582 test guarantees the fixed header and
                // geolocation offsets read further down are inside the record.
                if (main_header.record_class == 8 && main_header.record_size >= 26582)
                {
                    uint16_t number_of_samples = (uint16_t(nat_file.at(current_ptr + 22)) << 8) | uint16_t(nat_file.at(current_ptr + 23));

                    // Bail if the declared sample count overruns the remaining file.
                    if (number_of_samples == 0 || current_ptr + 24 + size_t(5 * number_of_samples * 2) > nat_file.size())
                        break;

                    bool ch3a = nat_file.at(current_ptr + 26581) & 1;

                    number_of_lines++;
                    image_width = number_of_samples;

                    for (int c = 0; c < 6; c++)
                        avhrr_data.at(c).resize(number_of_lines * number_of_samples);

                    for (int c = 0; c < 5; c++)
                    {
                        int c2 = 0;
                        if (c == 0)
                            c2 = 0;
                        if (c == 1)
                            c2 = 1;
                        if (c == 2)
                            c2 = ch3a ? 2 : 3;
                        if (c == 3)
                            c2 = 4;
                        if (c == 4)
                            c2 = 5;
                        for (int i = 0; i < number_of_samples; i++)
                        {
                            size_t off = current_ptr + 24 + (c * number_of_samples + i) * 2;
                            int16_t val = static_cast<int16_t>((uint16_t(nat_file.at(off)) << 8) | uint16_t(nat_file.at(off + 1)));
                            avhrr_data.at(c2).at((number_of_lines - 1) * image_width + i) = static_cast<uint16_t>(val + 32768);
                        }
                    }

                    timestamps.push_back(main_header.record_start_time);

                    {
                        double lat_first = get_deg_latlon(&nat_file.at(current_ptr + 20538 + 0));
                        double lon_first = get_deg_latlon(&nat_file.at(current_ptr + 20538 + 4));

                        double lat_last = get_deg_latlon(&nat_file.at(current_ptr + 20546 + 0));
                        double lon_last = get_deg_latlon(&nat_file.at(current_ptr + 20546 + 4));

                        gcps_all[all_gcps]["x"] = 0;
                        gcps_all[all_gcps]["y"] = number_of_lines - 1;
                        gcps_all[all_gcps]["lat"] = lat_first;
                        gcps_all[all_gcps]["lon"] = lon_first;
                        all_gcps++;

                        gcps_all[all_gcps]["x"] = image_width - 1;
                        gcps_all[all_gcps]["y"] = number_of_lines - 1;
                        gcps_all[all_gcps]["lat"] = lat_last;
                        gcps_all[all_gcps]["lon"] = lon_last;
                        all_gcps++;

                        // Keep 2 edge GCPs on every line (required for raytracing) and subsample interior tie-points
                        if ((number_of_lines - 1) % 50 == 0)
                        {
                            for (int i = 0; i < 103; i += 2)
                            {
                                int px = 4 + i * 20;
                                if (px >= 2048)
                                    continue;
                                double lat = get_deg_latlon(&nat_file.at(current_ptr + 21380 + i * 8 + 0));
                                double lon = get_deg_latlon(&nat_file.at(current_ptr + 21380 + i * 8 + 4));
                                gcps_all[all_gcps]["x"] = px;
                                gcps_all[all_gcps]["y"] = number_of_lines - 1;
                                gcps_all[all_gcps]["lat"] = lat;
                                gcps_all[all_gcps]["lon"] = lon;
                                all_gcps++;
                            }
                        }
                    }
                }

                current_ptr += main_header.record_size;
            }
        }

        std::shared_ptr<satdump::products::Product> AVHRRNatProcessor::getProduct()
        {
            if (number_of_lines == 0 || timestamps.empty())
                return nullptr;

            auto ptime = satdump::get_median(timestamps);
            auto info = getMetOpSatInfoFromID(sat_id, ptime);

            auto avhrr_products = std::make_shared<satdump::products::ImageProduct>();
            avhrr_products->instrument_name = "avhrr_3";
            avhrr_products->set_product_source(info.sat_name);
            avhrr_products->set_product_timestamp(ptime);

            auto p = loadJsonFile(resources::getResourcePath("projections_settings/metop_abc_avhrr.json"));
            nlohmann::json proj_cfg = p;
            proj_cfg["type"] = "gcps_timestamps_line";
            proj_cfg["gcp_cnt"] = all_gcps;
            proj_cfg["gcps"] = gcps_all;
            proj_cfg["timestamps"] = timestamps;
            avhrr_products->set_proj_cfg(proj_cfg);

            const std::array<std::string, 6> names = {"1", "2", "3a", "3b", "4", "5"};
            for (int i = 0; i < 6; i++)
                avhrr_products->images.push_back({i, "AVHRR-" + names.at(i), names.at(i), image::Image(avhrr_data.at(i).data(), 16, image_width, number_of_lines, 1)});

            nlohmann::json calib_coefs = loadJsonFile(resources::getResourcePath("calibration/AVHRR.json"));

            nlohmann::json calib_cfg;
            avhrr_products->set_calibration("metop_avhrr_nat", calib_cfg);
            for (int n = 0; n < 6; n++)
            {
                avhrr_products->set_channel_unit(n, n < 3 ? CALIBRATION_ID_REFLECTIVE_RADIANCE : CALIBRATION_ID_EMISSIVE_RADIANCE);
                avhrr_products->set_channel_wavenumber(n, calib_coefs.at("MetOp-B").at("channels").at(n).at("wavnb"));
            }

            return avhrr_products;
        }
    } // namespace firstparty
} // namespace satdump
