#ifndef HOSTINDEXER_VALIDATE_SNAPSHOT_HPP
#define HOSTINDEXER_VALIDATE_SNAPSHOT_HPP

#include "../model/models.hpp"
#include "validation_errors.hpp"

namespace host_indexer::validation {

ValidationReport validate_snapshot(const domain::Snapshot& snapshot);
ValidationReport validate_delta(const domain::DeltaSnapshot& delta,
                                const domain::Snapshot* base_snapshot = nullptr,
                                const domain::Snapshot* target_snapshot = nullptr);

} // namespace host_indexer::validation

#endif // HOSTINDEXER_VALIDATE_SNAPSHOT_HPP
