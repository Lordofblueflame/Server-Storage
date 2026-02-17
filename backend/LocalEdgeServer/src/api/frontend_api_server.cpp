#include "frontend_api_server.hpp"

#include <algorithm>
#include <atomic>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio/buffer.hpp>
#include <boost/asio/post.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/beast/websocket.hpp>

#include "command_load_balancer.hpp"
#include "frontend_dto.hpp"
#include "jwt_auth.hpp"

namespace localedge::api {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

namespace {

common::Status io_failure(const std::string_view context, const boost::system::error_code& error) {
    return common::Status::failure(
        common::ErrorCode::Internal,
        std::string(context) + " failed: " + error.message()
    );
}

void log_api(const std::string_view category, const std::string_view message) {
    std::cout << "[localedge.api][" << category << "] " << message << '\n';
}

void log_http(const http::request<http::string_body>& request,
              const std::string_view result,
              const std::string_view detail = {}) {
    std::ostringstream text;
    text << http::to_string(request.method()) << ' ' << request.target() << " -> " << result;
    if (!detail.empty()) {
        text << " (" << detail << ")";
    }
    log_api("http", text.str());
}

struct ParsedTarget {
    std::string path {};
    std::string query {};
    std::optional<std::string> access_token {};
};

ParsedTarget parse_target(const std::string_view target) {
    ParsedTarget parsed;
    const std::size_t q = target.find('?');
    if (q == std::string_view::npos) {
        parsed.path = std::string(target);
        return parsed;
    }

    parsed.path = std::string(target.substr(0, q));
    parsed.query = std::string(target.substr(q + 1U));

    const std::string_view query_view(parsed.query);
    const std::string_view key = "access_token=";
    const std::size_t key_pos = query_view.find(key);
    if (key_pos == std::string_view::npos) {
        return parsed;
    }

    std::size_t value_start = key_pos + key.size();
    std::size_t value_end = query_view.find('&', value_start);
    if (value_end == std::string_view::npos) {
        value_end = query_view.size();
    }
    if (value_end > value_start) {
        parsed.access_token = std::string(query_view.substr(value_start, value_end - value_start));
    }
    return parsed;
}

common::StatusOr<nlohmann::json> authorize_request(const http::request<http::string_body>& request,
                                                    const FrontendApiServerOptions& options,
                                                    const std::optional<std::string>& query_token) {
    if (!options.require_jwt) {
        return common::StatusOr<nlohmann::json>::success(nlohmann::json::object());
    }

    std::optional<std::string> token;
    const auto auth_header = request[http::field::authorization];
    if (!auth_header.empty()) {
        token = extract_bearer_token(std::string_view(auth_header.data(), auth_header.size()));
    }
    if (!token.has_value() && query_token.has_value()) {
        token = query_token;
    }
    if (!token.has_value()) {
        return common::StatusOr<nlohmann::json>::failure(
            common::ErrorCode::ValidationError,
            "Authorization Bearer token is required."
        );
    }

    JwtValidationOptions validation_options;
    validation_options.secret = options.jwt_secret;
    validation_options.expected_issuer = options.jwt_issuer;
    validation_options.expected_audience = options.jwt_audience;
    validation_options.clock_skew_seconds = options.jwt_clock_skew_seconds;
    validation_options.require_exp_claim = options.jwt_require_exp_claim;
    return validate_jwt_token(*token, validation_options);
}

http::status status_from_error(const common::ErrorCode code) {
    switch (code) {
        case common::ErrorCode::InvalidArgument:
        case common::ErrorCode::ParseError:
        case common::ErrorCode::ValidationError:
            return http::status::bad_request;
        case common::ErrorCode::NotFound:
            return http::status::not_found;
        case common::ErrorCode::StorageError:
            return http::status::internal_server_error;
        case common::ErrorCode::Internal:
            return http::status::service_unavailable;
        case common::ErrorCode::Ok:
        case common::ErrorCode::UnsupportedVersion:
        case common::ErrorCode::Duplicate:
        case common::ErrorCode::Gap:
            return http::status::unprocessable_entity;
    }
    return http::status::unprocessable_entity;
}

common::StatusOr<nlohmann::json> parse_json_object_body(const std::string_view body) {
    const auto parsed = backend::shared::crud::text_to_json(body);
    if (!parsed.has_value() || !parsed->is_object()) {
        return common::StatusOr<nlohmann::json>::failure(
            common::ErrorCode::ParseError,
            "Request body must be a JSON object."
        );
    }
    return common::StatusOr<nlohmann::json>::success(*parsed);
}

bool has_parent_traversal(const std::filesystem::path& path) {
    for (const auto& part : path) {
        if (part == "..") {
            return true;
        }
    }
    return false;
}

nlohmann::json build_openapi_json(const FrontendApiServerOptions& options) {
    nlohmann::json api = nlohmann::json::object();
    api["openapi"] = "3.0.3";

    nlohmann::json info = nlohmann::json::object();
    info["title"] = "LocalEdge Gateway API";
    info["version"] = "1.0.0";
    info["description"] =
        "Frontend gateway API for forwarding CRUD commands to HostIndexer and streaming validated results.";
    api["info"] = std::move(info);

    api["servers"] = nlohmann::json::array({
        {{"url", "http://localhost:" + std::to_string(options.port)}}
    });

    nlohmann::json paths = nlohmann::json::object();

    paths[options.health_path] = {
        {"get",
         {
             {"summary", "Gateway health"},
             {"responses",
              {
                  {"200", {{"description", "OK"}}}
              }}
         }}
    };

    paths[options.command_path] = {
        {"post",
         {
             {"summary", "Submit strict CRUD command DTO"},
             {"security", nlohmann::json::array({{{"bearerAuth", nlohmann::json::array()}}})},
             {"requestBody",
              {
                  {"required", true},
                  {"content",
                   {
                       {"application/json",
                        {
                            {"schema",
                             {
                                 {"$ref", "#/components/schemas/FrontendCommandRequest"}
                             }}
                        }}
                   }}
              }},
             {"responses",
              {
                  {"202",
                   {
                       {"description", "Accepted"},
                       {"content",
                        {
                            {"application/json",
                             {
                                 {"schema", {{"$ref", "#/components/schemas/FrontendCommandResponse"}}}
                             }}
                        }}
                   }},
                  {"400", {{"description", "Validation error"}}},
                  {"401", {{"description", "Unauthorized"}}},
                  {"503", {{"description", "Bridge unavailable"}}}
              }}
         }}
    };

    paths[options.stream_path] = {
        {"get",
         {
             {"summary", "WebSocket stream for command results"},
             {"description",
              "Upgrade to WebSocket. JWT required in Authorization header or access_token query parameter."}
         }}
    };

    paths[options.realtime_tree_path] = {
        {"post",
         {
             {"summary", "Get materialized children for a path"},
             {"security", nlohmann::json::array({{{"bearerAuth", nlohmann::json::array()}}})},
             {"responses",
              {
                  {"200", {{"description", "Realtime tree response"}}},
                  {"400", {{"description", "Validation error"}}},
                  {"401", {{"description", "Unauthorized"}}},
                  {"404", {{"description", "Host or path not found"}}}
              }}
         }}
    };

    paths[options.file_download_path] = {
        {"post",
         {
             {"summary", "Download file bytes as base64"},
             {"security", nlohmann::json::array({{{"bearerAuth", nlohmann::json::array()}}})},
             {"responses",
              {
                  {"200", {{"description", "File payload"}}},
                  {"400", {{"description", "Validation error"}}},
                  {"401", {{"description", "Unauthorized"}}},
                  {"404", {{"description", "File not found"}}}
              }}
         }}
    };

    paths[options.file_upload_path] = {
        {"post",
         {
             {"summary", "Upload a single file"},
             {"security", nlohmann::json::array({{{"bearerAuth", nlohmann::json::array()}}})},
             {"responses",
              {
                  {"200", {{"description", "Upload result"}}},
                  {"400", {{"description", "Validation error"}}},
                  {"401", {{"description", "Unauthorized"}}}
              }}
         }}
    };

    paths[options.directory_upload_path] = {
        {"post",
         {
             {"summary", "Upload directory payload in parallel"},
             {"security", nlohmann::json::array({{{"bearerAuth", nlohmann::json::array()}}})},
             {"responses",
              {
                  {"200", {{"description", "Upload result"}}},
                  {"400", {{"description", "Validation error"}}},
                  {"401", {{"description", "Unauthorized"}}}
              }}
         }}
    };

    paths[options.openapi_path] = {
        {"get", {{"summary", "OpenAPI document"}, {"responses", {{"200", {{"description", "OpenAPI JSON"}}}}}}}
    };

    paths[options.docs_path] = {
        {"get", {{"summary", "Swagger UI"}, {"responses", {{"200", {{"description", "Swagger UI HTML"}}}}}}}
    };

    api["paths"] = std::move(paths);

    nlohmann::json components = nlohmann::json::object();
    components["securitySchemes"] = {
        {"bearerAuth",
         {
             {"type", "http"},
             {"scheme", "bearer"},
             {"bearerFormat", "JWT"}
         }}
    };

    components["schemas"] = {
        {"FrontendCommandRequest",
         {
             {"type", "object"},
             {"additionalProperties", false},
             {"required", nlohmann::json::array({"operation"})},
             {"properties",
              {
                  {"request_id", {{"type", "string"}}},
                  {"operation",
                   {
                       {"type", "string"},
                       {"enum", nlohmann::json::array({"create", "read", "update", "delete"})}
                   }},
                  {"root_path", {{"type", "string"}}},
                  {"snapshot_id", {{"type", "integer"}, {"format", "uint64"}}},
                  {"base_snapshot_id", {{"type", "integer"}, {"format", "uint64"}}},
                  {"ack_sequence", {{"type", "integer"}, {"format", "uint64"}}},
                  {"outbox_batch_size", {{"type", "integer"}, {"format", "uint32"}, {"minimum", 1}}}
              }}
         }},
        {"FrontendCommandResponse",
         {
             {"type", "object"},
             {"required", nlohmann::json::array({"ok", "request_id", "message"})},
             {"properties",
              {
                  {"ok", {{"type", "boolean"}}},
                  {"request_id", {{"type", "string"}}},
                  {"message", {{"type", "string"}}}
              }}
         }}
    };
    api["components"] = std::move(components);
    return api;
}

std::string build_swagger_html(const std::string_view openapi_path) {
    std::string html = R"(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>LocalEdge API Docs</title>
  <link rel="stylesheet" href="https://unpkg.com/swagger-ui-dist@5/swagger-ui.css" />
</head>
<body>
  <div id="swagger-ui"></div>
  <script src="https://unpkg.com/swagger-ui-dist@5/swagger-ui-bundle.js"></script>
  <script>
    window.onload = function () {
      window.ui = SwaggerUIBundle({
        url: ')";
    html += "'";
    html += std::string(openapi_path);
    html += R"(',
        dom_id: '#swagger-ui',
        deepLinking: true,
        presets: [SwaggerUIBundle.presets.apis]
      });
    };
  </script>
</body>
</html>)";
    return html;
}

class WebsocketSession;

} // namespace

class FrontendApiSharedState final : public std::enable_shared_from_this<FrontendApiSharedState> {
public:
    FrontendApiSharedState(FrontendApiServerOptions options,
                           CommandHandler command_handler,
                           RealtimeTreeHandler realtime_tree_handler);

    const FrontendApiServerOptions& options() const noexcept;
    std::string next_request_id();
    common::Status execute_command(const backend::shared::crud::CrudCommandMessage& command);
    common::StatusOr<nlohmann::json> query_realtime_tree(std::string_view host_id, std::string_view path) const;
    void join(const std::shared_ptr<WebsocketSession>& session);
    void broadcast(std::string payload);

private:
    FrontendApiServerOptions options_ {};
    CommandLoadBalancer load_balancer_;
    RealtimeTreeHandler realtime_tree_handler_ {};
    std::atomic<std::uint64_t> request_counter_ {0U};
    std::mutex mutex_ {};
    std::vector<std::weak_ptr<WebsocketSession>> sessions_ {};
};

namespace {

class WebsocketSession final : public std::enable_shared_from_this<WebsocketSession> {
public:
    WebsocketSession(tcp::socket socket, std::shared_ptr<FrontendApiSharedState> state)
        : ws_(std::move(socket)),
          state_(std::move(state)) {
    }

    void run(http::request<http::string_body> request) {
        ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
        ws_.set_option(
            websocket::stream_base::decorator([](websocket::response_type& response) {
                response.set(http::field::server, std::string("localedge-api"));
            })
        );
        ws_.async_accept(
            request,
            beast::bind_front_handler(&WebsocketSession::on_accept, shared_from_this())
        );
    }

    void send(std::string payload) {
        asio::post(ws_.get_executor(), [self = shared_from_this(), payload = std::move(payload)]() mutable {
            const auto& options = self->state_->options();
            const std::size_t payload_size = payload.size();

            auto over_limit = [&options, self, payload_size]() -> bool {
                return self->write_queue_.size() >= options.ws_max_pending_messages ||
                       self->pending_bytes_ + payload_size > options.ws_max_pending_bytes;
            };

            if (over_limit()) {
                if (options.ws_backpressure_policy == BackpressurePolicy::DropNewest) {
                    log_api("ws", "dropping newest outbound frame due to backpressure");
                    return;
                }
                if (options.ws_backpressure_policy == BackpressurePolicy::DropOldest) {
                    std::size_t dropped = 0U;
                    while (over_limit() && !self->write_queue_.empty()) {
                        self->pending_bytes_ -= self->write_queue_.front().size();
                        self->write_queue_.pop_front();
                        ++dropped;
                    }
                    if (dropped > 0U) {
                        std::ostringstream text;
                        text << "dropped " << dropped << " queued outbound frame(s) due to backpressure";
                        log_api("ws", text.str());
                    }
                    if (over_limit()) {
                        log_api("ws", "outbound frame still over limit after dropping oldest, dropping current frame");
                        return;
                    }
                } else {
                    boost::system::error_code ignored;
                    self->ws_.close(websocket::close_code::policy_error, ignored);
                    log_api("ws", "closing websocket due to backpressure policy=disconnect");
                    return;
                }
            }

            const bool idle = self->write_queue_.empty();
            self->pending_bytes_ += payload_size;
            self->write_queue_.push_back(std::move(payload));
            if (idle) {
                self->do_write();
            }
        });
    }

private:
    void on_accept(const beast::error_code& error) {
        if (error) {
            std::ostringstream text;
            text << "websocket accept failed: " << error.message();
            log_api("ws", text.str());
            return;
        }
        log_api("ws", "websocket client connected");
        state_->join(shared_from_this());
        do_read();
    }

    void do_read() {
        ws_.async_read(
            read_buffer_,
            beast::bind_front_handler(&WebsocketSession::on_read, shared_from_this())
        );
    }

    void on_read(const beast::error_code& error, const std::size_t) {
        if (error == websocket::error::closed) {
            log_api("ws", "websocket client closed");
            return;
        }
        if (error) {
            std::ostringstream text;
            text << "websocket read failed: " << error.message();
            log_api("ws", text.str());
            return;
        }
        read_buffer_.consume(read_buffer_.size());
        do_read();
    }

    void do_write() {
        if (write_queue_.empty()) {
            return;
        }
        ws_.text(true);
        ws_.async_write(
            asio::buffer(write_queue_.front()),
            beast::bind_front_handler(&WebsocketSession::on_write, shared_from_this())
        );
    }

    void on_write(const beast::error_code& error, const std::size_t) {
        if (error) {
            std::ostringstream text;
            text << "websocket write failed: " << error.message();
            log_api("ws", text.str());
            return;
        }
        if (!write_queue_.empty()) {
            pending_bytes_ -= write_queue_.front().size();
            write_queue_.pop_front();
        }
        if (!write_queue_.empty()) {
            do_write();
        }
    }

    websocket::stream<tcp::socket> ws_;
    beast::flat_buffer read_buffer_ {};
    std::deque<std::string> write_queue_ {};
    std::size_t pending_bytes_ {0U};
    std::shared_ptr<FrontendApiSharedState> state_;
};

class HttpSession final : public std::enable_shared_from_this<HttpSession> {
public:
    HttpSession(tcp::socket socket, std::shared_ptr<FrontendApiSharedState> state)
        : stream_(std::move(socket)),
          state_(std::move(state)) {
    }

    void run() {
        do_read();
    }

private:
    void do_read() {
        request_ = {};
        http::async_read(
            stream_,
            read_buffer_,
            request_,
            beast::bind_front_handler(&HttpSession::on_read, shared_from_this())
        );
    }

    void on_read(const beast::error_code& error, const std::size_t) {
        if (error == http::error::end_of_stream) {
            do_close();
            return;
        }
        if (error) {
            std::ostringstream text;
            text << "http read failed: " << error.message();
            log_api("http", text.str());
            return;
        }

        const auto target = parse_target(request_.target());
        if (websocket::is_upgrade(request_) && target.path == state_->options().stream_path) {
            const auto auth = authorize_request(request_, state_->options(), target.access_token);
            if (!auth.ok()) {
                log_http(request_, "401", auth.status.message);
                send_json(http::status::unauthorized, R"({"ok":false,"message":"unauthorized"})");
                return;
            }

            log_http(request_, "101", "websocket upgrade accepted");
            std::make_shared<WebsocketSession>(stream_.release_socket(), state_)->run(std::move(request_));
            return;
        }

        handle_http_request(target);
    }

    void handle_http_request(const ParsedTarget& target) {
        auto require_auth = [&]() -> bool {
            const auto auth = authorize_request(request_, state_->options(), std::nullopt);
            if (auth.ok()) {
                return true;
            }
            log_http(request_, "401", auth.status.message);
            send_json(http::status::unauthorized, R"({"ok":false,"message":"unauthorized"})");
            return false;
        };

        auto parse_json_or_respond = [&]() -> std::optional<nlohmann::json> {
            const auto parsed = parse_json_object_body(request_.body());
            if (parsed.ok()) {
                return parsed.value;
            }
            log_http(request_, "400", parsed.status.message);
            send_json(http::status::bad_request, R"({"ok":false,"message":"invalid_json"})");
            return std::nullopt;
        };

        if (request_.method() == http::verb::get && target.path == state_->options().health_path) {
            nlohmann::json body = nlohmann::json::object();
            body["ok"] = true;
            body["service"] = "localedge-api-gateway";
            send_json(http::status::ok, body.dump());
            return;
        }

        if (request_.method() == http::verb::get && target.path == state_->options().openapi_path) {
            send_json(http::status::ok, build_openapi_json(state_->options()).dump());
            return;
        }

        if (request_.method() == http::verb::get && target.path == state_->options().docs_path) {
            send_html(http::status::ok, build_swagger_html(state_->options().openapi_path));
            return;
        }

        if (request_.method() == http::verb::post && target.path == state_->options().command_path) {
            if (!require_auth()) {
                return;
            }

            const auto parsed = parse_json_or_respond();
            if (!parsed.has_value()) {
                return;
            }

            const auto dto = parse_frontend_command_request(*parsed);
            if (!dto.ok()) {
                nlohmann::json error = nlohmann::json::object();
                error["ok"] = false;
                error["message"] = dto.status.message;
                log_http(request_, "400", dto.status.message);
                send_json(http::status::bad_request, error.dump());
                return;
            }

            auto request_dto = dto.value;
            if (request_dto.request_id.empty()) {
                request_dto.request_id = state_->next_request_id();
            }

            auto command = to_crud_command(request_dto);
            command.request_id = request_dto.request_id;

            const auto dispatch = state_->execute_command(command);
            FrontendCommandResponseDto response_dto;
            response_dto.ok = dispatch.ok();
            response_dto.request_id = request_dto.request_id;
            response_dto.message = dispatch.message;
            const auto response_json = to_frontend_command_response_json(response_dto);

            std::ostringstream detail;
            detail << "request_id=" << request_dto.request_id
                   << " operation=" << backend::shared::crud::operation_to_string(command.operation)
                   << " ok=" << (dispatch.ok() ? "true" : "false");
            if (!dispatch.message.empty()) {
                detail << " message=" << dispatch.message;
            }
            log_http(request_, dispatch.ok() ? "202" : "503", detail.str());
            send_json(dispatch.ok() ? http::status::accepted : http::status::service_unavailable, response_json.dump());
            return;
        }

        if (request_.method() == http::verb::post && target.path == state_->options().realtime_tree_path) {
            if (!require_auth()) {
                return;
            }

            const auto body = parse_json_or_respond();
            if (!body.has_value()) {
                return;
            }

            std::string host_id;
            if (body->contains("host_id")) {
                if (!body->at("host_id").is_string()) {
                    send_json(http::status::bad_request, R"({"ok":false,"message":"host_id must be a string"})");
                    return;
                }
                host_id = body->at("host_id").get<std::string>();
            }

            std::string path = "/";
            if (body->contains("path")) {
                if (!body->at("path").is_string()) {
                    send_json(http::status::bad_request, R"({"ok":false,"message":"path must be a string"})");
                    return;
                }
                path = body->at("path").get<std::string>();
            }

            const auto tree = state_->query_realtime_tree(host_id, path);
            if (!tree.ok()) {
                nlohmann::json error = nlohmann::json::object();
                error["ok"] = false;
                error["message"] = tree.status.message;
                send_json(status_from_error(tree.status.code), error.dump());
                return;
            }
            send_json(http::status::ok, tree.value.dump());
            return;
        }

        if (request_.method() == http::verb::post && target.path == state_->options().file_download_path) {
            if (!require_auth()) {
                return;
            }

            const auto body = parse_json_or_respond();
            if (!body.has_value()) {
                return;
            }
            if (!body->contains("path") || !body->at("path").is_string()) {
                send_json(http::status::bad_request, R"({"ok":false,"message":"path is required"})");
                return;
            }

            const std::filesystem::path file_path = body->at("path").get<std::string>();
            std::error_code fs_error;
            if (!std::filesystem::exists(file_path, fs_error) || fs_error ||
                !std::filesystem::is_regular_file(file_path, fs_error)) {
                send_json(http::status::not_found, R"({"ok":false,"message":"file not found"})");
                return;
            }

            std::ifstream stream(file_path, std::ios::binary);
            if (!stream) {
                send_json(http::status::internal_server_error, R"({"ok":false,"message":"failed to open file"})");
                return;
            }
            std::vector<std::uint8_t> bytes {
                std::istreambuf_iterator<char>(stream),
                std::istreambuf_iterator<char>()
            };

            nlohmann::json response = nlohmann::json::object();
            response["ok"] = true;
            response["path"] = file_path.string();
            response["size_bytes"] = bytes.size();
            response["content_b64"] = backend::shared::crud::base64_encode(bytes);
            send_json(http::status::ok, response.dump());
            return;
        }

        if (request_.method() == http::verb::post && target.path == state_->options().file_upload_path) {
            if (!require_auth()) {
                return;
            }

            const auto body = parse_json_or_respond();
            if (!body.has_value()) {
                return;
            }

            if (!body->contains("destination_dir") || !body->at("destination_dir").is_string() ||
                !body->contains("file_name") || !body->at("file_name").is_string() ||
                !body->contains("content_b64") || !body->at("content_b64").is_string()) {
                send_json(
                    http::status::bad_request,
                    R"({"ok":false,"message":"destination_dir, file_name, content_b64 are required"})"
                );
                return;
            }

            const std::filesystem::path destination_dir = body->at("destination_dir").get<std::string>();
            const std::filesystem::path file_name = body->at("file_name").get<std::string>();
            const std::string content_b64 = body->at("content_b64").get<std::string>();

            if (file_name.empty() || file_name.has_parent_path()) {
                send_json(http::status::bad_request, R"({"ok":false,"message":"file_name must be a single filename"})");
                return;
            }

            const auto decoded = backend::shared::crud::base64_decode(content_b64);
            if (!decoded.has_value()) {
                send_json(http::status::bad_request, R"({"ok":false,"message":"content_b64 is invalid"})");
                return;
            }

            const auto target_path = destination_dir / file_name;
            std::error_code fs_error;
            std::filesystem::create_directories(target_path.parent_path(), fs_error);
            if (fs_error) {
                send_json(http::status::internal_server_error, R"({"ok":false,"message":"failed to create directory"})");
                return;
            }

            std::ofstream stream(target_path, std::ios::binary | std::ios::trunc);
            if (!stream) {
                send_json(http::status::internal_server_error, R"({"ok":false,"message":"failed to create file"})");
                return;
            }
            stream.write(reinterpret_cast<const char*>(decoded->data()), static_cast<std::streamsize>(decoded->size()));
            if (!stream) {
                send_json(http::status::internal_server_error, R"({"ok":false,"message":"failed to write file"})");
                return;
            }

            nlohmann::json response = nlohmann::json::object();
            response["ok"] = true;
            response["path"] = target_path.string();
            response["size_bytes"] = decoded->size();
            send_json(http::status::ok, response.dump());
            return;
        }

        if (request_.method() == http::verb::post && target.path == state_->options().directory_upload_path) {
            if (!require_auth()) {
                return;
            }

            const auto body = parse_json_or_respond();
            if (!body.has_value()) {
                return;
            }

            if (!body->contains("destination_dir") || !body->at("destination_dir").is_string() ||
                !body->contains("files") || !body->at("files").is_array()) {
                send_json(
                    http::status::bad_request,
                    R"({"ok":false,"message":"destination_dir and files[] are required"})"
                );
                return;
            }

            struct UploadItem {
                std::string relative_path {};
                std::string content_b64 {};
            };

            std::vector<UploadItem> items;
            items.reserve(body->at("files").size());
            for (const auto& item : body->at("files")) {
                if (!item.is_object() ||
                    !item.contains("relative_path") || !item.at("relative_path").is_string() ||
                    !item.contains("content_b64") || !item.at("content_b64").is_string()) {
                    send_json(
                        http::status::bad_request,
                        R"({"ok":false,"message":"files[] items require relative_path and content_b64"})"
                    );
                    return;
                }
                std::filesystem::path relative = item.at("relative_path").get<std::string>();
                if (relative.empty() || relative.is_absolute() || has_parent_traversal(relative)) {
                    send_json(http::status::bad_request, R"({"ok":false,"message":"relative_path is invalid"})");
                    return;
                }
                items.push_back(
                    UploadItem {
                        relative.generic_string(),
                        item.at("content_b64").get<std::string>()
                    }
                );
            }

            const std::filesystem::path destination_dir = body->at("destination_dir").get<std::string>();
            std::size_t worker_count = std::max<std::size_t>(1U, state_->options().directory_upload_threads);
            if (body->contains("threads")) {
                if (!body->at("threads").is_number_unsigned() && !body->at("threads").is_number_integer()) {
                    send_json(http::status::bad_request, R"({"ok":false,"message":"threads must be a number"})");
                    return;
                }
                const auto requested = body->at("threads").get<std::int64_t>();
                if (requested <= 0) {
                    send_json(http::status::bad_request, R"({"ok":false,"message":"threads must be positive"})");
                    return;
                }
                worker_count = std::min<std::size_t>(
                    static_cast<std::size_t>(requested),
                    std::max<std::size_t>(1U, state_->options().directory_upload_threads)
                );
            }

            std::atomic<std::size_t> next_index {0U};
            std::atomic<std::size_t> success_count {0U};
            std::mutex failures_mutex;
            std::vector<nlohmann::json> failures;

            auto worker = [&]() {
                while (true) {
                    const std::size_t index = next_index.fetch_add(1U);
                    if (index >= items.size()) {
                        return;
                    }

                    const auto& item = items[index];
                    const auto decoded = backend::shared::crud::base64_decode(item.content_b64);
                    if (!decoded.has_value()) {
                        std::lock_guard<std::mutex> lock(failures_mutex);
                        failures.push_back(
                            {
                                {"relative_path", item.relative_path},
                                {"message", "content_b64 is invalid"}
                            }
                        );
                        continue;
                    }

                    const auto target = destination_dir / std::filesystem::path(item.relative_path);
                    std::error_code fs_error;
                    std::filesystem::create_directories(target.parent_path(), fs_error);
                    if (fs_error) {
                        std::lock_guard<std::mutex> lock(failures_mutex);
                        failures.push_back(
                            {
                                {"relative_path", item.relative_path},
                                {"message", "failed to create directory"}
                            }
                        );
                        continue;
                    }

                    std::ofstream stream(target, std::ios::binary | std::ios::trunc);
                    if (!stream) {
                        std::lock_guard<std::mutex> lock(failures_mutex);
                        failures.push_back(
                            {
                                {"relative_path", item.relative_path},
                                {"message", "failed to create file"}
                            }
                        );
                        continue;
                    }
                    stream.write(reinterpret_cast<const char*>(decoded->data()),
                                 static_cast<std::streamsize>(decoded->size()));
                    if (!stream) {
                        std::lock_guard<std::mutex> lock(failures_mutex);
                        failures.push_back(
                            {
                                {"relative_path", item.relative_path},
                                {"message", "failed to write file"}
                            }
                        );
                        continue;
                    }

                    success_count.fetch_add(1U);
                }
            };

            std::vector<std::thread> workers;
            workers.reserve(worker_count);
            for (std::size_t i = 0; i < worker_count; ++i) {
                workers.emplace_back(worker);
            }
            for (auto& worker_thread : workers) {
                if (worker_thread.joinable()) {
                    worker_thread.join();
                }
            }

            nlohmann::json response = nlohmann::json::object();
            response["ok"] = failures.empty();
            response["destination_dir"] = destination_dir.string();
            response["threads_used"] = worker_count;
            response["total_files"] = items.size();
            response["uploaded_files"] = success_count.load();
            response["failed_files"] = failures.size();
            if (!failures.empty()) {
                response["failures"] = failures;
            }
            send_json(http::status::ok, response.dump());
            return;
        }

        log_http(request_, "404");
        send_json(http::status::not_found, R"({"ok":false,"message":"not_found"})");
    }

    void send_json(const http::status status, std::string body) {
        auto response = std::make_shared<http::response<http::string_body>>(status, request_.version());
        response->set(http::field::server, "localedge-api");
        response->set(http::field::content_type, "application/json");
        response->body() = std::move(body);
        response->prepare_payload();
        response->keep_alive(false);

        http::async_write(
            stream_,
            *response,
            [self = shared_from_this(), response](const beast::error_code&, const std::size_t) {
                self->do_close();
            }
        );
    }

    void send_html(const http::status status, std::string body) {
        auto response = std::make_shared<http::response<http::string_body>>(status, request_.version());
        response->set(http::field::server, "localedge-api");
        response->set(http::field::content_type, "text/html; charset=utf-8");
        response->body() = std::move(body);
        response->prepare_payload();
        response->keep_alive(false);

        http::async_write(
            stream_,
            *response,
            [self = shared_from_this(), response](const beast::error_code&, const std::size_t) {
                self->do_close();
            }
        );
    }

    void do_close() {
        beast::error_code ignored;
        stream_.socket().shutdown(tcp::socket::shutdown_send, ignored);
    }

    beast::tcp_stream stream_;
    beast::flat_buffer read_buffer_ {};
    http::request<http::string_body> request_ {};
    std::shared_ptr<FrontendApiSharedState> state_;
};

} // namespace

FrontendApiSharedState::FrontendApiSharedState(FrontendApiServerOptions options,
                                               CommandHandler command_handler,
                                               RealtimeTreeHandler realtime_tree_handler)
    : options_(std::move(options)),
      load_balancer_(
          std::move(command_handler),
          CommandLoadBalancerOptions {
              options_.command_worker_threads,
              options_.command_max_queue_per_worker,
              options_.command_execution_timeout
          }),
      realtime_tree_handler_(std::move(realtime_tree_handler)) {
}

const FrontendApiServerOptions& FrontendApiSharedState::options() const noexcept {
    return options_;
}

std::string FrontendApiSharedState::next_request_id() {
    const auto next = request_counter_.fetch_add(1U, std::memory_order_relaxed) + 1U;
    return "frontend-" + std::to_string(next);
}

common::Status FrontendApiSharedState::execute_command(const backend::shared::crud::CrudCommandMessage& command) {
    return load_balancer_.execute(command);
}

common::StatusOr<nlohmann::json> FrontendApiSharedState::query_realtime_tree(const std::string_view host_id,
                                                                              const std::string_view path) const {
    if (!realtime_tree_handler_) {
        return common::StatusOr<nlohmann::json>::failure(
            common::ErrorCode::NotFound,
            "Realtime tree view is not configured."
        );
    }
    return realtime_tree_handler_(host_id, path);
}

void FrontendApiSharedState::join(const std::shared_ptr<WebsocketSession>& session) {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.push_back(session);
}

void FrontendApiSharedState::broadcast(std::string payload) {
    std::vector<std::shared_ptr<WebsocketSession>> active;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = sessions_.begin(); it != sessions_.end();) {
            const auto session = it->lock();
            if (!session) {
                it = sessions_.erase(it);
                continue;
            }
            active.push_back(session);
            ++it;
        }
    }

    for (const auto& session : active) {
        session->send(payload);
    }
}

FrontendApiServer::FrontendApiServer(boost::asio::io_context& io_context,
                                     FrontendApiServerOptions options,
                                     CommandHandler command_handler,
                                     RealtimeTreeHandler realtime_tree_handler)
    : io_context_(io_context),
      acceptor_(io_context),
      options_(std::move(options)),
      command_handler_(std::move(command_handler)),
      realtime_tree_handler_(std::move(realtime_tree_handler)),
      shared_state_(std::make_shared<FrontendApiSharedState>(options_, command_handler_, realtime_tree_handler_)) {
}

common::Status FrontendApiServer::start() {
    if (running_) {
        return common::Status::success();
    }

    if (options_.require_jwt && options_.jwt_secret.empty()) {
        return common::Status::failure(
            common::ErrorCode::InvalidArgument,
            "JWT is required but jwt_secret is empty."
        );
    }

    boost::system::error_code error;
    const auto address = asio::ip::make_address(options_.listen_address, error);
    if (error) {
        return io_failure("make_address", error);
    }

    tcp::endpoint endpoint(address, options_.port);
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
             << " health=" << options_.health_path
             << " command=" << options_.command_path
             << " stream=" << options_.stream_path
             << " realtime_tree=" << options_.realtime_tree_path
             << " file_download=" << options_.file_download_path
             << " file_upload=" << options_.file_upload_path
             << " directory_upload=" << options_.directory_upload_path
             << " directory_upload_threads=" << options_.directory_upload_threads
             << " jwt_required=" << (options_.require_jwt ? "true" : "false");
        log_api("server", text.str());
    }

    running_ = true;
    do_accept();
    return common::Status::success();
}

void FrontendApiServer::stop() {
    if (!running_) {
        return;
    }
    running_ = false;
    boost::system::error_code ignored;
    acceptor_.cancel(ignored);
    acceptor_.close(ignored);
    log_api("server", "stopped");
}

bool FrontendApiServer::running() const noexcept {
    return running_.load();
}

void FrontendApiServer::broadcast_crud_result(const backend::shared::crud::CrudResultMessage& result) {
    if (!result.records.empty() || !result.ok) {
        std::ostringstream text;
        text << "broadcast crud_result request_id=" << result.request_id
             << " ok=" << (result.ok ? "true" : "false")
             << " records=" << result.records.size();
        if (!result.message.empty()) {
            text << " message=" << result.message;
        }
        log_api("ws", text.str());
    }
    shared_state_->broadcast(to_frontend_stream_event_json(result).dump());
}

void FrontendApiServer::do_accept() {
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
                std::make_shared<HttpSession>(std::move(socket), shared_state_)->run();
            } else if (error != asio::error::operation_aborted) {
                std::ostringstream text;
                text << "accept failed: " << error.message();
                log_api("server", text.str());
            }
            do_accept();
        }
    );
}

} // namespace localedge::api
