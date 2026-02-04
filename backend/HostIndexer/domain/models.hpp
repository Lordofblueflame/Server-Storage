//
// Created by Dell on 4.02.2026.
//

#ifndef HOSTINDEXER_MODELS_HPP
#define HOSTINDEXER_MODELS_HPP

#include <filesystem>
#include <chrono>
#include <optional>
#include <unordered_map>

namespace models {

    // ========= Enums ==========
    enum EntryType : std::uint8_t{
        File,
        Directory,
        Symlink,
        Special
    };

    // Common Types & Aliases
    using TimePoint = std::chrono::time_point<std::chrono::system_clock>;
    using EntryId = std::uint64_t;
    using SnapshotId = std::uint64_t;
    using Hash64 = std::uint64_t;

    // Core Data

    struct FileEntry {
        EntryId id {0};
        EntryId parent_id {0};

        std::filesystem::path path;
        std::string name;

        EntryType type {EntryType::File};

        std::uint64_t size_bytes {0};
        std::uint64_t inode {0};
        std::uint64_t device_id {0};

        std::uint32_t permissions {0};
        std::uint32_t owner_uid {0};
        std::uint32_t owner_gid {0};

        TimePoint created_at {};
        TimePoint modified_at {};
        TimePoint accessed_at {};

        std::optional<uint64_t> content_hash;
    };

    struct SnapshotEntry {
        SnapshotId snapshot_id {0};
        TimePoint created_at {};

        std::uint32_t schema_version {1};
        std::uint32_t compatible_min_version {1};

        EntryId root_id {0};
        std::uint64_t entry_count{0};

        std::vector<FileEntry> entries;
    };

    struct ModifiedEntry {
        EntryId entry_id {0};
        FileEntry old_entry;
        FileEntry new_entry;
    };

    struct Delta {
        SnapshotId base_snapshot {0}; // snapshot_id
        SnapshotId target_snapshot {0};

        std::vector<FileEntry> added;
        std::vector<EntryId> removed; // entry_id
        std::vector<ModifiedEntry> modified;
    };

    struct DirectoryNode {
        EntryId dir_id {0};
        std::vector<EntryId> child_dirs;
        std::vector<EntryId> child_files;
    };

    // Metadata

    struct HostInfo {
        std::string hostname;
        std::string os;
        std::string filesystem_type;
    };

    struct IndexMetadata {
        std::string index_uuid;

        TimePoint created_at {};
        TimePoint last_scan_at {};

        HostInfo host_info;

        SnapshotId current_snapshot_id {0};
    };

    // Auxiliary Indexes

    using PathIndex = std::unordered_map<Hash64, EntryId>;
    using CurrentIndex = std::unordered_map<Hash64, std::vector<EntryId>>;

}

#endif //HOSTINDEXER_MODELS_HPP