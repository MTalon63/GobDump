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
    class TCPClient
    {
    private:
        sockaddr_in sock_addr;
        // Was `int`: SOCKET is 64-bit unsigned on Win64, so that truncated it and `== -1` never matched
#if defined(_WIN32)
        SOCKET sock = INVALID_SOCKET;
#else
        int sock = -1;
#endif

    public:
        TCPClient(const TCPClient &) = delete;
        TCPClient &operator=(const TCPClient &) = delete;

        TCPClient(char *address, int port)
        {
            ensure_winsock_init();

#if defined(_WIN32)
            if ((sock = socket(AF_INET, SOCK_STREAM, 0)) == INVALID_SOCKET)
#else
            if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0)
#endif
                throw std::runtime_error("Couldn't open TCP socket!");

            memset((char *)&sock_addr, 0, sizeof(sock_addr));
            sock_addr.sin_family = AF_INET;
            sock_addr.sin_addr.s_addr = inet_addr(address);
            sock_addr.sin_port = htons(port);

            if (connect(sock, (const sockaddr *)&sock_addr, sizeof(sock_addr)) != 0)
            {
                close_socket(); // was leaked when connect() failed
                throw std::runtime_error("Couldn't connect to TCP socket!");
            }
        }

        ~TCPClient() { close_socket(); }

        int sends(uint8_t *data, int len)
        {
            int r = send(sock, (char *)data, len, 0);
#if defined(_WIN32)
            if (r == SOCKET_ERROR)
                throw std::runtime_error("Error sending to TCP socket! WSA Error: " + std::to_string(WSAGetLastError()));
#else
            if (r < 0)
                throw std::runtime_error("Error sending to TCP socket!");
#endif
            return r;
        }

        int recvs(uint8_t *data, int len)
        {
            int r = recv(sock, (char *)data, len, 0);
#if defined(_WIN32)
            if (r == SOCKET_ERROR)
            {
                int err = WSAGetLastError();
                if (err == WSAETIMEDOUT || err == WSAEWOULDBLOCK)
                    return 0;
                throw std::runtime_error("Error receiving from TCP socket! WSA Error: " + std::to_string(err));
            }
#else
            if (r < 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    return 0;
                throw std::runtime_error("Error receiving from TCP socket!");
            }
#endif
            return r;
        }

    private:
        void close_socket()
        {
            // No WSACleanup: the refcount is shared process-wide, see winsock_init.h
#if defined(_WIN32)
            if (sock != INVALID_SOCKET)
            {
                closesocket(sock);
                sock = INVALID_SOCKET;
            }
#else
            if (sock >= 0)
            {
                close(sock);
                sock = -1;
            }
#endif
        }
    };
} // namespace net
