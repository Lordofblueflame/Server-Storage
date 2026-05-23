#ifndef MUNINN_DELTA_ENGINE_HPP
#define MUNINN_DELTA_ENGINE_HPP

#include "../domain/model/models.hpp"

namespace muninn::snapshot {

struct DeltaOptions {
    bool enable_rename_heuristic {true};
};

class DeltaEngine {
public:
    [[nodiscard]] domain::DeltaSnapshot compute(const domain::Snapshot& base_snapshot,
                                                const domain::Snapshot& target_snapshot,
                                                const DeltaOptions& options = {}) const;

    [[nodiscard]] domain::DeltaSnapshot invert(const domain::DeltaSnapshot& delta) const;
};

} // namespace muninn::snapshot

#endif // MUNINN_DELTA_ENGINE_HPP
