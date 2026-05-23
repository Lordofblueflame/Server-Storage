#include "validate_snapshot.hpp"

#include "validators.hpp"

namespace muninn::validation {

ValidationReport validate_snapshot(const domain::Snapshot& snapshot) {
    const SnapshotValidator validator;
    return validator.validate(snapshot);
}

ValidationReport validate_delta(const domain::DeltaSnapshot& delta,
                                const domain::Snapshot* base_snapshot,
                                const domain::Snapshot* target_snapshot) {
    const DeltaValidator validator;
    return validator.validate(delta, base_snapshot, target_snapshot);
}

} // namespace muninn::validation
