#ifndef HOSTINDEXER_MIGRATION_HPP
#define HOSTINDEXER_MIGRATION_HPP

#include "../model/models.hpp"
#include "../model/validation_types.hpp"

namespace host_indexer::migration {

enum class MigrationStatus : std::uint8_t {
    Success = 0,
    UnsupportedPath = 1,
    ValidationFailed = 2
};

struct SnapshotMigrationResult {
    MigrationStatus status {MigrationStatus::Success};
    domain::Snapshot snapshot {};
    domain::ValidationReport validation {};
    bool reversible {false};
};

struct DeltaMigrationResult {
    MigrationStatus status {MigrationStatus::Success};
    domain::DeltaSnapshot delta {};
    domain::ValidationReport validation {};
    bool reversible {false};
};

SnapshotMigrationResult migrate_snapshot(const domain::Snapshot& snapshot,
                                         const domain::SchemaHeader& target_schema);
SnapshotMigrationResult rollback_snapshot(const domain::Snapshot& snapshot,
                                          const domain::SchemaHeader& target_schema);

DeltaMigrationResult migrate_delta(const domain::DeltaSnapshot& delta,
                                   const domain::SchemaHeader& target_schema);
DeltaMigrationResult rollback_delta(const domain::DeltaSnapshot& delta,
                                    const domain::SchemaHeader& target_schema);

} // namespace host_indexer::migration

#endif // HOSTINDEXER_MIGRATION_HPP
