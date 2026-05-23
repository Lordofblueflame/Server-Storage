#include "test_common.hpp"

#include <sstream>

#include "../ingest/ingest_coordinator.hpp"
#include "../ingest/message_validator.hpp"
#include "../observability/metrics.hpp"
#include "../protocol/muninn_frame.hpp"
#include "../sinks/downstream_sink.hpp"
#include "../storage/in_memory_ingest_store.hpp"

#include "domain/model/models.hpp"
#include "domain/serialization/serialization.hpp"

namespace {

muninn::domain::Snapshot make_snapshot() {
    muninn::domain::Snapshot snapshot;
    snapshot.schema = muninn::domain::kCurrentSchemaHeader;
    snapshot.snapshot_id = 1001U;
    snapshot.created_at = 1'700'000'000;
    snapshot.host_identifier = "host-a";
    snapshot.root_entry_id = 1U;

    muninn::domain::FileEntry root;
    root.id = 1U;
    root.parent_id = 0U;
    root.normalized_path = "/";
    root.name = "/";
    root.type = muninn::domain::EntryType::Directory;
    root.metadata.created_at = 1'699'999'999;
    root.metadata.modified_at = 1'699'999'999;
    root.metadata.accessed_at = 1'699'999'999;
    root.metadata.permissions = 0755U;
    snapshot.entries.push_back(std::move(root));
    return snapshot;
}

std::vector<std::uint8_t> make_snapshot_frame_payload() {
    const auto snapshot = make_snapshot();
    std::ostringstream stream(std::ios::binary);
    const auto serialize = muninn::serialization::serialize_snapshot_binary(snapshot, stream);
    require_true(serialize.ok, "Snapshot serialization must succeed for test fixture");
    const auto payload_text = stream.str();

    hugin::protocol::MuninnFrame frame;
    frame.type = hugin::protocol::MuninnRecordType::Snapshot;
    frame.created_at_unix_seconds = 1'700'000'001U;
    frame.routing_key = "host-a";
    frame.payload.assign(payload_text.begin(), payload_text.end());
    return hugin::protocol::serialize_muninn_frame(frame);
}

} // namespace

bool run_ingest_coordinator_tests() {
    hugin::storage::InMemoryIngestStore store;
    hugin::ingest::MessageValidator validator;
    hugin::sinks::NoopSink sink;
    hugin::observability::IngestMetrics metrics;

    hugin::ingest::IngestCoordinator coordinator(
        store,
        validator,
        sink,
        metrics,
        hugin::ingest::IngestCoordinatorOptions {1U}
    );

    const auto frame = make_snapshot_frame_payload();

    const auto first = coordinator.ingest("host-a", 1U, frame, 1'700'000'100U);
    require_true(first.ok(), "First ingest should succeed");
    require_true(first.value.action == hugin::ingest::IngestAction::Committed, "First ingest should commit");
    require_true(first.value.ack_inclusive_sequence == 1U, "First ack should be 1");

    const auto duplicate = coordinator.ingest("host-a", 1U, frame, 1'700'000'101U);
    require_true(duplicate.ok(), "Duplicate ingest should succeed");
    require_true(duplicate.value.action == hugin::ingest::IngestAction::Duplicate, "Duplicate action mismatch");
    require_true(duplicate.value.ack_inclusive_sequence == 1U, "Duplicate ack should stay at 1");

    const auto gap = coordinator.ingest("host-a", 3U, frame, 1'700'000'102U);
    require_true(gap.ok(), "Gap ingest should return decision");
    require_true(gap.value.action == hugin::ingest::IngestAction::Gap, "Gap action mismatch");
    require_true(gap.value.expected_next_sequence == 2U, "Expected next sequence must be 2");

    return true;
}
