#ifndef HUGIN_STORAGE_IN_MEMORY_INGEST_STORE_HPP
#define HUGIN_STORAGE_IN_MEMORY_INGEST_STORE_HPP

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ingest_store.hpp"

namespace hugin::storage {

class InMemoryIngestStore final : public IIngestStore {
public:
    common::StatusOr<std::uint64_t> get_checkpoint(std::string_view host_id) const override;
    common::StatusOr<bool> has_record(std::string_view host_id, std::uint64_t sequence) const override;
    common::Status append_if_absent_and_checkpoint(const IngestLogRecord& record) override;

private:
    static std::string make_key(std::string_view host_id, std::uint64_t sequence);

    mutable std::mutex mutex_ {};
    std::unordered_map<std::string, std::uint64_t> checkpoints_ {};
    std::unordered_map<std::string, IngestLogRecord> records_ {};
};

} // namespace hugin::storage

#endif // HUGIN_STORAGE_IN_MEMORY_INGEST_STORE_HPP
