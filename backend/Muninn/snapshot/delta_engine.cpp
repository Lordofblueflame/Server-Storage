#include "delta_engine.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../utils/hash.hpp"

namespace muninn::snapshot {
namespace {

struct RenameFingerprint {
    domain::EntryType type {domain::EntryType::File};
    std::uint64_t device_id {0};
    std::uint64_t file_id {0};
    std::uint64_t size_bytes {0};
    domain::UnixTimestamp modified_at {0};
    std::uint64_t name_hash {0};

    bool operator==(const RenameFingerprint& other) const noexcept {
        return type == other.type &&
               device_id == other.device_id &&
               file_id == other.file_id &&
               size_bytes == other.size_bytes &&
               modified_at == other.modified_at &&
               name_hash == other.name_hash;
    }
};

struct RenameFingerprintHasher {
    std::size_t operator()(const RenameFingerprint& value) const noexcept {
        std::uint64_t seed = static_cast<std::uint64_t>(value.type);
        seed = utils::combine_hash(seed, value.device_id);
        seed = utils::combine_hash(seed, value.file_id);
        seed = utils::combine_hash(seed, value.size_bytes);
        seed = utils::combine_hash(seed, static_cast<std::uint64_t>(value.modified_at));
        seed = utils::combine_hash(seed, value.name_hash);
        return static_cast<std::size_t>(seed);
    }
};

RenameFingerprint make_rename_fingerprint(const domain::FileEntry& entry) {
    RenameFingerprint fingerprint;
    fingerprint.type = entry.type;
    fingerprint.size_bytes = entry.metadata.size_bytes;
    fingerprint.modified_at = entry.metadata.modified_at;
    fingerprint.name_hash = utils::fnv1a64(entry.name);

    if (entry.metadata.device_id != 0 && entry.metadata.file_id != 0) {
        fingerprint.device_id = entry.metadata.device_id;
        fingerprint.file_id = entry.metadata.file_id;
    }
    return fingerprint;
}

std::unordered_map<domain::EntryId, const domain::FileEntry*> index_by_id(const domain::Snapshot& snapshot) {
    std::unordered_map<domain::EntryId, const domain::FileEntry*> by_id;
    by_id.reserve(snapshot.entries.size());
    for (const auto& entry : snapshot.entries) {
        by_id.emplace(entry.id, &entry);
    }
    return by_id;
}

void sort_delta(domain::DeltaSnapshot& delta) {
    std::sort(
        delta.added_entries.begin(),
        delta.added_entries.end(),
        [](const domain::AddedEntry& lhs, const domain::AddedEntry& rhs) {
            if (lhs.entry.normalized_path == rhs.entry.normalized_path) {
                return lhs.entry.id < rhs.entry.id;
            }
            return lhs.entry.normalized_path < rhs.entry.normalized_path;
        }
    );

    std::sort(
        delta.removed_entries.begin(),
        delta.removed_entries.end(),
        [](const domain::RemovedEntry& lhs, const domain::RemovedEntry& rhs) {
            if (lhs.entry.normalized_path == rhs.entry.normalized_path) {
                return lhs.entry.id < rhs.entry.id;
            }
            return lhs.entry.normalized_path < rhs.entry.normalized_path;
        }
    );

    std::sort(
        delta.modified_entries.begin(),
        delta.modified_entries.end(),
        [](const domain::ModifiedEntry& lhs, const domain::ModifiedEntry& rhs) {
            return lhs.entry_id < rhs.entry_id;
        }
    );

    std::sort(
        delta.renamed_entries.begin(),
        delta.renamed_entries.end(),
        [](const domain::RenamedEntry& lhs, const domain::RenamedEntry& rhs) {
            if (lhs.old_path == rhs.old_path) {
                if (lhs.new_path == rhs.new_path) {
                    if (lhs.before_entry_id == rhs.before_entry_id) {
                        return lhs.after_entry_id < rhs.after_entry_id;
                    }
                    return lhs.before_entry_id < rhs.before_entry_id;
                }
                return lhs.new_path < rhs.new_path;
            }
            return lhs.old_path < rhs.old_path;
        }
    );
}

} // namespace

domain::DeltaSnapshot DeltaEngine::compute(const domain::Snapshot& base_snapshot,
                                           const domain::Snapshot& target_snapshot,
                                           const DeltaOptions& options) const {
    domain::DeltaSnapshot delta;
    delta.schema = domain::kCurrentSchemaHeader;
    delta.base_snapshot_id = base_snapshot.snapshot_id;
    delta.target_snapshot_id = target_snapshot.snapshot_id;

    const auto base_by_id = index_by_id(base_snapshot);
    const auto target_by_id = index_by_id(target_snapshot);

    std::vector<const domain::FileEntry*> unmatched_removed;
    unmatched_removed.reserve(base_snapshot.entries.size());

    std::vector<const domain::FileEntry*> unmatched_added;
    unmatched_added.reserve(target_snapshot.entries.size());

    for (const auto& base_entry : base_snapshot.entries) {
        const auto target_it = target_by_id.find(base_entry.id);
        if (target_it == target_by_id.end()) {
            unmatched_removed.push_back(&base_entry);
            continue;
        }

        const domain::FileEntry& target_entry = *target_it->second;
        if (base_entry.normalized_path != target_entry.normalized_path) {
            delta.renamed_entries.push_back(
                domain::RenamedEntry {
                    base_entry.id,
                    target_entry.id,
                    base_entry.normalized_path,
                    target_entry.normalized_path
                }
            );
        }

        if (!domain::metadata_equals(base_entry.metadata, target_entry.metadata) ||
            base_entry.type != target_entry.type) {
            delta.modified_entries.push_back(
                domain::ModifiedEntry {
                    target_entry.id,
                    base_entry.metadata,
                    target_entry.metadata
                }
            );
        }
    }

    for (const auto& target_entry : target_snapshot.entries) {
        if (!base_by_id.contains(target_entry.id)) {
            unmatched_added.push_back(&target_entry);
        }
    }

    if (options.enable_rename_heuristic) {
        std::sort(
            unmatched_removed.begin(),
            unmatched_removed.end(),
            [](const domain::FileEntry* lhs, const domain::FileEntry* rhs) {
                return lhs->normalized_path < rhs->normalized_path;
            }
        );

        std::sort(
            unmatched_added.begin(),
            unmatched_added.end(),
            [](const domain::FileEntry* lhs, const domain::FileEntry* rhs) {
                return lhs->normalized_path < rhs->normalized_path;
            }
        );

        std::unordered_map<RenameFingerprint, std::vector<std::size_t>, RenameFingerprintHasher> added_by_fingerprint;
        added_by_fingerprint.reserve(unmatched_added.size());
        for (std::size_t i = 0; i < unmatched_added.size(); ++i) {
            added_by_fingerprint[make_rename_fingerprint(*unmatched_added[i])].push_back(i);
        }

        std::vector<bool> consumed_added(unmatched_added.size(), false);
        std::vector<bool> consumed_removed(unmatched_removed.size(), false);

        for (std::size_t removed_index = 0; removed_index < unmatched_removed.size(); ++removed_index) {
            const auto* removed_entry = unmatched_removed[removed_index];
            const auto fingerprint = make_rename_fingerprint(*removed_entry);
            const auto bucket_it = added_by_fingerprint.find(fingerprint);
            if (bucket_it == added_by_fingerprint.end()) {
                continue;
            }

            auto& candidates = bucket_it->second;
            auto selected_it = std::find_if(
                candidates.begin(),
                candidates.end(),
                [&](const std::size_t candidate_index) {
                    return !consumed_added[candidate_index];
                }
            );
            if (selected_it == candidates.end()) {
                continue;
            }

            consumed_removed[removed_index] = true;
            consumed_added[*selected_it] = true;

            const auto* added_entry = unmatched_added[*selected_it];
            delta.renamed_entries.push_back(
                domain::RenamedEntry {
                    removed_entry->id,
                    added_entry->id,
                    removed_entry->normalized_path,
                    added_entry->normalized_path
                }
            );
        }

        for (std::size_t i = 0; i < unmatched_removed.size(); ++i) {
            if (!consumed_removed[i]) {
                delta.removed_entries.push_back(domain::RemovedEntry {*unmatched_removed[i]});
            }
        }

        for (std::size_t i = 0; i < unmatched_added.size(); ++i) {
            if (!consumed_added[i]) {
                delta.added_entries.push_back(domain::AddedEntry {*unmatched_added[i]});
            }
        }
    } else {
        for (const auto* removed_entry : unmatched_removed) {
            delta.removed_entries.push_back(domain::RemovedEntry {*removed_entry});
        }
        for (const auto* added_entry : unmatched_added) {
            delta.added_entries.push_back(domain::AddedEntry {*added_entry});
        }
    }

    sort_delta(delta);
    return delta;
}

domain::DeltaSnapshot DeltaEngine::invert(const domain::DeltaSnapshot& delta) const {
    domain::DeltaSnapshot inverse;
    inverse.schema = delta.schema;
    inverse.base_snapshot_id = delta.target_snapshot_id;
    inverse.target_snapshot_id = delta.base_snapshot_id;

    inverse.added_entries.reserve(delta.removed_entries.size());
    for (const auto& removed : delta.removed_entries) {
        inverse.added_entries.push_back(domain::AddedEntry {removed.entry});
    }

    inverse.removed_entries.reserve(delta.added_entries.size());
    for (const auto& added : delta.added_entries) {
        inverse.removed_entries.push_back(domain::RemovedEntry {added.entry});
    }

    inverse.modified_entries.reserve(delta.modified_entries.size());
    for (const auto& modified : delta.modified_entries) {
        inverse.modified_entries.push_back(
            domain::ModifiedEntry {
                modified.entry_id,
                modified.after,
                modified.before
            }
        );
    }

    inverse.renamed_entries.reserve(delta.renamed_entries.size());
    for (const auto& renamed : delta.renamed_entries) {
        inverse.renamed_entries.push_back(
            domain::RenamedEntry {
                renamed.after_entry_id,
                renamed.before_entry_id,
                renamed.new_path,
                renamed.old_path
            }
        );
    }

    sort_delta(inverse);
    return inverse;
}

} // namespace muninn::snapshot
