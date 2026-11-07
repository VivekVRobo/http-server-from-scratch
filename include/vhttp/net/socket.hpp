#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace vhttp::net {

#ifdef _WIN32
using NativeSocket = SOCKET;
inline constexpr NativeSocket invalid_socket = INVALID_SOCKET;
#else
using NativeSocket = int;
inline constexpr NativeSocket invalid_socket = -1;
#endif

class SocketTimeoutError : public std::runtime_error {
public:
    SocketTimeoutError() : std::runtime_error("socket receive timed out") {}
};

class SocketRuntime {
public:
    SocketRuntime();
    ~SocketRuntime();
    SocketRuntime(const SocketRuntime&) = delete;
    SocketRuntime& operator=(const SocketRuntime&) = delete;
};

class TcpStream {
public:
    TcpStream() = default;
    explicit TcpStream(NativeSocket socket) noexcept : socket_(socket) {}
    ~TcpStream();

    TcpStream(const TcpStream&) = delete;
    TcpStream& operator=(const TcpStream&) = delete;
    TcpStream(TcpStream&& other) noexcept;
    TcpStream& operator=(TcpStream&& other) noexcept;

    static TcpStream connect(std::string_view host, std::uint16_t port);

    [[nodiscard]] bool valid() const noexcept { return socket_ != invalid_socket; }
    std::ptrdiff_t receive(char* buffer, std::size_t size);
    void send_all(std::string_view bytes);
    void shutdown_send();
    void set_receive_timeout(std::chrono::milliseconds timeout);
    void close() noexcept;

private:
    NativeSocket socket_ = invalid_socket;
};

class TcpListener {
public:
    TcpListener() = default;
    ~TcpListener();

    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;
    TcpListener(TcpListener&& other) noexcept;
    TcpListener& operator=(TcpListener&& other) noexcept;

    static TcpListener bind(std::string_view host, std::uint16_t port, int backlog = 128);
    TcpStream accept();
    [[nodiscard]] std::optional<TcpStream> accept_for(std::chrono::milliseconds timeout);
    [[nodiscard]] std::uint16_t local_port() const;
    void close() noexcept;
    [[nodiscard]] bool valid() const noexcept { return socket_ != invalid_socket; }

private:
    explicit TcpListener(NativeSocket socket) noexcept : socket_(socket) {}
    NativeSocket socket_ = invalid_socket;
};

}  // namespace vhttp::net
