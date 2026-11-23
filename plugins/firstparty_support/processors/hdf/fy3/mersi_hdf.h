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
        class MERSIHdfProcessor : public FirstPartyProductProcessor
        {
        private:
            std::shared_ptr<satdump::products::ImageProduct> mersi_products;

            enum mersi_type_t
            {
                MERSI_NONE,
                MERSI_1,
                MERSI_2,
                MERSI_LL,
                MERSI_RM,
                MERSI_3,
            };

            mersi_type_t mersi_type = MERSI_NONE;

            // Multi-granule accumulators for MERSI-2 (FY-3D)
            std::array<std::vector<uint16_t>, 25> mersi2_data;
            int mersi2_width = 0;
            int mersi2_lines = 0;
            int mersi2_gcps_cnt = 0;
            nlohmann::json mersi2_gcps;
            std::vector<double> mersi2_timestamps;
            std::string mersi2_sat_name = "FY-3D";

        public:
            void ingestFile(std::vector<uint8_t> file);
            std::shared_ptr<satdump::products::Product> getProduct();
        };
    } // namespace firstparty
} // namespace satdump
