#include "test_common.hpp"

#include "../protocol/muninn_frame.hpp"

bool run_muninn_frame_tests() {
    using namespace hugin::protocol;

    MuninnFrame frame;
    frame.type = MuninnRecordType::Snapshot;
    frame.created_at_unix_seconds = 1'700'000'456;
    frame.routing_key = "host-a";
    frame.payload = {10U, 20U, 30U, 40U};

    const auto encoded = serialize_muninn_frame(frame);
    const auto decoded = parse_muninn_frame(encoded);
    require_true(decoded.ok(), "Muninn frame should decode");
    require_true(decoded.value.type == frame.type, "Frame type mismatch");
    require_true(decoded.value.created_at_unix_seconds == frame.created_at_unix_seconds, "Frame created_at mismatch");
    require_true(decoded.value.routing_key == frame.routing_key, "Frame routing key mismatch");
    require_true(decoded.value.payload == frame.payload, "Frame payload mismatch");

    return true;
}
