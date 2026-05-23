#include "migration.hpp"

#include "../validation/validate_snapshot.hpp"

namespace muninn::migration {
namespace {

bool is_supported_schema(const domain::SchemaHeader& schema) noexcept {
    return schema.major == 1;
}

void add_issue(domain::ValidationReport& report,
               const domain::ValidationErrorCode code,
               const domain::ValidationSeverity severity,
               const std::string_view message) {
    report.issues.push_back(
        domain::ValidationIssue {
            code,
            severity,
            std::string(message),
            std::nullopt,
            std::nullopt,
            {}
        }
    );
}

bool is_reversible_pair(const domain::SchemaHeader& source,
                        const domain::SchemaHeader& target) noexcept {
    if (source.major != target.major) {
        return false;
    }
    if (source.min_reader_major > target.major) {
        return false;
    }
    if (target.min_reader_major > source.major) {
        return false;
    }
    return true;
}

} // namespace

SnapshotMigrationResult migrate_snapshot(const domain::Snapshot& snapshot,
                                         const domain::SchemaHeader& target_schema) {
    SnapshotMigrationResult result;
    result.snapshot = snapshot;
    result.reversible = is_reversible_pair(snapshot.schema, target_schema);

    if (!is_supported_schema(snapshot.schema)) {
        result.status = MigrationStatus::UnsupportedPath;
        add_issue(
            result.validation,
            domain::ValidationErrorCode::MigrationUnsupportedSourceVersion,
            domain::ValidationSeverity::Error,
            "Snapshot source schema is unsupported for migration."
        );
        return result;
    }

    if (!is_supported_schema(target_schema)) {
        result.status = MigrationStatus::UnsupportedPath;
        add_issue(
            result.validation,
            domain::ValidationErrorCode::MigrationUnsupportedTargetVersion,
            domain::ValidationSeverity::Error,
            "Snapshot target schema is unsupported for migration."
        );
        return result;
    }

    if (snapshot.schema.min_reader_major > target_schema.major) {
        result.status = MigrationStatus::UnsupportedPath;
        add_issue(
            result.validation,
            domain::ValidationErrorCode::MigrationNoPathAvailable,
            domain::ValidationSeverity::Error,
            "No deterministic migration path exists between source and target schema."
        );
        return result;
    }

    result.snapshot.schema = target_schema;
    result.validation = validation::validate_snapshot(result.snapshot);
    if (result.validation.has_errors()) {
        result.status = MigrationStatus::ValidationFailed;
        add_issue(
            result.validation,
            domain::ValidationErrorCode::MigrationValidationFailed,
            domain::ValidationSeverity::Error,
            "Migrated snapshot does not satisfy validation constraints."
        );
        return result;
    }

    result.status = MigrationStatus::Success;
    if (!result.reversible) {
        add_issue(
            result.validation,
            domain::ValidationErrorCode::MigrationNonReversible,
            domain::ValidationSeverity::Warning,
            "Migration completed but is not guaranteed reversible."
        );
    }
    return result;
}

SnapshotMigrationResult rollback_snapshot(const domain::Snapshot& snapshot,
                                          const domain::SchemaHeader& target_schema) {
    return migrate_snapshot(snapshot, target_schema);
}

DeltaMigrationResult migrate_delta(const domain::DeltaSnapshot& delta,
                                   const domain::SchemaHeader& target_schema) {
    DeltaMigrationResult result;
    result.delta = delta;
    result.reversible = is_reversible_pair(delta.schema, target_schema);

    if (!is_supported_schema(delta.schema)) {
        result.status = MigrationStatus::UnsupportedPath;
        add_issue(
            result.validation,
            domain::ValidationErrorCode::MigrationUnsupportedSourceVersion,
            domain::ValidationSeverity::Error,
            "Delta source schema is unsupported for migration."
        );
        return result;
    }

    if (!is_supported_schema(target_schema)) {
        result.status = MigrationStatus::UnsupportedPath;
        add_issue(
            result.validation,
            domain::ValidationErrorCode::MigrationUnsupportedTargetVersion,
            domain::ValidationSeverity::Error,
            "Delta target schema is unsupported for migration."
        );
        return result;
    }

    if (delta.schema.min_reader_major > target_schema.major) {
        result.status = MigrationStatus::UnsupportedPath;
        add_issue(
            result.validation,
            domain::ValidationErrorCode::MigrationNoPathAvailable,
            domain::ValidationSeverity::Error,
            "No deterministic migration path exists between source and target delta schema."
        );
        return result;
    }

    result.delta.schema = target_schema;
    result.validation = validation::validate_delta(result.delta);
    if (result.validation.has_errors()) {
        result.status = MigrationStatus::ValidationFailed;
        add_issue(
            result.validation,
            domain::ValidationErrorCode::MigrationValidationFailed,
            domain::ValidationSeverity::Error,
            "Migrated delta does not satisfy validation constraints."
        );
        return result;
    }

    result.status = MigrationStatus::Success;
    if (!result.reversible) {
        add_issue(
            result.validation,
            domain::ValidationErrorCode::MigrationNonReversible,
            domain::ValidationSeverity::Warning,
            "Delta migration completed but is not guaranteed reversible."
        );
    }
    return result;
}

DeltaMigrationResult rollback_delta(const domain::DeltaSnapshot& delta,
                                    const domain::SchemaHeader& target_schema) {
    return migrate_delta(delta, target_schema);
}

} // namespace muninn::migration
