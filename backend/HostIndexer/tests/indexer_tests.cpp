#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include "api/indexing_pipeline.hpp"
#include "domain/migration/migration.hpp"
#include "domain/model/models.hpp"
#include "domain/model/validation_types.hpp"
#include "domain/serialization/serialization.hpp"
#include "domain/validation/validate_snapshot.hpp"
#include "snapshot/delta_engine.hpp"
#include "storage/lmdb_store.hpp"
#include "transport/ws_crud_server.hpp"

#include "../../shared/crud_protocol.hpp"

namespace {

using host_indexer::domain::DeltaSnapshot;
using host_indexer::domain::EntryType;
using host_indexer::domain::FileEntry;
using host_indexer::domain::FileMetadata;
using host_indexer::domain::Snapshot;
using host_indexer::domain::ValidationErrorCode;
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
namespace crud = backend::shared::crud;

void require(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "TEST FAILED: " << message << '\n';
        std::exit(1);
    }
}

std::string make_temp_name(const std::string_view prefix) {
    const auto now_ticks = static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count());
    return std::string(prefix) + "_" + std::to_string(now_ticks) + "_" + std::to_string(std::rand());
}

std::uint16_t reserve_free_port() {
    asio::io_context io_context;
    tcp::acceptor acceptor(io_context);
    boost::system::error_code error;

    acceptor.open(tcp::v4(), error);
    require(!error, "reserve_free_port: acceptor open failed");
    acceptor.bind(tcp::endpoint(tcp::v4(), 0), error);
    require(!error, "reserve_free_port: acceptor bind failed");
    const auto endpoint = acceptor.local_endpoint(error);
    require(!error, "reserve_free_port: local_endpoint failed");
    acceptor.close(error);
    require(!error, "reserve_free_port: acceptor close failed");

    return endpoint.port();
}

class WebsocketCrudTestClient final {
public:
    explicit WebsocketCrudTestClient(const std::uint16_t port)
        : port_(port) {
    }

    bool connect(const std::string& path, boost::system::error_code& error) {
        const auto endpoints = resolver_.resolve("127.0.0.1", std::to_string(port_), error);
        if (error) {
            return false;
        }

        asio::connect(websocket_.next_layer(), endpoints, error);
        if (error) {
            return false;
        }

        websocket_.handshake("127.0.0.1:" + std::to_string(port_), path, error);
        if (!error) {
            connected_ = true;
        }
        return !error;
    }

    void send_json(const nlohmann::json& payload) {
        const auto text = crud::json_to_text(payload);
        boost::system::error_code error;
        websocket_.write(asio::buffer(text), error);
        require(!error, "websocket write failed");
    }

    nlohmann::json read_json() {
        beast::flat_buffer buffer;
        boost::system::error_code error;
        websocket_.read(buffer, error);
        require(!error, "websocket read failed");

        const std::string payload = beast::buffers_to_string(buffer.data());
        const auto parsed = crud::text_to_json(payload);
        require(parsed.has_value(), "websocket response is not valid json");
        return *parsed;
    }

    nlohmann::json request(const nlohmann::json& payload) {
        send_json(payload);
        return read_json();
    }

    void close() {
        if (!connected_) {
            return;
        }
        boost::system::error_code ignored;
        websocket_.close(websocket::close_code::normal, ignored);
        connected_ = false;
    }

private:
    asio::io_context io_context_ {};
    tcp::resolver resolver_ {io_context_};
    websocket::stream<tcp::socket> websocket_ {io_context_};
    std::uint16_t port_ {0U};
    bool connected_ {false};
};

class WebsocketServerHarness final {
public:
    WebsocketServerHarness() = default;
    ~WebsocketServerHarness() {
        stop();
    }

    void start(std::string websocket_path) {
        if (started_) {
            return;
        }

        db_dir_ = std::filesystem::temp_directory_path() / make_temp_name("host_indexer_ws_db");
        root_dir_ = std::filesystem::temp_directory_path() / make_temp_name("host_indexer_ws_root");

        std::error_code fs_error;
        std::filesystem::create_directories(db_dir_, fs_error);
        require(!fs_error, "failed to create websocket test lmdb directory");
        std::filesystem::create_directories(root_dir_, fs_error);
        require(!fs_error, "failed to create websocket test root directory");

        {
            std::ofstream seed(root_dir_ / "seed.txt", std::ios::binary | std::ios::trunc);
            require(static_cast<bool>(seed), "failed to create websocket test seed file");
            seed << "seed-data";
        }

        host_indexer::storage::LmdbStoreOptions options;
        options.directory = db_dir_;
        options.map_size_bytes = 256ULL * 1024ULL * 1024ULL;
        const auto open_status = store_.open(options);
        require(open_status.ok(), "failed to open websocket test lmdb store");

        pipeline_ = std::make_unique<host_indexer::api::IndexingPipeline>(store_);
        port_ = reserve_free_port();

        host_indexer::transport::WebsocketCrudServerOptions server_options;
        server_options.listen_address = "127.0.0.1";
        server_options.port = port_;
        server_options.websocket_path = std::move(websocket_path);
        server_options.default_root_path = root_dir_;
        server_options.default_outbox_batch_size = 32U;
        server_options.allowed_client_instance_id = "test-client";
        server_options.bridge_auth_token = "test-token";

        server_ = std::make_unique<host_indexer::transport::WebsocketCrudServer>(
            io_context_,
            store_,
            *pipeline_,
            std::move(server_options)
        );

        const auto start_status = server_->start();
        require(start_status.ok(), "failed to start websocket test server");

        worker_ = std::thread([this]() {
            io_context_.run();
        });
        started_ = true;
    }

    void stop() {
        if (!started_) {
            return;
        }

        if (server_) {
            server_->stop();
        }
        io_context_.stop();
        if (worker_.joinable()) {
            worker_.join();
        }

        server_.reset();
        pipeline_.reset();
        store_.close();

        std::error_code fs_error;
        if (!db_dir_.empty()) {
            std::filesystem::remove_all(db_dir_, fs_error);
            fs_error.clear();
        }
        if (!root_dir_.empty()) {
            std::filesystem::remove_all(root_dir_, fs_error);
            fs_error.clear();
        }

        started_ = false;
    }

    [[nodiscard]] std::uint16_t port() const noexcept {
        return port_;
    }

    [[nodiscard]] const std::filesystem::path& root_dir() const noexcept {
        return root_dir_;
    }

private:
    boost::asio::io_context io_context_ {};
    host_indexer::storage::LmdbStore store_ {};
    std::unique_ptr<host_indexer::api::IndexingPipeline> pipeline_ {};
    std::unique_ptr<host_indexer::transport::WebsocketCrudServer> server_ {};
    std::thread worker_ {};
    std::filesystem::path db_dir_ {};
    std::filesystem::path root_dir_ {};
    std::uint16_t port_ {0U};
    bool started_ {false};
};

std::uint64_t max_record_sequence(const crud::CrudResultMessage& result) {
    std::uint64_t max_sequence = 0U;
    for (const auto& record : result.records) {
        max_sequence = std::max(max_sequence, record.sequence);
    }
    return max_sequence;
}

FileMetadata make_metadata(const std::uint64_t size,
                           const std::uint32_t permissions,
                           const std::uint64_t file_id,
                           const std::uint64_t device_id,
                           const std::int64_t base_ts) {
    FileMetadata metadata;
    metadata.size_bytes = size;
    metadata.permissions = permissions;
    metadata.file_id = file_id;
    metadata.device_id = device_id;
    metadata.created_at = base_ts;
    metadata.modified_at = base_ts + 1;
    metadata.accessed_at = base_ts + 2;
    return metadata;
}

FileEntry make_entry(const std::uint64_t id,
                     const std::uint64_t parent_id,
                     const std::string& path,
                     const std::string& name,
                     const EntryType type,
                     const FileMetadata& metadata) {
    FileEntry entry;
    entry.id = id;
    entry.parent_id = parent_id;
    entry.normalized_path = path;
    entry.name = name;
    entry.type = type;
    entry.metadata = metadata;
    return entry;
}

Snapshot make_empty_fs_fixture() {
    Snapshot snapshot;
    snapshot.schema = host_indexer::domain::kCurrentSchemaHeader;
    snapshot.snapshot_id = 1001;
    snapshot.created_at = 1'700'000'001;
    snapshot.host_identifier = "host-a";
    snapshot.root_entry_id = 1;

    snapshot.entries.push_back(
        make_entry(
            1,
            0,
            "/data",
            "data",
            EntryType::Directory,
            make_metadata(0, 0755, 10, 1, 1'700'000'001)
        )
    );
    return snapshot;
}

Snapshot make_deep_tree_fixture(const std::size_t depth) {
    Snapshot snapshot;
    snapshot.schema = host_indexer::domain::kCurrentSchemaHeader;
    snapshot.snapshot_id = 1002;
    snapshot.created_at = 1'700'000'010;
    snapshot.host_identifier = "host-b";
    snapshot.root_entry_id = 1;

    snapshot.entries.push_back(
        make_entry(
            1,
            0,
            "/deep",
            "deep",
            EntryType::Directory,
            make_metadata(0, 0755, 20, 1, 1'700'000'010)
        )
    );

    std::uint64_t parent_id = 1;
    std::string path = "/deep";
    for (std::size_t i = 0; i < depth; ++i) {
        const auto id = static_cast<std::uint64_t>(2 + i);
        path += "/d" + std::to_string(i);
        snapshot.entries.push_back(
            make_entry(
                id,
                parent_id,
                path,
                "d" + std::to_string(i),
                EntryType::Directory,
                make_metadata(0, 0755, 20 + id, 1, 1'700'000'010 + static_cast<std::int64_t>(i))
            )
        );
        parent_id = id;
    }

    return snapshot;
}

Snapshot make_rename_storm_base(const std::size_t count) {
    Snapshot snapshot;
    snapshot.schema = host_indexer::domain::kCurrentSchemaHeader;
    snapshot.snapshot_id = 2001;
    snapshot.created_at = 1'700'000'100;
    snapshot.host_identifier = "host-c";
    snapshot.root_entry_id = 1;

    snapshot.entries.push_back(
        make_entry(
            1,
            0,
            "/storm",
            "storm",
            EntryType::Directory,
            make_metadata(0, 0755, 100, 1, 1'700'000'100)
        )
    );

    for (std::size_t i = 0; i < count; ++i) {
        const auto id = static_cast<std::uint64_t>(2 + i);
        snapshot.entries.push_back(
            make_entry(
                id,
                1,
                "/storm/file_" + std::to_string(i) + ".txt",
                "file_" + std::to_string(i) + ".txt",
                EntryType::File,
                make_metadata(
                    128 + static_cast<std::uint64_t>(i),
                    0644,
                    200 + id,
                    1,
                    1'700'000'100 + static_cast<std::int64_t>(i)
                )
            )
        );
    }

    return snapshot;
}

Snapshot make_rename_storm_target(const Snapshot& base) {
    Snapshot target = base;
    target.snapshot_id = 2002;
    target.created_at = 1'700'000'200;

    for (std::size_t i = 1; i < target.entries.size(); ++i) {
        target.entries[i].normalized_path = "/storm/renamed_" + std::to_string(i) + ".txt";
        target.entries[i].name = "renamed_" + std::to_string(i) + ".txt";
    }

    return target;
}

Snapshot make_permission_edge_fixture() {
    Snapshot snapshot = make_empty_fs_fixture();
    snapshot.snapshot_id = 3001;
    snapshot.entries.push_back(
        make_entry(
            2,
            1,
            "/data/secret.bin",
            "secret.bin",
            EntryType::File,
            make_metadata(17, 0xFFFF0001U, 99, 1, 1'700'000'300)
        )
    );
    return snapshot;
}

bool report_has_code(const host_indexer::domain::ValidationReport& report, const ValidationErrorCode code) {
    for (const auto& issue : report.issues) {
        if (issue.code == code) {
            return true;
        }
    }
    return false;
}

bool snapshot_equals(const Snapshot& lhs, const Snapshot& rhs) {
    if (lhs.schema.major != rhs.schema.major ||
        lhs.schema.minor != rhs.schema.minor ||
        lhs.schema.patch != rhs.schema.patch ||
        lhs.schema.min_reader_major != rhs.schema.min_reader_major ||
        lhs.snapshot_id != rhs.snapshot_id ||
        lhs.created_at != rhs.created_at ||
        lhs.host_identifier != rhs.host_identifier ||
        lhs.root_entry_id != rhs.root_entry_id ||
        lhs.entries.size() != rhs.entries.size()) {
        return false;
    }

    for (std::size_t i = 0; i < lhs.entries.size(); ++i) {
        if (!host_indexer::domain::structural_entry_equals(lhs.entries[i], rhs.entries[i])) {
            return false;
        }
    }
    return true;
}

bool delta_equals(const DeltaSnapshot& lhs, const DeltaSnapshot& rhs) {
    if (lhs.schema.major != rhs.schema.major ||
        lhs.schema.minor != rhs.schema.minor ||
        lhs.schema.patch != rhs.schema.patch ||
        lhs.schema.min_reader_major != rhs.schema.min_reader_major ||
        lhs.base_snapshot_id != rhs.base_snapshot_id ||
        lhs.target_snapshot_id != rhs.target_snapshot_id ||
        lhs.added_entries.size() != rhs.added_entries.size() ||
        lhs.removed_entries.size() != rhs.removed_entries.size() ||
        lhs.modified_entries.size() != rhs.modified_entries.size() ||
        lhs.renamed_entries.size() != rhs.renamed_entries.size()) {
        return false;
    }

    for (std::size_t i = 0; i < lhs.added_entries.size(); ++i) {
        if (!host_indexer::domain::structural_entry_equals(lhs.added_entries[i].entry, rhs.added_entries[i].entry)) {
            return false;
        }
    }
    for (std::size_t i = 0; i < lhs.removed_entries.size(); ++i) {
        if (!host_indexer::domain::structural_entry_equals(lhs.removed_entries[i].entry, rhs.removed_entries[i].entry)) {
            return false;
        }
    }
    for (std::size_t i = 0; i < lhs.modified_entries.size(); ++i) {
        const auto& a = lhs.modified_entries[i];
        const auto& b = rhs.modified_entries[i];
        if (a.entry_id != b.entry_id ||
            !host_indexer::domain::metadata_equals(a.before, b.before) ||
            !host_indexer::domain::metadata_equals(a.after, b.after)) {
            return false;
        }
    }
    for (std::size_t i = 0; i < lhs.renamed_entries.size(); ++i) {
        const auto& a = lhs.renamed_entries[i];
        const auto& b = rhs.renamed_entries[i];
        if (a.before_entry_id != b.before_entry_id ||
            a.after_entry_id != b.after_entry_id ||
            a.old_path != b.old_path ||
            a.new_path != b.new_path) {
            return false;
        }
    }
    return true;
}

void test_empty_fixture_validation() {
    const auto snapshot = make_empty_fs_fixture();
    const auto report = host_indexer::validation::validate_snapshot(snapshot);
    require(!report.has_errors(), "empty fixture should validate");
}

void test_invalid_header_detected() {
    auto snapshot = make_empty_fs_fixture();
    snapshot.snapshot_id = 0;
    snapshot.host_identifier.clear();

    const auto report = host_indexer::validation::validate_snapshot(snapshot);
    require(report_has_code(report, ValidationErrorCode::SnapshotIdZero), "snapshot id zero must be reported");
    require(
        report_has_code(report, ValidationErrorCode::SnapshotHostIdentifierEmpty),
        "empty host identifier must be reported"
    );
}

void test_deep_tree_fixture() {
    const auto snapshot = make_deep_tree_fixture(256);
    const auto report = host_indexer::validation::validate_snapshot(snapshot);
    require(!report.has_errors(), "deep tree fixture should validate");
}

void test_permission_edge_fixture() {
    const auto snapshot = make_permission_edge_fixture();
    const auto report = host_indexer::validation::validate_snapshot(snapshot);
    require(
        report_has_code(report, ValidationErrorCode::EntryPermissionsInvalid),
        "permission edge fixture must trigger permissions warning"
    );
}

void test_delta_rename_storm() {
    const auto base = make_rename_storm_base(100);
    const auto target = make_rename_storm_target(base);

    host_indexer::snapshot::DeltaEngine engine;
    const auto delta = engine.compute(base, target, {});

    require(delta.added_entries.empty(), "rename storm should not create added entries");
    require(delta.removed_entries.empty(), "rename storm should not create removed entries");
    require(delta.modified_entries.empty(), "rename storm should not create modified entries");
    require(delta.renamed_entries.size() == 100, "rename storm should detect all renames");

    const auto report = host_indexer::validation::validate_delta(delta, &base, &target);
    require(!report.has_errors(), "rename storm delta should validate");
}

void test_snapshot_round_trip_binary_json() {
    const auto snapshot = make_rename_storm_base(8);

    std::stringstream buffer(std::ios::in | std::ios::out | std::ios::binary);
    auto result = host_indexer::serialization::serialize_snapshot_binary(snapshot, buffer);
    require(result.ok, "snapshot binary serialization should succeed");

    buffer.seekg(0);
    Snapshot roundtrip_binary;
    result = host_indexer::serialization::deserialize_snapshot_binary(buffer, roundtrip_binary);
    require(result.ok, "snapshot binary deserialization should succeed");
    require(snapshot_equals(snapshot, roundtrip_binary), "snapshot binary round-trip mismatch");

    const auto json = host_indexer::serialization::snapshot_to_json(snapshot);
    Snapshot roundtrip_json;
    result = host_indexer::serialization::snapshot_from_json(json, roundtrip_json);
    require(result.ok, "snapshot json deserialization should succeed");
    require(snapshot_equals(snapshot, roundtrip_json), "snapshot json round-trip mismatch");
}

void test_delta_round_trip_binary_json() {
    const auto base = make_rename_storm_base(12);
    const auto target = make_rename_storm_target(base);
    host_indexer::snapshot::DeltaEngine engine;
    const auto delta = engine.compute(base, target, {});

    std::stringstream buffer(std::ios::in | std::ios::out | std::ios::binary);
    auto result = host_indexer::serialization::serialize_delta_binary(delta, buffer);
    require(result.ok, "delta binary serialization should succeed");

    buffer.seekg(0);
    DeltaSnapshot roundtrip_binary;
    result = host_indexer::serialization::deserialize_delta_binary(buffer, roundtrip_binary);
    require(result.ok, "delta binary deserialization should succeed");
    require(delta_equals(delta, roundtrip_binary), "delta binary round-trip mismatch");

    const auto json = host_indexer::serialization::delta_to_json(delta);
    DeltaSnapshot roundtrip_json;
    result = host_indexer::serialization::delta_from_json(json, roundtrip_json);
    require(result.ok, "delta json deserialization should succeed");
    require(delta_equals(delta, roundtrip_json), "delta json round-trip mismatch");
}

void test_migration_contract() {
    const auto source = make_empty_fs_fixture();
    auto target_schema = host_indexer::domain::kCurrentSchemaHeader;
    target_schema.minor = 1;

    const auto migrated = host_indexer::migration::migrate_snapshot(source, target_schema);
    require(migrated.status == host_indexer::migration::MigrationStatus::Success, "migration should succeed");
    require(migrated.snapshot.schema.minor == 1, "migration should update schema");
    require(migrated.reversible, "same-major migration should be reversible");
}

void test_lmdb_store_round_trip() {
    const auto snapshot = make_rename_storm_base(6);
    const auto target = make_rename_storm_target(snapshot);
    host_indexer::snapshot::DeltaEngine engine;
    const auto delta = engine.compute(snapshot, target, {});

    const auto now_ticks = std::to_string(static_cast<long long>(std::rand()));
    const auto temp_dir = std::filesystem::temp_directory_path() / ("host_indexer_lmdb_test_" + now_ticks);
    std::filesystem::create_directories(temp_dir);

    host_indexer::storage::LmdbStore store;
    host_indexer::storage::LmdbStoreOptions options;
    options.directory = temp_dir;
    options.map_size_bytes = 256ULL * 1024ULL * 1024ULL;

    auto status = store.open(options);
    require(status.ok(), "lmdb open should succeed");

    status = store.put_snapshot(snapshot, true);
    require(status.ok(), "lmdb put snapshot should succeed");

    host_indexer::domain::Snapshot loaded_snapshot;
    status = store.get_snapshot(snapshot.snapshot_id, loaded_snapshot, true);
    require(status.ok(), "lmdb get snapshot should succeed");
    require(snapshot_equals(snapshot, loaded_snapshot), "lmdb snapshot round-trip mismatch");

    status = store.put_delta(delta, &snapshot, &target, true);
    require(status.ok(), "lmdb put delta should succeed");

    host_indexer::domain::DeltaSnapshot loaded_delta;
    status = store.get_delta(delta.base_snapshot_id, delta.target_snapshot_id, loaded_delta, true);
    require(status.ok(), "lmdb get delta should succeed");
    require(delta_equals(delta, loaded_delta), "lmdb delta round-trip mismatch");

    std::uint64_t seq1 = 0;
    std::uint64_t seq2 = 0;
    status = store.enqueue_snapshot(snapshot, true, &seq1);
    require(status.ok(), "enqueue snapshot should succeed");
    status = store.enqueue_delta(delta, &snapshot, &target, true, &seq2);
    require(status.ok(), "enqueue delta should succeed");
    require(seq2 > seq1, "transport sequence must increase");

    std::vector<host_indexer::storage::TransportRecord> records;
    status = store.fetch_transport_batch(16, records);
    require(status.ok(), "fetch transport batch should succeed");
    require(records.size() == 2, "transport batch should contain two records");

    status = store.ack_transport_until(seq2);
    require(status.ok(), "ack transport should succeed");
    records.clear();
    status = store.fetch_transport_batch(16, records);
    require(status.ok(), "fetch transport after ack should succeed");
    require(records.empty(), "transport outbox should be empty after ack");

    store.close();
    std::filesystem::remove_all(temp_dir);
}

void test_websocket_path_enforced() {
    WebsocketServerHarness harness;
    harness.start("/hostindexer-expected");

    WebsocketCrudTestClient client(harness.port());
    boost::system::error_code error;
    const bool connected = client.connect("/hostindexer-wrong", error);
    require(!connected, "websocket handshake should fail on unexpected path");
}

void test_websocket_crud_flow() {
    WebsocketServerHarness harness;
    harness.start("/hostindexer");

    WebsocketCrudTestClient client(harness.port());
    boost::system::error_code error;
    const bool connected = client.connect("/hostindexer", error);
    require(connected, "websocket handshake should succeed on configured path");

    crud::CrudCommandMessage before_hello;
    before_hello.request_id = "before-hello";
    before_hello.operation = crud::Operation::Read;
    const auto before_hello_json = client.request(crud::to_json(before_hello));
    const auto before_hello_result = crud::parse_crud_result(before_hello_json);
    require(before_hello_result.has_value(), "crud_result should be returned for pre-hello command");
    require(!before_hello_result->ok, "pre-hello crud_command must be rejected");

    crud::HelloMessage hello;
    hello.host_id = "test-host";
    hello.client_instance_id = "test-client";
    hello.auth_token = "test-token";
    const auto hello_ack_json = client.request(crud::to_json(hello));
    const auto hello_ack = crud::parse_hello_ack(hello_ack_json);
    require(hello_ack.has_value(), "hello_ack message must parse");
    require(hello_ack->accepted, "hello should be accepted for valid credentials");

    crud::CrudCommandMessage create;
    create.request_id = "create-1";
    create.operation = crud::Operation::Create;
    create.root_path = harness.root_dir().string();
    create.outbox_batch_size = 64U;
    const auto create_json = client.request(crud::to_json(create));
    const auto create_result = crud::parse_crud_result(create_json);
    require(create_result.has_value(), "create result should parse");
    require(create_result->ok, "create command should succeed");
    require(create_result->snapshot_id != 0U, "create result should return snapshot id");
    require(!create_result->records.empty(), "create result should include transport records");

    {
        std::ofstream mutate(harness.root_dir() / "seed.txt", std::ios::binary | std::ios::app);
        require(static_cast<bool>(mutate), "failed to mutate file before update");
        mutate << "changed";
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));

    crud::CrudCommandMessage update;
    update.request_id = "update-1";
    update.operation = crud::Operation::Update;
    update.root_path = harness.root_dir().string();
    update.base_snapshot_id = create_result->snapshot_id;
    update.outbox_batch_size = 64U;
    const auto update_json = client.request(crud::to_json(update));
    const auto update_result = crud::parse_crud_result(update_json);
    require(update_result.has_value(), "update result should parse");
    require(update_result->ok, "update command should succeed");
    require(update_result->target_snapshot_id != 0U, "update should return target snapshot id");
    require(!update_result->records.empty(), "update result should include transport records");

    const std::uint64_t ack_sequence = std::max(
        max_record_sequence(*create_result),
        max_record_sequence(*update_result)
    );
    require(ack_sequence != 0U, "ack sequence must be non-zero");

    crud::IngestAckMessage ingest_ack;
    ingest_ack.request_id = "ack-1";
    ingest_ack.ok = true;
    ingest_ack.ack_sequence = ack_sequence;
    ingest_ack.message = "ok";
    client.send_json(crud::to_json(ingest_ack));

    crud::CrudCommandMessage read_after_ack;
    read_after_ack.request_id = "read-after-ack";
    read_after_ack.operation = crud::Operation::Read;
    read_after_ack.outbox_batch_size = 64U;
    const auto read_after_ack_json = client.request(crud::to_json(read_after_ack));
    const auto read_after_ack_result = crud::parse_crud_result(read_after_ack_json);
    require(read_after_ack_result.has_value(), "read-after-ack result should parse");
    require(read_after_ack_result->ok, "read-after-ack should succeed");
    require(read_after_ack_result->records.empty(), "outbox should be empty after ingest ack");

    crud::CrudCommandMessage del;
    del.request_id = "delete-1";
    del.operation = crud::Operation::Delete;
    const auto delete_json = client.request(crud::to_json(del));
    const auto delete_result = crud::parse_crud_result(delete_json);
    require(delete_result.has_value(), "delete result should parse");
    require(delete_result->ok, "delete command should succeed");
    require(delete_result->message == "acknowledged", "delete command should acknowledge request");

    client.close();
}

} // namespace

int main() {
    test_empty_fixture_validation();
    test_invalid_header_detected();
    test_deep_tree_fixture();
    test_permission_edge_fixture();
    test_delta_rename_storm();
    test_snapshot_round_trip_binary_json();
    test_delta_round_trip_binary_json();
    test_migration_contract();
    test_lmdb_store_round_trip();
    test_websocket_path_enforced();
    test_websocket_crud_flow();

    std::cout << "All HostIndexer tests passed.\n";
    return 0;
}
