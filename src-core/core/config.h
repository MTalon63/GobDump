#pragma once

#include "core/exception.h"
#include "core/plugin.h"
#include "dll_export.h"
#include "nlohmann/json.hpp"
#include "logger.h"
#include <exception>
#include <functional>

namespace satdump
{
    namespace config
    {
        struct PluginConfigHandler
        {
            std::string name;
            std::function<void()> render;
            std::function<void()> save;
        };

        struct RegisterPluginConfigHandlersEvent
        {
            std::vector<PluginConfigHandler> &plugin_config_handlers;
        };

        struct WebFFTSettings
        {
            bool enable = true;
            int size = 8192;
            int rate = 30;
            float avg = 10.0f;
            float scale_min = -110.0f;
            float scale_max = 0.0f;
        };
    } // namespace config

    class SatDumpConfigHandler
    {
    public:                                                              // TODOREWORK make this private again
        nlohmann::ordered_json main_cfg;                                 // The actual config we should use, that includes user changes
        std::vector<config::PluginConfigHandler> plugin_config_handlers; // TODOREWORK handle UI rendering here!

    public:                                 // TODOREWORK make private again asap!
        nlohmann::ordered_json default_cfg; // The main system-wide satdump_cfg.json
        std::string user_cfg_path;

    private:
        void checkOutputDirs();

    public:
        void load(std::string path, std::string user_path);
        void loadUser(std::string user_path);
        void saveUser();

        void registerPlugins()
        {
            eventBus->fire_event<config::RegisterPluginConfigHandlersEvent>({plugin_config_handlers});
            // TODOREWORK redo plugin config system to integrate them?
        }

    public:
        template <typename T>
        inline void tryAssignValueFromSatDumpGeneral(T &val, std::string variablename)
        {
            if (main_cfg["satdump_general"][variablename]["value"].is_null())
                return;
            val = main_cfg["satdump_general"][variablename]["value"].get<T>();
        }

        template <typename T>
        inline T getValueFromSatDumpGeneral(std::string variablename)
        {
            if (main_cfg["satdump_general"][variablename]["value"].is_null())
                throw satdump_exception("Config key " + variablename + " is missing!");
            return main_cfg["satdump_general"][variablename]["value"].get<T>();
        }

        template <typename T>
        inline T getValueFromSatDumpDirectories(std::string variablename)
        {
            if (main_cfg["satdump_directories"][variablename]["value"].is_null())
                throw satdump_exception("Config key " + variablename + " is missing!");
            return main_cfg["satdump_directories"][variablename]["value"].get<T>();
        }

        template <typename T>
        inline T getValueFromSatDumpUI(std::string variablename)
        {
            if (main_cfg["user_interface"][variablename]["value"].is_null())
                throw satdump_exception("Config key " + variablename + " is missing!");
            return main_cfg["user_interface"][variablename]["value"].get<T>();
        }

        // Reads the top-level "web_fft" section, falling back to the defaults for any unset key
        inline config::WebFFTSettings getValueFromWebFFT()
        {
            config::WebFFTSettings out;
            auto &web_fft = main_cfg["web_fft"];
            if (!web_fft.is_null())
            {
                // Each read is guarded: a malformed (wrongly typed) value logs and keeps the default instead of throwing
                if (web_fft.contains("enable") && !web_fft["enable"].is_null())
                {
                    try { out.enable = web_fft["enable"].get<bool>(); }
                    catch (std::exception &e) { logger->warn("web_fft[enable] invalid, using default: %s", e.what()); }
                }
                if (web_fft.contains("size") && !web_fft["size"].is_null())
                {
                    try { out.size = web_fft["size"].get<int>(); }
                    catch (std::exception &e) { logger->warn("web_fft[size] invalid, using default: %s", e.what()); }
                }
                if (web_fft.contains("rate") && !web_fft["rate"].is_null())
                {
                    try { out.rate = web_fft["rate"].get<int>(); }
                    catch (std::exception &e) { logger->warn("web_fft[rate] invalid, using default: %s", e.what()); }
                }
                if (web_fft.contains("avg") && !web_fft["avg"].is_null())
                {
                    try { out.avg = web_fft["avg"].get<float>(); }
                    catch (std::exception &e) { logger->warn("web_fft[avg] invalid, using default: %s", e.what()); }
                }
                if (web_fft.contains("scale_min") && !web_fft["scale_min"].is_null())
                {
                    try { out.scale_min = web_fft["scale_min"].get<float>(); }
                    catch (std::exception &e) { logger->warn("web_fft[scale_min] invalid, using default: %s", e.what()); }
                }
                if (web_fft.contains("scale_max") && !web_fft["scale_max"].is_null())
                {
                    try { out.scale_max = web_fft["scale_max"].get<float>(); }
                    catch (std::exception &e) { logger->warn("web_fft[scale_max] invalid, using default: %s", e.what()); }
                }
            }
            // Guard against an invalid (non-positive) size by falling back to the default
            if (out.size <= 0)
            {
                logger->warn("web_fft[size] is %d, using default %d", out.size, 8192);
                out.size = 8192;
            }
            return out;
        }

        // TODOREWORK have a set_user / get_user?

    public:
        bool shouldAutoprocessProducts() { return getValueFromSatDumpGeneral<bool>("auto_process_products"); } // TODOREWORK remove since per pipeline?
        bool shouldPlayAudio() { return getValueFromSatDumpUI<bool>("play_audio"); }
    };

    SATDUMP_DLL extern SatDumpConfigHandler satdump_cfg; // SatDump configuratiin
} // namespace satdump
