#include "cli/rdas_align.h"

#include "core/resources.h"
#include "elektro_arktika/instruments/msugs/rdas_aligner.h"
#include "logger.h"
#include "nlohmann/json_utils.h"
#include "products/dataset.h"
#include "products/image_product.h"
#include "products/product.h"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

namespace satdump
{
    namespace fs = std::filesystem;

    namespace
    {
        // gobdump's CLI teardown can hang while unloading plugin DLLs; bypass it.
        [[noreturn]] void exitNow(int code)
        {
#ifdef _WIN32
            TerminateProcess(GetCurrentProcess(), (UINT)code);
#endif
            std::_Exit(code);
        }

        // Restore raw channel transforms in the product CBOR (PNGs untouched).
        bool restoreRawTransforms(products::ImageProduct &product, const std::string &satellite, bool vis1,
                                  const fs::path &product_dir)
        {
            if (satellite.empty())
                return false;

            std::string prefix;
            prefix += (char)std::tolower(satellite[0]);
            prefix += satellite.substr(1);
            const std::string resource = "elektro/" + prefix + "_cfg.json";
            if (!resources::resourceExists(resource))
                return false;

            nlohmann::json cfg;
            try
            {
                cfg = nlohmann::json::parse(loadJsonFile(resources::getResourcePath(resource)).dump());
            }
            catch (std::exception &)
            {
                return false;
            }

            const std::string key = vis1 ? "msu_vis_1" : "msu_vis_2";
            if (!cfg.contains(key) || !cfg[key].is_object())
                return false;
            if (!product.contents.contains("images") || !product.contents["images"].is_array())
                return false;

            bool changed = false;
            for (auto &image : product.contents["images"])
            {
                if (!image.is_object())
                    continue;

                nlohmann::json transform = {{"type", 1}};
                const std::string channel = image.value("name", std::string());
                if (!channel.empty() && cfg[key].contains(channel) && !cfg[key][channel].is_null())
                    transform = cfg[key][channel];

                if (!image.contains("transform") || image["transform"] != transform)
                {
                    image["transform"] = transform;
                    changed = true;
                }
            }

            if (!changed)
                return false;

            const std::vector<uint8_t> cbor = nlohmann::json::to_cbor(product.contents);
            std::ofstream out((product_dir / "product.cbor").string(), std::ios::binary | std::ios::trunc);
            out.write((const char *)cbor.data(), (std::streamsize)cbor.size());
            return out.good();
        }
    } // namespace

    void RDASAlignCmdHandler::reg(CLI::App *app)
    {
        CLI::App *sub = app->add_subcommand("rdas_align",
                                            "Align and merge the MSU-GS VIS layers of an existing RDAS output directory");
        sub->add_option("directory", directory, "Dataset directory containing MSUGS_VIS1 / MSUGS_VIS2")->required();
        sub->add_option("-s,--satellite", satellite, "Satellite key (L2, L3, L5, M1, M2...)")->default_val("L2");
        sub->add_flag("--recalibrate", recalibrate, "Force a new layer calibration before aligning");
    }

    void RDASAlignCmdHandler::run(CLI::App *, CLI::App *, bool)
    {
        const fs::path dir(directory);
        if (!fs::is_directory(dir))
        {
            logger->error("RDAS aligner: " + directory + " is not a directory");
            exit(1);
        }

        auto loadScan = [&dir](const char *folder) -> std::shared_ptr<products::ImageProduct> {
            const fs::path product_dir = dir / folder;
            if (!fs::exists(product_dir / "product.cbor"))
            {
                logger->warn(std::string("RDAS aligner: no ") + folder + " product in " + dir.string());
                return nullptr;
            }

            try
            {
                auto product = std::dynamic_pointer_cast<products::ImageProduct>(
                    products::loadProduct(product_dir.string()));
                if (!product || product->images.size() < 3)
                {
                    logger->error(std::string("RDAS aligner: ") + folder + " is not a usable MSU-GS image product");
                    return nullptr;
                }
                return product;
            }
            catch (std::exception &e)
            {
                logger->error(std::string("RDAS aligner: could not load ") + folder + ": " + e.what());
                return nullptr;
            }
        };

        auto vis1 = loadScan("MSUGS_VIS1");
        auto vis2 = loadScan("MSUGS_VIS2");
        if (!vis1 && !vis2)
        {
            logger->error("RDAS aligner: no MSU-GS VIS products found in " + dir.string());
            exit(1);
        }

        // Repair outputs written by a build that dropped the raw channel transforms.
        if (vis1 && restoreRawTransforms(*vis1, satellite, true, dir / "MSUGS_VIS1"))
            logger->log(slog::LOG_INFO, "RDAS aligner: restored MSU-GS channel transforms in MSUGS_VIS1");
        if (vis2 && restoreRawTransforms(*vis2, satellite, false, dir / "MSUGS_VIS2"))
            logger->log(slog::LOG_INFO, "RDAS aligner: restored MSU-GS channel transforms in MSUGS_VIS2");

        products::DataSet dataset;
        bool have_dataset = false;
        if (fs::exists(dir / "dataset.json"))
        {
            try
            {
                dataset.load((dir / "dataset.json").string());
                have_dataset = true;
            }
            catch (std::exception &e)
            {
                logger->warn(std::string("RDAS aligner: could not read dataset.json: ") + e.what());
            }
        }

        elektro_arktika::msugs::RDASImages images;
        if (vis1)
        {
            images.vis1_ch1 = &vis1->images[0].image;
            images.vis1_ch2 = &vis1->images[1].image;
            images.vis1_ch3 = &vis1->images[2].image;
        }
        if (vis2)
        {
            images.vis2_ch1 = &vis2->images[0].image;
            images.vis2_ch2 = &vis2->images[1].image;
            images.vis2_ch3 = &vis2->images[2].image;
        }

        elektro_arktika::msugs::RDASProcessOptions options;
        options.satellite = satellite;
        options.recalibrate = recalibrate;
        options.output_directory = dir.string();
        options.product_source = have_dataset ? dataset.satellite_name : "ELEKTRO-L";
        options.vis1_timestamp = vis1 && vis1->has_product_timestamp() ? vis1->get_product_timestamp() : 0;
        options.vis2_timestamp = vis2 && vis2->has_product_timestamp() ? vis2->get_product_timestamp() : 0;
        options.dataset = have_dataset ? &dataset : nullptr;

        const auto result = elektro_arktika::msugs::processRDASLayers(images, options);

        if (have_dataset && result.success)
        {
            dataset.save(dir.string());
            logger->log(slog::LOG_INFO, "RDAS aligner: updated " + (dir / "dataset.json").string());
        }

        if (!result.success)
        {
            logger->error("RDAS aligner: " + (result.error.empty() ? std::string("alignment failed") : result.error));
            // Avoid the long-standing CLI teardown hang: all files are already closed.
            exitNow(1);
        }

        logger->log(slog::LOG_INFO, "RDAS aligner: done, wrote " + std::to_string(result.products.size()) + " product(s)");
        exitNow(0);
    }
} // namespace satdump
