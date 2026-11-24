#include "svissr_hdf.h"
#include "common/calibration.h"
#include "common/utils.h"
#include "image/image.h"
#include "logger.h"
#include "products/image/calibration_units.h"
#include "utils/string.h"
#include <H5Cpp.h>
#include <H5LTpublic.h>
#include <algorithm>
#include <array>
#include <zlib.h>

namespace satdump
{
    namespace firstparty
    {
        image::Image get_img_from_hdf(H5::H5File &file, std::string path)
        {
            H5::DataSet s = file.openDataSet(path);

            if (s.getSpace().getSimpleExtentNdims() != 2)
            {
                logger->error("Rank is not 2!");
                return image::Image();
            }

            std::vector<hsize_t> image_dims(2);
            s.getSpace().getSimpleExtentDims(image_dims.data());
            image::Image img(16, image_dims.at(1), image_dims.at(0), 1);
            s.read(img.raw_data(), H5::PredType::NATIVE_UINT16);

            return img;
        }

        std::vector<float> get_lut_from_hdf(H5::H5File &file, std::string path, size_t expected_len)
        {
            std::vector<float> lut(expected_len, 0.0f);
            if (H5Lexists(file.getId(), path.c_str(), H5P_DEFAULT) <= 0)
                return lut;

            H5::DataSet s = file.openDataSet(path);
            hsize_t total = s.getSpace().getSimpleExtentNpoints();
            if (total >= expected_len)
            {
                std::vector<float> raw(total);
                s.read(raw.data(), H5::PredType::NATIVE_FLOAT);
                std::copy(raw.begin(), raw.begin() + expected_len, lut.begin());
            }
            return lut;
        }

        void SVISSRHdfProcessor::ingestFile(std::vector<uint8_t> vec)
        {
            // EUMETCast distributes FY-2 NOM files as gzip-compressed HDF5 (.HDF.gz)
            if (vec.size() > 2 && vec.at(0) == 0x1f && vec.at(1) == 0x8b)
            {
                z_stream strm{};
                if (inflateInit2(&strm, 16 + MAX_WBITS) == Z_OK)
                {
                    strm.next_in = vec.data();
                    strm.avail_in = vec.size();
                    std::vector<uint8_t> out(16 * 1024 * 1024);
                    int ret = Z_OK;
                    while (true)
                    {
                        if (strm.total_out >= out.size())
                            out.resize(out.size() * 2);
                        strm.next_out = out.data() + strm.total_out;
                        strm.avail_out = out.size() - strm.total_out;
                        ret = inflate(&strm, Z_NO_FLUSH);

                        if (ret == Z_STREAM_END)
                            break;
                        if (ret == Z_OK)
                        {
                            if (strm.avail_in == 0 && strm.avail_out > 0)
                                break; // all input consumed, no further output
                            continue;
                        }
                        if (ret == Z_BUF_ERROR && strm.avail_out == 0)
                            continue; // output buffer was full; grown above on the next pass
                        logger->error("FY-2 .HDF.gz inflate stopped early (zlib ret %d, avail_in=%u, total_out=%lu)", ret, strm.avail_in,
                                      (unsigned long)strm.total_out);
                        break;
                    }
                    out.resize(strm.total_out);
                    inflateEnd(&strm);
                    vec = std::move(out);
                }
            }

            H5::H5File file(H5LTopen_file_image(vec.data(), vec.size(), H5F_ACC_RDONLY));

            if (!svissr_products)
            {
                struct NomFileInfo
                {
                    unsigned short StartYear;
                    unsigned short StartMonth;
                    unsigned short StartDay;
                    unsigned short StartHour;
                    unsigned short StartMinute;
                    unsigned short StartSecond;
                    std::array<char, 13> Satellite;
                };

                NomFileInfo hdr{};

                if (H5Lexists(file.getId(), "NomFileInfo", H5P_DEFAULT) > 0)
                {
                    H5::CompType type(sizeof(NomFileInfo));
                    type.insertMember("StartYear", HOFFSET(NomFileInfo, StartYear), H5::PredType::NATIVE_USHORT);
                    type.insertMember("StartMonth", HOFFSET(NomFileInfo, StartMonth), H5::PredType::NATIVE_USHORT);
                    type.insertMember("StartDay", HOFFSET(NomFileInfo, StartDay), H5::PredType::NATIVE_USHORT);
                    type.insertMember("StartHour", HOFFSET(NomFileInfo, StartHour), H5::PredType::NATIVE_USHORT);
                    type.insertMember("StartMinute", HOFFSET(NomFileInfo, StartMinute), H5::PredType::NATIVE_USHORT);
                    type.insertMember("StartSecond", HOFFSET(NomFileInfo, StartSecond), H5::PredType::NATIVE_USHORT);
                    hid_t strtype = H5Tcopy(H5T_C_S1);
                    H5Tset_size(strtype, 13);
                    type.insertMember("Satellite", HOFFSET(NomFileInfo, Satellite), strtype);

                    file.openDataSet("NomFileInfo").read(&hdr, type);
                }

                std::tm timeS;
                memset(&timeS, 0, sizeof(std::tm));
                timeS.tm_year = hdr.StartYear;
                timeS.tm_mon = hdr.StartMonth;
                timeS.tm_mday = hdr.StartDay;
                timeS.tm_hour = hdr.StartHour;
                timeS.tm_min = hdr.StartMinute;
                timeS.tm_sec = hdr.StartSecond;
                timeS.tm_year -= 1900;
                timeS.tm_mon -= 1;
                double timestamp = timegm(&timeS);

                std::string satellite_name(hdr.Satellite.data(), strnlen(hdr.Satellite.data(), hdr.Satellite.size()));
                replaceAllStr(satellite_name, "FY2", "FY-2");

                svissr_products = std::make_shared<satdump::products::ImageProduct>();
                svissr_products->instrument_name = "fy2-svissr";
                svissr_products->set_product_timestamp(timestamp);
                svissr_products->set_product_source(satellite_name);

                nlohmann::json calib_cfg;
                calib_cfg.emplace("ch1_lut", std::vector<float>(1024, 0.0f));
                calib_cfg.emplace("ch2_lut", std::vector<float>(1024, 0.0f));
                calib_cfg.emplace("ch3_lut", std::vector<float>(1024, 0.0f));
                calib_cfg.emplace("ch4_lut", std::vector<float>(1024, 0.0f));
                calib_cfg.emplace("ch5_lut", std::vector<float>(1024, 0.0f));
                svissr_products->set_calibration("fy2_svissr", calib_cfg);
            }

            struct ChannelDef
            {
                int abs_index;
                const char *name;
                const char *channel_id;
                const char *dataset;
                const char *cal_dataset;
                const char *lut_key;
                double wavelength_m;
                int bit_depth;
                bool is_vis;
                bool is_vis_4km;
            };

            // Ordered by ascending wavelength to match SatDump's fy2_svissr calibrator:
            // 0: VIS (0.725um), 1: IR4 (3.75um), 2: IR3 (6.75um), 3: IR1 (10.8um), 4: IR2 (12.0um)
            const std::vector<ChannelDef> channels = {
                // The 1km VIS file ships its VIS calibration table as CALChannelVIS (there is no
                // CALChannelVIS1KM in the NOM files), so both VIS entries read the same table.
                {0, "SVISSR-1", "1", "NOMChannelVIS1KM", "CALChannelVIS", "ch1_lut", 0.725e-6, 6, true, false},
                {0, "SVISSR-1", "1", "NOMChannelVIS", "CALChannelVIS", "ch1_lut", 0.725e-6, 6, true, true},
                {1, "SVISSR-2", "2", "NOMChannelIR4", "CALChannelIR4", "ch2_lut", 3.75e-6, 10, false, false},
                {2, "SVISSR-3", "3", "NOMChannelIR3", "CALChannelIR3", "ch3_lut", 6.75e-6, 10, false, false},
                {3, "SVISSR-4", "4", "NOMChannelIR1", "CALChannelIR1", "ch4_lut", 10.8e-6, 10, false, false},
                {4, "SVISSR-5", "5", "NOMChannelIR2", "CALChannelIR2", "ch5_lut", 12.0e-6, 10, false, false},
            };

            nlohmann::json calib_cfg = svissr_products->get_calibration_raw();
            bool calib_updated = false;

            for (const auto &ch : channels)
            {
                if (H5Lexists(file.getId(), ch.dataset, H5P_DEFAULT) <= 0)
                    continue;

                // Prefer 1KM VIS over 4KM VIS if both exist in the group
                bool already_loaded = false;
                for (size_t idx = 0; idx < svissr_products->images.size(); idx++)
                {
                    if (svissr_products->images.at(idx).abs_index == ch.abs_index)
                    {
                        if (ch.is_vis_4km)
                            already_loaded = true;
                        else
                            svissr_products->images.erase(svissr_products->images.begin() + idx);
                        break;
                    }
                }
                if (already_loaded)
                    continue;

                image::Image img = get_img_from_hdf(file, ch.dataset);
                if (img.size() == 0)
                    continue;

                if (!ch.is_vis)
                {
                    for (size_t i = 0; i < img.size(); i++)
                        if (img.get(i) == 65535)
                            img.set(i, 0);
                        else
                            img.set(i, 65535 - (img.get(i) << 6));

                    // Reverse CALChannelIRx LUT (stored hot->cold) to match inverted 10-bit counts
                    std::vector<float> lut = get_lut_from_hdf(file, ch.cal_dataset, 1024);
                    std::reverse(lut.begin(), lut.end());
                    calib_cfg.at(ch.lut_key) = lut;
                    calib_updated = true;
                }
                else
                {
                    for (size_t i = 0; i < img.size(); i++)
                        if (img.get(i) == 65535)
                            img.set(i, 0);
                        else
                            img.set(i, img.get(i) << 10);

                    std::vector<float> lut = get_lut_from_hdf(file, ch.cal_dataset, 64);
                    for (float &v : lut)
                        v /= 100.0f;
                    lut.resize(1024, 0.0f);
                    calib_cfg.at(ch.lut_key) = lut;
                    calib_updated = true;
                }

                auto transform = (ch.is_vis && !ch.is_vis_4km) ? satdump::ChannelTransform().init_none() : satdump::ChannelTransform().init_affine(4, 4, 0, 0);
                svissr_products->images.push_back({ch.abs_index, ch.name, ch.channel_id, std::move(img), ch.bit_depth, transform});
                svissr_products->set_channel_wavenumber(ch.abs_index, freq_to_wavenumber(SPEED_OF_LIGHT_M_S / ch.wavelength_m));
                svissr_products->set_channel_unit(ch.abs_index, ch.is_vis ? CALIBRATION_ID_ALBEDO : CALIBRATION_ID_BRIGHTNESS_TEMPERATURE);
            }
            if (calib_updated)
                svissr_products->set_calibration("fy2_svissr", calib_cfg);
        }
        std::shared_ptr<satdump::products::Product> SVISSRHdfProcessor::getProduct() { return svissr_products; }
    } // namespace firstparty
} // namespace satdump
