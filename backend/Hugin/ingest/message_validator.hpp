#ifndef HUGIN_INGEST_MESSAGE_VALIDATOR_HPP
#define HUGIN_INGEST_MESSAGE_VALIDATOR_HPP

#include <optional>
#include <string_view>

#include "../common/status.hpp"
#include "../protocol/muninn_frame.hpp"

#include "domain/model/models.hpp"

namespace hugin::ingest {

struct ValidatedRecord {
    protocol::MuninnFrame frame {};
    std::optional<muninn::domain::Snapshot> snapshot {};
    std::optional<muninn::domain::DeltaSnapshot> delta {};
};

class MessageValidator final {
public:
    common::StatusOr<ValidatedRecord> validate_and_decode(const protocol::MuninnFrame& frame,
                                                          std::string_view expected_host_id) const;
};

} // namespace hugin::ingest

#endif // HUGIN_INGEST_MESSAGE_VALIDATOR_HPP
