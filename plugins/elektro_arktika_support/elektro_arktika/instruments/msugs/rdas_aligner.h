#pragma once

/// In-process MSU-GS layer alignment, replacing the old rdas_layer_aligner binary.

#include "image/affine_align.h"
#include "image/image.h"
#include "nlohmann/json.hpp"

#include <string>
#include <vector>

namespace satdump
{
    namespace products
    {
        struct DataSet;
    }
}

namespace elektro_arktika
{
    namespace msugs
    {
        /// Nominal MSU-GS VIS detector stagger; the ECC fit is seeded with these.
        static constexpr int RDAS_CH2_OFFSET = 3606;
        static constexpr int RDAS_CH3_OFFSET = 1804;

        /// Calibration of one scan direction (VIS1 or VIS2).
        struct RDASLayerCalibration
        {
            image::AffineTransform ch2; // aligned -> raw CH2
            image::AffineTransform ch3; // aligned -> raw CH3
            bool has_ch2 = false;
            bool has_ch3 = false;

            bool has_roi = false;
            int roi_x = 0;
            int roi_y = 0;
            int roi_width = 0;
            int roi_height = 0;

            bool has_affines() const { return has_ch2 && has_ch3; }
        };

        struct RDASCalibration
        {
            RDASLayerCalibration vis1;
            RDASLayerCalibration vis2;
        };

        /// Raw MSU-GS VIS channels, as decoded by the module.
        struct RDASImages
        {
            const image::Image *vis1_ch1 = nullptr;
            const image::Image *vis1_ch2 = nullptr;
            const image::Image *vis1_ch3 = nullptr;
            const image::Image *vis2_ch1 = nullptr;
            const image::Image *vis2_ch2 = nullptr;
            const image::Image *vis2_ch3 = nullptr;
        };

        struct RDASProcessOptions
        {
            std::string satellite; ///< "L2", "L3", "M1", ...
            bool recalibrate = false;
            std::string output_directory;
            std::string product_source = "ELEKTRO-L";
            double vis1_timestamp = 0;
            double vis2_timestamp = 0;
            satdump::products::DataSet *dataset = nullptr;
        };

        struct RDASProcessResult
        {
            bool success = false;
            bool calibrated = false;
            bool merged = false;
            std::string calibration_path;
            std::vector<std::string> products;
            std::string error;
        };

        /// <user config>/rdas_aligner/<satellite>/config.json, migrating legacy copies.
        std::string getRDASCalibrationPath(const std::string &satellite);

        /// Load, seed or migrate the calibration. Returns true if a matrix was usable.
        bool loadRDASCalibration(const std::string &satellite, RDASCalibration &calibration);

        /// Fit CH2/CH3 of both scans and persist the result. Returns true if updated.
        bool calibrateRDASLayers(const RDASImages &images, const std::string &satellite,
                                 RDASCalibration &calibration, bool force, RDASProcessResult &result);

        /// Align, merge and save the MSU-GS products, appending names to options.dataset.
        RDASProcessResult processRDASLayers(const RDASImages &images, const RDASProcessOptions &options);
    } // namespace msugs
} // namespace elektro_arktika
