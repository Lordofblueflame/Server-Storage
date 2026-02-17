// Core domain models.
// This layer is intentionally pure data + invariants, with no I/O behavior.

#ifndef HOSTINDEXER_MODELS_HPP
#define HOSTINDEXER_MODELS_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace host_indexer::domain {

using EntryId = std::uint64_t;
using SnapshotId = std::uint64_t;
using Hash64 = std::uint64_t;
using UnixTimestamp = std::int64_t;

struct SchemaHeader {
    std::uint16_t major {1};
    std::uint16_t minor {0};
    std::uint16_t patch {0};
    std::uint16_t min_reader_major {1};
};

inline constexpr SchemaHeader kCurrentSchemaHeader {
    1, 0, 0, 1
};

enum class EntryType : std::uint8_t {
    File = 0,
    Directory = 1,
    Symlink = 2,
    Special = 3
};

struct FileMetadata {
    std::uint64_t size_bytes {0};
    UnixTimestamp created_at {0};
    UnixTimestamp modified_at {0};
    UnixTimestamp accessed_at {0};
    std::uint32_t permissions {0};
    std::uint64_t file_id {0};
    std::uint64_t device_id {0};
    std::optional<Hash64> content_hash {};
};

struct FileEntry {
    EntryId id {0};
    EntryId parent_id {0};
    std::string normalized_path {};
    std::string name {};
    EntryType type {EntryType::File};
    FileMetadata metadata {};
};

struct Snapshot {
    SchemaHeader schema {};
    SnapshotId snapshot_id {0};
    UnixTimestamp created_at {0};
    std::string host_identifier {};
    EntryId root_entry_id {0};
    std::vector<FileEntry> entries {};
};

struct AddedEntry {
    FileEntry entry {};
};

struct RemovedEntry {
    FileEntry entry {};
};

struct ModifiedEntry {
    EntryId entry_id {0};
    FileMetadata before {};
    FileMetadata after {};
};

struct RenamedEntry {
    EntryId before_entry_id {0};
    EntryId after_entry_id {0};
    std::string old_path {};
    std::string new_path {};
};

struct DeltaSnapshot {
    SchemaHeader schema {};
    SnapshotId base_snapshot_id {0};
    SnapshotId target_snapshot_id {0};
    std::vector<AddedEntry> added_entries {};
    std::vector<RemovedEntry> removed_entries {};
    std::vector<ModifiedEntry> modified_entries {};
    std::vector<RenamedEntry> renamed_entries {};
};

inline bool metadata_equals(const FileMetadata& lhs, const FileMetadata& rhs) noexcept {
    return lhs.size_bytes == rhs.size_bytes &&
           lhs.created_at == rhs.created_at &&
           lhs.modified_at == rhs.modified_at &&
           lhs.accessed_at == rhs.accessed_at &&
           lhs.permissions == rhs.permissions &&
           lhs.file_id == rhs.file_id &&
           lhs.device_id == rhs.device_id &&
           lhs.content_hash == rhs.content_hash;
}

inline bool structural_entry_equals(const FileEntry& lhs, const FileEntry& rhs) noexcept {
    return lhs.id == rhs.id &&
           lhs.parent_id == rhs.parent_id &&
           lhs.normalized_path == rhs.normalized_path &&
           lhs.name == rhs.name &&
           lhs.type == rhs.type &&
           metadata_equals(lhs.metadata, rhs.metadata);
}

} // namespace host_indexer::domain

#endif // HOSTINDEXER_MODELS_HPP
