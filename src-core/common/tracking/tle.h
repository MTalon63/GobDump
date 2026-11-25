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

    // Minimal, conservative sanity check for a pair of TLE element lines. This is what
    // keeps garbage away from the orbit propagator: a malformed pair makes predict_*()
    // return constants, and the AOS/pass searches around it then loop forever. The check
    // is deliberately loose on formatting (no checksum requirement) so real rows fetched
    // from Celestrak or rebuilt from the database always pass, while empty, truncated,
    // shifted or unrelated lines cannot.
    inline bool isValidTle(const std::string &line1, const std::string &line2)
    {
        if (line1.size() < 60 || line2.size() < 60)
            return false;
        if (line1[0] != '1' || line2[0] != '2')
            return false;

        // Both lines must carry the same satellite-number field (columns 3-7). Compare the field
        // as-is rather than demanding five digits: strL() pads with SPACES, so a 4-digit NORAD is
        // published as " 6235", and a digits-only test would reject a real satellite. Requiring at
        // least one digit still rejects a pair of blank fields, and a shifted, duplicated or
        // mismatched pair fails the equality test below.
        const std::string n1 = line1.substr(2, 5);
        const std::string n2 = line2.substr(2, 5);
        if (n1 != n2)
            return false;
        for (char c : n1)
            if (c >= '0' && c <= '9')
                return true;
        return false;
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
