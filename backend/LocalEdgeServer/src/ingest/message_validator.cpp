#include "message_validator.hpp"

#include <sstream>

#include "domain/serialization/serialization.hpp"
#include "domain/validation/validate_snapshot.hpp"

namespace localedge::ingest {
namespace {

common::Status validate_delta_routing_key(const std::string_view routing_key,
                                          const host_indexer::domain::DeltaSnapshot& delta) {
    const auto delimiter = routing_key.find("->");
    if (delimiter == std::string_view::npos) {
        return common::Status::failure(common::ErrorCode::ValidationError, "Delta routing key is malformed.");
    }

    try {
        const auto base_text = routing_key.substr(0, delimiter);
        const auto target_text = routing_key.substr(delimiter + 2U);
        const std::uint64_t base = std::stoull(std::string(base_text));
        const std::uint64_t target = std::stoull(std::string(target_text));
        if (base != delta.base_snapshot_id || target != delta.target_snapshot_id) {
            return common::Status::failure(
                common::ErrorCode::ValidationError,
                "Delta routing key does not match payload snapshot ids."
            );
        }
    } catch (const std::exception&) {
        return common::Status::failure(common::ErrorCode::ValidationError, "Delta routing key parse failed.");
    }

    return common::Status::success();
}

} // namespace

common::StatusOr<ValidatedRecord> MessageValidator::validate_and_decode(const protocol::HostIndexerFrame& frame,
                                                                        const std::string_view expected_host_id) const {
    ValidatedRecord out;
    out.frame = frame;

    if (frame.type == protocol::HostIndexerRecordType::Snapshot) {
        host_indexer::domain::Snapshot snapshot;
        const std::string payload_bytes(frame.payload.begin(), frame.payload.end());
        std::istringstream payload_stream(payload_bytes, std::ios::binary);

        const auto deserialize = host_indexer::serialization::deserialize_snapshot_binary(payload_stream, snapshot);
        if (!deserialize.ok) {
            return common::StatusOr<ValidatedRecord>::failure(
                common::ErrorCode::ParseError,
                "Snapshot payload decode failed: " + deserialize.message
            );
        }

        const auto validation = host_indexer::validation::validate_snapshot(snapshot);
        if (validation.has_errors()) {
            return common::StatusOr<ValidatedRecord>::failure(
                common::ErrorCode::ValidationError,
                "Snapshot payload validation failed."
            );
        }

        if (!expected_host_id.empty() && snapshot.host_identifier != expected_host_id) {
            return common::StatusOr<ValidatedRecord>::failure(
                common::ErrorCode::ValidationError,
                "Snapshot host identifier does not match authenticated host."
            );
        }

        if (!frame.routing_key.empty() && frame.routing_key != snapshot.host_identifier) {
            return common::StatusOr<ValidatedRecord>::failure(
                common::ErrorCode::ValidationError,
                "Snapshot routing key does not match payload host identifier."
            );
        }

        out.snapshot = std::move(snapshot);
        return common::StatusOr<ValidatedRecord>::success(std::move(out));
    }

    if (frame.type == protocol::HostIndexerRecordType::Delta) {
        host_indexer::domain::DeltaSnapshot delta;
        const std::string payload_bytes(frame.payload.begin(), frame.payload.end());
        std::istringstream payload_stream(payload_bytes, std::ios::binary);

        const auto deserialize = host_indexer::serialization::deserialize_delta_binary(payload_stream, delta);
        if (!deserialize.ok) {
            return common::StatusOr<ValidatedRecord>::failure(
                common::ErrorCode::ParseError,
                "Delta payload decode failed: " + deserialize.message
            );
        }

        const auto validation = host_indexer::validation::validate_delta(delta);
        if (validation.has_errors()) {
            return common::StatusOr<ValidatedRecord>::failure(
                common::ErrorCode::ValidationError,
                "Delta payload validation failed."
            );
        }

        const auto route_status = validate_delta_routing_key(frame.routing_key, delta);
        if (!route_status.ok()) {
            return common::StatusOr<ValidatedRecord>::failure(route_status.code, route_status.message);
        }

        out.delta = std::move(delta);
        return common::StatusOr<ValidatedRecord>::success(std::move(out));
    }

    return common::StatusOr<ValidatedRecord>::failure(common::ErrorCode::ValidationError, "Unsupported frame type.");
}

} // namespace localedge::ingest
