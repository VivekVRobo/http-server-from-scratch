#pragma once

#include "vhttp/http/parser.hpp"
#include "vhttp/server/server.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace vhttp::server {

enum class SessionState {
    need_more_data,
    response_ready,
    closed,
};

struct SessionUpdate {
    SessionState state = SessionState::need_more_data;
    std::string response;
    bool close_after_write = false;
};

// Transport-agnostic HTTP/1.x connection lifecycle.
//
// The session owns parser/persistence/request-count state but performs no socket
// I/O. Runtimes feed received bytes, flush at most one returned response, then
// call on_write_complete(). Delaying pipelined request dispatch until the prior
// response has been fully written keeps per-connection output bounded to one
// serialized response and preserves response ordering for future event loops.
class ConnectionSession {
public:
    explicit ConnectionSession(Handler handler, ConnectionConfig config = {});

    // Feed newly received bytes while the session is waiting for input.
    // Returns either need_more_data or one response_ready update.
    SessionUpdate feed(std::string_view bytes);

    // Advance after the runtime has fully written the response returned by the
    // previous response_ready update. This may immediately produce the next
    // pipelined response without requiring another socket read.
    SessionUpdate on_write_complete();

    [[nodiscard]] SessionState state() const noexcept;
    [[nodiscard]] std::size_t requests_served() const noexcept { return requests_served_; }

private:
    enum class Phase {
        reading,
        response_pending,
        closed,
    };

    SessionUpdate evaluate(http::ParseStatus status);

    Handler handler_;
    ConnectionConfig config_;
    http::HttpRequestParser parser_;
    std::size_t requests_served_ = 0;
    std::string pipelined_input_;
    Phase phase_ = Phase::reading;
    bool close_after_write_ = false;
};

}  // namespace vhttp::server
