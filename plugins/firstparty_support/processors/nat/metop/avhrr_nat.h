#pragma once

#include "../../processor.h"
#include "nlohmann/json.hpp"
#include "products/image_product.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace satdump
{
    namespace firstparty
    {
        class AVHRRNatProcessor : public FirstPartyProductProcessor
        {
        private:
            // Accumulated across all ingested NAT records (a pass can be split over several files);
            // the product is built once in getProduct() rather than per record.
            int image_width = 0;
            std::array<std::vector<uint16_t>, 6> avhrr_data;
            int number_of_lines = 0;
            int all_gcps = 0;
            nlohmann::json gcps_all;
            std::vector<double> timestamps;
            std::string sat_id = "M00";

        public:
            void ingestFile(std::vector<uint8_t> file);
            std::shared_ptr<satdump::products::Product> getProduct();
        };
    } // namespace firstparty
} // namespace satdump
