#pragma once
#include "common/net/winsock_init.h"
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>

#ifdef _WIN32
#include <WinSock2.h>
#include <WS2tcpip.h>
#else
#include <unistd.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#endif

// Was `#define __attribute__(x)`, never #undef'd: it stripped the attribute from every later
// declaration in the TU, including ImGui's IM_FMTARGS and Eigen's alignment.
#ifdef _MSC_VER
#pragma pack(push, 1)
struct command_t {
    unsigned char cmd;
    unsigned int param;
};
#pragma pack(pop)
#else
struct command_t {
    unsigned char cmd;
    unsigned int param;
} __attribute__((packed));
#endif
static_assert(sizeof(command_t) == 5, "rtl_tcp command_t must be 5 bytes on the wire");

class RTLTCPClient {
public:
    RTLTCPClient() {
    }

    bool connectToRTL(char* host, uint16_t port) {
        if (connected) {
            return true;
        }

#ifdef _WIN32
        struct addrinfo* result = NULL;
        struct addrinfo* ptr = NULL;
        struct addrinfo hints;

        // Was WSAStartup per connect + WSACleanup on every error path; that refcount is shared
        // process-wide with curl/MQTT. Init once, never clean up.
        net::ensure_winsock_init();

        ZeroMemory(&hints, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        char buf[128];
        sprintf(buf, "%hu", port);

        int iResult = getaddrinfo(host, buf, &hints, &result);
        if (iResult != 0) {
            // TODO: log error
            printf("\n%s\n", gai_strerror(iResult));
            return false;
        }
        ptr = result;

        sock = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);

        if (sock == INVALID_SOCKET) {
            // TODO: log error
            printf("B");
            freeaddrinfo(result);
            return false;
        }

        iResult = connect(sock, ptr->ai_addr, (int)ptr->ai_addrlen);
        if (iResult == SOCKET_ERROR) {
            printf("C");
            closesocket(sock);
            sock = INVALID_SOCKET;
            freeaddrinfo(result);
            return false;
        }
        freeaddrinfo(result);
#else
        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0) {
            // TODO: Log error
            return false;
        }
        struct hostent* server = gethostbyname(host);
        struct sockaddr_in serv_addr;
        bzero(&serv_addr, sizeof(struct sockaddr_in));
        serv_addr.sin_family = AF_INET;
        bcopy((char*)server->h_addr, (char*)&serv_addr.sin_addr.s_addr, server->h_length);
        serv_addr.sin_port = htons(port);
        if (connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
            // TODO: log error
            return false;
        }
#endif

        connected = true;

        printf("Connected");

        return true;
    }

    void disconnect() {
        if (!connected) {
            return;
        }
#ifdef _WIN32
        closesocket(sock); // no WSACleanup: refcount is process-wide, see winsock_init.h
        sock = INVALID_SOCKET;
#else
        close(sockfd);
        sockfd = -1;
#endif
        connected = false;
    }

    // struct command_t {
    //     uint8_t cmd;
    //     uint32_t arg;
    // };

    void sendCommand(uint8_t command, uint32_t param) {
        command_t cmd;
        cmd.cmd = command;
        cmd.param = htonl(param);
#ifdef _WIN32
        send(sock, (char*)&cmd, sizeof(command_t), 0);
#else
        (void)write(sockfd, &cmd, sizeof(command_t));
#endif
    }

    void receiveData(uint8_t* buf, size_t count) {
        int received = 0;
        int ret = 0;
        while (received < (int)count) {
#ifdef _WIN32
            ret = recv(sock, (char*)&buf[received], count - received, 0);
#else
            ret = read(sockfd, &buf[received], count - received);
#endif
            if (ret <= 0) { return; }
            received += ret;
        }
    }

    void setFrequency(double freq) {
        sendCommand(1, freq);
    }

    void setSampleRate(double sr) {
        sendCommand(2, sr);
    }

    void setGainMode(int mode) {
        sendCommand(3, mode);
    }

    void setGain(double gain) {
        sendCommand(4, gain);
    }

    void setPPM(int ppm) {
        sendCommand(5, (uint32_t)ppm);
    }

    void setAGCMode(int mode) {
        sendCommand(8, mode);
    }

    void setDirectSampling(int mode) {
        sendCommand(9, mode);
    }

    void setOffsetTuning(bool enabled) {
        sendCommand(10, enabled);
    }

    void setGainIndex(int index) {
        sendCommand(13, index);
    }

    void setBiasTee(bool enabled) {
        sendCommand(14, enabled);
    }

private:
#ifdef _WIN32
    SOCKET sock;
#else
    int sockfd;
#endif

    bool connected = false;
};