#include "test_common.hpp"

#include <cstdint>
#include <limits>

#include <nlohmann/json.hpp>

#include "../api/frontend_dto.hpp"

bool run_frontend_dto_tests() {
    using hugin::api::parse_frontend_command_request;

    nlohmann::json max_snapshot = {
        {"operation", "read"},
        {"snapshot_id", std::numeric_limits<std::uint64_t>::max()}
    };
    const auto parsed_max_snapshot = parse_frontend_command_request(max_snapshot);
    require_true(parsed_max_snapshot.ok(), "frontend dto test: uint64 snapshot_id should parse");
    require_true(
        parsed_max_snapshot.value.snapshot_id == std::numeric_limits<std::uint64_t>::max(),
        "frontend dto test: uint64 snapshot_id should be preserved"
    );

    nlohmann::json negative_snapshot = {
        {"operation", "read"},
        {"snapshot_id", -1}
    };
    const auto parsed_negative_snapshot = parse_frontend_command_request(negative_snapshot);
    require_true(!parsed_negative_snapshot.ok(), "frontend dto test: negative snapshot_id should be rejected");

    nlohmann::json oversized_batch = {
        {"operation", "read"},
        {"outbox_batch_size", static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1U}
    };
    const auto parsed_oversized_batch = parse_frontend_command_request(oversized_batch);
    require_true(!parsed_oversized_batch.ok(), "frontend dto test: oversized outbox batch should be rejected");

    return true;
}
