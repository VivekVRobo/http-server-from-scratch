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
        "POST / HTTP/1.1\r\nContent-Length: 4\r\nContent-Length: 5\r\n\r\nhello");
    expect(status == vhttp::http::ParseStatus::error, "conflicting Content-Length rejected");
}

void rejects_unsupported_transfer_coding() {
    vhttp::http::HttpRequestParser parser;
    const auto status = parser.feed(
        "POST / HTTP/1.1\r\nTransfer-Encoding: gzip\r\n\r\nhello");
    expect(status == vhttp::http::ParseStatus::error, "unsupported Transfer-Encoding rejected explicitly");
}

void rejects_control_and_non_ascii_request_target_octets() {
    const unsigned char invalid_bytes[] = {'\t', '\0', 0x1FU, 0x7FU, 0x80U};
    for (const unsigned char invalid_byte : invalid_bytes) {
        std::string request = "GET /path";
        request.push_back(static_cast<char>(invalid_byte));
        request += "tail HTTP/1.1\r\n\r\n";

        vhttp::http::HttpRequestParser parser;
        expect(parser.feed(request) == vhttp::http::ParseStatus::error,
               "control and non-ASCII request-target octets must be rejected");
    }

    vhttp::http::HttpRequestParser parser;
    expect(parser.feed("GET /caf%C3%A9 HTTP/1.1\r\n\r\n") ==
               vhttp::http::ParseStatus::complete,
           "percent-encoded non-ASCII path bytes remain valid");
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
    parses_body_and_preserves_extra_bytes();
    rejects_conflicting_content_length();
    rejects_unsupported_transfer_coding();
    enforces_request_line_limit();
    rejects_control_and_non_ascii_request_target_octets();
    std::cout << "parser tests passed\n";
}
