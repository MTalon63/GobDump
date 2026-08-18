#pragma once

#include "dll_export.h"
#include "nlohmann/json.hpp"
#include <deque>
#include <optional>
#include <string>

namespace satdump
{
    // Simple struct to hold TLE definitions
    struct TLE
    {
        int norad = -1;
        std::string name;
        std::string line1;
        std::string line2;
        uint64_t time;
    };

    inline void to_json(nlohmann::json &j, const TLE &v)
    {
        j["norad"] = v.norad;
        j["name"] = v.name;
        j["line1"] = v.line1;
        j["line2"] = v.line2;
    }

    inline void from_json(const nlohmann::json &j, TLE &v)
    {
        if (!j.is_object())
            return;

        if (j.contains("norad") && j["norad"].is_number())
            v.norad = j["norad"].get<int>();
        if (j.contains("name") && j["name"].is_string())
            v.name = j["name"].get<std::string>();
        if (j.contains("line1") && j["line1"].is_string())
            v.line1 = j["line1"].get<std::string>();
        if (j.contains("line2") && j["line2"].is_string())
            v.line2 = j["line2"].get<std::string>();
    }

    struct TLEsUpdatedEvent
    {
    };

    int parseTLEStream(std::istream &inputStream, std::vector<TLE> &new_registry); // Helper - Takes an input stream and parses out the valid TLEs

    std::vector<TLE> tryFetchTLEsFromFileURL(std::string url_str);
    std::vector<TLE> tryFetchSingleTLEwithNorad(int norad);
    std::optional<TLE> get_from_spacetrack_time(int norad, time_t timestamp);
    std::vector<TLE> get_from_spacetrack_latest_list(std::vector<int> norad);

    std::optional<TLE> get_from_norad_in_vec(std::vector<TLE> vec, int norad);
} // namespace satdump
