#ifndef HUGIN_NET_WS_SERVER_HPP
#define HUGIN_NET_WS_SERVER_HPP

#include <functional>
#include <string>
#include <variant>

#include "../common/status.hpp"
#include "../protocol/wire.hpp"

namespace hugin::net {

struct WebSocketServerOptions {
    std::string listen_address {"0.0.0.0"};
    unsigned short port {9440};
    std::string ingest_path {"/ingest"};
};

using HelloHandler = std::function<common::StatusOr<protocol::ServerHello>(const protocol::ClientHello&)>;
using DataHandler =
    std::function<common::StatusOr<std::variant<protocol::AckMessage, protocol::NackMessage, protocol::ErrorMessage>>(
        const protocol::DataMessage&)>;

class WebSocketIngressServer final {
public:
    WebSocketIngressServer(WebSocketServerOptions options, HelloHandler hello_handler, DataHandler data_handler);

    common::Status start();
    void stop();
    [[nodiscard]] bool running() const noexcept;

private:
    WebSocketServerOptions options_ {};
    HelloHandler hello_handler_ {};
    DataHandler data_handler_ {};
    bool running_ {false};
};

} // namespace hugin::net

#endif // HUGIN_NET_WS_SERVER_HPP
