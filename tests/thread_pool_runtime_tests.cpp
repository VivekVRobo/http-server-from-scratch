#include "vhttp/net/socket.hpp"
#include "vhttp/server/thread_pool_runtime.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

namespace {

using namespace std::chrono_literals;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

template <class Predicate>
bool wait_until(Predicate predicate, std::chrono::milliseconds timeout = 2000ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return predicate();
}

std::string receive_until_close(vhttp::net::TcpStream& stream) {
    std::array<char, 4096> buffer{};
    std::string result;
    while (true) {
        const auto received = stream.receive(buffer.data(), buffer.size());
        if (received == 0) {
            break;
        }
        result.append(buffer.data(), static_cast<std::size_t>(received));
    }
    return result;
}

struct RunningRuntime {
    explicit RunningRuntime(vhttp::server::ThreadPoolRuntime& value) : runtime(value) {
        thread = std::thread([this] {
            try {
                runtime.run("127.0.0.1", 0);
            } catch (...) {
                failure = std::current_exception();
            }
        });
        expect(runtime.wait_until_listening(3000ms), "runtime should bind loopback listener");
    }

    ~RunningRuntime() {
        runtime.request_stop();
        if (thread.joinable()) {
            thread.join();
        }
    }

    void stop_and_join() {
        runtime.request_stop();
        if (thread.joinable()) {
            thread.join();
        }
        if (failure) {
            std::rethrow_exception(failure);
        }
    }

    vhttp::server::ThreadPoolRuntime& runtime;
    std::thread thread;
    std::exception_ptr failure;
};

void handles_connections_concurrently() {
    std::atomic<int> entered{0};
    std::atomic<int> active_handlers{0};
    std::atomic<int> max_active{0};

    vhttp::server::ConnectionConfig connection_config;
    connection_config.idle_timeout = 2000ms;

    vhttp::server::ThreadPoolConfig pool_config;
    pool_config.worker_count = 2;
    pool_config.max_pending_connections = 4;
    pool_config.accept_poll_interval = 20ms;

    vhttp::server::ThreadPoolRuntime runtime(
        [&](const vhttp::http::HttpRequest&) {
            const int current = active_handlers.fetch_add(1) + 1;
            int observed = max_active.load();
            while (observed < current && !max_active.compare_exchange_weak(observed, current)) {
            }
            entered.fetch_add(1);

            const auto deadline = std::chrono::steady_clock::now() + 1000ms;
            while (entered.load() < 2 && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(2ms);
            }

            active_handlers.fetch_sub(1);
            return vhttp::http::HttpResponse::text(200, "OK", "concurrent\n");
        },
        connection_config,
        pool_config);

    RunningRuntime running(runtime);
    const auto port = runtime.bound_port();

    std::string first_wire;
    std::string second_wire;
    std::thread first([&] {
        auto client = vhttp::net::TcpStream::connect("127.0.0.1", port);
        client.set_receive_timeout(3000ms);
        client.send_all("GET /a HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        first_wire = receive_until_close(client);
    });
    std::thread second([&] {
        auto client = vhttp::net::TcpStream::connect("127.0.0.1", port);
        client.set_receive_timeout(3000ms);
        client.send_all("GET /b HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        second_wire = receive_until_close(client);
    });

    first.join();
    second.join();
    running.stop_and_join();

    expect(first_wire.find("HTTP/1.1 200 OK") != std::string::npos, "first concurrent request should succeed");
    expect(second_wire.find("HTTP/1.1 200 OK") != std::string::npos, "second concurrent request should succeed");
    expect(max_active.load() >= 2, "two workers should execute independent handlers concurrently");

    const auto stats = runtime.stats();
    expect(stats.accepted == 2, "two clients should be accepted");
    expect(stats.rejected == 0, "healthy concurrent load should not be rejected");
    expect(stats.completed == 2 && stats.failed == 0, "both concurrent connections should complete cleanly");
    expect(stats.peak_active >= 2, "runtime should record the observed concurrent active-connection peak");
    expect(stats.active == 0 && stats.queued == 0, "runtime should be drained after join");
}

void bounded_queue_rejects_excess_connections() {
    vhttp::server::ConnectionConfig connection_config;
    connection_config.idle_timeout = 3000ms;

    vhttp::server::ThreadPoolConfig pool_config;
    pool_config.worker_count = 1;
    pool_config.max_pending_connections = 1;
    pool_config.accept_poll_interval = 10ms;

    vhttp::server::ThreadPoolRuntime runtime(
        [](const vhttp::http::HttpRequest&) {
            return vhttp::http::HttpResponse::text(200, "OK", "unexpected\n");
        },
        connection_config,
        pool_config);

    RunningRuntime running(runtime);
    const auto port = runtime.bound_port();

    auto first = vhttp::net::TcpStream::connect("127.0.0.1", port);
    first.send_all("GET /one HTTP/1.1\r\nHost:");
    expect(wait_until([&] { return runtime.stats().active == 1; }),
           "first partial client should occupy the only worker");

    auto second = vhttp::net::TcpStream::connect("127.0.0.1", port);
    second.send_all("GET /two HTTP/1.1\r\nHost:");
    expect(wait_until([&] { return runtime.stats().queued == 1; }),
           "second partial client should occupy the bounded pending slot");

    auto third = vhttp::net::TcpStream::connect("127.0.0.1", port);
    third.set_receive_timeout(2000ms);
    third.send_all("GET /three HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    expect(wait_until([&] { return runtime.stats().rejected >= 1; }),
           "third client should be rejected once active+pending capacity is saturated");

    first.close();
    second.close();
    third.close();
    running.stop_and_join();

    const auto stats = runtime.stats();
    expect(stats.accepted >= 3, "saturation scenario should accept three transport connections");
    expect(stats.rejected >= 1, "bounded queue should report at least one rejection");
    expect(stats.completed >= 2, "active and queued connections should be drained after peer close");
    expect(stats.peak_active == 1, "one-worker runtime must never report more than one active connection");
    expect(stats.active == 0 && stats.queued == 0, "drained runtime should expose no active or queued connections");
}

void stop_drains_active_request_before_returning() {
    std::atomic<bool> handler_entered{false};

    vhttp::server::ConnectionConfig connection_config;
    connection_config.idle_timeout = 2000ms;

    vhttp::server::ThreadPoolConfig pool_config;
    pool_config.worker_count = 1;
    pool_config.max_pending_connections = 2;
    pool_config.accept_poll_interval = 10ms;

    vhttp::server::ThreadPoolRuntime runtime(
        [&](const vhttp::http::HttpRequest&) {
            handler_entered.store(true);
            std::this_thread::sleep_for(150ms);
            return vhttp::http::HttpResponse::text(200, "OK", "drained\n");
        },
        connection_config,
        pool_config);

    RunningRuntime running(runtime);
    auto client = vhttp::net::TcpStream::connect("127.0.0.1", runtime.bound_port());
    client.set_receive_timeout(3000ms);
    client.send_all("GET /drain HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");

    expect(wait_until([&] { return handler_entered.load(); }), "handler should begin before stop request");
    runtime.request_stop();

    const auto wire = receive_until_close(client);
    running.stop_and_join();

    expect(wire.find("HTTP/1.1 200 OK") != std::string::npos,
           "graceful stop should allow active request to finish and write its response");
    expect(wire.find("drained\n") != std::string::npos, "active response body should complete before runtime returns");
    const auto stats = runtime.stats();
    expect(stats.completed == 1 && stats.failed == 0, "drain should report the active connection as completed");
    expect(stats.peak_active == 1, "single active drain should be represented in peak-active evidence");
}

void validates_pool_configuration() {
    bool worker_error = false;
    try {
        vhttp::server::ThreadPoolConfig config;
        config.worker_count = 0;
        vhttp::server::ThreadPoolRuntime runtime(
            [](const auto&) { return vhttp::http::HttpResponse::text(200, "OK", ""); }, {}, config);
        (void)runtime;
    } catch (const std::invalid_argument&) {
        worker_error = true;
    }
    expect(worker_error, "zero workers should be rejected");

    bool queue_error = false;
    try {
        vhttp::server::ThreadPoolConfig config;
        config.max_pending_connections = 0;
        vhttp::server::ThreadPoolRuntime runtime(
            [](const auto&) { return vhttp::http::HttpResponse::text(200, "OK", ""); }, {}, config);
        (void)runtime;
    } catch (const std::invalid_argument&) {
        queue_error = true;
    }
    expect(queue_error, "zero pending capacity should be rejected");
}

}  // namespace

int main() {
    vhttp::net::SocketRuntime socket_runtime;
    validates_pool_configuration();
    handles_connections_concurrently();
    bounded_queue_rejects_excess_connections();
    stop_drains_active_request_before_returning();
    std::cout << "thread pool runtime tests passed\n";
}
