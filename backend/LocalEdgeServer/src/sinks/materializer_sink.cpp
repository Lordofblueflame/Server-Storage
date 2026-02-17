#include "materializer_sink.hpp"

#include <algorithm>
#include <filesystem>

namespace localedge::sinks {
namespace {

std::string normalize_path_lexical(const std::string_view path) {
    return std::filesystem::path(path).lexically_normal().generic_string();
}

std::string filename_from_path(const std::string_view path) {
    return std::filesystem::path(path).filename().generic_string();
}

std::string parent_from_path(const std::string_view path) {
    return std::filesystem::path(path).parent_path().generic_string();
}

void index_entry(HostMaterializedState& state, const host_indexer::domain::FileEntry& entry) {
    state.id_by_path[entry.normalized_path] = entry.id;
    state.entries_by_id[entry.id] = entry;
}

void remove_entry(HostMaterializedState& state, const host_indexer::domain::FileEntry& entry) {
    state.id_by_path.erase(entry.normalized_path);
    state.entries_by_id.erase(entry.id);
}

} // namespace

HostMaterializedState MaterializerSink::build_from_snapshot(const host_indexer::domain::Snapshot& snapshot,
                                                            const std::uint64_t sequence) {
    HostMaterializedState state;
    state.last_sequence = sequence;
    state.last_snapshot_id = snapshot.snapshot_id;
    state.host_identifier = snapshot.host_identifier;
    state.entries_by_id.reserve(snapshot.entries.size());
    state.id_by_path.reserve(snapshot.entries.size());
    for (const auto& entry : snapshot.entries) {
        index_entry(state, entry);
    }
    return state;
}

common::Status MaterializerSink::apply_snapshot(const std::string_view host_id,
                                                const std::uint64_t sequence,
                                                const host_indexer::domain::Snapshot& snapshot) {
    std::string resolved_host_id = std::string(host_id);
    if (resolved_host_id.empty()) {
        resolved_host_id = snapshot.host_identifier;
    }
    if (resolved_host_id.empty()) {
        return common::Status::failure(common::ErrorCode::InvalidArgument, "Host id is empty.");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    states_[resolved_host_id] = build_from_snapshot(snapshot, sequence);
    last_updated_host_id_ = resolved_host_id;
    return common::Status::success();
}

common::Status MaterializerSink::apply_delta(const std::string_view host_id,
                                             const std::uint64_t sequence,
                                             const host_indexer::domain::DeltaSnapshot& delta) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto state_it = states_.end();
    std::string resolved_host_id(host_id);
    if (!resolved_host_id.empty()) {
        state_it = states_.find(resolved_host_id);
    }
    if (state_it == states_.end()) {
        // Delta payload does not carry host id. If host_id is not provided,
        // resolve state by matching base snapshot id.
        for (auto it = states_.begin(); it != states_.end(); ++it) {
            if (it->second.last_snapshot_id == delta.base_snapshot_id) {
                state_it = it;
                resolved_host_id = it->first;
                break;
            }
        }
    }
    if (state_it == states_.end()) {
        return common::Status::failure(
            common::ErrorCode::NotFound,
            "Cannot apply delta without an existing materialized snapshot state."
        );
    }
    auto& state = state_it->second;

    if (state.last_snapshot_id != delta.base_snapshot_id) {
        return common::Status::failure(
            common::ErrorCode::ValidationError,
            "Delta base snapshot id does not match current materialized state."
        );
    }

    for (const auto& removed : delta.removed_entries) {
        remove_entry(state, removed.entry);
    }
    for (const auto& added : delta.added_entries) {
        index_entry(state, added.entry);
    }
    for (const auto& modified : delta.modified_entries) {
        const auto it = state.entries_by_id.find(modified.entry_id);
        if (it == state.entries_by_id.end()) {
            return common::Status::failure(
                common::ErrorCode::ValidationError,
                "Delta modified entry id is missing in materialized state."
            );
        }
        it->second.metadata = modified.after;
    }
    for (const auto& renamed : delta.renamed_entries) {
        auto it = state.entries_by_id.find(renamed.before_entry_id);
        if (it == state.entries_by_id.end()) {
            it = state.entries_by_id.find(renamed.after_entry_id);
        }
        if (it == state.entries_by_id.end()) {
            return common::Status::failure(
                common::ErrorCode::ValidationError,
                "Delta renamed entry id is missing in materialized state."
            );
        }

        host_indexer::domain::FileEntry renamed_entry = it->second;
        const std::string normalized_new_path = normalize_path_lexical(renamed.new_path);
        state.id_by_path.erase(it->second.normalized_path);
        renamed_entry.id = renamed.after_entry_id;
        renamed_entry.normalized_path = normalized_new_path;
        state.entries_by_id[renamed.after_entry_id] = std::move(renamed_entry);
        if (renamed.after_entry_id != renamed.before_entry_id) {
            state.entries_by_id.erase(renamed.before_entry_id);
        }
        state.id_by_path[normalized_new_path] = renamed.after_entry_id;
    }
    for (const auto& renamed : delta.renamed_entries) {
        const auto it = state.entries_by_id.find(renamed.after_entry_id);
        if (it == state.entries_by_id.end()) {
            return common::Status::failure(
                common::ErrorCode::ValidationError,
                "Delta renamed entry target id is missing in materialized state."
            );
        }

        auto& entry = it->second;
        entry.name = filename_from_path(entry.normalized_path);

        const auto parent_path = parent_from_path(entry.normalized_path);
        if (parent_path.empty() || parent_path == entry.normalized_path) {
            entry.parent_id = 0;
            continue;
        }

        const auto parent_id_it = state.id_by_path.find(parent_path);
        if (parent_id_it == state.id_by_path.end()) {
            entry.parent_id = 0;
            continue;
        }
        entry.parent_id = parent_id_it->second;
    }

    state.last_snapshot_id = delta.target_snapshot_id;
    state.last_sequence = sequence;
    last_updated_host_id_ = resolved_host_id;
    return common::Status::success();
}

common::Status MaterializerSink::flush() {
    return common::Status::success();
}

common::StatusOr<std::string> MaterializerSink::latest_host_id() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (last_updated_host_id_.empty() || !states_.contains(last_updated_host_id_)) {
        return common::StatusOr<std::string>::failure(common::ErrorCode::NotFound, "Host state not found.");
    }
    return common::StatusOr<std::string>::success(last_updated_host_id_);
}

common::StatusOr<std::uint64_t> MaterializerSink::last_snapshot_id(const std::string_view host_id) const {
    if (host_id.empty()) {
        return common::StatusOr<std::uint64_t>::failure(common::ErrorCode::InvalidArgument, "Host id is empty.");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = states_.find(std::string(host_id));
    if (it == states_.end()) {
        return common::StatusOr<std::uint64_t>::failure(common::ErrorCode::NotFound, "Host state not found.");
    }
    return common::StatusOr<std::uint64_t>::success(it->second.last_snapshot_id);
}

common::StatusOr<std::vector<host_indexer::domain::FileEntry>> MaterializerSink::list_children(
    const std::string_view host_id,
    const std::string_view parent_path) const {
    if (host_id.empty()) {
        return common::StatusOr<std::vector<host_indexer::domain::FileEntry>>::failure(
            common::ErrorCode::InvalidArgument,
            "Host id is empty."
        );
    }

    const std::string normalized_parent = parent_path.empty() ? "/" : std::string(parent_path);
    std::lock_guard<std::mutex> lock(mutex_);
    const auto state_it = states_.find(std::string(host_id));
    if (state_it == states_.end()) {
        return common::StatusOr<std::vector<host_indexer::domain::FileEntry>>::failure(
            common::ErrorCode::NotFound,
            "Host state not found."
        );
    }
    const auto& state = state_it->second;

    const auto parent_id_it = state.id_by_path.find(normalized_parent);
    if (parent_id_it == state.id_by_path.end()) {
        return common::StatusOr<std::vector<host_indexer::domain::FileEntry>>::failure(
            common::ErrorCode::NotFound,
            "Parent path not found in materialized state."
        );
    }

    const auto parent_entry_it = state.entries_by_id.find(parent_id_it->second);
    if (parent_entry_it == state.entries_by_id.end()) {
        return common::StatusOr<std::vector<host_indexer::domain::FileEntry>>::failure(
            common::ErrorCode::NotFound,
            "Parent entry not found in materialized state."
        );
    }

    if (parent_entry_it->second.type != host_indexer::domain::EntryType::Directory) {
        return common::StatusOr<std::vector<host_indexer::domain::FileEntry>>::failure(
            common::ErrorCode::ValidationError,
            "Parent path is not a directory."
        );
    }

    std::vector<host_indexer::domain::FileEntry> children;
    for (const auto& [entry_id, entry] : state.entries_by_id) {
        (void)entry_id;
        if (entry.parent_id == parent_entry_it->second.id) {
            children.push_back(entry);
        }
    }

    std::sort(
        children.begin(),
        children.end(),
        [](const host_indexer::domain::FileEntry& lhs, const host_indexer::domain::FileEntry& rhs) {
            const bool lhs_directory = lhs.type == host_indexer::domain::EntryType::Directory;
            const bool rhs_directory = rhs.type == host_indexer::domain::EntryType::Directory;
            if (lhs_directory != rhs_directory) {
                return lhs_directory;
            }
            return lhs.name < rhs.name;
        }
    );

    return common::StatusOr<std::vector<host_indexer::domain::FileEntry>>::success(std::move(children));
}

common::StatusOr<std::size_t> MaterializerSink::entry_count(const std::string_view host_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = states_.find(std::string(host_id));
    if (it == states_.end()) {
        return common::StatusOr<std::size_t>::failure(common::ErrorCode::NotFound, "Host state not found.");
    }
    return common::StatusOr<std::size_t>::success(it->second.entries_by_id.size());
}

} // namespace localedge::sinks
