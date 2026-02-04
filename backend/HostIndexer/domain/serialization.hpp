//
// Created by Dell on 4.02.2026.
//

#ifndef HOSTINDEXER_SERIALIZATION_HPP
#define HOSTINDEXER_SERIALIZATION_HPP

#include <ostream>
#include <istream>
#include <sstream>

#include "models.hpp"

using namespace models;

// helper functions

template<typename T>
inline void write_pod(std::ostream& os, const T& value) {
    os.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template<typename T>
inline void read_pod(std::istream& is, T& value) {
    is.read(reinterpret_cast<char*>(&value), sizeof(T));
}

inline void write_string(std::ostream& os, const std::string& s) {
    const std::uint64_t size = s.size();
    write_pod(os, size);
    os.write(s.data(), size);
}

inline void read_string(std::istream& is, std::string& s) {
    std::uint64_t size = 0;
    read_pod(is, size);
    s.resize(size);
    is.read(s.data(), size);
}

inline void write_path(std::ostream& os, const std::filesystem::path& p) {
    write_string(os, p.string());
}

inline void read_path(std::istream& is, std::filesystem::path& p) {
    std::string tmp;
    read_string(is, tmp);
    p = std::filesystem::path(tmp);
}

inline std::int64_t to_unix(const TimePoint& tp) {
    return std::chrono::duration_cast<std::chrono::seconds>(tp.time_since_epoch()).count();
}

inline TimePoint from_unix(const std::int64_t& unix) {
    return TimePoint(std::chrono::seconds{unix});
}

// Binary File Entry Serialization

inline void serialize_binary(std::ostream& os, const FileEntry& e) {
    write_pod(os, e.id);
    write_pod(os, e.parent_id);

    write_path(os, e.path);
    write_string(os,e.name);

    write_pod(os,e.type);
    write_pod(os,e.size_bytes);
    write_pod(os,e.inode);
    write_pod(os,e.device_id);
    write_pod(os,e.permissions);
    write_pod(os,e.owner_uid);
    write_pod(os,e.owner_gid);

    write_pod(os, to_unix(e.created_at));
    write_pod(os,to_unix(e.modified_at));
    write_pod(os, to_unix(e.accessed_at));

    const bool has_hash = e.content_hash.has_value();
    write_pod(os,has_hash);
    if (has_hash) write_pod(os,*e.content_hash);
}

inline void deserialize_binary(std::istream& is, FileEntry& e) {
    read_pod(is, e.id);
    read_pod(is, e.parent_id);
    read_path(is, e.path);
    read_string(is, e.name);
    read_pod(is, e.type);
    read_pod(is, e.size_bytes);
    read_pod(is, e.inode);
    read_pod(is, e.device_id);
    read_pod(is, e.permissions);
    read_pod(is, e.owner_uid);
    read_pod(is, e.owner_gid);
    std::int64_t t;
    read_pod(is, t); e.created_at = from_unix(t);
    read_pod(is, t); e.modified_at = from_unix(t);
    read_pod(is, t); e.accessed_at = from_unix(t);

    bool has_hash = false;
    read_pod(is, has_hash);
    if (has_hash) {
        Hash64 h;
        read_pod(is, h);
        e.content_hash = h;
    } else {
        e.content_hash.reset();
    }

}

// Binary Snapshot Serialization

inline void serialize_binary(std::ostream& os, const SnapshotEntry& s) {
    write_pod(os, s.snapshot_id);
    write_pod(os,to_unix(s.created_at));
    write_pod(os,s.schema_version);
    write_pod(os,s.compatible_min_version);
    write_pod(os,s.root_id);

    const std::uint64_t count = s.entries.size();
    write_pod(os, count);
    for (const auto& e : s.entries) {
        serialize_binary(os, e);
    }
}

inline void deserialize_binary(std::istream& is, SnapshotEntry& s) {
    std::int64_t t;
    read_pod(is, s.snapshot_id);
    read_pod(is, t); s.created_at = from_unix(t);
    read_pod(is, s.schema_version);
    read_pod(is, s.compatible_min_version);
    read_pod(is, s.root_id);

    std::uint64_t count = 0;
    read_pod(is, count);
    s.entries.resize(count);
    for (auto& e : s.entries) {
        deserialize_binary(is, e);
    }
}

// =============================== JSON ===============================

#include <nlohmann/json.hpp>
using json = nlohmann::json;
#include <magic_enum.hpp>

inline void to_json(json& j, const FileEntry& e) {
    j = json{
        {"id",e.id},
        {"parent_id", e.parent_id},
        {"path",e.path},
        {"name",e.name},
        {"type", std::string(magic_enum::enum_name(e.type))},
        {"size_bytes", e.size_bytes},
        {"inode", e.inode},
        {"device_id", e.device_id},
        {"permissions", e.permissions},
        {"owner_uid",e.owner_uid},
        {"owner_gid",e.owner_gid},
        {"created_at",e.created_at},
        {"modified_at",e.modified_at},
        {"accessed_at",e.accessed_at}
    };

    if ( e.content_hash) {
        j["content_hash"] = *e.content_hash;
    }
}

inline void from_json(const json& j, FileEntry& e) {
    j.at("id").get_to(e.id);
    j.at("parent_id").get_to(e.parent_id);

    std::string p;
    j.at("path").get_to(p);
    e.path = std::filesystem::path(p);
    j.at("name").get_to(e.name);
    std::string type;
    j.at("type").get_to(type);
    if (const auto opt = magic_enum::enum_cast<EntryType>(type)) {
        e.type = *opt;
    } else {
        e.type = EntryType::Special;
    }

    j.at("size_bytes").get_to(e.size_bytes);
    j.at("inode").get_to(e.inode);
    j.at("device_id").get_to(e.device_id);
    j.at("permissions").get_to(e.permissions);
    j.at("owner_uid").get_to(e.owner_uid);
    j.at("owner_gid").get_to(e.owner_gid);

    e.created_at = from_unix(j.at("created_at").get<std::uint64_t>());
    e.modified_at = from_unix(j.at("modified_at").get<std::uint64_t>());
    e.accessed_at = from_unix(j.at("accessed_at").get<std::uint64_t>());

    e.content_hash = j.at("content_hash");

}

inline void to_json(json& j, const SnapshotEntry& s) {

}

inline void from_json(const json& j, SnapshotEntry& s) {

}

#endif //HOSTINDEXER_SERIALIZATION_HPP