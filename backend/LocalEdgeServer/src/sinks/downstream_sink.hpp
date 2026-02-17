#ifndef LOCALEDGE_SINKS_DOWNSTREAM_SINK_HPP
#define LOCALEDGE_SINKS_DOWNSTREAM_SINK_HPP

#include <cstdint>
#include <string_view>

#include "../common/status.hpp"

#include "domain/model/models.hpp"

namespace localedge::sinks {

class DownstreamSink {
public:
    virtual ~DownstreamSink() = default;

    virtual common::Status apply_snapshot(std::string_view host_id,
                                          std::uint64_t sequence,
                                          const host_indexer::domain::Snapshot& snapshot) = 0;
    virtual common::Status apply_delta(std::string_view host_id,
                                       std::uint64_t sequence,
                                       const host_indexer::domain::DeltaSnapshot& delta) = 0;
    virtual common::Status flush() = 0;
};

class NoopSink final : public DownstreamSink {
public:
    common::Status apply_snapshot(std::string_view,
                                  std::uint64_t,
                                  const host_indexer::domain::Snapshot&) override {
        return common::Status::success();
    }

    common::Status apply_delta(std::string_view,
                               std::uint64_t,
                               const host_indexer::domain::DeltaSnapshot&) override {
        return common::Status::success();
    }

    common::Status flush() override {
        return common::Status::success();
    }
};

} // namespace localedge::sinks

#endif // LOCALEDGE_SINKS_DOWNSTREAM_SINK_HPP
