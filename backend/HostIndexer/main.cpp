#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <boost/asio/io_context.hpp>

#include "api/indexing_pipeline.hpp"
#include "domain/validation/validate_snapshot.hpp"
#include "filesystem/filesystem_scanner.hpp"
#include "snapshot/delta_engine.hpp"
#include "snapshot/snapshot_builder.hpp"
#include "storage/lmdb_store.hpp"
#include "transport/ws_crud_server.hpp"

namespace {

std::atomic<bool> g_stop_requested {false};

void signal_handler(int) {
    g_stop_requested.store(true);
}

const char* severity_name(const host_indexer::domain::ValidationSeverity severity) {
    switch (severity) {
        case host_indexer::domain::ValidationSeverity::Info:
            return "Info";
        case host_indexer::domain::ValidationSeverity::Warning:
            return "Warning";
        case host_indexer::domain::ValidationSeverity::Error:
            return "Error";
        case host_indexer::domain::ValidationSeverity::Fatal:
            return "Fatal";
    }
    return "Unknown";
}

void print_validation(const host_indexer::domain::ValidationReport& report, const std::string_view label) {
    std::cout << "\n[" << label << "] validation issues=" << report.issues.size() << '\n';
    for (const auto& issue : report.issues) {
        std::cout << "  - " << severity_name(issue.severity)
                  << " code=" << static_cast<std::uint16_t>(issue.code);
        if (issue.entry_id.has_value()) {
            std::cout << " entry=" << *issue.entry_id;
        }
        if (!issue.path.empty()) {
            std::cout << " path=" << issue.path;
        }
        std::cout << " msg=" << issue.message << '\n';
    }
}

void print_scan_summary(const host_indexer::filesystem::RawScanResult& scan,
                        const host_indexer::snapshot::BuildResult& build,
                        const host_indexer::domain::ValidationReport& validation,
                        const std::string_view label) {
    std::cout << "\n[" << label << "]\n";
    std::cout << "  host=" << scan.host_identifier << '\n';
    std::cout << "  root=" << scan.root_path << '\n';
    std::cout << "  scanned_entries=" << scan.entries.size() << '\n';
    std::cout << "  build_entries=" << build.snapshot.entries.size() << '\n';
    std::cout << "  build_diagnostics=" << build.diagnostics.size() << '\n';
    std::cout << "  validation_issues=" << validation.issues.size() << '\n';
}

void print_store_status(const host_indexer::storage::StoreStatus& status, const std::string_view label) {
    std::cout << "  " << label << ": ";
    if (status.ok()) {
        std::cout << "ok\n";
        return;
    }
    std::cout << "failed code=" << static_cast<std::uint16_t>(status.code)
              << " lmdb_rc=" << status.lmdb_rc
              << " message=" << status.message << '\n';
}

} // namespace

int main(int argc, char** argv) {
    std::cout.setf(std::ios::unitbuf);
    std::cerr.setf(std::ios::unitbuf);

    std::vector<std::string> positional_args;
    std::optional<std::filesystem::path> db_path;
    std::optional<std::filesystem::path> default_root_path;
    bool serve_mode = false;
    std::string listen_address = "0.0.0.0";
    std::uint16_t listen_port = 9460;
    std::string websocket_path = "/hostindexer";
    std::size_t io_threads = std::max<std::size_t>(2U, std::thread::hardware_concurrency());
    std::size_t outbox_batch_size = 64U;
    bool bootstrap_create = true;
    std::string allowed_client_instance_id = "localedge-gateway-1";
    std::string bridge_auth_token {};
    if (const char* allowed_client = std::getenv("HOSTINDEXER_ALLOWED_CLIENT_INSTANCE_ID");
        allowed_client != nullptr && allowed_client[0] != '\0') {
        allowed_client_instance_id = allowed_client;
    }
    if (const char* bridge_token = std::getenv("HOSTINDEXER_BRIDGE_AUTH_TOKEN");
        bridge_token != nullptr && bridge_token[0] != '\0') {
        bridge_auth_token = bridge_token;
    }

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        auto require_next = [&](const std::string_view flag) -> std::optional<std::string> {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << flag << '\n';
                return std::nullopt;
            }
            return std::string(argv[++i]);
        };

        if (arg == "--db") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            db_path = std::filesystem::path(*value);
        } else if (arg == "--serve") {
            serve_mode = true;
        } else if (arg == "--listen-address") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            listen_address = *value;
        } else if (arg == "--listen-port") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            listen_port = static_cast<std::uint16_t>(std::stoul(*value));
        } else if (arg == "--ws-path") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            websocket_path = *value;
        } else if (arg == "--default-root") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            default_root_path = std::filesystem::path(*value);
        } else if (arg == "--io-threads") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            io_threads = std::max<std::size_t>(1U, static_cast<std::size_t>(std::stoull(*value)));
        } else if (arg == "--outbox-batch") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            outbox_batch_size = std::max<std::size_t>(1U, static_cast<std::size_t>(std::stoull(*value)));
        } else if (arg == "--bootstrap-create") {
            bootstrap_create = true;
        } else if (arg == "--disable-bootstrap-create") {
            bootstrap_create = false;
        } else if (arg == "--allowed-client-instance-id") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            allowed_client_instance_id = *value;
        } else if (arg == "--bridge-auth-token") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            bridge_auth_token = *value;
        } else {
            positional_args.emplace_back(argv[i]);
        }
    }

    if (serve_mode) {
        if (!db_path.has_value()) {
            std::cerr << "Serve mode requires --db <path>.\n";
            return 1;
        }

        host_indexer::storage::LmdbStore store;
        host_indexer::storage::LmdbStoreOptions options;
        options.directory = *db_path;
        const auto open_status = store.open(options);
        if (!open_status.ok()) {
            std::cerr << "Failed to open LMDB store: " << open_status.message << '\n';
            return 1;
        }

        host_indexer::api::IndexingPipeline pipeline(store);
        boost::asio::io_context io_context(static_cast<int>(io_threads));

        if (bootstrap_create) {
            if (!default_root_path.has_value()) {
                std::cout << "Bootstrap snapshot skipped: --default-root is not configured.\n";
            } else {
                const auto bootstrap_result = pipeline.capture_snapshot(*default_root_path, {}, true);
                if (!bootstrap_result.ok() || !bootstrap_result.transport_snapshot_status.ok()) {
                    std::string reason = "bootstrap snapshot failed";
                    if (!bootstrap_result.snapshot_store_status.ok()) {
                        reason = bootstrap_result.snapshot_store_status.message;
                    } else if (!bootstrap_result.transport_snapshot_status.ok()) {
                        reason = bootstrap_result.transport_snapshot_status.message;
                    }
                    std::cerr << "Bootstrap snapshot failed: " << reason << '\n';
                } else {
                    std::cout << "Bootstrap snapshot captured snapshot_id="
                              << bootstrap_result.snapshot_build.snapshot.snapshot_id << '\n';
                }
            }
        }

        host_indexer::transport::WebsocketCrudServerOptions server_options;
        server_options.listen_address = listen_address;
        server_options.port = listen_port;
        server_options.websocket_path = websocket_path;
        server_options.default_outbox_batch_size = outbox_batch_size;
        server_options.allowed_client_instance_id = allowed_client_instance_id;
        server_options.bridge_auth_token = bridge_auth_token;
        if (default_root_path.has_value()) {
            server_options.default_root_path = *default_root_path;
        }

        host_indexer::transport::WebsocketCrudServer server(io_context, store, pipeline, server_options);
        const auto server_status = server.start();
        if (!server_status.ok()) {
            std::cerr << "Failed to start websocket CRUD server: " << server_status.message << '\n';
            return 1;
        }

        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);

        std::vector<std::thread> workers;
        workers.reserve(io_threads);
        for (std::size_t i = 0; i < io_threads; ++i) {
            workers.emplace_back([&io_context]() {
                io_context.run();
            });
        }

        std::cout << "HostIndexer websocket CRUD server started.\n";
        std::cout << "  db=" << db_path->string() << '\n';
        std::cout << "  listen=" << listen_address << ':' << listen_port << websocket_path << '\n';
        if (default_root_path.has_value()) {
            std::cout << "  default_root=" << default_root_path->string() << '\n';
        }
        std::cout << "  io_threads=" << io_threads << '\n';
        std::cout << "  outbox_batch=" << outbox_batch_size << '\n';
        std::cout << "  bootstrap_create=" << (bootstrap_create ? "true" : "false") << '\n';
        std::cout << "  allowed_client_instance_id=" << allowed_client_instance_id << '\n';
        std::cout << "  bridge_auth_token_configured=" << (bridge_auth_token.empty() ? "false" : "true") << '\n';

        while (!g_stop_requested.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        std::cout << "HostIndexer shutdown requested.\n";

        server.stop();
        io_context.stop();
        for (auto& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }

        store.close();
        std::cout << "HostIndexer stopped.\n";
        return 0;
    }

    const std::filesystem::path base_path = !positional_args.empty()
        ? std::filesystem::path(positional_args[0])
        : std::filesystem::current_path();

    host_indexer::filesystem::LocalFilesystemScanner scanner;
    host_indexer::snapshot::SnapshotBuilder builder;

    const auto base_scan = scanner.scan(base_path, {});
    const auto base_build = builder.build(base_scan, {});
    const auto base_validation = host_indexer::validation::validate_snapshot(base_build.snapshot);

    print_scan_summary(base_scan, base_build, base_validation, "BASE SNAPSHOT");
    print_validation(base_validation, "BASE SNAPSHOT");

    if (db_path.has_value()) {
        host_indexer::storage::LmdbStore store;
        host_indexer::storage::LmdbStoreOptions options;
        options.directory = *db_path;
        const auto open_status = store.open(options);

        std::cout << "\n[LMDB]\n";
        print_store_status(open_status, "open");
        if (!open_status.ok()) {
            return 1;
        }

        host_indexer::api::IndexingPipeline pipeline(store);
        const auto persisted_base = pipeline.capture_snapshot(base_path, {}, true);
        std::cout << "  persisted_base_snapshot_id=" << persisted_base.snapshot_build.snapshot.snapshot_id << '\n';
        print_store_status(persisted_base.snapshot_store_status, "persist_snapshot");
        print_store_status(persisted_base.transport_snapshot_status, "enqueue_snapshot");
        bool persistence_ok = persisted_base.ok();

        if (positional_args.size() > 1) {
            const std::filesystem::path target_path = std::filesystem::path(positional_args[1]);
            const auto persisted_target = pipeline.capture_snapshot_and_delta(
                target_path,
                persisted_base.snapshot_build.snapshot.snapshot_id,
                {},
                true
            );
            std::cout << "  persisted_target_snapshot_id=" << persisted_target.snapshot_build.snapshot.snapshot_id << '\n';
            print_store_status(persisted_target.snapshot_store_status, "persist_target_snapshot");
            print_store_status(persisted_target.transport_snapshot_status, "enqueue_target_snapshot");
            print_store_status(persisted_target.delta_store_status, "persist_delta");
            print_store_status(persisted_target.transport_delta_status, "enqueue_delta");
            persistence_ok = persistence_ok && persisted_target.ok();
        }

        return persistence_ok ? 0 : 1;
    }

    if (positional_args.size() > 1) {
        const std::filesystem::path target_path = std::filesystem::path(positional_args[1]);
        const auto target_scan = scanner.scan(target_path, {});
        const auto target_build = builder.build(target_scan, {});
        const auto target_validation = host_indexer::validation::validate_snapshot(target_build.snapshot);

        print_scan_summary(target_scan, target_build, target_validation, "TARGET SNAPSHOT");
        print_validation(target_validation, "TARGET SNAPSHOT");

        host_indexer::snapshot::DeltaEngine delta_engine;
        const auto delta = delta_engine.compute(base_build.snapshot, target_build.snapshot, {});
        const auto delta_validation = host_indexer::validation::validate_delta(
            delta,
            &base_build.snapshot,
            &target_build.snapshot
        );

        std::cout << "\n[DELTA]\n";
        std::cout << "  added=" << delta.added_entries.size() << '\n';
        std::cout << "  removed=" << delta.removed_entries.size() << '\n';
        std::cout << "  modified=" << delta.modified_entries.size() << '\n';
        std::cout << "  renamed=" << delta.renamed_entries.size() << '\n';
        print_validation(delta_validation, "DELTA");

        return (base_validation.has_errors() || target_validation.has_errors() || delta_validation.has_errors())
            ? 1
            : 0;
    }

    return base_validation.has_errors() ? 1 : 0;
}
