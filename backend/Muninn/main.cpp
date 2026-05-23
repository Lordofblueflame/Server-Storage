#include <algorithm>
#include <atomic>
#include <chrono>
#include <charconv>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
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
#include "transport/crud_server.hpp"

namespace {

std::atomic<bool> g_stop_requested {false};

void signal_handler(int) {
    g_stop_requested.store(true);
}

const char* severity_name(const muninn::domain::ValidationSeverity severity) {
    switch (severity) {
        case muninn::domain::ValidationSeverity::Info:
            return "Info";
        case muninn::domain::ValidationSeverity::Warning:
            return "Warning";
        case muninn::domain::ValidationSeverity::Error:
            return "Error";
        case muninn::domain::ValidationSeverity::Fatal:
            return "Fatal";
    }
    return "Unknown";
}

void print_validation(const muninn::domain::ValidationReport& report, const std::string_view label) {
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

void print_scan_summary(const muninn::filesystem::RawScanResult& scan,
                        const muninn::snapshot::BuildResult& build,
                        const muninn::domain::ValidationReport& validation,
                        const std::string_view label) {
    std::cout << "\n[" << label << "]\n";
    std::cout << "  host=" << scan.host_identifier << '\n';
    std::cout << "  root=" << scan.root_path << '\n';
    std::cout << "  scanned_entries=" << scan.entries.size() << '\n';
    std::cout << "  build_entries=" << build.snapshot.entries.size() << '\n';
    std::cout << "  build_diagnostics=" << build.diagnostics.size() << '\n';
    std::cout << "  validation_issues=" << validation.issues.size() << '\n';
}

void print_store_status(const muninn::storage::StoreStatus& status, const std::string_view label) {
    std::cout << "  " << label << ": ";
    if (status.ok()) {
        std::cout << "ok\n";
        return;
    }
    std::cout << "failed code=" << static_cast<std::uint16_t>(status.code)
              << " lmdb_rc=" << status.lmdb_rc
              << " message=" << status.message << '\n';
}

std::string describe_pipeline_failure(const muninn::api::PipelineResult& pipeline_result) {
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

std::size_t count_delta_changes(const std::optional<muninn::domain::DeltaSnapshot>& delta) {
    if (!delta.has_value()) {
        return 0U;
    }

    return delta->added_entries.size() +
           delta->removed_entries.size() +
           delta->modified_entries.size() +
           delta->renamed_entries.size();
}

void print_usage(const char* program_name) {
    std::cout << "Usage:\n";
    std::cout << "  " << program_name << " [rootPath] [targetPath] [--db <lmdbDir>]\n";
    std::cout << "  " << program_name
              << " --core-debug --db <lmdbDir> --default-root <rootPath>\n";
    std::cout << "               [--watch-interval-seconds <n>] [--stop-after-watch-cycles <n>]\n";
    std::cout << "               [--metrics-log-interval-seconds <n>]\n";
    std::cout << "               [--scan-follow-symlinks] [--scan-max-entries <n>]\n";
    std::cout << "               [--scan-include <glob>]... [--scan-exclude <glob>]...\n";
    std::cout << "               [--retention-keep-snapshots <n>]\n";
    std::cout << "               [--bootstrap-create|--disable-bootstrap-create]\n";
    std::cout << "  " << program_name
              << " --serve --db <lmdbDir> [--listen-address <addr>] [--listen-port <port>] [--ws-path <path>]\n";
    std::cout << "               [--default-root <rootPath>] [--outbox-batch <n>] [--io-threads <n>]\n";
    std::cout << "               [--metrics-log-interval-seconds <n>]\n";
    std::cout << "               [--watch-interval-seconds <n>]\n";
    std::cout << "               [--scan-follow-symlinks] [--scan-max-entries <n>]\n";
    std::cout << "               [--scan-include <glob>]... [--scan-exclude <glob>]...\n";
    std::cout << "               [--retention-keep-snapshots <n>]\n";
    std::cout << "               [--security-mode <dev|prod>]\n";
    std::cout << "               [--allow-plain-websocket-in-prod]\n";
    std::cout << "               [--allow-remote-mutating-commands]\n";
    std::cout << "               [--bootstrap-create|--disable-bootstrap-create]\n";
    std::cout << "               [--allowed-client-instance-id <id>] [--bridge-auth-token <token>]\n";
    std::cout << "               (HTTP endpoints: /healthz, /readyz, /metrics)\n";
    std::cout << "  " << program_name << " --help\n";
}

std::optional<std::uint64_t> parse_u64_arg(const std::string_view flag, const std::string& value) {
    std::uint64_t parsed = 0;
    const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (ec != std::errc {} || ptr != value.data() + value.size()) {
        std::cerr << "Invalid numeric value for " << flag << ": " << value << '\n';
        return std::nullopt;
    }
    return parsed;
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

std::optional<muninn::transport::TransportSecurityMode> parse_security_mode_arg(
    const std::string_view flag,
    const std::string& value) {
    if (value == "dev") {
        return muninn::transport::TransportSecurityMode::Dev;
    }
    if (value == "prod") {
        return muninn::transport::TransportSecurityMode::Prod;
    }

    std::cerr << "Invalid value for " << flag << ": " << value << " (expected dev or prod)\n";
    return std::nullopt;
}

std::string_view security_mode_to_string(const muninn::transport::TransportSecurityMode mode) {
    switch (mode) {
        case muninn::transport::TransportSecurityMode::Dev:
            return "dev";
        case muninn::transport::TransportSecurityMode::Prod:
            return "prod";
    }
    return "dev";
}

std::optional<bool> parse_bool_env(const std::string_view env_name, const std::string& value) {
    if (value == "1" || value == "true" || value == "TRUE" || value == "yes" || value == "YES") {
        return true;
    }
    if (value == "0" || value == "false" || value == "FALSE" || value == "no" || value == "NO") {
        return false;
    }

    std::cerr << "Invalid value for " << env_name << ": " << value << " (expected boolean)\n";
    return std::nullopt;
}

} // namespace

int main(int argc, char** argv) {
    std::cout.setf(std::ios::unitbuf);
    std::cerr.setf(std::ios::unitbuf);

    std::vector<std::string> positional_args;
    std::optional<std::filesystem::path> db_path;
    std::optional<std::filesystem::path> default_root_path;
    bool serve_mode = false;
    bool core_debug_mode = false;
    std::string listen_address = "0.0.0.0";
    std::uint16_t listen_port = 9460;
    std::string websocket_path = "/muninn";
    std::size_t io_threads = std::max<std::size_t>(2U, std::thread::hardware_concurrency());
    std::size_t outbox_batch_size = 64U;
    std::size_t metrics_log_interval_seconds = 0U;
    std::size_t watch_interval_seconds = 0U;
    std::size_t stop_after_watch_cycles = 0U;
    bool watch_interval_explicitly_set = false;
    bool scan_follow_symlinks = false;
    std::size_t scan_max_entries = 5'000'000;
    std::vector<std::string> scan_include_globs;
    std::vector<std::string> scan_exclude_globs;
    std::size_t retention_keep_latest_snapshots = 0U;
    bool bootstrap_create = true;
    muninn::transport::TransportSecurityMode security_mode = muninn::transport::TransportSecurityMode::Dev;
    bool allow_plain_websocket_in_prod = false;
    bool allow_remote_mutating_commands = false;
    std::string allowed_client_instance_id = "hugin-gateway-1";
    std::string bridge_auth_token {};
    if (const char* mode_value = std::getenv("MUNINN_SECURITY_MODE");
        mode_value != nullptr && mode_value[0] != '\0') {
        const auto parsed_mode = parse_security_mode_arg("MUNINN_SECURITY_MODE", mode_value);
        if (!parsed_mode.has_value()) {
            return 1;
        }
        security_mode = *parsed_mode;
    }
    if (const char* allow_plain_ws = std::getenv("MUNINN_ALLOW_PLAIN_WEBSOCKET_IN_PROD");
        allow_plain_ws != nullptr && allow_plain_ws[0] != '\0') {
        const auto parsed_bool = parse_bool_env("MUNINN_ALLOW_PLAIN_WEBSOCKET_IN_PROD", allow_plain_ws);
        if (!parsed_bool.has_value()) {
            return 1;
        }
        allow_plain_websocket_in_prod = *parsed_bool;
    }
    if (const char* allow_remote_mutations = std::getenv("MUNINN_ALLOW_REMOTE_MUTATING_COMMANDS");
        allow_remote_mutations != nullptr && allow_remote_mutations[0] != '\0') {
        const auto parsed_bool = parse_bool_env(
            "MUNINN_ALLOW_REMOTE_MUTATING_COMMANDS",
            allow_remote_mutations
        );
        if (!parsed_bool.has_value()) {
            return 1;
        }
        allow_remote_mutating_commands = *parsed_bool;
    }
    if (const char* retention_keep = std::getenv("MUNINN_RETENTION_KEEP_SNAPSHOTS");
        retention_keep != nullptr && retention_keep[0] != '\0') {
        const auto parsed_retention = parse_size_arg("MUNINN_RETENTION_KEEP_SNAPSHOTS", retention_keep);
        if (!parsed_retention.has_value()) {
            return 1;
        }
        retention_keep_latest_snapshots = *parsed_retention;
    }
    if (const char* metrics_interval = std::getenv("MUNINN_METRICS_LOG_INTERVAL_SECONDS");
        metrics_interval != nullptr && metrics_interval[0] != '\0') {
        const auto parsed_metrics_interval = parse_size_arg("MUNINN_METRICS_LOG_INTERVAL_SECONDS", metrics_interval);
        if (!parsed_metrics_interval.has_value()) {
            return 1;
        }
        metrics_log_interval_seconds = *parsed_metrics_interval;
    }
    if (const char* watch_interval = std::getenv("MUNINN_WATCH_INTERVAL_SECONDS");
        watch_interval != nullptr && watch_interval[0] != '\0') {
        const auto parsed_watch_interval = parse_size_arg("MUNINN_WATCH_INTERVAL_SECONDS", watch_interval);
        if (!parsed_watch_interval.has_value()) {
            return 1;
        }
        watch_interval_seconds = *parsed_watch_interval;
        watch_interval_explicitly_set = true;
    }
    if (const char* allowed_client = std::getenv("MUNINN_ALLOWED_CLIENT_INSTANCE_ID");
        allowed_client != nullptr && allowed_client[0] != '\0') {
        allowed_client_instance_id = allowed_client;
    }
    if (const char* bridge_token = std::getenv("MUNINN_BRIDGE_AUTH_TOKEN");
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
        } else if (arg == "--core-debug") {
            core_debug_mode = true;
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
            const auto parsed_port = parse_u16_arg(arg, *value);
            if (!parsed_port.has_value()) {
                return 1;
            }
            listen_port = *parsed_port;
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
            const auto parsed_threads = parse_size_arg(arg, *value);
            if (!parsed_threads.has_value()) {
                return 1;
            }
            io_threads = std::max<std::size_t>(1U, *parsed_threads);
        } else if (arg == "--outbox-batch") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            const auto parsed_batch = parse_size_arg(arg, *value);
            if (!parsed_batch.has_value()) {
                return 1;
            }
            outbox_batch_size = std::max<std::size_t>(1U, *parsed_batch);
        } else if (arg == "--metrics-log-interval-seconds") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            const auto parsed_metrics_interval = parse_size_arg(arg, *value);
            if (!parsed_metrics_interval.has_value()) {
                return 1;
            }
            metrics_log_interval_seconds = *parsed_metrics_interval;
        } else if (arg == "--watch-interval-seconds") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            const auto parsed_watch_interval = parse_size_arg(arg, *value);
            if (!parsed_watch_interval.has_value()) {
                return 1;
            }
            watch_interval_seconds = *parsed_watch_interval;
            watch_interval_explicitly_set = true;
        } else if (arg == "--stop-after-watch-cycles") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            const auto parsed_cycles = parse_size_arg(arg, *value);
            if (!parsed_cycles.has_value()) {
                return 1;
            }
            stop_after_watch_cycles = *parsed_cycles;
        } else if (arg == "--scan-follow-symlinks") {
            scan_follow_symlinks = true;
        } else if (arg == "--scan-max-entries") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            const auto parsed_max_entries = parse_size_arg(arg, *value);
            if (!parsed_max_entries.has_value()) {
                return 1;
            }
            scan_max_entries = std::max<std::size_t>(1U, *parsed_max_entries);
        } else if (arg == "--scan-include") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            scan_include_globs.push_back(*value);
        } else if (arg == "--scan-exclude") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            scan_exclude_globs.push_back(*value);
        } else if (arg == "--retention-keep-snapshots") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            const auto parsed_retention = parse_size_arg(arg, *value);
            if (!parsed_retention.has_value()) {
                return 1;
            }
            retention_keep_latest_snapshots = *parsed_retention;
        } else if (arg == "--security-mode") {
            const auto value = require_next(arg);
            if (!value.has_value()) {
                return 1;
            }
            const auto parsed_mode = parse_security_mode_arg(arg, *value);
            if (!parsed_mode.has_value()) {
                return 1;
            }
            security_mode = *parsed_mode;
        } else if (arg == "--allow-plain-websocket-in-prod") {
            allow_plain_websocket_in_prod = true;
        } else if (arg == "--allow-remote-mutating-commands") {
            allow_remote_mutating_commands = true;
        } else if (arg == "--bootstrap-create") {
            bootstrap_create = true;
        } else if (arg == "--disable-bootstrap-create") {
            bootstrap_create = false;
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
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
            if (!arg.empty() && arg.front() == '-') {
                std::cerr << "Unknown option: " << arg << '\n';
                print_usage(argv[0]);
                return 1;
            }
            positional_args.emplace_back(argv[i]);
        }
    }

    muninn::filesystem::ScanOptions scan_options;
    scan_options.follow_symlinks = scan_follow_symlinks;
    scan_options.max_entries = scan_max_entries;
    scan_options.include_globs = scan_include_globs;
    scan_options.exclude_globs = scan_exclude_globs;

    if (!watch_interval_explicitly_set && default_root_path.has_value()) {
        watch_interval_seconds = 2U;
    }

    if (serve_mode && core_debug_mode) {
        std::cerr << "--serve and --core-debug are mutually exclusive.\n";
        return 1;
    }

    if (core_debug_mode) {
        if (!db_path.has_value()) {
            std::cerr << "Core debug mode requires --db <path>.\n";
            return 1;
        }
        if (!default_root_path.has_value()) {
            std::cerr << "Core debug mode requires --default-root <path>.\n";
            return 1;
        }

        muninn::storage::LmdbStore store;
        muninn::storage::LmdbStoreOptions options;
        options.directory = *db_path;
        const auto open_status = store.open(options);
        if (!open_status.ok()) {
            std::cerr << "Failed to open LMDB store: " << open_status.message << '\n';
            return 1;
        }

        muninn::api::IndexingPipeline pipeline(store);
        muninn::api::IndexingPipeline watch_pipeline(store);
        muninn::domain::SnapshotId watch_base_snapshot_id = 0U;
        bool bootstrap_ok = true;

        if (bootstrap_create) {
            const auto bootstrap_result = pipeline.capture_snapshot(*default_root_path, scan_options, false);
            if (!bootstrap_result.ok()) {
                std::cerr << "Core debug bootstrap snapshot failed: "
                          << describe_pipeline_failure(bootstrap_result) << '\n';
                bootstrap_ok = false;
            } else {
                watch_base_snapshot_id = bootstrap_result.snapshot_build.snapshot.snapshot_id;
                std::cout << "Core debug bootstrap snapshot captured snapshot_id="
                          << watch_base_snapshot_id << '\n';
            }
        }

        if (retention_keep_latest_snapshots > 0U) {
            muninn::storage::RetentionCleanupStats retention_stats;
            const auto retention_status = store.apply_snapshot_retention(
                retention_keep_latest_snapshots,
                &retention_stats
            );
            if (!retention_status.ok()) {
                std::cerr << "Core debug retention cleanup failed: " << retention_status.message << '\n';
                store.close();
                return 1;
            }
            std::cout << "Core debug retention cleanup applied snapshots_removed="
                      << retention_stats.snapshots_removed
                      << " deltas_removed=" << retention_stats.deltas_removed << '\n';
        }

        const bool watch_enabled = watch_interval_seconds > 0U;
        std::cout << "Muninn core debug mode started.\n";
        std::cout << "  db=" << db_path->string() << '\n';
        std::cout << "  default_root=" << default_root_path->string() << '\n';
        std::cout << "  watch_interval_seconds=" << watch_interval_seconds << '\n';
        std::cout << "  watch_enabled=" << (watch_enabled ? "true" : "false") << '\n';
        std::cout << "  stop_after_watch_cycles=" << stop_after_watch_cycles << '\n';
        std::cout << "  metrics_log_interval_seconds=" << metrics_log_interval_seconds << '\n';
        std::cout << "  scan_follow_symlinks=" << (scan_options.follow_symlinks ? "true" : "false") << '\n';
        std::cout << "  scan_max_entries=" << scan_options.max_entries << '\n';
        std::cout << "  scan_include_globs=" << scan_options.include_globs.size() << '\n';
        std::cout << "  scan_exclude_globs=" << scan_options.exclude_globs.size() << '\n';
        std::cout << "  retention_keep_latest_snapshots=" << retention_keep_latest_snapshots << '\n';

        if (!watch_enabled) {
            muninn::storage::StoreStats stats;
            const auto stats_status = store.get_stats(stats);
            if (stats_status.ok()) {
                std::cout << "[CORE] snapshots=" << stats.snapshots_count
                          << " deltas=" << stats.deltas_count
                          << " outbox=" << stats.transport_outbox_count << '\n';
            } else {
                std::cerr << "[CORE] failed to read stats: " << stats_status.message << '\n';
            }
            store.close();
            return bootstrap_ok ? 0 : 1;
        }

        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);

        auto last_metrics_log_at = std::chrono::steady_clock::now();
        auto last_watch_run_at = std::chrono::steady_clock::now();
        if (watch_base_snapshot_id == 0U) {
            last_watch_run_at -= std::chrono::seconds(watch_interval_seconds);
        }

        std::size_t completed_watch_cycles = 0U;
        while (!g_stop_requested.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            const auto now = std::chrono::steady_clock::now();

            if (now - last_watch_run_at >= std::chrono::seconds(watch_interval_seconds)) {
                bool watch_capture_ok = false;
                if (watch_base_snapshot_id == 0U) {
                    const auto capture = watch_pipeline.capture_snapshot(*default_root_path, scan_options, false);
                    if (capture.ok()) {
                        watch_base_snapshot_id = capture.snapshot_build.snapshot.snapshot_id;
                        watch_capture_ok = true;
                        std::cout << "[CORE-WATCH] full snapshot captured snapshot_id="
                                  << watch_base_snapshot_id << '\n';
                    } else {
                        std::cerr << "[CORE-WATCH] full snapshot failed reason="
                                  << describe_pipeline_failure(capture) << '\n';
                    }
                } else {
                    const auto base_snapshot_id = watch_base_snapshot_id;
                    const auto update = watch_pipeline.capture_snapshot_and_delta(
                        *default_root_path,
                        base_snapshot_id,
                        scan_options,
                        false
                    );
                    if (update.ok()) {
                        watch_base_snapshot_id = update.snapshot_build.snapshot.snapshot_id;
                        watch_capture_ok = true;
                        const auto delta_change_count = count_delta_changes(update.computed_delta);
                        if (delta_change_count > 0U || watch_base_snapshot_id != base_snapshot_id) {
                            std::cout << "[CORE-WATCH] incremental snapshot captured base_snapshot_id="
                                      << base_snapshot_id
                                      << " target_snapshot_id=" << watch_base_snapshot_id
                                      << " delta_changes=" << delta_change_count << '\n';
                        }
                    } else {
                        std::cerr << "[CORE-WATCH] incremental capture failed base_snapshot_id="
                                  << base_snapshot_id
                                  << " reason=" << describe_pipeline_failure(update) << '\n';

                        const bool base_not_found = !update.delta_store_status.ok() &&
                            update.delta_store_status.code == muninn::storage::StoreErrorCode::NotFound;
                        if (base_not_found) {
                            watch_base_snapshot_id = 0U;
                            std::cerr
                                << "[CORE-WATCH] base snapshot not found; full snapshot will be retried on next cycle.\n";
                        }
                    }
                }

                if (watch_capture_ok && retention_keep_latest_snapshots > 0U) {
                    muninn::storage::RetentionCleanupStats retention_stats;
                    const auto retention_status = store.apply_snapshot_retention(
                        retention_keep_latest_snapshots,
                        &retention_stats
                    );
                    if (!retention_status.ok()) {
                        std::cerr << "[CORE-WATCH] retention cleanup failed: " << retention_status.message << '\n';
                    }
                }

                last_watch_run_at = now;
                ++completed_watch_cycles;
                if (stop_after_watch_cycles > 0U && completed_watch_cycles >= stop_after_watch_cycles) {
                    std::cout << "Core debug stop condition reached after "
                              << completed_watch_cycles << " watch cycles.\n";
                    break;
                }
            }

            if (metrics_log_interval_seconds > 0U &&
                now - last_metrics_log_at >= std::chrono::seconds(metrics_log_interval_seconds)) {
                muninn::storage::StoreStats stats;
                const auto stats_status = store.get_stats(stats);
                if (stats_status.ok()) {
                    std::cout << "[CORE-METRICS] snapshots=" << stats.snapshots_count
                              << " deltas=" << stats.deltas_count
                              << " outbox=" << stats.transport_outbox_count
                              << " consumers=" << stats.transport_consumers_count
                              << " next_transport_sequence=" << stats.next_transport_sequence << '\n';
                } else {
                    std::cerr << "[CORE-METRICS] failed to read stats: " << stats_status.message << '\n';
                }
                last_metrics_log_at = now;
            }
        }

        store.close();
        std::cout << "Muninn core debug mode stopped.\n";
        return bootstrap_ok ? 0 : 1;
    }

    if (serve_mode) {
        if (!db_path.has_value()) {
            std::cerr << "Serve mode requires --db <path>.\n";
            return 1;
        }
        if (security_mode == muninn::transport::TransportSecurityMode::Prod &&
            bridge_auth_token.empty()) {
            std::cerr << "Serve mode in prod requires --bridge-auth-token (or MUNINN_BRIDGE_AUTH_TOKEN).\n";
            return 1;
        }
        if (security_mode == muninn::transport::TransportSecurityMode::Prod &&
            !allow_plain_websocket_in_prod) {
            std::cerr
                << "Serve mode in prod requires explicit --allow-plain-websocket-in-prod "
                << "(Muninn expects TLS termination upstream).\n";
            return 1;
        }

        muninn::storage::LmdbStore store;
        muninn::storage::LmdbStoreOptions options;
        options.directory = *db_path;
        const auto open_status = store.open(options);
        if (!open_status.ok()) {
            std::cerr << "Failed to open LMDB store: " << open_status.message << '\n';
            return 1;
        }

        muninn::api::IndexingPipeline pipeline(store);
        muninn::api::IndexingPipeline watch_pipeline(store);
        boost::asio::io_context io_context(static_cast<int>(io_threads));
        muninn::domain::SnapshotId watch_base_snapshot_id = 0U;

        if (bootstrap_create) {
            if (!default_root_path.has_value()) {
                std::cout << "Bootstrap snapshot skipped: --default-root is not configured.\n";
            } else {
                const auto bootstrap_result = pipeline.capture_snapshot(*default_root_path, scan_options, true);
                if (!bootstrap_result.ok() || !bootstrap_result.transport_snapshot_status.ok()) {
                    std::cerr << "Bootstrap snapshot failed: "
                              << describe_pipeline_failure(bootstrap_result) << '\n';
                } else {
                    watch_base_snapshot_id = bootstrap_result.snapshot_build.snapshot.snapshot_id;
                    std::cout << "Bootstrap snapshot captured snapshot_id="
                              << watch_base_snapshot_id << '\n';
                }
            }
        }
        if (retention_keep_latest_snapshots > 0U) {
            muninn::storage::RetentionCleanupStats retention_stats;
            const auto retention_status = store.apply_snapshot_retention(
                retention_keep_latest_snapshots,
                &retention_stats
            );
            if (!retention_status.ok()) {
                std::cerr << "Retention cleanup failed: " << retention_status.message << '\n';
                return 1;
            }
            std::cout << "Retention cleanup applied snapshots_removed=" << retention_stats.snapshots_removed
                      << " deltas_removed=" << retention_stats.deltas_removed << '\n';
        }

        bool watch_enabled = watch_interval_seconds > 0U;
        if (watch_enabled && !default_root_path.has_value()) {
            std::cerr << "Watch mode disabled: --watch-interval-seconds requires --default-root.\n";
            watch_enabled = false;
        }

        muninn::transport::CrudServerOptions server_options;
        server_options.listen_address = listen_address;
        server_options.port = listen_port;
        server_options.websocket_path = websocket_path;
        server_options.default_outbox_batch_size = outbox_batch_size;
        server_options.scan_options = scan_options;
        server_options.retention_keep_latest_snapshots = retention_keep_latest_snapshots;
        server_options.security_mode = security_mode;
        server_options.allow_plain_websocket_in_prod = allow_plain_websocket_in_prod;
        server_options.allow_remote_mutating_commands = allow_remote_mutating_commands;
        server_options.allowed_client_instance_id = allowed_client_instance_id;
        server_options.bridge_auth_token = bridge_auth_token;
        if (default_root_path.has_value()) {
            server_options.default_root_path = *default_root_path;
        }

        auto server = muninn::transport::create_crud_transport_server(
            io_context,
            store,
            pipeline,
            server_options
        );
        const auto server_status = server->start();
        if (!server_status.ok()) {
            std::cerr << "Failed to start CRUD transport server: " << server_status.message << '\n';
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

        std::cout << "Muninn CRUD transport server started"
                  << " transport=" << muninn::transport::crud_transport_kind_name(server_options.transport_kind)
                  << " listen=" << listen_address << ':' << listen_port << websocket_path
                  << " db=" << db_path->string()
                  << " default_root=" << (default_root_path.has_value() ? default_root_path->string() : "<none>")
                  << " watch=" << (watch_enabled ? std::to_string(watch_interval_seconds) + "s" : "disabled")
                  << " io_threads=" << io_threads
                  << " security_mode=" << security_mode_to_string(security_mode)
                  << " bridge_auth_token_configured=" << (bridge_auth_token.empty() ? "false" : "true")
                  << '\n';

        auto last_metrics_log_at = std::chrono::steady_clock::now();
        auto last_watch_run_at = std::chrono::steady_clock::now();
        if (watch_enabled && watch_base_snapshot_id == 0U) {
            last_watch_run_at -= std::chrono::seconds(watch_interval_seconds);
        }
        while (!g_stop_requested.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            const auto now = std::chrono::steady_clock::now();

            if (watch_enabled && now - last_watch_run_at >= std::chrono::seconds(watch_interval_seconds)) {
                bool watch_capture_ok = false;
                if (watch_base_snapshot_id == 0U) {
                    const auto capture = watch_pipeline.capture_snapshot(*default_root_path, scan_options, true);
                    if (capture.ok() && capture.transport_snapshot_status.ok()) {
                        watch_base_snapshot_id = capture.snapshot_build.snapshot.snapshot_id;
                        watch_capture_ok = true;
                        std::cout << "[WATCH] full snapshot captured snapshot_id=" << watch_base_snapshot_id << '\n';
                    } else {
                        std::cerr << "[WATCH] full snapshot failed reason="
                                  << describe_pipeline_failure(capture) << '\n';
                    }
                } else {
                    const auto base_snapshot_id = watch_base_snapshot_id;
                    const auto update = watch_pipeline.capture_snapshot_and_delta(
                        *default_root_path,
                        base_snapshot_id,
                        scan_options,
                        true
                    );
                    if (update.ok() &&
                        update.transport_snapshot_status.ok() &&
                        update.transport_delta_status.ok()) {
                        watch_base_snapshot_id = update.snapshot_build.snapshot.snapshot_id;
                        watch_capture_ok = true;
                        const auto delta_change_count = count_delta_changes(update.computed_delta);
                        if (delta_change_count > 0U || watch_base_snapshot_id != base_snapshot_id) {
                            std::cout << "[WATCH] incremental snapshot captured base_snapshot_id="
                                      << base_snapshot_id
                                      << " target_snapshot_id=" << watch_base_snapshot_id
                                      << " delta_changes=" << delta_change_count << '\n';
                        }
                    } else {
                        std::cerr << "[WATCH] incremental capture failed base_snapshot_id=" << base_snapshot_id
                                  << " reason=" << describe_pipeline_failure(update) << '\n';

                        const bool base_not_found = !update.delta_store_status.ok() &&
                            update.delta_store_status.code == muninn::storage::StoreErrorCode::NotFound;
                        if (base_not_found) {
                            watch_base_snapshot_id = 0U;
                            std::cerr << "[WATCH] base snapshot not found; full snapshot will be retried on next cycle.\n";
                        }
                    }
                }

                if (watch_capture_ok && retention_keep_latest_snapshots > 0U) {
                    muninn::storage::RetentionCleanupStats retention_stats;
                    const auto retention_status = store.apply_snapshot_retention(
                        retention_keep_latest_snapshots,
                        &retention_stats
                    );
                    if (!retention_status.ok()) {
                        std::cerr << "[WATCH] retention cleanup failed: " << retention_status.message << '\n';
                    }
                }

                last_watch_run_at = now;
            }

            if (metrics_log_interval_seconds > 0U &&
                now - last_metrics_log_at >= std::chrono::seconds(metrics_log_interval_seconds)) {
                muninn::storage::StoreStats stats;
                const auto stats_status = store.get_stats(stats);
                if (stats_status.ok()) {
                    std::cout << "[METRICS] snapshots=" << stats.snapshots_count
                              << " deltas=" << stats.deltas_count
                              << " outbox=" << stats.transport_outbox_count
                              << " consumers=" << stats.transport_consumers_count
                              << " next_transport_sequence=" << stats.next_transport_sequence << '\n';
                } else {
                    std::cerr << "[METRICS] failed to read stats: " << stats_status.message << '\n';
                }
                last_metrics_log_at = now;
            }
        }
        std::cout << "Muninn shutdown requested.\n";

        server->stop();
        io_context.stop();
        for (auto& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }

        store.close();
        std::cout << "Muninn stopped.\n";
        return 0;
    }

    const std::filesystem::path base_path = !positional_args.empty()
        ? std::filesystem::path(positional_args[0])
        : std::filesystem::current_path();

    muninn::filesystem::LocalFilesystemScanner scanner;
    muninn::snapshot::SnapshotBuilder builder;

    const auto base_scan = scanner.scan(base_path, scan_options);
    const auto base_build = builder.build(base_scan, {});
    const auto base_validation = muninn::validation::validate_snapshot(base_build.snapshot);

    print_scan_summary(base_scan, base_build, base_validation, "BASE SNAPSHOT");
    print_validation(base_validation, "BASE SNAPSHOT");

    if (db_path.has_value()) {
        muninn::storage::LmdbStore store;
        muninn::storage::LmdbStoreOptions options;
        options.directory = *db_path;
        const auto open_status = store.open(options);

        std::cout << "\n[LMDB]\n";
        print_store_status(open_status, "open");
        if (!open_status.ok()) {
            return 1;
        }

        muninn::api::IndexingPipeline pipeline(store);
        const auto persisted_base = pipeline.capture_snapshot(base_path, scan_options, true);
        std::cout << "  persisted_base_snapshot_id=" << persisted_base.snapshot_build.snapshot.snapshot_id << '\n';
        print_store_status(persisted_base.snapshot_store_status, "persist_snapshot");
        print_store_status(persisted_base.transport_snapshot_status, "enqueue_snapshot");
        bool persistence_ok = persisted_base.ok();

        if (positional_args.size() > 1) {
            const std::filesystem::path target_path = std::filesystem::path(positional_args[1]);
            const auto persisted_target = pipeline.capture_snapshot_and_delta(
                target_path,
                persisted_base.snapshot_build.snapshot.snapshot_id,
                scan_options,
                true
            );
            std::cout << "  persisted_target_snapshot_id=" << persisted_target.snapshot_build.snapshot.snapshot_id << '\n';
            print_store_status(persisted_target.snapshot_store_status, "persist_target_snapshot");
            print_store_status(persisted_target.transport_snapshot_status, "enqueue_target_snapshot");
            print_store_status(persisted_target.delta_store_status, "persist_delta");
            print_store_status(persisted_target.transport_delta_status, "enqueue_delta");
            persistence_ok = persistence_ok && persisted_target.ok();
        }

        if (retention_keep_latest_snapshots > 0U) {
            muninn::storage::RetentionCleanupStats retention_stats;
            const auto retention_status = store.apply_snapshot_retention(
                retention_keep_latest_snapshots,
                &retention_stats
            );
            std::cout << "  retention_keep_latest_snapshots=" << retention_keep_latest_snapshots << '\n';
            print_store_status(retention_status, "retention_cleanup");
            if (retention_status.ok()) {
                std::cout << "  retention_snapshots_removed=" << retention_stats.snapshots_removed << '\n';
                std::cout << "  retention_deltas_removed=" << retention_stats.deltas_removed << '\n';
            } else {
                persistence_ok = false;
            }
        }

        return persistence_ok ? 0 : 1;
    }

    if (positional_args.size() > 1) {
        const std::filesystem::path target_path = std::filesystem::path(positional_args[1]);
        const auto target_scan = scanner.scan(target_path, scan_options);
        const auto target_build = builder.build(target_scan, {});
        const auto target_validation = muninn::validation::validate_snapshot(target_build.snapshot);

        print_scan_summary(target_scan, target_build, target_validation, "TARGET SNAPSHOT");
        print_validation(target_validation, "TARGET SNAPSHOT");

        muninn::snapshot::DeltaEngine delta_engine;
        const auto delta = delta_engine.compute(base_build.snapshot, target_build.snapshot, {});
        const auto delta_validation = muninn::validation::validate_delta(
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
