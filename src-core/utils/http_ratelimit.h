#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace satdump
{
    /** @brief Per-host rate-limit tuning. 0 for a window means unlimited. */
    struct HostRateLimit
    {
        int per_minute = 0;
        int per_hour = 0;
        int max_retry_after_seconds = 120; // cap when honoring HTTP 429 Retry-After
        int min_spacing_ms = 0;            // optional minimum gap between requests
    };

    /** @brief Per-host sliding-window limiter; blocks the caller, never drops. */
    class HttpRateLimiter
    {
    public:
        static HttpRateLimiter &get();

        // Refresh limits from config; safe to call more than once (merges).
        void configureFromConfig();

        // Block the calling thread until a slot is reserved. Never drops requests.
        void acquire(const std::string &host);

        // Report a request's outcome; on HTTP 429 the host is blocked until the
        // Retry-After instant (capped at limit.max_retry_after_seconds).
        void reportResult(const std::string &host, long http_code,
                          const std::string &retry_after_header);

        // Normalize a URL to a host key: lowercase, no userinfo/port/"www.".
        static std::string hostFromUrl(const std::string &url);

        void setHostLimits(const std::string &host, const HostRateLimit &limit);

    private:
        struct HostState
        {
            std::mutex mtx;
            std::condition_variable cv;
            std::deque<std::chrono::steady_clock::time_point> minute_hits; // sliding 60 s window
            std::deque<std::chrono::steady_clock::time_point> hour_hits;   // sliding 3600 s window
            std::chrono::steady_clock::time_point blocked_until{};         // 429 / Retry-After
            HostRateLimit limit;
        };

        std::shared_ptr<HostState> getState(const std::string &host);
        void configureIfNeeded();

        std::once_flag config_flag_;
        std::mutex map_mtx_;
        std::unordered_map<std::string, std::shared_ptr<HostState>> hosts_;
    };
} // namespace satdump