#include "http_ratelimit.h"
#include "core/config.h"
#include "logger.h"
#include <algorithm>
#include <cctype>
#include <chrono>

namespace satdump
{
    static const std::chrono::seconds MINUTE_WINDOW(60);
    static const std::chrono::seconds HOUR_WINDOW(3600);

    HttpRateLimiter &HttpRateLimiter::get()
    {
        static HttpRateLimiter instance;
        return instance;
    }

    std::string HttpRateLimiter::hostFromUrl(const std::string &url)
    {
        std::string authority = url;

        auto scheme = authority.find("://");
        if (scheme != std::string::npos)
            authority = authority.substr(scheme + 3);

        size_t cut = authority.find_first_of("/?#");
        if (cut != std::string::npos)
            authority = authority.substr(0, cut);

        // Strip userinfo up to and including the last '@'.
        auto at = authority.find_last_of('@');
        if (at != std::string::npos)
            authority = authority.substr(at + 1);

        auto colon = authority.rfind(':');
        if (colon != std::string::npos && authority.find(':') == colon)
            authority = authority.substr(0, colon);

        std::transform(authority.begin(), authority.end(), authority.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        if (authority.rfind("www.", 0) == 0)
            authority = authority.substr(4);
        if (!authority.empty() && authority.back() == '.')
            authority.pop_back();

        return authority;
    }

    std::shared_ptr<HttpRateLimiter::HostState> HttpRateLimiter::getState(const std::string &host)
    {
        {
            std::lock_guard<std::mutex> lock(map_mtx_);
            auto it = hosts_.find(host);
            if (it != hosts_.end())
                return it->second;
        }

        auto state = std::make_shared<HostState>();

        // Unknown hosts stay unlimited unless a default was set before config loaded.
        std::lock_guard<std::mutex> lock(map_mtx_);
        auto it = hosts_.find(host);
        if (it != hosts_.end())
            return it->second;
        hosts_[host] = state;
        return state;
    }

    void HttpRateLimiter::setHostLimits(const std::string &host, const HostRateLimit &limit)
    {
        auto state = getState(host);
        std::lock_guard<std::mutex> lock(state->mtx);
        state->limit = limit;
        state->cv.notify_all();
    }

    void HttpRateLimiter::configureIfNeeded()
    {
        std::call_once(config_flag_, [this]() { configureFromConfig(); });
    }

    void HttpRateLimiter::configureFromConfig()
    {
        auto limits = satdump_cfg.getHttpRateLimits();
        for (auto &kv : limits)
            setHostLimits(kv.first, kv.second);
        logger->info("Configured %d HTTP rate-limit host(s)", (int)limits.size());
    }

    void HttpRateLimiter::acquire(const std::string &host)
    {
        configureIfNeeded();

        auto state = getState(host);
        std::unique_lock<std::mutex> lock(state->mtx);
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();

        while (true)
        {
            now = std::chrono::steady_clock::now();

            while (!state->minute_hits.empty() && state->minute_hits.front() + MINUTE_WINDOW <= now)
                state->minute_hits.pop_front();
            while (!state->hour_hits.empty() && state->hour_hits.front() + HOUR_WINDOW <= now)
                state->hour_hits.pop_front();

            std::chrono::steady_clock::time_point wait_until = now;

            if (state->blocked_until > wait_until)
                wait_until = state->blocked_until;

            if (state->limit.per_minute > 0 && (int)state->minute_hits.size() >= state->limit.per_minute)
                wait_until = std::max(wait_until, state->minute_hits.front() + MINUTE_WINDOW);

            if (state->limit.per_hour > 0 && (int)state->hour_hits.size() >= state->limit.per_hour)
                wait_until = std::max(wait_until, state->hour_hits.front() + HOUR_WINDOW);

            if (state->limit.min_spacing_ms > 0 && !state->minute_hits.empty())
            {
                auto min_gap = state->minute_hits.back() + std::chrono::milliseconds(state->limit.min_spacing_ms);
                if (min_gap > wait_until)
                    wait_until = min_gap;
            }

            if (wait_until <= now)
            {
                state->minute_hits.push_back(now);
                state->hour_hits.push_back(now);
                return;
            }

            state->cv.wait_until(lock, wait_until);
        }
    }

    void HttpRateLimiter::reportResult(const std::string &host, long http_code,
                                       const std::string &retry_after_header)
    {
        if (http_code != 429)
            return;

        auto state = getState(host);
        std::lock_guard<std::mutex> lock(state->mtx);

        int max_retry = state->limit.max_retry_after_seconds;
        int retry_after = max_retry > 0 ? max_retry : 120; // fall back to 120 when unset/invalid

        if (!retry_after_header.empty())
        {
            try
            {
                int parsed = std::stoi(retry_after_header);
                if (parsed > 0)
                {
                    // Cap the honored Retry-After to max_retry_after_seconds; when that is
                    // not set (<=0), honor the header value as-is (uncapped).
                    int cap = max_retry > 0 ? max_retry : parsed;
                    retry_after = std::min(parsed, cap);
                }
            }
            catch (std::exception &)
            {
                // Malformed header: fall back to the cap.
            }
        }

        auto blocked_until = std::chrono::steady_clock::now() + std::chrono::seconds(retry_after);
        if (blocked_until > state->blocked_until)
            state->blocked_until = blocked_until;

        logger->warn("HTTP 429 for host %s, blocking for %d s", host.c_str(), retry_after);
        state->cv.notify_all();
    }
} // namespace satdump