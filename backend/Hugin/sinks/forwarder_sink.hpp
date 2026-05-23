#ifndef HUGIN_SINKS_FORWARDER_SINK_HPP
#define HUGIN_SINKS_FORWARDER_SINK_HPP

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "downstream_sink.hpp"

namespace hugin::sinks {

struct ForwarderSinkOptions {
    std::string endpoint {};
};

struct ForwarderEvent {
    std::string host_id {};
    std::uint64_t sequence {0};
    std::string kind {};
    std::uint64_t source_id_a {0};
    std::uint64_t source_id_b {0};
};

class ForwarderSink final : public DownstreamSink {
public:
    explicit ForwarderSink(ForwarderSinkOptions options);

    common::Status apply_snapshot(std::string_view host_id,
                                  std::uint64_t sequence,
                                  const muninn::domain::Snapshot& snapshot) override;
    common::Status apply_delta(std::string_view host_id,
                               std::uint64_t sequence,
                               const muninn::domain::DeltaSnapshot& delta) override;
    common::Status flush() override;

private:
    ForwarderSinkOptions options_ {};
    std::mutex mutex_ {};
    std::vector<ForwarderEvent> pending_ {};
};

} // namespace hugin::sinks

#endif // HUGIN_SINKS_FORWARDER_SINK_HPP
