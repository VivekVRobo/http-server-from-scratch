#include "vhttp/server/thread_pool_runtime.hpp"

#include "vhttp/net/socket.hpp"

#include <exception>
#include <stdexcept>
#include <utility>

namespace vhttp::server {

ThreadPoolRuntime::ThreadPoolRuntime(Handler handler,
                                     ConnectionConfig connection_config,
                                     ThreadPoolConfig pool_config)
    : connection_server_(std::move(handler), connection_config),
      pool_config_(pool_config) {
    if (pool_config_.worker_count == 0) {
        throw std::invalid_argument("thread-pool worker_count must be greater than zero");
    }
    if (pool_config_.max_pending_connections == 0) {
        throw std::invalid_argument("thread-pool max_pending_connections must be greater than zero");
    }
    if (pool_config_.listen_backlog <= 0) {
        throw std::invalid_argument("thread-pool listen_backlog must be greater than zero");
    }
    if (pool_config_.accept_poll_interval.count() <= 0) {
        throw std::invalid_argument("thread-pool accept_poll_interval must be positive");
    }
}

ThreadPoolRuntime::~ThreadPoolRuntime() {
    request_stop();
}

void ThreadPoolRuntime::run(std::string host, std::uint16_t port) {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        throw std::logic_error("thread-pool runtime is already running");
    }

    stop_requested_.store(false);
    accepted_.store(0);
    rejected_.store(0);
    completed_.store(0);
    failed_.store(0);
    active_.store(0);
    peak_active_.store(0);
    {
        std::lock_guard lock(queue_mutex_);
        queue_.clear();
        workers_stopping_ = false;
    }
    {
        std::lock_guard lock(state_mutex_);
        listening_ = false;
        bound_port_ = 0;
    }

    std::exception_ptr failure;
    try {
        net::SocketRuntime socket_runtime;
        auto listener = net::TcpListener::bind(host, port, pool_config_.listen_backlog);
        {
            std::lock_guard lock(state_mutex_);
            bound_port_ = listener.local_port();
            listening_ = true;
        }
        state_cv_.notify_all();

        start_workers();

        while (!stop_requested_.load()) {
            auto accepted_stream = listener.accept_for(pool_config_.accept_poll_interval);
            if (!accepted_stream.has_value()) {
                continue;
            }

            accepted_.fetch_add(1);
            bool queued = false;
            {
                std::lock_guard lock(queue_mutex_);
                if (!stop_requested_.load() && queue_.size() < pool_config_.max_pending_connections) {
                    queue_.push_back(std::move(*accepted_stream));
                    queued = true;
                }
            }

            if (queued) {
                queue_cv_.notify_one();
            } else {
                rejected_.fetch_add(1);
                accepted_stream->close();
            }
        }
    } catch (...) {
        failure = std::current_exception();
        stop_requested_.store(true);
    }

    stop_workers_and_drain();
    mark_not_running();

    if (failure) {
        std::rethrow_exception(failure);
    }
}

void ThreadPoolRuntime::request_stop() noexcept {
    stop_requested_.store(true);
    queue_cv_.notify_all();
}

bool ThreadPoolRuntime::wait_until_listening(std::chrono::milliseconds timeout) {
    if (timeout.count() < 0) {
        throw std::invalid_argument("wait timeout must not be negative");
    }
    std::unique_lock lock(state_mutex_);
    return state_cv_.wait_for(lock, timeout, [this] { return listening_; });
}

std::uint16_t ThreadPoolRuntime::bound_port() const {
    std::lock_guard lock(state_mutex_);
    if (!listening_ && bound_port_ == 0) {
        throw std::logic_error("thread-pool runtime has not bound a listener");
    }
    return bound_port_;
}

bool ThreadPoolRuntime::running() const noexcept {
    return running_.load();
}

ThreadPoolStats ThreadPoolRuntime::stats() const {
    ThreadPoolStats snapshot;
    snapshot.accepted = accepted_.load();
    snapshot.rejected = rejected_.load();
    snapshot.completed = completed_.load();
    snapshot.failed = failed_.load();
    snapshot.active = active_.load();
    snapshot.peak_active = peak_active_.load();
    {
        std::lock_guard lock(queue_mutex_);
        snapshot.queued = queue_.size();
    }
    return snapshot;
}

void ThreadPoolRuntime::worker_loop() {
    while (true) {
        net::TcpStream stream;
        {
            std::unique_lock lock(queue_mutex_);
            queue_cv_.wait(lock, [this] { return workers_stopping_ || !queue_.empty(); });
            if (queue_.empty()) {
                if (workers_stopping_) {
                    return;
                }
                continue;
            }
            stream = std::move(queue_.front());
            queue_.pop_front();
            const std::size_t current = active_.fetch_add(1) + 1;
            publish_peak_active(current);
        }

        try {
            // The transport/parser state is connection-local. Applications that
            // capture mutable state inside Handler are responsible for making
            // that shared application state safe for concurrent invocation.
            connection_server_.serve_connection(std::move(stream));
        } catch (...) {
            failed_.fetch_add(1);
        }

        active_.fetch_sub(1);
        completed_.fetch_add(1);
    }
}

void ThreadPoolRuntime::start_workers() {
    workers_.clear();
    workers_.reserve(pool_config_.worker_count);
    for (std::size_t i = 0; i < pool_config_.worker_count; ++i) {
        workers_.emplace_back([this] { worker_loop(); });
    }
}

void ThreadPoolRuntime::stop_workers_and_drain() noexcept {
    {
        std::lock_guard lock(queue_mutex_);
        workers_stopping_ = true;
    }
    queue_cv_.notify_all();

    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
}

void ThreadPoolRuntime::mark_not_running() noexcept {
    {
        std::lock_guard lock(state_mutex_);
        listening_ = false;
    }
    running_.store(false);
    state_cv_.notify_all();
}

void ThreadPoolRuntime::publish_peak_active(std::size_t current) noexcept {
    std::size_t observed = peak_active_.load();
    while (observed < current && !peak_active_.compare_exchange_weak(observed, current)) {
    }
}

}  // namespace vhttp::server
