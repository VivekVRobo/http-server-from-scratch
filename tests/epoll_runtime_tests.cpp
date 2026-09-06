#include "vhttp/net/socket.hpp"
#include "vhttp/server/epoll_runtime.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <exception>
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
bool wait_until(Predicate predicate, std::chrono::milliseconds timeout = 2500ms) {
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

struct RunningEpoll {
    explicit RunningEpoll(vhttp::server::EpollRuntime& value) : runtime(value) {
        thread = std::thread([this] {
            try {
                runtime.run("127.0.0.1", 0);
            } catch (...) {
                failure = std::current_exception();
            }
        });
        expect(runtime.wait_until_listening(3000ms), "epoll runtime should bind loopback listener");
    }

    ~RunningEpoll() {
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

    vhttp::server::EpollRuntime& runtime;
    std::thread thread;
    std::exception_ptr failure;
};

void serves_pipelined_requests_in_order() {
    vhttp::server::ConnectionConfig connection_config;
    connection_config.idle_timeout = 2000ms;

    vhttp::server::EpollConfig epoll_config;
    epoll_config.max_connections = 16;
    epoll_config.poll_interval = 10ms;

    vhttp::server::EpollRuntime runtime(
        [](const vhttp::http::HttpRequest& request) {
            return vhttp::http::HttpResponse::text(200, "OK", request.target + "\n");
        },
        connection_config,
        epoll_config);

    RunningEpoll running(runtime);
    auto client = vhttp::net::TcpStream::connect("127.0.0.1", runtime.bound_port());
    client.set_receive_timeout(3000ms);
    client.send_all(
        "GET /one HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "GET /two HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");

    const std::string wire = receive_until_close(client);
    expect(wire.find("HTTP/1.1 200 OK") != std::string::npos,
           "epoll runtime should serialize successful responses");
    const auto first = wire.find("/one\n");
    const auto second = wire.find("/two\n");
    expect(first != std::string::npos && second != std::string::npos,
           "both pipelined response bodies should be present");
    expect(first < second, "pipelined responses must preserve request order");
    expect(wait_until([&] { return runtime.stats().active == 0; }),
           "Connection: close should retire the event-loop connection");

    running.stop_and_join();
    const auto stats = runtime.stats();
    expect(stats.accepted == 1 && stats.rejected == 0,
           "single healthy client should be accepted without admission rejection");
    expect(stats.completed == 1 && stats.failed == 0,
           "healthy pipelined connection should complete without runtime failure");
    expect(stats.peak_active >= 1, "runtime should record peak active connection count");
}

void rejects_connections_above_admission_bound() {
    vhttp::server::ConnectionConfig connection_config;
    connection_config.idle_timeout = 1500ms;

    vhttp::server::EpollConfig epoll_config;
    epoll_config.max_connections = 1;
    epoll_config.poll_interval = 10ms;

    vhttp::server::EpollRuntime runtime(
        [](const vhttp::http::HttpRequest&) {
            return vhttp::http::HttpResponse::text(200, "OK", "held\n");
        },
        connection_config,
        epoll_config);

    RunningEpoll running(runtime);
    auto first = vhttp::net::TcpStream::connect("127.0.0.1", runtime.bound_port());
    first.send_all("GET /hold HTTP/1.1\r\nHost:");
    expect(wait_until([&] { return runtime.stats().active == 1; }),
           "partial first request should occupy the sole admission slot");

    auto second = vhttp::net::TcpStream::connect("127.0.0.1", runtime.bound_port());
    expect(wait_until([&] { return runtime.stats().rejected >= 1; }),
           "second accepted transport should be rejected at the connection bound");

    first.close();
    second.close();
    running.stop_and_join();

    const auto stats = runtime.stats();
    expect(stats.accepted >= 2, "admission test should observe both transport accepts");
    expect(stats.rejected >= 1, "admission bound should expose rejection accounting");
    expect(stats.peak_active == 1, "active connection count must never exceed configured bound");
    expect(stats.active == 0, "runtime should have no active connections after drain");
}

void retires_idle_connections_without_busy_waiting() {
    vhttp::server::ConnectionConfig connection_config;
    connection_config.idle_timeout = 120ms;

    vhttp::server::EpollConfig epoll_config;
    epoll_config.max_connections = 4;
    epoll_config.poll_interval = 10ms;

    vhttp::server::EpollRuntime runtime(
        [](const vhttp::http::HttpRequest&) {
            return vhttp::http::HttpResponse::text(200, "OK", "idle\n");
        },
        connection_config,
        epoll_config);

    RunningEpoll running(runtime);
    auto client = vhttp::net::TcpStream::connect("127.0.0.1", runtime.bound_port());
    expect(wait_until([&] { return runtime.stats().active == 1; }),
           "idle client should first become active");
    expect(wait_until([&] { return runtime.stats().active == 0; }, 2500ms),
           "idle timeout should retire a silent client");
    client.close();

    running.stop_and_join();
    const auto stats = runtime.stats();
    expect(stats.completed >= 1 && stats.failed == 0,
           "idle retirement should be a normal completed connection, not a runtime failure");
}

void bounds_pending_response_memory() {
    vhttp::server::ConnectionConfig connection_config;
    connection_config.idle_timeout = 1000ms;

    vhttp::server::EpollConfig epoll_config;
    epoll_config.max_connections = 4;
    epoll_config.max_pending_output_bytes = 128;
    epoll_config.poll_interval = 10ms;

    vhttp::server::EpollRuntime runtime(
        [](const vhttp::http::HttpRequest&) {
            return vhttp::http::HttpResponse::text(200, "OK", std::string(1024, 'x'));
        },
        connection_config,
        epoll_config);

    RunningEpoll running(runtime);
    auto client = vhttp::net::TcpStream::connect("127.0.0.1", runtime.bound_port());
    client.send_all("GET /large HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");

    expect(wait_until([&] { return runtime.stats().failed >= 1; }),
           "response larger than the event-loop output bound should fail the connection");
    expect(wait_until([&] { return runtime.stats().active == 0; }),
           "oversized response connection should be retired immediately");
    client.close();

    running.stop_and_join();
    const auto stats = runtime.stats();
    expect(stats.completed == 1 && stats.failed == 1,
           "bounded-output rejection should be visible in completed/failed counters");
}

void stop_drains_response_already_in_progress() {
    std::atomic<bool> handler_entered{false};

    vhttp::server::ConnectionConfig connection_config;
    connection_config.idle_timeout = 1500ms;

    vhttp::server::EpollConfig epoll_config;
    epoll_config.max_connections = 4;
    epoll_config.poll_interval = 10ms;

    vhttp::server::EpollRuntime runtime(
        [&](const vhttp::http::HttpRequest&) {
            handler_entered.store(true);
            std::this_thread::sleep_for(150ms);
            return vhttp::http::HttpResponse::text(200, "OK", "drained\n");
        },
        connection_config,
        epoll_config);

    RunningEpoll running(runtime);
    auto client = vhttp::net::TcpStream::connect("127.0.0.1", runtime.bound_port());
    client.set_receive_timeout(3000ms);
    client.send_all("GET /drain HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");

    expect(wait_until([&] { return handler_entered.load(); }),
           "handler should begin before the stop request");
    runtime.request_stop();

    const std::string wire = receive_until_close(client);
    expect(wire.find("HTTP/1.1 200 OK") != std::string::npos,
           "stop should not discard a response already being produced");
    expect(wire.find("drained\n") != std::string::npos,
           "active response body should finish during event-loop drain");

    running.stop_and_join();
    const auto stats = runtime.stats();
    expect(stats.completed == 1 && stats.failed == 0,
           "gracefully drained request should complete without failure");
}

void validates_epoll_configuration() {
    expect(vhttp::server::EpollRuntime::supported(), "epoll test target must run only on Linux");

    bool connection_error = false;
    try {
        vhttp::server::EpollConfig config;
        config.max_connections = 0;
        vhttp::server::EpollRuntime runtime(
            [](const auto&) { return vhttp::http::HttpResponse::text(200, "OK", ""); }, {}, config);
        (void)runtime;
    } catch (const std::invalid_argument&) {
        connection_error = true;
    }
    expect(connection_error, "zero epoll connection capacity should be rejected");

    bool output_error = false;
    try {
        vhttp::server::EpollConfig config;
        config.max_pending_output_bytes = 0;
        vhttp::server::EpollRuntime runtime(
            [](const auto&) { return vhttp::http::HttpResponse::text(200, "OK", ""); }, {}, config);
        (void)runtime;
    } catch (const std::invalid_argument&) {
        output_error = true;
    }
    expect(output_error, "zero pending output budget should be rejected");

    bool interval_error = false;
    try {
        vhttp::server::EpollConfig config;
        config.poll_interval = 0ms;
        vhttp::server::EpollRuntime runtime(
            [](const auto&) { return vhttp::http::HttpResponse::text(200, "OK", ""); }, {}, config);
        (void)runtime;
    } catch (const std::invalid_argument&) {
        interval_error = true;
    }
    expect(interval_error, "non-positive epoll poll interval should be rejected");
}

}  // namespace

int main() {
    vhttp::net::SocketRuntime socket_runtime;
    validates_epoll_configuration();
    serves_pipelined_requests_in_order();
    rejects_connections_above_admission_bound();
    retires_idle_connections_without_busy_waiting();
    bounds_pending_response_memory();
    stop_drains_response_already_in_progress();
    std::cout << "epoll runtime tests passed\n";
}
