#include "vhttp/http/parser.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <limits>
#include <utility>

namespace vhttp::http {
namespace {

std::string ascii_lower(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    for (const unsigned char ch : input) {
        out.push_back(static_cast<char>(std::tolower(ch)));
    }
    return out;
}

bool ascii_iequals(std::string_view lhs, std::string_view rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        const auto a = static_cast<char>(std::tolower(static_cast<unsigned char>(lhs[i])));
        const auto b = static_cast<char>(std::tolower(static_cast<unsigned char>(rhs[i])));
        if (a != b) {
            return false;
        }
    }
    return true;
}

std::string_view trim_ows(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    return value;
}

bool valid_token(std::string_view value) {
    if (value.empty()) {
        return false;
    }

    constexpr std::string_view separators = "()<>@,;:\\\"/[]?={} \t";
    for (const unsigned char ch : value) {
        if (ch <= 0x20U || ch >= 0x7FU || separators.find(static_cast<char>(ch)) != std::string_view::npos) {
            return false;
        }
    }
    return true;
}

bool valid_request_target(std::string_view value) {
    // HTTP request-targets are URI syntax: raw whitespace, control bytes, DEL,
    // and non-ASCII bytes must be percent-encoded before reaching the parser.
    for (const unsigned char ch : value) {
        if (ch < 0x21U || ch > 0x7EU) {
            return false;
        }
    }
    return true;
}

int hex_digit(unsigned char ch) {
    if (ch >= '0' && ch <= '9') {
        return static_cast<int>(ch - '0');
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + static_cast<int>(ch - 'a');
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + static_cast<int>(ch - 'A');
    }
    return -1;
}

bool forbidden_trailer_name(std::string_view name) {
    // Keep framing, routing, and connection metadata out of trailers. More
    // application-specific trailer policy can be layered above this parser.
    return name == "content-length" || name == "transfer-encoding" ||
           name == "host" || name == "connection" || name == "trailer";
}

}  // namespace

std::string_view HttpRequest::header(std::string_view name) const {
    const auto key = ascii_lower(name);
    const auto it = headers.find(key);
    if (it == headers.end()) {
        return {};
    }
    return it->second;
}

std::string_view HttpRequest::trailer(std::string_view name) const {
    const auto key = ascii_lower(name);
    const auto it = trailers.find(key);
    if (it == trailers.end()) {
        return {};
    }
    return it->second;
}

std::string_view HttpRequest::path_param(std::string_view name) const {
    const auto it = path_params.find(std::string(name));
    return it == path_params.end() ? std::string_view{} : std::string_view(it->second);
}

std::string_view HttpRequest::query(std::string_view name) const {
    const auto it = query_params.find(std::string(name));
    if (it == query_params.end() || it->second.empty()) {
        return {};
    }
    return it->second.front();
}

HttpRequestParser::HttpRequestParser(ParserLimits limits) : limits_(limits) {}

ParseStatus HttpRequestParser::feed(std::string_view bytes) {
    if (state_ == State::complete) {
        buffer_.append(bytes.data(), bytes.size());
        return ParseStatus::complete;
    }
    if (state_ == State::error) {
        return ParseStatus::error;
    }

    buffer_.append(bytes.data(), bytes.size());
    return parse_available();
}

ParseStatus HttpRequestParser::parse_available() {
    while (true) {
        if (state_ == State::request_line || state_ == State::headers) {
            const auto pos = buffer_.find("\r\n");
            if (pos == std::string::npos) {
                if (state_ == State::request_line && buffer_.size() > limits_.max_request_line_bytes) {
                    fail("request line exceeds configured limit");
                    return ParseStatus::error;
                }
                if (state_ == State::headers && header_bytes_ + buffer_.size() > limits_.max_header_bytes) {
                    fail("headers exceed configured byte limit");
                    return ParseStatus::error;
                }
                return ParseStatus::need_more_data;
            }

            const std::string line = buffer_.substr(0, pos);
            buffer_.erase(0, pos + 2);

            if (state_ == State::request_line) {
                if (line.size() > limits_.max_request_line_bytes || !parse_request_line(line)) {
                    return ParseStatus::error;
                }
                state_ = State::headers;
                continue;
            }

            header_bytes_ += line.size() + 2;
            if (header_bytes_ > limits_.max_header_bytes) {
                fail("headers exceed configured byte limit");
                return ParseStatus::error;
            }

            if (line.empty()) {
                if (!finalize_headers()) {
                    return ParseStatus::error;
                }
                if (chunked_) {
                    state_ = State::chunk_size;
                    continue;
                }
                if (!content_length_.has_value() || *content_length_ == 0) {
                    state_ = State::complete;
                    return ParseStatus::complete;
                }
                state_ = State::content_length_body;
                continue;
            }

            if (++header_count_ > limits_.max_header_count) {
                fail("header count exceeds configured limit");
                return ParseStatus::error;
            }
            if (!parse_header_line(line)) {
                return ParseStatus::error;
            }
            continue;
        }

        if (state_ == State::content_length_body) {
            const auto needed = *content_length_ - request_.body.size();
            const auto take = std::min(needed, buffer_.size());
            request_.body.append(buffer_.data(), take);
            buffer_.erase(0, take);

            if (request_.body.size() == *content_length_) {
                state_ = State::complete;
                return ParseStatus::complete;
            }
            return ParseStatus::need_more_data;
        }

        if (state_ == State::chunk_size) {
            const auto pos = buffer_.find("\r\n");
            if (pos == std::string::npos) {
                if (buffer_.size() > limits_.max_chunk_line_bytes) {
                    fail("chunk-size line exceeds configured limit");
                    return ParseStatus::error;
                }
                return ParseStatus::need_more_data;
            }
            if (pos > limits_.max_chunk_line_bytes) {
                fail("chunk-size line exceeds configured limit");
                return ParseStatus::error;
            }

            const std::string line = buffer_.substr(0, pos);
            buffer_.erase(0, pos + 2);
            if (!parse_chunk_size_line(line)) {
                return ParseStatus::error;
            }
            state_ = current_chunk_size_ == 0 ? State::trailers : State::chunk_data;
            continue;
        }

        if (state_ == State::chunk_data) {
            if (buffer_.size() < current_chunk_size_) {
                return ParseStatus::need_more_data;
            }
            request_.body.append(buffer_.data(), current_chunk_size_);
            buffer_.erase(0, current_chunk_size_);
            state_ = State::chunk_data_crlf;
            continue;
        }

        if (state_ == State::chunk_data_crlf) {
            if (buffer_.size() < 2) {
                return ParseStatus::need_more_data;
            }
            if (buffer_[0] != '\r' || buffer_[1] != '\n') {
                fail("chunk data is not followed by CRLF");
                return ParseStatus::error;
            }
            buffer_.erase(0, 2);
            state_ = State::chunk_size;
            continue;
        }

        if (state_ == State::trailers) {
            const auto pos = buffer_.find("\r\n");
            if (pos == std::string::npos) {
                if (trailer_bytes_ + buffer_.size() > limits_.max_trailer_bytes) {
                    fail("trailers exceed configured byte limit");
                    return ParseStatus::error;
                }
                return ParseStatus::need_more_data;
            }

            const std::string line = buffer_.substr(0, pos);
            buffer_.erase(0, pos + 2);
            trailer_bytes_ += line.size() + 2;
            if (trailer_bytes_ > limits_.max_trailer_bytes) {
                fail("trailers exceed configured byte limit");
                return ParseStatus::error;
            }

            if (line.empty()) {
                state_ = State::complete;
                return ParseStatus::complete;
            }

            if (++trailer_count_ > limits_.max_trailer_count) {
                fail("trailer count exceeds configured limit");
                return ParseStatus::error;
            }
            if (!parse_trailer_line(line)) {
                return ParseStatus::error;
            }
            continue;
        }

        return state_ == State::complete ? ParseStatus::complete : ParseStatus::error;
    }
}

bool HttpRequestParser::parse_request_line(std::string_view line) {
    const auto first_space = line.find(' ');
    if (first_space == std::string_view::npos) {
        fail("malformed request line: missing method separator");
        return false;
    }
    const auto second_space = line.find(' ', first_space + 1);
    if (second_space == std::string_view::npos || line.find(' ', second_space + 1) != std::string_view::npos) {
        fail("malformed request line: expected METHOD SP TARGET SP VERSION");
        return false;
    }

    const auto method = line.substr(0, first_space);
    const auto target = line.substr(first_space + 1, second_space - first_space - 1);
    const auto version = line.substr(second_space + 1);

    if (!valid_token(method)) {
        fail("invalid HTTP method token");
        return false;
    }
    if (target.empty()) {
        fail("empty request target");
        return false;
    }
    if (!valid_request_target(target)) {
        fail("invalid request target: only visible ASCII bytes are allowed");
        return false;
    }
    if (version != "HTTP/1.1" && version != "HTTP/1.0") {
        fail("unsupported HTTP version");
        return false;
    }

    request_.method.assign(method);
    request_.target.assign(target);
    request_.version.assign(version);
    return true;
}

bool HttpRequestParser::parse_header_line(std::string_view line) {
    const auto colon = line.find(':');
    if (colon == std::string_view::npos) {
        fail("malformed header: missing colon");
        return false;
    }

    const auto raw_name = line.substr(0, colon);
    if (!valid_token(raw_name)) {
        fail("invalid header name");
        return false;
    }

    const std::string name = ascii_lower(raw_name);
    const std::string value(trim_ows(line.substr(colon + 1)));

    auto [it, inserted] = request_.headers.emplace(name, value);
    if (!inserted) {
        if (name == "content-length" && it->second != value) {
            fail("conflicting Content-Length headers");
            return false;
        }
        if (name != "content-length") {
            it->second.append(", ");
            it->second.append(value);
        }
    }
    return true;
}

bool HttpRequestParser::parse_trailer_line(std::string_view line) {
    const auto colon = line.find(':');
    if (colon == std::string_view::npos) {
        fail("malformed trailer: missing colon");
        return false;
    }

    const auto raw_name = line.substr(0, colon);
    if (!valid_token(raw_name)) {
        fail("invalid trailer name");
        return false;
    }

    const std::string name = ascii_lower(raw_name);
    if (forbidden_trailer_name(name)) {
        fail("forbidden framing or routing field in trailer");
        return false;
    }

    const std::string value(trim_ows(line.substr(colon + 1)));
    auto [it, inserted] = request_.trailers.emplace(name, value);
    if (!inserted) {
        it->second.append(", ");
        it->second.append(value);
    }
    return true;
}

bool HttpRequestParser::parse_chunk_size_line(std::string_view line) {
    if (line.empty()) {
        fail("empty chunk-size line");
        return false;
    }

    const auto semicolon = line.find(';');
    const auto size_text = line.substr(0, semicolon);
    if (size_text.empty()) {
        fail("missing chunk size");
        return false;
    }

    std::size_t parsed = 0;
    for (const unsigned char ch : size_text) {
        const int digit = hex_digit(ch);
        if (digit < 0) {
            fail("invalid hexadecimal chunk size");
            return false;
        }
        const auto unsigned_digit = static_cast<std::size_t>(digit);
        if (parsed > (std::numeric_limits<std::size_t>::max() - unsigned_digit) / 16U) {
            fail("chunk size overflows size_t");
            return false;
        }
        parsed = parsed * 16U + unsigned_digit;
    }

    if (semicolon != std::string_view::npos) {
        const auto extension = line.substr(semicolon + 1);
        if (extension.empty()) {
            fail("empty chunk extension");
            return false;
        }
        // Extensions are intentionally not interpreted yet, but bounded visible
        // bytes are accepted so intermediaries can attach metadata safely.
        for (const unsigned char ch : extension) {
            if (ch < 0x20U || ch == 0x7FU) {
                fail("control character in chunk extension");
                return false;
            }
        }
    }

    if (parsed > limits_.max_body_bytes ||
        request_.body.size() > limits_.max_body_bytes - parsed) {
        fail("decoded chunked body exceeds configured limit");
        return false;
    }

    current_chunk_size_ = parsed;
    return true;
}

bool HttpRequestParser::finalize_headers() {
    const auto te = request_.headers.find("transfer-encoding");
    const auto cl = request_.headers.find("content-length");

    if (te != request_.headers.end() && cl != request_.headers.end()) {
        fail("Transfer-Encoding and Content-Length cannot be combined");
        return false;
    }

    if (te != request_.headers.end()) {
        if (request_.version != "HTTP/1.1") {
            fail("Transfer-Encoding is not supported for HTTP/1.0 requests");
            return false;
        }

        const auto coding = trim_ows(te->second);
        if (!ascii_iequals(coding, "chunked")) {
            fail("unsupported or invalid Transfer-Encoding; only a single final chunked coding is supported");
            return false;
        }
        chunked_ = true;
        return true;
    }

    if (cl == request_.headers.end()) {
        return true;
    }

    std::size_t parsed = 0;
    const auto begin = cl->second.data();
    const auto end = begin + cl->second.size();
    auto [ptr, ec] = std::from_chars(begin, end, parsed);
    if (ec != std::errc{} || ptr != end) {
        fail("invalid Content-Length");
        return false;
    }
    if (parsed > limits_.max_body_bytes) {
        fail("request body exceeds configured limit");
        return false;
    }
    content_length_ = parsed;
    return true;
}

void HttpRequestParser::fail(std::string message) {
    state_ = State::error;
    error_message_ = std::move(message);
}

std::string HttpRequestParser::take_remaining() {
    std::string out = std::move(buffer_);
    buffer_.clear();
    return out;
}

void HttpRequestParser::reset() {
    state_ = State::request_line;
    request_ = {};
    buffer_.clear();
    error_message_.clear();
    header_bytes_ = 0;
    header_count_ = 0;
    trailer_bytes_ = 0;
    trailer_count_ = 0;
    current_chunk_size_ = 0;
    content_length_.reset();
    chunked_ = false;
}

}  // namespace vhttp::http
