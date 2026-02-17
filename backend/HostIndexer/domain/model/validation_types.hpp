#ifndef HOSTINDEXER_VALIDATION_TYPES_HPP
#define HOSTINDEXER_VALIDATION_TYPES_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "models.hpp"

namespace host_indexer::domain {

enum class ValidationSeverity : std::uint8_t {
    Info = 0,
    Warning = 1,
    Error = 2,
    Fatal = 3
};

enum class ValidationErrorCode : std::uint16_t {
    // Snapshot-level errors (100-199)
    SnapshotIdZero = 100,
    SnapshotSchemaInvalid = 101,
    SnapshotSchemaTooNew = 102,
    SnapshotSchemaTooOld = 103,
    SnapshotTimestampInvalid = 104,
    SnapshotHostIdentifierEmpty = 105,
    SnapshotRootIdZero = 106,
    SnapshotEntriesEmpty = 107,
    SnapshotRootMissing = 108,
    SnapshotRootNotDirectory = 109,
    SnapshotRootParentNotZero = 110,

    // Entry-level errors (200-299)
    EntryIdZero = 200,
    EntryIdDuplicate = 201,
    EntryPathEmpty = 202,
    EntryPathNotAbsolute = 203,
    EntryPathNotNormalized = 204,
    EntryPathTraversalDetected = 205,
    EntryNameMismatch = 206,
    EntryPermissionsInvalid = 207,
    EntryTypeInvalid = 208,
    EntryTimestampOrderInvalid = 209,
    EntryTimestampInFuture = 210,

    // Structural graph errors (300-399)
    ParentIdZeroNonRoot = 300,
    ParentMissing = 301,
    ParentNotDirectory = 302,
    GraphCycleDetected = 303,
    GraphDisconnectedEntry = 304,
    GraphDuplicatePath = 305,
    GraphMaxDepthExceeded = 306,

    // Delta-specific errors (400-499)
    DeltaBaseSnapshotIdZero = 400,
    DeltaTargetSnapshotIdZero = 401,
    DeltaBaseEqualsTarget = 402,
    DeltaAddedEntryDuplicate = 403,
    DeltaRemovedEntryDuplicate = 404,
    DeltaModifiedEntryDuplicate = 405,
    DeltaRenamedEntryDuplicate = 406,
    DeltaConflictingOperations = 407,
    DeltaRemovedEntryMissingInBase = 408,
    DeltaAddedEntryMissingInTarget = 409,
    DeltaModifiedEntryMissingInBase = 410,
    DeltaModifiedEntryMissingInTarget = 411,
    DeltaRenamedOldMissingInBase = 412,
    DeltaRenamedNewMissingInTarget = 413,
    DeltaRenamePathConflict = 414,

    // Migration/versioning errors (500-599)
    MigrationUnsupportedSourceVersion = 500,
    MigrationUnsupportedTargetVersion = 501,
    MigrationNoPathAvailable = 502,
    MigrationValidationFailed = 503,
    MigrationNonReversible = 504
};

struct ValidationIssue {
    ValidationErrorCode code {ValidationErrorCode::SnapshotIdZero};
    ValidationSeverity severity {ValidationSeverity::Error};
    std::string message {};
    std::optional<SnapshotId> snapshot_id {};
    std::optional<EntryId> entry_id {};
    std::string path {};
};

struct ValidationReport {
    std::vector<ValidationIssue> issues {};

    [[nodiscard]] bool has_errors() const noexcept {
        for (const auto& issue : issues) {
            if (issue.severity == ValidationSeverity::Error || issue.severity == ValidationSeverity::Fatal) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool has_fatal() const noexcept {
        for (const auto& issue : issues) {
            if (issue.severity == ValidationSeverity::Fatal) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool ok() const noexcept {
        return !has_errors();
    }
};

} // namespace host_indexer::domain

#endif // HOSTINDEXER_VALIDATION_TYPES_HPP
