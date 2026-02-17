#include "in_memory_ingest_store.hpp"

#include <sstream>

namespace localedge::storage {

std::string InMemoryIngestStore::make_key(const std::string_view host_id, const std::uint64_t sequence) {
    std::ostringstream key;
    key << host_id << '#' << sequence;
    return key.str();
}

common::StatusOr<std::uint64_t> InMemoryIngestStore::get_checkpoint(const std::string_view host_id) const {
    if (host_id.empty()) {
        return common::StatusOr<std::uint64_t>::failure(common::ErrorCode::InvalidArgument, "Host id is empty.");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = checkpoints_.find(std::string(host_id));
    if (it == checkpoints_.end()) {
        return common::StatusOr<std::uint64_t>::success(0U);
    }
    return common::StatusOr<std::uint64_t>::success(it->second);
}

common::StatusOr<bool> InMemoryIngestStore::has_record(const std::string_view host_id, const std::uint64_t sequence) const {
    if (host_id.empty() || sequence == 0U) {
        return common::StatusOr<bool>::failure(common::ErrorCode::InvalidArgument, "Host id or sequence is invalid.");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto key = make_key(host_id, sequence);
    return common::StatusOr<bool>::success(records_.contains(key));
}

common::Status InMemoryIngestStore::append_if_absent_and_checkpoint(const IngestLogRecord& record) {
    if (record.host_id.empty() || record.sequence == 0U) {
        return common::Status::failure(common::ErrorCode::InvalidArgument, "Host id or sequence is invalid.");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto key = make_key(record.host_id, record.sequence);
    records_.try_emplace(key, record);

    auto& checkpoint = checkpoints_[record.host_id];
    if (record.sequence > checkpoint) {
        checkpoint = record.sequence;
    }
    return common::Status::success();
}

} // namespace localedge::storage
