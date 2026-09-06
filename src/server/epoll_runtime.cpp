#include "vhttp/server/epoll_runtime.hpp"

#include "vhttp/server/connection_session.hpp"

#include <climits>
#include <exception>
#include <stdexcept>
#include <system_error>
#include <utility>

#if defined(__linux__)
#include <array>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <string>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>
#endif

namespace vhttp::server {

EpollRuntime::EpollRuntime(Handler handler,
                           ConnectionConfig connection_config,
                           EpollConfig epoll_config)
    : handler_(std::move(handler)),
      connection_config_(connection_config),
      epoll_config_(epoll_config) {
    if (!handler_) {
        throw std::invalid_argument("EpollRuntime requires a request handler");
    }
    if (connection_config_.max_requests_per_connection == 0) {
        throw std::invalid_argument("max_requests_per_connection must be greater than zero");
    }
    if (connection_config_.idle_timeout.count() <= 0) {
        throw std::invalid_argument("idle_timeout must be positive");
    }
    if (epoll_config_.max_connections == 0) {
        throw std::invalid_argument("epoll max_connections must be greater than zero");
    }
    if (epoll_config_.listen_backlog <= 0) {
        throw std::invalid_argument("epoll listen_backlog must be greater than zero");
    }
    if (epoll_config_.max_events == 0 ||
        epoll_config_.max_events > static_cast<std::size_t>(INT_MAX)) {
        throw std::invalid_argument("epoll max_events must be between 1 and INT_MAX");
    }
    if (epoll_config_.max_pending_output_bytes == 0) {
        throw std::invalid_argument("epoll max_pending_output_bytes must be greater than zero");
    }
    if (epoll_config_.poll_interval.count() <= 0 ||
        epoll_config_.poll_interval.count() > INT_MAX) {
        throw std::invalid_argument("epoll poll_interval must be between 1 ms and INT_MAX ms");
    }
}

EpollRuntime::~EpollRuntime() {
    request_stop();
}

void EpollRuntime::request_stop() noexcept {
    stop_requested_.store(true);
}

bool EpollRuntime::wait_until_listening(std::chrono::milliseconds timeout) {
    if (timeout.count() < 0) {
        throw std::invalid_argument("wait timeout must not be negative");
    }
    std::unique_lock lock(state_mutex_);
    return state_cv_.wait_for(lock, timeout, [this] { return listening_; });
}

std::uint16_t EpollRuntime::bound_port() const {
    std::lock_guard lock(state_mutex_);
    if (!listening_ && bound_port_ == 0) {
        throw std::logic_error("epoll runtime has not bound a listener");
    }
    return bound_port_;
}

bool EpollRuntime::running() const noexcept {
    return running_.load();
}

EpollStats EpollRuntime::stats() const noexcept {
    EpollStats snapshot;
    snapshot.accepted = accepted_.load();
    snapshot.rejected = rejected_.load();
    snapshot.completed = completed_.load();
    snapshot.failed = failed_.load();
    snapshot.active = active_.load();
    snapshot.peak_active = peak_active_.load();
    return snapshot;
}

void EpollRuntime::mark_not_running() noexcept {
    {
        std::lock_guard lock(state_mutex_);
        listening_ = false;
    }
    running_.store(false);
    state_cv_.notify_all();
}

#if defined(__linux__)
namespace {

using Clock = std::chrono::steady_clock;

struct EventConnection {
    EventConnection(const Handler& handler, ConnectionConfig config)
        : session(handler, config), last_activity(Clock::now()) {}

    ConnectionSession session;
    std::string output;
    std::size_t output_offset = 0;
    bool close_after_write = false;
    Clock::time_point last_activity;
};

[[noreturn]] void throw_errno(std::string_view operation) {
    throw std::system_error(errno, std::generic_category(), std::string(operation));
}

void close_fd(int& fd) noexcept {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

bool configure_nonblocking_cloexec(int fd) noexcept {
    const int status_flags = ::fcntl(fd, F_GETFL, 0);
    if (status_flags < 0 || ::fcntl(fd, F_SETFL, status_flags | O_NONBLOCK) < 0) {
        return false;
    }

    const int descriptor_flags = ::fcntl(fd, F_GETFD, 0);
    if (descriptor_flags < 0 || ::fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) < 0) {
        return false;
    }
    return true;
}

int bind_nonblocking_listener(const std::string& host,
                              std::uint16_t port,
                              int backlog) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = host.empty() ? AI_PASSIVE : 0;

    const std::string service = std::to_string(port);
    addrinfo* addresses = nullptr;
    const int lookup = ::getaddrinfo(
        host.empty() ? nullptr : host.c_str(), service.c_str(), &hints, &addresses);
    if (lookup != 0) {
        throw std::runtime_error(std::string("getaddrinfo failed: ") + ::gai_strerror(lookup));
    }

    int listener = -1;
    int last_error = 0;
    for (addrinfo* address = addresses; address != nullptr; address = address->ai_next) {
        const int candidate = ::socket(
            address->ai_family,
            address->ai_socktype,
            address->ai_protocol);
        if (candidate < 0) {
            last_error = errno;
            continue;
        }
        if (!configure_nonblocking_cloexec(candidate)) {
            last_error = errno;
            ::close(candidate);
            continue;
        }

        int reuse = 1;
        if (::setsockopt(
                candidate,
                SOL_SOCKET,
                SO_REUSEADDR,
                &reuse,
                static_cast<socklen_t>(sizeof(reuse))) != 0) {
            last_error = errno;
            ::close(candidate);
            continue;
        }
        if (::bind(candidate, address->ai_addr, address->ai_addrlen) != 0) {
            last_error = errno;
            ::close(candidate);
            continue;
        }
        if (::listen(candidate, backlog) != 0) {
            last_error = errno;
            ::close(candidate);
            continue;
        }

        listener = candidate;
        break;
    }
    ::freeaddrinfo(addresses);

    if (listener < 0) {
        const int error = last_error == 0 ? EADDRNOTAVAIL : last_error;
        throw std::system_error(error, std::generic_category(), "failed to bind epoll listener");
    }
    return listener;
}

std::uint16_t local_port(int fd) {
    sockaddr_storage address{};
    socklen_t length = static_cast<socklen_t>(sizeof(address));
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        throw_errno("getsockname");
    }

    if (address.ss_family == AF_INET) {
        const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(&address);
        return ntohs(ipv4->sin_port);
    }
    if (address.ss_family == AF_INET6) {
        const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(&address);
        return ntohs(ipv6->sin6_port);
    }
    throw std::runtime_error("epoll listener bound an unsupported address family");
}

void add_interest(int epoll_fd, int fd, std::uint32_t events) {
    epoll_event event{};
    event.events = events | EPOLLRDHUP;
    event.data.fd = fd;
    if (::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event) != 0) {
        throw_errno("epoll_ctl add");
    }
}

void modify_interest(int epoll_fd, int fd, std::uint32_t events) {
    epoll_event event{};
    event.events = events | EPOLLRDHUP;
    event.data.fd = fd;
    if (::epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &event) != 0) {
        throw_errno("epoll_ctl modify");
    }
}

}  // namespace
#endif

void EpollRuntime::run(std::string host, std::uint16_t port) {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        throw std::logic_error("epoll runtime is already running");
    }

    stop_requested_.store(false);
    accepted_.store(0);
    rejected_.store(0);
    completed_.store(0);
    failed_.store(0);
    active_.store(0);
    peak_active_.store(0);
    {
        std::lock_guard lock(state_mutex_);
        listening_ = false;
        bound_port_ = 0;
    }

#if !defined(__linux__)
    (void)host;
    (void)port;
    mark_not_running();
    throw std::runtime_error("EpollRuntime is supported only on Linux");
#else
    int listener_fd = -1;
    int epoll_fd = -1;
    std::unordered_map<int, EventConnection> connections;
    std::exception_ptr failure;

    auto close_connection = [&](int fd, bool failed) {
        const auto found = connections.find(fd);
        if (found == connections.end()) {
            return;
        }
        if (epoll_fd >= 0) {
            (void)::epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
        }
        ::close(fd);
        connections.erase(found);
        active_.fetch_sub(1);
        completed_.fetch_add(1);
        if (failed) {
            failed_.fetch_add(1);
        }
    };

    auto publish_peak = [&](std::size_t current) {
        std::size_t observed = peak_active_.load();
        while (observed < current &&
               !peak_active_.compare_exchange_weak(observed, current)) {
        }
    };

    auto set_response = [&](EventConnection& connection, SessionUpdate update) {
        if (update.state != SessionState::response_ready) {
            return true;
        }
        if (update.response.size() > epoll_config_.max_pending_output_bytes) {
            return false;
        }
        connection.output = std::move(update.response);
        connection.output_offset = 0;
        connection.close_after_write = update.close_after_write;
        return true;
    };

    try {
        listener_fd = bind_nonblocking_listener(host, port, epoll_config_.listen_backlog);
        epoll_fd = ::epoll_create1(EPOLL_CLOEXEC);
        if (epoll_fd < 0) {
            throw_errno("epoll_create1");
        }
        add_interest(epoll_fd, listener_fd, EPOLLIN);

        {
            std::lock_guard lock(state_mutex_);
            bound_port_ = local_port(listener_fd);
            listening_ = true;
        }
        state_cv_.notify_all();

        std::vector<epoll_event> events(epoll_config_.max_events);
        const int wait_timeout = static_cast<int>(epoll_config_.poll_interval.count());
        bool draining = false;

        while (true) {
            if (stop_requested_.load() && listener_fd >= 0) {
                (void)::epoll_ctl(epoll_fd, EPOLL_CTL_DEL, listener_fd, nullptr);
                close_fd(listener_fd);
                {
                    std::lock_guard lock(state_mutex_);
                    listening_ = false;
                }
                draining = true;
            }

            if (draining && connections.empty()) {
                break;
            }

            const int ready = ::epoll_wait(
                epoll_fd,
                events.data(),
                static_cast<int>(events.size()),
                wait_timeout);
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw_errno("epoll_wait");
            }

            for (int index = 0; index < ready; ++index) {
                const int fd = events[static_cast<std::size_t>(index)].data.fd;
                const std::uint32_t flags = events[static_cast<std::size_t>(index)].events;

                if (listener_fd >= 0 && fd == listener_fd) {
                    if ((flags & (EPOLLERR | EPOLLHUP)) != 0U) {
                        throw std::runtime_error("epoll listener reported an error/hangup");
                    }

                    while (true) {
                        const int client_fd = ::accept(listener_fd, nullptr, nullptr);
                        if (client_fd < 0) {
                            if (errno == EINTR || errno == ECONNABORTED) {
                                continue;
                            }
                            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                break;
                            }
                            throw_errno("accept");
                        }

                        accepted_.fetch_add(1);
                        if (!configure_nonblocking_cloexec(client_fd)) {
                            failed_.fetch_add(1);
                            ::close(client_fd);
                            continue;
                        }
                        if (connections.size() >= epoll_config_.max_connections) {
                            rejected_.fetch_add(1);
                            ::close(client_fd);
                            continue;
                        }

                        auto [connection_it, inserted] = connections.try_emplace(
                            client_fd, handler_, connection_config_);
                        if (!inserted) {
                            ::close(client_fd);
                            failed_.fetch_add(1);
                            continue;
                        }

                        try {
                            add_interest(epoll_fd, client_fd, EPOLLIN);
                        } catch (...) {
                            ::close(client_fd);
                            connections.erase(connection_it);
                            failed_.fetch_add(1);
                            continue;
                        }

                        const std::size_t current = active_.fetch_add(1) + 1;
                        publish_peak(current);
                    }
                    continue;
                }

                auto connection_it = connections.find(fd);
                if (connection_it == connections.end()) {
                    continue;
                }

                if ((flags & EPOLLERR) != 0U) {
                    close_connection(fd, true);
                    continue;
                }

                const bool peer_closed = (flags & (EPOLLRDHUP | EPOLLHUP)) != 0U;

                try {
                    if (!connection_it->second.output.empty()) {
                        if ((flags & EPOLLOUT) == 0U) {
                            if (peer_closed) {
                                close_connection(fd, false);
                            }
                            continue;
                        }

                        auto& connection = connection_it->second;
                        bool write_blocked = false;
                        while (connection.output_offset < connection.output.size()) {
                            const char* data = connection.output.data() + connection.output_offset;
                            const std::size_t remaining =
                                connection.output.size() - connection.output_offset;
                            const ssize_t sent = ::send(fd, data, remaining, MSG_NOSIGNAL);
                            if (sent > 0) {
                                connection.output_offset += static_cast<std::size_t>(sent);
                                connection.last_activity = Clock::now();
                                continue;
                            }
                            if (sent < 0 && errno == EINTR) {
                                continue;
                            }
                            if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                                write_blocked = true;
                                break;
                            }
                            close_connection(fd, true);
                            write_blocked = true;
                            break;
                        }

                        if (write_blocked || connections.find(fd) == connections.end()) {
                            continue;
                        }

                        const bool close_after_write = connection.close_after_write;
                        connection.output.clear();
                        connection.output_offset = 0;
                        connection.close_after_write = false;

                        if (close_after_write) {
                            close_connection(fd, false);
                            continue;
                        }

                        auto update = connection.session.on_write_complete();
                        if (update.state == SessionState::closed) {
                            close_connection(fd, false);
                            continue;
                        }
                        if (update.state == SessionState::response_ready) {
                            if (!set_response(connection, std::move(update))) {
                                close_connection(fd, true);
                                continue;
                            }
                            modify_interest(epoll_fd, fd, EPOLLOUT);
                            continue;
                        }

                        if (peer_closed) {
                            close_connection(fd, false);
                            continue;
                        }
                        modify_interest(epoll_fd, fd, EPOLLIN);
                        continue;
                    }

                    if ((flags & EPOLLIN) != 0U) {
                        bool read_peer_closed = peer_closed;
                        bool switched_to_write = false;
                        std::array<char, 8192> buffer{};

                        while (true) {
                            const ssize_t received = ::recv(fd, buffer.data(), buffer.size(), 0);
                            if (received > 0) {
                                connection_it->second.last_activity = Clock::now();
                                auto update = connection_it->second.session.feed(
                                    std::string_view(
                                        buffer.data(), static_cast<std::size_t>(received)));
                                if (update.state == SessionState::response_ready) {
                                    if (!set_response(connection_it->second, std::move(update))) {
                                        close_connection(fd, true);
                                        switched_to_write = true;
                                        break;
                                    }
                                    modify_interest(epoll_fd, fd, EPOLLOUT);
                                    switched_to_write = true;
                                    break;
                                }
                                if (update.state == SessionState::closed) {
                                    close_connection(fd, false);
                                    switched_to_write = true;
                                    break;
                                }
                                continue;
                            }
                            if (received == 0) {
                                read_peer_closed = true;
                                break;
                            }
                            if (errno == EINTR) {
                                continue;
                            }
                            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                break;
                            }
                            close_connection(fd, true);
                            switched_to_write = true;
                            break;
                        }

                        if (switched_to_write || connections.find(fd) == connections.end()) {
                            continue;
                        }
                        if (read_peer_closed) {
                            close_connection(fd, false);
                            continue;
                        }
                        continue;
                    }

                    if (peer_closed) {
                        close_connection(fd, false);
                    }
                } catch (...) {
                    close_connection(fd, true);
                }
            }

            const auto now = Clock::now();
            for (auto connection_it = connections.begin();
                 connection_it != connections.end();) {
                if (now - connection_it->second.last_activity >= connection_config_.idle_timeout) {
                    const int expired_fd = connection_it->first;
                    ++connection_it;
                    close_connection(expired_fd, false);
                } else {
                    ++connection_it;
                }
            }
        }
    } catch (...) {
        failure = std::current_exception();
    }

    while (!connections.empty()) {
        close_connection(connections.begin()->first, failure != nullptr);
    }
    close_fd(listener_fd);
    close_fd(epoll_fd);
    mark_not_running();

    if (failure) {
        std::rethrow_exception(failure);
    }
#endif
}

}  // namespace vhttp::server
