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
    result.raw_scan = scanner_.scan(root_path, scan_options);
    result.snapshot_build = snapshot_builder_.build(result.raw_scan, {});
    result.snapshot_validation = validation::validate_snapshot(result.snapshot_build.snapshot);

    if (!result.snapshot_validation.ok()) {
        result.snapshot_store_status = storage::StoreStatus::failure(
            storage::StoreErrorCode::ValidationFailure,
            "Snapshot validation failed before persistence."
        );
        return result;
    }

    result.snapshot_store_status = store_->put_snapshot(result.snapshot_build.snapshot, false);
    if (!result.snapshot_store_status.ok()) {
        return result;
    }

    if (enqueue_transport) {
        result.transport_snapshot_status = store_->enqueue_snapshot(result.snapshot_build.snapshot, false, nullptr);
    } else {
        result.transport_snapshot_status = storage::StoreStatus::success();
    }

    result.delta_store_status = storage::StoreStatus::success();
    result.transport_delta_status = storage::StoreStatus::success();
    return result;
}

PipelineResult IndexingPipeline::capture_snapshot_and_delta(const std::filesystem::path& root_path,
                                                            const domain::SnapshotId base_snapshot_id,
                                                            const filesystem::ScanOptions& scan_options,
                                                            const bool enqueue_transport) {
    PipelineResult result = capture_snapshot(root_path, scan_options, enqueue_transport);
    if (!result.snapshot_store_status.ok()) {
        return result;
    }

    domain::Snapshot base_snapshot;
    auto base_status = store_->get_snapshot(base_snapshot_id, base_snapshot, true);
    if (!base_status.ok()) {
        result.delta_store_status = base_status;
        result.transport_delta_status = base_status;
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
        result.delta_store_status = storage::StoreStatus::failure(
            storage::StoreErrorCode::ValidationFailure,
            "Delta validation failed before persistence."
        );
        result.transport_delta_status = result.delta_store_status;
        return result;
    }

    result.delta_store_status = store_->put_delta(
        *result.computed_delta,
        &base_snapshot,
        &result.snapshot_build.snapshot,
        false
    );
    if (!result.delta_store_status.ok()) {
        return result;
    }

    if (enqueue_transport) {
        result.transport_delta_status = store_->enqueue_delta(
            *result.computed_delta,
            &base_snapshot,
            &result.snapshot_build.snapshot,
            false,
            nullptr
        );
    } else {
        result.transport_delta_status = storage::StoreStatus::success();
    }

    return result;
}

} // namespace host_indexer::api
