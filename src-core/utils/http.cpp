#include "http.h"
#include "http_ratelimit.h"
#include "logger.h"
#include "satdump_vars.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <curl/curl.h>
#include <mutex>
#include <string>

#define CURL_TIMEOUT 5000
#define CURL_CONNECT_TIMEOUT 5

namespace satdump
{
    // Must run once: it is not thread-safe, and CURL_GLOBAL_ALL means curl_global_cleanup
    // would call WSACleanup() process-wide (which must NOT be called here).
    void ensure_curl_global_init()
    {
        static std::once_flag curl_init_flag;
        std::call_once(curl_init_flag, []() { curl_global_init(CURL_GLOBAL_ALL); });
    }
    size_t curl_write_std_string(void *contents, size_t size, size_t nmemb, std::string *s)
    {
        size_t newLength = size * nmemb;
        try
        {
            s->append((char *)contents, newLength);
        }
        catch (std::bad_alloc &)
        {
            return 0;
        }
        return newLength;
    }

    int curl_float_progress_func(void *ptr, curl_off_t TotalToDownload, curl_off_t NowDownloaded, curl_off_t TotalToUpload, curl_off_t NowUploaded)
    {
        float *pptr = (float *)ptr;
        if (TotalToDownload != 0)
            *pptr = (float)NowDownloaded / (float)TotalToDownload;
        else if (TotalToUpload != 0)
            *pptr = (float)NowUploaded / (float)TotalToUpload;
        return 0;
    }

    struct RetryAfterCapture
    {
        std::string value;
        static const std::string header_name;
    };
    const std::string RetryAfterCapture::header_name = "retry-after:";

    size_t curl_header_retry_after(char *buffer, size_t size, size_t nitems, RetryAfterCapture *out)
    {
        size_t len = size * nitems;
        std::string line(buffer, len);
        // Header lines are case-insensitive.
        std::string lower = line;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
        if (lower.rfind(RetryAfterCapture::header_name, 0) == 0)
        {
            std::string value = line.substr(RetryAfterCapture::header_name.size());
            size_t first = value.find_first_not_of(" \t\r\n");
            if (first != std::string::npos)
                value = value.substr(first);
            out->value = value;
        }
        return len;
    }

    // Rate-limits the host, performs the request with bounded 429 retries, and reports
    // results so the limiter can back off.
    static int perform_http_request_internal(std::string url_str, std::string &result, std::string added_header,
                                             std::string post_req, bool is_post, float *progress)
    {
        const int max_429_retries = 2; // plus the initial attempt
        bool ret = 1;

        ensure_curl_global_init();

        std::string host = HttpRateLimiter::hostFromUrl(url_str);

        for (int attempt = 0; attempt <= max_429_retries; attempt++)
        {
            HttpRateLimiter::get().acquire(host);

            CURL *curl = curl_easy_init();
            if (!curl)
                return 1;

            char error_buffer[CURL_ERROR_SIZE] = {0};
            RetryAfterCapture retry_after;
            CURLcode res;

            result.clear();

            curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
            curl_easy_setopt(curl, CURLOPT_USERAGENT, std::string((std::string) "GobDump/v" + satdump::SATDUMP_VERSION).c_str());
            curl_easy_setopt(curl, CURLOPT_URL, url_str.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_std_string);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, CURL_TIMEOUT);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, CURL_CONNECT_TIMEOUT);
            curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, curl_header_retry_after);
            curl_easy_setopt(curl, CURLOPT_HEADERDATA, &retry_after);

#ifdef CURLSSLOPT_NATIVE_CA
            curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
#endif

            struct curl_slist *chunk = NULL;
            if (added_header != "")
            {
                chunk = curl_slist_append(chunk, added_header.c_str());
                curl_easy_setopt(curl, CURLOPT_HTTPHEADER, chunk);
            }

            if (is_post)
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_req.c_str());

            if (progress != nullptr)
            {
                curl_easy_setopt(curl, CURLOPT_XFERINFODATA, progress);
                curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curl_float_progress_func);
                curl_easy_setopt(curl, CURLOPT_NOPROGRESS, false);
            }

            res = curl_easy_perform(curl);

            long http_code = 0;
            if (res == CURLE_OK)
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

            curl_easy_cleanup(curl);
            if (chunk != NULL)
                curl_slist_free_all(chunk);

            if (res != CURLE_OK)
            {
                if (strlen(error_buffer))
                    logger->error("curl_easy_perform() failed: %s", error_buffer);
                else
                    logger->error("curl_easy_perform() failed: %s", curl_easy_strerror(res));
                ret = 1;
                break; // transport error: not rate-limit related
            }

            if (http_code == 429)
            {
                // Record the penalty; the next acquire() waits out the Retry-After.
                HttpRateLimiter::get().reportResult(host, 429, retry_after.value);
                ret = 1;
                continue; // re-acquire (blocks for the penalty) and retry
            }

            // Non-429: report and return as today (includes non-2xx, which previously returned
            // success on CURLE_OK).
            HttpRateLimiter::get().reportResult(host, http_code, retry_after.value);
            ret = 0;
            break;
        }

        return ret;
    }

    int perform_http_request(std::string url_str, std::string &result, std::string added_header, float *progress)
    {
        return perform_http_request_internal(url_str, result, added_header, "", false, progress);
    }

    int perform_http_request_post(std::string url_str, std::string &result, std::string post_req, std::string added_header)
    {
        return perform_http_request_internal(url_str, result, added_header, post_req, true, nullptr);
    }
} // namespace satdump