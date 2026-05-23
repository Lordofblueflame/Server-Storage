#ifndef MUNINN_VALIDATE_SNAPSHOT_HPP
#define MUNINN_VALIDATE_SNAPSHOT_HPP

#include "../model/models.hpp"
#include "validation_errors.hpp"

namespace muninn::validation {

ValidationReport validate_snapshot(const domain::Snapshot& snapshot);
ValidationReport validate_delta(const domain::DeltaSnapshot& delta,
                                const domain::Snapshot* base_snapshot = nullptr,
                                const domain::Snapshot* target_snapshot = nullptr);

} // namespace muninn::validation

#endif // MUNINN_VALIDATE_SNAPSHOT_HPP
