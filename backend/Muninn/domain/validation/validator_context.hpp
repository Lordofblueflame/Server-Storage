#ifndef MUNINN_VALIDATOR_CONTEXT_HPP
#define MUNINN_VALIDATOR_CONTEXT_HPP

#include <optional>
#include <string_view>

#include "../model/models.hpp"
#include "validation_errors.hpp"

namespace muninn::validation {

class ValidationCollector {
public:
    explicit ValidationCollector(ValidationReport& report)
        : report_(report) {
    }

    void add(const ValidationErrorCode code,
             const ValidationSeverity severity,
             const std::string_view message,
             const std::optional<domain::SnapshotId> snapshot_id = std::nullopt,
             const std::optional<domain::EntryId> entry_id = std::nullopt,
             const std::string_view path = {}) {
        report_.issues.push_back(
            ValidationIssue {
                code,
                severity,
                std::string(message),
                snapshot_id,
                entry_id,
                std::string(path)
            }
        );
    }

private:
    ValidationReport& report_;
};

} // namespace muninn::validation

#endif // MUNINN_VALIDATOR_CONTEXT_HPP
