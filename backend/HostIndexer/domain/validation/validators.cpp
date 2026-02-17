#include "validators.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../model/invariants.hpp"

namespace host_indexer::validation {
namespace {

constexpr std::size_t kMaxReasonableDepth = 4096;

std::unordered_map<domain::EntryId, const domain::FileEntry*> build_entry_index(const domain::Snapshot& snapshot) {
    std::unordered_map<domain::EntryId, const domain::FileEntry*> by_id;
    by_id.reserve(snapshot.entries.size());
    for (const auto& entry : snapshot.entries) {
        by_id[entry.id] = &entry;
    }
    return by_id;
}

std::unordered_map<domain::EntryId, std::vector<domain::EntryId>> build_children_index(const domain::Snapshot& snapshot) {
    std::unordered_map<domain::EntryId, std::vector<domain::EntryId>> children;
    children.reserve(snapshot.entries.size());
    for (const auto& entry : snapshot.entries) {
        if (entry.parent_id != 0) {
            children[entry.parent_id].push_back(entry.id);
        }
    }
    return children;
}

bool has_path_traversal(const std::filesystem::path& path) {
    for (const auto& part : path) {
        if (part == "..") {
            return true;
        }
    }
    return false;
}

} // namespace

std::string_view SnapshotHeaderValidationPass::name() const noexcept {
    return "SnapshotHeaderValidationPass";
}

void SnapshotHeaderValidationPass::run(const domain::Snapshot& snapshot, ValidationCollector& collector) const {
    if (!domain::is_non_zero_snapshot_id(snapshot.snapshot_id)) {
        collector.add(
            ValidationErrorCode::SnapshotIdZero,
            ValidationSeverity::Error,
            "Snapshot ID must be non-zero."
        );
    }

    if (!domain::is_valid_schema(snapshot.schema)) {
        collector.add(
            ValidationErrorCode::SnapshotSchemaInvalid,
            ValidationSeverity::Error,
            "Snapshot schema header is invalid.",
            snapshot.snapshot_id
        );
    }

    if (snapshot.schema.major > domain::kCurrentSchemaHeader.major) {
        collector.add(
            ValidationErrorCode::SnapshotSchemaTooNew,
            ValidationSeverity::Error,
            "Snapshot schema major version is newer than this reader supports.",
            snapshot.snapshot_id
        );
    }

    if (snapshot.schema.major < domain::kCurrentSchemaHeader.min_reader_major) {
        collector.add(
            ValidationErrorCode::SnapshotSchemaTooOld,
            ValidationSeverity::Error,
            "Snapshot schema major version is older than supported minimum.",
            snapshot.snapshot_id
        );
    }

    if (snapshot.created_at <= 0) {
        collector.add(
            ValidationErrorCode::SnapshotTimestampInvalid,
            ValidationSeverity::Warning,
            "Snapshot creation timestamp is unset or invalid.",
            snapshot.snapshot_id
        );
    }

    if (snapshot.host_identifier.empty()) {
        collector.add(
            ValidationErrorCode::SnapshotHostIdentifierEmpty,
            ValidationSeverity::Error,
            "Snapshot host identifier is empty.",
            snapshot.snapshot_id
        );
    }

    if (snapshot.root_entry_id == 0) {
        collector.add(
            ValidationErrorCode::SnapshotRootIdZero,
            ValidationSeverity::Error,
            "Snapshot root entry ID is zero.",
            snapshot.snapshot_id
        );
    }

    if (snapshot.entries.empty()) {
        collector.add(
            ValidationErrorCode::SnapshotEntriesEmpty,
            ValidationSeverity::Error,
            "Snapshot entry collection is empty.",
            snapshot.snapshot_id
        );
        return;
    }

    const auto root_it = std::find_if(
        snapshot.entries.begin(),
        snapshot.entries.end(),
        [&](const domain::FileEntry& entry) { return entry.id == snapshot.root_entry_id; }
    );

    if (root_it == snapshot.entries.end()) {
        collector.add(
            ValidationErrorCode::SnapshotRootMissing,
            ValidationSeverity::Error,
            "Snapshot root entry ID does not reference any entry.",
            snapshot.snapshot_id,
            snapshot.root_entry_id
        );
        return;
    }

    if (root_it->type != domain::EntryType::Directory) {
        collector.add(
            ValidationErrorCode::SnapshotRootNotDirectory,
            ValidationSeverity::Error,
            "Snapshot root entry is not a directory.",
            snapshot.snapshot_id,
            root_it->id,
            root_it->normalized_path
        );
    }

    if (root_it->parent_id != 0) {
        collector.add(
            ValidationErrorCode::SnapshotRootParentNotZero,
            ValidationSeverity::Error,
            "Snapshot root entry must have parent ID 0.",
            snapshot.snapshot_id,
            root_it->id,
            root_it->normalized_path
        );
    }
}

std::string_view EntryIdentityValidationPass::name() const noexcept {
    return "EntryIdentityValidationPass";
}

void EntryIdentityValidationPass::run(const domain::Snapshot& snapshot, ValidationCollector& collector) const {
    std::unordered_set<domain::EntryId> ids;
    ids.reserve(snapshot.entries.size());

    for (const auto& entry : snapshot.entries) {
        if (entry.id == 0) {
            collector.add(
                ValidationErrorCode::EntryIdZero,
                ValidationSeverity::Error,
                "Entry ID must be non-zero.",
                snapshot.snapshot_id,
                entry.id,
                entry.normalized_path
            );
        }

        if (!ids.insert(entry.id).second) {
            collector.add(
                ValidationErrorCode::EntryIdDuplicate,
                ValidationSeverity::Error,
                "Duplicate entry ID detected.",
                snapshot.snapshot_id,
                entry.id,
                entry.normalized_path
            );
        }

        if (entry.id != snapshot.root_entry_id && entry.parent_id == 0) {
            collector.add(
                ValidationErrorCode::ParentIdZeroNonRoot,
                ValidationSeverity::Error,
                "Non-root entry has parent ID 0.",
                snapshot.snapshot_id,
                entry.id,
                entry.normalized_path
            );
        }
    }
}

std::string_view PathNormalizationValidationPass::name() const noexcept {
    return "PathNormalizationValidationPass";
}

void PathNormalizationValidationPass::run(const domain::Snapshot& snapshot, ValidationCollector& collector) const {
    std::unordered_set<std::string> unique_paths;
    unique_paths.reserve(snapshot.entries.size());

    for (const auto& entry : snapshot.entries) {
        if (entry.normalized_path.empty()) {
            collector.add(
                ValidationErrorCode::EntryPathEmpty,
                ValidationSeverity::Error,
                "Entry path is empty.",
                snapshot.snapshot_id,
                entry.id
            );
            continue;
        }

        const std::filesystem::path fs_path(entry.normalized_path);

        if (!fs_path.is_absolute()) {
            collector.add(
                ValidationErrorCode::EntryPathNotAbsolute,
                ValidationSeverity::Error,
                "Entry path must be absolute.",
                snapshot.snapshot_id,
                entry.id,
                entry.normalized_path
            );
        }

        const auto normalized = fs_path.lexically_normal().generic_string();
        if (normalized != entry.normalized_path) {
            collector.add(
                ValidationErrorCode::EntryPathNotNormalized,
                ValidationSeverity::Error,
                "Entry path is not lexically normalized.",
                snapshot.snapshot_id,
                entry.id,
                entry.normalized_path
            );
        }

        if (has_path_traversal(fs_path)) {
            collector.add(
                ValidationErrorCode::EntryPathTraversalDetected,
                ValidationSeverity::Error,
                "Entry path contains traversal segment '..'.",
                snapshot.snapshot_id,
                entry.id,
                entry.normalized_path
            );
        }

        if (!unique_paths.insert(entry.normalized_path).second) {
            collector.add(
                ValidationErrorCode::GraphDuplicatePath,
                ValidationSeverity::Error,
                "Duplicate normalized path detected.",
                snapshot.snapshot_id,
                entry.id,
                entry.normalized_path
            );
        }

        if (entry.parent_id != 0) {
            const auto filename = fs_path.filename().generic_string();
            if (entry.name != filename) {
                collector.add(
                    ValidationErrorCode::EntryNameMismatch,
                    ValidationSeverity::Warning,
                    "Entry name does not match path filename.",
                    snapshot.snapshot_id,
                    entry.id,
                    entry.normalized_path
                );
            }
        }

        if ((entry.metadata.permissions & 0xFFFF0000U) != 0U) {
            collector.add(
                ValidationErrorCode::EntryPermissionsInvalid,
                ValidationSeverity::Warning,
                "Entry permissions contain unsupported high bits.",
                snapshot.snapshot_id,
                entry.id,
                entry.normalized_path
            );
        }

        const auto type_value = static_cast<std::uint8_t>(entry.type);
        if (type_value > static_cast<std::uint8_t>(domain::EntryType::Special)) {
            collector.add(
                ValidationErrorCode::EntryTypeInvalid,
                ValidationSeverity::Error,
                "Entry type value is out of range.",
                snapshot.snapshot_id,
                entry.id,
                entry.normalized_path
            );
        }
    }
}

std::string_view ParentGraphValidationPass::name() const noexcept {
    return "ParentGraphValidationPass";
}

void ParentGraphValidationPass::run(const domain::Snapshot& snapshot, ValidationCollector& collector) const {
    if (snapshot.entries.empty()) {
        return;
    }

    const auto by_id = build_entry_index(snapshot);
    const auto children = build_children_index(snapshot);

    for (const auto& entry : snapshot.entries) {
        if (entry.id == snapshot.root_entry_id) {
            continue;
        }

        if (entry.parent_id == 0) {
            collector.add(
                ValidationErrorCode::ParentIdZeroNonRoot,
                ValidationSeverity::Error,
                "Non-root entry has parent ID 0.",
                snapshot.snapshot_id,
                entry.id,
                entry.normalized_path
            );
            continue;
        }

        const auto parent_it = by_id.find(entry.parent_id);
        if (parent_it == by_id.end()) {
            collector.add(
                ValidationErrorCode::ParentMissing,
                ValidationSeverity::Error,
                "Entry parent ID does not exist.",
                snapshot.snapshot_id,
                entry.id,
                entry.normalized_path
            );
            continue;
        }

        if (parent_it->second->type != domain::EntryType::Directory) {
            collector.add(
                ValidationErrorCode::ParentNotDirectory,
                ValidationSeverity::Error,
                "Entry parent is not a directory.",
                snapshot.snapshot_id,
                entry.id,
                entry.normalized_path
            );
        }
    }

    enum class Color : std::uint8_t { White, Gray, Black };
    std::unordered_map<domain::EntryId, Color> colors;
    colors.reserve(snapshot.entries.size());
    for (const auto& entry : snapshot.entries) {
        colors.emplace(entry.id, Color::White);
    }

    struct Frame {
        domain::EntryId id {0};
        std::size_t next_child_index {0};
    };

    for (const auto& entry : snapshot.entries) {
        if (colors[entry.id] != Color::White) {
            continue;
        }

        std::vector<Frame> stack;
        stack.push_back(Frame {entry.id, 0});
        colors[entry.id] = Color::Gray;

        while (!stack.empty()) {
            auto& top = stack.back();
            const auto child_it = children.find(top.id);

            if (stack.size() > kMaxReasonableDepth) {
                collector.add(
                    ValidationErrorCode::GraphMaxDepthExceeded,
                    ValidationSeverity::Warning,
                    "Tree depth exceeds configured validation limit.",
                    snapshot.snapshot_id,
                    top.id
                );
            }

            if (child_it == children.end() || top.next_child_index >= child_it->second.size()) {
                colors[top.id] = Color::Black;
                stack.pop_back();
                continue;
            }

            const auto child_id = child_it->second[top.next_child_index++];
            const auto color_it = colors.find(child_id);
            if (color_it == colors.end()) {
                continue;
            }

            if (color_it->second == Color::Gray) {
                collector.add(
                    ValidationErrorCode::GraphCycleDetected,
                    ValidationSeverity::Error,
                    "Cycle detected in parent/child graph.",
                    snapshot.snapshot_id,
                    child_id
                );
                continue;
            }

            if (color_it->second == Color::White) {
                color_it->second = Color::Gray;
                stack.push_back(Frame {child_id, 0});
            }
        }
    }

    const auto root_it = by_id.find(snapshot.root_entry_id);
    if (root_it == by_id.end()) {
        return;
    }

    std::unordered_set<domain::EntryId> reachable;
    reachable.reserve(snapshot.entries.size());
    std::vector<domain::EntryId> stack;
    stack.push_back(snapshot.root_entry_id);
    reachable.insert(snapshot.root_entry_id);

    while (!stack.empty()) {
        const auto current_id = stack.back();
        stack.pop_back();
        const auto child_it = children.find(current_id);
        if (child_it == children.end()) {
            continue;
        }
        for (const auto child_id : child_it->second) {
            if (reachable.insert(child_id).second) {
                stack.push_back(child_id);
            }
        }
    }

    for (const auto& entry_item : by_id) {
        if (!reachable.contains(entry_item.first)) {
            collector.add(
                ValidationErrorCode::GraphDisconnectedEntry,
                ValidationSeverity::Warning,
                "Entry is not reachable from root.",
                snapshot.snapshot_id,
                entry_item.first,
                entry_item.second->normalized_path
            );
        }
    }
}

std::string_view TimestampSanityValidationPass::name() const noexcept {
    return "TimestampSanityValidationPass";
}

void TimestampSanityValidationPass::run(const domain::Snapshot& snapshot, ValidationCollector& collector) const {
    const auto now_epoch = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    for (const auto& entry : snapshot.entries) {
        if (entry.metadata.created_at > entry.metadata.modified_at ||
            entry.metadata.modified_at > entry.metadata.accessed_at) {
            collector.add(
                ValidationErrorCode::EntryTimestampOrderInvalid,
                ValidationSeverity::Warning,
                "Entry timestamps are not monotonic: created <= modified <= accessed failed.",
                snapshot.snapshot_id,
                entry.id,
                entry.normalized_path
            );
        }

        if (entry.metadata.created_at > now_epoch ||
            entry.metadata.modified_at > now_epoch ||
            entry.metadata.accessed_at > now_epoch) {
            collector.add(
                ValidationErrorCode::EntryTimestampInFuture,
                ValidationSeverity::Warning,
                "Entry timestamps are in the future.",
                snapshot.snapshot_id,
                entry.id,
                entry.normalized_path
            );
        }
    }
}

std::string_view DeltaReferentialIntegrityPass::name() const noexcept {
    return "DeltaReferentialIntegrityPass";
}

void DeltaReferentialIntegrityPass::run(const domain::DeltaSnapshot& delta,
                                        const domain::Snapshot* base_snapshot,
                                        const domain::Snapshot* target_snapshot,
                                        ValidationCollector& collector) const {
    if (delta.base_snapshot_id == 0) {
        collector.add(
            ValidationErrorCode::DeltaBaseSnapshotIdZero,
            ValidationSeverity::Error,
            "Delta base snapshot ID must be non-zero.",
            delta.base_snapshot_id
        );
    }

    if (delta.target_snapshot_id == 0) {
        collector.add(
            ValidationErrorCode::DeltaTargetSnapshotIdZero,
            ValidationSeverity::Error,
            "Delta target snapshot ID must be non-zero.",
            delta.target_snapshot_id
        );
    }

    if (delta.base_snapshot_id == delta.target_snapshot_id && delta.base_snapshot_id != 0) {
        collector.add(
            ValidationErrorCode::DeltaBaseEqualsTarget,
            ValidationSeverity::Warning,
            "Delta base and target snapshot IDs are identical.",
            delta.base_snapshot_id
        );
    }

    std::unordered_set<domain::EntryId> added_ids;
    std::unordered_set<domain::EntryId> removed_ids;
    std::unordered_set<domain::EntryId> modified_ids;
    std::unordered_set<domain::EntryId> renamed_old_ids;
    std::unordered_set<domain::EntryId> renamed_new_ids;
    std::unordered_set<std::string> rename_targets;

    added_ids.reserve(delta.added_entries.size());
    removed_ids.reserve(delta.removed_entries.size());
    modified_ids.reserve(delta.modified_entries.size());
    renamed_old_ids.reserve(delta.renamed_entries.size());
    renamed_new_ids.reserve(delta.renamed_entries.size());
    rename_targets.reserve(delta.renamed_entries.size());

    for (const auto& added : delta.added_entries) {
        if (!added_ids.insert(added.entry.id).second) {
            collector.add(
                ValidationErrorCode::DeltaAddedEntryDuplicate,
                ValidationSeverity::Error,
                "Delta contains duplicate added entry ID.",
                delta.target_snapshot_id,
                added.entry.id,
                added.entry.normalized_path
            );
        }
    }

    for (const auto& removed : delta.removed_entries) {
        if (!removed_ids.insert(removed.entry.id).second) {
            collector.add(
                ValidationErrorCode::DeltaRemovedEntryDuplicate,
                ValidationSeverity::Error,
                "Delta contains duplicate removed entry ID.",
                delta.base_snapshot_id,
                removed.entry.id,
                removed.entry.normalized_path
            );
        }
    }

    for (const auto& modified : delta.modified_entries) {
        if (!modified_ids.insert(modified.entry_id).second) {
            collector.add(
                ValidationErrorCode::DeltaModifiedEntryDuplicate,
                ValidationSeverity::Error,
                "Delta contains duplicate modified entry ID.",
                delta.target_snapshot_id,
                modified.entry_id
            );
        }
    }

    for (const auto& renamed : delta.renamed_entries) {
        if (!renamed_old_ids.insert(renamed.before_entry_id).second ||
            !renamed_new_ids.insert(renamed.after_entry_id).second) {
            collector.add(
                ValidationErrorCode::DeltaRenamedEntryDuplicate,
                ValidationSeverity::Error,
                "Delta contains duplicate rename endpoint IDs.",
                delta.target_snapshot_id,
                renamed.after_entry_id,
                renamed.new_path
            );
        }

        if (!rename_targets.insert(renamed.new_path).second) {
            collector.add(
                ValidationErrorCode::DeltaRenamePathConflict,
                ValidationSeverity::Error,
                "Multiple rename operations target the same path.",
                delta.target_snapshot_id,
                renamed.after_entry_id,
                renamed.new_path
            );
        }
    }

    for (const auto entry_id : added_ids) {
        if (removed_ids.contains(entry_id) || modified_ids.contains(entry_id)) {
            collector.add(
                ValidationErrorCode::DeltaConflictingOperations,
                ValidationSeverity::Error,
                "Entry appears in conflicting delta operation sets.",
                delta.target_snapshot_id,
                entry_id
            );
        }
    }

    for (const auto entry_id : removed_ids) {
        if (modified_ids.contains(entry_id)) {
            collector.add(
                ValidationErrorCode::DeltaConflictingOperations,
                ValidationSeverity::Error,
                "Removed entry cannot also be marked modified.",
                delta.base_snapshot_id,
                entry_id
            );
        }
    }

    if (base_snapshot != nullptr) {
        std::unordered_set<domain::EntryId> base_ids;
        base_ids.reserve(base_snapshot->entries.size());
        for (const auto& entry : base_snapshot->entries) {
            base_ids.insert(entry.id);
        }

        for (const auto& removed : delta.removed_entries) {
            if (!base_ids.contains(removed.entry.id)) {
                collector.add(
                    ValidationErrorCode::DeltaRemovedEntryMissingInBase,
                    ValidationSeverity::Error,
                    "Removed entry ID does not exist in base snapshot.",
                    base_snapshot->snapshot_id,
                    removed.entry.id,
                    removed.entry.normalized_path
                );
            }
        }

        for (const auto& modified : delta.modified_entries) {
            if (!base_ids.contains(modified.entry_id)) {
                collector.add(
                    ValidationErrorCode::DeltaModifiedEntryMissingInBase,
                    ValidationSeverity::Error,
                    "Modified entry ID does not exist in base snapshot.",
                    base_snapshot->snapshot_id,
                    modified.entry_id
                );
            }
        }

        for (const auto& renamed : delta.renamed_entries) {
            if (!base_ids.contains(renamed.before_entry_id)) {
                collector.add(
                    ValidationErrorCode::DeltaRenamedOldMissingInBase,
                    ValidationSeverity::Error,
                    "Rename source entry ID does not exist in base snapshot.",
                    base_snapshot->snapshot_id,
                    renamed.before_entry_id,
                    renamed.old_path
                );
            }
        }
    }

    if (target_snapshot != nullptr) {
        std::unordered_set<domain::EntryId> target_ids;
        target_ids.reserve(target_snapshot->entries.size());
        for (const auto& entry : target_snapshot->entries) {
            target_ids.insert(entry.id);
        }

        for (const auto& added : delta.added_entries) {
            if (!target_ids.contains(added.entry.id)) {
                collector.add(
                    ValidationErrorCode::DeltaAddedEntryMissingInTarget,
                    ValidationSeverity::Error,
                    "Added entry ID does not exist in target snapshot.",
                    target_snapshot->snapshot_id,
                    added.entry.id,
                    added.entry.normalized_path
                );
            }
        }

        for (const auto& modified : delta.modified_entries) {
            if (!target_ids.contains(modified.entry_id)) {
                collector.add(
                    ValidationErrorCode::DeltaModifiedEntryMissingInTarget,
                    ValidationSeverity::Error,
                    "Modified entry ID does not exist in target snapshot.",
                    target_snapshot->snapshot_id,
                    modified.entry_id
                );
            }
        }

        for (const auto& renamed : delta.renamed_entries) {
            if (!target_ids.contains(renamed.after_entry_id)) {
                collector.add(
                    ValidationErrorCode::DeltaRenamedNewMissingInTarget,
                    ValidationSeverity::Error,
                    "Rename target entry ID does not exist in target snapshot.",
                    target_snapshot->snapshot_id,
                    renamed.after_entry_id,
                    renamed.new_path
                );
            }
        }
    }
}

SnapshotValidator::SnapshotValidator()
    : passes_() {
    passes_.push_back(std::make_unique<SnapshotHeaderValidationPass>());
    passes_.push_back(std::make_unique<EntryIdentityValidationPass>());
    passes_.push_back(std::make_unique<PathNormalizationValidationPass>());
    passes_.push_back(std::make_unique<ParentGraphValidationPass>());
    passes_.push_back(std::make_unique<TimestampSanityValidationPass>());
}

SnapshotValidator::SnapshotValidator(std::vector<std::unique_ptr<ISnapshotValidationPass>> passes)
    : passes_(std::move(passes)) {
}

ValidationReport SnapshotValidator::validate(const domain::Snapshot& snapshot) const {
    ValidationReport report;
    ValidationCollector collector(report);
    for (const auto& pass : passes_) {
        pass->run(snapshot, collector);
    }
    return report;
}

DeltaValidator::DeltaValidator()
    : passes_() {
    passes_.push_back(std::make_unique<DeltaReferentialIntegrityPass>());
}

DeltaValidator::DeltaValidator(std::vector<std::unique_ptr<IDeltaValidationPass>> passes)
    : passes_(std::move(passes)) {
}

ValidationReport DeltaValidator::validate(const domain::DeltaSnapshot& delta,
                                          const domain::Snapshot* base_snapshot,
                                          const domain::Snapshot* target_snapshot) const {
    ValidationReport report;
    ValidationCollector collector(report);
    for (const auto& pass : passes_) {
        pass->run(delta, base_snapshot, target_snapshot, collector);
    }
    return report;
}

} // namespace host_indexer::validation
