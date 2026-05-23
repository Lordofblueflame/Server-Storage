#include "lmdb_ingest_store.hpp"

#include <algorithm>
#include <array>
#include <sstream>

#include "../common/byte_codec.hpp"

namespace hugin::storage {
namespace {

constexpr std::string_view kDbMeta = "meta";
constexpr std::string_view kDbIngestLog = "ingest_log";
constexpr std::string_view kDbCheckpoints = "checkpoints";

constexpr std::string_view kMetaStoreVersion = "store_version";
constexpr std::uint8_t kRecordVersion = 1U;

std::array<std::uint8_t, 8> encode_u64_be(const std::uint64_t value) {
    std::array<std::uint8_t, 8> out {};
    out[0] = static_cast<std::uint8_t>((value >> 56U) & 0xFFU);
    out[1] = static_cast<std::uint8_t>((value >> 48U) & 0xFFU);
    out[2] = static_cast<std::uint8_t>((value >> 40U) & 0xFFU);
    out[3] = static_cast<std::uint8_t>((value >> 32U) & 0xFFU);
    out[4] = static_cast<std::uint8_t>((value >> 24U) & 0xFFU);
    out[5] = static_cast<std::uint8_t>((value >> 16U) & 0xFFU);
    out[6] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
    out[7] = static_cast<std::uint8_t>(value & 0xFFU);
    return out;
}

std::uint64_t decode_u64_be(const void* bytes) {
    const auto* raw = static_cast<const std::uint8_t*>(bytes);
    return (static_cast<std::uint64_t>(raw[0]) << 56U) |
           (static_cast<std::uint64_t>(raw[1]) << 48U) |
           (static_cast<std::uint64_t>(raw[2]) << 40U) |
           (static_cast<std::uint64_t>(raw[3]) << 32U) |
           (static_cast<std::uint64_t>(raw[4]) << 24U) |
           (static_cast<std::uint64_t>(raw[5]) << 16U) |
           (static_cast<std::uint64_t>(raw[6]) << 8U) |
           static_cast<std::uint64_t>(raw[7]);
}

common::Status lmdb_error(const int rc, const std::string_view message) {
    std::ostringstream text;
    text << message << " (rc=" << rc << ", detail=" << mdb_strerror(rc) << ")";
    return common::Status::failure(common::ErrorCode::StorageError, text.str());
}

std::vector<std::uint8_t> make_ingest_key(const std::string_view host_id, const std::uint64_t sequence) {
    std::vector<std::uint8_t> key;
    key.reserve(2U + host_id.size() + 8U);
    common::append_u16_be(key, static_cast<std::uint16_t>(host_id.size()));
    key.insert(key.end(), host_id.begin(), host_id.end());
    common::append_u64_be(key, sequence);
    return key;
}

std::vector<std::uint8_t> serialize_record_value(const IngestLogRecord& record) {
    std::vector<std::uint8_t> value;
    value.reserve(1U + 8U + 4U + record.raw_frame.size());
    value.push_back(kRecordVersion);
    common::append_u64_be(value, record.received_at_unix_seconds);
    common::append_u32_be(value, static_cast<std::uint32_t>(record.raw_frame.size()));
    value.insert(value.end(), record.raw_frame.begin(), record.raw_frame.end());
    return value;
}

common::Status put_blob(MDB_txn* txn,
                        const MDB_dbi dbi,
                        const void* key_data,
                        const std::size_t key_size,
                        const void* value_data,
                        const std::size_t value_size,
                        const unsigned int flags) {
    MDB_val key {
        key_size,
        const_cast<void*>(key_data)
    };
    MDB_val value {
        value_size,
        const_cast<void*>(value_data)
    };
    const int rc = mdb_put(txn, dbi, &key, &value, flags);
    if (rc != MDB_SUCCESS) {
        return lmdb_error(rc, "mdb_put");
    }
    return common::Status::success();
}

int get_blob(const MDB_txn* txn,
             const MDB_dbi dbi,
             const void* key_data,
             const std::size_t key_size,
             MDB_val& out_value) {
    MDB_val key {
        key_size,
        const_cast<void*>(key_data)
    };
    return mdb_get(const_cast<MDB_txn*>(txn), dbi, &key, &out_value);
}

common::StatusOr<std::uint64_t> checkpoint_in_txn(const MDB_txn* txn, const MDB_dbi dbi, const std::string_view host_id) {
    MDB_val value {};
    const int rc = get_blob(txn, dbi, host_id.data(), host_id.size(), value);
    if (rc == MDB_NOTFOUND) {
        return common::StatusOr<std::uint64_t>::success(0U);
    }
    if (rc != MDB_SUCCESS) {
        return common::StatusOr<std::uint64_t>::failure(common::ErrorCode::StorageError, "mdb_get(checkpoint) failed.");
    }
    if (value.mv_size != 8U) {
        return common::StatusOr<std::uint64_t>::failure(common::ErrorCode::ParseError, "Checkpoint payload size is invalid.");
    }
    return common::StatusOr<std::uint64_t>::success(decode_u64_be(value.mv_data));
}

} // namespace

LmdbIngestStore::~LmdbIngestStore() {
    close();
}

common::Status LmdbIngestStore::open(const LmdbIngestStoreOptions& options) {
    if (open_) {
        return common::Status::success();
    }

    if (options.directory.empty()) {
        return common::Status::failure(common::ErrorCode::InvalidArgument, "LMDB directory is empty.");
    }

    std::error_code filesystem_error;
    std::filesystem::create_directories(options.directory, filesystem_error);
    if (filesystem_error) {
        return common::Status::failure(
            common::ErrorCode::StorageError,
            "Failed to create LMDB directory: " + filesystem_error.message()
        );
    }

    int rc = mdb_env_create(&env_);
    if (rc != MDB_SUCCESS) {
        return lmdb_error(rc, "mdb_env_create");
    }

    rc = mdb_env_set_mapsize(env_, options.map_size_bytes);
    if (rc != MDB_SUCCESS) {
        close();
        return lmdb_error(rc, "mdb_env_set_mapsize");
    }

    rc = mdb_env_set_maxdbs(env_, options.max_dbs);
    if (rc != MDB_SUCCESS) {
        close();
        return lmdb_error(rc, "mdb_env_set_maxdbs");
    }

    rc = mdb_env_set_maxreaders(env_, options.max_readers);
    if (rc != MDB_SUCCESS) {
        close();
        return lmdb_error(rc, "mdb_env_set_maxreaders");
    }

    unsigned int env_flags = 0U;
    if (options.no_sync) {
        env_flags |= MDB_NOSYNC;
    }
    if (options.write_map) {
        env_flags |= MDB_WRITEMAP;
    }

    const auto path = options.directory.string();
    rc = mdb_env_open(env_, path.c_str(), env_flags, 0664);
    if (rc != MDB_SUCCESS) {
        close();
        return lmdb_error(rc, "mdb_env_open");
    }

    open_ = true;

    MDB_txn* write_txn = nullptr;
    auto status = begin_txn(&write_txn, 0U);
    if (!status.ok()) {
        close();
        return status;
    }

    status = initialize_dbs(write_txn);
    if (!status.ok()) {
        mdb_txn_abort(write_txn);
        close();
        return status;
    }

    rc = mdb_txn_commit(write_txn);
    if (rc != MDB_SUCCESS) {
        close();
        return lmdb_error(rc, "mdb_txn_commit(open)");
    }

    return common::Status::success();
}

void LmdbIngestStore::close() {
    if (env_ != nullptr) {
        mdb_dbi_close(env_, meta_dbi_);
        mdb_dbi_close(env_, ingest_log_dbi_);
        mdb_dbi_close(env_, checkpoints_dbi_);
        mdb_env_close(env_);
        env_ = nullptr;
    }
    meta_dbi_ = 0;
    ingest_log_dbi_ = 0;
    checkpoints_dbi_ = 0;
    open_ = false;
}

bool LmdbIngestStore::is_open() const noexcept {
    return open_ && env_ != nullptr;
}

common::Status LmdbIngestStore::begin_txn(MDB_txn** out_txn, const unsigned int flags) const {
    if (!is_open()) {
        return common::Status::failure(common::ErrorCode::StorageError, "LMDB ingest store is not open.");
    }
    const int rc = mdb_txn_begin(env_, nullptr, flags, out_txn);
    if (rc != MDB_SUCCESS) {
        return lmdb_error(rc, "mdb_txn_begin");
    }
    return common::Status::success();
}

common::Status LmdbIngestStore::initialize_dbs(MDB_txn* txn) {
    if (txn == nullptr) {
        return common::Status::failure(common::ErrorCode::InvalidArgument, "LMDB write transaction is null.");
    }

    int rc = mdb_dbi_open(txn, kDbMeta.data(), MDB_CREATE, &meta_dbi_);
    if (rc != MDB_SUCCESS) {
        return lmdb_error(rc, "mdb_dbi_open(meta)");
    }
    rc = mdb_dbi_open(txn, kDbIngestLog.data(), MDB_CREATE, &ingest_log_dbi_);
    if (rc != MDB_SUCCESS) {
        return lmdb_error(rc, "mdb_dbi_open(ingest_log)");
    }
    rc = mdb_dbi_open(txn, kDbCheckpoints.data(), MDB_CREATE, &checkpoints_dbi_);
    if (rc != MDB_SUCCESS) {
        return lmdb_error(rc, "mdb_dbi_open(checkpoints)");
    }

    MDB_val existing {};
    rc = get_blob(txn, meta_dbi_, kMetaStoreVersion.data(), kMetaStoreVersion.size(), existing);
    if (rc == MDB_NOTFOUND) {
        const std::array<std::uint8_t, 1> version {1U};
        return put_blob(
            txn,
            meta_dbi_,
            kMetaStoreVersion.data(),
            kMetaStoreVersion.size(),
            version.data(),
            version.size(),
            0U
        );
    }
    if (rc != MDB_SUCCESS) {
        return lmdb_error(rc, "mdb_get(meta:store_version)");
    }
    return common::Status::success();
}

common::StatusOr<std::uint64_t> LmdbIngestStore::get_checkpoint(const std::string_view host_id) const {
    if (host_id.empty()) {
        return common::StatusOr<std::uint64_t>::failure(common::ErrorCode::InvalidArgument, "Host id is empty.");
    }

    MDB_txn* read_txn = nullptr;
    auto status = begin_txn(&read_txn, MDB_RDONLY);
    if (!status.ok()) {
        return common::StatusOr<std::uint64_t>::failure(status.code, status.message);
    }

    auto checkpoint = checkpoint_in_txn(read_txn, checkpoints_dbi_, host_id);
    mdb_txn_abort(read_txn);
    return checkpoint;
}

common::StatusOr<bool> LmdbIngestStore::has_record(const std::string_view host_id, const std::uint64_t sequence) const {
    if (host_id.empty() || sequence == 0U) {
        return common::StatusOr<bool>::failure(common::ErrorCode::InvalidArgument, "Host id or sequence is invalid.");
    }
    if (host_id.size() > static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max())) {
        return common::StatusOr<bool>::failure(common::ErrorCode::InvalidArgument, "Host id is too long.");
    }

    MDB_txn* read_txn = nullptr;
    auto status = begin_txn(&read_txn, MDB_RDONLY);
    if (!status.ok()) {
        return common::StatusOr<bool>::failure(status.code, status.message);
    }

    const auto key = make_ingest_key(host_id, sequence);
    MDB_val value {};
    const int rc = get_blob(read_txn, ingest_log_dbi_, key.data(), key.size(), value);
    mdb_txn_abort(read_txn);
    if (rc == MDB_NOTFOUND) {
        return common::StatusOr<bool>::success(false);
    }
    if (rc != MDB_SUCCESS) {
        return common::StatusOr<bool>::failure(common::ErrorCode::StorageError, "mdb_get(ingest_log) failed.");
    }
    return common::StatusOr<bool>::success(true);
}

common::Status LmdbIngestStore::append_if_absent_and_checkpoint(const IngestLogRecord& record) {
    if (record.host_id.empty()) {
        return common::Status::failure(common::ErrorCode::InvalidArgument, "Host id is empty.");
    }
    if (record.sequence == 0U) {
        return common::Status::failure(common::ErrorCode::InvalidArgument, "Sequence must be non-zero.");
    }
    if (record.host_id.size() > static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max())) {
        return common::Status::failure(common::ErrorCode::InvalidArgument, "Host id is too long.");
    }
    if (record.raw_frame.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        return common::Status::failure(common::ErrorCode::InvalidArgument, "Raw frame is too large.");
    }

    std::lock_guard<std::mutex> lock(writer_mutex_);
    MDB_txn* write_txn = nullptr;
    auto status = begin_txn(&write_txn, 0U);
    if (!status.ok()) {
        return status;
    }

    const auto key = make_ingest_key(record.host_id, record.sequence);
    const auto value = serialize_record_value(record);

    MDB_val key_val {
        key.size(),
        const_cast<std::uint8_t*>(key.data())
    };
    MDB_val value_val {
        value.size(),
        const_cast<std::uint8_t*>(value.data())
    };

    int rc = mdb_put(write_txn, ingest_log_dbi_, &key_val, &value_val, MDB_NOOVERWRITE);
    if (rc != MDB_SUCCESS && rc != MDB_KEYEXIST) {
        mdb_txn_abort(write_txn);
        return lmdb_error(rc, "mdb_put(ingest_log)");
    }

    auto checkpoint = checkpoint_in_txn(write_txn, checkpoints_dbi_, record.host_id);
    if (!checkpoint.ok()) {
        mdb_txn_abort(write_txn);
        return checkpoint.status;
    }

    const std::uint64_t next_checkpoint = std::max(checkpoint.value, record.sequence);
    if (next_checkpoint > checkpoint.value) {
        const auto encoded = encode_u64_be(next_checkpoint);
        status = put_blob(
            write_txn,
            checkpoints_dbi_,
            record.host_id.data(),
            record.host_id.size(),
            encoded.data(),
            encoded.size(),
            0U
        );
        if (!status.ok()) {
            mdb_txn_abort(write_txn);
            return status;
        }
    }

    rc = mdb_txn_commit(write_txn);
    if (rc != MDB_SUCCESS) {
        return lmdb_error(rc, "mdb_txn_commit(append_if_absent_and_checkpoint)");
    }
    return common::Status::success();
}

} // namespace hugin::storage
