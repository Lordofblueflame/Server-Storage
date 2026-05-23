#ifndef HUGIN_NET_MUNINN_CLIENT_HPP
#define HUGIN_NET_MUNINN_CLIENT_HPP

#include <atomic>
#include <deque>
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

#include "muninn_transport.hpp"

namespace hugin::net {
class MuninnWebSocketClient final
    : public MuninnTransportClient,
      public std::enable_shared_from_this<MuninnWebSocketClient> {
public:
    MuninnWebSocketClient(boost::asio::io_context& io_context,
                               MuninnClientOptions options,
                               CrudResultCallback on_result = {},
                               ConnectionStateCallback on_connection_state = {});

    void start() override;
    void stop() override;
    [[nodiscard]] bool running() const noexcept override;
    [[nodiscard]] bool connected() const noexcept override;

    common::Status submit_command(backend::shared::crud::CrudCommandMessage command) override;
    common::Status request_read(std::uint64_t ack_sequence = 0U) override;

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
    MuninnClientOptions options_ {};
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

} // namespace hugin::net

#endif // HUGIN_NET_MUNINN_CLIENT_HPP
