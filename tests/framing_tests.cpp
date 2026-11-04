#include "vhttp/http/parser.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

void decodes_fragmented_chunks_extensions_trailers_and_remaining_bytes() {
    vhttp::http::HttpRequestParser parser;
    expect(parser.feed(
               "POST /wiki HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n"
               "Trailer: X-Checksum\r\n\r\n4;foo=bar\r\nWi") ==
               vhttp::http::ParseStatus::need_more_data,
           "partial first chunk should need more data");

    expect(parser.feed(
               "ki\r\n5\r\npedia\r\n0\r\nX-Checksum: abc123\r\n\r\nNEXT") ==
               vhttp::http::ParseStatus::complete,
           "fragmented chunked body should complete");

    expect(parser.request().body == "Wikipedia", "decoded chunks should be concatenated");
    expect(parser.request().trailer("x-checksum") == "abc123", "allowed trailer should be parsed");
    expect(parser.take_remaining() == "NEXT", "bytes after trailer terminator should be preserved");
}

void accepts_ows_around_single_chunked_coding() {
    vhttp::http::HttpRequestParser parser;
    const auto status = parser.feed(
        "POST / HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding:\tchunked \t\r\n\r\n0\r\n\r\n");
    expect(status == vhttp::http::ParseStatus::complete, "OWS around chunked coding should be accepted");
}

void rejects_transfer_encoding_plus_content_length() {
    vhttp::http::HttpRequestParser parser;
    const auto status = parser.feed(
        "POST / HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\nContent-Length: 4\r\n\r\n"
        "4\r\ntest\r\n0\r\n\r\n");
    expect(status == vhttp::http::ParseStatus::error,
           "Transfer-Encoding plus Content-Length must be rejected as ambiguous framing");
}

void rejects_nonfinal_or_unsupported_transfer_codings() {
    vhttp::http::HttpRequestParser parser;
    const auto status = parser.feed(
        "POST / HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: gzip, chunked\r\n\r\n0\r\n\r\n");
    expect(status == vhttp::http::ParseStatus::error,
           "unsupported coding chains should be rejected instead of partially decoded");
}

void rejects_chunked_on_http10() {
    vhttp::http::HttpRequestParser parser;
    const auto status = parser.feed(
        "POST / HTTP/1.0\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n");
    expect(status == vhttp::http::ParseStatus::error, "HTTP/1.0 chunked request should be rejected");
}

void rejects_invalid_or_overflowing_chunk_size() {
    {
        vhttp::http::HttpRequestParser parser;
        const auto status = parser.feed(
            "POST / HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\nZ\r\n");
        expect(status == vhttp::http::ParseStatus::error, "non-hex chunk size should be rejected");
    }
    {
        vhttp::http::HttpRequestParser parser;
        const auto status = parser.feed(
            "POST / HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n"
            "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF\r\n");
        expect(status == vhttp::http::ParseStatus::error, "overflowing chunk size should be rejected");
    }
}

void rejects_missing_crlf_after_chunk_data() {
    vhttp::http::HttpRequestParser parser;
    const auto status = parser.feed(
        "POST / HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n1\r\naZZ");
    expect(status == vhttp::http::ParseStatus::error, "chunk data must be followed by CRLF");
}

void enforces_decoded_body_limit_before_buffering_chunk() {
    vhttp::http::ParserLimits limits;
    limits.max_body_bytes = 4;
    vhttp::http::HttpRequestParser parser(limits);
    const auto status = parser.feed(
        "POST / HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n5\r\n");
    expect(status == vhttp::http::ParseStatus::error, "oversized decoded chunk should be rejected from size line");
}

void enforces_chunk_line_limit() {
    vhttp::http::ParserLimits limits;
    limits.max_chunk_line_bytes = 4;
    vhttp::http::HttpRequestParser parser(limits);
    const auto status = parser.feed(
        "POST / HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n1;toolong");
    expect(status == vhttp::http::ParseStatus::error, "oversized chunk-size/extension line should be rejected");
}

void rejects_forbidden_trailer_fields() {
    vhttp::http::HttpRequestParser parser;
    const auto status = parser.feed(
        "POST / HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n"
        "0\r\nContent-Length: 10\r\n\r\n");
    expect(status == vhttp::http::ParseStatus::error, "framing fields must not be accepted from trailers");
}

void rejects_empty_chunk_extension() {
    vhttp::http::HttpRequestParser parser;
    const auto status = parser.feed(
        "POST / HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n1;\r\n");
    expect(status == vhttp::http::ParseStatus::error, "bare semicolon chunk extension should be rejected");
}

}  // namespace

int main() {
    decodes_fragmented_chunks_extensions_trailers_and_remaining_bytes();
    accepts_ows_around_single_chunked_coding();
    rejects_transfer_encoding_plus_content_length();
    rejects_nonfinal_or_unsupported_transfer_codings();
    rejects_chunked_on_http10();
    rejects_invalid_or_overflowing_chunk_size();
    rejects_missing_crlf_after_chunk_data();
    enforces_decoded_body_limit_before_buffering_chunk();
    enforces_chunk_line_limit();
    rejects_forbidden_trailer_fields();
    rejects_empty_chunk_extension();
    std::cout << "framing tests passed\n";
}
