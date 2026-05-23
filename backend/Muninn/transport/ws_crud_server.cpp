#include "ws_crud_server.hpp"

#include <algorithm>
#include <atomic>
#include <deque>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio/buffer.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/beast/websocket.hpp>

#include "../../shared/crud_protocol.hpp"
#include "../../shared/service_log.hpp"

namespace muninn::transport {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
namespace crud = backend::shared::crud;
namespace logging = backend::shared::logging;

const logging::Logger& ws_logger() {
    static const logging::Logger logger("muninn.ws");
    return logger;
}

void log_ws(const std::string_view category, const std::string_view message) {
    ws_logger().info(category, message);
}

void log_ws_warning(const std::string_view category, const std::string_view message) {
    ws_logger().warn(category, message);
}

void log_ws_error(const std::string_view category, const std::string_view message) {
    ws_logger().error(category, message);
}

const char* command_name(const crud::Operation operation) {
    switch (operation) {
        case crud::Operation::Create:
            return "create";
        case crud::Operation::Read:
            return "read";
        case crud::Operation::Update:
            return "update";
        case crud::Operation::Delete:
            return "delete";
    }
    return "read";
}

const char* security_mode_name(const TransportSecurityMode mode) {
    switch (mode) {
        case TransportSecurityMode::Dev:
            return "dev";
        case TransportSecurityMode::Prod:
            return "prod";
    }
    return "dev";
}

bool client_instance_allowed(const std::string& configured_pattern, const std::string& client_instance_id) {
    if (configured_pattern.empty()) {
        return true;
    }
    if (configured_pattern.back() == '*') {
        const std::string_view prefix(configured_pattern.data(), configured_pattern.size() - 1U);
        return client_instance_id.size() >= prefix.size() &&
               std::string_view(client_instance_id.data(), prefix.size()) == prefix;
    }
    return client_instance_id == configured_pattern;
}

std::optional<std::string> validate_bridge_auth_token(const crud::HelloMessage& hello,
                                                      const WebsocketCrudServerOptions& options) {
    if (options.security_mode == TransportSecurityMode::Prod) {
        if (options.bridge_auth_token.empty()) {
            return std::string("server bridge auth token is not configured.");
        }
        if (hello.auth_token.empty()) {
            return std::string("bridge auth token is required in production mode.");
        }
    }

    if (!options.bridge_auth_token.empty() && hello.auth_token != options.bridge_auth_token) {
        return std::string("bridge auth token mismatch.");
    }

    return std::nullopt;
}

std::atomic<std::uint64_t> g_session_counter {0U};

storage::StoreStatus io_failure(const std::string_view context, const boost::system::error_code& error) {
    std::ostringstream text;
    text << context << " failed: " << error.message() << " (code=" << error.value() << ")";
    return storage::StoreStatus::failure(storage::StoreErrorCode::IoFailure, text.str());
}

crud::CrudResultMessage make_error_result(std::string request_id, std::string message) {
    crud::CrudResultMessage result;
    result.request_id = std::move(request_id);
    result.ok = false;
    result.message = std::move(message);
    return result;
}

beast::string_view request_path_from_target(const beast::string_view target) {
    const auto query_pos = target.find('?');
    return target.substr(0, query_pos);
}

class CrudWebsocketSession final : public std::enable_shared_from_this<CrudWebsocketSession> {
public:
    CrudWebsocketSession(tcp::socket socket,
                         const WebsocketCrudServerOptions& options,
                         storage::LmdbStore& store,
                         const CrudCommandProcessor& command_processor)
        : websocket_(std::move(socket)),
          session_id_(g_session_counter.fetch_add(1U) + 1U),
          options_(options),
          store_(store),
          command_processor_(command_processor) {
        boost::system::error_code error;
        const auto endpoint = websocket_.next_layer().remote_endpoint(error);
        if (!error) {
            peer_endpoint_ = endpoint.address().to_string() + ":" + std::to_string(endpoint.port());
        } else {
            peer_endpoint_ = "unknown";
        }

    }

    void run() {
        websocket_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
        websocket_.set_option(
            websocket::stream_base::decorator(
                [](websocket::response_type& response) {
                    response.set(http::field::server, std::string("host-indexer"));
                }
            )
        );
        http::async_read(
            websocket_.next_layer(),
            handshake_buffer_,
            handshake_request_,
            beast::bind_front_handler(&CrudWebsocketSession::on_upgrade_request, shared_from_this())
        );
    }

private:
    void send_http_response(const http::status status,
                            std::string content_type,
                            std::string body,
                            const bool keep_alive = false) {
        auto response = std::make_shared<http::response<http::string_body>>(status, handshake_request_.version());
        response->set(http::field::server, std::string("host-indexer"));
        response->set(http::field::content_type, std::move(content_type));
        response->keep_alive(keep_alive);
        response->body() = std::move(body);
        response->prepare_payload();

        http::async_write(
            websocket_.next_layer(),
            *response,
            [self = shared_from_this(), response](const beast::error_code& write_error, const std::size_t) {
                if (write_error) {
                    std::ostringstream text;
                    text << "session=" << self->session_id_
                         << " upgrade reject write failed: " << write_error.message();
                    log_ws_error("session", text.str());
                }

                boost::system::error_code ignored;
                self->websocket_.next_layer().shutdown(tcp::socket::shutdown_both, ignored);
                self->websocket_.next_layer().close(ignored);
            }
        );
    }

    void reject_upgrade(const http::status status, std::string message) {
        send_http_response(status, "text/plain; charset=utf-8", std::move(message), false);
    }

    bool handle_operational_http_request() {
        if (handshake_request_.method() != http::verb::get) {
            return false;
        }

        const auto requested_path = request_path_from_target(handshake_request_.target());
        const auto path_matches = [&](const std::string& configured_path) {
            if (configured_path.empty()) {
                return false;
            }
            const beast::string_view configured_view(configured_path.data(), configured_path.size());
            return requested_path == configured_view;
        };

        if (path_matches(options_.health_path)) {
            nlohmann::json payload {
                {"service", "muninn"},
                {"status", "ok"},
                {"mode", "live"}
            };
            send_http_response(
                http::status::ok,
                "application/json; charset=utf-8",
                crud::json_to_text(payload),
                false
            );
            return true;
        }

        if (path_matches(options_.readiness_path)) {
            storage::StoreStats stats;
            const auto stats_status = store_.get_stats(stats);
            if (!stats_status.ok()) {
                nlohmann::json payload {
                    {"service", "muninn"},
                    {"ready", false},
                    {"error", stats_status.message}
                };
                send_http_response(
                    http::status::service_unavailable,
                    "application/json; charset=utf-8",
                    crud::json_to_text(payload),
                    false
                );
                return true;
            }

            nlohmann::json payload {
                {"service", "muninn"},
                {"ready", true},
                {"snapshots_count", stats.snapshots_count},
                {"deltas_count", stats.deltas_count},
                {"transport_outbox_count", stats.transport_outbox_count},
                {"transport_consumers_count", stats.transport_consumers_count},
                {"next_transport_sequence", stats.next_transport_sequence}
            };
            send_http_response(
                http::status::ok,
                "application/json; charset=utf-8",
                crud::json_to_text(payload),
                false
            );
            return true;
        }

        if (path_matches(options_.metrics_path)) {
            storage::StoreStats stats;
            const auto stats_status = store_.get_stats(stats);
            if (!stats_status.ok()) {
                std::ostringstream body;
                body << "# muninn metrics unavailable\n";
                body << "muninn_metrics_up 0\n";
                send_http_response(
                    http::status::service_unavailable,
                    "text/plain; version=0.0.4; charset=utf-8",
                    body.str(),
                    false
                );
                return true;
            }

            std::ostringstream body;
            body << "# HELP muninn_metrics_up Whether Muninn metrics are available.\n";
            body << "# TYPE muninn_metrics_up gauge\n";
            body << "muninn_metrics_up 1\n";
            body << "# TYPE muninn_snapshots_count gauge\n";
            body << "muninn_snapshots_count " << stats.snapshots_count << '\n';
            body << "# TYPE muninn_deltas_count gauge\n";
            body << "muninn_deltas_count " << stats.deltas_count << '\n';
            body << "# TYPE muninn_transport_outbox_count gauge\n";
            body << "muninn_transport_outbox_count " << stats.transport_outbox_count << '\n';
            body << "# TYPE muninn_transport_consumers_count gauge\n";
            body << "muninn_transport_consumers_count " << stats.transport_consumers_count << '\n';
            body << "# TYPE muninn_next_transport_sequence gauge\n";
            body << "muninn_next_transport_sequence " << stats.next_transport_sequence << '\n';
            send_http_response(
                http::status::ok,
                "text/plain; version=0.0.4; charset=utf-8",
                body.str(),
                false
            );
            return true;
        }

        return false;
    }

    void on_upgrade_request(const beast::error_code& error, const std::size_t) {
        if (error) {
            std::ostringstream text;
            text << "session=" << session_id_ << " upgrade request read failed: " << error.message();
            log_ws_error("session", text.str());
            return;
        }

        if (!websocket::is_upgrade(handshake_request_)) {
            if (handle_operational_http_request()) {
                return;
            }
            std::ostringstream text;
            text << "session=" << session_id_ << " rejected non-websocket upgrade request";
            log_ws_warning("protocol", text.str());
            reject_upgrade(http::status::bad_request, "WebSocket upgrade is required.");
            return;
        }

        const auto requested_path = request_path_from_target(handshake_request_.target());
        const beast::string_view expected_path(options_.websocket_path.data(), options_.websocket_path.size());
        if (!options_.websocket_path.empty() && requested_path != expected_path) {
            std::ostringstream text;
            text << "session=" << session_id_ << " rejected websocket path=" << std::string(requested_path)
                 << " expected=" << options_.websocket_path;
            log_ws_warning("auth", text.str());
            reject_upgrade(http::status::not_found, "WebSocket path not found.");
            return;
        }

        websocket_.async_accept(
            handshake_request_,
            beast::bind_front_handler(&CrudWebsocketSession::on_accept, shared_from_this())
        );
    }

    void on_accept(const beast::error_code& error) {
        if (error) {
            std::ostringstream text;
            text << "session=" << session_id_ << " accept failed: " << error.message();
            log_ws_error("session", text.str());
            return;
        }
        do_read();
    }

    void do_read() {
        websocket_.async_read(
            read_buffer_,
            beast::bind_front_handler(&CrudWebsocketSession::on_read, shared_from_this())
        );
    }

    void on_read(const beast::error_code& error, const std::size_t) {
        if (error == websocket::error::closed) {
            return;
        }
        if (error) {
            std::ostringstream text;
            text << "session=" << session_id_ << " read failed: " << error.message();
            log_ws_warning("session", text.str());
            return;
        }

        const std::string payload = beast::buffers_to_string(read_buffer_.data());
        read_buffer_.consume(read_buffer_.size());
        handle_message(payload);

        do_read();
    }

    void handle_message(const std::string_view payload) {
        const auto parsed_json = crud::text_to_json(payload);
        if (!parsed_json.has_value() || !parsed_json->is_object()) {
            std::ostringstream text;
            text << "session=" << session_id_ << " received malformed json message";
            log_ws_warning("protocol", text.str());
            enqueue_json(crud::to_json(make_error_result("", "Malformed JSON message.")));
            return;
        }

        const std::string type = parsed_json->value("type", "");
        if (type == "hello") {
            handle_hello(*parsed_json);
            return;
        }

        if (type == "ingest_ack") {
            if (!hello_accepted_) {
                std::ostringstream text;
                text << "session=" << session_id_ << " received ingest_ack before hello completed";
                log_ws_warning("protocol", text.str());
                enqueue_json(
                    crud::to_json(
                        make_error_result(
                            parsed_json->value("request_id", ""),
                            "hello must be completed before ingest_ack."
                        )
                    )
                );
                return;
            }
            handle_ingest_ack(*parsed_json);
            return;
        }

        if (type == "crud_command") {
            if (!hello_accepted_) {
                std::ostringstream text;
                text << "session=" << session_id_ << " received crud_command before hello completed";
                log_ws_warning("protocol", text.str());
                enqueue_json(
                    crud::to_json(
                        make_error_result(
                            parsed_json->value("request_id", ""),
                            "hello must be completed before crud_command."
                        )
                    )
                );
                return;
            }
            handle_command(*parsed_json);
            return;
        }

        {
            std::ostringstream text;
            text << "session=" << session_id_ << " received unsupported message type=" << type;
            log_ws_warning("protocol", text.str());
        }
        enqueue_json(crud::to_json(make_error_result("", "Unsupported message type.")));
    }

    void handle_hello(const nlohmann::json& payload) {
        crud::HelloAckMessage ack;
        const auto hello = crud::parse_hello(payload);
        if (!hello.has_value()) {
            ack.accepted = false;
            ack.reason = "Invalid hello payload.";
            std::ostringstream text;
            text << "session=" << session_id_ << " invalid hello payload";
            log_ws_warning("protocol", text.str());
            enqueue_json(crud::to_json(ack));
            return;
        }

        if (hello->client_instance_id.empty()) {
            ack.accepted = false;
            ack.reason = "client_instance_id is required.";
            std::ostringstream text;
            text << "session=" << session_id_ << " hello rejected: missing client_instance_id";
            log_ws_warning("auth", text.str());
            enqueue_json(crud::to_json(ack));
            return;
        }

        if (!client_instance_allowed(options_.allowed_client_instance_id, hello->client_instance_id)) {
            ack.accepted = false;
            ack.reason = "client_instance_id is not allowed.";
            std::ostringstream text;
            text << "session=" << session_id_ << " hello rejected: client_instance_id="
                 << hello->client_instance_id << " is not allowed";
            log_ws_warning("auth", text.str());
            enqueue_json(crud::to_json(ack));
            return;
        }

        if (const auto auth_error = validate_bridge_auth_token(*hello, options_); auth_error.has_value()) {
            ack.accepted = false;
            ack.reason = *auth_error;
            std::ostringstream text;
            text << "session=" << session_id_
                 << " hello rejected: bridge auth token validation failed reason=" << *auth_error;
            log_ws_warning("auth", text.str());
            enqueue_json(crud::to_json(ack));
            return;
        }

        const std::string consumer_id = hello->client_instance_id + ":" + hello->host_id;
        const auto register_status = store_.register_transport_consumer(consumer_id);
        if (!register_status.ok()) {
            ack.accepted = false;
            ack.reason = "failed to initialize transport consumer state.";
            std::ostringstream text;
            text << "session=" << session_id_ << " hello rejected: failed to register consumer_id="
                 << consumer_id << " message=" << register_status.message;
            log_ws_error("store", text.str());
            enqueue_json(crud::to_json(ack));
            return;
        }

        hello_accepted_ = true;
        peer_host_id_ = hello->host_id;
        peer_client_instance_id_ = hello->client_instance_id;
        consumer_id_ = consumer_id;
        ack.accepted = true;
        ack.reason.clear();
        enqueue_json(crud::to_json(ack));
    }

    void handle_ingest_ack(const nlohmann::json& payload) {
        const auto ingest_ack = crud::parse_ingest_ack(payload);
        if (!ingest_ack.has_value()) {
            std::ostringstream text;
            text << "session=" << session_id_ << " invalid ingest_ack payload";
            log_ws_warning("protocol", text.str());
            enqueue_json(crud::to_json(make_error_result("", "Invalid ingest_ack payload.")));
            return;
        }

        if (!ingest_ack->ok || ingest_ack->ack_sequence == 0U) {
            return;
        }

        const auto ack_status = store_.ack_transport_until_for_consumer(consumer_id_, ingest_ack->ack_sequence);
        if (!ack_status.ok()) {
            enqueue_json(crud::to_json(make_error_result(ingest_ack->request_id, ack_status.message)));
            std::ostringstream text;
            text << "session=" << session_id_
                 << " consumer_id=" << consumer_id_
                 << " ack_transport_until_for_consumer failed request_id=" << ingest_ack->request_id
                 << " ack_sequence=" << ingest_ack->ack_sequence
                 << " message=" << ack_status.message;
            log_ws("store", text.str());
        }
    }

    void handle_command(const nlohmann::json& payload) {
        const auto command = crud::parse_crud_command(payload);
        if (!command.has_value()) {
            std::ostringstream text;
            text << "session=" << session_id_ << " invalid crud_command payload";
            log_ws_warning("protocol", text.str());
            enqueue_json(crud::to_json(make_error_result("", "Invalid crud_command payload.")));
            return;
        }

        const auto result = command_processor_.process(*command, consumer_id_);
        if (command->operation != crud::Operation::Read || !result.ok) {
            std::ostringstream text;
            text << "session=" << session_id_
                 << " request_id=" << command->request_id
                 << " op=" << command_name(command->operation)
                 << " ok=" << (result.ok ? "true" : "false")
                 << " records=" << result.records.size();
            if (!result.message.empty()) {
                text << " message=" << result.message;
            }
            log_ws("command", text.str());
        }
        enqueue_json(crud::to_json(result));
    }

    void enqueue_json(const nlohmann::json& payload) {
        enqueue_text(crud::json_to_text(payload));
    }

    void enqueue_text(std::string payload) {
        const bool idle = write_queue_.empty();
        write_queue_.push_back(std::move(payload));
        if (idle) {
            do_write();
        }
    }

    void do_write() {
        if (write_queue_.empty()) {
            return;
        }

        websocket_.text(true);
        websocket_.async_write(
            asio::buffer(write_queue_.front()),
            beast::bind_front_handler(&CrudWebsocketSession::on_write, shared_from_this())
        );
    }

    void on_write(const beast::error_code& error, const std::size_t) {
        if (error) {
            std::ostringstream text;
            text << "session=" << session_id_ << " write failed: " << error.message();
            log_ws_warning("session", text.str());
            return;
        }
        write_queue_.pop_front();
        if (!write_queue_.empty()) {
            do_write();
        }
    }

    websocket::stream<tcp::socket> websocket_;
    std::uint64_t session_id_ {0U};
    std::string peer_endpoint_ {};
    beast::flat_buffer handshake_buffer_ {};
    http::request<http::string_body> handshake_request_ {};
    beast::flat_buffer read_buffer_ {};
    std::deque<std::string> write_queue_ {};
    WebsocketCrudServerOptions options_ {};
    storage::LmdbStore& store_;
    const CrudCommandProcessor& command_processor_;
    bool hello_accepted_ {false};
    std::string peer_host_id_ {};
    std::string peer_client_instance_id_ {};
    std::string consumer_id_ {};
};

} // namespace

WebsocketCrudServer::WebsocketCrudServer(boost::asio::io_context& io_context,
                                         storage::LmdbStore& store,
                                         api::IndexingPipeline& pipeline,
                                         WebsocketCrudServerOptions options)
    : io_context_(io_context),
      acceptor_(io_context),
      store_(&store),
      options_(std::move(options)),
      command_processor_(options_, store, pipeline, pipeline_mutex_) {
}

storage::StoreStatus WebsocketCrudServer::start() {
    if (running_) {
        return storage::StoreStatus::success();
    }
    if (options_.security_mode == TransportSecurityMode::Prod && options_.bridge_auth_token.empty()) {
        return storage::StoreStatus::failure(
            storage::StoreErrorCode::InvalidArgument,
            "bridge_auth_token is required when security mode is prod."
        );
    }
    if (options_.security_mode == TransportSecurityMode::Prod && !options_.allow_plain_websocket_in_prod) {
        return storage::StoreStatus::failure(
            storage::StoreErrorCode::InvalidArgument,
            "allow_plain_websocket_in_prod must be explicitly enabled in prod mode when TLS is terminated externally."
        );
    }

    boost::system::error_code error;
    const auto address = asio::ip::make_address(options_.listen_address, error);
    if (error) {
        return io_failure("make_address", error);
    }

    const tcp::endpoint endpoint(address, options_.port);
    acceptor_.open(endpoint.protocol(), error);
    if (error) {
        return io_failure("acceptor.open", error);
    }

    acceptor_.set_option(asio::socket_base::reuse_address(true), error);
    if (error) {
        return io_failure("acceptor.set_option", error);
    }

    acceptor_.bind(endpoint, error);
    if (error) {
        return io_failure("acceptor.bind", error);
    }

    acceptor_.listen(asio::socket_base::max_listen_connections, error);
    if (error) {
        return io_failure("acceptor.listen", error);
    }

    {
        std::ostringstream text;
        text << "listening on " << options_.listen_address << ':' << options_.port
             << " path=" << options_.websocket_path
             << " health_path=" << options_.health_path
             << " readiness_path=" << options_.readiness_path
             << " metrics_path=" << options_.metrics_path
             << " default_outbox_batch=" << options_.default_outbox_batch_size
             << " scan_follow_symlinks=" << (options_.scan_options.follow_symlinks ? "true" : "false")
             << " scan_max_entries=" << options_.scan_options.max_entries
             << " scan_include_globs=" << options_.scan_options.include_globs.size()
             << " scan_exclude_globs=" << options_.scan_options.exclude_globs.size()
             << " retention_keep_latest_snapshots=" << options_.retention_keep_latest_snapshots
             << " security_mode=" << security_mode_name(options_.security_mode)
             << " allow_plain_websocket_in_prod=" << (options_.allow_plain_websocket_in_prod ? "true" : "false")
             << " allow_remote_mutating_commands="
             << (options_.allow_remote_mutating_commands ? "true" : "false")
             << " allowed_client_instance_id=" << options_.allowed_client_instance_id
             << " bridge_auth_token_configured="
             << (options_.bridge_auth_token.empty() ? "false" : "true");
        if (!options_.default_root_path.empty()) {
            text << " default_root=" << options_.default_root_path.string();
        }
        log_ws("server", text.str());
    }

    running_ = true;
    do_accept();
    return storage::StoreStatus::success();
}

void WebsocketCrudServer::stop() {
    if (!running_) {
        return;
    }
    running_ = false;
    boost::system::error_code ignored;
    acceptor_.cancel(ignored);
    acceptor_.close(ignored);
    log_ws("server", "stopped");
}

bool WebsocketCrudServer::running() const noexcept {
    return running_.load();
}

void WebsocketCrudServer::do_accept() {
    if (!running_) {
        return;
    }

    acceptor_.async_accept(
        io_context_,
        [this](const boost::system::error_code& error, tcp::socket socket) {
            if (!running_) {
                return;
            }
            if (!error) {
                std::make_shared<CrudWebsocketSession>(
                    std::move(socket),
                    options_,
                    *store_,
                    command_processor_
                )->run();
            } else if (error != asio::error::operation_aborted) {
                std::ostringstream text;
                text << "accept failed: " << error.message();
                log_ws_error("server", text.str());
            }
            do_accept();
        }
    );
}

std::unique_ptr<CrudTransportServer> create_crud_transport_server(boost::asio::io_context& io_context,
                                                                  storage::LmdbStore& store,
                                                                  api::IndexingPipeline& pipeline,
                                                                  CrudServerOptions options) {
    switch (options.transport_kind) {
        case CrudTransportKind::WebSocket:
            return std::make_unique<WebsocketCrudServer>(
                io_context,
                store,
                pipeline,
                std::move(options)
            );
    }

    return std::make_unique<WebsocketCrudServer>(
        io_context,
        store,
        pipeline,
        std::move(options)
    );
}

} // namespace muninn::transport
