#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace satdump
{
    namespace utils
    {
        class FilesIteratorItem
        {
        public:
            const std::string name;
            FilesIteratorItem(std::string name) : name(name) {}
            virtual std::vector<uint8_t> getPayload() = 0;

            // Identification only needs headers; folder scans must not slurp whole files.
            virtual std::vector<uint8_t> getPayloadPrefix(size_t max_size)
            {
                std::vector<uint8_t> v = getPayload();
                if (v.size() > max_size)
                    v.resize(max_size);
                return v;
            }
        };

        class FilesIterator
        {
        public:
            virtual bool getNext(std::shared_ptr<FilesIteratorItem> &v) = 0;
            virtual void reset() = 0;
        };
    } // namespace utils
} // namespace satdump