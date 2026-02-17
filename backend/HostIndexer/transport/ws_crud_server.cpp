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

namespace host_indexer::transport {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
namespace crud = backend::shared::crud;

void log_ws(const std::string_view category, const std::string_view message) {
    std::cout << "[hostindexer.ws][" << category << "] " << message << '\n';
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

std::optional<std::filesystem::path> resolve_root_path(const std::string_view command_root,
                                                       const WebsocketCrudServerOptions& options) {
    if (!command_root.empty()) {
        return std::filesystem::path(std::string(command_root));
    }
    if (!options.default_root_path.empty()) {
        return options.default_root_path;
    }
    return std::nullopt;
}

beast::string_view request_path_from_target(const beast::string_view target) {
    const auto query_pos = target.find('?');
    return target.substr(0, query_pos);
}

storage::StoreStatus append_transport_records(storage::LmdbStore& store,
                                              const std::size_t max_items,
                                              std::vector<crud::TransportRecordMessage>& out_records) {
    std::vector<storage::TransportRecord> records;
    const auto fetch_status = store.fetch_transport_batch(std::max<std::size_t>(1U, max_items), records);
    if (!fetch_status.ok()) {
        return fetch_status;
    }

    out_records.clear();
    out_records.reserve(records.size());
    for (const auto& record : records) {
        crud::TransportRecordMessage item;
        item.sequence = record.sequence;
        item.record_type = static_cast<std::uint8_t>(record.type);
        item.created_at = record.created_at;
        item.routing_key = record.routing_key;
        item.payload_b64 = crud::base64_encode(record.payload);
        out_records.push_back(std::move(item));
    }
    return storage::StoreStatus::success();
}

std::string pipeline_error_message(const api::PipelineResult& pipeline_result) {
    if (!pipeline_result.snapshot_store_status.ok()) {
        return pipeline_result.snapshot_store_status.message;
    }
    if (!pipeline_result.transport_snapshot_status.ok()) {
        return pipeline_result.transport_snapshot_status.message;
    }
    if (!pipeline_result.delta_store_status.ok()) {
        return pipeline_result.delta_store_status.message;
    }
    if (!pipeline_result.transport_delta_status.ok()) {
        return pipeline_result.transport_delta_status.message;
    }
    if (!pipeline_result.snapshot_validation.ok()) {
        return "Snapshot validation failed.";
    }
    if (pipeline_result.computed_delta.has_value() && !pipeline_result.delta_validation.ok()) {
        return "Delta validation failed.";
    }
    return "Pipeline execution failed.";
}

class CrudWebsocketSession final : public std::enable_shared_from_this<CrudWebsocketSession> {
public:
    CrudWebsocketSession(tcp::socket socket,
                         const WebsocketCrudServerOptions& options,
                         storage::LmdbStore& store,
                         api::IndexingPipeline& pipeline,
                         std::mutex& pipeline_mutex)
        : websocket_(std::move(socket)),
          session_id_(g_session_counter.fetch_add(1U) + 1U),
          options_(options),
          store_(store),
          pipeline_(pipeline),
          pipeline_mutex_(pipeline_mutex) {
        boost::system::error_code error;
        const auto endpoint = websocket_.next_layer().remote_endpoint(error);
        if (!error) {
            peer_endpoint_ = endpoint.address().to_string() + ":" + std::to_string(endpoint.port());
        } else {
            peer_endpoint_ = "unknown";
        }

        std::ostringstream text;
        text << "session=" << session_id_ << " peer=" << peer_endpoint_ << " connected";
        log_ws("session", text.str());
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
    void reject_upgrade(const http::status status, std::string message) {
        auto response = std::make_shared<http::response<http::string_body>>(status, handshake_request_.version());
        response->set(http::field::server, std::string("host-indexer"));
        response->set(http::field::content_type, "text/plain; charset=utf-8");
        response->keep_alive(false);
        response->body() = std::move(message);
        response->prepare_payload();

        http::async_write(
            websocket_.next_layer(),
            *response,
            [self = shared_from_this(), response](const beast::error_code& write_error, const std::size_t) {
                if (write_error) {
                    std::ostringstream text;
                    text << "session=" << self->session_id_
                         << " upgrade reject write failed: " << write_error.message();
                    log_ws("session", text.str());
                }

                boost::system::error_code ignored;
                self->websocket_.next_layer().shutdown(tcp::socket::shutdown_both, ignored);
                self->websocket_.next_layer().close(ignored);
            }
        );
    }

    void on_upgrade_request(const beast::error_code& error, const std::size_t) {
        if (error) {
            std::ostringstream text;
            text << "session=" << session_id_ << " upgrade request read failed: " << error.message();
            log_ws("session", text.str());
            return;
        }

        if (!websocket::is_upgrade(handshake_request_)) {
            std::ostringstream text;
            text << "session=" << session_id_ << " rejected non-websocket upgrade request";
            log_ws("protocol", text.str());
            reject_upgrade(http::status::bad_request, "WebSocket upgrade is required.");
            return;
        }

        const auto requested_path = request_path_from_target(handshake_request_.target());
        const beast::string_view expected_path(options_.websocket_path.data(), options_.websocket_path.size());
        if (!options_.websocket_path.empty() && requested_path != expected_path) {
            std::ostringstream text;
            text << "session=" << session_id_ << " rejected websocket path=" << std::string(requested_path)
                 << " expected=" << options_.websocket_path;
            log_ws("auth", text.str());
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
            log_ws("session", text.str());
            return;
        }
        std::ostringstream text;
        text << "session=" << session_id_ << " websocket upgrade accepted";
        log_ws("session", text.str());
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
            std::ostringstream text;
            text << "session=" << session_id_ << " closed by peer";
            log_ws("session", text.str());
            return;
        }
        if (error) {
            std::ostringstream text;
            text << "session=" << session_id_ << " read failed: " << error.message();
            log_ws("session", text.str());
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
            log_ws("protocol", text.str());
            enqueue_json(crud::to_json(make_error_result("", "Malformed JSON message.")));
            return;
        }

        const std::string type = parsed_json->value("type", "");
        if (type == "hello") {
            handle_hello(*parsed_json);
            return;
        }

        if (type == "ingest_ack") {
            handle_ingest_ack(*parsed_json);
            return;
        }

        if (type == "crud_command") {
            if (!hello_accepted_) {
                enqueue_json(crud::to_json(make_error_result("", "hello must be completed before crud_command.")));
                return;
            }
            handle_command(*parsed_json);
            return;
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
            log_ws("protocol", text.str());
            enqueue_json(crud::to_json(ack));
            return;
        }

        if (hello->client_instance_id.empty()) {
            ack.accepted = false;
            ack.reason = "client_instance_id is required.";
            std::ostringstream text;
            text << "session=" << session_id_ << " hello rejected: missing client_instance_id";
            log_ws("auth", text.str());
            enqueue_json(crud::to_json(ack));
            return;
        }

        if (!options_.allowed_client_instance_id.empty() &&
            hello->client_instance_id != options_.allowed_client_instance_id) {
            ack.accepted = false;
            ack.reason = "client_instance_id is not allowed.";
            std::ostringstream text;
            text << "session=" << session_id_ << " hello rejected: client_instance_id="
                 << hello->client_instance_id << " is not allowed";
            log_ws("auth", text.str());
            enqueue_json(crud::to_json(ack));
            return;
        }

        if (!options_.bridge_auth_token.empty() &&
            hello->auth_token != options_.bridge_auth_token) {
            ack.accepted = false;
            ack.reason = "bridge auth token mismatch.";
            std::ostringstream text;
            text << "session=" << session_id_ << " hello rejected: bridge auth token mismatch";
            log_ws("auth", text.str());
            enqueue_json(crud::to_json(ack));
            return;
        }

        hello_accepted_ = true;
        peer_host_id_ = hello->host_id;
        {
            std::ostringstream text;
            text << "session=" << session_id_ << " hello accepted host_id=" << peer_host_id_
                 << " client_instance_id=" << hello->client_instance_id;
            log_ws("protocol", text.str());
        }
        ack.accepted = true;
        ack.reason.clear();
        enqueue_json(crud::to_json(ack));
    }

    void handle_ingest_ack(const nlohmann::json& payload) {
        const auto ingest_ack = crud::parse_ingest_ack(payload);
        if (!ingest_ack.has_value()) {
            std::ostringstream text;
            text << "session=" << session_id_ << " invalid ingest_ack payload";
            log_ws("protocol", text.str());
            enqueue_json(crud::to_json(make_error_result("", "Invalid ingest_ack payload.")));
            return;
        }

        if (!ingest_ack->ok || ingest_ack->ack_sequence == 0U) {
            return;
        }

        const auto ack_status = store_.ack_transport_until(ingest_ack->ack_sequence);
        if (!ack_status.ok()) {
            enqueue_json(crud::to_json(make_error_result(ingest_ack->request_id, ack_status.message)));
            std::ostringstream text;
            text << "session=" << session_id_ << " ack_transport_until failed request_id=" << ingest_ack->request_id
                 << " ack_sequence=" << ingest_ack->ack_sequence
                 << " message=" << ack_status.message;
            log_ws("store", text.str());
        } else {
            std::ostringstream text;
            text << "session=" << session_id_ << " acked transport until sequence=" << ingest_ack->ack_sequence;
            log_ws("store", text.str());
        }
    }

    void handle_command(const nlohmann::json& payload) {
        const auto command = crud::parse_crud_command(payload);
        if (!command.has_value()) {
            std::ostringstream text;
            text << "session=" << session_id_ << " invalid crud_command payload";
            log_ws("protocol", text.str());
            enqueue_json(crud::to_json(make_error_result("", "Invalid crud_command payload.")));
            return;
        }

        const auto result = process_command(*command);
        if (command->operation != crud::Operation::Read || !result.ok || !result.records.empty()) {
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

    crud::CrudResultMessage process_command(const crud::CrudCommandMessage& command) {
        crud::CrudResultMessage result;
        result.request_id = command.request_id;
        result.ok = false;
        const std::size_t batch_size = command.outbox_batch_size == 0U
            ? std::max<std::size_t>(1U, options_.default_outbox_batch_size)
            : static_cast<std::size_t>(command.outbox_batch_size);

        if (command.ack_sequence > 0U) {
            const auto ack_status = store_.ack_transport_until(command.ack_sequence);
            if (!ack_status.ok()) {
                result.message = ack_status.message;
                return result;
            }
        }

        switch (command.operation) {
            case crud::Operation::Read: {
                const auto fetch_status = append_transport_records(
                    store_,
                    batch_size,
                    result.records
                );
                if (!fetch_status.ok()) {
                    result.message = fetch_status.message;
                    return result;
                }
                result.ok = true;
                result.message = "ok";
                return result;
            }

            case crud::Operation::Create: {
                const auto root_path = resolve_root_path(command.root_path, options_);
                if (!root_path.has_value()) {
                    result.message = "Create operation requires root_path.";
                    return result;
                }

                api::PipelineResult pipeline_result;
                {
                    std::lock_guard<std::mutex> lock(pipeline_mutex_);
                    pipeline_result = pipeline_.capture_snapshot(*root_path, {}, true);
                }

                if (!pipeline_result.ok() || !pipeline_result.transport_snapshot_status.ok()) {
                    result.message = pipeline_error_message(pipeline_result);
                    return result;
                }

                result.snapshot_id = pipeline_result.snapshot_build.snapshot.snapshot_id;
                const auto fetch_status = append_transport_records(
                    store_,
                    batch_size,
                    result.records
                );
                if (!fetch_status.ok()) {
                    result.message = fetch_status.message;
                    return result;
                }

                result.ok = true;
                result.message = "snapshot_created";
                return result;
            }

            case crud::Operation::Update: {
                const auto root_path = resolve_root_path(command.root_path, options_);
                if (!root_path.has_value()) {
                    result.message = "Update operation requires root_path.";
                    return result;
                }

                const std::uint64_t base_snapshot = command.base_snapshot_id != 0U
                    ? command.base_snapshot_id
                    : command.snapshot_id;
                if (base_snapshot == 0U) {
                    result.message = "Update operation requires base_snapshot_id (or snapshot_id).";
                    return result;
                }

                api::PipelineResult pipeline_result;
                {
                    std::lock_guard<std::mutex> lock(pipeline_mutex_);
                    pipeline_result = pipeline_.capture_snapshot_and_delta(*root_path, base_snapshot, {}, true);
                }

                if (!pipeline_result.ok() ||
                    !pipeline_result.transport_snapshot_status.ok() ||
                    !pipeline_result.transport_delta_status.ok()) {
                    result.message = pipeline_error_message(pipeline_result);
                    return result;
                }

                result.base_snapshot_id = base_snapshot;
                result.target_snapshot_id = pipeline_result.snapshot_build.snapshot.snapshot_id;
                result.snapshot_id = result.target_snapshot_id;

                const auto fetch_status = append_transport_records(
                    store_,
                    batch_size,
                    result.records
                );
                if (!fetch_status.ok()) {
                    result.message = fetch_status.message;
                    return result;
                }

                result.ok = true;
                result.message = "snapshot_updated";
                return result;
            }

            case crud::Operation::Delete: {
                result.ok = true;
                result.message = "acknowledged";
                return result;
            }
        }

        result.message = "Unsupported CRUD operation.";
        return result;
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
            log_ws("session", text.str());
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
    api::IndexingPipeline& pipeline_;
    std::mutex& pipeline_mutex_;
    bool hello_accepted_ {false};
    std::string peer_host_id_ {};
};

} // namespace

WebsocketCrudServer::WebsocketCrudServer(boost::asio::io_context& io_context,
                                         storage::LmdbStore& store,
                                         api::IndexingPipeline& pipeline,
                                         WebsocketCrudServerOptions options)
    : io_context_(io_context),
      acceptor_(io_context),
      store_(&store),
      pipeline_(&pipeline),
      options_(std::move(options)) {
}

storage::StoreStatus WebsocketCrudServer::start() {
    if (running_) {
        return storage::StoreStatus::success();
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
             << " default_outbox_batch=" << options_.default_outbox_batch_size
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
                    *pipeline_,
                    pipeline_mutex_
                )->run();
            } else if (error != asio::error::operation_aborted) {
                std::ostringstream text;
                text << "accept failed: " << error.message();
                log_ws("server", text.str());
            }
            do_accept();
        }
    );
}

} // namespace host_indexer::transport
