#ifndef MUNINN_IVALIDATOR_HPP
#define MUNINN_IVALIDATOR_HPP

#include <string_view>

#include "../model/models.hpp"
#include "validator_context.hpp"

namespace muninn::validation {

class ISnapshotValidationPass {
public:
    virtual ~ISnapshotValidationPass() = default;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    virtual void run(const domain::Snapshot& snapshot, ValidationCollector& collector) const = 0;
};

class IDeltaValidationPass {
public:
    virtual ~IDeltaValidationPass() = default;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    virtual void run(const domain::DeltaSnapshot& delta,
                     const domain::Snapshot* base_snapshot,
                     const domain::Snapshot* target_snapshot,
                     ValidationCollector& collector) const = 0;
};

} // namespace muninn::validation

#endif // MUNINN_IVALIDATOR_HPP
