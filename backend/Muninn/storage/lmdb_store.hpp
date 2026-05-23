#ifndef MUNINN_LMDB_STORE_HPP
#define MUNINN_LMDB_STORE_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <lmdb.h>

#include "../domain/model/models.hpp"

namespace muninn::storage {

enum class StoreErrorCode : std::uint16_t {
    Ok = 0,
    InvalidArgument = 1,
    NotOpen = 2,
    IoFailure = 3,
    LmdbError = 4,
    NotFound = 5,
    SerializationFailure = 6,
    ValidationFailure = 7,
    CorruptedPayload = 8
};

struct StoreStatus {
    StoreErrorCode code {StoreErrorCode::Ok};
    int lmdb_rc {MDB_SUCCESS};
    std::string message {};

    [[nodiscard]] bool ok() const noexcept {
        return code == StoreErrorCode::Ok;
    }

    static StoreStatus success() {
        return StoreStatus {};
    }

    static StoreStatus failure(const StoreErrorCode code_value,
                               const std::string_view message_value,
                               const int lmdb_error = MDB_SUCCESS) {
        StoreStatus status;
        status.code = code_value;
        status.message = std::string(message_value);
        status.lmdb_rc = lmdb_error;
        return status;
    }
};

enum class TransportRecordType : std::uint8_t {
    Snapshot = 1,
    Delta = 2
};

struct TransportRecord {
    std::uint64_t sequence {0};
    std::uint64_t created_at {0};
    TransportRecordType type {TransportRecordType::Snapshot};
    std::string routing_key {};
    std::vector<std::uint8_t> payload {};
};

struct LmdbStoreOptions {
    std::filesystem::path directory {};
    std::size_t map_size_bytes {4ULL * 1024ULL * 1024ULL * 1024ULL};
    unsigned int max_dbs {16U};
    unsigned int max_readers {4096U};
    bool no_sync {false};
    bool write_map {false};
};

struct RetentionCleanupStats {
    std::size_t snapshots_removed {0U};
    std::size_t deltas_removed {0U};
};

struct SnapshotDeleteStats {
    std::size_t snapshots_removed {0U};
    std::size_t deltas_removed {0U};
};

struct StoreStats {
    std::size_t snapshots_count {0U};
    std::size_t deltas_count {0U};
    std::size_t transport_outbox_count {0U};
    std::size_t transport_consumers_count {0U};
    std::uint64_t next_transport_sequence {0U};
};

class LmdbStore final {
public:
    LmdbStore() = default;
    ~LmdbStore();

    LmdbStore(const LmdbStore&) = delete;
    LmdbStore& operator=(const LmdbStore&) = delete;

    LmdbStore(LmdbStore&& other) noexcept;
    LmdbStore& operator=(LmdbStore&& other) noexcept;

    StoreStatus open(const LmdbStoreOptions& options);
    void close();

    [[nodiscard]] bool is_open() const noexcept;

    StoreStatus put_snapshot(const domain::Snapshot& snapshot, bool validate_before_store = true);
    StoreStatus put_snapshot_with_transport(const domain::Snapshot& snapshot,
                                            bool enqueue_transport = true,
                                            bool validate_before_store = true,
                                            std::uint64_t* out_transport_sequence = nullptr);
    StoreStatus get_snapshot(domain::SnapshotId snapshot_id,
                             domain::Snapshot& out_snapshot,
                             bool validate_after_read = true) const;

    StoreStatus put_delta(const domain::DeltaSnapshot& delta,
                          const domain::Snapshot* base_snapshot = nullptr,
                          const domain::Snapshot* target_snapshot = nullptr,
                          bool validate_before_store = true);
    StoreStatus put_snapshot_and_delta_with_transport(const domain::Snapshot& snapshot,
                                                      const domain::DeltaSnapshot& delta,
                                                      const domain::Snapshot* base_snapshot = nullptr,
                                                      bool enqueue_transport = true,
                                                      bool validate_before_store = true,
                                                      std::uint64_t* out_snapshot_transport_sequence = nullptr,
                                                      std::uint64_t* out_delta_transport_sequence = nullptr);
    StoreStatus get_delta(domain::SnapshotId base_snapshot_id,
                          domain::SnapshotId target_snapshot_id,
                          domain::DeltaSnapshot& out_delta,
                          bool validate_after_read = true) const;

    StoreStatus enqueue_snapshot(const domain::Snapshot& snapshot,
                                 bool validate_before_store = true,
                                 std::uint64_t* out_sequence = nullptr);
    StoreStatus enqueue_delta(const domain::DeltaSnapshot& delta,
                              const domain::Snapshot* base_snapshot = nullptr,
                              const domain::Snapshot* target_snapshot = nullptr,
                              bool validate_before_store = true,
                              std::uint64_t* out_sequence = nullptr);

    StoreStatus register_transport_consumer(std::string_view consumer_id);
    StoreStatus fetch_transport_batch_for_consumer(std::string_view consumer_id,
                                                   std::size_t max_items,
                                                   std::vector<TransportRecord>& out_records) const;
    StoreStatus ack_transport_until_for_consumer(std::string_view consumer_id, std::uint64_t inclusive_sequence);
    StoreStatus compact_transport_up_to_min_acked();
    StoreStatus apply_snapshot_retention(std::size_t keep_latest_snapshots,
                                         RetentionCleanupStats* out_stats = nullptr);
    StoreStatus delete_snapshot(domain::SnapshotId snapshot_id,
                                SnapshotDeleteStats* out_stats = nullptr);
    StoreStatus get_stats(StoreStats& out_stats) const;

    StoreStatus fetch_transport_batch(std::size_t max_items, std::vector<TransportRecord>& out_records) const;
    StoreStatus ack_transport_until(std::uint64_t inclusive_sequence);

private:
    StoreStatus begin_txn(MDB_txn** out_txn, unsigned int flags) const;
    StoreStatus initialize_dbs(MDB_txn* write_txn);

    StoreStatus put_blob(MDB_dbi dbi,
                         MDB_txn* txn,
                         const void* key_data,
                         std::size_t key_size,
                         const void* value_data,
                         std::size_t value_size,
                         unsigned int flags = 0U) const;
    StoreStatus get_blob(MDB_dbi dbi,
                         MDB_txn* txn,
                         const void* key_data,
                         std::size_t key_size,
                         MDB_val& out_value) const;

    StoreStatus enqueue_transport_record(TransportRecordType type,
                                         std::string_view routing_key,
                                         const std::vector<std::uint8_t>& payload,
                                         std::uint64_t created_at,
                                         std::uint64_t* out_sequence);
    StoreStatus enqueue_transport_record_in_txn(MDB_txn* write_txn,
                                                TransportRecordType type,
                                                std::string_view routing_key,
                                                const std::vector<std::uint8_t>& payload,
                                                std::uint64_t created_at,
                                                std::uint64_t* out_sequence) const;

    StoreStatus reserve_next_sequence(MDB_txn* write_txn, std::uint64_t& out_sequence) const;
    StoreStatus read_next_sequence(MDB_txn* read_txn, std::uint64_t& out_sequence) const;
    StoreStatus write_next_sequence(MDB_txn* write_txn, std::uint64_t next_sequence) const;
    StoreStatus read_consumer_ack_sequence(MDB_txn* txn,
                                           std::string_view consumer_id,
                                           std::uint64_t& out_sequence) const;
    StoreStatus write_consumer_ack_sequence(MDB_txn* txn,
                                            std::string_view consumer_id,
                                            std::uint64_t sequence) const;

    mutable std::mutex writer_mutex_ {};
    MDB_env* env_ {nullptr};
    MDB_dbi meta_dbi_ {0};
    MDB_dbi snapshots_dbi_ {0};
    MDB_dbi deltas_dbi_ {0};
    MDB_dbi transport_dbi_ {0};
    MDB_dbi transport_consumers_dbi_ {0};
    bool open_ {false};
};

} // namespace muninn::storage

#endif // MUNINN_LMDB_STORE_HPP
