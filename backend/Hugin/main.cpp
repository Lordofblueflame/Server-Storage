#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>

#include "api/frontend_api_server.hpp"
#include "ingest/message_validator.hpp"
#include "net/muninn_transport.hpp"
#include "protocol/muninn_frame.hpp"
#include "sinks/materializer_sink.hpp"

namespace {

std::atomic<bool> g_stop_requested {false};

void signal_handler(int) {
    g_stop_requested.store(true);
}

std::string detect_host_id() {
    if (const char* hostname = std::getenv("HOSTNAME"); hostname != nullptr && hostname[0] != '\0') {
        return std::string(hostname);
    }
    if (const char* computer_name = std::getenv("COMPUTERNAME"); computer_name != nullptr && computer_name[0] != '\0') {
        return std::string(computer_name);
    }
    return "unknown-host";
}

std::optional<hugin::api::BackpressurePolicy> parse_backpressure_policy(const std::string_view value) {
    if (value == "drop_oldest") {
        return hugin::api::BackpressurePolicy::DropOldest;
    }
    if (value == "drop_newest") {
        return hugin::api::BackpressurePolicy::DropNewest;
    }
    if (value == "disconnect") {
        return hugin::api::BackpressurePolicy::Disconnect;
    }
    return std::nullopt;
}

std::string backpressure_policy_to_text(const hugin::api::BackpressurePolicy policy) {
    switch (policy) {
        case hugin::api::BackpressurePolicy::DropOldest:
            return "drop_oldest";
        case hugin::api::BackpressurePolicy::DropNewest:
            return "drop_newest";
        case hugin::api::BackpressurePolicy::Disconnect:
            return "disconnect";
    }
    return "drop_oldest";
}

std::string entry_type_text(const muninn::domain::EntryType type) {
    switch (type) {
        case muninn::domain::EntryType::File:
            return "file";
        case muninn::domain::EntryType::Directory:
            return "directory";
        case muninn::domain::EntryType::Symlink:
            return "symlink";
        case muninn::domain::EntryType::Special:
            return "special";
    }
    return "special";
}

std::optional<std::uint64_t> parse_u64_arg(const std::string_view flag, const std::string& value) {
    try {
        std::size_t parsed_chars = 0;
        const auto parsed = std::stoull(value, &parsed_chars, 10);
        if (parsed_chars != value.size()) {
            std::cerr << "Invalid numeric value for " << flag << ": " << value << '\n';
            return std::nullopt;
        }
        return static_cast<std::uint64_t>(parsed);
    } catch (const std::exception&) {
        std::cerr << "Invalid numeric value for " << flag << ": " << value << '\n';
        return std::nullopt;
    }
}

std::optional<std::uint16_t> parse_u16_arg(const std::string_view flag, const std::string& value) {
    const auto parsed = parse_u64_arg(flag, value);
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    if (*parsed > static_cast<std::uint64_t>(std::numeric_limits<std::uint16_t>::max())) {
        std::cerr << "Value out of range for " << flag << ": " << value << '\n';
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(*parsed);
}

std::optional<std::size_t> parse_size_arg(const std::string_view flag, const std::string& value) {
    const auto parsed = parse_u64_arg(flag, value);
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    if (*parsed > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        std::cerr << "Value out of range for " << flag << ": " << value << '\n';
        return std::nullopt;
    }
    return static_cast<std::size_t>(*parsed);
}

} // namespace

int main(int argc, char** argv) {
    std::cout.setf(std::ios::unitbuf);
    std::cerr.setf(std::ios::unitbuf);

    std::size_t io_threads = std::max<std::size_t>(2U, std::thread::hardware_concurrency());

    hugin::net::MuninnClientOptions muninn_options;
    muninn_options.host_id = detect_host_id();
    muninn_options.client_instance_id = "hugin-gateway-1";
    if (const char* bridge_token = std::getenv("MUNINN_BRIDGE_AUTH_TOKEN");
        bridge_token != nullptr && bridge_token[0] != '\0') {
        muninn_options.auth_token = bridge_token;
    }
    if (const char* reconnect_max_ms = std::getenv("HUGIN_RECONNECT_MAX_MS");
        reconnect_max_ms != nullptr && reconnect_max_ms[0] != '\0') {
        const auto parsed = parse_u64_arg("HUGIN_RECONNECT_MAX_MS", reconnect_max_ms);
        if (!parsed.has_value()) {
            return 1;
        }
        muninn_options.reconnect_max_delay = std::chrono::milliseconds(*parsed);
    }
    if (const char* reconnect_backoff_factor = std::getenv("HUGIN_RECONNECT_BACKOFF_FACTOR");
        reconnect_backoff_factor != nullptr && reconnect_backoff_factor[0] != '\0') {
        const auto parsed = parse_u64_arg("HUGIN_RECONNECT_BACKOFF_FACTOR", reconnect_backoff_factor);
        if (!parsed.has_value() || *parsed > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
            std::cerr << "Invalid value for HUGIN_RECONNECT_BACKOFF_FACTOR\n";
            return 1;
        }
        muninn_options.reconnect_backoff_factor = static_cast<std::uint32_t>(*parsed);
    }
    if (const char* reconnect_jitter_percent = std::getenv("HUGIN_RECONNECT_JITTER_PERCENT");
        reconnect_jitter_percent != nullptr && reconnect_jitter_percent[0] != '\0') {
        const auto parsed = parse_u64_arg("HUGIN_RECONNECT_JITTER_PERCENT", reconnect_jitter_percent);
        if (!parsed.has_value() || *parsed > 100U) {
            std::cerr << "Invalid value for HUGIN_RECONNECT_JITTER_PERCENT (expected 0-100)\n";
            return 1;
        }
        muninn_options.reconnect_jitter_percent = static_cast<std::uint32_t>(*parsed);
    }

    hugin::api::FrontendApiServerOptions api_options;
    if (const char* secret = std::getenv("HUGIN_JWT_SECRET"); secret != nullptr && secret[0] != '\0') {
        api_options.jwt_secret = secret;
    }
    if (const char* issuer = std::getenv("HUGIN_JWT_ISSUER"); issuer != nullptr && issuer[0] != '\0') {
        api_options.jwt_issuer = issuer;
    }
    if (const char* audience = std::getenv("HUGIN_JWT_AUDIENCE"); audience != nullptr && audience[0] != '\0') {
        api_options.jwt_audience = audience;
    }
    if (const char* local_access_token = std::getenv("HUGIN_LOCAL_ACCESS_TOKEN");
        local_access_token != nullptr && local_access_token[0] != '\0') {
        api_options.local_access_token = local_access_token;
    }
    if (const char* max_body_bytes = std::getenv("HUGIN_MAX_HTTP_REQUEST_BODY_BYTES");
        max_body_bytes != nullptr && max_body_bytes[0] != '\0') {
        const auto parsed = parse_size_arg("HUGIN_MAX_HTTP_REQUEST_BODY_BYTES", max_body_bytes);
        if (!parsed.has_value()) {
            return 1;
        }
        api_options.max_http_request_body_bytes = *parsed;
    }
    if (const char* allowed_read_root = std::getenv("HUGIN_ALLOWED_READ_ROOT");
        allowed_read_root != nullptr && allowed_read_root[0] != '\0') {
        api_options.allowed_read_root = allowed_read_root;
    }
    if (const char* allowed_write_root = std::getenv("HUGIN_ALLOWED_WRITE_ROOT");
        allowed_write_root != nullptr && allowed_write_root[0] != '\0') {
        api_options.allowed_write_root = allowed_write_root;
    }
    if (const char* max_upload_file_bytes = std::getenv("HUGIN_MAX_UPLOAD_FILE_BYTES");
        max_upload_file_bytes != nullptr && max_upload_file_bytes[0] != '\0') {
        const auto parsed = parse_size_arg("HUGIN_MAX_UPLOAD_FILE_BYTES", max_upload_file_bytes);
        if (!parsed.has_value()) {
            return 1;
        }
        api_options.max_upload_file_bytes = *parsed;
    }
    if (const char* max_upload_directory_files = std::getenv("HUGIN_MAX_UPLOAD_DIRECTORY_FILES");
        max_upload_directory_files != nullptr && max_upload_directory_files[0] != '\0') {
        const auto parsed = parse_size_arg("HUGIN_MAX_UPLOAD_DIRECTORY_FILES", max_upload_directory_files);
        if (!parsed.has_value()) {
            return 1;
        }
        api_options.max_upload_directory_files = *parsed;
    }
    if (const char* max_upload_total_bytes = std::getenv("HUGIN_MAX_UPLOAD_TOTAL_DECODED_BYTES");
        max_upload_total_bytes != nullptr && max_upload_total_bytes[0] != '\0') {
        const auto parsed = parse_size_arg("HUGIN_MAX_UPLOAD_TOTAL_DECODED_BYTES", max_upload_total_bytes);
        if (!parsed.has_value()) {
            return 1;
        }
        api_options.max_upload_total_decoded_bytes = *parsed;
    }

    bool start_muninn_client = true;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        auto require_next = [&](const std::string_view flag) -> std::optional<std::string> {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << flag << '\n';
                return std::nullopt;
            }
            return std::string(argv[++i]);
        };

        if (arg == "--io-threads") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            const auto parsed = parse_size_arg(arg, *value);
            if (!parsed.has_value()) {
                return 1;
            }
            io_threads = std::max<std::size_t>(1U, *parsed);
        } else if (arg == "--disable-muninn-client") {
            start_muninn_client = false;
        } else if (arg == "--host-id") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            muninn_options.host_id = *value;
        } else if (arg == "--client-instance-id") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            muninn_options.client_instance_id = *value;
        } else if (arg == "--muninn-host") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            muninn_options.remote_host = *value;
        } else if (arg == "--muninn-port") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            const auto parsed = parse_u16_arg(arg, *value);
            if (!parsed.has_value()) {
                return 1;
            }
            muninn_options.remote_port = *parsed;
        } else if (arg == "--muninn-path") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            muninn_options.websocket_path = *value;
        } else if (arg == "--muninn-auth-token") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            muninn_options.auth_token = *value;
        } else if (arg == "--poll-ms") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            const auto parsed = parse_u64_arg(arg, *value);
            if (!parsed.has_value()) {
                return 1;
            }
            muninn_options.idle_pull_interval = std::chrono::milliseconds(*parsed);
        } else if (arg == "--hot-poll-ms") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            const auto parsed = parse_u64_arg(arg, *value);
            if (!parsed.has_value()) {
                return 1;
            }
            muninn_options.hot_pull_interval = std::chrono::milliseconds(*parsed);
        } else if (arg == "--reconnect-ms") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            const auto parsed = parse_u64_arg(arg, *value);
            if (!parsed.has_value()) {
                return 1;
            }
            muninn_options.reconnect_delay = std::chrono::milliseconds(*parsed);
        } else if (arg == "--reconnect-max-ms") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            const auto parsed = parse_u64_arg(arg, *value);
            if (!parsed.has_value()) {
                return 1;
            }
            muninn_options.reconnect_max_delay = std::chrono::milliseconds(*parsed);
        } else if (arg == "--reconnect-backoff-factor") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            const auto parsed = parse_u64_arg(arg, *value);
            if (!parsed.has_value() || *parsed > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
                std::cerr << "Invalid value for " << arg << '\n';
                return 1;
            }
            muninn_options.reconnect_backoff_factor = static_cast<std::uint32_t>(*parsed);
        } else if (arg == "--reconnect-jitter-percent") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            const auto parsed = parse_u64_arg(arg, *value);
            if (!parsed.has_value() || *parsed > 100U) {
                std::cerr << "Invalid value for " << arg << " (expected 0-100)\n";
                return 1;
            }
            muninn_options.reconnect_jitter_percent = static_cast<std::uint32_t>(*parsed);
        } else if (arg == "--outbox-batch") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            const auto parsed = parse_size_arg(arg, *value);
            if (!parsed.has_value()) {
                return 1;
            }
            muninn_options.outbox_batch_size =
                std::max<std::size_t>(1U, *parsed);
        } else if (arg == "--disable-auto-pull") {
            muninn_options.auto_pull_enabled = false;
        } else if (arg == "--api-address") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            api_options.listen_address = *value;
        } else if (arg == "--api-port") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            const auto parsed = parse_u16_arg(arg, *value);
            if (!parsed.has_value()) {
                return 1;
            }
            api_options.port = *parsed;
        } else if (arg == "--api-health-path") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            api_options.health_path = *value;
        } else if (arg == "--api-command-path") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            api_options.command_path = *value;
        } else if (arg == "--api-stream-path") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            api_options.stream_path = *value;
        } else if (arg == "--api-openapi-path") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            api_options.openapi_path = *value;
        } else if (arg == "--api-docs-path") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            api_options.docs_path = *value;
        } else if (arg == "--local-access-token") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            api_options.local_access_token = *value;
        } else if (arg == "--max-http-request-body-bytes") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            const auto parsed = parse_size_arg(arg, *value);
            if (!parsed.has_value()) {
                return 1;
            }
            api_options.max_http_request_body_bytes = *parsed;
        } else if (arg == "--api-realtime-tree-path") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            api_options.realtime_tree_path = *value;
        } else if (arg == "--api-realtime-snapshot-path") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            api_options.realtime_snapshot_path = *value;
        } else if (arg == "--api-file-download-path") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            api_options.file_download_path = *value;
        } else if (arg == "--api-file-upload-path") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            api_options.file_upload_path = *value;
        } else if (arg == "--api-directory-upload-path") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            api_options.directory_upload_path = *value;
        } else if (arg == "--allowed-read-root") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            api_options.allowed_read_root = *value;
        } else if (arg == "--allowed-write-root") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            api_options.allowed_write_root = *value;
        } else if (arg == "--directory-upload-threads") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            const auto parsed = parse_size_arg(arg, *value);
            if (!parsed.has_value()) {
                return 1;
            }
            api_options.directory_upload_threads =
                std::min<std::size_t>(4U, std::max<std::size_t>(1U, *parsed));
        } else if (arg == "--max-upload-file-bytes") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            const auto parsed = parse_size_arg(arg, *value);
            if (!parsed.has_value()) {
                return 1;
            }
            api_options.max_upload_file_bytes = *parsed;
        } else if (arg == "--max-upload-directory-files") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            const auto parsed = parse_size_arg(arg, *value);
            if (!parsed.has_value()) {
                return 1;
            }
            api_options.max_upload_directory_files = *parsed;
        } else if (arg == "--max-upload-total-decoded-bytes") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            const auto parsed = parse_size_arg(arg, *value);
            if (!parsed.has_value()) {
                return 1;
            }
            api_options.max_upload_total_decoded_bytes = *parsed;
        } else if (arg == "--disable-jwt") {
            api_options.require_jwt = false;
        } else if (arg == "--enable-jwt") {
            api_options.require_jwt = true;
        } else if (arg == "--jwt-secret") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            api_options.jwt_secret = *value;
        } else if (arg == "--jwt-issuer") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            api_options.jwt_issuer = *value;
        } else if (arg == "--jwt-audience") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            api_options.jwt_audience = *value;
        } else if (arg == "--jwt-clock-skew-sec") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            const auto parsed = parse_u64_arg(arg, *value);
            if (!parsed.has_value()) {
                return 1;
            }
            api_options.jwt_clock_skew_seconds = *parsed;
        } else if (arg == "--jwt-no-exp-required") {
            api_options.jwt_require_exp_claim = false;
        } else if (arg == "--jwt-exp-required") {
            api_options.jwt_require_exp_claim = true;
        } else if (arg == "--ws-max-pending-messages") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            const auto parsed = parse_size_arg(arg, *value);
            if (!parsed.has_value()) {
                return 1;
            }
            api_options.ws_max_pending_messages =
                std::max<std::size_t>(1U, *parsed);
        } else if (arg == "--ws-max-pending-bytes") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            const auto parsed = parse_size_arg(arg, *value);
            if (!parsed.has_value()) {
                return 1;
            }
            api_options.ws_max_pending_bytes =
                std::max<std::size_t>(1U, *parsed);
        } else if (arg == "--ws-backpressure-policy") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            const auto policy = parse_backpressure_policy(*value);
            if (!policy.has_value()) {
                std::cerr << "Invalid --ws-backpressure-policy, expected drop_oldest|drop_newest|disconnect\n";
                return 1;
            }
            api_options.ws_backpressure_policy = *policy;
        } else if (arg == "--command-worker-threads") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            const auto parsed = parse_size_arg(arg, *value);
            if (!parsed.has_value()) {
                return 1;
            }
            api_options.command_worker_threads =
                std::max<std::size_t>(1U, *parsed);
        } else if (arg == "--command-max-queue-per-worker") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            const auto parsed = parse_size_arg(arg, *value);
            if (!parsed.has_value()) {
                return 1;
            }
            api_options.command_max_queue_per_worker =
                std::max<std::size_t>(1U, *parsed);
        } else if (arg == "--command-timeout-ms") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            const auto parsed = parse_u64_arg(arg, *value);
            if (!parsed.has_value()) {
                return 1;
            }
            api_options.command_execution_timeout = std::chrono::milliseconds(*parsed);
        }
    }

    boost::asio::io_context io_context(static_cast<int>(io_threads));
    auto guard = boost::asio::make_work_guard(io_context);

    std::shared_ptr<hugin::api::FrontendApiServer> api_server;
    std::shared_ptr<hugin::net::MuninnTransportClient> muninn_client;
    hugin::ingest::MessageValidator message_validator;
    hugin::sinks::MaterializerSink materializer_sink;

    auto on_crud_result = [&api_server, &message_validator, &materializer_sink](
                              const backend::shared::crud::CrudResultMessage& result) {
        if (api_server) {
            api_server->broadcast_crud_result(result);
        }

        for (const auto& record : result.records) {
            hugin::protocol::MuninnFrame frame;
            if (record.record_type == 1U) {
                frame.type = hugin::protocol::MuninnRecordType::Snapshot;
            } else if (record.record_type == 2U) {
                frame.type = hugin::protocol::MuninnRecordType::Delta;
            } else {
                continue;
            }

            frame.created_at_unix_seconds = record.created_at;
            frame.routing_key = record.routing_key;
            const auto payload = backend::shared::crud::base64_decode(record.payload_b64);
            if (!payload.has_value()) {
                std::cerr << "[hugin.materializer] failed to decode payload_b64 sequence=" << record.sequence << '\n';
                continue;
            }
            frame.payload = std::move(*payload);

            const auto validated = message_validator.validate_and_decode(frame, {});
            if (!validated.ok()) {
                std::cerr << "[hugin.materializer] validation failed sequence=" << record.sequence
                          << " message=" << validated.status.message << '\n';
                continue;
            }

            hugin::common::Status apply_status = hugin::common::Status::success();
            if (validated.value.snapshot.has_value()) {
                apply_status = materializer_sink.apply_snapshot(
                    validated.value.snapshot->host_identifier,
                    record.sequence,
                    *validated.value.snapshot
                );
            } else if (validated.value.delta.has_value()) {
                apply_status = materializer_sink.apply_delta("", record.sequence, *validated.value.delta);
            }

            if (!apply_status.ok()) {
                std::cerr << "[hugin.materializer] apply failed sequence=" << record.sequence
                          << " message=" << apply_status.message << '\n';
            }
        }
    };

    hugin::api::RealtimeTreeHandler realtime_tree_handler = [&materializer_sink](
                                                                     const std::string_view requested_host,
                                                                     const std::string_view requested_path)
        -> hugin::common::StatusOr<nlohmann::json> {
        std::string host_id(requested_host);
        if (host_id.empty()) {
            const auto latest_host = materializer_sink.latest_host_id();
            if (!latest_host.ok()) {
                return hugin::common::StatusOr<nlohmann::json>::failure(
                    latest_host.status.code,
                    latest_host.status.message
                );
            }
            host_id = latest_host.value;
        }

        std::string path = std::string(requested_path);
        if (path.empty()) {
            const auto resolved_root = materializer_sink.root_path(host_id);
            if (!resolved_root.ok()) {
                return hugin::common::StatusOr<nlohmann::json>::failure(
                    resolved_root.status.code,
                    resolved_root.status.message
                );
            }
            path = resolved_root.value;
        }
        const auto children = materializer_sink.list_children(host_id, path);
        if (!children.ok()) {
            return hugin::common::StatusOr<nlohmann::json>::failure(
                children.status.code,
                children.status.message
            );
        }

        const auto snapshot_id = materializer_sink.last_snapshot_id(host_id);
        if (!snapshot_id.ok()) {
            return hugin::common::StatusOr<nlohmann::json>::failure(
                snapshot_id.status.code,
                snapshot_id.status.message
            );
        }

        nlohmann::json response = nlohmann::json::object();
        response["ok"] = true;
        response["host_id"] = host_id;
        response["path"] = path;
        response["snapshot_id"] = snapshot_id.value;

        nlohmann::json entries = nlohmann::json::array();
        for (const auto& entry : children.value) {
            entries.push_back(
                {
                    {"id", entry.id},
                    {"parent_id", entry.parent_id},
                    {"path", entry.normalized_path},
                    {"name", entry.name},
                    {"type", entry_type_text(entry.type)},
                    {"size_bytes", entry.metadata.size_bytes},
                    {"created_at", entry.metadata.created_at},
                    {"modified_at", entry.metadata.modified_at},
                    {"accessed_at", entry.metadata.accessed_at},
                    {"permissions", entry.metadata.permissions}
                }
            );
        }
        response["entries"] = std::move(entries);
        return hugin::common::StatusOr<nlohmann::json>::success(std::move(response));
    };

    hugin::api::RealtimeSnapshotHandler realtime_snapshot_handler = [&materializer_sink](
                                                                            const std::string_view requested_host,
                                                                            const std::string_view requested_path,
                                                                            const bool include_entries)
        -> hugin::common::StatusOr<nlohmann::json> {
        std::string host_id(requested_host);
        if (host_id.empty()) {
            const auto latest_host = materializer_sink.latest_host_id();
            if (!latest_host.ok()) {
                return hugin::common::StatusOr<nlohmann::json>::failure(
                    latest_host.status.code,
                    latest_host.status.message
                );
            }
            host_id = latest_host.value;
        }

        const auto snapshot_id = materializer_sink.last_snapshot_id(host_id);
        if (!snapshot_id.ok()) {
            return hugin::common::StatusOr<nlohmann::json>::failure(
                snapshot_id.status.code,
                snapshot_id.status.message
            );
        }

        const auto last_sequence = materializer_sink.last_sequence(host_id);
        if (!last_sequence.ok()) {
            return hugin::common::StatusOr<nlohmann::json>::failure(
                last_sequence.status.code,
                last_sequence.status.message
            );
        }

        const auto entry_count = materializer_sink.entry_count(host_id);
        if (!entry_count.ok()) {
            return hugin::common::StatusOr<nlohmann::json>::failure(
                entry_count.status.code,
                entry_count.status.message
            );
        }

        std::string path = std::string(requested_path);
        if (path.empty()) {
            const auto resolved_root = materializer_sink.root_path(host_id);
            if (!resolved_root.ok()) {
                return hugin::common::StatusOr<nlohmann::json>::failure(
                    resolved_root.status.code,
                    resolved_root.status.message
                );
            }
            path = resolved_root.value;
        }

        nlohmann::json response = nlohmann::json::object();
        response["ok"] = true;
        response["host_id"] = host_id;
        response["snapshot_id"] = snapshot_id.value;
        response["last_sequence"] = last_sequence.value;
        response["entry_count"] = entry_count.value;
        response["path"] = path;
        response["include_entries"] = include_entries;

        if (include_entries) {
            const auto children = materializer_sink.list_children(host_id, path);
            if (!children.ok()) {
                return hugin::common::StatusOr<nlohmann::json>::failure(
                    children.status.code,
                    children.status.message
                );
            }

            nlohmann::json entries = nlohmann::json::array();
            for (const auto& entry : children.value) {
                entries.push_back(
                    {
                        {"id", entry.id},
                        {"parent_id", entry.parent_id},
                        {"path", entry.normalized_path},
                        {"name", entry.name},
                        {"type", entry_type_text(entry.type)},
                        {"size_bytes", entry.metadata.size_bytes},
                        {"created_at", entry.metadata.created_at},
                        {"modified_at", entry.metadata.modified_at},
                        {"accessed_at", entry.metadata.accessed_at},
                        {"permissions", entry.metadata.permissions}
                    }
                );
            }
            response["entries"] = std::move(entries);
        }

        return hugin::common::StatusOr<nlohmann::json>::success(std::move(response));
    };

    auto on_connection_state = [](const bool connected, const std::string_view message) {
        std::cout << "[muninn] connected=" << (connected ? "true" : "false")
                  << " state=" << message << '\n';
    };

    if (start_muninn_client) {
        muninn_client = hugin::net::create_muninn_transport_client(
            io_context,
            muninn_options,
            on_crud_result,
            on_connection_state
        );
    }

    hugin::api::CommandHandler command_handler = [muninn_client](
                                                         const backend::shared::crud::CrudCommandMessage& command)
        -> hugin::common::Status {
        if (!muninn_client) {
            return hugin::common::Status::failure(
                hugin::common::ErrorCode::Internal,
                "Muninn bridge client is disabled."
            );
        }
        return muninn_client->submit_command(command);
    };

    api_server = std::make_shared<hugin::api::FrontendApiServer>(
        io_context,
        api_options,
        std::move(command_handler),
        std::move(realtime_tree_handler),
        std::move(realtime_snapshot_handler)
    );

    const auto api_start = api_server->start();
    if (!api_start.ok()) {
        std::cerr << "Failed to start frontend API server: " << api_start.message << '\n';
        return 1;
    }

    if (muninn_client) {
        muninn_client->start();
        if (muninn_options.auto_pull_enabled) {
            (void)muninn_client->request_read(0U);
        }
    }

    std::vector<std::thread> workers;
    workers.reserve(io_threads);
    for (std::size_t i = 0; i < io_threads; ++i) {
        workers.emplace_back([&io_context]() {
            io_context.run();
        });
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::cout << "Hugin gateway initialized.\n";
    std::cout << "  api=http://" << api_options.listen_address << ':' << api_options.port << '\n';
    std::cout << "  health_path=" << api_options.health_path << '\n';
    std::cout << "  command_path=" << api_options.command_path << '\n';
    std::cout << "  stream_path=" << api_options.stream_path << '\n';
    std::cout << "  openapi_path=" << api_options.openapi_path << '\n';
    std::cout << "  docs_path=" << api_options.docs_path << '\n';
    std::cout << "  realtime_tree_path=" << api_options.realtime_tree_path << '\n';
    std::cout << "  realtime_snapshot_path=" << api_options.realtime_snapshot_path << '\n';
    std::cout << "  file_download_path=" << api_options.file_download_path << '\n';
    std::cout << "  file_upload_path=" << api_options.file_upload_path << '\n';
    std::cout << "  directory_upload_path=" << api_options.directory_upload_path << '\n';
    std::cout << "  local_access_token_configured=" << (!api_options.local_access_token.empty() ? "true" : "false") << '\n';
    std::cout << "  max_http_request_body_bytes=" << api_options.max_http_request_body_bytes << '\n';
    std::cout << "  allowed_read_root="
              << (api_options.allowed_read_root.empty() ? "<unrestricted>" : api_options.allowed_read_root.string())
              << '\n';
    std::cout << "  allowed_write_root="
              << (api_options.allowed_write_root.empty() ? "<unrestricted>" : api_options.allowed_write_root.string())
              << '\n';
    std::cout << "  directory_upload_threads=" << api_options.directory_upload_threads << '\n';
    std::cout << "  max_upload_file_bytes=" << api_options.max_upload_file_bytes << '\n';
    std::cout << "  max_upload_directory_files=" << api_options.max_upload_directory_files << '\n';
    std::cout << "  max_upload_total_decoded_bytes=" << api_options.max_upload_total_decoded_bytes << '\n';
    std::cout << "  jwt_required=" << (api_options.require_jwt ? "true" : "false") << '\n';
    std::cout << "  ws_backpressure=" << backpressure_policy_to_text(api_options.ws_backpressure_policy) << '\n';
    std::cout << "  ws_max_pending_messages=" << api_options.ws_max_pending_messages << '\n';
    std::cout << "  ws_max_pending_bytes=" << api_options.ws_max_pending_bytes << '\n';
    std::cout << "  command_worker_threads=" << api_options.command_worker_threads << '\n';
    std::cout << "  command_queue_per_worker=" << api_options.command_max_queue_per_worker << '\n';
    std::cout << "  command_timeout_ms=" << api_options.command_execution_timeout.count() << '\n';
    std::cout << "  io_threads=" << io_threads << '\n';
    std::cout << "  muninn_bridge=" << (muninn_client ? "enabled" : "disabled") << '\n';
    if (muninn_client) {
        std::cout << "  muninn_transport="
                  << hugin::net::muninn_transport_kind_name(muninn_options.transport_kind) << '\n';
        std::cout << "  muninn_endpoint=" << muninn_options.remote_host << ':'
                  << muninn_options.remote_port << muninn_options.websocket_path << '\n';
        std::cout << "  host_id=" << muninn_options.host_id << '\n';
        std::cout << "  bridge_auth_token_configured="
                  << (muninn_options.auth_token.empty() ? "false" : "true") << '\n';
        std::cout << "  auto_pull=" << (muninn_options.auto_pull_enabled ? "true" : "false") << '\n';
        std::cout << "  reconnect_delay_ms=" << muninn_options.reconnect_delay.count() << '\n';
        std::cout << "  reconnect_max_delay_ms=" << muninn_options.reconnect_max_delay.count() << '\n';
        std::cout << "  reconnect_backoff_factor=" << muninn_options.reconnect_backoff_factor << '\n';
        std::cout << "  reconnect_jitter_percent=" << muninn_options.reconnect_jitter_percent << '\n';
    }

    while (!g_stop_requested.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    std::cout << "Hugin shutdown requested.\n";

    if (muninn_client) {
        muninn_client->stop();
    }
    api_server->stop();

    guard.reset();
    io_context.stop();
    for (auto& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    std::cout << "Hugin stopped.\n";
    return 0;
}
