#pragma once

#include "winsock_init.h"
#include <cerrno>
#include <cstdint>
#include <stdexcept>
#include <string.h>
#include <string>

#if defined(_WIN32)
#include <stdio.h>
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace net
{
    class UDPClient
    {
    private:
        sockaddr_in sock_addr;
#if defined(_WIN32)
        SOCKET sock = INVALID_SOCKET;
#else
        int sock = -1;
#endif

    public:
        UDPClient(const UDPClient &) = delete;
        UDPClient &operator=(const UDPClient &) = delete;

        UDPClient(char *address, int port)
        {
            ensure_winsock_init();

#if defined(_WIN32)
            if ((sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == INVALID_SOCKET)
#else
            if ((sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0)
#endif
                throw std::runtime_error("Couldn't open UDP socket!");

            memset((char *)&sock_addr, 0, sizeof(sock_addr));
            sock_addr.sin_family = AF_INET;
            sock_addr.sin_port = htons(port);

#if defined(_WIN32)
            sock_addr.sin_addr.S_un.S_addr = inet_addr(address);
            if (sock_addr.sin_addr.S_un.S_addr == INADDR_NONE)
            {
                closesocket(sock); // was leaked on this error path
                throw std::runtime_error("Couldn't connect to UDP socket!");
            }
#else
            if (inet_aton(address, &sock_addr.sin_addr) == 0)
            {
                close(sock);
                throw std::runtime_error("Couldn't connect to UDP socket!");
            }
#endif
        }

        ~UDPClient()
        {
            // No WSACleanup: the refcount is shared process-wide, see winsock_init.h
#if defined(_WIN32)
            if (sock != INVALID_SOCKET)
                closesocket(sock);
#else
            if (sock >= 0)
                close(sock);
#endif
        }

        int send(uint8_t *data, int len)
        {
            int slen = sizeof(sockaddr);
            int r = sendto(sock, (char *)data, len, 0, (sockaddr *)&sock_addr, slen);
#if defined(_WIN32)
            if (r == SOCKET_ERROR)
#else
            if (r < 0)
#endif

#if defined(_WIN32)
                throw std::runtime_error("Error sending to UDP socket! WSA Error: " + std::to_string(WSAGetLastError()));
#else
                throw std::runtime_error("Error sending to UDP socket!");
#endif
            return r;
        }

        int recv(uint8_t *data, int len)
        {
#if defined(_WIN32)
            int slen = sizeof(sockaddr);
#else
            socklen_t slen = sizeof(sockaddr);
#endif

            int r = recvfrom(sock, (char *)data, len, 0, (struct sockaddr *)&sock_addr, &slen);
#if defined(_WIN32)
            if (r == SOCKET_ERROR)
#else
            if (r < 0)
#endif

                throw std::runtime_error("Error receiving from UDP socket!");
            return r;
        }
    };

    class UDPServer
    {
    private:
        sockaddr_in sock_addr;
#if defined(_WIN32)
        SOCKET sock = INVALID_SOCKET;
#else
        int sock = -1;
#endif

    public:
        UDPServer(const UDPServer &) = delete;
        UDPServer &operator=(const UDPServer &) = delete;

        UDPServer(int port)
        {
            ensure_winsock_init();

#if defined(_WIN32)
            if ((sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == INVALID_SOCKET)
#else
            if ((sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0)
#endif
                throw std::runtime_error("Couldn't open UDP socket!");

            memset((char *)&sock_addr, 0, sizeof(sock_addr));
            sock_addr.sin_family = AF_INET;
            sock_addr.sin_addr.s_addr = INADDR_ANY;
            sock_addr.sin_port = htons(port);

#if defined(_WIN32)
            sock_addr.sin_addr.S_un.S_addr = INADDR_ANY;
#endif
            // SO_REUSEADDR must be set BEFORE bind() to have any effect; it was dead code after it.
            // Not set on Windows: there SO_REUSEADDR allows hijacking a bound port rather than
            // permitting TIME_WAIT reuse as on POSIX.
#ifndef _WIN32
            int ttrue = 1;
            setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char *)&ttrue, sizeof(int));
#endif

            if (bind(sock, (struct sockaddr *)&sock_addr, sizeof(sock_addr)) < 0)
            {
#if defined(_WIN32)
                closesocket(sock); // was leaked on this error path
#else
                close(sock);
#endif
                throw std::runtime_error("Couldn't connect to UDP socket!");
            }
        }

        ~UDPServer()
        {
            // No WSACleanup: the refcount is shared process-wide, see winsock_init.h
#if defined(_WIN32)
            if (sock != INVALID_SOCKET)
                closesocket(sock);
#else
            if (sock >= 0)
            {
                shutdown(sock, SHUT_RDWR);
                close(sock);
            }
#endif
        }

        void enableTimeout()
        {
#if defined(_WIN32)
            DWORD read_timeout = 10;
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&read_timeout, sizeof read_timeout);
#else
            struct timeval read_timeout;
            read_timeout.tv_sec = 0;
            read_timeout.tv_usec = 1e4;
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const void *)&read_timeout, sizeof read_timeout);
#endif
        }

        int send(uint8_t *data, int len)
        {
            int slen = sizeof(sockaddr);
            int r = sendto(sock, (char *)data, len, 0, (sockaddr *)&sock_addr, slen);
#if defined(_WIN32)
            if (r == SOCKET_ERROR)
#else
            if (r < 0)
#endif
                throw std::runtime_error("Error sending to UDP socket!");
            return r;
        }

        int recv(uint8_t *data, int len)
        {
#if defined(_WIN32)
            int slen = sizeof(sockaddr);
#else
            socklen_t slen = sizeof(sockaddr);
#endif
            int r = recvfrom(sock, (char *)data, len, 0, (struct sockaddr *)&sock_addr, &slen);
#if defined(_WIN32)
            if (r == SOCKET_ERROR)
            {
                // A timeout is routine once enableTimeout() is used (10ms), not an error. Only
                // WSAEINTR was forgiven before, so every idle period threw ~100 exceptions/second.
                int err = WSAGetLastError();
                if (err == WSAETIMEDOUT || err == WSAEWOULDBLOCK)
                    return 0;
                if (err != WSAEINTR)
                    throw std::runtime_error("Error receiving from UDP socket! WSA Error: " + std::to_string(err));
            }
#else
            if (r < 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    return 0;
                if (errno != EINTR)
                    throw std::runtime_error("Error receiving from UDP socket!");
            }
#endif
            return r;
        }
    };
} // namespace net
