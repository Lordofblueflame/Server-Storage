#include "snapshot_builder.hpp"

#include <algorithm>
#include <sstream>
#include <unordered_map>

#include "../utils/hash.hpp"
#include "../utils/path_utils.hpp"

namespace host_indexer::snapshot {
namespace {

void add_diagnostic(BuildResult& result,
                    const BuildDiagnosticCode code,
                    const BuildSeverity severity,
                    const std::string_view message,
                    const std::string_view path = {}) {
    result.diagnostics.push_back(
        BuildDiagnostic {
            code,
            severity,
            std::string(message),
            std::string(path)
        }
    );
}

std::string make_base_stable_key(const filesystem::RawEntry& entry,
                                 const std::string_view normalized_path) {
    std::ostringstream stream;
    stream << static_cast<std::uint32_t>(entry.type) << '|';
    if (entry.metadata.device_id != 0 && entry.metadata.file_id != 0) {
        stream << entry.metadata.device_id << '|' << entry.metadata.file_id;
    } else {
        stream << normalized_path;
    }
    return stream.str();
}

domain::EntryId generate_stable_id(const filesystem::RawEntry& entry,
                                   const std::string_view normalized_path,
                                   std::unordered_map<domain::EntryId, std::string>& used_ids,
                                   BuildResult& result) {
    const std::string base_key = make_base_stable_key(entry, normalized_path);
    std::string candidate_key = base_key;

    std::uint32_t salt = 0;
    while (true) {
        domain::EntryId candidate_id = utils::fnv1a64(candidate_key);
        if (candidate_id == 0) {
            candidate_id = 1;
        }

        const auto used_it = used_ids.find(candidate_id);
        if (used_it == used_ids.end()) {
            used_ids.emplace(candidate_id, candidate_key);
            return candidate_id;
        }

        ++salt;
        std::ostringstream salted;
        salted << base_key << "|path=" << normalized_path << "|salt=" << salt;
        candidate_key = salted.str();

        add_diagnostic(
            result,
            BuildDiagnosticCode::StableIdCollision,
            BuildSeverity::Warning,
            "Stable ID collision resolved with deterministic salt.",
            normalized_path
        );
    }
}

domain::SnapshotId compute_snapshot_id(const domain::Snapshot& snapshot) {
    std::ostringstream stream;
    stream << snapshot.schema.major << '|'
           << snapshot.schema.minor << '|'
           << snapshot.schema.patch << '|'
           << snapshot.schema.min_reader_major << '|'
           << snapshot.host_identifier << '|'
           << snapshot.created_at << '|'
           << snapshot.root_entry_id << '|'
           << snapshot.entries.size() << '|';

    for (const auto& entry : snapshot.entries) {
        stream << entry.id << '|'
               << entry.parent_id << '|'
               << static_cast<std::uint32_t>(entry.type) << '|'
               << entry.normalized_path << '|'
               << entry.name << '|'
               << entry.metadata.size_bytes << '|'
               << entry.metadata.created_at << '|'
               << entry.metadata.modified_at << '|'
               << entry.metadata.accessed_at << '|'
               << entry.metadata.permissions << '|'
               << entry.metadata.file_id << '|'
               << entry.metadata.device_id << '|';
        if (entry.metadata.content_hash.has_value()) {
            stream << *entry.metadata.content_hash;
        } else {
            stream << "null";
        }
        stream << ';';
    }

    auto snapshot_id = utils::fnv1a64(stream.str());
    if (snapshot_id == 0) {
        snapshot_id = 1;
    }
    return snapshot_id;
}

} // namespace

BuildResult SnapshotBuilder::build(const filesystem::RawScanResult& scan_result,
                                   const BuildOptions& options) const {
    BuildResult result;
    result.snapshot.schema = domain::kCurrentSchemaHeader;
    result.snapshot.host_identifier = scan_result.host_identifier;
    result.snapshot.created_at = options.created_at != 0 ? options.created_at : scan_result.scanned_at;
    result.snapshot.snapshot_id = options.snapshot_id;

    std::vector<const filesystem::RawEntry*> sorted_entries;
    sorted_entries.reserve(scan_result.entries.size());
    for (const auto& entry : scan_result.entries) {
        sorted_entries.push_back(&entry);
    }

    std::sort(
        sorted_entries.begin(),
        sorted_entries.end(),
        [](const filesystem::RawEntry* lhs, const filesystem::RawEntry* rhs) {
            return lhs->normalized_path < rhs->normalized_path;
        }
    );

    std::unordered_map<std::string, std::size_t> path_to_index;
    path_to_index.reserve(sorted_entries.size());

    std::unordered_map<domain::EntryId, std::string> used_ids;
    used_ids.reserve(sorted_entries.size());

    for (const auto* raw_entry : sorted_entries) {
        const auto normalized_path = utils::normalize_path_lexical(raw_entry->normalized_path);
        if (path_to_index.contains(normalized_path)) {
            add_diagnostic(
                result,
                BuildDiagnosticCode::DuplicatePath,
                BuildSeverity::Warning,
                "Duplicate path in scan result ignored.",
                normalized_path
            );
            continue;
        }

        domain::FileEntry snapshot_entry;
        snapshot_entry.normalized_path = normalized_path;
        snapshot_entry.name = raw_entry->name.empty()
                                  ? utils::filename_from_path(normalized_path)
                                  : raw_entry->name;
        snapshot_entry.type = raw_entry->type;
        snapshot_entry.metadata = raw_entry->metadata;
        snapshot_entry.id = generate_stable_id(*raw_entry, normalized_path, used_ids, result);

        path_to_index.emplace(normalized_path, result.snapshot.entries.size());
        result.snapshot.entries.push_back(std::move(snapshot_entry));
    }

    const auto normalized_root_path = utils::normalize_path_lexical(scan_result.root_path);
    for (auto& entry : result.snapshot.entries) {
        if (entry.normalized_path == normalized_root_path) {
            entry.parent_id = 0;
            result.snapshot.root_entry_id = entry.id;
            continue;
        }

        const auto parent_path = utils::parent_from_path(entry.normalized_path);
        const auto parent_it = path_to_index.find(parent_path);
        if (parent_it == path_to_index.end()) {
            entry.parent_id = 0;
            add_diagnostic(
                result,
                BuildDiagnosticCode::ParentPathMissing,
                BuildSeverity::Error,
                "Parent path not found while building snapshot entry graph.",
                entry.normalized_path
            );
        } else {
            entry.parent_id = result.snapshot.entries[parent_it->second].id;
        }
    }

    if (result.snapshot.root_entry_id == 0 && !result.snapshot.entries.empty()) {
        result.snapshot.root_entry_id = result.snapshot.entries.front().id;
        result.snapshot.entries.front().parent_id = 0;
        add_diagnostic(
            result,
            BuildDiagnosticCode::RootEntryMissing,
            BuildSeverity::Error,
            "Root entry was not found by root path; selected first entry as fallback.",
            normalized_root_path
        );
    }

    if (result.snapshot.snapshot_id == 0) {
        result.snapshot.snapshot_id = compute_snapshot_id(result.snapshot);
    }

    return result;
}

} // namespace host_indexer::snapshot
