#include "hrit_generic.h"
#include "libs/bzlib_utils.h"
#include "xrit/xrit_file.h"

#include "logger.h"
#include "xrit/fy4/decomp.h"
#include "xrit/gk2a/decomp.h"
#include "xrit/goes/goes_headers.h"
#include "xrit/identify.h"
#include "xrit/msg/decomp.h"
#include "xrit/msg/msg_headers.h"

#include "image/io.h"
#include <memory>
#include <string>

namespace satdump
{
    namespace firstparty
    {
        void HRITGenericProcessor::ingestFile(std::vector<uint8_t> vec)
        {
            // EUMETCast wraps HRIT/LRIT segments in bzip2 (.bz2)
            if (vec.size() > 4 && vec.at(0) == 0x42 && vec.at(1) == 0x5a && vec.at(2) == 0x68)
            {
                std::vector<uint8_t> decomp(32 * 1024 * 1024);
                size_t source_offset = 0, dest_offset = 0;
                int bz2_result = BZ_OK;
                while (bz2_result == BZ_OK && source_offset < vec.size())
                {
                    if (dest_offset == decomp.size())
                        decomp.resize(decomp.size() * 2);
                    unsigned int dest_length = decomp.size() - dest_offset;
                    unsigned int consumed = 0;
                    bz2_result = BZ2_bzBuffToBuffDecompress_M((char *)decomp.data() + dest_offset, &dest_length, (char *)vec.data() + source_offset, vec.size() - source_offset, &consumed, 0, 0);
                    source_offset += consumed;
                    dest_offset += dest_length;
                }
                decomp.resize(dest_offset);
                vec = std::move(decomp);
            }

            xrit::XRITFile file;
            file.lrit_data = std::move(vec);
            file.parseHeaders();

            xrit::XRITFileInfo finfo = xrit::identifyXRITFIle(file);

            // Check if this is image data
            if (file.hasHeader<xrit::ImageStructureRecord>())
            {
                if (finfo.type == xrit::XRIT_ELEKTRO_MSUGS || finfo.type == xrit::XRIT_MSG_SEVIRI)
                    xrit::msg::decompressMsgHritFileIfRequired(file);
                else if (finfo.type == xrit::XRIT_GK2A_AMI)
                    xrit::gk2a::decompressGK2AHritFileIfRequired(file);
                else if (finfo.type == xrit::XRIT_FY4_AGRI)
                    xrit::fy4::decompressFY4HritFileIfRequired(file);
            }

            processor_test.push(finfo, file);
        }

        std::shared_ptr<satdump::products::Product> HRITGenericProcessor::getProduct()
        {
            processor_test.flush();

            if (processor_test.in_memory_products.size())
                return processor_test.in_memory_products.begin()->second;
            else
                return nullptr;
        }
    } // namespace firstparty
} // namespace satdump
