#ifndef HUGIN_INGEST_INGEST_COORDINATOR_HPP
#define HUGIN_INGEST_INGEST_COORDINATOR_HPP

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

#include "../common/status.hpp"
#include "../observability/metrics.hpp"
#include "../sinks/downstream_sink.hpp"
#include "../storage/ingest_store.hpp"
#include "message_validator.hpp"

namespace hugin::ingest {

enum class IngestAction : std::uint8_t {
    Committed = 1,
    Duplicate = 2,
    Gap = 3
};

struct IngestDecision {
    IngestAction action {IngestAction::Committed};
    bool should_ack {true};
    std::uint64_t ack_inclusive_sequence {0};
    std::uint64_t expected_next_sequence {0};
    std::string reason {};
};

struct IngestCoordinatorOptions {
    std::size_t ack_batch_size {1U};
};

class IngestCoordinator final {
public:
    IngestCoordinator(storage::IIngestStore& store,
                      MessageValidator& validator,
                      sinks::DownstreamSink& sink,
                      observability::IngestMetrics& metrics,
                      IngestCoordinatorOptions options = {});

    common::StatusOr<IngestDecision> ingest(std::string_view host_id,
                                            std::uint64_t sequence,
                                            std::span<const std::uint8_t> raw_frame,
                                            std::uint64_t received_at_unix_seconds);

    common::StatusOr<std::uint64_t> expected_next_sequence(std::string_view host_id);

private:
    struct HostState {
        bool loaded {false};
        std::uint64_t last_committed_sequence {0};
        std::size_t since_last_ack {0};
    };

    common::StatusOr<HostState*> load_host_state_locked(std::string_view host_id);

    storage::IIngestStore& store_;
    MessageValidator& validator_;
    sinks::DownstreamSink& sink_;
    observability::IngestMetrics& metrics_;
    IngestCoordinatorOptions options_ {};
    std::mutex mutex_ {};
    std::unordered_map<std::string, HostState> host_states_ {};
};

} // namespace hugin::ingest

#endif // HUGIN_INGEST_INGEST_COORDINATOR_HPP
