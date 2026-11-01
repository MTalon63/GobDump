#include "usrp_local_server.h"
#include "remote.h"

#include "logger.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <limits.h>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace usrp_local_server
{
#ifdef _WIN32
    static HANDLE server_process = nullptr;
#else
    static pid_t server_pid = -1;
#endif
    static int server_port = -1;
    static std::mutex server_mtx;
    static bool atexit_registered = false;

    static std::filesystem::path executableDir()
    {
#ifdef _WIN32
        char path[MAX_PATH];
        DWORD len = GetModuleFileNameA(nullptr, path, MAX_PATH);
        if (len == 0)
            return {};
        return std::filesystem::path(std::string(path, len)).parent_path();
#elif defined(__APPLE__)
        char path[PATH_MAX];
        uint32_t size = sizeof(path);
        if (_NSGetExecutablePath(path, &size) != 0)
            return {};
        char resolved[PATH_MAX];
        if (realpath(path, resolved) != nullptr)
            return std::filesystem::path(resolved).parent_path();
        return std::filesystem::path(path).parent_path();
#else
        char path[PATH_MAX];
        ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
        if (len <= 0)
            return {};
        path[len] = 0;
        return std::filesystem::path(path).parent_path();
#endif
    }

    static bool serverAlive()
    {
#ifdef _WIN32
        if (server_process != nullptr)
        {
            DWORD code = 0;
            if (GetExitCodeProcess(server_process, &code) && code == STILL_ACTIVE)
                return true;
            CloseHandle(server_process);
            server_process = nullptr;
        }
#else
        if (server_pid > 0)
        {
            int status = 0;
            pid_t ret = waitpid(server_pid, &status, WNOHANG);
            if (ret == 0)
                return true;
            if (ret == server_pid)
                server_pid = -1;
        }
#endif
        return false;
    }

    static void stopServerLocked()
    {
#ifdef _WIN32
        if (server_process != nullptr)
        {
            TerminateProcess(server_process, 0);
            WaitForSingleObject(server_process, 1000);
            CloseHandle(server_process);
            server_process = nullptr;
        }
#else
        if (server_pid > 0)
        {
            kill(server_pid, SIGTERM);
            waitpid(server_pid, nullptr, 0);
            server_pid = -1;
        }
#endif
        server_port = -1;
    }

    static void stopServer()
    {
        std::lock_guard<std::mutex> lock(server_mtx);
        stopServerLocked();
    }

    static bool portFree(int port)
    {
#ifdef _WIN32
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
            return false;
        SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == INVALID_SOCKET)
        {
            WSACleanup();
            return false;
        }
#else
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0)
            return false;
#endif
        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);
        int ret = connect(sock, (sockaddr *)&addr, sizeof(addr));
#ifdef _WIN32
        closesocket(sock);
        WSACleanup();
#else
        close(sock);
#endif
        return ret != 0;
    }

    static bool spawnServerLocked()
    {
        auto exe = executableDir() / "satdump_sdr_server";
#ifdef _WIN32
        exe += ".exe";
#endif
        if (!std::filesystem::exists(exe))
        {
            logger->error("Could not find satdump_sdr_server next to the executable!");
            return false;
        }

        static int next_port = 5700;
        server_port = -1;
        for (int i = 0; i < 100; i++)
        {
            int candidate = next_port++;
            if (next_port > 5799)
                next_port = 5700;
            if (portFree(candidate))
            {
                server_port = candidate;
                break;
            }
        }
        if (server_port < 0)
        {
            logger->error("Could not find a free port for satdump_sdr_server!");
            return false;
        }

        logger->debug("Starting USRP server on port %d", server_port);
        std::string port = std::to_string(server_port);
#ifdef _WIN32
        std::string cmdline = "\"" + exe.string() + "\" " + port + " --uhd-server";
        STARTUPINFOA si = {};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi = {};
        if (!CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        {
            logger->error("Could not start satdump_sdr_server!");
            server_port = -1;
            return false;
        }
        CloseHandle(pi.hThread);
        server_process = pi.hProcess;
#else
        server_pid = fork();
        if (server_pid == 0)
        {
            setsid();
            execl(exe.c_str(), exe.c_str(), port.c_str(), "--uhd-server", (char *)nullptr);
            _exit(127);
        }
        if (server_pid < 0)
        {
            logger->error("Could not start satdump_sdr_server!");
            server_port = -1;
            return false;
        }
#endif

        if (!atexit_registered)
        {
            atexit(stopServer);
            atexit_registered = true;
        }
        return true;
    }

    std::vector<dsp::SourceDescriptor> getSources()
    {
        std::lock_guard<std::mutex> lock(server_mtx);
        if (!serverAlive() && !spawnServerLocked())
            return {};

        for (int attempt = 0; attempt < 2; attempt++)
        {
            for (int retry = 0; retry < 200; retry++)
            {
                if (!serverAlive())
                    break;

                try
                {
                    TCPClient client((char *)"127.0.0.1", server_port, 20);
                    client.readOne = true;

                    std::atomic<bool> got{false};
                    std::vector<dsp::SourceDescriptor> parsed;
                    client.callback_func = [&](uint8_t *buf, int len)
                    {
                        if (len < 2 || buf[0] != dsp::remote::PKT_TYPE_SOURCELIST)
                            return;
                        try
                        {
                            auto list = nlohmann::json::from_cbor(std::vector<uint8_t>(buf + 1, buf + len)).get<std::vector<dsp::SourceDescriptor>>();
                            for (auto &src : list)
                            {
                                if (src.source_type != "usrp")
                                    continue;
                                src.source_type = "remote";
                                src.name = "127.0.0.1:" + std::to_string(server_port) + " - " + src.name;
                                src.remote_ok = false;
                                parsed.push_back(src);
                            }
                        }
                        catch (const std::exception &e)
                        {
                            logger->error("Could not parse USRP server source list: %s", e.what());
                        }
                        got = true;
                    };

                    dsp::remote::sendPacketWithVector(&client, dsp::remote::PKT_TYPE_SOURCELIST);
                    for (int i = 0; i < 400 && !got; i++)
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));

                    if (got)
                    {
                        logger->debug("USRP server returned %zu sources", parsed.size());
                        return parsed;
                    }
                    break;
                }
                catch (const std::exception &e)
                {
                    logger->debug("USRP server connection failed: %s", e.what());
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }

            stopServerLocked();
            if (!spawnServerLocked())
                return {};
        }

        return {};
    }

    void stop() { stopServer(); }
}
