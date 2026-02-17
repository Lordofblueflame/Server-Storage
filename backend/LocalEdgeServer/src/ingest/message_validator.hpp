#ifndef LOCALEDGE_INGEST_MESSAGE_VALIDATOR_HPP
#define LOCALEDGE_INGEST_MESSAGE_VALIDATOR_HPP

#include <optional>
#include <string_view>

#include "../common/status.hpp"
#include "../protocol/hostindexer_frame.hpp"

#include "domain/model/models.hpp"

namespace localedge::ingest {

struct ValidatedRecord {
    protocol::HostIndexerFrame frame {};
    std::optional<host_indexer::domain::Snapshot> snapshot {};
    std::optional<host_indexer::domain::DeltaSnapshot> delta {};
};

class MessageValidator final {
public:
    common::StatusOr<ValidatedRecord> validate_and_decode(const protocol::HostIndexerFrame& frame,
                                                          std::string_view expected_host_id) const;
};

} // namespace localedge::ingest

#endif // LOCALEDGE_INGEST_MESSAGE_VALIDATOR_HPP
