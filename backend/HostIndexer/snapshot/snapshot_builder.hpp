#ifndef HOSTINDEXER_SNAPSHOT_BUILDER_HPP
#define HOSTINDEXER_SNAPSHOT_BUILDER_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "../domain/model/models.hpp"
#include "../filesystem/filesystem_scanner.hpp"

namespace host_indexer::snapshot {

enum class BuildSeverity : std::uint8_t {
    Info = 0,
    Warning = 1,
    Error = 2
};

enum class BuildDiagnosticCode : std::uint16_t {
    None = 0,
    RootEntryMissing = 1,
    ParentPathMissing = 2,
    DuplicatePath = 3,
    StableIdCollision = 4
};

struct BuildDiagnostic {
    BuildDiagnosticCode code {BuildDiagnosticCode::None};
    BuildSeverity severity {BuildSeverity::Info};
    std::string message {};
    std::string path {};
};

struct BuildOptions {
    domain::SnapshotId snapshot_id {0};
    domain::UnixTimestamp created_at {0};
};

struct BuildResult {
    domain::Snapshot snapshot {};
    std::vector<BuildDiagnostic> diagnostics {};
};

class SnapshotBuilder {
public:
    [[nodiscard]] BuildResult build(const filesystem::RawScanResult& scan_result,
                                    const BuildOptions& options = {}) const;
};

} // namespace host_indexer::snapshot

#endif // HOSTINDEXER_SNAPSHOT_BUILDER_HPP
