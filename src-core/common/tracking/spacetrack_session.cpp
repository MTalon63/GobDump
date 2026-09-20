#include "spacetrack_session.h"
#include "core/config.h"
#include "logger.h"
#include "satdump_vars.h"
#include "utils/http.h"
#include "utils/http_ratelimit.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <curl/curl.h>

#define SPACETRACK_LOGIN_URL "https://www.space-track.org/ajaxauth/login"
#define SPACETRACK_LOGOUT_URL "https://www.space-track.org/ajaxauth/logout"
#define SPACETRACK_QUERY_BASE "https://www.space-track.org/basicspacedata/query"

namespace satdump
{
    static const char *space_track_host = "space-track.org";

    SpaceTrackSession &SpaceTrackSession::get()
    {
        static SpaceTrackSession instance;
        return instance;
    }

    SpaceTrackSession::~SpaceTrackSession()
    {
        // Do NOT perform a network logout here: process-static teardown may have already
        // destroyed curl's globals and the logging sink (static destruction order is
        // unspecified), so only free the handle. Use the explicit shutdown() for a clean
        // logout while the process is still alive.
        std::lock_guard<std::mutex> lock(mtx_);
        if (curl_ != nullptr)
        {
            curl_easy_cleanup(curl_);
            curl_ = nullptr;
            logged_in_ = false;
        }
    }

    void SpaceTrackSession::setCredentials(const std::string &login, const std::string &password)
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (login_ != login || password_ != password)
        {
            login_ = login;
            password_ = password;
            logged_in_ = false; // credentials changed: force a re-login
        }
    }

    bool SpaceTrackSession::hasUsableCredentials() const
    {
        std::lock_guard<std::mutex> lock(mtx_);
        return login_ != "" && password_ != "" &&
               login_ != "yourloginemail" && password_ != "yourpassword";
    }

    bool SpaceTrackSession::ensureLoggedIn()
    {
        std::lock_guard<std::mutex> lock(mtx_);
        return loginLocked();
    }

    bool SpaceTrackSession::loginLocked()
    {
        auto now = std::chrono::steady_clock::now();
        // Reuse an existing session within its TTL (5-minute safety skew).
        if (logged_in_ && curl_ && (now - login_time_ < session_ttl_ - std::chrono::minutes(5)))
            return true;

        if (login_ == "" || password_ == "")
            return false;

        if (curl_ == nullptr)
        {
            curl_ = curl_easy_init();
            if (curl_ == nullptr)
            {
                logger->error("SpaceTrackSession: could not create the persistent curl handle!");
                return false;
            }
            // In-memory cookie engine: no temp file (Android / no filesystem), cookie reuse.
            curl_easy_setopt(curl_, CURLOPT_COOKIEFILE, "");
            curl_easy_setopt(curl_, CURLOPT_USERAGENT, std::string((std::string) "GobDump/v" + SATDUMP_VERSION).c_str());
#ifdef CURLSSLOPT_NATIVE_CA
            curl_easy_setopt(curl_, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
#endif
        }

        std::string post_fields = "identity=" + login_ + "&password=" + password_;
        std::string discard_result;

        // Login counts as a request against the rate limit.
        HttpRateLimiter::get().acquire(space_track_host);

        curl_easy_setopt(curl_, CURLOPT_URL, SPACETRACK_LOGIN_URL);
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, post_fields.c_str());
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, curl_write_std_string);
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &discard_result);

        CURLcode res = curl_easy_perform(curl_);

        long http_code = 0;
        if (res == CURLE_OK)
            curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &http_code);

        HttpRateLimiter::get().reportResult(space_track_host, http_code, "");

        // Reset to GET now, while post_fields is still alive. CURLOPT_POSTFIELDS keeps a
        // non-owning pointer; leaving POST set would leave the handle referencing a soon
        // destroyed local string (a latent use-after-free on the next query perform).
        curl_easy_setopt(curl_, CURLOPT_HTTPGET, 1L);

        if (res != CURLE_OK)
        {
            logger->warn("Failed to authenticate to Space Track (curl error)! Using current TLE");
            logged_in_ = false;
            return false;
        }
        if (http_code != 200 && http_code != 302)
        {
            logger->warn("Failed to authenticate to Space Track (HTTP %ld)! Using current TLE", http_code);
            logged_in_ = false;
            return false;
        }

        logged_in_ = true;
        login_time_ = std::chrono::steady_clock::now();
        return true;
    }

    // GET a Space-Track URL with cookie reuse. On a stale session (401/403) we re-login
    // once and retry. Always holds the session mutex while talking to the rate limiter
    // (never the other way around — that lock ordering must be respected).
    bool SpaceTrackSession::get(const std::string &url, std::string &result)
    {
        std::lock_guard<std::mutex> lock(mtx_);

        if (!loginLocked())
            return false;

        if (curl_ == nullptr)
            return false;

        for (int attempt = 0; attempt < 2; attempt++)
        {
            HttpRateLimiter::get().acquire(space_track_host);

            result.clear();
            curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl_, CURLOPT_HTTPGET, 1L); // hard-won fix: reset POST→GET so login POSTFIELDS don't linger
            curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, curl_write_std_string);
            curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &result);

            CURLcode res = curl_easy_perform(curl_);

            long http_code = 0;
            if (res == CURLE_OK)
                curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &http_code);

            HttpRateLimiter::get().reportResult(space_track_host, http_code, "");

            if (res != CURLE_OK)
            {
                logger->warn("Failed to download from Space Track (curl error)!");
                return false;
            }

            // Stale cookie: force a re-login and retry once.
            if (http_code == 401 || http_code == 403)
            {
                if (attempt == 0)
                {
                    logger->warn("Space Track session expired or unauthorized (HTTP %ld), re-logging in...", http_code);
                    logged_in_ = false;
                    if (!loginLocked())
                        return false;
                    continue;
                }
                logger->warn("Space Track re-login did not help (HTTP %ld)!", http_code);
                return false;
            }

            return true; // any other HTTP code behaves as before
        }

        return false;
    }

    void SpaceTrackSession::logoutLocked()
    {
        if (curl_ == nullptr)
            return;

        HttpRateLimiter::get().acquire(space_track_host);

        std::string discard_result;
        curl_easy_setopt(curl_, CURLOPT_URL, SPACETRACK_LOGOUT_URL);
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, "");
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, curl_write_std_string);
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &discard_result);
        curl_easy_perform(curl_);
    }

    void SpaceTrackSession::shutdown()
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (curl_ != nullptr)
        {
            try
            {
                logoutLocked();
            }
            catch (std::exception &)
            {
            }
            curl_easy_cleanup(curl_);
            curl_ = nullptr;
            logged_in_ = false;
        }
    }

    static std::string pad_num(int v)
    {
        return v > 9 ? std::to_string(v) : "0" + std::to_string(v);
    }

    std::string buildSpaceTrackLatestUrl(const std::vector<int> &norads)
    {
        if (norads.empty())
            return "";

        std::ostringstream url;
        url << SPACETRACK_QUERY_BASE
            << "/class/gp"
               "/decay_date/null-val"
               "/epoch/%3Enow-10"
               "/NORAD_CAT_ID/";
        for (size_t i = 0; i < norads.size(); i++)
        {
            if (i > 0)
                url << "%2C";
            url << norads[i];
        }
        url << "/orderby/NORAD_CAT_ID/emptyresult/show";
        return url.str();
    }

    std::string buildSpaceTrackHistoricUrl(int norad, time_t timestamp)
    {
        if (timestamp < 0)
            timestamp = 0;

        time_t tttime = timestamp;
        // gmtime() is not reentrant; use the *_r/_s variants.
        std::tm timeReadable = {};
        bool gmtime_ok = false;
#ifdef _WIN32
        gmtime_ok = gmtime_s(&timeReadable, &tttime) == 0;
#else
        gmtime_ok = gmtime_r(&tttime, &timeReadable) != nullptr;
#endif
        if (!gmtime_ok)
            return "";

        std::string timestamp_day = std::to_string(timeReadable.tm_year + 1900) + "-" + pad_num(timeReadable.tm_mon + 1) + "-" + pad_num(timeReadable.tm_mday);
        std::string timestamp_daytime = pad_num(timeReadable.tm_hour) + "%3A" + pad_num(timeReadable.tm_min) + "%3A" + pad_num(timeReadable.tm_sec);

        std::ostringstream url;
        url << SPACETRACK_QUERY_BASE
            << "/class/gp"
               "/NORAD_CAT_ID/"
            << norad
            << "/EPOCH/%3C" << timestamp_day << "T" << timestamp_daytime
            << "/orderby/EPOCH%20desc/limit/1/emptyresult/show";
        return url.str();
    }
} // namespace satdump