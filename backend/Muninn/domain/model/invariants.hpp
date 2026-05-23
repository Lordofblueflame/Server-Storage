#ifndef MUNINN_INVARIANTS_HPP
#define MUNINN_INVARIANTS_HPP

#include <filesystem>
#include <string>
#include <string_view>

#include "models.hpp"

namespace muninn::domain {

inline bool is_valid_schema(const SchemaHeader& schema) noexcept {
    if (schema.major == 0) {
        return false;
    }
    if (schema.min_reader_major == 0) {
        return false;
    }
    return schema.min_reader_major <= schema.major;
}

inline bool is_non_zero_snapshot_id(const SnapshotId id) noexcept {
    return id != 0;
}

inline bool is_non_zero_entry_id(const EntryId id) noexcept {
    return id != 0;
}

inline bool is_absolute_normalized_path(const std::string_view path) {
    if (path.empty()) {
        return false;
    }

    const std::filesystem::path fs_path(path);
    if (!fs_path.is_absolute()) {
        return false;
    }

    const auto normalized = fs_path.lexically_normal().generic_string();
    return normalized == path;
}

inline std::string normalize_path(const std::string_view path) {
    return std::filesystem::path(path).lexically_normal().generic_string();
}

} // namespace muninn::domain

#endif // MUNINN_INVARIANTS_HPP
