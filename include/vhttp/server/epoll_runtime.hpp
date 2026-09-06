#pragma once

#include "vhttp/server/server.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

namespace vhttp::server {

struct EpollConfig {
    std::size_t max_connections = 4096;
    int listen_backlog = 512;
    std::size_t max_events = 256;
    std::size_t max_pending_output_bytes = 16 * 1024 * 1024;
    std::chrono::milliseconds poll_interval{50};
};

struct EpollStats {
    std::uint64_t accepted = 0;
    std::uint64_t rejected = 0;
    std::uint64_t completed = 0;
    std::uint64_t failed = 0;
    std::size_t active = 0;
    std::size_t peak_active = 0;
};

// Linux event-driven runtime backed by epoll. HTTP parsing, persistence,
// pipelining, request ceilings, HEAD behavior, and response serialization are
// delegated to ConnectionSession so this runtime does not fork HTTP semantics.
//
// The type is visible on every platform so cross-platform consumers can compile
// feature-detection code. run() throws std::runtime_error on non-Linux systems.
class EpollRuntime {
public:
    EpollRuntime(Handler handler,
                 ConnectionConfig connection_config = {},
                 EpollConfig epoll_config = {});
    ~EpollRuntime();

    EpollRuntime(const EpollRuntime&) = delete;
    EpollRuntime& operator=(const EpollRuntime&) = delete;

    [[nodiscard]] static constexpr bool supported() noexcept {
#if defined(__linux__)
        return true;
#else
        return false;
#endif
    }

    // Blocks until request_stop() is observed and active connections have
    // drained or retired under the normal connection idle-timeout policy.
    void run(std::string host, std::uint16_t port);
    void request_stop() noexcept;

    [[nodiscard]] bool wait_until_listening(std::chrono::milliseconds timeout);
    [[nodiscard]] std::uint16_t bound_port() const;
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] EpollStats stats() const noexcept;

private:
    void mark_not_running() noexcept;

    Handler handler_;
    ConnectionConfig connection_config_;
    EpollConfig epoll_config_;

    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> accepted_{0};
    std::atomic<std::uint64_t> rejected_{0};
    std::atomic<std::uint64_t> completed_{0};
    std::atomic<std::uint64_t> failed_{0};
    std::atomic<std::size_t> active_{0};
    std::atomic<std::size_t> peak_active_{0};

    mutable std::mutex state_mutex_;
    std::condition_variable state_cv_;
    bool listening_ = false;
    std::uint16_t bound_port_ = 0;
};

}  // namespace vhttp::server
