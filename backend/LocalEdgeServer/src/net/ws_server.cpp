#include "ws_server.hpp"

namespace localedge::net {

WebSocketIngressServer::WebSocketIngressServer(WebSocketServerOptions options,
                                               HelloHandler hello_handler,
                                               DataHandler data_handler)
    : options_(std::move(options)),
      hello_handler_(std::move(hello_handler)),
      data_handler_(std::move(data_handler)) {
}

common::Status WebSocketIngressServer::start() {
    if (running_) {
        return common::Status::success();
    }

    if (!hello_handler_ || !data_handler_) {
        return common::Status::failure(common::ErrorCode::InvalidArgument, "WebSocket handlers are not configured.");
    }

    // Networking is intentionally left as an integration seam for Beast/uWebSockets.
    // The ingest/persistence/validation pipeline is fully implemented independently.
    running_ = true;
    (void)options_;
    return common::Status::success();
}

void WebSocketIngressServer::stop() {
    running_ = false;
}

bool WebSocketIngressServer::running() const noexcept {
    return running_;
}

} // namespace localedge::net
