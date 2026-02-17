#ifndef LOCALEDGE_STORAGE_LMDB_INGEST_STORE_HPP
#define LOCALEDGE_STORAGE_LMDB_INGEST_STORE_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <lmdb.h>

#include "ingest_store.hpp"

namespace localedge::storage {

struct LmdbIngestStoreOptions {
    std::filesystem::path directory {};
    std::size_t map_size_bytes {4ULL * 1024ULL * 1024ULL * 1024ULL};
    unsigned int max_dbs {8U};
    unsigned int max_readers {2048U};
    bool no_sync {false};
    bool write_map {false};
};

class LmdbIngestStore final : public IIngestStore {
public:
    LmdbIngestStore() = default;
    ~LmdbIngestStore();

    LmdbIngestStore(const LmdbIngestStore&) = delete;
    LmdbIngestStore& operator=(const LmdbIngestStore&) = delete;

    common::Status open(const LmdbIngestStoreOptions& options);
    void close();

    [[nodiscard]] bool is_open() const noexcept;

    common::StatusOr<std::uint64_t> get_checkpoint(std::string_view host_id) const override;
    common::StatusOr<bool> has_record(std::string_view host_id, std::uint64_t sequence) const override;
    common::Status append_if_absent_and_checkpoint(const IngestLogRecord& record) override;

private:
    common::Status begin_txn(MDB_txn** out_txn, unsigned int flags) const;
    common::Status initialize_dbs(MDB_txn* txn);

    mutable std::mutex writer_mutex_ {};
    MDB_env* env_ {nullptr};
    MDB_dbi meta_dbi_ {0};
    MDB_dbi ingest_log_dbi_ {0};
    MDB_dbi checkpoints_dbi_ {0};
    bool open_ {false};
};

} // namespace localedge::storage

#endif // LOCALEDGE_STORAGE_LMDB_INGEST_STORE_HPP
