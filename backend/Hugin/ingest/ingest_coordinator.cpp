#include "ingest_coordinator.hpp"

#include "../protocol/muninn_frame.hpp"

namespace hugin::ingest {

IngestCoordinator::IngestCoordinator(storage::IIngestStore& store,
                                     MessageValidator& validator,
                                     sinks::DownstreamSink& sink,
                                     observability::IngestMetrics& metrics,
                                     IngestCoordinatorOptions options)
    : store_(store),
      validator_(validator),
      sink_(sink),
      metrics_(metrics),
      options_(options) {
    if (options_.ack_batch_size == 0U) {
        options_.ack_batch_size = 1U;
    }
}

common::StatusOr<IngestCoordinator::HostState*> IngestCoordinator::load_host_state_locked(const std::string_view host_id) {
    auto& state = host_states_[std::string(host_id)];
    if (state.loaded) {
        return common::StatusOr<HostState*>::success(&state);
    }

    const auto checkpoint = store_.get_checkpoint(host_id);
    if (!checkpoint.ok()) {
        return common::StatusOr<HostState*>::failure(checkpoint.status.code, checkpoint.status.message);
    }
    state.last_committed_sequence = checkpoint.value;
    state.loaded = true;
    state.since_last_ack = 0U;
    return common::StatusOr<HostState*>::success(&state);
}

common::StatusOr<IngestDecision> IngestCoordinator::ingest(const std::string_view host_id,
                                                           const std::uint64_t sequence,
                                                           const std::span<const std::uint8_t> raw_frame,
                                                           const std::uint64_t received_at_unix_seconds) {
    metrics_.frames_received.fetch_add(1U);

    if (host_id.empty()) {
        return common::StatusOr<IngestDecision>::failure(common::ErrorCode::InvalidArgument, "Host id is empty.");
    }
    if (sequence == 0U) {
        return common::StatusOr<IngestDecision>::failure(common::ErrorCode::InvalidArgument, "Sequence is zero.");
    }

    HostState* state = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto loaded = load_host_state_locked(host_id);
        if (!loaded.ok()) {
            metrics_.frames_storage_failed.fetch_add(1U);
            return common::StatusOr<IngestDecision>::failure(loaded.status.code, loaded.status.message);
        }
        state = loaded.value;

        const std::uint64_t expected = state->last_committed_sequence + 1U;
        if (sequence <= state->last_committed_sequence) {
            metrics_.frames_duplicate.fetch_add(1U);
            IngestDecision duplicate;
            duplicate.action = IngestAction::Duplicate;
            duplicate.should_ack = true;
            duplicate.ack_inclusive_sequence = state->last_committed_sequence;
            duplicate.expected_next_sequence = expected;
            duplicate.reason = "duplicate_sequence";
            return common::StatusOr<IngestDecision>::success(std::move(duplicate));
        }

        if (sequence > expected) {
            metrics_.frames_gap.fetch_add(1U);
            IngestDecision gap;
            gap.action = IngestAction::Gap;
            gap.should_ack = false;
            gap.ack_inclusive_sequence = state->last_committed_sequence;
            gap.expected_next_sequence = expected;
            gap.reason = "sequence_gap";
            return common::StatusOr<IngestDecision>::success(std::move(gap));
        }
    }

    const auto parsed = protocol::parse_muninn_frame(raw_frame);
    if (!parsed.ok()) {
        metrics_.frames_validation_failed.fetch_add(1U);
        return common::StatusOr<IngestDecision>::failure(parsed.status.code, parsed.status.message);
    }

    const auto validated = validator_.validate_and_decode(parsed.value, host_id);
    if (!validated.ok()) {
        metrics_.frames_validation_failed.fetch_add(1U);
        return common::StatusOr<IngestDecision>::failure(validated.status.code, validated.status.message);
    }

    storage::IngestLogRecord record;
    record.host_id = std::string(host_id);
    record.sequence = sequence;
    record.received_at_unix_seconds = received_at_unix_seconds;
    record.raw_frame.assign(raw_frame.begin(), raw_frame.end());

    const auto write_status = store_.append_if_absent_and_checkpoint(record);
    if (!write_status.ok()) {
        metrics_.frames_storage_failed.fetch_add(1U);
        return common::StatusOr<IngestDecision>::failure(write_status.code, write_status.message);
    }

    common::Status sink_status = common::Status::success();
    if (validated.value.snapshot.has_value()) {
        sink_status = sink_.apply_snapshot(host_id, sequence, *validated.value.snapshot);
    } else if (validated.value.delta.has_value()) {
        sink_status = sink_.apply_delta(host_id, sequence, *validated.value.delta);
    }
    if (!sink_status.ok()) {
        metrics_.sink_failures.fetch_add(1U);
    }

    IngestDecision decision;
    decision.action = IngestAction::Committed;
    decision.reason = sink_status.ok() ? "committed" : "committed_sink_failed";

    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto loaded = load_host_state_locked(host_id);
        if (!loaded.ok()) {
            metrics_.frames_storage_failed.fetch_add(1U);
            return common::StatusOr<IngestDecision>::failure(loaded.status.code, loaded.status.message);
        }
        state = loaded.value;

        if (sequence > state->last_committed_sequence) {
            state->last_committed_sequence = sequence;
        }
        state->since_last_ack += 1U;
        decision.should_ack = state->since_last_ack >= options_.ack_batch_size;
        if (decision.should_ack) {
            state->since_last_ack = 0U;
        }

        decision.ack_inclusive_sequence = state->last_committed_sequence;
        decision.expected_next_sequence = state->last_committed_sequence + 1U;
    }

    metrics_.frames_committed.fetch_add(1U);
    return common::StatusOr<IngestDecision>::success(std::move(decision));
}

common::StatusOr<std::uint64_t> IngestCoordinator::expected_next_sequence(const std::string_view host_id) {
    if (host_id.empty()) {
        return common::StatusOr<std::uint64_t>::failure(common::ErrorCode::InvalidArgument, "Host id is empty.");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const auto loaded = load_host_state_locked(host_id);
    if (!loaded.ok()) {
        return common::StatusOr<std::uint64_t>::failure(loaded.status.code, loaded.status.message);
    }
    return common::StatusOr<std::uint64_t>::success(loaded.value->last_committed_sequence + 1U);
}

} // namespace hugin::ingest
