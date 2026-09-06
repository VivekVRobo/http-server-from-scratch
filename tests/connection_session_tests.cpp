#include "vhttp/server/connection_session.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using namespace std::chrono_literals;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

void fragmented_request_waits_for_completion() {
    int calls = 0;
    vhttp::server::ConnectionSession session(
        [&](const vhttp::http::HttpRequest& request) {
            ++calls;
            return vhttp::http::HttpResponse::text(200, "OK", request.target + "\n");
        });

    const auto first = session.feed("GET /fragmented HTTP/1.1\r\nHost:");
    expect(first.state == vhttp::server::SessionState::need_more_data,
           "fragmented request should remain in read state");
    expect(calls == 0, "handler must not run before the request is complete");

    const auto second = session.feed(" localhost\r\nConnection: close\r\n\r\n");
    expect(second.state == vhttp::server::SessionState::response_ready,
           "completed fragmented request should produce one response");
    expect(second.close_after_write, "Connection: close should retire the session after the response");
    expect(second.response.find("HTTP/1.1 200 OK") != std::string::npos,
           "fragmented request should receive a successful response");
    expect(second.response.find("/fragmented\n") != std::string::npos,
           "handler response body should be serialized");
    expect(calls == 1, "handler should execute exactly once for the completed request");

    const auto closed = session.on_write_complete();
    expect(closed.state == vhttp::server::SessionState::closed,
           "session should close after the requested final response is written");
}

void pipeline_dispatches_one_response_at_a_time() {
    int calls = 0;
    vhttp::server::ConnectionSession session(
        [&](const vhttp::http::HttpRequest& request) {
            ++calls;
            return vhttp::http::HttpResponse::text(200, "OK", request.target + "\n");
        });

    const std::string wire =
        "GET /one HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "GET /two HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";

    const auto first = session.feed(wire);
    expect(first.state == vhttp::server::SessionState::response_ready,
           "first pipelined request should produce a response");
    expect(!first.close_after_write, "first HTTP/1.1 request should remain persistent");
    expect(first.response.find("/one\n") != std::string::npos,
           "first response should correspond to the first request");
    expect(first.response.find("/two\n") == std::string::npos,
           "first response must not contain the second response");
    expect(calls == 1,
           "second pipelined request must not dispatch before the first response is fully written");

    bool feed_rejected = false;
    try {
        (void)session.feed("GET /illegal HTTP/1.1\r\nHost: localhost\r\n\r\n");
    } catch (const std::logic_error&) {
        feed_rejected = true;
    }
    expect(feed_rejected, "runtime must not feed additional input while a response is pending");

    const auto second = session.on_write_complete();
    expect(second.state == vhttp::server::SessionState::response_ready,
           "draining the first response should immediately expose the pipelined second response");
    expect(second.close_after_write, "second request explicitly asks the session to close");
    expect(second.response.find("/two\n") != std::string::npos,
           "second response should correspond to the second request");
    expect(calls == 2, "second handler should run only after the first write completes");

    const auto closed = session.on_write_complete();
    expect(closed.state == vhttp::server::SessionState::closed,
           "session should close after the final pipelined response is written");
    expect(session.requests_served() == 2, "session should count both completed requests");
}

void request_ceiling_closes_exactly_at_limit() {
    vhttp::server::ConnectionConfig config;
    config.max_requests_per_connection = 2;
    config.idle_timeout = 1000ms;

    vhttp::server::ConnectionSession session(
        [](const vhttp::http::HttpRequest& request) {
            return vhttp::http::HttpResponse::text(200, "OK", request.target + "\n");
        },
        config);

    const std::string wire =
        "GET /first HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "GET /second HTTP/1.1\r\nHost: localhost\r\n\r\n";

    const auto first = session.feed(wire);
    expect(!first.close_after_write, "request before the ceiling should remain persistent");
    expect(first.response.find("Connection: keep-alive") != std::string::npos,
           "first response should advertise the persistent decision");

    const auto second = session.on_write_complete();
    expect(second.state == vhttp::server::SessionState::response_ready,
           "second pipelined request should be dispatched after first write");
    expect(second.close_after_write, "request at the configured ceiling should close the connection");
    expect(second.response.find("Connection: close") != std::string::npos,
           "serialized response must match the request-ceiling closure decision");
    expect(session.requests_served() == 2, "request counter should stop at the configured ceiling");
}

void parser_error_produces_single_closing_400() {
    int calls = 0;
    vhttp::server::ConnectionSession session(
        [&](const vhttp::http::HttpRequest&) {
            ++calls;
            return vhttp::http::HttpResponse::text(200, "OK", "unexpected\n");
        });

    const auto update = session.feed("NOT-A-VALID-REQUEST-LINE\r\n\r\n");
    expect(update.state == vhttp::server::SessionState::response_ready,
           "parser failure should produce a response-ready update");
    expect(update.close_after_write, "bad request response must close the connection");
    expect(update.response.find("HTTP/1.1 400 Bad Request") != std::string::npos,
           "parser failure should serialize HTTP 400");
    expect(update.response.find("Connection: close") != std::string::npos,
           "bad request response should advertise closure");
    expect(calls == 0, "application handler must not run for a malformed request");

    const auto closed = session.on_write_complete();
    expect(closed.state == vhttp::server::SessionState::closed,
           "session should close once the 400 response has been written");
}

void head_uses_get_metadata_without_payload_bytes() {
    vhttp::server::ConnectionSession session(
        [](const vhttp::http::HttpRequest&) {
            return vhttp::http::HttpResponse::text(200, "OK", "payload");
        });

    const auto update = session.feed(
        "HEAD /resource HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    expect(update.state == vhttp::server::SessionState::response_ready,
           "HEAD request should produce a response");
    expect(update.response.find("Content-Length: 7") != std::string::npos,
           "HEAD response should retain the GET-equivalent content length");
    expect(update.response.find("\r\n\r\npayload") == std::string::npos,
           "HEAD response must suppress payload bytes");
}

void handler_close_request_overrides_persistence() {
    vhttp::server::ConnectionSession session(
        [](const vhttp::http::HttpRequest&) {
            auto response = vhttp::http::HttpResponse::text(200, "OK", "bye\n");
            response.set_header("Connection", "close");
            return response;
        });

    const auto update = session.feed("GET /close HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expect(update.close_after_write, "handler Connection: close request should retire the session");
    expect(update.response.find("Connection: close") != std::string::npos,
           "serializer should emit the final close decision exactly once");
}

void validates_session_configuration() {
    bool empty_handler_error = false;
    try {
        vhttp::server::Handler handler;
        vhttp::server::ConnectionSession session(handler);
        (void)session;
    } catch (const std::invalid_argument&) {
        empty_handler_error = true;
    }
    expect(empty_handler_error, "empty request handler should be rejected");

    bool request_limit_error = false;
    try {
        vhttp::server::ConnectionConfig config;
        config.max_requests_per_connection = 0;
        vhttp::server::ConnectionSession session(
            [](const auto&) { return vhttp::http::HttpResponse::text(200, "OK", ""); }, config);
        (void)session;
    } catch (const std::invalid_argument&) {
        request_limit_error = true;
    }
    expect(request_limit_error, "zero request ceiling should be rejected");

    bool timeout_error = false;
    try {
        vhttp::server::ConnectionConfig config;
        config.idle_timeout = 0ms;
        vhttp::server::ConnectionSession session(
            [](const auto&) { return vhttp::http::HttpResponse::text(200, "OK", ""); }, config);
        (void)session;
    } catch (const std::invalid_argument&) {
        timeout_error = true;
    }
    expect(timeout_error, "non-positive idle timeout should be rejected");
}

}  // namespace

int main() {
    fragmented_request_waits_for_completion();
    pipeline_dispatches_one_response_at_a_time();
    request_ceiling_closes_exactly_at_limit();
    parser_error_produces_single_closing_400();
    head_uses_get_metadata_without_payload_bytes();
    handler_close_request_overrides_persistence();
    validates_session_configuration();
    std::cout << "connection session tests passed\n";
}
