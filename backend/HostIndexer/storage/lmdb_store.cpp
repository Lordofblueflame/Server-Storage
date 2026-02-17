#include "lmdb_store.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <sstream>
#include <utility>

#include "../domain/serialization/serialization.hpp"
#include "../domain/validation/validate_snapshot.hpp"

namespace host_indexer::storage {
namespace {

constexpr std::string_view kDbMeta = "meta";
constexpr std::string_view kDbSnapshots = "snapshots";
constexpr std::string_view kDbDeltas = "deltas";
constexpr std::string_view kDbTransport = "transport_outbox";

constexpr std::string_view kKeyNextSequence = "next_transport_sequence";

struct DeltaKey {
    std::array<std::uint8_t, 16> bytes {};
};

std::array<std::uint8_t, 8> encode_u64_be(const std::uint64_t value) {
    std::array<std::uint8_t, 8> encoded {};
    encoded[0] = static_cast<std::uint8_t>((value >> 56U) & 0xFFU);
    encoded[1] = static_cast<std::uint8_t>((value >> 48U) & 0xFFU);
    encoded[2] = static_cast<std::uint8_t>((value >> 40U) & 0xFFU);
    encoded[3] = static_cast<std::uint8_t>((value >> 32U) & 0xFFU);
    encoded[4] = static_cast<std::uint8_t>((value >> 24U) & 0xFFU);
    encoded[5] = static_cast<std::uint8_t>((value >> 16U) & 0xFFU);
    encoded[6] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
    encoded[7] = static_cast<std::uint8_t>(value & 0xFFU);
    return encoded;
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

DeltaKey make_delta_key(const domain::SnapshotId base_snapshot_id,
                        const domain::SnapshotId target_snapshot_id) {
    DeltaKey key {};
    const auto base_encoded = encode_u64_be(base_snapshot_id);
    const auto target_encoded = encode_u64_be(target_snapshot_id);
    std::copy(base_encoded.begin(), base_encoded.end(), key.bytes.begin());
    std::copy(target_encoded.begin(), target_encoded.end(), key.bytes.begin() + 8);
    return key;
}

std::uint64_t unix_now_u64() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
}

StoreStatus lmdb_error(const int rc, const std::string_view message) {
    std::ostringstream stream;
    stream << message << " (rc=" << rc << ", detail=" << mdb_strerror(rc) << ")";
    return StoreStatus::failure(StoreErrorCode::LmdbError, stream.str(), rc);
}

StoreStatus validate_snapshot_for_store(const domain::Snapshot& snapshot) {
    const auto report = validation::validate_snapshot(snapshot);
    if (report.has_errors()) {
        std::ostringstream stream;
        stream << "Snapshot validation failed with " << report.issues.size() << " issues.";
        return StoreStatus::failure(StoreErrorCode::ValidationFailure, stream.str());
    }
    return StoreStatus::success();
}

StoreStatus validate_delta_for_store(const domain::DeltaSnapshot& delta,
                                     const domain::Snapshot* base_snapshot,
                                     const domain::Snapshot* target_snapshot) {
    const auto report = validation::validate_delta(delta, base_snapshot, target_snapshot);
    if (report.has_errors()) {
        std::ostringstream stream;
        stream << "Delta validation failed with " << report.issues.size() << " issues.";
        return StoreStatus::failure(StoreErrorCode::ValidationFailure, stream.str());
    }
    return StoreStatus::success();
}

std::vector<std::uint8_t> serialize_frame(const TransportRecordType type,
                                          const std::string_view routing_key,
                                          const std::vector<std::uint8_t>& payload,
                                          const std::uint64_t created_at) {
    // Frame format:
    // [1:type][8:created_at][4:routing_key_size][N:routing_key][4:payload_size][M:payload]
    const std::uint32_t routing_size = static_cast<std::uint32_t>(routing_key.size());
    const std::uint32_t payload_size = static_cast<std::uint32_t>(payload.size());

    std::vector<std::uint8_t> frame;
    frame.reserve(1U + 8U + 4U + routing_size + 4U + payload_size);
    frame.push_back(static_cast<std::uint8_t>(type));

    const auto created = encode_u64_be(created_at);
    frame.insert(frame.end(), created.begin(), created.end());

    const auto route_len = encode_u64_be(static_cast<std::uint64_t>(routing_size));
    frame.insert(frame.end(), route_len.begin() + 4, route_len.end());
    frame.insert(frame.end(), routing_key.begin(), routing_key.end());

    const auto payload_len = encode_u64_be(static_cast<std::uint64_t>(payload_size));
    frame.insert(frame.end(), payload_len.begin() + 4, payload_len.end());
    frame.insert(frame.end(), payload.begin(), payload.end());

    return frame;
}

StoreStatus parse_frame(const MDB_val& value, TransportRecord& out_record) {
    const auto* raw = static_cast<const std::uint8_t*>(value.mv_data);
    const auto size = static_cast<std::size_t>(value.mv_size);

    if (size < 1U + 8U + 4U + 4U) {
        return StoreStatus::failure(StoreErrorCode::CorruptedPayload, "Transport frame is truncated.");
    }

    std::size_t offset = 0;
    out_record.type = static_cast<TransportRecordType>(raw[offset]);
    offset += 1;

    out_record.created_at = decode_u64_be(raw + offset);
    offset += 8U;

    std::array<std::uint8_t, 8> route_size_be {};
    route_size_be.fill(0);
    std::memcpy(route_size_be.data() + 4, raw + offset, 4U);
    const auto route_size = static_cast<std::size_t>(decode_u64_be(route_size_be.data()));
    offset += 4U;

    if (offset + route_size + 4U > size) {
        return StoreStatus::failure(StoreErrorCode::CorruptedPayload, "Transport frame routing key is invalid.");
    }

    out_record.routing_key.assign(reinterpret_cast<const char*>(raw + offset), route_size);
    offset += route_size;

    std::array<std::uint8_t, 8> payload_size_be {};
    payload_size_be.fill(0);
    std::memcpy(payload_size_be.data() + 4, raw + offset, 4U);
    const auto payload_size = static_cast<std::size_t>(decode_u64_be(payload_size_be.data()));
    offset += 4U;

    if (offset + payload_size != size) {
        return StoreStatus::failure(StoreErrorCode::CorruptedPayload, "Transport frame payload size is invalid.");
    }

    out_record.payload.resize(payload_size);
    if (payload_size > 0) {
        std::memcpy(out_record.payload.data(), raw + offset, payload_size);
    }

    return StoreStatus::success();
}

StoreStatus serialization_to_store_status(const serialization::SerializationResult& result,
                                          const std::string_view operation) {
    if (result.ok) {
        return StoreStatus::success();
    }

    std::ostringstream stream;
    stream << operation << " failed: " << result.message
           << " (serialization_code=" << static_cast<std::uint16_t>(result.code) << ")";
    return StoreStatus::failure(StoreErrorCode::SerializationFailure, stream.str());
}

} // namespace

LmdbStore::~LmdbStore() {
    close();
}

LmdbStore::LmdbStore(LmdbStore&& other) noexcept {
    env_ = std::exchange(other.env_, nullptr);
    meta_dbi_ = std::exchange(other.meta_dbi_, 0);
    snapshots_dbi_ = std::exchange(other.snapshots_dbi_, 0);
    deltas_dbi_ = std::exchange(other.deltas_dbi_, 0);
    transport_dbi_ = std::exchange(other.transport_dbi_, 0);
    open_ = std::exchange(other.open_, false);
}

LmdbStore& LmdbStore::operator=(LmdbStore&& other) noexcept {
    if (this != &other) {
        close();
        env_ = std::exchange(other.env_, nullptr);
        meta_dbi_ = std::exchange(other.meta_dbi_, 0);
        snapshots_dbi_ = std::exchange(other.snapshots_dbi_, 0);
        deltas_dbi_ = std::exchange(other.deltas_dbi_, 0);
        transport_dbi_ = std::exchange(other.transport_dbi_, 0);
        open_ = std::exchange(other.open_, false);
    }
    return *this;
}

StoreStatus LmdbStore::open(const LmdbStoreOptions& options) {
    if (open_) {
        return StoreStatus::success();
    }

    if (options.directory.empty()) {
        return StoreStatus::failure(StoreErrorCode::InvalidArgument, "LMDB directory must not be empty.");
    }

    std::error_code filesystem_error;
    std::filesystem::create_directories(options.directory, filesystem_error);
    if (filesystem_error) {
        return StoreStatus::failure(
            StoreErrorCode::IoFailure,
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

    unsigned int flags = 0U;
    if (options.no_sync) {
        flags |= MDB_NOSYNC;
    }
    if (options.write_map) {
        flags |= MDB_WRITEMAP;
    }

    const auto path = options.directory.string();
    rc = mdb_env_open(env_, path.c_str(), flags, 0664);
    if (rc != MDB_SUCCESS) {
        close();
        return lmdb_error(rc, "mdb_env_open");
    }
    open_ = true;

    MDB_txn* write_txn = nullptr;
    const auto begin_status = begin_txn(&write_txn, 0U);
    if (!begin_status.ok()) {
        close();
        return begin_status;
    }

    const auto init_status = initialize_dbs(write_txn);
    if (!init_status.ok()) {
        mdb_txn_abort(write_txn);
        close();
        return init_status;
    }

    rc = mdb_txn_commit(write_txn);
    if (rc != MDB_SUCCESS) {
        close();
        return lmdb_error(rc, "mdb_txn_commit(open)");
    }

    return StoreStatus::success();
}

void LmdbStore::close() {
    if (env_ != nullptr) {
        mdb_dbi_close(env_, meta_dbi_);
        mdb_dbi_close(env_, snapshots_dbi_);
        mdb_dbi_close(env_, deltas_dbi_);
        mdb_dbi_close(env_, transport_dbi_);
        mdb_env_close(env_);
        env_ = nullptr;
    }

    meta_dbi_ = 0;
    snapshots_dbi_ = 0;
    deltas_dbi_ = 0;
    transport_dbi_ = 0;
    open_ = false;
}

bool LmdbStore::is_open() const noexcept {
    return open_ && env_ != nullptr;
}

StoreStatus LmdbStore::begin_txn(MDB_txn** out_txn, const unsigned int flags) const {
    if (!is_open()) {
        return StoreStatus::failure(StoreErrorCode::NotOpen, "LMDB store is not open.");
    }

    const int rc = mdb_txn_begin(env_, nullptr, flags, out_txn);
    if (rc != MDB_SUCCESS) {
        return lmdb_error(rc, "mdb_txn_begin");
    }
    return StoreStatus::success();
}

StoreStatus LmdbStore::initialize_dbs(MDB_txn* write_txn) {
    if (write_txn == nullptr) {
        return StoreStatus::failure(StoreErrorCode::InvalidArgument, "Write transaction is null.");
    }

    int rc = mdb_dbi_open(write_txn, kDbMeta.data(), MDB_CREATE, &meta_dbi_);
    if (rc != MDB_SUCCESS) {
        return lmdb_error(rc, "mdb_dbi_open(meta)");
    }

    rc = mdb_dbi_open(write_txn, kDbSnapshots.data(), MDB_CREATE, &snapshots_dbi_);
    if (rc != MDB_SUCCESS) {
        return lmdb_error(rc, "mdb_dbi_open(snapshots)");
    }

    rc = mdb_dbi_open(write_txn, kDbDeltas.data(), MDB_CREATE, &deltas_dbi_);
    if (rc != MDB_SUCCESS) {
        return lmdb_error(rc, "mdb_dbi_open(deltas)");
    }

    rc = mdb_dbi_open(write_txn, kDbTransport.data(), MDB_CREATE, &transport_dbi_);
    if (rc != MDB_SUCCESS) {
        return lmdb_error(rc, "mdb_dbi_open(transport)");
    }

    std::uint64_t current_next = 0;
    const auto read_next = read_next_sequence(write_txn, current_next);
    if (!read_next.ok() && read_next.code != StoreErrorCode::NotFound) {
        return read_next;
    }

    if (read_next.code == StoreErrorCode::NotFound) {
        const auto write_status = write_next_sequence(write_txn, 1U);
        if (!write_status.ok()) {
            return write_status;
        }
    }

    return StoreStatus::success();
}

StoreStatus LmdbStore::put_blob(MDB_dbi dbi,
                                MDB_txn* txn,
                                const void* key_data,
                                const std::size_t key_size,
                                const void* value_data,
                                const std::size_t value_size,
                                const unsigned int flags) const {
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
    return StoreStatus::success();
}

StoreStatus LmdbStore::get_blob(MDB_dbi dbi,
                                MDB_txn* txn,
                                const void* key_data,
                                const std::size_t key_size,
                                MDB_val& out_value) const {
    MDB_val key {
        key_size,
        const_cast<void*>(key_data)
    };

    const int rc = mdb_get(txn, dbi, &key, &out_value);
    if (rc == MDB_NOTFOUND) {
        return StoreStatus::failure(StoreErrorCode::NotFound, "Record not found.");
    }
    if (rc != MDB_SUCCESS) {
        return lmdb_error(rc, "mdb_get");
    }
    return StoreStatus::success();
}

StoreStatus LmdbStore::put_snapshot(const domain::Snapshot& snapshot, const bool validate_before_store) {
    if (!is_open()) {
        return StoreStatus::failure(StoreErrorCode::NotOpen, "LMDB store is not open.");
    }

    if (validate_before_store) {
        const auto validate_status = validate_snapshot_for_store(snapshot);
        if (!validate_status.ok()) {
            return validate_status;
        }
    }

    std::ostringstream buffer(std::ios::binary);
    const auto serialize = serialization::serialize_snapshot_binary(snapshot, buffer);
    const auto serialize_status = serialization_to_store_status(serialize, "serialize_snapshot_binary");
    if (!serialize_status.ok()) {
        return serialize_status;
    }
    const std::string payload = buffer.str();
    const auto key = encode_u64_be(snapshot.snapshot_id);

    std::lock_guard<std::mutex> lock(writer_mutex_);
    MDB_txn* write_txn = nullptr;
    auto status = begin_txn(&write_txn, 0U);
    if (!status.ok()) {
        return status;
    }

    status = put_blob(
        snapshots_dbi_,
        write_txn,
        key.data(),
        key.size(),
        payload.data(),
        payload.size(),
        0U
    );
    if (!status.ok()) {
        mdb_txn_abort(write_txn);
        return status;
    }

    const int commit_rc = mdb_txn_commit(write_txn);
    if (commit_rc != MDB_SUCCESS) {
        return lmdb_error(commit_rc, "mdb_txn_commit(put_snapshot)");
    }

    return StoreStatus::success();
}

StoreStatus LmdbStore::get_snapshot(const domain::SnapshotId snapshot_id,
                                    domain::Snapshot& out_snapshot,
                                    const bool validate_after_read) const {
    if (!is_open()) {
        return StoreStatus::failure(StoreErrorCode::NotOpen, "LMDB store is not open.");
    }

    MDB_txn* read_txn = nullptr;
    auto status = begin_txn(&read_txn, MDB_RDONLY);
    if (!status.ok()) {
        return status;
    }

    const auto key = encode_u64_be(snapshot_id);
    MDB_val value {};
    status = get_blob(snapshots_dbi_, read_txn, key.data(), key.size(), value);
    if (!status.ok()) {
        mdb_txn_abort(read_txn);
        return status;
    }

    std::string payload(static_cast<const char*>(value.mv_data), value.mv_size);
    mdb_txn_abort(read_txn);

    std::istringstream input(payload, std::ios::binary);
    const auto deserialize = serialization::deserialize_snapshot_binary(input, out_snapshot);
    const auto deserialize_status = serialization_to_store_status(deserialize, "deserialize_snapshot_binary");
    if (!deserialize_status.ok()) {
        return deserialize_status;
    }

    if (validate_after_read) {
        const auto validation_status = validate_snapshot_for_store(out_snapshot);
        if (!validation_status.ok()) {
            return validation_status;
        }
    }

    return StoreStatus::success();
}

StoreStatus LmdbStore::put_delta(const domain::DeltaSnapshot& delta,
                                 const domain::Snapshot* base_snapshot,
                                 const domain::Snapshot* target_snapshot,
                                 const bool validate_before_store) {
    if (!is_open()) {
        return StoreStatus::failure(StoreErrorCode::NotOpen, "LMDB store is not open.");
    }

    if (validate_before_store) {
        const auto validate_status = validate_delta_for_store(delta, base_snapshot, target_snapshot);
        if (!validate_status.ok()) {
            return validate_status;
        }
    }

    std::ostringstream buffer(std::ios::binary);
    const auto serialize = serialization::serialize_delta_binary(delta, buffer);
    const auto serialize_status = serialization_to_store_status(serialize, "serialize_delta_binary");
    if (!serialize_status.ok()) {
        return serialize_status;
    }
    const std::string payload = buffer.str();
    const auto key = make_delta_key(delta.base_snapshot_id, delta.target_snapshot_id);

    std::lock_guard<std::mutex> lock(writer_mutex_);
    MDB_txn* write_txn = nullptr;
    auto status = begin_txn(&write_txn, 0U);
    if (!status.ok()) {
        return status;
    }

    status = put_blob(
        deltas_dbi_,
        write_txn,
        key.bytes.data(),
        key.bytes.size(),
        payload.data(),
        payload.size(),
        0U
    );
    if (!status.ok()) {
        mdb_txn_abort(write_txn);
        return status;
    }

    const int commit_rc = mdb_txn_commit(write_txn);
    if (commit_rc != MDB_SUCCESS) {
        return lmdb_error(commit_rc, "mdb_txn_commit(put_delta)");
    }

    return StoreStatus::success();
}

StoreStatus LmdbStore::get_delta(const domain::SnapshotId base_snapshot_id,
                                 const domain::SnapshotId target_snapshot_id,
                                 domain::DeltaSnapshot& out_delta,
                                 const bool validate_after_read) const {
    if (!is_open()) {
        return StoreStatus::failure(StoreErrorCode::NotOpen, "LMDB store is not open.");
    }

    MDB_txn* read_txn = nullptr;
    auto status = begin_txn(&read_txn, MDB_RDONLY);
    if (!status.ok()) {
        return status;
    }

    const auto key = make_delta_key(base_snapshot_id, target_snapshot_id);
    MDB_val value {};
    status = get_blob(deltas_dbi_, read_txn, key.bytes.data(), key.bytes.size(), value);
    if (!status.ok()) {
        mdb_txn_abort(read_txn);
        return status;
    }

    std::string payload(static_cast<const char*>(value.mv_data), value.mv_size);
    mdb_txn_abort(read_txn);

    std::istringstream input(payload, std::ios::binary);
    const auto deserialize = serialization::deserialize_delta_binary(input, out_delta);
    const auto deserialize_status = serialization_to_store_status(deserialize, "deserialize_delta_binary");
    if (!deserialize_status.ok()) {
        return deserialize_status;
    }

    if (validate_after_read) {
        const auto validation_status = validate_delta_for_store(out_delta, nullptr, nullptr);
        if (!validation_status.ok()) {
            return validation_status;
        }
    }

    return StoreStatus::success();
}

StoreStatus LmdbStore::enqueue_snapshot(const domain::Snapshot& snapshot,
                                        const bool validate_before_store,
                                        std::uint64_t* out_sequence) {
    if (validate_before_store) {
        const auto validate_status = validate_snapshot_for_store(snapshot);
        if (!validate_status.ok()) {
            return validate_status;
        }
    }

    std::ostringstream buffer(std::ios::binary);
    const auto serialize = serialization::serialize_snapshot_binary(snapshot, buffer);
    const auto serialize_status = serialization_to_store_status(serialize, "serialize_snapshot_binary");
    if (!serialize_status.ok()) {
        return serialize_status;
    }

    const auto payload_string = buffer.str();
    const std::vector<std::uint8_t> payload(payload_string.begin(), payload_string.end());

    return enqueue_transport_record(
        TransportRecordType::Snapshot,
        snapshot.host_identifier,
        payload,
        unix_now_u64(),
        out_sequence
    );
}

StoreStatus LmdbStore::enqueue_delta(const domain::DeltaSnapshot& delta,
                                     const domain::Snapshot* base_snapshot,
                                     const domain::Snapshot* target_snapshot,
                                     const bool validate_before_store,
                                     std::uint64_t* out_sequence) {
    if (validate_before_store) {
        const auto validate_status = validate_delta_for_store(delta, base_snapshot, target_snapshot);
        if (!validate_status.ok()) {
            return validate_status;
        }
    }

    std::ostringstream buffer(std::ios::binary);
    const auto serialize = serialization::serialize_delta_binary(delta, buffer);
    const auto serialize_status = serialization_to_store_status(serialize, "serialize_delta_binary");
    if (!serialize_status.ok()) {
        return serialize_status;
    }

    const auto payload_string = buffer.str();
    const std::vector<std::uint8_t> payload(payload_string.begin(), payload_string.end());

    std::ostringstream route;
    route << delta.base_snapshot_id << "->" << delta.target_snapshot_id;

    return enqueue_transport_record(
        TransportRecordType::Delta,
        route.str(),
        payload,
        unix_now_u64(),
        out_sequence
    );
}

StoreStatus LmdbStore::enqueue_transport_record(const TransportRecordType type,
                                                const std::string_view routing_key,
                                                const std::vector<std::uint8_t>& payload,
                                                const std::uint64_t created_at,
                                                std::uint64_t* out_sequence) {
    if (!is_open()) {
        return StoreStatus::failure(StoreErrorCode::NotOpen, "LMDB store is not open.");
    }

    std::lock_guard<std::mutex> lock(writer_mutex_);
    MDB_txn* write_txn = nullptr;
    auto status = begin_txn(&write_txn, 0U);
    if (!status.ok()) {
        return status;
    }

    std::uint64_t sequence = 0;
    status = reserve_next_sequence(write_txn, sequence);
    if (!status.ok()) {
        mdb_txn_abort(write_txn);
        return status;
    }

    const auto key = encode_u64_be(sequence);
    const auto frame = serialize_frame(type, routing_key, payload, created_at);

    status = put_blob(
        transport_dbi_,
        write_txn,
        key.data(),
        key.size(),
        frame.data(),
        frame.size(),
        0U
    );
    if (!status.ok()) {
        mdb_txn_abort(write_txn);
        return status;
    }

    const int commit_rc = mdb_txn_commit(write_txn);
    if (commit_rc != MDB_SUCCESS) {
        return lmdb_error(commit_rc, "mdb_txn_commit(enqueue_transport_record)");
    }

    if (out_sequence != nullptr) {
        *out_sequence = sequence;
    }

    return StoreStatus::success();
}

StoreStatus LmdbStore::read_next_sequence(MDB_txn* read_txn, std::uint64_t& out_sequence) const {
    MDB_val key {
        kKeyNextSequence.size(),
        const_cast<char*>(kKeyNextSequence.data())
    };
    MDB_val value {};
    const int rc = mdb_get(read_txn, meta_dbi_, &key, &value);
    if (rc == MDB_NOTFOUND) {
        return StoreStatus::failure(StoreErrorCode::NotFound, "Next sequence key is not set.");
    }
    if (rc != MDB_SUCCESS) {
        return lmdb_error(rc, "mdb_get(next_sequence)");
    }

    if (value.mv_size != 8U) {
        return StoreStatus::failure(StoreErrorCode::CorruptedPayload, "Invalid next sequence payload size.");
    }
    out_sequence = decode_u64_be(value.mv_data);
    return StoreStatus::success();
}

StoreStatus LmdbStore::write_next_sequence(MDB_txn* write_txn, const std::uint64_t next_sequence) const {
    const auto encoded = encode_u64_be(next_sequence);
    return put_blob(
        meta_dbi_,
        write_txn,
        kKeyNextSequence.data(),
        kKeyNextSequence.size(),
        encoded.data(),
        encoded.size(),
        0U
    );
}

StoreStatus LmdbStore::reserve_next_sequence(MDB_txn* write_txn, std::uint64_t& out_sequence) const {
    std::uint64_t next_sequence = 0;
    auto status = read_next_sequence(write_txn, next_sequence);
    if (!status.ok()) {
        if (status.code != StoreErrorCode::NotFound) {
            return status;
        }
        next_sequence = 1U;
    }

    out_sequence = next_sequence;
    status = write_next_sequence(write_txn, next_sequence + 1U);
    if (!status.ok()) {
        return status;
    }
    return StoreStatus::success();
}

StoreStatus LmdbStore::fetch_transport_batch(const std::size_t max_items,
                                             std::vector<TransportRecord>& out_records) const {
    if (!is_open()) {
        return StoreStatus::failure(StoreErrorCode::NotOpen, "LMDB store is not open.");
    }

    if (max_items == 0) {
        out_records.clear();
        return StoreStatus::success();
    }

    MDB_txn* read_txn = nullptr;
    auto status = begin_txn(&read_txn, MDB_RDONLY);
    if (!status.ok()) {
        return status;
    }

    MDB_cursor* cursor = nullptr;
    const int cursor_rc = mdb_cursor_open(read_txn, transport_dbi_, &cursor);
    if (cursor_rc != MDB_SUCCESS) {
        mdb_txn_abort(read_txn);
        return lmdb_error(cursor_rc, "mdb_cursor_open(transport)");
    }

    out_records.clear();
    out_records.reserve(max_items);

    MDB_val key {};
    MDB_val value {};
    int rc = mdb_cursor_get(cursor, &key, &value, MDB_FIRST);
    while (rc == MDB_SUCCESS && out_records.size() < max_items) {
        if (key.mv_size != 8U) {
            mdb_cursor_close(cursor);
            mdb_txn_abort(read_txn);
            return StoreStatus::failure(StoreErrorCode::CorruptedPayload, "Invalid transport key size.");
        }

        TransportRecord record;
        record.sequence = decode_u64_be(key.mv_data);
        status = parse_frame(value, record);
        if (!status.ok()) {
            mdb_cursor_close(cursor);
            mdb_txn_abort(read_txn);
            return status;
        }

        out_records.push_back(std::move(record));
        rc = mdb_cursor_get(cursor, &key, &value, MDB_NEXT);
    }

    if (rc != MDB_SUCCESS && rc != MDB_NOTFOUND) {
        mdb_cursor_close(cursor);
        mdb_txn_abort(read_txn);
        return lmdb_error(rc, "mdb_cursor_get(fetch_transport_batch)");
    }

    mdb_cursor_close(cursor);
    mdb_txn_abort(read_txn);
    return StoreStatus::success();
}

StoreStatus LmdbStore::ack_transport_until(const std::uint64_t inclusive_sequence) {
    if (!is_open()) {
        return StoreStatus::failure(StoreErrorCode::NotOpen, "LMDB store is not open.");
    }

    std::lock_guard<std::mutex> lock(writer_mutex_);
    MDB_txn* write_txn = nullptr;
    auto status = begin_txn(&write_txn, 0U);
    if (!status.ok()) {
        return status;
    }

    MDB_cursor* cursor = nullptr;
    const int open_cursor_rc = mdb_cursor_open(write_txn, transport_dbi_, &cursor);
    if (open_cursor_rc != MDB_SUCCESS) {
        mdb_txn_abort(write_txn);
        return lmdb_error(open_cursor_rc, "mdb_cursor_open(ack_transport_until)");
    }

    MDB_val key {};
    MDB_val value {};
    int rc = mdb_cursor_get(cursor, &key, &value, MDB_FIRST);
    while (rc == MDB_SUCCESS) {
        if (key.mv_size != 8U) {
            mdb_cursor_close(cursor);
            mdb_txn_abort(write_txn);
            return StoreStatus::failure(StoreErrorCode::CorruptedPayload, "Invalid transport key size.");
        }

        const auto sequence = decode_u64_be(key.mv_data);
        if (sequence > inclusive_sequence) {
            break;
        }

        const int delete_rc = mdb_cursor_del(cursor, 0U);
        if (delete_rc != MDB_SUCCESS) {
            mdb_cursor_close(cursor);
            mdb_txn_abort(write_txn);
            return lmdb_error(delete_rc, "mdb_cursor_del(ack_transport_until)");
        }

        rc = mdb_cursor_get(cursor, &key, &value, MDB_NEXT);
    }

    if (rc != MDB_SUCCESS && rc != MDB_NOTFOUND) {
        mdb_cursor_close(cursor);
        mdb_txn_abort(write_txn);
        return lmdb_error(rc, "mdb_cursor_get(ack_transport_until)");
    }

    mdb_cursor_close(cursor);

    const int commit_rc = mdb_txn_commit(write_txn);
    if (commit_rc != MDB_SUCCESS) {
        return lmdb_error(commit_rc, "mdb_txn_commit(ack_transport_until)");
    }
    return StoreStatus::success();
}

} // namespace host_indexer::storage
