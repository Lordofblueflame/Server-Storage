#ifndef MUNINN_INDEXING_PIPELINE_HPP
#define MUNINN_INDEXING_PIPELINE_HPP

#include <filesystem>
#include <optional>

#include "../domain/model/models.hpp"
#include "../domain/model/validation_types.hpp"
#include "../filesystem/filesystem_scanner.hpp"
#include "../snapshot/delta_engine.hpp"
#include "../snapshot/snapshot_builder.hpp"
#include "../storage/lmdb_store.hpp"

namespace muninn::api {

struct PipelineResult {
    filesystem::RawScanResult raw_scan {};
    snapshot::BuildResult snapshot_build {};
    domain::ValidationReport snapshot_validation {};

    std::optional<domain::Snapshot> base_snapshot {};
    std::optional<domain::DeltaSnapshot> computed_delta {};
    domain::ValidationReport delta_validation {};

    storage::StoreStatus snapshot_store_status {};
    storage::StoreStatus delta_store_status {};
    storage::StoreStatus transport_snapshot_status {};
    storage::StoreStatus transport_delta_status {};

    [[nodiscard]] bool ok() const noexcept {
        return snapshot_store_status.ok() &&
               delta_store_status.ok() &&
               transport_snapshot_status.ok() &&
               transport_delta_status.ok() &&
               snapshot_validation.ok() &&
               (!computed_delta.has_value() || delta_validation.ok());
    }
};

class IndexingPipeline final {
public:
    explicit IndexingPipeline(storage::LmdbStore& store);

    PipelineResult capture_snapshot(const std::filesystem::path& root_path,
                                    const filesystem::ScanOptions& scan_options = {},
                                    bool enqueue_transport = true);

    PipelineResult capture_snapshot_and_delta(const std::filesystem::path& root_path,
                                              domain::SnapshotId base_snapshot_id,
                                              const filesystem::ScanOptions& scan_options = {},
                                              bool enqueue_transport = true);

private:
    filesystem::LocalFilesystemScanner scanner_ {};
    snapshot::SnapshotBuilder snapshot_builder_ {};
    snapshot::DeltaEngine delta_engine_ {};
    storage::LmdbStore* store_ {nullptr};
};

} // namespace muninn::api

#endif // MUNINN_INDEXING_PIPELINE_HPP
