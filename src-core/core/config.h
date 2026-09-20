#pragma once

#include "core/exception.h"
#include "core/plugin.h"
#include "dll_export.h"
#include "nlohmann/json.hpp"
#include "utils/http_ratelimit.h"
#include "logger.h"
#include <exception>
#include <functional>
#include <unordered_map>

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

        // Reads top-level "http_rate_limits" per host; malformed entries fall back to defaults.
        inline std::unordered_map<std::string, satdump::HostRateLimit> getHttpRateLimits()
        {
            std::unordered_map<std::string, satdump::HostRateLimit> out;
            auto &rate_limits = main_cfg["http_rate_limits"];
            if (rate_limits.is_null() || !rate_limits.is_object())
                return out;

            for (auto it = rate_limits.begin(); it != rate_limits.end(); ++it)
            {
                std::string host = it.key();
                auto &entry = it.value();

                HostRateLimit limit;
                auto read_field = [&](const std::string &field, int &target)
                {
                    if (entry.contains(field) && !entry[field].is_null())
                    {
                        try
                        {
                            target = entry[field].get<int>();
                        }
                        catch (std::exception &e)
                        {
                            logger->warn("http_rate_limits[%s][%s] invalid, using default: %s", host.c_str(), field.c_str(), e.what());
                        }
                    }
                };

                read_field("per_minute", limit.per_minute);
                read_field("per_hour", limit.per_hour);
                read_field("max_retry_after_seconds", limit.max_retry_after_seconds);
                read_field("min_spacing_ms", limit.min_spacing_ms);

                out[host] = limit;
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
