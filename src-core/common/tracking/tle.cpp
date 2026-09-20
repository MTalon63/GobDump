#include "init.h"
#include <optional>
#include <string>
#include <vector>
#define SATDUMP_DLL_EXPORT 1
#include "common/utils.h"
#include "core/config.h"
#include "core/plugin.h"
#include "logger.h"
#include "nlohmann/json_utils.h"
#include "satdump_vars.h"
#include "spacetrack_session.h"
#include "tle.h"
#include "utils/http.h"
#include <curl/curl.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

namespace satdump
{
    int parseTLEStream(std::istream &inputStream, std::vector<TLE> &new_registry)
    {
        std::string this_line;
        std::deque<std::string> tle_lines;
        int total_lines = 0;
        while (std::getline(inputStream, this_line))
        {
            this_line.erase(0, this_line.find_first_not_of(" \t\n\r"));
            this_line.erase(this_line.find_last_not_of(" \t\n\r") + 1);
            if (this_line != "")
                tle_lines.push_back(this_line);

            if (tle_lines.size() == 3)
            {
                bool checksum_good = true;
                for (int i = 1; i < 3; i++)
                {
                    if (!isdigit(tle_lines[i].back()) || !isdigit(tle_lines[i].front()) || tle_lines[i].front() - '0' != i)
                    {
                        checksum_good = false;
                        break;
                    }

                    int checksum = tle_lines[i].back() - '0';
                    int actualsum = 0;
                    for (int j = 0; j < (int)tle_lines[i].size() - 1; j++)
                    {
                        if (tle_lines[i][j] == '-')
                            actualsum++;
                        else if (isdigit(tle_lines[i][j]))
                            actualsum += tle_lines[i][j] - '0';
                    }

                    checksum_good = checksum == actualsum % 10;
                    if (!checksum_good)
                        break;
                }
                if (!checksum_good)
                {
                    tle_lines.pop_front();
                    continue;
                }

                int norad;
                try
                {
                    std::string noradstr = tle_lines[2].substr(2, tle_lines[2].substr(2, tle_lines[2].size() - 1).find(' '));
                    norad = std::stoi(noradstr);
                }
                catch (std::exception &)
                {
                    tle_lines.pop_front();
                    continue;
                }

                new_registry.push_back({norad, tle_lines[0], tle_lines[1], tle_lines[2]});
                tle_lines.clear();
                total_lines++;
            }
        }

        // Sort and remove duplicates
        sort(new_registry.begin(), new_registry.end(), [](TLE &a, TLE &b) { return a.name < b.name; });
        new_registry.erase(std::unique(new_registry.begin(), new_registry.end(), [](TLE &a, TLE &b) { return a.norad == b.norad; }), new_registry.end());

        return total_lines;
    }

    std::vector<TLE> tryFetchTLEsFromFileURL(std::string url_str)
    {
        bool success = true;
        std::vector<TLE> new_registry;

        logger->info(url_str);
        std::string result;
        int http_res = 1, trials = 0;
        while (http_res == 1 && trials < 10)
        {
            if ((http_res = satdump::perform_http_request(url_str, result)) != 1)
            {
                std::istringstream tle_stream(result);
                success = parseTLEStream(tle_stream, new_registry) > 0;
            }
            else
                success = false;
            trials++;
            if (!success)
            {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                logger->info("Failed getting TLEs. Retrying...");
            }
        }

        if (!success)
            logger->warn("Failed to get TLE for %s", url_str.c_str());

        return new_registry;
    }

    std::vector<TLE> tryFetchSingleTLEwithNorad(int norad)
    {
        bool success = true;
        std::vector<TLE> new_registry;

        std::string url_str = satdump_cfg.main_cfg["kepler_settings"]["url_template"].get<std::string>();
        while (url_str.find("%NORAD%") != std::string::npos)
            url_str.replace(url_str.find("%NORAD%"), 7, std::to_string(norad));

        logger->info(url_str);
        std::string result;
        int http_res = 1, trials = 0;
        while (http_res == 1 && trials < 10)
        {
            if ((http_res = perform_http_request(url_str, result)) != 1)
            {
                std::istringstream tle_stream(result);
                success = parseTLEStream(tle_stream, new_registry) > 0;
            }
            else
                success = false;
            trials++;
            if (!success)
            {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                logger->info("Failed getting TLEs. Retrying...");
            }
        }

        if (!success)
            logger->error("Error updating TLE for %d. Ignoring!", norad);

        return new_registry;
    }

    std::optional<TLE> get_from_spacetrack_time(int norad, time_t timestamp)
    {
        time_t last_update = std::stoull(db->get_meta("tles_last_updated", "0"));
        std::string sc_login = satdump_cfg.getValueFromSatDumpGeneral<std::string>("tle_space_track_login");
        std::string sc_passw = satdump_cfg.getValueFromSatDumpGeneral<std::string>("tle_space_track_password");

        // Use local/catalog data when no credentials, or when the requested epoch is within ~4 days.
        if (sc_login == "" || sc_passw == "" || sc_login == "yourloginemail" || sc_passw == "yourpassword" || fabs((double)last_update - (double)timestamp) < 4 * 24 * 3600)
            return std::optional<TLE>();

        // Pull from the Space-Track archive via the shared session (cookie reuse + rate limiting).
        logger->trace("Pulling historical TLE from Space Track...");
        SpaceTrackSession::get().setCredentials(sc_login, sc_passw);
        if (!SpaceTrackSession::get().ensureLoggedIn())
        {
            logger->warn("Failed to authenticate to Space Track! Using current TLE");
            return std::optional<TLE>();
        }

        std::string final_url = buildSpaceTrackHistoricUrl(norad, timestamp);
        if (final_url.empty())
        {
            logger->warn("Failed to build Space Track URL! Using current TLE");
            return std::optional<TLE>();
        }

        logger->trace("Request URL : %s", final_url.c_str());

        std::string result;
        if (!SpaceTrackSession::get().get(final_url, result))
        {
            logger->warn("Failed to download TLE from Space Track! Using built-in TLE");
            return std::optional<TLE>();
        }

        try
        {
            bool parsed = true;
            nlohmann::json res;
            try
            {
                res = nlohmann::json::parse(result)[0];
            }
            catch (std::exception &)
            {
                parsed = false;
            }
            if (!parsed || !res.contains("TLE_LINE0") || !res.contains("TLE_LINE1") || !res.contains("TLE_LINE2"))
            {
                logger->warn("Error pulling TLE from Space-Track! Returned data: %s", result.c_str());
                return std::optional<TLE>();
            }

            TLE tle;
            tle.norad = norad;
            tle.name = res["TLE_LINE0"].get<std::string>().substr(2, res["TLE_LINE0"].get<std::string>().size());
            tle.line1 = res["TLE_LINE1"].get<std::string>();
            tle.line2 = res["TLE_LINE2"].get<std::string>();
            tle.time = timestamp; // TODOREWORK actual time!!!!
            return tle;
        }
        catch (std::exception &e)
        {
            logger->error("Could not get TLE from Space-Track : %s", e.what());
            return std::optional<TLE>();
        }

        return std::optional<TLE>();
    }

    std::vector<TLE> get_from_spacetrack_latest_list(std::vector<int> norad)
    {
        if (norad.empty())
            return std::vector<TLE>();

        std::string sc_login = satdump_cfg.getValueFromSatDumpGeneral<std::string>("tle_space_track_login");
        std::string sc_passw = satdump_cfg.getValueFromSatDumpGeneral<std::string>("tle_space_track_password");

        if (sc_login == "" || sc_passw == "" || sc_login == "yourloginemail" || sc_passw == "yourpassword")
            return std::vector<TLE>();

        logger->warn("Pulling current TLEs from Space Track... (ONLY COVERS OBJECTS CURRENTLY IN DATABASE!)");
        SpaceTrackSession::get().setCredentials(sc_login, sc_passw);
        if (!SpaceTrackSession::get().ensureLoggedIn())
        {
            logger->warn("Failed to authenticate to Space Track! Using current TLE");
            return std::vector<TLE>();
        }

        std::vector<TLE> tles;

        // Chunk NORADs (~100/request) to cap URL length within proxy/server limits.
        const int chunk_size = 100;
        for (size_t start = 0; start < norad.size(); start += chunk_size)
        {
            std::vector<int> chunk(norad.begin() + start, norad.begin() + std::min(norad.size(), start + chunk_size));

            std::string final_url = buildSpaceTrackLatestUrl(chunk);
            if (final_url.empty())
                continue;

            logger->trace("Request URL : %s", final_url.c_str());

            std::string result;
            if (!SpaceTrackSession::get().get(final_url, result))
            {
                logger->warn("Failed to download TLEs from Space Track! Using built-in TLE");
                continue;
            }

            try
            {
                nlohmann::json res = nlohmann::json::parse(result);
                for (int g = 0; g < (int)res.size(); g++)
                {
                    auto &f = res[g];

                    TLE tle;
                    tle.norad = std::stoi(f["NORAD_CAT_ID"].get<std::string>());
                    tle.name = f["TLE_LINE0"].get<std::string>().substr(2, f["TLE_LINE0"].get<std::string>().size());
                    tle.line1 = f["TLE_LINE1"].get<std::string>();
                    tle.line2 = f["TLE_LINE2"].get<std::string>();
                    tles.push_back(tle);
                }
            }
            catch (std::exception &e)
            {
                logger->error("Could not get current TLEs from Space-Track : %s", e.what());
            }
        }

        return tles;
    }

    std::optional<TLE> get_from_norad_in_vec(std::vector<TLE> vec, int norad)
    {
        std::vector<TLE>::iterator it = std::find_if(vec.begin(), vec.end(), [&norad](const TLE &e) { return e.norad == norad; });

        if (it != vec.end())
            return std::optional<TLE>(*it);
        else
            return std::optional<TLE>();
    }
} // namespace satdump
