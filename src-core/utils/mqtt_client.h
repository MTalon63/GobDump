#pragma once

#include "libs/mqttc/mqtt.h"
#include <functional>
#include <string>
#include <thread>

namespace satdump
{
    // Failure is INVALID_SOCKET on Windows (an unsigned ~0), not -1
#ifdef _WIN32
#define MQTT_INVALID_SOCKET INVALID_SOCKET
#else
#define MQTT_INVALID_SOCKET (-1)
#endif

    class MQTTClient
    {
    private:
        // Was `int`: the handle is SOCKET on Windows (64-bit unsigned), so that truncated it
        mqtt_pal_socket_handle sockfd = 0;
        struct mqtt_client client;
        char *client_id = NULL;

        uint8_t *sendbuf;
        uint8_t *recvbuf;

        std::function<void(std::string topic, uint8_t *data, int len)> callback;

    private:
        static void publish_callback(void **publish_response_callback_state, struct mqtt_response_publish *published);

        std::thread run_refresh_th;
        bool run_refresh = true;
        static void *client_refresher(void *client);

        void close_socket();

    public:
        MQTTClient(std::string addr, std::string port, int bufsize, std::function<void(std::string topic, uint8_t *data, int len)> callback = [](std::string, uint8_t *, int) {});

        ~MQTTClient();

        void publish(std::string topic, uint8_t *data, int len, uint8_t publish_flags);

        void subscribe(std::string topic, int max_qos = MQTT_PUBLISH_QOS_0);
    };
} // namespace satdump