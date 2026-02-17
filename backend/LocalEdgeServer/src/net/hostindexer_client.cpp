#include "hostindexer_client.hpp"

#include <algorithm>
#include <iostream>
#include <sstream>
#include <utility>

#include <boost/asio/buffer.hpp>
#include <boost/asio/post.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>

namespace localedge::net {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace crud = backend::shared::crud;

void log_bridge(const std::string_view message) {
    std::cout << "[localedge.bridge] " << message << '\n';
}

} // namespace

HostIndexerWebSocketClient::HostIndexerWebSocketClient(boost::asio::io_context& io_context,
                                                       HostIndexerClientOptions options,
                                                       CrudResultCallback on_result,
                                                       ConnectionStateCallback on_connection_state)
    : io_context_(io_context),
      resolver_(io_context),
      pull_timer_(io_context),
      reconnect_timer_(io_context),
      options_(std::move(options)),
      on_result_(std::move(on_result)),
      on_connection_state_(std::move(on_connection_state)) {
}

void HostIndexerWebSocketClient::start() {
    if (running_) {
        return;
    }
    running_ = true;
    request_sequence_ = 0U;
    last_ack_sequence_ = 0U;
    {
        std::ostringstream text;
        text << "starting websocket bridge to " << options_.remote_host << ':' << options_.remote_port
             << options_.websocket_path
             << " host_id=" << options_.host_id
             << " auto_pull=" << (options_.auto_pull_enabled ? "true" : "false");
        log_bridge(text.str());
    }
    start_connect();
}

void HostIndexerWebSocketClient::stop() {
    running_ = false;
    handshake_complete_ = false;
    hello_complete_ = false;
    write_queue_.clear();
    pending_after_hello_.clear();

    boost::system::error_code ignored;
    pull_timer_.cancel(ignored);
    reconnect_timer_.cancel(ignored);
    resolver_.cancel();

    if (websocket_) {
        websocket_->close(websocket::close_code::normal, ignored);
        websocket_->next_layer().socket().shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
        websocket_->next_layer().socket().close(ignored);
        websocket_.reset();
    }

    log_bridge("bridge stopped");
}

bool HostIndexerWebSocketClient::running() const noexcept {
    return running_;
}

bool HostIndexerWebSocketClient::connected() const noexcept {
    return running_ && handshake_complete_ && hello_complete_;
}

common::Status HostIndexerWebSocketClient::submit_command(crud::CrudCommandMessage command) {
    if (!running_) {
        return common::Status::failure(common::ErrorCode::Internal, "HostIndexer client is not running.");
    }

    if (command.outbox_batch_size == 0U) {
        command.outbox_batch_size = static_cast<std::uint32_t>(std::max<std::size_t>(1U, options_.outbox_batch_size));
    }

    asio::post(io_context_, [self = shared_from_this(), command = std::move(command)]() mutable {
        if (command.request_id.empty()) {
            command.request_id = self->next_request_id("cmd");
        }
        if (command.operation != crud::Operation::Read) {
            std::ostringstream text;
            text << "enqueue command request_id=" << command.request_id
                 << " op=" << crud::operation_to_string(command.operation)
                 << " root_path=" << command.root_path;
            log_bridge(text.str());
        }
        const auto payload = crud::json_to_text(crud::to_json(command));
        if (!self->hello_complete_) {
            self->pending_after_hello_.push_back(payload);
            return;
        }
        self->enqueue_text(payload);
    });

    return common::Status::success();
}

common::Status HostIndexerWebSocketClient::request_read(const std::uint64_t ack_sequence) {
    crud::CrudCommandMessage command;
    command.request_id = next_request_id("read");
    command.operation = crud::Operation::Read;
    command.ack_sequence = ack_sequence;
    command.outbox_batch_size = static_cast<std::uint32_t>(std::max<std::size_t>(1U, options_.outbox_batch_size));
    return submit_command(std::move(command));
}

void HostIndexerWebSocketClient::start_connect() {
    if (!running_) {
        return;
    }

    handshake_complete_ = false;
    hello_complete_ = false;
    write_queue_.clear();
    read_buffer_.consume(read_buffer_.size());
    websocket_ = std::make_unique<WebSocket>(io_context_);
    log_bridge("resolving hostindexer endpoint");

    resolver_.async_resolve(
        options_.remote_host,
        std::to_string(options_.remote_port),
        beast::bind_front_handler(&HostIndexerWebSocketClient::on_resolve, shared_from_this())
    );
}

void HostIndexerWebSocketClient::on_resolve(const boost::system::error_code& error,
                                            Tcp::resolver::results_type results) {
    if (!running_) {
        return;
    }

    if (error) {
        std::ostringstream text;
        text << "resolve failed: " << error.message();
        log_bridge(text.str());
        schedule_reconnect("resolve_failed");
        return;
    }

    websocket_->next_layer().expires_after(std::chrono::seconds(15));
    websocket_->next_layer().async_connect(
        results,
        beast::bind_front_handler(&HostIndexerWebSocketClient::on_connect, shared_from_this())
    );
}

void HostIndexerWebSocketClient::on_connect(const boost::system::error_code& error,
                                            const Tcp::resolver::results_type::endpoint_type&) {
    if (!running_) {
        return;
    }

    if (error) {
        std::ostringstream text;
        text << "tcp connect failed: " << error.message();
        log_bridge(text.str());
        schedule_reconnect("connect_failed");
        return;
    }

    log_bridge("tcp connected, performing websocket handshake");

    websocket_->next_layer().expires_never();
    websocket_->set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
    websocket_->set_option(
        websocket::stream_base::decorator([](websocket::request_type& request) {
            request.set(boost::beast::http::field::user_agent, std::string("localedge-gateway"));
        })
    );

    const std::string host = options_.remote_host + ":" + std::to_string(options_.remote_port);
    websocket_->async_handshake(
        host,
        options_.websocket_path,
        beast::bind_front_handler(&HostIndexerWebSocketClient::on_handshake, shared_from_this())
    );
}

void HostIndexerWebSocketClient::on_handshake(const boost::system::error_code& error) {
    if (!running_) {
        return;
    }

    if (error) {
        std::ostringstream text;
        text << "websocket handshake failed: " << error.message();
        log_bridge(text.str());
        schedule_reconnect("handshake_failed");
        return;
    }

    handshake_complete_ = true;
    log_bridge("websocket handshake succeeded, sending hello");
    notify_connection_state(false, "socket_connected_waiting_hello");
    do_read();
    send_hello();
}

void HostIndexerWebSocketClient::do_read() {
    if (!running_ || !websocket_) {
        return;
    }

    websocket_->async_read(
        read_buffer_,
        beast::bind_front_handler(&HostIndexerWebSocketClient::on_read, shared_from_this())
    );
}

void HostIndexerWebSocketClient::on_read(const boost::system::error_code& error, const std::size_t) {
    if (!running_) {
        return;
    }

    if (error == websocket::error::closed) {
        log_bridge("websocket closed by remote");
        schedule_reconnect("remote_closed");
        return;
    }
    if (error) {
        std::ostringstream text;
        text << "websocket read failed: " << error.message();
        log_bridge(text.str());
        schedule_reconnect("read_failed");
        return;
    }

    const std::string payload = beast::buffers_to_string(read_buffer_.data());
    read_buffer_.consume(read_buffer_.size());
    handle_message(payload);
    do_read();
}

void HostIndexerWebSocketClient::handle_message(const std::string_view payload) {
    const auto parsed_json = crud::text_to_json(payload);
    if (!parsed_json.has_value() || !parsed_json->is_object()) {
        log_bridge("received malformed json frame from hostindexer");
        return;
    }

    const std::string type = parsed_json->value("type", "");
    if (type == "hello_ack") {
        const auto hello_ack = crud::parse_hello_ack(*parsed_json);
        if (!hello_ack.has_value() || !hello_ack->accepted) {
            log_bridge("hello rejected by hostindexer");
            schedule_reconnect("hello_rejected");
            return;
        }

        hello_complete_ = true;
        log_bridge("hello acknowledged by hostindexer");
        notify_connection_state(true, "connected");
        flush_pending_commands();
        if (options_.auto_pull_enabled) {
            schedule_next_pull(false);
        }
        return;
    }

    if (type == "crud_result") {
        const auto result = crud::parse_crud_result(*parsed_json);
        if (!result.has_value()) {
            log_bridge("failed to parse crud_result payload");
            return;
        }
        apply_crud_result(*result);
    }
}

void HostIndexerWebSocketClient::send_hello() {
    crud::HelloMessage hello;
    hello.host_id = options_.host_id;
    hello.client_instance_id = options_.client_instance_id;
    hello.auth_token = options_.auth_token;
    enqueue_json(crud::to_json(hello));
}

void HostIndexerWebSocketClient::schedule_next_pull(const bool had_records) {
    if (!running_ || !hello_complete_ || !options_.auto_pull_enabled) {
        return;
    }

    const auto delay = had_records ? options_.hot_pull_interval : options_.idle_pull_interval;
    boost::system::error_code ignored;
    pull_timer_.cancel(ignored);
    pull_timer_.expires_after(delay);
    pull_timer_.async_wait([self = shared_from_this()](const boost::system::error_code& error) {
        if (error || !self->running_ || !self->hello_complete_ || !self->options_.auto_pull_enabled) {
            return;
        }
        (void)self->request_read(self->last_ack_sequence_);
    });
}

void HostIndexerWebSocketClient::schedule_reconnect(const std::string_view reason) {
    if (!running_) {
        return;
    }

    handshake_complete_ = false;
    hello_complete_ = false;
    {
        std::ostringstream text;
        text << "scheduling reconnect reason=" << reason
             << " delay_ms=" << options_.reconnect_delay.count();
        log_bridge(text.str());
    }
    notify_connection_state(false, reason);

    boost::system::error_code ignored;
    pull_timer_.cancel(ignored);

    if (websocket_) {
        websocket_->close(websocket::close_code::normal, ignored);
        websocket_->next_layer().socket().shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
        websocket_->next_layer().socket().close(ignored);
        websocket_.reset();
    }

    reconnect_timer_.cancel(ignored);
    reconnect_timer_.expires_after(options_.reconnect_delay);
    reconnect_timer_.async_wait([self = shared_from_this()](const boost::system::error_code& error) {
        if (error || !self->running_) {
            return;
        }
        self->start_connect();
    });
}

void HostIndexerWebSocketClient::flush_pending_commands() {
    if (!pending_after_hello_.empty()) {
        std::ostringstream text;
        text << "flushing " << pending_after_hello_.size() << " queued command(s)";
        log_bridge(text.str());
    }
    while (!pending_after_hello_.empty()) {
        enqueue_text(std::move(pending_after_hello_.front()));
        pending_after_hello_.pop_front();
    }
}

void HostIndexerWebSocketClient::notify_connection_state(const bool connected_state, const std::string_view message) {
    if (!on_connection_state_) {
        return;
    }
    on_connection_state_(connected_state, message);
}

void HostIndexerWebSocketClient::enqueue_json(const nlohmann::json& payload) {
    enqueue_text(crud::json_to_text(payload));
}

void HostIndexerWebSocketClient::enqueue_text(std::string payload) {
    const bool idle = write_queue_.empty();
    write_queue_.push_back(std::move(payload));
    if (idle) {
        do_write();
    }
}

void HostIndexerWebSocketClient::do_write() {
    if (!running_ || !handshake_complete_ || !websocket_ || write_queue_.empty()) {
        return;
    }

    websocket_->text(true);
    websocket_->async_write(
        asio::buffer(write_queue_.front()),
        beast::bind_front_handler(&HostIndexerWebSocketClient::on_write, shared_from_this())
    );
}

void HostIndexerWebSocketClient::on_write(const boost::system::error_code& error, const std::size_t) {
    if (!running_) {
        return;
    }

    if (error) {
        std::ostringstream text;
        text << "websocket write failed: " << error.message();
        log_bridge(text.str());
        schedule_reconnect("write_failed");
        return;
    }

    write_queue_.pop_front();
    if (!write_queue_.empty()) {
        do_write();
    }
}

std::string HostIndexerWebSocketClient::next_request_id(const std::string_view prefix) {
    const std::uint64_t value = request_sequence_.fetch_add(1U) + 1U;
    return std::string(prefix) + "-" + std::to_string(value);
}

void HostIndexerWebSocketClient::apply_crud_result(const crud::CrudResultMessage& result) {
    if (on_result_) {
        on_result_(result);
    }

    if (!result.ok || !result.records.empty()) {
        std::ostringstream text;
        text << "crud_result request_id=" << result.request_id
             << " ok=" << (result.ok ? "true" : "false")
             << " records=" << result.records.size();
        if (!result.message.empty()) {
            text << " message=" << result.message;
        }
        log_bridge(text.str());
    }

    std::uint64_t max_sequence = last_ack_sequence_;
    for (const auto& record : result.records) {
        max_sequence = std::max(max_sequence, record.sequence);
    }

    if (max_sequence > last_ack_sequence_) {
        send_ingest_ack(max_sequence, result.request_id);
        last_ack_sequence_ = max_sequence;
    }

    schedule_next_pull(!result.records.empty());
}

void HostIndexerWebSocketClient::send_ingest_ack(const std::uint64_t ack_sequence, const std::string_view request_id) {
    crud::IngestAckMessage ack;
    ack.request_id = std::string(request_id);
    ack.ok = true;
    ack.ack_sequence = ack_sequence;
    ack.message = "ok";
    enqueue_json(crud::to_json(ack));
}

} // namespace localedge::net
