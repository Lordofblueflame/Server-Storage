#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "api/indexing_pipeline.hpp"
#include "domain/model/models.hpp"
#include "domain/validation/validate_snapshot.hpp"
#include "storage/lmdb_store.hpp"

namespace {

namespace fs = std::filesystem;

class TempDir final {
public:
    explicit TempDir(const std::string_view prefix) {
        const auto ticks = static_cast<long long>(
            std::chrono::steady_clock::now().time_since_epoch().count()
        );
        path_ = fs::temp_directory_path() / (std::string(prefix) + "_" + std::to_string(ticks));

        std::error_code error;
        fs::create_directories(path_, error);
        EXPECT_FALSE(error) << error.message();
    }

    ~TempDir() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }

    [[nodiscard]] const fs::path& path() const noexcept {
        return path_;
    }

private:
    fs::path path_ {};
};

void write_text_file(const fs::path& path, const std::string_view contents) {
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    ASSERT_FALSE(error) << error.message();

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(static_cast<bool>(output)) << "failed to open file for write: " << path.string();
    output << contents;
}

void append_text_file(const fs::path& path, const std::string_view contents) {
    std::ofstream output(path, std::ios::binary | std::ios::app);
    ASSERT_TRUE(static_cast<bool>(output)) << "failed to open file for append: " << path.string();
    output << contents;
}

const muninn::domain::FileEntry* find_entry_by_path(const muninn::domain::Snapshot& snapshot,
                                                    const std::string_view normalized_path) {
    for (const auto& entry : snapshot.entries) {
        if (entry.normalized_path == normalized_path) {
            return &entry;
        }
    }
    return nullptr;
}

std::vector<muninn::domain::FileEntry> sorted_entries(const muninn::domain::Snapshot& snapshot) {
    auto entries = snapshot.entries;
    std::sort(
        entries.begin(),
        entries.end(),
        [](const muninn::domain::FileEntry& lhs, const muninn::domain::FileEntry& rhs) {
            if (lhs.normalized_path == rhs.normalized_path) {
                return lhs.id < rhs.id;
            }
            return lhs.normalized_path < rhs.normalized_path;
        }
    );
    return entries;
}

bool snapshot_entry_graph_equals(const muninn::domain::Snapshot& lhs, const muninn::domain::Snapshot& rhs) {
    if (lhs.root_entry_id != rhs.root_entry_id) {
        return false;
    }

    const auto lhs_entries = sorted_entries(lhs);
    const auto rhs_entries = sorted_entries(rhs);
    if (lhs_entries.size() != rhs_entries.size()) {
        return false;
    }

    for (std::size_t index = 0; index < lhs_entries.size(); ++index) {
        if (!muninn::domain::structural_entry_equals(lhs_entries[index], rhs_entries[index])) {
            return false;
        }
    }

    return true;
}

muninn::domain::Snapshot apply_delta_for_test(const muninn::domain::Snapshot& base,
                                              const muninn::domain::DeltaSnapshot& delta) {
    muninn::domain::Snapshot applied = base;
    applied.snapshot_id = delta.target_snapshot_id;

    for (const auto& removed : delta.removed_entries) {
        const auto remove_it = std::remove_if(
            applied.entries.begin(),
            applied.entries.end(),
            [&](const muninn::domain::FileEntry& entry) {
                return entry.id == removed.entry.id;
            }
        );
        applied.entries.erase(remove_it, applied.entries.end());
    }

    for (const auto& modified : delta.modified_entries) {
        auto entry_it = std::find_if(
            applied.entries.begin(),
            applied.entries.end(),
            [&](const muninn::domain::FileEntry& entry) {
                return entry.id == modified.entry_id;
            }
        );
        if (entry_it == applied.entries.end()) {
            ADD_FAILURE() << "modified entry missing from base snapshot: " << modified.entry_id;
            continue;
        }
        entry_it->metadata = modified.after;
    }

    for (const auto& added : delta.added_entries) {
        applied.entries.push_back(added.entry);
    }

    return applied;
}

muninn::storage::LmdbStore open_store(const fs::path& directory) {
    muninn::storage::LmdbStore store;
    muninn::storage::LmdbStoreOptions options;
    options.directory = directory;
    options.map_size_bytes = 256ULL * 1024ULL * 1024ULL;

    const auto status = store.open(options);
    EXPECT_TRUE(status.ok()) << status.message;
    return store;
}

struct CapturedSnapshot {
    muninn::api::PipelineResult pipeline_result {};
    muninn::domain::Snapshot stored_snapshot {};
};

CapturedSnapshot capture_snapshot(muninn::api::IndexingPipeline& pipeline,
                                  muninn::storage::LmdbStore& store,
                                  const fs::path& root_path) {
    CapturedSnapshot captured;
    captured.pipeline_result = pipeline.capture_snapshot(root_path, {}, false);
    EXPECT_TRUE(captured.pipeline_result.ok()) << captured.pipeline_result.snapshot_store_status.message;
    EXPECT_TRUE(captured.pipeline_result.snapshot_validation.ok());

    const auto snapshot_id = captured.pipeline_result.snapshot_build.snapshot.snapshot_id;
    const auto status = store.get_snapshot(snapshot_id, captured.stored_snapshot, true);
    EXPECT_TRUE(status.ok()) << status.message;
    return captured;
}

} // namespace

TEST(MuninnCoreTest, CaptureSnapshotBuildsExpectedGraph) {
    TempDir root("muninn_core_snapshot_root");
    TempDir db("muninn_core_snapshot_db");

    write_text_file(root.path() / "alpha.txt", "alpha");
    write_text_file(root.path() / "nested" / "beta.txt", "beta");

    auto store = open_store(db.path());
    muninn::api::IndexingPipeline pipeline(store);
    const auto captured = capture_snapshot(pipeline, store, root.path());

    const auto validation = muninn::validation::validate_snapshot(captured.stored_snapshot);
    EXPECT_TRUE(validation.ok());
    EXPECT_GE(captured.pipeline_result.raw_scan.entries.size(), 4U);
    EXPECT_EQ(captured.stored_snapshot.entries.size(), captured.pipeline_result.snapshot_build.snapshot.entries.size());

    const auto root_path = fs::absolute(root.path()).lexically_normal().generic_string();
    const auto alpha_path = fs::absolute(root.path() / "alpha.txt").lexically_normal().generic_string();
    const auto beta_path = fs::absolute(root.path() / "nested" / "beta.txt").lexically_normal().generic_string();

    const auto* root_entry = find_entry_by_path(captured.stored_snapshot, root_path);
    const auto* alpha_entry = find_entry_by_path(captured.stored_snapshot, alpha_path);
    const auto* beta_entry = find_entry_by_path(captured.stored_snapshot, beta_path);

    ASSERT_NE(root_entry, nullptr);
    ASSERT_NE(alpha_entry, nullptr);
    ASSERT_NE(beta_entry, nullptr);
    EXPECT_EQ(root_entry->parent_id, 0U);
    EXPECT_EQ(alpha_entry->type, muninn::domain::EntryType::File);
    EXPECT_EQ(alpha_entry->metadata.size_bytes, 5U);
    EXPECT_EQ(beta_entry->type, muninn::domain::EntryType::File);
    EXPECT_EQ(beta_entry->metadata.size_bytes, 4U);
}

TEST(MuninnCoreTest, IncrementalCaptureDeltaReconstructsTargetSnapshot) {
    TempDir root("muninn_core_delta_root");
    TempDir db("muninn_core_delta_db");

    write_text_file(root.path() / "alpha.txt", "alpha");

    auto store = open_store(db.path());
    muninn::api::IndexingPipeline pipeline(store);
    const auto base = capture_snapshot(pipeline, store, root.path());

    std::this_thread::sleep_for(std::chrono::seconds(1));
    append_text_file(root.path() / "alpha.txt", "-changed");
    write_text_file(root.path() / "beta.txt", "beta");

    const auto update = pipeline.capture_snapshot_and_delta(
        root.path(),
        base.stored_snapshot.snapshot_id,
        {},
        false
    );
    ASSERT_TRUE(update.ok()) << update.delta_store_status.message;
    ASSERT_TRUE(update.computed_delta.has_value());
    EXPECT_TRUE(update.delta_validation.ok());
    EXPECT_FALSE(update.computed_delta->added_entries.empty());
    EXPECT_FALSE(update.computed_delta->modified_entries.empty());

    muninn::domain::Snapshot stored_target;
    auto status = store.get_snapshot(update.snapshot_build.snapshot.snapshot_id, stored_target, true);
    ASSERT_TRUE(status.ok()) << status.message;

    muninn::domain::DeltaSnapshot stored_delta;
    status = store.get_delta(
        base.stored_snapshot.snapshot_id,
        stored_target.snapshot_id,
        stored_delta,
        true
    );
    ASSERT_TRUE(status.ok()) << status.message;

    const auto applied_snapshot = apply_delta_for_test(base.stored_snapshot, stored_delta);
    EXPECT_TRUE(snapshot_entry_graph_equals(applied_snapshot, stored_target));
}

TEST(MuninnCoreTest, NoOpRescanReusesBaseSnapshotAndDoesNotPersistExtraRecords) {
    TempDir root("muninn_core_noop_root");
    TempDir db("muninn_core_noop_db");

    write_text_file(root.path() / "alpha.txt", "alpha");

    auto store = open_store(db.path());
    muninn::api::IndexingPipeline pipeline(store);
    const auto base = capture_snapshot(pipeline, store, root.path());

    muninn::storage::StoreStats before_stats;
    auto status = store.get_stats(before_stats);
    ASSERT_TRUE(status.ok()) << status.message;
    EXPECT_EQ(before_stats.snapshots_count, 1U);
    EXPECT_EQ(before_stats.deltas_count, 0U);
    EXPECT_EQ(before_stats.transport_outbox_count, 0U);

    const auto update = pipeline.capture_snapshot_and_delta(
        root.path(),
        base.stored_snapshot.snapshot_id,
        {},
        false
    );
    ASSERT_TRUE(update.ok()) << update.delta_store_status.message;
    ASSERT_TRUE(update.computed_delta.has_value());
    EXPECT_EQ(update.snapshot_build.snapshot.snapshot_id, base.stored_snapshot.snapshot_id);
    EXPECT_EQ(update.computed_delta->base_snapshot_id, base.stored_snapshot.snapshot_id);
    EXPECT_EQ(update.computed_delta->target_snapshot_id, base.stored_snapshot.snapshot_id);
    EXPECT_TRUE(update.computed_delta->added_entries.empty());
    EXPECT_TRUE(update.computed_delta->removed_entries.empty());
    EXPECT_TRUE(update.computed_delta->modified_entries.empty());
    EXPECT_TRUE(update.computed_delta->renamed_entries.empty());

    muninn::storage::StoreStats after_stats;
    status = store.get_stats(after_stats);
    ASSERT_TRUE(status.ok()) << status.message;
    EXPECT_EQ(after_stats.snapshots_count, before_stats.snapshots_count);
    EXPECT_EQ(after_stats.deltas_count, before_stats.deltas_count);
    EXPECT_EQ(after_stats.transport_outbox_count, 0U);
}

TEST(MuninnCoreTest, MissingBaseAfterRetentionFailsIncrementalAndAllowsFullRecovery) {
    TempDir root("muninn_core_recovery_root");
    TempDir db("muninn_core_recovery_db");

    write_text_file(root.path() / "alpha.txt", "alpha");

    auto store = open_store(db.path());
    muninn::api::IndexingPipeline pipeline(store);
    const auto base = capture_snapshot(pipeline, store, root.path());

    std::this_thread::sleep_for(std::chrono::seconds(1));
    append_text_file(root.path() / "alpha.txt", "-changed");

    const auto incremental = pipeline.capture_snapshot_and_delta(
        root.path(),
        base.stored_snapshot.snapshot_id,
        {},
        false
    );
    ASSERT_TRUE(incremental.ok()) << incremental.delta_store_status.message;

    muninn::storage::RetentionCleanupStats retention_stats;
    auto status = store.apply_snapshot_retention(1U, &retention_stats);
    ASSERT_TRUE(status.ok()) << status.message;
    EXPECT_EQ(retention_stats.snapshots_removed, 1U);
    EXPECT_EQ(retention_stats.deltas_removed, 1U);

    muninn::storage::StoreStats before_failed_update_stats;
    status = store.get_stats(before_failed_update_stats);
    ASSERT_TRUE(status.ok()) << status.message;

    std::this_thread::sleep_for(std::chrono::seconds(1));
    append_text_file(root.path() / "alpha.txt", "-again");

    const auto failed_incremental = pipeline.capture_snapshot_and_delta(
        root.path(),
        base.stored_snapshot.snapshot_id,
        {},
        false
    );
    EXPECT_FALSE(failed_incremental.ok());
    EXPECT_EQ(failed_incremental.delta_store_status.code, muninn::storage::StoreErrorCode::NotFound);

    muninn::storage::StoreStats after_failed_update_stats;
    status = store.get_stats(after_failed_update_stats);
    ASSERT_TRUE(status.ok()) << status.message;
    EXPECT_EQ(after_failed_update_stats.snapshots_count, before_failed_update_stats.snapshots_count);
    EXPECT_EQ(after_failed_update_stats.deltas_count, before_failed_update_stats.deltas_count);

    const auto recovered_full = pipeline.capture_snapshot(root.path(), {}, false);
    ASSERT_TRUE(recovered_full.ok()) << recovered_full.snapshot_store_status.message;

    muninn::storage::StoreStats recovered_stats;
    status = store.get_stats(recovered_stats);
    ASSERT_TRUE(status.ok()) << status.message;
    EXPECT_EQ(recovered_stats.snapshots_count, before_failed_update_stats.snapshots_count + 1U);
}
