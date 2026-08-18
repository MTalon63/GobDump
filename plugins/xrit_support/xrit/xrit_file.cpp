#include "xrit_file.h"
#include "logger.h"
#include <algorithm>

namespace satdump
{
    namespace xrit
    {
        // `safe` used to gate the only bounds check, and defaulted to false on 3 of 6 call paths - while
        // total_header_length is an attacker-chosen uint32 that can far exceed what actually arrived.
        // Checking is now unconditional; the parameter is kept so callers need not change.
        void XRITFile::parseHeaders(bool /*safe*/)
        {
            all_headers.clear();

            if (lrit_data.size() < 16) // PrimaryHeader itself reads 16 bytes
                return;

            PrimaryHeader primary_header = getHeader<PrimaryHeader>();

            // Get all other headers
            uint64_t hdr_len = std::min<uint64_t>(primary_header.total_header_length, lrit_data.size());
            for (uint64_t i = 0; i + 3 <= hdr_len;)
            {
                uint8_t type = lrit_data[i];
                uint16_t record_length = lrit_data[i + 1] << 8 | lrit_data[i + 2];

                // < 3 also covers the old == 0 case, and stops the record constructors from
                // building a reversed &data[3]..&data[record_length] iterator range.
                if (record_length < 3 || i + record_length > hdr_len)
                    break;

                all_headers.emplace(std::pair<int, int>(type, (int)i));

                i += record_length;
            }

            // Check if this has a filename
            if (all_headers.count(AnnotationRecord::TYPE) > 0)
            {
                AnnotationRecord annotation_record = getHeader<AnnotationRecord>();
                try
                {
                    filename = std::string(annotation_record.annotation_text.data());

                    std::replace(filename.begin(), filename.end(), '/', '_');  // Safety
                    std::replace(filename.begin(), filename.end(), '\\', '_'); // Safety

                    for (char &c : filename) // Strip invalid chars
                    {
                        if (c < 33)
                            c = '_';
                    }
                }
                catch (std::exception &e)
                {
                    filename.clear();
                }
            }

            total_header_length = primary_header.total_header_length;
        }
    } // namespace xrit
} // namespace satdump