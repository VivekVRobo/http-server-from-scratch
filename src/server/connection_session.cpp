#include "vhttp/server/connection_session.hpp"

#include "vhttp/server/connection.hpp"

#include <stdexcept>
#include <utility>

namespace vhttp::server {

ConnectionSession::ConnectionSession(Handler handler, ConnectionConfig config)
    : handler_(std::move(handler)), config_(config) {
    if (!handler_) {
        throw std::invalid_argument("ConnectionSession requires a request handler");
    }
    if (config_.max_requests_per_connection == 0) {
        throw std::invalid_argument("max_requests_per_connection must be greater than zero");
    }
    if (config_.idle_timeout.count() <= 0) {
        throw std::invalid_argument("idle_timeout must be positive");
    }
}

SessionUpdate ConnectionSession::feed(std::string_view bytes) {
    if (phase_ == Phase::closed) {
        return {SessionState::closed, {}, false};
    }
    if (phase_ != Phase::reading) {
        throw std::logic_error("cannot feed ConnectionSession while a response is pending");
    }
    if (bytes.empty()) {
        return {SessionState::need_more_data, {}, false};
    }
    return evaluate(parser_.feed(bytes));
}

SessionUpdate ConnectionSession::on_write_complete() {
    if (phase_ == Phase::closed) {
        return {SessionState::closed, {}, false};
    }
    if (phase_ != Phase::response_pending) {
        throw std::logic_error("on_write_complete requires a pending response");
    }

    if (close_after_write_) {
        pipelined_input_.clear();
        phase_ = Phase::closed;
        return {SessionState::closed, {}, false};
    }

    parser_.reset();
    phase_ = Phase::reading;
    close_after_write_ = false;

    if (pipelined_input_.empty()) {
        return {SessionState::need_more_data, {}, false};
    }

    std::string pending = std::move(pipelined_input_);
    pipelined_input_.clear();
    return evaluate(parser_.feed(pending));
}

SessionState ConnectionSession::state() const noexcept {
    switch (phase_) {
        case Phase::reading:
            return SessionState::need_more_data;
        case Phase::response_pending:
            return SessionState::response_ready;
        case Phase::closed:
            return SessionState::closed;
    }
    return SessionState::closed;
}

SessionUpdate ConnectionSession::evaluate(http::ParseStatus status) {
    if (status == http::ParseStatus::need_more_data) {
        return {SessionState::need_more_data, {}, false};
    }

    if (status == http::ParseStatus::error) {
        auto response = http::HttpResponse::text(400, "Bad Request", "Bad Request\n");
        phase_ = Phase::response_pending;
        close_after_write_ = true;
        pipelined_input_.clear();
        return {SessionState::response_ready, response.serialize(false), true};
    }

    const auto& request = parser_.request();
    bool keep_alive = request_allows_persistent_connection(request);
    auto response = handler_(request);

    if (response_requests_connection_close(response)) {
        keep_alive = false;
    }

    ++requests_served_;
    if (requests_served_ >= config_.max_requests_per_connection) {
        keep_alive = false;
    }

    std::string wire = response.serialize(keep_alive, request.method == "HEAD");
    if (keep_alive) {
        pipelined_input_ = parser_.take_remaining();
    } else {
        pipelined_input_.clear();
    }

    phase_ = Phase::response_pending;
    close_after_write_ = !keep_alive;
    return {SessionState::response_ready, std::move(wire), close_after_write_};
}

}  // namespace vhttp::server
