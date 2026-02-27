#ifndef LOCALEDGE_NET_HOSTINDEXER_CLIENT_HPP
#define LOCALEDGE_NET_HOSTINDEXER_CLIENT_HPP

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/websocket/stream.hpp>

#include "../common/status.hpp"
#include "../../../shared/crud_protocol.hpp"

namespace localedge::net {

struct HostIndexerClientOptions {
    std::string remote_host {"hostindexer"};
    std::uint16_t remote_port {9460};
    std::string websocket_path {"/hostindexer"};
    std::string host_id {};
    std::string client_instance_id {"localedge-1"};
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

class HostIndexerWebSocketClient final : public std::enable_shared_from_this<HostIndexerWebSocketClient> {
public:
    HostIndexerWebSocketClient(boost::asio::io_context& io_context,
                               HostIndexerClientOptions options,
                               CrudResultCallback on_result = {},
                               ConnectionStateCallback on_connection_state = {});

    void start();
    void stop();
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] bool connected() const noexcept;

    common::Status submit_command(backend::shared::crud::CrudCommandMessage command);
    common::Status request_read(std::uint64_t ack_sequence = 0U);

private:
    using Tcp = boost::asio::ip::tcp;
    using WebSocket = boost::beast::websocket::stream<boost::beast::tcp_stream>;

    void start_connect();
    void on_resolve(const boost::system::error_code& error, Tcp::resolver::results_type results);
    void on_connect(const boost::system::error_code& error,
                    const Tcp::resolver::results_type::endpoint_type& endpoint);
    void on_handshake(const boost::system::error_code& error);
    void do_read();
    void on_read(const boost::system::error_code& error, std::size_t bytes_transferred);
    void handle_message(std::string_view payload);

    void send_hello();
    void schedule_next_pull(bool had_records);
    void schedule_reconnect(std::string_view reason);
    std::chrono::milliseconds compute_reconnect_delay() const;
    void flush_pending_commands();
    void notify_connection_state(bool connected, std::string_view message);

    void enqueue_json(const nlohmann::json& payload);
    void enqueue_text(std::string payload);
    void do_write();
    void on_write(const boost::system::error_code& error, std::size_t bytes_transferred);

    std::string next_request_id(std::string_view prefix);
    void apply_crud_result(const backend::shared::crud::CrudResultMessage& result);
    void send_ingest_ack(std::uint64_t ack_sequence, std::string_view request_id);

    boost::asio::io_context& io_context_;
    Tcp::resolver resolver_;
    std::unique_ptr<WebSocket> websocket_ {};
    boost::beast::flat_buffer read_buffer_ {};
    boost::asio::steady_timer pull_timer_;
    boost::asio::steady_timer reconnect_timer_;
    HostIndexerClientOptions options_ {};
    CrudResultCallback on_result_ {};
    ConnectionStateCallback on_connection_state_ {};
    std::deque<std::string> write_queue_ {};
    std::deque<std::string> pending_after_hello_ {};
    std::atomic<std::uint64_t> request_sequence_ {0U};
    std::uint64_t last_ack_sequence_ {0U};
    std::uint64_t reconnect_events_total_ {0U};
    std::uint64_t reconnect_recoveries_total_ {0U};
    std::unordered_map<std::string, std::uint64_t> reconnect_reason_counts_ {};
    std::uint32_t reconnect_attempt_ {0U};
    mutable std::mt19937 rng_ {std::random_device{}()};
    bool running_ {false};
    bool handshake_complete_ {false};
    bool hello_complete_ {false};
};

} // namespace localedge::net

#endif // LOCALEDGE_NET_HOSTINDEXER_CLIENT_HPP
