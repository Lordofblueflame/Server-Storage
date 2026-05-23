#ifndef HUGIN_NET_MUNINN_TRANSPORT_HPP
#define HUGIN_NET_MUNINN_TRANSPORT_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <boost/asio/io_context.hpp>

#include "../common/status.hpp"
#include "shared/crud_protocol.hpp"

namespace hugin::net {

enum class MuninnTransportKind {
    WebSocket
};

inline constexpr std::string_view muninn_transport_kind_name(const MuninnTransportKind kind) noexcept {
    switch (kind) {
        case MuninnTransportKind::WebSocket:
            return "websocket";
    }
    return "websocket";
}

struct MuninnClientOptions {
    MuninnTransportKind transport_kind {MuninnTransportKind::WebSocket};
    std::string remote_host {"muninn"};
    std::uint16_t remote_port {9460};
    std::string websocket_path {"/muninn"};
    std::string host_id {};
    std::string client_instance_id {"hugin-1"};
    std::string auth_token {};

    bool auto_pull_enabled {true};
    std::size_t outbox_batch_size {256U};
    std::chrono::milliseconds idle_pull_interval {250};
    std::chrono::milliseconds hot_pull_interval {10};

    std::chrono::milliseconds reconnect_delay {1'500};
    std::chrono::milliseconds reconnect_max_delay {30'000};
    std::uint32_t reconnect_backoff_factor {2U};
    std::uint32_t reconnect_jitter_percent {20U};
};

using CrudResultCallback = std::function<void(const backend::shared::crud::CrudResultMessage&)>;
using ConnectionStateCallback = std::function<void(bool connected, std::string_view message)>;

class MuninnTransportClient {
public:
    virtual ~MuninnTransportClient() = default;

    virtual void start() = 0;
    virtual void stop() = 0;
    [[nodiscard]] virtual bool running() const noexcept = 0;
    [[nodiscard]] virtual bool connected() const noexcept = 0;

    virtual common::Status submit_command(backend::shared::crud::CrudCommandMessage command) = 0;
    virtual common::Status request_read(std::uint64_t ack_sequence = 0U) = 0;
};

std::shared_ptr<MuninnTransportClient> create_muninn_transport_client(
    boost::asio::io_context& io_context,
    MuninnClientOptions options,
    CrudResultCallback on_result = {},
    ConnectionStateCallback on_connection_state = {}
);

} // namespace hugin::net

#endif // HUGIN_NET_MUNINN_TRANSPORT_HPP
