#ifndef MUNINN_SERIALIZATION_HPP
#define MUNINN_SERIALIZATION_HPP

#include <istream>
#include <ostream>

#include <nlohmann/json.hpp>

#include "../model/models.hpp"
#include "serialization_helper.hpp"

namespace muninn::serialization {

SerializationResult serialize_snapshot_binary(const domain::Snapshot& snapshot, std::ostream& stream);
SerializationResult deserialize_snapshot_binary(std::istream& stream, domain::Snapshot& snapshot);

SerializationResult serialize_delta_binary(const domain::DeltaSnapshot& delta, std::ostream& stream);
SerializationResult deserialize_delta_binary(std::istream& stream, domain::DeltaSnapshot& delta);

nlohmann::json snapshot_to_json(const domain::Snapshot& snapshot);
SerializationResult snapshot_from_json(const nlohmann::json& json, domain::Snapshot& snapshot);

nlohmann::json delta_to_json(const domain::DeltaSnapshot& delta);
SerializationResult delta_from_json(const nlohmann::json& json, domain::DeltaSnapshot& delta);

} // namespace muninn::serialization

#endif // MUNINN_SERIALIZATION_HPP
