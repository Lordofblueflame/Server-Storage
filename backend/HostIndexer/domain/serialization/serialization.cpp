#include "serialization.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>

namespace host_indexer::serialization {
namespace {

using json = nlohmann::json;

bool write_schema(BinaryWriter& writer, const domain::SchemaHeader& schema) {
    return writer.write_u16(schema.major) &&
           writer.write_u16(schema.minor) &&
           writer.write_u16(schema.patch) &&
           writer.write_u16(schema.min_reader_major);
}

bool read_schema(BinaryReader& reader, domain::SchemaHeader& schema) {
    return reader.read_u16(schema.major) &&
           reader.read_u16(schema.minor) &&
           reader.read_u16(schema.patch) &&
           reader.read_u16(schema.min_reader_major);
}

bool write_metadata(BinaryWriter& writer, const domain::FileMetadata& metadata) {
    const std::uint8_t has_hash = metadata.content_hash.has_value() ? 1 : 0;
    return writer.write_u64(metadata.size_bytes) &&
           writer.write_i64(metadata.created_at) &&
           writer.write_i64(metadata.modified_at) &&
           writer.write_i64(metadata.accessed_at) &&
           writer.write_u32(metadata.permissions) &&
           writer.write_u64(metadata.file_id) &&
           writer.write_u64(metadata.device_id) &&
           writer.write_u8(has_hash) &&
           (!has_hash || writer.write_u64(*metadata.content_hash));
}

bool read_metadata(BinaryReader& reader, domain::FileMetadata& metadata) {
    std::uint8_t has_hash = 0;
    if (!reader.read_u64(metadata.size_bytes) ||
        !reader.read_i64(metadata.created_at) ||
        !reader.read_i64(metadata.modified_at) ||
        !reader.read_i64(metadata.accessed_at) ||
        !reader.read_u32(metadata.permissions) ||
        !reader.read_u64(metadata.file_id) ||
        !reader.read_u64(metadata.device_id) ||
        !reader.read_u8(has_hash)) {
        return false;
    }

    if (has_hash != 0) {
        std::uint64_t hash = 0;
        if (!reader.read_u64(hash)) {
            return false;
        }
        metadata.content_hash = hash;
    } else {
        metadata.content_hash.reset();
    }
    return true;
}

bool write_entry(BinaryWriter& writer, const domain::FileEntry& entry) {
    return writer.write_u64(entry.id) &&
           writer.write_u64(entry.parent_id) &&
           writer.write_string(entry.normalized_path) &&
           writer.write_string(entry.name) &&
           writer.write_u8(static_cast<std::uint8_t>(entry.type)) &&
           write_metadata(writer, entry.metadata);
}

bool read_entry(BinaryReader& reader, domain::FileEntry& entry) {
    std::uint8_t raw_type = 0;
    if (!reader.read_u64(entry.id) ||
        !reader.read_u64(entry.parent_id) ||
        !reader.read_string(entry.normalized_path) ||
        !reader.read_string(entry.name) ||
        !reader.read_u8(raw_type) ||
        !read_metadata(reader, entry.metadata)) {
        return false;
    }
    entry.type = static_cast<domain::EntryType>(raw_type);
    return true;
}

bool write_binary_header(BinaryWriter& writer, const std::uint32_t magic) {
    return writer.write_u32(magic) &&
           writer.write_u16(kBinaryFormatMajor) &&
           writer.write_u16(kBinaryFormatMinor);
}

bool read_binary_header(BinaryReader& reader, const std::uint32_t expected_magic) {
    std::uint32_t magic = 0;
    std::uint16_t format_major = 0;
    std::uint16_t format_minor = 0;

    if (!reader.read_u32(magic) ||
        !reader.read_u16(format_major) ||
        !reader.read_u16(format_minor)) {
        return false;
    }

    if (magic != expected_magic) {
        return false;
    }

    return format_major == kBinaryFormatMajor && format_minor == kBinaryFormatMinor;
}

std::string entry_type_to_string(const domain::EntryType type) {
    switch (type) {
        case domain::EntryType::File:
            return "file";
        case domain::EntryType::Directory:
            return "directory";
        case domain::EntryType::Symlink:
            return "symlink";
        case domain::EntryType::Special:
            return "special";
    }
    return "special";
}

bool parse_entry_type(const std::string& value, domain::EntryType& type) {
    if (value == "file") {
        type = domain::EntryType::File;
        return true;
    }
    if (value == "directory") {
        type = domain::EntryType::Directory;
        return true;
    }
    if (value == "symlink") {
        type = domain::EntryType::Symlink;
        return true;
    }
    if (value == "special") {
        type = domain::EntryType::Special;
        return true;
    }
    return false;
}

template <typename T>
bool extract_required_numeric(const json& object, const char* key, T& value, SerializationResult& result) {
    static_assert(std::is_integral_v<T>);
    if (!object.contains(key)) {
        result = SerializationResult::failure(
            SerializationErrorCode::JsonFieldMissing,
            std::string("Missing JSON field: ") + key
        );
        return false;
    }

    const auto& field = object.at(key);
    if constexpr (std::is_unsigned_v<T>) {
        if (!field.is_number_unsigned()) {
            result = SerializationResult::failure(
                SerializationErrorCode::JsonTypeMismatch,
                std::string("Field has wrong type: ") + key
            );
            return false;
        }
        value = field.get<T>();
    } else {
        if (!field.is_number_integer()) {
            result = SerializationResult::failure(
                SerializationErrorCode::JsonTypeMismatch,
                std::string("Field has wrong type: ") + key
            );
            return false;
        }
        value = field.get<T>();
    }
    return true;
}

bool extract_required_string(const json& object,
                             const char* key,
                             std::string& value,
                             SerializationResult& result) {
    if (!object.contains(key)) {
        result = SerializationResult::failure(
            SerializationErrorCode::JsonFieldMissing,
            std::string("Missing JSON field: ") + key
        );
        return false;
    }
    const auto& field = object.at(key);
    if (!field.is_string()) {
        result = SerializationResult::failure(
            SerializationErrorCode::JsonTypeMismatch,
            std::string("Field has wrong type: ") + key
        );
        return false;
    }
    value = field.get<std::string>();
    return true;
}

json metadata_to_json(const domain::FileMetadata& metadata) {
    json j = {
        {"size_bytes", metadata.size_bytes},
        {"created_at", metadata.created_at},
        {"modified_at", metadata.modified_at},
        {"accessed_at", metadata.accessed_at},
        {"permissions", metadata.permissions},
        {"file_id", metadata.file_id},
        {"device_id", metadata.device_id}
    };

    if (metadata.content_hash.has_value()) {
        j["content_hash"] = *metadata.content_hash;
    }
    return j;
}

SerializationResult metadata_from_json(const json& j, domain::FileMetadata& metadata) {
    SerializationResult result = SerializationResult::success();

    if (!extract_required_numeric(j, "size_bytes", metadata.size_bytes, result) ||
        !extract_required_numeric(j, "created_at", metadata.created_at, result) ||
        !extract_required_numeric(j, "modified_at", metadata.modified_at, result) ||
        !extract_required_numeric(j, "accessed_at", metadata.accessed_at, result) ||
        !extract_required_numeric(j, "permissions", metadata.permissions, result) ||
        !extract_required_numeric(j, "file_id", metadata.file_id, result) ||
        !extract_required_numeric(j, "device_id", metadata.device_id, result)) {
        return result;
    }

    if (j.contains("content_hash")) {
        if (!j.at("content_hash").is_number_unsigned()) {
            return SerializationResult::failure(
                SerializationErrorCode::JsonTypeMismatch,
                "Field has wrong type: content_hash"
            );
        }
        metadata.content_hash = j.at("content_hash").get<std::uint64_t>();
    } else {
        metadata.content_hash.reset();
    }

    return SerializationResult::success();
}

json entry_to_json(const domain::FileEntry& entry) {
    return json {
        {"id", entry.id},
        {"parent_id", entry.parent_id},
        {"path", entry.normalized_path},
        {"name", entry.name},
        {"type", entry_type_to_string(entry.type)},
        {"metadata", metadata_to_json(entry.metadata)}
    };
}

SerializationResult entry_from_json(const json& j, domain::FileEntry& entry) {
    SerializationResult result = SerializationResult::success();
    std::string type_string;

    if (!extract_required_numeric(j, "id", entry.id, result) ||
        !extract_required_numeric(j, "parent_id", entry.parent_id, result) ||
        !extract_required_string(j, "path", entry.normalized_path, result) ||
        !extract_required_string(j, "name", entry.name, result) ||
        !extract_required_string(j, "type", type_string, result)) {
        return result;
    }

    if (!parse_entry_type(type_string, entry.type)) {
        return SerializationResult::failure(
            SerializationErrorCode::InvalidValue,
            "Invalid entry type value."
        );
    }

    if (!j.contains("metadata") || !j.at("metadata").is_object()) {
        return SerializationResult::failure(
            SerializationErrorCode::JsonFieldMissing,
            "Missing or invalid JSON field: metadata"
        );
    }

    return metadata_from_json(j.at("metadata"), entry.metadata);
}

json schema_to_json(const domain::SchemaHeader& schema) {
    return json {
        {"major", schema.major},
        {"minor", schema.minor},
        {"patch", schema.patch},
        {"min_reader_major", schema.min_reader_major}
    };
}

SerializationResult schema_from_json(const json& j, domain::SchemaHeader& schema) {
    SerializationResult result = SerializationResult::success();
    if (!extract_required_numeric(j, "major", schema.major, result) ||
        !extract_required_numeric(j, "minor", schema.minor, result) ||
        !extract_required_numeric(j, "patch", schema.patch, result) ||
        !extract_required_numeric(j, "min_reader_major", schema.min_reader_major, result)) {
        return result;
    }
    return SerializationResult::success();
}

json container_version_json() {
    return json {
        {"major", kBinaryFormatMajor},
        {"minor", kBinaryFormatMinor}
    };
}

bool validate_container_version(const json& object, SerializationResult& result) {
    if (!object.contains("container_version")) {
        return true;
    }
    const auto& container = object.at("container_version");
    if (!container.is_object()) {
        result = SerializationResult::failure(
            SerializationErrorCode::JsonTypeMismatch,
            "container_version must be an object."
        );
        return false;
    }

    std::uint16_t major = 0;
    std::uint16_t minor = 0;
    if (!extract_required_numeric(container, "major", major, result) ||
        !extract_required_numeric(container, "minor", minor, result)) {
        return false;
    }

    if (major != kBinaryFormatMajor || minor != kBinaryFormatMinor) {
        result = SerializationResult::failure(
            SerializationErrorCode::UnsupportedFormatVersion,
            "Unsupported container version."
        );
        return false;
    }
    return true;
}

} // namespace

SerializationResult serialize_snapshot_binary(const domain::Snapshot& snapshot, std::ostream& stream) {
    BinaryWriter writer(stream);
    if (!write_binary_header(writer, kSnapshotMagic) ||
        !write_schema(writer, snapshot.schema) ||
        !writer.write_u64(snapshot.snapshot_id) ||
        !writer.write_i64(snapshot.created_at) ||
        !writer.write_string(snapshot.host_identifier) ||
        !writer.write_u64(snapshot.root_entry_id)) {
        return writer.status();
    }

    if (snapshot.entries.size() > kMaxBinaryEntries) {
        return SerializationResult::failure(
            SerializationErrorCode::EntryCountTooLarge,
            "Snapshot entry count exceeds serialization limit."
        );
    }

    if (!writer.write_u32(static_cast<std::uint32_t>(snapshot.entries.size()))) {
        return writer.status();
    }

    for (const auto& entry : snapshot.entries) {
        if (!write_entry(writer, entry)) {
            return writer.status();
        }
    }

    return writer.status();
}

SerializationResult deserialize_snapshot_binary(std::istream& stream, domain::Snapshot& snapshot) {
    BinaryReader reader(stream);
    if (!read_binary_header(reader, kSnapshotMagic)) {
        return SerializationResult::failure(
            SerializationErrorCode::InvalidMagic,
            "Invalid snapshot binary header."
        );
    }

    std::uint32_t entry_count = 0;
    if (!read_schema(reader, snapshot.schema) ||
        !reader.read_u64(snapshot.snapshot_id) ||
        !reader.read_i64(snapshot.created_at) ||
        !reader.read_string(snapshot.host_identifier) ||
        !reader.read_u64(snapshot.root_entry_id) ||
        !reader.read_u32(entry_count)) {
        return reader.status();
    }

    if (entry_count > kMaxBinaryEntries) {
        return SerializationResult::failure(
            SerializationErrorCode::EntryCountTooLarge,
            "Serialized snapshot entry count is above limit."
        );
    }

    snapshot.entries.clear();
    snapshot.entries.reserve(entry_count);
    for (std::uint32_t i = 0; i < entry_count; ++i) {
        domain::FileEntry entry;
        if (!read_entry(reader, entry)) {
            return reader.status();
        }
        snapshot.entries.push_back(std::move(entry));
    }
    return reader.status();
}

SerializationResult serialize_delta_binary(const domain::DeltaSnapshot& delta, std::ostream& stream) {
    BinaryWriter writer(stream);
    if (!write_binary_header(writer, kDeltaMagic) ||
        !write_schema(writer, delta.schema) ||
        !writer.write_u64(delta.base_snapshot_id) ||
        !writer.write_u64(delta.target_snapshot_id)) {
        return writer.status();
    }

    if (delta.added_entries.size() > kMaxBinaryEntries ||
        delta.removed_entries.size() > kMaxBinaryEntries ||
        delta.modified_entries.size() > kMaxBinaryEntries ||
        delta.renamed_entries.size() > kMaxBinaryEntries) {
        return SerializationResult::failure(
            SerializationErrorCode::EntryCountTooLarge,
            "Delta operation count exceeds serialization limit."
        );
    }

    if (!writer.write_u32(static_cast<std::uint32_t>(delta.added_entries.size())) ||
        !writer.write_u32(static_cast<std::uint32_t>(delta.removed_entries.size())) ||
        !writer.write_u32(static_cast<std::uint32_t>(delta.modified_entries.size())) ||
        !writer.write_u32(static_cast<std::uint32_t>(delta.renamed_entries.size()))) {
        return writer.status();
    }

    for (const auto& added : delta.added_entries) {
        if (!write_entry(writer, added.entry)) {
            return writer.status();
        }
    }

    for (const auto& removed : delta.removed_entries) {
        if (!write_entry(writer, removed.entry)) {
            return writer.status();
        }
    }

    for (const auto& modified : delta.modified_entries) {
        if (!writer.write_u64(modified.entry_id) ||
            !write_metadata(writer, modified.before) ||
            !write_metadata(writer, modified.after)) {
            return writer.status();
        }
    }

    for (const auto& renamed : delta.renamed_entries) {
        if (!writer.write_u64(renamed.before_entry_id) ||
            !writer.write_u64(renamed.after_entry_id) ||
            !writer.write_string(renamed.old_path) ||
            !writer.write_string(renamed.new_path)) {
            return writer.status();
        }
    }

    return writer.status();
}

SerializationResult deserialize_delta_binary(std::istream& stream, domain::DeltaSnapshot& delta) {
    BinaryReader reader(stream);
    if (!read_binary_header(reader, kDeltaMagic)) {
        return SerializationResult::failure(
            SerializationErrorCode::InvalidMagic,
            "Invalid delta binary header."
        );
    }

    std::uint32_t added_count = 0;
    std::uint32_t removed_count = 0;
    std::uint32_t modified_count = 0;
    std::uint32_t renamed_count = 0;

    if (!read_schema(reader, delta.schema) ||
        !reader.read_u64(delta.base_snapshot_id) ||
        !reader.read_u64(delta.target_snapshot_id) ||
        !reader.read_u32(added_count) ||
        !reader.read_u32(removed_count) ||
        !reader.read_u32(modified_count) ||
        !reader.read_u32(renamed_count)) {
        return reader.status();
    }

    if (added_count > kMaxBinaryEntries ||
        removed_count > kMaxBinaryEntries ||
        modified_count > kMaxBinaryEntries ||
        renamed_count > kMaxBinaryEntries) {
        return SerializationResult::failure(
            SerializationErrorCode::EntryCountTooLarge,
            "Serialized delta operation count is above limit."
        );
    }

    delta.added_entries.clear();
    delta.removed_entries.clear();
    delta.modified_entries.clear();
    delta.renamed_entries.clear();

    delta.added_entries.reserve(added_count);
    delta.removed_entries.reserve(removed_count);
    delta.modified_entries.reserve(modified_count);
    delta.renamed_entries.reserve(renamed_count);

    for (std::uint32_t i = 0; i < added_count; ++i) {
        domain::FileEntry entry;
        if (!read_entry(reader, entry)) {
            return reader.status();
        }
        delta.added_entries.push_back(domain::AddedEntry {std::move(entry)});
    }

    for (std::uint32_t i = 0; i < removed_count; ++i) {
        domain::FileEntry entry;
        if (!read_entry(reader, entry)) {
            return reader.status();
        }
        delta.removed_entries.push_back(domain::RemovedEntry {std::move(entry)});
    }

    for (std::uint32_t i = 0; i < modified_count; ++i) {
        domain::ModifiedEntry modified;
        if (!reader.read_u64(modified.entry_id) ||
            !read_metadata(reader, modified.before) ||
            !read_metadata(reader, modified.after)) {
            return reader.status();
        }
        delta.modified_entries.push_back(std::move(modified));
    }

    for (std::uint32_t i = 0; i < renamed_count; ++i) {
        domain::RenamedEntry renamed;
        if (!reader.read_u64(renamed.before_entry_id) ||
            !reader.read_u64(renamed.after_entry_id) ||
            !reader.read_string(renamed.old_path) ||
            !reader.read_string(renamed.new_path)) {
            return reader.status();
        }
        delta.renamed_entries.push_back(std::move(renamed));
    }

    return reader.status();
}

nlohmann::json snapshot_to_json(const domain::Snapshot& snapshot) {
    json output = {
        {"container_version", container_version_json()},
        {"schema", schema_to_json(snapshot.schema)},
        {"snapshot_id", snapshot.snapshot_id},
        {"created_at", snapshot.created_at},
        {"host_identifier", snapshot.host_identifier},
        {"root_entry_id", snapshot.root_entry_id},
        {"entries", json::array()}
    };

    for (const auto& entry : snapshot.entries) {
        output["entries"].push_back(entry_to_json(entry));
    }
    return output;
}

SerializationResult snapshot_from_json(const nlohmann::json& json_input, domain::Snapshot& snapshot) {
    if (!json_input.is_object()) {
        return SerializationResult::failure(
            SerializationErrorCode::JsonTypeMismatch,
            "Snapshot JSON must be an object."
        );
    }

    SerializationResult result = SerializationResult::success();
    if (!validate_container_version(json_input, result)) {
        return result;
    }

    if (!json_input.contains("schema") || !json_input.at("schema").is_object()) {
        return SerializationResult::failure(
            SerializationErrorCode::JsonFieldMissing,
            "Missing or invalid schema object."
        );
    }

    result = schema_from_json(json_input.at("schema"), snapshot.schema);
    if (!result.ok) {
        return result;
    }

    if (!extract_required_numeric(json_input, "snapshot_id", snapshot.snapshot_id, result) ||
        !extract_required_numeric(json_input, "created_at", snapshot.created_at, result) ||
        !extract_required_string(json_input, "host_identifier", snapshot.host_identifier, result) ||
        !extract_required_numeric(json_input, "root_entry_id", snapshot.root_entry_id, result)) {
        return result;
    }

    if (!json_input.contains("entries") || !json_input.at("entries").is_array()) {
        return SerializationResult::failure(
            SerializationErrorCode::JsonFieldMissing,
            "Missing entries array."
        );
    }

    const auto& entries = json_input.at("entries");
    if (entries.size() > kMaxBinaryEntries) {
        return SerializationResult::failure(
            SerializationErrorCode::EntryCountTooLarge,
            "Snapshot JSON entries exceed supported limit."
        );
    }

    snapshot.entries.clear();
    snapshot.entries.reserve(entries.size());

    for (const auto& entry_json : entries) {
        if (!entry_json.is_object()) {
            return SerializationResult::failure(
                SerializationErrorCode::JsonTypeMismatch,
                "Snapshot entry must be an object."
            );
        }

        domain::FileEntry entry;
        result = entry_from_json(entry_json, entry);
        if (!result.ok) {
            return result;
        }
        snapshot.entries.push_back(std::move(entry));
    }

    return SerializationResult::success();
}

nlohmann::json delta_to_json(const domain::DeltaSnapshot& delta) {
    json output = {
        {"container_version", container_version_json()},
        {"schema", schema_to_json(delta.schema)},
        {"base_snapshot_id", delta.base_snapshot_id},
        {"target_snapshot_id", delta.target_snapshot_id},
        {"added", json::array()},
        {"removed", json::array()},
        {"modified", json::array()},
        {"renamed", json::array()}
    };

    for (const auto& added : delta.added_entries) {
        output["added"].push_back(entry_to_json(added.entry));
    }
    for (const auto& removed : delta.removed_entries) {
        output["removed"].push_back(entry_to_json(removed.entry));
    }
    for (const auto& modified : delta.modified_entries) {
        output["modified"].push_back(
            json {
                {"entry_id", modified.entry_id},
                {"before", metadata_to_json(modified.before)},
                {"after", metadata_to_json(modified.after)}
            }
        );
    }
    for (const auto& renamed : delta.renamed_entries) {
        output["renamed"].push_back(
            json {
                {"before_entry_id", renamed.before_entry_id},
                {"after_entry_id", renamed.after_entry_id},
                {"old_path", renamed.old_path},
                {"new_path", renamed.new_path}
            }
        );
    }

    return output;
}

SerializationResult delta_from_json(const nlohmann::json& json_input, domain::DeltaSnapshot& delta) {
    if (!json_input.is_object()) {
        return SerializationResult::failure(
            SerializationErrorCode::JsonTypeMismatch,
            "Delta JSON must be an object."
        );
    }

    SerializationResult result = SerializationResult::success();
    if (!validate_container_version(json_input, result)) {
        return result;
    }

    if (!json_input.contains("schema") || !json_input.at("schema").is_object()) {
        return SerializationResult::failure(
            SerializationErrorCode::JsonFieldMissing,
            "Missing or invalid schema object."
        );
    }
    result = schema_from_json(json_input.at("schema"), delta.schema);
    if (!result.ok) {
        return result;
    }

    if (!extract_required_numeric(json_input, "base_snapshot_id", delta.base_snapshot_id, result) ||
        !extract_required_numeric(json_input, "target_snapshot_id", delta.target_snapshot_id, result)) {
        return result;
    }

    const auto required_array_field = [&](const char* key) -> const json* {
        if (!json_input.contains(key) || !json_input.at(key).is_array()) {
            result = SerializationResult::failure(
                SerializationErrorCode::JsonFieldMissing,
                std::string("Missing array field: ") + key
            );
            return nullptr;
        }
        return &json_input.at(key);
    };

    const json* added = required_array_field("added");
    if (added == nullptr) {
        return result;
    }
    const json* removed = required_array_field("removed");
    if (removed == nullptr) {
        return result;
    }
    const json* modified = required_array_field("modified");
    if (modified == nullptr) {
        return result;
    }
    const json* renamed = required_array_field("renamed");
    if (renamed == nullptr) {
        return result;
    }

    delta.added_entries.clear();
    delta.removed_entries.clear();
    delta.modified_entries.clear();
    delta.renamed_entries.clear();

    delta.added_entries.reserve(added->size());
    for (const auto& added_item : *added) {
        domain::FileEntry entry;
        result = entry_from_json(added_item, entry);
        if (!result.ok) {
            return result;
        }
        delta.added_entries.push_back(domain::AddedEntry {std::move(entry)});
    }

    delta.removed_entries.reserve(removed->size());
    for (const auto& removed_item : *removed) {
        domain::FileEntry entry;
        result = entry_from_json(removed_item, entry);
        if (!result.ok) {
            return result;
        }
        delta.removed_entries.push_back(domain::RemovedEntry {std::move(entry)});
    }

    delta.modified_entries.reserve(modified->size());
    for (const auto& modified_item : *modified) {
        if (!modified_item.is_object()) {
            return SerializationResult::failure(
                SerializationErrorCode::JsonTypeMismatch,
                "Delta modified item must be an object."
            );
        }
        domain::ModifiedEntry entry;
        if (!extract_required_numeric(modified_item, "entry_id", entry.entry_id, result)) {
            return result;
        }
        if (!modified_item.contains("before") || !modified_item.at("before").is_object() ||
            !modified_item.contains("after") || !modified_item.at("after").is_object()) {
            return SerializationResult::failure(
                SerializationErrorCode::JsonFieldMissing,
                "Delta modified item must contain before and after metadata objects."
            );
        }
        result = metadata_from_json(modified_item.at("before"), entry.before);
        if (!result.ok) {
            return result;
        }
        result = metadata_from_json(modified_item.at("after"), entry.after);
        if (!result.ok) {
            return result;
        }
        delta.modified_entries.push_back(std::move(entry));
    }

    delta.renamed_entries.reserve(renamed->size());
    for (const auto& renamed_item : *renamed) {
        if (!renamed_item.is_object()) {
            return SerializationResult::failure(
                SerializationErrorCode::JsonTypeMismatch,
                "Delta renamed item must be an object."
            );
        }
        domain::RenamedEntry entry;
        if (!extract_required_numeric(renamed_item, "before_entry_id", entry.before_entry_id, result) ||
            !extract_required_numeric(renamed_item, "after_entry_id", entry.after_entry_id, result) ||
            !extract_required_string(renamed_item, "old_path", entry.old_path, result) ||
            !extract_required_string(renamed_item, "new_path", entry.new_path, result)) {
            return result;
        }
        delta.renamed_entries.push_back(std::move(entry));
    }

    return SerializationResult::success();
}

} // namespace host_indexer::serialization
