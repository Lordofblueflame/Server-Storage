#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>

#include "src/api/frontend_api_server.hpp"
#include "src/ingest/message_validator.hpp"
#include "src/net/hostindexer_client.hpp"
#include "src/protocol/hostindexer_frame.hpp"
#include "src/sinks/materializer_sink.hpp"

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

std::optional<localedge::api::BackpressurePolicy> parse_backpressure_policy(const std::string_view value) {
    if (value == "drop_oldest") {
        return localedge::api::BackpressurePolicy::DropOldest;
    }
    if (value == "drop_newest") {
        return localedge::api::BackpressurePolicy::DropNewest;
    }
    if (value == "disconnect") {
        return localedge::api::BackpressurePolicy::Disconnect;
    }
    return std::nullopt;
}

std::string backpressure_policy_to_text(const localedge::api::BackpressurePolicy policy) {
    switch (policy) {
        case localedge::api::BackpressurePolicy::DropOldest:
            return "drop_oldest";
        case localedge::api::BackpressurePolicy::DropNewest:
            return "drop_newest";
        case localedge::api::BackpressurePolicy::Disconnect:
            return "disconnect";
    }
    return "drop_oldest";
}

std::string entry_type_text(const host_indexer::domain::EntryType type) {
    switch (type) {
        case host_indexer::domain::EntryType::File:
            return "file";
        case host_indexer::domain::EntryType::Directory:
            return "directory";
        case host_indexer::domain::EntryType::Symlink:
            return "symlink";
        case host_indexer::domain::EntryType::Special:
            return "special";
    }
    return "special";
}

} // namespace

int main(int argc, char** argv) {
    std::cout.setf(std::ios::unitbuf);
    std::cerr.setf(std::ios::unitbuf);

    std::size_t io_threads = std::max<std::size_t>(2U, std::thread::hardware_concurrency());

    localedge::net::HostIndexerClientOptions hostindexer_options;
    hostindexer_options.host_id = detect_host_id();
    hostindexer_options.client_instance_id = "localedge-gateway-1";
    if (const char* bridge_token = std::getenv("HOSTINDEXER_BRIDGE_AUTH_TOKEN");
        bridge_token != nullptr && bridge_token[0] != '\0') {
        hostindexer_options.auth_token = bridge_token;
    }

    localedge::api::FrontendApiServerOptions api_options;
    if (const char* secret = std::getenv("LOCALEDGE_JWT_SECRET"); secret != nullptr && secret[0] != '\0') {
        api_options.jwt_secret = secret;
    }
    if (const char* issuer = std::getenv("LOCALEDGE_JWT_ISSUER"); issuer != nullptr && issuer[0] != '\0') {
        api_options.jwt_issuer = issuer;
    }
    if (const char* audience = std::getenv("LOCALEDGE_JWT_AUDIENCE"); audience != nullptr && audience[0] != '\0') {
        api_options.jwt_audience = audience;
    }

    bool start_hostindexer_client = true;

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
            io_threads = std::max<std::size_t>(1U, static_cast<std::size_t>(std::stoull(*value)));
        } else if (arg == "--disable-hostindexer-client") {
            start_hostindexer_client = false;
        } else if (arg == "--host-id") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            hostindexer_options.host_id = *value;
        } else if (arg == "--client-instance-id") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            hostindexer_options.client_instance_id = *value;
        } else if (arg == "--hostindexer-host") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            hostindexer_options.remote_host = *value;
        } else if (arg == "--hostindexer-port") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            hostindexer_options.remote_port = static_cast<std::uint16_t>(std::stoul(*value));
        } else if (arg == "--hostindexer-path") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            hostindexer_options.websocket_path = *value;
        } else if (arg == "--hostindexer-auth-token") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            hostindexer_options.auth_token = *value;
        } else if (arg == "--poll-ms") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            hostindexer_options.idle_pull_interval = std::chrono::milliseconds(std::stoll(*value));
        } else if (arg == "--hot-poll-ms") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            hostindexer_options.hot_pull_interval = std::chrono::milliseconds(std::stoll(*value));
        } else if (arg == "--reconnect-ms") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            hostindexer_options.reconnect_delay = std::chrono::milliseconds(std::stoll(*value));
        } else if (arg == "--outbox-batch") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            hostindexer_options.outbox_batch_size =
                std::max<std::size_t>(1U, static_cast<std::size_t>(std::stoull(*value)));
        } else if (arg == "--disable-auto-pull") {
            hostindexer_options.auto_pull_enabled = false;
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
            api_options.port = static_cast<std::uint16_t>(std::stoul(*value));
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
        } else if (arg == "--api-realtime-tree-path") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            api_options.realtime_tree_path = *value;
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
        } else if (arg == "--directory-upload-threads") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            api_options.directory_upload_threads =
                std::min<std::size_t>(4U, std::max<std::size_t>(1U, static_cast<std::size_t>(std::stoull(*value))));
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
            api_options.jwt_clock_skew_seconds =
                static_cast<std::uint64_t>(std::stoull(*value));
        } else if (arg == "--jwt-no-exp-required") {
            api_options.jwt_require_exp_claim = false;
        } else if (arg == "--jwt-exp-required") {
            api_options.jwt_require_exp_claim = true;
        } else if (arg == "--ws-max-pending-messages") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            api_options.ws_max_pending_messages =
                std::max<std::size_t>(1U, static_cast<std::size_t>(std::stoull(*value)));
        } else if (arg == "--ws-max-pending-bytes") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            api_options.ws_max_pending_bytes =
                std::max<std::size_t>(1U, static_cast<std::size_t>(std::stoull(*value)));
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
            api_options.command_worker_threads =
                std::max<std::size_t>(1U, static_cast<std::size_t>(std::stoull(*value)));
        } else if (arg == "--command-max-queue-per-worker") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            api_options.command_max_queue_per_worker =
                std::max<std::size_t>(1U, static_cast<std::size_t>(std::stoull(*value)));
        } else if (arg == "--command-timeout-ms") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            api_options.command_execution_timeout = std::chrono::milliseconds(std::stoll(*value));
        }
    }

    boost::asio::io_context io_context(static_cast<int>(io_threads));
    auto guard = boost::asio::make_work_guard(io_context);

    std::shared_ptr<localedge::api::FrontendApiServer> api_server;
    std::shared_ptr<localedge::net::HostIndexerWebSocketClient> hostindexer_client;
    localedge::ingest::MessageValidator message_validator;
    localedge::sinks::MaterializerSink materializer_sink;

    auto on_crud_result = [&api_server, &message_validator, &materializer_sink](
                              const backend::shared::crud::CrudResultMessage& result) {
        if (api_server) {
            api_server->broadcast_crud_result(result);
        }

        for (const auto& record : result.records) {
            localedge::protocol::HostIndexerFrame frame;
            if (record.record_type == 1U) {
                frame.type = localedge::protocol::HostIndexerRecordType::Snapshot;
            } else if (record.record_type == 2U) {
                frame.type = localedge::protocol::HostIndexerRecordType::Delta;
            } else {
                continue;
            }

            frame.created_at_unix_seconds = record.created_at;
            frame.routing_key = record.routing_key;
            const auto payload = backend::shared::crud::base64_decode(record.payload_b64);
            if (!payload.has_value()) {
                std::cerr << "[localedge.materializer] failed to decode payload_b64 sequence=" << record.sequence << '\n';
                continue;
            }
            frame.payload = std::move(*payload);

            const auto validated = message_validator.validate_and_decode(frame, {});
            if (!validated.ok()) {
                std::cerr << "[localedge.materializer] validation failed sequence=" << record.sequence
                          << " message=" << validated.status.message << '\n';
                continue;
            }

            localedge::common::Status apply_status = localedge::common::Status::success();
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
                std::cerr << "[localedge.materializer] apply failed sequence=" << record.sequence
                          << " message=" << apply_status.message << '\n';
            }
        }
    };

    localedge::api::RealtimeTreeHandler realtime_tree_handler = [&materializer_sink](
                                                                     const std::string_view requested_host,
                                                                     const std::string_view requested_path)
        -> localedge::common::StatusOr<nlohmann::json> {
        std::string host_id(requested_host);
        if (host_id.empty()) {
            const auto latest_host = materializer_sink.latest_host_id();
            if (!latest_host.ok()) {
                return localedge::common::StatusOr<nlohmann::json>::failure(
                    latest_host.status.code,
                    latest_host.status.message
                );
            }
            host_id = latest_host.value;
        }

        const std::string path = requested_path.empty() ? "/" : std::string(requested_path);
        const auto children = materializer_sink.list_children(host_id, path);
        if (!children.ok()) {
            return localedge::common::StatusOr<nlohmann::json>::failure(
                children.status.code,
                children.status.message
            );
        }

        const auto snapshot_id = materializer_sink.last_snapshot_id(host_id);
        if (!snapshot_id.ok()) {
            return localedge::common::StatusOr<nlohmann::json>::failure(
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
        return localedge::common::StatusOr<nlohmann::json>::success(std::move(response));
    };

    auto on_connection_state = [](const bool connected, const std::string_view message) {
        std::cout << "[hostindexer] connected=" << (connected ? "true" : "false")
                  << " state=" << message << '\n';
    };

    if (start_hostindexer_client) {
        hostindexer_client = std::make_shared<localedge::net::HostIndexerWebSocketClient>(
            io_context,
            hostindexer_options,
            on_crud_result,
            on_connection_state
        );
    }

    localedge::api::CommandHandler command_handler = [hostindexer_client](
                                                         const backend::shared::crud::CrudCommandMessage& command)
        -> localedge::common::Status {
        if (!hostindexer_client) {
            return localedge::common::Status::failure(
                localedge::common::ErrorCode::Internal,
                "HostIndexer bridge client is disabled."
            );
        }
        return hostindexer_client->submit_command(command);
    };

    api_server = std::make_shared<localedge::api::FrontendApiServer>(
        io_context,
        api_options,
        std::move(command_handler),
        std::move(realtime_tree_handler)
    );

    const auto api_start = api_server->start();
    if (!api_start.ok()) {
        std::cerr << "Failed to start frontend API server: " << api_start.message << '\n';
        return 1;
    }

    if (hostindexer_client) {
        hostindexer_client->start();
        if (hostindexer_options.auto_pull_enabled) {
            (void)hostindexer_client->request_read(0U);
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

    std::cout << "LocalEdgeServer gateway initialized.\n";
    std::cout << "  api=http://" << api_options.listen_address << ':' << api_options.port << '\n';
    std::cout << "  health_path=" << api_options.health_path << '\n';
    std::cout << "  command_path=" << api_options.command_path << '\n';
    std::cout << "  stream_path=" << api_options.stream_path << '\n';
    std::cout << "  openapi_path=" << api_options.openapi_path << '\n';
    std::cout << "  docs_path=" << api_options.docs_path << '\n';
    std::cout << "  realtime_tree_path=" << api_options.realtime_tree_path << '\n';
    std::cout << "  file_download_path=" << api_options.file_download_path << '\n';
    std::cout << "  file_upload_path=" << api_options.file_upload_path << '\n';
    std::cout << "  directory_upload_path=" << api_options.directory_upload_path << '\n';
    std::cout << "  directory_upload_threads=" << api_options.directory_upload_threads << '\n';
    std::cout << "  jwt_required=" << (api_options.require_jwt ? "true" : "false") << '\n';
    std::cout << "  ws_backpressure=" << backpressure_policy_to_text(api_options.ws_backpressure_policy) << '\n';
    std::cout << "  ws_max_pending_messages=" << api_options.ws_max_pending_messages << '\n';
    std::cout << "  ws_max_pending_bytes=" << api_options.ws_max_pending_bytes << '\n';
    std::cout << "  command_worker_threads=" << api_options.command_worker_threads << '\n';
    std::cout << "  command_queue_per_worker=" << api_options.command_max_queue_per_worker << '\n';
    std::cout << "  command_timeout_ms=" << api_options.command_execution_timeout.count() << '\n';
    std::cout << "  io_threads=" << io_threads << '\n';
    std::cout << "  hostindexer_bridge=" << (hostindexer_client ? "enabled" : "disabled") << '\n';
    if (hostindexer_client) {
        std::cout << "  hostindexer_endpoint=" << hostindexer_options.remote_host << ':'
                  << hostindexer_options.remote_port << hostindexer_options.websocket_path << '\n';
        std::cout << "  host_id=" << hostindexer_options.host_id << '\n';
        std::cout << "  bridge_auth_token_configured="
                  << (hostindexer_options.auth_token.empty() ? "false" : "true") << '\n';
        std::cout << "  auto_pull=" << (hostindexer_options.auto_pull_enabled ? "true" : "false") << '\n';
    }

    while (!g_stop_requested.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    std::cout << "LocalEdgeServer shutdown requested.\n";

    if (hostindexer_client) {
        hostindexer_client->stop();
    }
    api_server->stop();

    guard.reset();
    io_context.stop();
    for (auto& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    std::cout << "LocalEdgeServer stopped.\n";
    return 0;
}
