#include "forwarder_sink.hpp"

namespace localedge::sinks {

ForwarderSink::ForwarderSink(ForwarderSinkOptions options)
    : options_(std::move(options)) {
}

common::Status ForwarderSink::apply_snapshot(const std::string_view host_id,
                                             const std::uint64_t sequence,
                                             const host_indexer::domain::Snapshot& snapshot) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.push_back(
        ForwarderEvent {
            std::string(host_id),
            sequence,
            "snapshot",
            snapshot.snapshot_id,
            0U
        }
    );
    return common::Status::success();
}

common::Status ForwarderSink::apply_delta(const std::string_view host_id,
                                          const std::uint64_t sequence,
                                          const host_indexer::domain::DeltaSnapshot& delta) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.push_back(
        ForwarderEvent {
            std::string(host_id),
            sequence,
            "delta",
            delta.base_snapshot_id,
            delta.target_snapshot_id
        }
    );
    return common::Status::success();
}

common::Status ForwarderSink::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    // WebSocket/HTTP delivery is intentionally deferred to the real forwarder implementation.
    pending_.clear();
    return common::Status::success();
}

} // namespace localedge::sinks
