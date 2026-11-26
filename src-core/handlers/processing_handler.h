#pragma once

/**
 * @file processing_handler.h
 */

#include "handler.h"
#include <thread>

namespace satdump
{
    namespace handlers
    {
        /**
         * @brief ProcessingHandler base class
         *
         * Some handlers implement a lot of processing already in order
         * to allow manipulating products, images, or more. Therefore,
         * it'd be stupid not to re-use this code for headless
         * processing as well! :-)
         *
         * This is a common interface Handlers can implement if required
         * that allows implementing processing in a way common to both
         * the UI and headless uses. This implements the multi-threaded
         * processing required in the UI as well.
         *
         * @param is_processing variable to be used in the UI to disable controls
         * while processing is ongoing
         */
        class ProcessingHandler
        {
        public:
            ProcessingHandler() {}
            /** Join the async processing thread. Idempotent, and safe to call from a derived
             *  destructor when the base's thread must be stopped before the derived's own members
             *  are torn down. Do NOT call ~ProcessingHandler() by hand for that ordering: the base
             *  is then destroyed twice, and on Android bionic aborts the process with
             *  "pthread_mutex_destroy called on a destroyed mutex".
             */
            void waitForProcessing() { if (async_thread.joinable()) async_thread.join(); }

            ~ProcessingHandler()
            {
                waitForProcessing();
            }

        protected:
            bool is_processing = false;
            std::mutex is_processing_mtx;

            /**
             * @brief Actual processing function to be implemented by the child class
             */
            virtual void do_process() = 0;

            std::thread async_thread;

        public:
            /**
             * @brief Performing processing. This is not multi-threaded, intended to call it externally
             */
            void process()
            {
                is_processing_mtx.lock();
                is_processing = true;
                is_processing_mtx.unlock();
                do_process();
                is_processing_mtx.lock();
                is_processing = false;
                is_processing_mtx.unlock();
            }

            /**
             * @brief Set the processing status externally, to force waiting before starting a new thread
             * @param v to set if it's processing or not
             */
            void set_is_processing(bool v)
            {
                is_processing_mtx.lock();
                is_processing = v;
                is_processing_mtx.unlock();
            }

            /**
             * @brief Get the processing status externally
             * @return current status
             */
            bool get_is_processing()
            {
                is_processing_mtx.lock();
                bool s = is_processing;
                is_processing_mtx.unlock();
                return s;
            }

            /**
             * @brief Perform processing asynchronously, in a thread
             */
            void asyncProcess()
            {
                is_processing_mtx.lock();
                if (is_processing)
                {
                    printf("ALREADY PROCESSING!!!!\n"); // TODOREWORK
                    is_processing_mtx.unlock();
                    return;
                }
                is_processing = true;
                is_processing_mtx.unlock();

                try
                {
                    async_thread.join();
                }
                catch (std::exception &e)
                {
                }

                auto fun = [this]() { process(); };
                async_thread = std::thread(fun);
            }
        };
    } // namespace handlers
} // namespace satdump