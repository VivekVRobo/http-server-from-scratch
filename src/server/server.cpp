#include "vhttp/server/server.hpp"

#include "vhttp/net/socket.hpp"
#include "vhttp/server/connection_session.hpp"

#include <array>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace vhttp::server {

Server::Server(Handler handler, ConnectionConfig config)
    : handler_(std::move(handler)), config_(config) {
    if (!handler_) {
        throw std::invalid_argument("Server requires a request handler");
    }
    if (config_.max_requests_per_connection == 0) {
        throw std::invalid_argument("max_requests_per_connection must be greater than zero");
    }
    if (config_.idle_timeout.count() <= 0) {
        throw std::invalid_argument("idle_timeout must be positive");
    }
}

void Server::listen_and_serve(std::string host, std::uint16_t port) {
    net::SocketRuntime runtime;
    auto listener = net::TcpListener::bind(host, port);

    std::cout << "vhttp listening on " << (host.empty() ? "0.0.0.0" : host) << ':' << port << '\n';

    while (true) {
        try {
            serve_connection(listener.accept());
        } catch (const std::exception& ex) {
            std::cerr << "connection error: " << ex.what() << '\n';
        }
    }
}

void Server::serve_connection(net::TcpStream stream) {
    stream.set_receive_timeout(config_.idle_timeout);

    ConnectionSession session(handler_, config_);
    std::array<char, 8192> buffer{};

    while (true) {
        std::ptrdiff_t received = 0;
        try {
            received = stream.receive(buffer.data(), buffer.size());
        } catch (const net::SocketTimeoutError&) {
            // An idle persistent connection is simply retired. No response is
            // written because a complete HTTP request was not available.
            return;
        }

        if (received == 0) {
            return;
        }

        auto update = session.feed(
            std::string_view(buffer.data(), static_cast<std::size_t>(received)));

        while (update.state == SessionState::response_ready) {
            stream.send_all(update.response);
            if (update.close_after_write) {
                return;
            }
            update = session.on_write_complete();
        }

        if (update.state == SessionState::closed) {
            return;
        }
    }
}

}  // namespace vhttp::server
