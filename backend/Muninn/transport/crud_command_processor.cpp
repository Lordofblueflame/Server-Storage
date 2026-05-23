#include "crud_command_processor.hpp"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "../../shared/service_log.hpp"

namespace muninn::transport {
namespace {

namespace crud = backend::shared::crud;
namespace logging = backend::shared::logging;

const logging::Logger& crud_logger() {
    static const logging::Logger logger("muninn.crud");
    return logger;
}

void log_command(const logging::LogLevel level,
                 const crud::CrudCommandMessage& command,
                 const std::string_view consumer_id,
                 const std::string_view message) {
    std::ostringstream text;
    text << "consumer=" << consumer_id
         << " request_id=" << command.request_id
         << " op=" << crud::operation_to_string(command.operation)
         << ' ' << message;

    switch (level) {
        case logging::LogLevel::Info:
            crud_logger().info("command", text.str());
            return;
        case logging::LogLevel::Warning:
            crud_logger().warn("command", text.str());
            return;
        case logging::LogLevel::Error:
            crud_logger().error("command", text.str());
            return;
    }
}

std::optional<std::filesystem::path> resolve_root_path(const std::string_view command_root,
                                                       const CrudServerOptions& options) {
    if (!command_root.empty()) {
        return std::filesystem::path(std::string(command_root));
    }
    if (!options.default_root_path.empty()) {
        return options.default_root_path;
    }
    return std::nullopt;
}

storage::StoreStatus append_transport_records(storage::LmdbStore& store,
                                              std::string_view consumer_id,
                                              const std::size_t max_items,
                                              std::vector<crud::TransportRecordMessage>& out_records) {
    std::vector<storage::TransportRecord> records;
    const auto fetch_status = store.fetch_transport_batch_for_consumer(
        consumer_id,
        std::max<std::size_t>(1U, max_items),
        records
    );
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

bool remote_mutating_commands_allowed(const CrudServerOptions& options,
                                      const crud::CrudCommandMessage& command,
                                      std::string& out_message) {
    if (options.allow_remote_mutating_commands || command.operation == crud::Operation::Read) {
        return true;
    }

    std::ostringstream text;
    text << crud::operation_to_string(command.operation)
         << " is disabled on the Muninn bridge. Muninn owns capture and persistence; remote bridge commands are read/ack only by default.";
    out_message = text.str();
    return false;
}

} // namespace

CrudCommandProcessor::CrudCommandProcessor(const CrudServerOptions& options,
                                           storage::LmdbStore& store,
                                           api::IndexingPipeline& pipeline,
                                           std::mutex& pipeline_mutex)
    : options_(options),
      store_(&store),
      pipeline_(&pipeline),
      pipeline_mutex_(&pipeline_mutex) {
}

backend::shared::crud::CrudResultMessage CrudCommandProcessor::process(const crud::CrudCommandMessage& command,
                                                                       const std::string_view consumer_id) const {
    crud::CrudResultMessage result;
    result.request_id = command.request_id;
    result.ok = false;
    const std::size_t batch_size = command.outbox_batch_size == 0U
        ? std::max<std::size_t>(1U, options_.default_outbox_batch_size)
        : static_cast<std::size_t>(command.outbox_batch_size);

    if (command.ack_sequence > 0U) {
        const auto ack_status = store_->ack_transport_until_for_consumer(consumer_id, command.ack_sequence);
        if (!ack_status.ok()) {
            result.message = ack_status.message;
            log_command(
                logging::LogLevel::Error,
                command,
                consumer_id,
                std::string("ack failed: ") + result.message
            );
            return result;
        }

        const auto compact_status = store_->compact_transport_up_to_min_acked();
        if (!compact_status.ok()) {
            result.message = compact_status.message;
            log_command(
                logging::LogLevel::Error,
                command,
                consumer_id,
                std::string("compaction failed: ") + result.message
            );
            return result;
        }
    }

    if (std::string disabled_message; !remote_mutating_commands_allowed(options_, command, disabled_message)) {
        result.message = std::move(disabled_message);
        log_command(logging::LogLevel::Warning, command, consumer_id, result.message);
        return result;
    }

    switch (command.operation) {
        case crud::Operation::Read: {
            const auto fetch_status = append_transport_records(
                *store_,
                consumer_id,
                batch_size,
                result.records
            );
            if (!fetch_status.ok()) {
                result.message = fetch_status.message;
                log_command(
                    logging::LogLevel::Error,
                    command,
                    consumer_id,
                    std::string("read failed while fetching outbox records: ") + result.message
                );
                return result;
            }
            result.ok = true;
            result.message = "ok";
            {
                std::ostringstream text;
                text << "read succeeded: returned_records=" << result.records.size()
                     << " ack_sequence=" << command.ack_sequence;
                log_command(logging::LogLevel::Info, command, consumer_id, text.str());
            }
            return result;
        }

        case crud::Operation::Create: {
            const auto root_path = resolve_root_path(command.root_path, options_);
            if (!root_path.has_value()) {
                result.message = "Create operation requires root_path.";
                log_command(logging::LogLevel::Warning, command, consumer_id, result.message);
                return result;
            }

            api::PipelineResult pipeline_result;
            {
                std::lock_guard<std::mutex> lock(*pipeline_mutex_);
                pipeline_result = pipeline_->capture_snapshot(*root_path, options_.scan_options, true);
            }

            if (!pipeline_result.ok() || !pipeline_result.transport_snapshot_status.ok()) {
                result.message = pipeline_error_message(pipeline_result);
                log_command(
                    logging::LogLevel::Error,
                    command,
                    consumer_id,
                    std::string("create failed: ") + result.message
                );
                return result;
            }

            if (options_.retention_keep_latest_snapshots > 0U) {
                storage::RetentionCleanupStats retention_stats;
                const auto retention_status = store_->apply_snapshot_retention(
                    options_.retention_keep_latest_snapshots,
                    &retention_stats
                );
                if (!retention_status.ok()) {
                    result.message = retention_status.message;
                    log_command(
                        logging::LogLevel::Error,
                        command,
                        consumer_id,
                        std::string("retention failed: ") + result.message
                    );
                    return result;
                }
            }

            result.snapshot_id = pipeline_result.snapshot_build.snapshot.snapshot_id;
            const auto fetch_status = append_transport_records(
                *store_,
                consumer_id,
                batch_size,
                result.records
            );
            if (!fetch_status.ok()) {
                result.message = fetch_status.message;
                log_command(
                    logging::LogLevel::Error,
                    command,
                    consumer_id,
                    std::string("create succeeded but outbox fetch failed: ") + result.message
                );
                return result;
            }

            result.ok = true;
            result.message = "snapshot_created";
            {
                std::ostringstream text;
                text << "create succeeded: snapshot_id=" << result.snapshot_id
                     << " entries=" << pipeline_result.snapshot_build.snapshot.entries.size()
                     << " returned_records=" << result.records.size();
                log_command(logging::LogLevel::Info, command, consumer_id, text.str());
            }
            return result;
        }

        case crud::Operation::Update: {
            const auto root_path = resolve_root_path(command.root_path, options_);
            if (!root_path.has_value()) {
                result.message = "Update operation requires root_path.";
                log_command(logging::LogLevel::Warning, command, consumer_id, result.message);
                return result;
            }

            const std::uint64_t base_snapshot = command.base_snapshot_id != 0U
                ? command.base_snapshot_id
                : command.snapshot_id;
            if (base_snapshot == 0U) {
                result.message = "Update operation requires base_snapshot_id (or snapshot_id).";
                log_command(logging::LogLevel::Warning, command, consumer_id, result.message);
                return result;
            }

            api::PipelineResult pipeline_result;
            {
                std::lock_guard<std::mutex> lock(*pipeline_mutex_);
                pipeline_result = pipeline_->capture_snapshot_and_delta(
                    *root_path,
                    base_snapshot,
                    options_.scan_options,
                    true
                );
            }

            if (!pipeline_result.ok() ||
                !pipeline_result.transport_snapshot_status.ok() ||
                !pipeline_result.transport_delta_status.ok()) {
                result.message = pipeline_error_message(pipeline_result);
                log_command(
                    logging::LogLevel::Error,
                    command,
                    consumer_id,
                    std::string("update failed: ") + result.message
                );
                return result;
            }

            if (options_.retention_keep_latest_snapshots > 0U) {
                storage::RetentionCleanupStats retention_stats;
                const auto retention_status = store_->apply_snapshot_retention(
                    options_.retention_keep_latest_snapshots,
                    &retention_stats
                );
                if (!retention_status.ok()) {
                    result.message = retention_status.message;
                    log_command(
                        logging::LogLevel::Error,
                        command,
                        consumer_id,
                        std::string("retention failed: ") + result.message
                    );
                    return result;
                }
            }

            result.base_snapshot_id = base_snapshot;
            result.target_snapshot_id = pipeline_result.snapshot_build.snapshot.snapshot_id;
            result.snapshot_id = result.target_snapshot_id;

            const auto fetch_status = append_transport_records(
                *store_,
                consumer_id,
                batch_size,
                result.records
            );
            if (!fetch_status.ok()) {
                result.message = fetch_status.message;
                log_command(
                    logging::LogLevel::Error,
                    command,
                    consumer_id,
                    std::string("update succeeded but outbox fetch failed: ") + result.message
                );
                return result;
            }

            result.ok = true;
            result.message = "snapshot_updated";
            {
                const auto& delta = *pipeline_result.computed_delta;
                std::ostringstream text;
                text << "update succeeded: base_snapshot_id=" << result.base_snapshot_id
                     << " target_snapshot_id=" << result.target_snapshot_id
                     << " delta_added=" << delta.added_entries.size()
                     << " delta_removed=" << delta.removed_entries.size()
                     << " delta_modified=" << delta.modified_entries.size()
                     << " delta_renamed=" << delta.renamed_entries.size()
                     << " returned_records=" << result.records.size();
                log_command(logging::LogLevel::Info, command, consumer_id, text.str());
            }
            return result;
        }

        case crud::Operation::Delete: {
            const std::uint64_t snapshot_id = command.snapshot_id;
            if (snapshot_id == 0U) {
                result.message = "Delete operation requires snapshot_id.";
                log_command(logging::LogLevel::Warning, command, consumer_id, result.message);
                return result;
            }

            storage::SnapshotDeleteStats delete_stats;
            const auto delete_status = store_->delete_snapshot(snapshot_id, &delete_stats);
            if (!delete_status.ok()) {
                result.message = delete_status.message;
                log_command(
                    logging::LogLevel::Error,
                    command,
                    consumer_id,
                    std::string("delete failed: ") + result.message
                );
                return result;
            }

            result.ok = true;
            result.snapshot_id = snapshot_id;
            result.message = "snapshot_deleted";
            {
                std::ostringstream text;
                text << "delete succeeded: snapshot_id=" << snapshot_id
                     << " removed_snapshots=" << delete_stats.snapshots_removed
                     << " removed_deltas=" << delete_stats.deltas_removed;
                log_command(logging::LogLevel::Info, command, consumer_id, text.str());
            }
            return result;
        }
    }

    result.message = "Unsupported CRUD operation.";
    log_command(logging::LogLevel::Warning, command, consumer_id, result.message);
    return result;
}

} // namespace muninn::transport
