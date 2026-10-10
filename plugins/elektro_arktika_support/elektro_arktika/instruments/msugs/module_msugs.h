#pragma once

#include "msu_ir_reader.h"
#include "msu_vis_reader.h"
#include "pipeline/modules/base/filestream_to_filestream.h"
#include "pipeline/modules/instrument_utils.h"

#include <memory>

namespace elektro_arktika
{
    namespace msugs
    {
        class MSUGSDecoderModule : public satdump::pipeline::base::FileStreamToFileStreamModule
        {
        protected:
            // Readers
            MSUVISReader vis1_reader;
            MSUVISReader vis2_reader;
            MSUVISReader vis3_reader;
            std::unique_ptr<MSUIRReader> infr_reader; // Only allocated when IR decoding is enabled

            // Statuses
            instrument_status_t channels_statuses[10] = {DECODING, DECODING, DECODING, DECODING, DECODING, DECODING, DECODING, DECODING, DECODING, DECODING};

            // For counter correction
            bool apply_correction;

            bool is_arktika = false;
            int sat_num = 0;

            bool decode_ir = false;
            bool project_vis2 = false;
            bool fill_missing = false;
            size_t max_fill_lines = 50;

#ifdef ENABLE_RDAS_ALIGNER
            bool recalibrate_aligner = false;
#endif

        public:
            MSUGSDecoderModule(std::string input_file, std::string output_file_hint, nlohmann::json parameters);
            void process();
            void drawUI(bool window);

        public:
            static std::string getID();
            virtual std::string getIDM() { return getID(); };
            static nlohmann::json getParams() { return {}; } // TODOREWORK
            static std::shared_ptr<ProcessingModule> getInstance(std::string input_file, std::string output_file_hint, nlohmann::json parameters);
        };
    } // namespace msugs
} // namespace elektro_arktika