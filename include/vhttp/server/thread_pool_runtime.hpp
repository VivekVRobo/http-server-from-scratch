#pragma once

#include "vhttp/server/server.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vhttp::server {

struct ThreadPoolConfig {
    std::size_t worker_count = 4;
    std::size_t max_pending_connections = 128;
    int listen_backlog = 128;
    std::chrono::milliseconds accept_poll_interval{50};
};

struct ThreadPoolStats {
    std::uint64_t accepted = 0;
    std::uint64_t rejected = 0;
    std::uint64_t completed = 0;
    std::uint64_t failed = 0;
    std::size_t active = 0;
    std::size_t queued = 0;
    std::size_t peak_active = 0;
};

class ThreadPoolRuntime {
public:
    ThreadPoolRuntime(Handler handler,
                      ConnectionConfig connection_config = {},
                      ThreadPoolConfig pool_config = {});
    ~ThreadPoolRuntime();

    ThreadPoolRuntime(const ThreadPoolRuntime&) = delete;
    ThreadPoolRuntime& operator=(const ThreadPoolRuntime&) = delete;

    // Blocks until request_stop() is called. Accepted connections are handed to
    // a bounded fixed worker pool. On stop, accepting ceases and queued/active
    // connections are allowed to drain before run() returns.
    void run(std::string host, std::uint16_t port);

    void request_stop() noexcept;

    // Useful when run() executes on a controlling thread and port 0 was used.
    [[nodiscard]] bool wait_until_listening(std::chrono::milliseconds timeout);
    [[nodiscard]] std::uint16_t bound_port() const;
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] ThreadPoolStats stats() const;

private:
    void worker_loop();
    void start_workers();
    void stop_workers_and_drain() noexcept;
    void mark_not_running() noexcept;
    void publish_peak_active(std::size_t current) noexcept;

    Server connection_server_;
    ThreadPoolConfig pool_config_;

    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> accepted_{0};
    std::atomic<std::uint64_t> rejected_{0};
    std::atomic<std::uint64_t> completed_{0};
    std::atomic<std::uint64_t> failed_{0};
    std::atomic<std::size_t> active_{0};
    std::atomic<std::size_t> peak_active_{0};

    mutable std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<net::TcpStream> queue_;
    bool workers_stopping_ = false;
    std::vector<std::thread> workers_;

    mutable std::mutex state_mutex_;
    std::condition_variable state_cv_;
    bool listening_ = false;
    std::uint16_t bound_port_ = 0;
};

}  // namespace vhttp::server
