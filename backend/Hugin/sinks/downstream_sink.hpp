#ifndef HUGIN_SINKS_DOWNSTREAM_SINK_HPP
#define HUGIN_SINKS_DOWNSTREAM_SINK_HPP

#include <cstdint>
#include <string_view>

#include "../common/status.hpp"

#include "domain/model/models.hpp"

namespace hugin::sinks {

class DownstreamSink {
public:
    virtual ~DownstreamSink() = default;

    virtual common::Status apply_snapshot(std::string_view host_id,
                                          std::uint64_t sequence,
                                          const muninn::domain::Snapshot& snapshot) = 0;
    virtual common::Status apply_delta(std::string_view host_id,
                                       std::uint64_t sequence,
                                       const muninn::domain::DeltaSnapshot& delta) = 0;
    virtual common::Status flush() = 0;
};

class NoopSink final : public DownstreamSink {
public:
    common::Status apply_snapshot(std::string_view,
                                  std::uint64_t,
                                  const muninn::domain::Snapshot&) override {
        return common::Status::success();
    }

    common::Status apply_delta(std::string_view,
                               std::uint64_t,
                               const muninn::domain::DeltaSnapshot&) override {
        return common::Status::success();
    }

    common::Status flush() override {
        return common::Status::success();
    }
};

} // namespace hugin::sinks

#endif // HUGIN_SINKS_DOWNSTREAM_SINK_HPP
