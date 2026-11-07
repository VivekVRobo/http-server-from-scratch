#include "vhttp/net/socket.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace vhttp::net {
namespace {

void close_native(NativeSocket socket) noexcept {
    if (socket == invalid_socket) {
        return;
    }
#ifdef _WIN32
    ::closesocket(socket);
#else
    ::close(socket);
#endif
}

[[noreturn]] void throw_socket_error(const char* operation) {
#ifdef _WIN32
    throw std::system_error(WSAGetLastError(), std::system_category(), operation);
#else
    throw std::system_error(errno, std::generic_category(), operation);
#endif
}

addrinfo* resolve(std::string_view host, std::uint16_t port, bool passive) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = passive ? AI_PASSIVE : 0;

    const std::string host_string(host);
    const std::string port_string = std::to_string(port);
    addrinfo* results = nullptr;
    const int gai = ::getaddrinfo(
        passive && host.empty() ? nullptr : host_string.c_str(),
        port_string.c_str(), &hints, &results);
    if (gai != 0) {
#ifdef _WIN32
        throw std::runtime_error("getaddrinfo failed with code " + std::to_string(gai));
#else
        throw std::runtime_error(std::string("getaddrinfo failed: ") + ::gai_strerror(gai));
#endif
    }
    return results;
}

}  // namespace

SocketRuntime::SocketRuntime() {
#ifdef _WIN32
    WSADATA data{};
    const int rc = ::WSAStartup(MAKEWORD(2, 2), &data);
    if (rc != 0) {
        throw std::system_error(rc, std::system_category(), "WSAStartup");
    }
#endif
}

SocketRuntime::~SocketRuntime() {
#ifdef _WIN32
    ::WSACleanup();
#endif
}

TcpStream::~TcpStream() { close(); }

TcpStream::TcpStream(TcpStream&& other) noexcept : socket_(std::exchange(other.socket_, invalid_socket)) {}

TcpStream& TcpStream::operator=(TcpStream&& other) noexcept {
    if (this != &other) {
        close();
        socket_ = std::exchange(other.socket_, invalid_socket);
    }
    return *this;
}

TcpStream TcpStream::connect(std::string_view host, std::uint16_t port) {
    addrinfo* results = resolve(host, port, false);
    NativeSocket connected = invalid_socket;

    for (auto* current = results; current != nullptr; current = current->ai_next) {
        NativeSocket candidate = ::socket(current->ai_family, current->ai_socktype, current->ai_protocol);
        if (candidate == invalid_socket) {
            continue;
        }
        if (::connect(candidate, current->ai_addr, static_cast<int>(current->ai_addrlen)) == 0) {
            connected = candidate;
            break;
        }
        close_native(candidate);
    }

    ::freeaddrinfo(results);
    if (connected == invalid_socket) {
        throw_socket_error("connect");
    }
    return TcpStream(connected);
}

std::ptrdiff_t TcpStream::receive(char* buffer, std::size_t size) {
#ifdef _WIN32
    const int capped = size > static_cast<std::size_t>(INT_MAX) ? INT_MAX : static_cast<int>(size);
    const int rc = ::recv(socket_, buffer, capped, 0);
    if (rc == SOCKET_ERROR) {
        const int error = ::WSAGetLastError();
        if (error == WSAETIMEDOUT || error == WSAEWOULDBLOCK) {
            throw SocketTimeoutError();
        }
        throw std::system_error(error, std::system_category(), "recv");
    }
    return static_cast<std::ptrdiff_t>(rc);
#else
    const auto rc = ::recv(socket_, buffer, size, 0);
    if (rc < 0) {
        if (errno == EINTR) {
            return receive(buffer, size);
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            throw SocketTimeoutError();
        }
        throw_socket_error("recv");
    }
    return rc;
#endif
}

void TcpStream::send_all(std::string_view bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
#ifdef _WIN32
        const auto remaining = bytes.size() - sent;
        const int capped = remaining > static_cast<std::size_t>(INT_MAX) ? INT_MAX : static_cast<int>(remaining);
        const int rc = ::send(socket_, bytes.data() + sent, capped, 0);
        if (rc == SOCKET_ERROR) {
            throw_socket_error("send");
        }
#else
        const auto rc = ::send(socket_, bytes.data() + sent, bytes.size() - sent, 0);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw_socket_error("send");
        }
#endif
        if (rc == 0) {
            throw std::runtime_error("send returned zero before payload was transmitted");
        }
        sent += static_cast<std::size_t>(rc);
    }
}

void TcpStream::shutdown_send() {
    if (!valid()) {
        throw std::logic_error("cannot half-close an invalid socket");
    }
#ifdef _WIN32
    if (::shutdown(socket_, SD_SEND) == SOCKET_ERROR) {
        throw_socket_error("shutdown(SD_SEND)");
    }
#else
    if (::shutdown(socket_, SHUT_WR) != 0) {
        throw_socket_error("shutdown(SHUT_WR)");
    }
#endif
}

void TcpStream::set_receive_timeout(std::chrono::milliseconds timeout) {
    if (!valid()) {
        throw std::logic_error("cannot configure an invalid socket");
    }
    if (timeout.count() <= 0) {
        throw std::invalid_argument("receive timeout must be positive");
    }

#ifdef _WIN32
    const auto capped = std::min<std::uint64_t>(
        static_cast<std::uint64_t>(timeout.count()),
        static_cast<std::uint64_t>(std::numeric_limits<DWORD>::max()));
    const DWORD value = static_cast<DWORD>(capped);
    if (::setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char*>(&value), sizeof(value)) == SOCKET_ERROR) {
        throw_socket_error("setsockopt(SO_RCVTIMEO)");
    }
#else
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(timeout);
    const auto remainder = timeout - std::chrono::duration_cast<std::chrono::milliseconds>(seconds);
    timeval value{};
    value.tv_sec = static_cast<time_t>(seconds.count());
    value.tv_usec = static_cast<suseconds_t>(remainder.count() * 1000);
    if (::setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value)) < 0) {
        throw_socket_error("setsockopt(SO_RCVTIMEO)");
    }
#endif
}

void TcpStream::close() noexcept {
    close_native(socket_);
    socket_ = invalid_socket;
}

TcpListener::~TcpListener() { close(); }

TcpListener::TcpListener(TcpListener&& other) noexcept : socket_(std::exchange(other.socket_, invalid_socket)) {}

TcpListener& TcpListener::operator=(TcpListener&& other) noexcept {
    if (this != &other) {
        close();
        socket_ = std::exchange(other.socket_, invalid_socket);
    }
    return *this;
}

TcpListener TcpListener::bind(std::string_view host, std::uint16_t port, int backlog) {
    addrinfo* results = resolve(host, port, true);
    NativeSocket bound = invalid_socket;

    for (auto* current = results; current != nullptr; current = current->ai_next) {
        NativeSocket candidate = ::socket(current->ai_family, current->ai_socktype, current->ai_protocol);
        if (candidate == invalid_socket) {
            continue;
        }

        int reuse = 1;
#ifdef _WIN32
        ::setsockopt(candidate, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#else
        ::setsockopt(candidate, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif

        if (::bind(candidate, current->ai_addr, static_cast<int>(current->ai_addrlen)) == 0 &&
            ::listen(candidate, backlog) == 0) {
            bound = candidate;
            break;
        }
        close_native(candidate);
    }

    ::freeaddrinfo(results);
    if (bound == invalid_socket) {
        throw_socket_error("bind/listen");
    }
    return TcpListener(bound);
}

TcpStream TcpListener::accept() {
    const NativeSocket client = ::accept(socket_, nullptr, nullptr);
    if (client == invalid_socket) {
        throw_socket_error("accept");
    }
    return TcpStream(client);
}

std::optional<TcpStream> TcpListener::accept_for(std::chrono::milliseconds timeout) {
    if (!valid()) {
        throw std::logic_error("cannot accept on an invalid listener");
    }
    if (timeout.count() < 0) {
        throw std::invalid_argument("accept timeout must not be negative");
    }

    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(socket_, &readable);

    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(timeout);
    const auto remainder = timeout - std::chrono::duration_cast<std::chrono::milliseconds>(seconds);
    timeval wait{};
    wait.tv_sec = static_cast<long>(seconds.count());
    wait.tv_usec = static_cast<long>(remainder.count() * 1000);

#ifdef _WIN32
    const int ready = ::select(0, &readable, nullptr, nullptr, &wait);
    if (ready == SOCKET_ERROR) {
        throw_socket_error("select(accept)");
    }
#else
    int ready = 0;
    do {
        ready = ::select(socket_ + 1, &readable, nullptr, nullptr, &wait);
    } while (ready < 0 && errno == EINTR);
    if (ready < 0) {
        throw_socket_error("select(accept)");
    }
#endif

    if (ready == 0) {
        return std::nullopt;
    }
    return accept();
}

std::uint16_t TcpListener::local_port() const {
    if (!valid()) {
        throw std::logic_error("cannot query an invalid listener");
    }

    sockaddr_storage address{};
#ifdef _WIN32
    int length = sizeof(address);
#else
    socklen_t length = sizeof(address);
#endif
    if (::getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        throw_socket_error("getsockname");
    }

    if (address.ss_family == AF_INET) {
        const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(&address);
        return ntohs(ipv4->sin_port);
    }
    if (address.ss_family == AF_INET6) {
        const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(&address);
        return ntohs(ipv6->sin6_port);
    }
    throw std::runtime_error("unsupported socket family from getsockname");
}

void TcpListener::close() noexcept {
    close_native(socket_);
    socket_ = invalid_socket;
}

}  // namespace vhttp::net
