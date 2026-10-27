#include "rdas_aligner.h"

#include "core/config.h"
#include "core/exception.h"
#include "core/resources.h"
#include "init.h"
#include "logger.h"
#include "products/dataset.h"
#include "products/image_product.h"
#include "satdump_vars.h"
#include "utils/stats.h"

#include "nlohmann/json_utils.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <limits>
#include <utility>

namespace elektro_arktika
{
    namespace msugs
    {
        namespace fs = std::filesystem;

        namespace
        {
            std::string scanIndex(const std::string &satellite, bool vis1) { return satellite + (vis1 ? "VIS1" : "VIS2"); }

            nlohmann::ordered_json seededCalibration(const std::string &satellite)
            {
                nlohmann::ordered_json seed = nlohmann::ordered_json::object();

                if (!resources::resourceExists("elektro/rdas_aligner.json"))
                    return seed;

                try
                {
                    nlohmann::ordered_json defaults = loadJsonFile(resources::getResourcePath("elektro/rdas_aligner.json"));
                    for (const std::string &key : {scanIndex(satellite, true), scanIndex(satellite, false)})
                        if (defaults.contains(key) && defaults[key].is_object())
                            for (auto &el : defaults[key].items())
                                seed[key][el.key()] = el.value();
                }
                catch (std::exception &e)
                {
                    logger->warn(std::string("RDAS aligner: could not read alignment defaults: ") + e.what());
                }

                return seed;
            }

            std::vector<fs::path> calibrationCandidates(const std::string &satellite)
            {
                std::vector<fs::path> paths;
                auto add = [&paths, &satellite](const fs::path &base) {
                    if (base.empty())
                        return;
                    fs::path p = base / "rdas_aligner" / satellite / "config.json";
                    if (std::find(paths.begin(), paths.end(), p) == paths.end())
                        paths.push_back(p);
                };

                add(satdump::user_path);

                if (!satdump::satdump_cfg.user_cfg_path.empty())
                {
                    fs::path settings = fs::path(satdump::satdump_cfg.user_cfg_path).parent_path();
                    if (!settings.empty())
                        add(settings);
                }

                const fs::path exe_dir = satdump::getExecutableDir();
                add(exe_dir / "config");
                add(exe_dir.parent_path() / "config"); // running from build/ in the source tree

                return paths;
            }

            bool readCalibrationFile(const fs::path &path, nlohmann::ordered_json &json)
            {
                if (!fs::exists(path))
                    return false;
                try
                {
                    json = loadJsonFile(path.string());
                    return json.is_object();
                }
                catch (std::exception &e)
                {
                    logger->warn(std::string("RDAS aligner: cannot parse ") + path.string() + ": " + e.what());
                    return false;
                }
            }

            bool writeCalibrationFile(const fs::path &path, const nlohmann::ordered_json &json)
            {
                std::error_code ec;
                if (path.has_parent_path())
                    fs::create_directories(path.parent_path(), ec);
                try
                {
                    saveJsonFile(path.string(), json);
                    return true;
                }
                catch (std::exception &e)
                {
                    logger->error(std::string("RDAS aligner: cannot save calibration: ") + e.what());
                    return false;
                }
            }

            bool readAffine(const nlohmann::ordered_json &json, const std::string &index, const char *channel,
                            image::AffineTransform &out)
            {
                if (!json.contains(index) || !json[index].is_object() || !json[index].contains(channel))
                    return false;
                const auto &c = json[index][channel];
                if (!c.is_object())
                    return false;

                try
                {
                    out.a = c.value("a", 1.0);
                    out.b = c.value("b", 0.0);
                    out.tx = c.value("tx", 0.0);
                    out.c = c.value("c", 0.0);
                    out.d = c.value("d", 1.0);
                    out.ty = c.value("ty", 0.0);
                }
                catch (std::exception &)
                {
                    return false;
                }

                return std::isfinite(out.a) && std::isfinite(out.b) && std::isfinite(out.tx) &&
                       std::isfinite(out.c) && std::isfinite(out.d) && std::isfinite(out.ty);
            }

            void writeAffine(nlohmann::ordered_json &json, const std::string &index, const char *channel,
                             const image::AffineTransform &m)
            {
                json[index][channel]["a"] = m.a;
                json[index][channel]["b"] = m.b;
                json[index][channel]["c"] = m.c;
                json[index][channel]["d"] = m.d;
                json[index][channel]["tx"] = m.tx;
                json[index][channel]["ty"] = m.ty;
            }

            void readLayerCalibration(const nlohmann::ordered_json &json, const std::string &index,
                                      RDASLayerCalibration &out)
            {
                out.has_ch2 = readAffine(json, index, "CH2", out.ch2);
                out.has_ch3 = readAffine(json, index, "CH3", out.ch3);

                if (json.contains(index) && json[index].contains("ROI") && json[index]["ROI"].is_object())
                {
                    const auto &roi = json[index]["ROI"];
                    out.roi_x = roi.value("x", 0);
                    out.roi_y = roi.value("y", 0);
                    out.roi_width = roi.value("width", 0);
                    out.roi_height = roi.value("height", 0);
                    out.has_roi = out.roi_width > 0 && out.roi_height > 0;
                }
            }

            void writeLayerCalibration(nlohmann::ordered_json &json, const std::string &index,
                                       const RDASLayerCalibration &in, bool write_affines, bool write_roi)
            {
                if (write_affines)
                {
                    if (in.has_ch2)
                        writeAffine(json, index, "CH2", in.ch2);
                    if (in.has_ch3)
                        writeAffine(json, index, "CH3", in.ch3);
                }

                if (write_roi && in.has_roi)
                {
                    json[index]["ROI"]["x"] = in.roi_x;
                    json[index]["ROI"]["y"] = in.roi_y;
                    json[index]["ROI"]["width"] = in.roi_width;
                    json[index]["ROI"]["height"] = in.roi_height;
                }
            }

            bool affineIsSane(const image::AffineTransform &m, double expected_ty, std::string &why)
            {
                if (!std::isfinite(m.a) || !std::isfinite(m.b) || !std::isfinite(m.tx) ||
                    !std::isfinite(m.c) || !std::isfinite(m.d) || !std::isfinite(m.ty))
                {
                    why = "non-finite coefficients";
                    return false;
                }

                if (std::fabs(m.a - 1.0) > 0.02 || std::fabs(m.d - 1.0) > 0.02 ||
                    std::fabs(m.b) > 0.015 || std::fabs(m.c) > 0.015)
                {
                    why = "implausible scale/rotation";
                    return false;
                }

                if (std::fabs(m.tx) > 512.0 || std::fabs(m.ty - expected_ty) > 512.0)
                {
                    why = "implausible translation";
                    return false;
                }

                return true;
            }

            bool alignOneChannel(const image::Image &reference, const image::Image &target, double vertical_offset,
                                 const image::AffineTransform *seed, const std::string &label,
                                 image::AffineTransform &out, std::string &why)
            {
                // Raw channels are staggered by 3606 / 1804 rows; seed ECC with that
                // offset (or an existing calibration) and refine the residual.
                image::AffineTransform map;
                if (seed != nullptr)
                    map = *seed;
                else
                    map.ty = vertical_offset;

                image::AffineECCOptions opts;
                opts.pyramid_factors = {8, 4, 2};
                opts.iterations = {80, 50, 30};
                opts.epsilon = 1e-6;
                opts.gaussian_blur = true;
                opts.mask_out_of_bounds = true;
                opts.estimate_linear_part = true;

                logger->info("RDAS aligner: calibrating " + label + " (offset " + std::to_string(vertical_offset) +
                             (seed ? ", seeded)" : ")") + "...");

                const double rho = image::findAffineTransformECC(reference, target, map, opts);
                if (!std::isfinite(rho))
                {
                    why = "ECC did not converge";
                    return false;
                }
                if (!affineIsSane(map, vertical_offset, why))
                    return false;

                logger->info("RDAS aligner: " + label + " rho=" + std::to_string(rho) +
                             " a=" + std::to_string(map.a) + " b=" + std::to_string(map.b) +
                             " c=" + std::to_string(map.c) + " d=" + std::to_string(map.d) +
                             " tx=" + std::to_string(map.tx) + " ty=" + std::to_string(map.ty));

                out = map;
                return true;
            }

            bool calibrateScan(const RDASImages &images, bool vis1, RDASLayerCalibration &out, bool force,
                               RDASProcessResult &result)
            {
                const image::Image *ch1 = vis1 ? images.vis1_ch1 : images.vis2_ch1;
                const image::Image *ch2 = vis1 ? images.vis1_ch2 : images.vis2_ch2;
                const image::Image *ch3 = vis1 ? images.vis1_ch3 : images.vis2_ch3;
                if (!ch1 || !ch2 || !ch3)
                    return false;

                const std::string base = (vis1 ? "VIS1" : "VIS2");
                bool any = false;

                // Fit each layer independently; short captures may only have CH3 overlap.
                image::AffineTransform m2, m3;
                std::string why;

                if (force || !out.has_ch2)
                {
                    if (alignOneChannel(*ch1, *ch2, RDAS_CH2_OFFSET, out.has_ch2 ? &out.ch2 : nullptr,
                                        base + " CH2", m2, why))
                    {
                        out.ch2 = m2;
                        out.has_ch2 = true;
                        any = true;
                    }
                    else
                    {
                        result.error = base + " CH2: " + why;
                        logger->warn("RDAS aligner: " + result.error + (vis1 ? " (VIS1)" : " (VIS2)"));
                    }
                }

                if (force || !out.has_ch3)
                {
                    if (alignOneChannel(*ch1, *ch3, RDAS_CH3_OFFSET, out.has_ch3 ? &out.ch3 : nullptr,
                                        base + " CH3", m3, why))
                    {
                        out.ch3 = m3;
                        out.has_ch3 = true;
                        any = true;
                    }
                    else
                    {
                        result.error = base + " CH3: " + why;
                        logger->warn("RDAS aligner: " + result.error + (vis1 ? " (VIS1)" : " (VIS2)"));
                    }
                }

                return any;
            }

            /// Cheap content test: an empty scan (no frames decoded) has no non-zero pixels.
            bool imageHasContent(const image::Image &img)
            {
                if (img.width() == 0 || img.height() == 0 || img.channels() != 1 || img.raw_data() == nullptr)
                    return false;

                const size_t step_x = std::max<size_t>(1, img.width() / 64);
                const size_t step_y = std::max<size_t>(1, img.height() / 64);
                size_t checked = 0, non_zero = 0;

                if (img.depth() > 8)
                {
                    const uint16_t *data = (const uint16_t *)img.raw_data();
                    for (size_t y = 0; y < img.height(); y += step_y)
                        for (size_t x = 0; x < img.width(); x += step_x)
                        {
                            checked++;
                            if (data[y * img.width() + x] != 0)
                                non_zero++;
                        }
                }
                else
                {
                    const uint8_t *data = (const uint8_t *)img.raw_data();
                    for (size_t y = 0; y < img.height(); y += step_y)
                        for (size_t x = 0; x < img.width(); x += step_x)
                        {
                            checked++;
                            if (data[y * img.width() + x] != 0)
                                non_zero++;
                        }
                }

                return checked > 0 && (double)non_zero / (double)checked > 0.01;
            }

            bool validRoi(const RDASLayerCalibration &cal, const image::Image &aligned)
            {
                return cal.has_roi && cal.roi_x >= 0 && cal.roi_y >= 0 &&
                       cal.roi_width > 0 && cal.roi_height > 0 &&
                       (size_t)(cal.roi_x + cal.roi_width) <= aligned.width() &&
                       (size_t)(cal.roi_y + cal.roi_height) <= aligned.height();
            }

            void copyBlock(image::Image &dst, int dx, int dy, const image::Image &src, int sx, int sy, int w, int h)
            {
                if (w <= 0 || h <= 0)
                    return;

                if (dst.depth() > 8 && src.depth() > 8 && dst.channels() == 1 && src.channels() == 1)
                {
                    const uint16_t *src_px = (const uint16_t *)src.raw_data();
                    uint16_t *dst_px = (uint16_t *)dst.raw_data();
                    for (int y = 0; y < h; y++)
                        memcpy(dst_px + (size_t)(dy + y) * dst.width() + dx,
                               src_px + (size_t)(sy + y) * src.width() + sx,
                               (size_t)w * sizeof(uint16_t));
                }
                else if (dst.depth() <= 8 && src.depth() <= 8 && dst.channels() == 1 && src.channels() == 1)
                {
                    const uint8_t *src_px = (const uint8_t *)src.raw_data();
                    uint8_t *dst_px = (uint8_t *)dst.raw_data();
                    for (int y = 0; y < h; y++)
                        memcpy(dst_px + (size_t)(dy + y) * dst.width() + dx,
                               src_px + (size_t)(sy + y) * src.width() + sx,
                               (size_t)w);
                }
                else
                {
                    // Mixed depths / channels never happen for MSU-GS, keep a safe fallback.
                    for (int y = 0; y < h; y++)
                        for (int x = 0; x < w; x++)
                            dst.set(0, dx + x, dy + y, const_cast<image::Image &>(src).get(0, sx + x, sy + y));
                }
            }

            image::Image mergeChannel(const RDASCalibration &cal, const image::Image &left, const image::Image &right,
                                      bool &ok)
            {
                const int lw = std::min(cal.vis1.roi_width, (int)left.width() - cal.vis1.roi_x);
                const int lh = std::min(cal.vis1.roi_height, (int)left.height() - cal.vis1.roi_y);
                const int rw = std::min(cal.vis2.roi_width, (int)right.width() - cal.vis2.roi_x);
                const int rh = std::min(cal.vis2.roi_height, (int)right.height() - cal.vis2.roi_y);

                ok = lw > 0 && lh > 0 && rw > 0 && rh > 0;
                if (!ok)
                    return image::Image();

                const int width = lw + rw;
                const int height = std::max(lh, rh);
                image::Image merged(left.depth(), width, height, 1);
                copyBlock(merged, 0, 0, left, cal.vis1.roi_x, cal.vis1.roi_y, lw, lh);
                copyBlock(merged, lw, 0, right, cal.vis2.roi_x, cal.vis2.roi_y, rw, rh);
                return merged;
            }

            std::string saveAlignedProduct(const std::string &directory, const std::string &folder,
                                           const std::string &source, double timestamp,
                                           std::vector<image::Image> &channels,
                                           satdump::products::DataSet *dataset)
            {
                fs::path dir = fs::path(directory) / folder;
                std::error_code ec;
                fs::create_directories(dir, ec);

                satdump::products::ImageProduct product;
                product.instrument_name = "msugs_vis";
                if (!source.empty())
                    product.set_product_source(source);
                if (timestamp > 0)
                    product.set_product_timestamp(timestamp);

                for (size_t c = 0; c < channels.size(); c++)
                {
                    product.images.push_back({(int)c,
                                              "MSUGS-VIS-" + std::to_string(c + 1),
                                              std::to_string(c + 1),
                                              std::move(channels[c]),
                                              10,
                                              satdump::ChannelTransform().init_none()});
                }

                logger->info("RDAS aligner: saving " + dir.string() + " (" +
                             std::to_string(product.images[0].image.width()) + "x" +
                             std::to_string(product.images[0].image.height()) + ")");
                product.save(dir.string());

                channels.clear();

                if (dataset)
                {
                    if (std::find(dataset->products_list.begin(), dataset->products_list.end(), folder) ==
                        dataset->products_list.end())
                        dataset->products_list.push_back(folder);
                }

                return folder;
            }

            /// Read or create the calibration JSON, migrating legacy copies.
            bool ensureCalibrationJson(const std::string &satellite, nlohmann::ordered_json &json, fs::path &path)
            {
                path = getRDASCalibrationPath(satellite);
                bool write_back = false;
                fs::path migrated_from;

                if (!readCalibrationFile(path, json))
                {
                    // Try the legacy locations the separate aligner used to pollute.
                    for (const fs::path &candidate : calibrationCandidates(satellite))
                    {
                        if (candidate == path)
                            continue;

                        nlohmann::ordered_json legacy;
                        if (!readCalibrationFile(candidate, legacy))
                            continue;

                        RDASLayerCalibration check;
                        readLayerCalibration(legacy, scanIndex(satellite, true), check);
                        if (!check.has_affines())
                            continue;

                        json = legacy;
                        migrated_from = candidate;
                        write_back = true;
                        break;
                    }
                }

                if (!json.is_object())
                    json = nlohmann::ordered_json::object();

                // Fill in anything the defaults provide that the file lacks.
                const nlohmann::ordered_json seed = seededCalibration(satellite);
                bool seeded = false;
                for (auto &el : seed.items())
                    if (!json.contains(el.key()))
                    {
                        json[el.key()] = el.value();
                        write_back = true;
                        seeded = true;
                    }

                // Mark only freshly seeded files as current; migrated legacy files are
                // handled (and versioned) by loadRDASCalibration().
                if (seeded && migrated_from.empty() && !json.contains("_meta"))
                {
                    json["_meta"]["version"] = 4;
                    json["_meta"]["generator"] = "gobdump-inprocess";
                    json["_meta"]["source"] = "defaults";
                    json["_meta"]["updated"] = (double)std::time(nullptr);
                }

                if (write_back)
                {
                    if (writeCalibrationFile(path, json) && !migrated_from.empty())
                        logger->log(slog::LOG_INFO, "RDAS aligner: migrated calibration from " +
                                                       migrated_from.string() + " to " + path.string());
                }

                return true;
            }

            /// Align one scan's three raw channels into crop_height-row aligned channels.
            bool alignScan(const image::Image &ch1, const image::Image &ch2, const image::Image &ch3,
                           const RDASLayerCalibration &cal, std::vector<image::Image> &out)
            {
                if (!cal.has_affines() || ch1.height() <= (size_t)RDAS_CH2_OFFSET)
                    return false;

                const size_t width = ch1.width();
                const size_t height = ch1.height() - RDAS_CH2_OFFSET;

                out.clear();
                out.reserve(3);

                image::Image ch1_aligned = ch1;
                ch1_aligned.crop(0, 0, (int)width, (int)height);
                out.push_back(std::move(ch1_aligned));
                out.push_back(image::warpAffine(ch2, cal.ch2, width, height));
                out.push_back(image::warpAffine(ch3, cal.ch3, width, height));
                return true;
            }

        } // namespace

        std::string getRDASCalibrationPath(const std::string &satellite)
        {
            return (fs::path(satdump::user_path) / "rdas_aligner" / satellite / "config.json").string();
        }

        bool loadRDASCalibration(const std::string &satellite, RDASCalibration &calibration)
        {
            nlohmann::ordered_json json;
            fs::path path;
            ensureCalibrationJson(satellite, json, path);

            readLayerCalibration(json, scanIndex(satellite, true), calibration.vis1);
            readLayerCalibration(json, scanIndex(satellite, false), calibration.vis2);

            logger->log(slog::LOG_INFO, "RDAS aligner: calibration file " + path.string());

            // v4 = offset-seeded ECC. Unmarked files come from the old tool and are
            // migrated as-is; v2/v3 experimental fits are dropped and recalibrated.
            const bool has_meta = json.contains("_meta") && json["_meta"].is_object();
            const bool current = has_meta && json["_meta"].value("version", 0) >= 4;
            if (!current)
            {
                const bool had_affines = calibration.vis1.has_affines() || calibration.vis2.has_affines();

                if (had_affines && !has_meta)
                {
                    // Keep a safety copy, then promote the external calibration to v4.
                    fs::path backup = path;
                    backup.replace_filename("config.legacy.json");
                    std::error_code ec;
                    if (!fs::exists(backup))
                        fs::copy_file(path, backup, ec);
                    if (!ec)
                        logger->log(slog::LOG_INFO, "RDAS aligner: kept a copy of the legacy calibration at " +
                                                       backup.string());

                    json["_meta"]["version"] = 4;
                    json["_meta"]["generator"] = "gobdump-inprocess";
                    json["_meta"]["source"] = "migrated-legacy";
                    json["_meta"]["updated"] = (double)std::time(nullptr);
                    if (writeCalibrationFile(path, json))
                        logger->log(slog::LOG_INFO, "RDAS aligner: migrated the existing calibration to v4 for " +
                                                       satellite);
                    return true;
                }

                calibration.vis1.has_ch2 = calibration.vis1.has_ch3 = false;
                calibration.vis2.has_ch2 = calibration.vis2.has_ch3 = false;
                if (had_affines)
                    logger->info("RDAS aligner: calibration format updated, recalibrating " + satellite);
                return false;
            }

            if (calibration.vis1.has_affines() || calibration.vis2.has_affines())
                logger->log(slog::LOG_INFO, std::string("RDAS aligner: reusing calibration for ") + satellite +
                                                (calibration.vis1.has_affines() ? " VIS1" : "") +
                                                (calibration.vis2.has_affines() ? " VIS2" : ""));
            return calibration.vis1.has_affines() || calibration.vis2.has_affines();
        }

        bool calibrateRDASLayers(const RDASImages &images, const std::string &satellite,
                                 RDASCalibration &calibration, bool force, RDASProcessResult &result)
        {
            nlohmann::ordered_json json;
            fs::path canon;
            ensureCalibrationJson(satellite, json, canon);

            // Drop stale v2/v3 matrices before refitting; keep the ROIs.
            const bool stale_file = !(json.contains("_meta") && json["_meta"].is_object() &&
                                      json["_meta"].value("version", 0) >= 4);
            if (stale_file)
                for (const std::string &index : {scanIndex(satellite, true), scanIndex(satellite, false)})
                    if (json.contains(index) && json[index].is_object())
                    {
                        json[index].erase("CH2");
                        json[index].erase("CH3");
                    }

            bool changed = false;

            // Only calibrate scans that actually decoded something.
            const bool can_vis1 = images.vis1_ch1 && images.vis1_ch2 && images.vis1_ch3 && imageHasContent(*images.vis1_ch1);
            const bool can_vis2 = images.vis2_ch1 && images.vis2_ch2 && images.vis2_ch3 && imageHasContent(*images.vis2_ch1);

            if (can_vis1 && (force || !calibration.vis1.has_affines()))
            {
                RDASLayerCalibration updated = calibration.vis1;
                if (calibrateScan(images, true, updated, force, result))
                {
                    // Keep the ROI that is already configured.
                    if (!updated.has_roi && calibration.vis1.has_roi)
                    {
                        updated.roi_x = calibration.vis1.roi_x;
                        updated.roi_y = calibration.vis1.roi_y;
                        updated.roi_width = calibration.vis1.roi_width;
                        updated.roi_height = calibration.vis1.roi_height;
                        updated.has_roi = true;
                    }
                    calibration.vis1 = updated;
                    writeLayerCalibration(json, scanIndex(satellite, true), calibration.vis1, true, true);
                    changed = true;
                }
            }
            else if (!calibration.vis1.has_affines())
            {
                logger->info("RDAS aligner: VIS1 scan has no usable data, skipping calibration");
            }

            if (can_vis2 && (force || !calibration.vis2.has_affines()))
            {
                RDASLayerCalibration updated = calibration.vis2;
                if (calibrateScan(images, false, updated, force, result))
                {
                    if (!updated.has_roi && calibration.vis2.has_roi)
                    {
                        updated.roi_x = calibration.vis2.roi_x;
                        updated.roi_y = calibration.vis2.roi_y;
                        updated.roi_width = calibration.vis2.roi_width;
                        updated.roi_height = calibration.vis2.roi_height;
                        updated.has_roi = true;
                    }
                    calibration.vis2 = updated;
                    writeLayerCalibration(json, scanIndex(satellite, false), calibration.vis2, true, true);
                    changed = true;
                }
            }
            else if (!calibration.vis2.has_affines())
            {
                logger->info("RDAS aligner: VIS2 scan has no usable data, skipping calibration");
            }

            if (changed)
            {
                json["_meta"]["version"] = 4;
                json["_meta"]["generator"] = "gobdump-inprocess";
                json["_meta"]["source"] = "recalibrated";
                json["_meta"].erase("translation_only");
                json["_meta"]["updated"] = (double)std::time(nullptr);

                if (!writeCalibrationFile(canon, json))
                {
                    result.error = "could not save calibration";
                    return false;
                }
                result.calibrated = true;
                logger->log(slog::LOG_INFO, "RDAS aligner: saved calibration to " + canon.string());
            }

            result.calibration_path = canon.string();
            return changed;
        }

        RDASProcessResult processRDASLayers(const RDASImages &images, const RDASProcessOptions &options)
        {
            RDASProcessResult result;
            if (options.satellite.empty())
            {
                result.error = "no satellite selected";
                logger->warn("RDAS aligner: no satellite selected, skipping layer alignment");
                return result;
            }

            // Nothing decoded means nothing to align or calibrate.
            const bool vis1_has_data = images.vis1_ch1 && imageHasContent(*images.vis1_ch1);
            const bool vis2_has_data = images.vis2_ch1 && imageHasContent(*images.vis2_ch1);
            if (!vis1_has_data && !vis2_has_data)
            {
                result.error = "no decoded MSU-GS scan lines";
                logger->warn("RDAS aligner: no decoded MSU-GS scan lines, skipping layer alignment");
                return result;
            }

            RDASCalibration calibration;
            bool have_calibration = loadRDASCalibration(options.satellite, calibration);
            result.calibration_path = getRDASCalibrationPath(options.satellite);

            const bool need_vis1 = images.vis1_ch1 && !calibration.vis1.has_affines();
            const bool need_vis2 = images.vis2_ch1 && !calibration.vis2.has_affines();

            if (!have_calibration || options.recalibrate || need_vis1 || need_vis2)
            {
                if (!have_calibration)
                    logger->info("RDAS aligner: no calibration for " + options.satellite + ", calibrating (this takes a couple of minutes)...");
                else if (options.recalibrate)
                    logger->info("RDAS aligner: recalibrating " + options.satellite + " (forced)...");
                else
                    logger->info("RDAS aligner: calibration is incomplete for " + options.satellite + ", calibrating...");

                calibrateRDASLayers(images, options.satellite, calibration, options.recalibrate, result);
            }

            std::vector<image::Image> vis1_aligned, vis2_aligned;
            bool vis1_ok = false, vis2_ok = false;

            if (calibration.vis1.has_affines() && images.vis1_ch1 && images.vis1_ch2 && images.vis1_ch3 &&
                imageHasContent(*images.vis1_ch1))
            {
                logger->info("RDAS aligner: aligning VIS1...");
                vis1_ok = alignScan(*images.vis1_ch1, *images.vis1_ch2, *images.vis1_ch3, calibration.vis1, vis1_aligned);
            }

            if (calibration.vis2.has_affines() && images.vis2_ch1 && images.vis2_ch2 && images.vis2_ch3 &&
                imageHasContent(*images.vis2_ch1))
            {
                logger->info("RDAS aligner: aligning VIS2...");
                vis2_ok = alignScan(*images.vis2_ch1, *images.vis2_ch2, *images.vis2_ch3, calibration.vis2, vis2_aligned);
            }

            bool can_merge = vis1_ok && vis2_ok;
            for (int c = 0; c < 3 && can_merge; c++)
                can_merge = validRoi(calibration.vis1, vis1_aligned[c]) && validRoi(calibration.vis2, vis2_aligned[c]);

            if (can_merge)
            {
                logger->info("RDAS aligner: merge ROIs found, creating the merged MSU-GS product...");
                std::vector<image::Image> merged;
                merged.reserve(3);
                for (int c = 0; c < 3; c++)
                {
                    bool merge_ok = true;
                    image::Image channel = mergeChannel(calibration, vis1_aligned[c], vis2_aligned[c], merge_ok);
                    if (!merge_ok)
                        throw satdump_exception("RDAS aligner: merge ROI is outside the aligned image!");
                    merged.push_back(std::move(channel));
                    vis1_aligned[c].clear();
                    vis2_aligned[c].clear();
                }

                const double ts = options.vis1_timestamp > 0 ? options.vis1_timestamp : options.vis2_timestamp;
                result.products.push_back(saveAlignedProduct(options.output_directory, "MSUGS_VIS_Aligned",
                                                             options.product_source, ts, merged, options.dataset));
                result.merged = true;
            }

            if (!result.merged)
            {
                if (vis1_ok)
                {
                    result.products.push_back(saveAlignedProduct(options.output_directory, "MSUGS_VIS-1_Aligned",
                                                                 options.product_source, options.vis1_timestamp,
                                                                 vis1_aligned, options.dataset));
                }

                if (vis2_ok)
                {
                    result.products.push_back(saveAlignedProduct(options.output_directory, "MSUGS_VIS-2_Aligned",
                                                                 options.product_source, options.vis2_timestamp,
                                                                 vis2_aligned, options.dataset));
                }
            }

            result.success = !result.products.empty();
            if (!result.success && result.error.empty())
                result.error = have_calibration || result.calibrated ? "no scan data to align" : "calibration failed";

            return result;
        }
    } // namespace msugs
} // namespace elektro_arktika
