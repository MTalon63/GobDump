#pragma once

#include <chrono>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>
#include <curl/curl.h>

// Note: members reference the libcurl handle type CURL*, so <curl/curl.h> is pulled in
// here. Do NOT try to forward-declare CURL: curl defines it as "typedef void CURL;", so a
// struct forward declaration would be an unrelated type and fail to compile.

namespace satdump
{
    /**
     * @brief Persistent Space-Track REST session.
     *
     * Holds one long-lived libcurl easy handle with an in-memory cookie engine so the
     * session cookie is reused across queries for ~2 h. The handle is serialized via an
     * internal mutex (libcurl easy handles are not thread-safe); every request is routed
     * through HttpRateLimiter to respect the published 30 req/min / 300 req/hr limits.
     */
    class SpaceTrackSession
    {
    public:
        static SpaceTrackSession &get();

        void setCredentials(const std::string &login, const std::string &password);
        bool hasUsableCredentials() const; // "", "yourloginemail", "yourpassword" => false

        // Logs in if no valid session exists; otherwise reuses the cookie.
        bool ensureLoggedIn();

        // GET an absolute Space-Track URL, reusing the session cookie; true on CURLE_OK.
        bool get(const std::string &url, std::string &result);

        void shutdown(); // optional clean logout + curl_easy_cleanup

    private:
        SpaceTrackSession() = default;
        ~SpaceTrackSession();
        SpaceTrackSession(const SpaceTrackSession &) = delete;
        SpaceTrackSession &operator=(const SpaceTrackSession &) = delete;

        bool loginLocked();
        void logoutLocked();

        mutable std::mutex mtx_;
        CURL *curl_ = nullptr; // persistent handle, in-memory cookie engine
        bool logged_in_ = false;
        std::string login_;
        std::string password_;
        std::chrono::steady_clock::time_point login_time_{};
        std::chrono::seconds session_ttl_{7200}; // ~2 h
    };

    // Batched latest-elements URL using class/gp (NOT the discouraged gp_history). Filters
    // to on-orbit objects with elements newer than ~10 days, ordering by NORAD id.
    std::string buildSpaceTrackLatestUrl(const std::vector<int> &norads);

    // Single-object historic URL using class/gp, selecting the element set closest
    // before the requested epoch. Returns an empty string on formatting problems.
    std::string buildSpaceTrackHistoricUrl(int norad, time_t timestamp);
} // namespace satdump