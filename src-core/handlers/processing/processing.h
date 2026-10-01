#pragma once

#include "handlers/handler.h"
#include "pipeline/module.h"
#include "pipeline/pipeline.h"
#include <mutex>
#include <thread>

// TODOREWORK, move into plugin? Or Core?
namespace satdump
{
    namespace handlers
    {
        class OffProcessingHandler : public Handler
        {
        private:
            std::shared_ptr<std::vector<std::shared_ptr<pipeline::ProcessingModule>>> ui_call_list;
            std::shared_ptr<std::mutex> ui_call_list_mutex;

            void process(pipeline::Pipeline downlink_pipeline, std::string input_level, std::string input_file, std::string output_file, nlohmann::json parameters);

            std::thread proc_thread;

            std::string pipeline_name = "Pipeline";
            std::mutex pipeline_name_mtx;

        public:
            OffProcessingHandler(pipeline::Pipeline downlink_pipeline, std::string input_level, std::string input_file, std::string output_file, nlohmann::json parameters);

            OffProcessingHandler(std::string downlink_pipeline, std::string input_level, std::string input_file, std::string output_file, nlohmann::json parameters);

            ~OffProcessingHandler();

            // The Rest
            void drawMenu();
            void drawContents(ImVec2 win_size);

            std::string getName()
            {
                std::lock_guard<std::mutex> l(pipeline_name_mtx);
                return pipeline_name;
            }

            std::string getID() { return "processing_handler"; }
        };
    } // namespace handlers
} // namespace satdump