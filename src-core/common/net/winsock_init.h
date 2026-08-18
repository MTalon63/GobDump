#pragma once

/**
 * @file winsock_init.h
 * @brief One-time, process-wide Winsock initialisation.
 */

#ifdef _WIN32
#include <mutex>
#include <stdexcept>
#include <string>
#include <winsock2.h>
#endif

namespace net
{
    /**
     * @brief Initialise Winsock exactly once for the process.
     *
     * WSAStartup/WSACleanup share one process-wide refcount with curl, MQTT and every plugin, so a
     * per-object WSACleanup could tear Winsock down under other live sockets. No cleanup by design:
     * the OS releases it at exit.
     */
    inline void ensure_winsock_init()
    {
#ifdef _WIN32
        static std::once_flag flag;
        static int result = 0;
        std::call_once(flag,
                       []()
                       {
                           WSADATA wsa;
                           result = WSAStartup(MAKEWORD(2, 2), &wsa);
                       });
        if (result != 0)
            throw std::runtime_error("Couldn't startup WSA socket! Error: " + std::to_string(result));
#endif
    }
} // namespace net
