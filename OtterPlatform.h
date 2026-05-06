#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <io.h>
#pragma comment(lib, "ws2_32")
#define OTTER_PLATFORM_WINDOWS 1
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#define OTTER_PLATFORM_POSIX 1
#endif

namespace Otter::Platform
{
#if defined(OTTER_PLATFORM_WINDOWS)
    using NativeSocket = SOCKET;
    inline constexpr NativeSocket invalid_socket = INVALID_SOCKET;
    inline constexpr int socket_error = SOCKET_ERROR;
    using SockLen = int;
#else
    using NativeSocket = int;
    inline constexpr NativeSocket invalid_socket = -1;
    inline constexpr int socket_error = -1;
    using SockLen = socklen_t;
#endif

    inline void sleep_ms(uint32_t ms)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }

    inline uint64_t monotonic_ms()
    {
        using namespace std::chrono;
        return static_cast<uint64_t>(
            duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
    }

    inline uint64_t random_seed()
    {
        const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        return static_cast<uint64_t>(now) ^ static_cast<uint64_t>(
            reinterpret_cast<uintptr_t>(&now));
    }

    inline void debug_output(std::string_view text)
    {
#if defined(OTTER_PLATFORM_WINDOWS)
        OutputDebugStringA(std::string(text).c_str());
#else
        (void)text;
#endif
    }

    class NetworkSystem
    {
    public:
        NetworkSystem()
        {
#if defined(OTTER_PLATFORM_WINDOWS)
            WSADATA data{};
            ok_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
            ok_ = true;
#endif
        }

        ~NetworkSystem() noexcept
        {
#if defined(OTTER_PLATFORM_WINDOWS)
            if (ok_)
                WSACleanup();
#endif
        }

        NetworkSystem(const NetworkSystem&) = delete;
        NetworkSystem& operator=(const NetworkSystem&) = delete;

        bool ok() const noexcept { return ok_; }

    private:
        bool ok_ = false;
    };

    inline bool network_system_ready()
    {
        static NetworkSystem system;
        return system.ok();
    }

    inline int close_socket(NativeSocket socket) noexcept
    {
#if defined(OTTER_PLATFORM_WINDOWS)
        return closesocket(socket);
#else
        return ::close(socket);
#endif
    }

    inline bool set_socket_nonblocking(NativeSocket socket, bool enable)
    {
#if defined(OTTER_PLATFORM_WINDOWS)
        u_long mode = enable ? 1ul : 0ul;
        return ioctlsocket(socket, FIONBIO, &mode) == 0;
#else
        const int flags = fcntl(socket, F_GETFL, 0);
        if (flags < 0)
            return false;
        const int updated = enable ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
        return fcntl(socket, F_SETFL, updated) == 0;
#endif
    }

    inline bool socket_would_block()
    {
#if defined(OTTER_PLATFORM_WINDOWS)
        const int error = WSAGetLastError();
        return error == WSAEWOULDBLOCK || error == WSAEINTR;
#else
        return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR;
#endif
    }

    inline bool inet_ntop_v4(const in_addr& address, char* buffer, size_t buffer_size)
    {
#if defined(OTTER_PLATFORM_WINDOWS)
        return InetNtopA(AF_INET, const_cast<in_addr*>(&address), buffer,
                         static_cast<DWORD>(buffer_size)) != nullptr;
#else
        return inet_ntop(AF_INET, &address, buffer, static_cast<socklen_t>(buffer_size)) != nullptr;
#endif
    }
}
