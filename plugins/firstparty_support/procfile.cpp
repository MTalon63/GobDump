#include "procfile.h"
#include "core/exception.h"
#include "logger.h"
#include "product_info.h"
#include "type.h"
#include "utils/file/file_iterators.h"
#include "utils/file/folder_file_iterators.h"
#include "utils/file/zip_file_iterators.h"
#include <H5Cpp.h>
#include <algorithm>
#include <cmath>
#include <exception>
#include <memory>
#include <vector>

namespace satdump
{
    namespace firstparty
    {
        FirstPartyProductInfo identifyFirstPartyProductFile(std::string fpath)
        {
            // Identify the product to process. If a single file, just work off that
            // and figure out if it's multi-file, if it's a zip just identify the first
            // one we can.

            FirstPartyProductInfo toproc_info;

            bool is_zip = std::filesystem::path(fpath).extension() == ".zip";
            bool is_file = std::filesystem::is_regular_file(fpath);

            if (is_zip)
            {
                // Test if the ZIP itself is a product
                std::shared_ptr<satdump::utils::FilesIteratorItem> _f = std::make_shared<satdump::utils::FolderFileIteratorItem>(fpath);
                toproc_info = parseFirstPartyInfo(_f);

                if (toproc_info.type == PRODUCT_NONE)
                {
                    satdump::utils::ZipFilesIterator fit(fpath);
                    std::shared_ptr<satdump::utils::FilesIteratorItem> f;

                    while (fit.getNext(f))
                    {
                        if (f)
                        {
                            toproc_info = parseFirstPartyInfo(f);
                            if (toproc_info.type != PRODUCT_NONE)
                                break;
                        }
                    }
                }
            }
            else if (is_file)
            {
                std::shared_ptr<satdump::utils::FilesIteratorItem> f = std::make_shared<satdump::utils::FolderFileIteratorItem>(fpath);
                toproc_info = parseFirstPartyInfo(f);
            }
            else
            {
                logger->error("Not a file or ZIP! => " + fpath);
            }

            return toproc_info;
        }

        std::shared_ptr<products::Product> processFirstPartyProductFile(FirstPartyProductInfo toproc_info, std::string fpath)
        {
            // Check it is valid & setup processor

            if (toproc_info.type == PRODUCT_NONE)
                throw satdump_exception("No product present in file " + fpath + "!");

            logger->debug("Processing product : " + toproc_info.name);

            auto processor = getProcessorForProduct(toproc_info);

            // Run all required files through.
            bool is_zip = std::filesystem::path(fpath).extension() == ".zip";

            if (toproc_info.group_id.size() || is_zip) // If we have a group, we need to iterate through all of them (also do so if we got a single one in a file)
            {
                logger->trace("Group : " + toproc_info.group_id);

                std::shared_ptr<satdump::utils::FilesIterator> fit;
                if (is_zip)
                    fit = std::make_unique<satdump::utils::ZipFilesIterator>(fpath);
                else
                    fit = std::make_unique<satdump::utils::FolderFilesIterator>(std::filesystem::path(fpath).parent_path().string());

                struct GroupItem
                {
                    FirstPartyProductInfo info;
                    std::shared_ptr<satdump::utils::FilesIteratorItem> item;
                };

                std::vector<GroupItem> matched;
                std::shared_ptr<satdump::utils::FilesIteratorItem> f;
                while (fit->getNext(f))
                {
                    if (!f)
                        continue;

                    auto finfo = parseFirstPartyInfo(f);
                    if (toproc_info.group_id == finfo.group_id)
                        matched.push_back({finfo, f});
                }

                std::sort(matched.begin(), matched.end(),
                          [](const GroupItem &a, const GroupItem &b)
                          {
                              if (a.info.timestamp != b.info.timestamp)
                                  return a.info.timestamp < b.info.timestamp;
                              return a.item->name < b.item->name;
                          });

                // Keep only the contiguous pass within +-30 minutes (1800s) containing toproc_info.timestamp
                if (toproc_info.group_id.rfind("contig_", 0) == 0 && !matched.empty())
                {
                    size_t seed = 0;
                    double best_diff = std::abs(matched.at(0).info.timestamp - toproc_info.timestamp);
                    for (size_t idx = 1; idx < matched.size(); idx++)
                    {
                        double diff = std::abs(matched.at(idx).info.timestamp - toproc_info.timestamp);
                        if (diff < best_diff)
                        {
                            best_diff = diff;
                            seed = idx;
                        }
                    }

                    double max_gap = (toproc_info.group_id.rfind("contig_eum_", 0) == 0) ? 90.0 : 240.0;

                    size_t lo = seed;
                    while (lo > 0 && (matched.at(lo).info.timestamp - matched.at(lo - 1).info.timestamp) <= max_gap && (matched.at(seed).info.timestamp - matched.at(lo - 1).info.timestamp) <= 1800.0)
                        lo--;

                    size_t hi = seed;
                    while (hi + 1 < matched.size() && (matched.at(hi + 1).info.timestamp - matched.at(hi).info.timestamp) <= max_gap &&
                           (matched.at(hi + 1).info.timestamp - matched.at(seed).info.timestamp) <= 1800.0)
                        hi++;

                    matched = std::vector<GroupItem>(matched.begin() + lo, matched.begin() + hi + 1);
                }

                for (auto &g : matched)
                {
                    logger->trace(" - " + g.item->name);

                    try
                    {
                        processor->ingestFile(g.item->getPayload());
                    }
                    catch (std::exception &e)
                    {
                        logger->error("Error processing file! %s", e.what());
                    }
                    catch (H5::Exception &e)
                    {
                        logger->error("Error processing file! %s", e.getCDetailMsg());
                    }
                }
            }
            else
            {
                std::unique_ptr<satdump::utils::FilesIteratorItem> f = std::make_unique<satdump::utils::FolderFileIteratorItem>(fpath);
                logger->trace(" - " + f->name);
                processor->ingestFile(f->getPayload());
            }

            // Finally, return product
            auto product = processor->getProduct();
            // product->save("/tmp/satdump_firstparty/test");
            return product;
        }
    } // namespace firstparty
} // namespace satdump
