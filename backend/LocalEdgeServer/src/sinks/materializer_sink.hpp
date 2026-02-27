#ifndef LOCALEDGE_SINKS_MATERIALIZER_SINK_HPP
#define LOCALEDGE_SINKS_MATERIALIZER_SINK_HPP

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "downstream_sink.hpp"

namespace localedge::sinks {

struct HostMaterializedState {
    std::uint64_t last_sequence {0};
    std::uint64_t last_snapshot_id {0};
    std::string host_identifier {};
    std::unordered_map<host_indexer::domain::EntryId, host_indexer::domain::FileEntry> entries_by_id {};
    std::unordered_map<std::string, host_indexer::domain::EntryId> id_by_path {};
};

class MaterializerSink final : public DownstreamSink {
public:
    common::Status apply_snapshot(std::string_view host_id,
                                  std::uint64_t sequence,
                                  const host_indexer::domain::Snapshot& snapshot) override;
    common::Status apply_delta(std::string_view host_id,
                               std::uint64_t sequence,
                               const host_indexer::domain::DeltaSnapshot& delta) override;
    common::Status flush() override;

    common::StatusOr<std::string> latest_host_id() const;
    common::StatusOr<std::string> root_path(std::string_view host_id) const;
    common::StatusOr<std::uint64_t> last_snapshot_id(std::string_view host_id) const;
    common::StatusOr<std::uint64_t> last_sequence(std::string_view host_id) const;
    common::StatusOr<std::vector<host_indexer::domain::FileEntry>> list_children(std::string_view host_id,
                                                                                  std::string_view parent_path) const;
    common::StatusOr<std::size_t> entry_count(std::string_view host_id) const;

private:
    static HostMaterializedState build_from_snapshot(const host_indexer::domain::Snapshot& snapshot,
                                                     std::uint64_t sequence);

    mutable std::mutex mutex_ {};
    std::unordered_map<std::string, HostMaterializedState> states_ {};
    std::string last_updated_host_id_ {};
};

} // namespace localedge::sinks

#endif // LOCALEDGE_SINKS_MATERIALIZER_SINK_HPP
