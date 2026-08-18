#pragma once

/**
 * @file http.h
 * @brief HTTP Get/Post functions and others
 */

#include <string>

namespace satdump
{
    /**
     * @brief Initialise libcurl globally, exactly once, in a thread-safe way.
     *
     * Call this before any direct curl_easy_* use. Never call curl_global_init or
     * curl_global_cleanup directly: they are not thread-safe, and on Windows
     * CURL_GLOBAL_ALL implies CURL_GLOBAL_WIN32, so cleanup runs WSACleanup()
     * process-wide and can tear Winsock down under unrelated live sockets.
     */
    void ensure_curl_global_init();

    /**
     * @brief cURL helper function, writing
     * the payload into a std::string
     *
     * Provided a std::string is passed as userdata
     * for Curl, this can be used a handler to write
     * incoming data into said string.
     */
    size_t curl_write_std_string(void *contents, size_t size, size_t nmemb, std::string *s);

    /**
     * @brief Perform a HTTP Request on the
     * provided URL and return the result as
     * a string.
     *
     * @param url_str URL to use
     * @param result HTTP response, as a string
     * @param added_header optional additional headers
     * @param progress optional progress float pointer
     */
    int perform_http_request(std::string url, std::string &result, std::string added_header = "", float *progress = nullptr);

    /**
     * @brief Perform a HTTP Request on the
     * provided URL and return the result as
     * a string, with POST data.
     *
     * @param url_str URL to use
     * @param result HTTP response, as a string
     * @param post_req POST payload
     * @param added_header optional additional headers
     */
    int perform_http_request_post(std::string url_str, std::string &result, std::string post_req, std::string added_header = "");
} // namespace satdump