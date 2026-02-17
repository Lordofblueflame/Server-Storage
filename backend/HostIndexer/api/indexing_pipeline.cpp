#include "indexing_pipeline.hpp"

#include "../domain/validation/validate_snapshot.hpp"

namespace host_indexer::api {

IndexingPipeline::IndexingPipeline(storage::LmdbStore& store)
    : store_(&store) {
}

PipelineResult IndexingPipeline::capture_snapshot(const std::filesystem::path& root_path,
                                                  const filesystem::ScanOptions& scan_options,
                                                  const bool enqueue_transport) {
    PipelineResult result;
    result.snapshot_store_status = storage::StoreStatus::success();
    result.delta_store_status = storage::StoreStatus::success();
    result.transport_snapshot_status = storage::StoreStatus::success();
    result.transport_delta_status = storage::StoreStatus::success();
    result.raw_scan = scanner_.scan(root_path, scan_options);
    result.snapshot_build = snapshot_builder_.build(result.raw_scan, {});
    result.snapshot_validation = validation::validate_snapshot(result.snapshot_build.snapshot);

    if (!result.snapshot_validation.ok()) {
        const auto validation_failure = storage::StoreStatus::failure(
            storage::StoreErrorCode::ValidationFailure,
            "Snapshot validation failed before persistence."
        );
        result.snapshot_store_status = validation_failure;
        if (enqueue_transport) {
            result.transport_snapshot_status = validation_failure;
        }
        return result;
    }

    const auto store_status = store_->put_snapshot_with_transport(
        result.snapshot_build.snapshot,
        enqueue_transport,
        false,
        nullptr
    );
    if (!store_status.ok()) {
        result.snapshot_store_status = store_status;
        if (enqueue_transport) {
            result.transport_snapshot_status = store_status;
        }
        return result;
    }

    return result;
}

PipelineResult IndexingPipeline::capture_snapshot_and_delta(const std::filesystem::path& root_path,
                                                            const domain::SnapshotId base_snapshot_id,
                                                            const filesystem::ScanOptions& scan_options,
                                                            const bool enqueue_transport) {
    PipelineResult result;
    result.snapshot_store_status = storage::StoreStatus::success();
    result.delta_store_status = storage::StoreStatus::success();
    result.transport_snapshot_status = storage::StoreStatus::success();
    result.transport_delta_status = storage::StoreStatus::success();
    result.raw_scan = scanner_.scan(root_path, scan_options);
    result.snapshot_build = snapshot_builder_.build(result.raw_scan, {});
    result.snapshot_validation = validation::validate_snapshot(result.snapshot_build.snapshot);

    if (!result.snapshot_validation.ok()) {
        const auto validation_failure = storage::StoreStatus::failure(
            storage::StoreErrorCode::ValidationFailure,
            "Snapshot validation failed before persistence."
        );
        result.snapshot_store_status = validation_failure;
        result.delta_store_status = validation_failure;
        if (enqueue_transport) {
            result.transport_snapshot_status = validation_failure;
            result.transport_delta_status = validation_failure;
        }
        return result;
    }

    domain::Snapshot base_snapshot;
    const auto base_status = store_->get_snapshot(base_snapshot_id, base_snapshot, true);
    if (!base_status.ok()) {
        result.delta_store_status = base_status;
        if (enqueue_transport) {
            result.transport_delta_status = base_status;
        }
        return result;
    }

    result.base_snapshot = base_snapshot;
    result.computed_delta = delta_engine_.compute(base_snapshot, result.snapshot_build.snapshot, {});
    result.delta_validation = validation::validate_delta(
        *result.computed_delta,
        &base_snapshot,
        &result.snapshot_build.snapshot
    );

    if (!result.delta_validation.ok()) {
        const auto validation_failure = storage::StoreStatus::failure(
            storage::StoreErrorCode::ValidationFailure,
            "Delta validation failed before persistence."
        );
        result.delta_store_status = validation_failure;
        if (enqueue_transport) {
            result.transport_delta_status = validation_failure;
        }
        return result;
    }

    const auto store_status = store_->put_snapshot_and_delta_with_transport(
        result.snapshot_build.snapshot,
        *result.computed_delta,
        &base_snapshot,
        enqueue_transport,
        false
    );
    if (!store_status.ok()) {
        result.snapshot_store_status = store_status;
        result.delta_store_status = store_status;
        if (enqueue_transport) {
            result.transport_snapshot_status = store_status;
            result.transport_delta_status = store_status;
        }
        return result;
    }

    return result;
}

} // namespace host_indexer::api
