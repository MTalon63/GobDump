#pragma once

#include "pipeline/modules/base/filestream_to_filestream.h"
#include "xrit/processor/xrit_channel_processor.h"
#include "xrit/xrit_file.h"

namespace elektro
{
    namespace xrit
    {
        class ELEKTROXRITDataDecoderModule : public satdump::pipeline::base::FileStreamToFileStreamModule
        {
        protected:
            satdump::xrit::XRITChannelProcessor processor;

            std::string directory;

            void processXRITFile(satdump::xrit::XRITFile &file);

        public:
            ELEKTROXRITDataDecoderModule(std::string input_file, std::string output_file_hint, nlohmann::json parameters);
            ~ELEKTROXRITDataDecoderModule();
            void process();
            void drawUI(bool window);

        public:
            static std::string getID();
            virtual std::string getIDM() { return getID(); };
            static nlohmann::json getParams() { return {}; } // TODOREWORK
            static std::shared_ptr<ProcessingModule> getInstance(std::string input_file, std::string output_file_hint, nlohmann::json parameters);
        };
    } // namespace xrit
} // namespace elektro