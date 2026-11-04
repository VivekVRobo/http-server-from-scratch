#include "vhttp/http/parser.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

void parses_fragmented_request() {
    vhttp::http::HttpRequestParser parser;
    expect(parser.feed("GET /hel") == vhttp::http::ParseStatus::need_more_data, "fragment 1 should need more data");
    expect(parser.feed("lo HTTP/1.1\r\nHo") == vhttp::http::ParseStatus::need_more_data, "fragment 2 should need more data");
    expect(parser.feed("st: example.com\r\nUser-Agent: test\r\n\r\n") == vhttp::http::ParseStatus::complete,
           "fragmented request should complete");

    const auto& request = parser.request();
    expect(request.method == "GET", "method parsed");
    expect(request.target == "/hello", "target parsed");
    expect(request.version == "HTTP/1.1", "version parsed");
    expect(request.header("HOST") == "example.com", "header lookup is case-insensitive");
}

void rejects_http11_without_host() {
    vhttp::http::HttpRequestParser parser;
    const auto status = parser.feed("GET / HTTP/1.1\r\nUser-Agent: test\r\n\r\n");
    expect(status == vhttp::http::ParseStatus::error,
           "HTTP/1.1 requests without Host must be rejected");
}

void rejects_empty_http11_host() {
    vhttp::http::HttpRequestParser parser;
    const auto status = parser.feed("GET / HTTP/1.1\r\nHost:   \r\n\r\n");
    expect(status == vhttp::http::ParseStatus::error,
           "HTTP/1.1 requests with an empty Host value must be rejected");
}

void rejects_duplicate_http11_host() {
    vhttp::http::HttpRequestParser parser;
    const auto status = parser.feed(
        "GET / HTTP/1.1\r\nHost: example.com\r\nhost: example.org\r\n\r\n");
    expect(status == vhttp::http::ParseStatus::error,
           "HTTP/1.1 requests with multiple Host field lines must be rejected");
}

void accepts_http10_without_host() {
    vhttp::http::HttpRequestParser parser;
    const auto status = parser.feed("GET / HTTP/1.0\r\n\r\n");
    expect(status == vhttp::http::ParseStatus::complete,
           "HTTP/1.0 requests may omit Host");
}

void parses_body_and_preserves_extra_bytes() {
    vhttp::http::HttpRequestParser parser;
    const std::string payload =
        "POST /items HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\n\r\nhelloNEXT";
    expect(parser.feed(payload) == vhttp::http::ParseStatus::complete, "request with body should complete");
    expect(parser.request().body == "hello", "body respects Content-Length");
    expect(parser.take_remaining() == "NEXT", "extra bytes preserved for future keep-alive parser");
}

void rejects_conflicting_content_length() {
    vhttp::http::HttpRequestParser parser;
    const auto status = parser.feed(
        "POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 4\r\nContent-Length: 5\r\n\r\nhello");
    expect(status == vhttp::http::ParseStatus::error, "conflicting Content-Length rejected");
}

void rejects_unsupported_transfer_coding() {
    vhttp::http::HttpRequestParser parser;
    const auto status = parser.feed(
        "POST / HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: gzip\r\n\r\nhello");
    expect(status == vhttp::http::ParseStatus::error, "unsupported Transfer-Encoding rejected explicitly");
}

void enforces_request_line_limit() {
    vhttp::http::ParserLimits limits;
    limits.max_request_line_bytes = 16;
    vhttp::http::HttpRequestParser parser(limits);
    const auto status = parser.feed("GET /this-is-way-too-long HTTP/1.1");
    expect(status == vhttp::http::ParseStatus::error, "oversized request line rejected");
}

}  // namespace

int main() {
    parses_fragmented_request();
    rejects_http11_without_host();
    rejects_empty_http11_host();
    rejects_duplicate_http11_host();
    accepts_http10_without_host();
    parses_body_and_preserves_extra_bytes();
    rejects_conflicting_content_length();
    rejects_unsupported_transfer_coding();
    enforces_request_line_limit();
    std::cout << "parser tests passed\n";
}
