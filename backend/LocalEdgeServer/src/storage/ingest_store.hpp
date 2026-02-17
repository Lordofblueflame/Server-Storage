#ifndef LOCALEDGE_STORAGE_INGEST_STORE_HPP
#define LOCALEDGE_STORAGE_INGEST_STORE_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../common/status.hpp"

namespace localedge::storage {

struct IngestLogRecord {
    std::string host_id {};
    std::uint64_t sequence {0};
    std::uint64_t received_at_unix_seconds {0};
    std::vector<std::uint8_t> raw_frame {};
};

class IIngestStore {
public:
    virtual ~IIngestStore() = default;

    virtual common::StatusOr<std::uint64_t> get_checkpoint(std::string_view host_id) const = 0;
    virtual common::StatusOr<bool> has_record(std::string_view host_id, std::uint64_t sequence) const = 0;
    virtual common::Status append_if_absent_and_checkpoint(const IngestLogRecord& record) = 0;
};

} // namespace localedge::storage

#endif // LOCALEDGE_STORAGE_INGEST_STORE_HPP
