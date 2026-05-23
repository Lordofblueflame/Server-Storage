#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
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

using muninn::domain::DeltaSnapshot;
using muninn::domain::EntryType;
using muninn::domain::FileEntry;
using muninn::domain::FileMetadata;
using muninn::domain::Snapshot;
using muninn::domain::ValidationErrorCode;
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
namespace crud = backend::shared::crud;

void require(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "TEST FAILED: " << message << '\n';
        std::exit(1);
    }
}

void log_test(const std::string_view category, const std::string_view message) {
    std::cout << "[muninn.tests][" << category << "] " << message << '\n';
}

std::optional<Snapshot> decode_snapshot_record(const crud::TransportRecordMessage& record) {
    if (record.record_type != static_cast<std::uint8_t>(muninn::storage::TransportRecordType::Snapshot)) {
        return std::nullopt;
    }

    const auto payload = crud::base64_decode(record.payload_b64);
    if (!payload.has_value()) {
        return std::nullopt;
    }

    const std::string binary(payload->begin(), payload->end());
    std::istringstream stream(binary, std::ios::binary);
    Snapshot snapshot;
    const auto status = muninn::serialization::deserialize_snapshot_binary(stream, snapshot);
    if (!status.ok) {
        return std::nullopt;
    }
    return snapshot;
}

std::optional<DeltaSnapshot> decode_delta_record(const crud::TransportRecordMessage& record) {
    if (record.record_type != static_cast<std::uint8_t>(muninn::storage::TransportRecordType::Delta)) {
        return std::nullopt;
    }

    const auto payload = crud::base64_decode(record.payload_b64);
    if (!payload.has_value()) {
        return std::nullopt;
    }

    const std::string binary(payload->begin(), payload->end());
    std::istringstream stream(binary, std::ios::binary);
    DeltaSnapshot delta;
    const auto status = muninn::serialization::deserialize_delta_binary(stream, delta);
    if (!status.ok) {
        return std::nullopt;
    }
    return delta;
}

std::size_t count_records_of_type(const crud::CrudResultMessage& result, const std::uint8_t record_type) {
    std::size_t count = 0U;
    for (const auto& record : result.records) {
        if (record.record_type == record_type) {
            ++count;
        }
    }
    return count;
}

const crud::TransportRecordMessage* find_first_record_of_type(const crud::CrudResultMessage& result,
                                                              const std::uint8_t record_type) {
    for (const auto& record : result.records) {
        if (record.record_type == record_type) {
            return &record;
        }
    }
    return nullptr;
}

bool snapshot_entry_graph_equals(const Snapshot& lhs, const Snapshot& rhs) {
    if (lhs.root_entry_id != rhs.root_entry_id || lhs.entries.size() != rhs.entries.size()) {
        return false;
    }

    for (std::size_t i = 0; i < lhs.entries.size(); ++i) {
        if (!muninn::domain::structural_entry_equals(lhs.entries[i], rhs.entries[i])) {
            return false;
        }
    }
    return true;
}

const FileEntry* find_entry_by_path(const Snapshot& snapshot, const std::string_view normalized_path) {
    for (const auto& entry : snapshot.entries) {
        if (entry.normalized_path == normalized_path) {
            return &entry;
        }
    }
    return nullptr;
}

Snapshot apply_delta_for_test(const Snapshot& base, const DeltaSnapshot& delta) {
    Snapshot applied = base;
    applied.snapshot_id = delta.target_snapshot_id;

    for (const auto& removed : delta.removed_entries) {
        const auto remove_it = std::remove_if(
            applied.entries.begin(),
            applied.entries.end(),
            [&](const FileEntry& entry) {
                return entry.id == removed.entry.id;
            }
        );
        applied.entries.erase(remove_it, applied.entries.end());
    }

    for (const auto& modified : delta.modified_entries) {
        auto entry_it = std::find_if(
            applied.entries.begin(),
            applied.entries.end(),
            [&](const FileEntry& entry) {
                return entry.id == modified.entry_id;
            }
        );
        if (entry_it != applied.entries.end()) {
            entry_it->metadata = modified.after;
        }
    }

    for (const auto& renamed : delta.renamed_entries) {
        auto entry_it = std::find_if(
            applied.entries.begin(),
            applied.entries.end(),
            [&](const FileEntry& entry) {
                return entry.id == renamed.before_entry_id;
            }
        );
        if (entry_it != applied.entries.end()) {
            if (applied.root_entry_id == renamed.before_entry_id) {
                applied.root_entry_id = renamed.after_entry_id;
            }
            entry_it->id = renamed.after_entry_id;
            entry_it->normalized_path = renamed.new_path;
            entry_it->name = std::filesystem::path(renamed.new_path).filename().string();
        }
    }

    for (const auto& added : delta.added_entries) {
        applied.entries.push_back(added.entry);
    }

    std::sort(
        applied.entries.begin(),
        applied.entries.end(),
        [](const FileEntry& lhs, const FileEntry& rhs) {
            if (lhs.normalized_path == rhs.normalized_path) {
                return lhs.id < rhs.id;
            }
            return lhs.normalized_path < rhs.normalized_path;
        }
    );

    return applied;
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

struct HttpGetResult {
    unsigned int status_code {0U};
    std::string body {};
    std::string content_type {};
};

HttpGetResult http_get(const std::uint16_t port, const std::string& target) {
    asio::io_context io_context;
    tcp::resolver resolver(io_context);
    tcp::socket socket(io_context);
    beast::flat_buffer read_buffer;
    boost::system::error_code error;

    const auto endpoints = resolver.resolve("127.0.0.1", std::to_string(port), error);
    require(!error, "http_get: resolve failed");
    asio::connect(socket, endpoints, error);
    require(!error, "http_get: connect failed");

    http::request<http::empty_body> request(http::verb::get, target, 11);
    request.set(http::field::host, "127.0.0.1");
    request.set(http::field::user_agent, "muninn-test");
    http::write(socket, request, error);
    require(!error, "http_get: write failed");

    http::response<http::string_body> response;
    http::read(socket, read_buffer, response, error);
    require(!error, "http_get: read failed");

    socket.shutdown(tcp::socket::shutdown_both, error);
    error.clear();
    socket.close(error);

    HttpGetResult result;
    result.status_code = static_cast<unsigned int>(response.result_int());
    result.body = std::move(response.body());
    if (const auto content_type = response.find(http::field::content_type); content_type != response.end()) {
        result.content_type = std::string(content_type->value());
    }
    return result;
}

class WebsocketServerHarness final {
public:
    WebsocketServerHarness() = default;
    ~WebsocketServerHarness() {
        stop();
    }

    void start(std::string websocket_path,
               const muninn::transport::TransportSecurityMode security_mode =
                   muninn::transport::TransportSecurityMode::Dev,
               std::string bridge_auth_token = "test-token",
               const bool allow_plain_websocket_in_prod = true) {
        if (started_) {
            return;
        }

        db_dir_ = std::filesystem::temp_directory_path() / make_temp_name("muninn_ws_db");
        root_dir_ = std::filesystem::temp_directory_path() / make_temp_name("muninn_ws_root");

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

        muninn::storage::LmdbStoreOptions options;
        options.directory = db_dir_;
        options.map_size_bytes = 256ULL * 1024ULL * 1024ULL;
        const auto open_status = store_.open(options);
        require(open_status.ok(), "failed to open websocket test lmdb store");

        pipeline_ = std::make_unique<muninn::api::IndexingPipeline>(store_);
        port_ = reserve_free_port();

        muninn::transport::WebsocketCrudServerOptions server_options;
        server_options.listen_address = "127.0.0.1";
        server_options.port = port_;
        server_options.websocket_path = std::move(websocket_path);
        server_options.default_root_path = root_dir_;
        server_options.default_outbox_batch_size = 32U;
        server_options.security_mode = security_mode;
        server_options.allow_plain_websocket_in_prod = allow_plain_websocket_in_prod;
        server_options.allowed_client_instance_id = "test-client";
        server_options.bridge_auth_token = std::move(bridge_auth_token);

        server_ = std::make_unique<muninn::transport::WebsocketCrudServer>(
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

    [[nodiscard]] const muninn::storage::LmdbStore& store() const noexcept {
        return store_;
    }

private:
    boost::asio::io_context io_context_ {};
    muninn::storage::LmdbStore store_ {};
    std::unique_ptr<muninn::api::IndexingPipeline> pipeline_ {};
    std::unique_ptr<muninn::transport::WebsocketCrudServer> server_ {};
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

crud::HelloAckMessage send_hello_and_parse_ack(WebsocketCrudTestClient& client,
                                               const std::string& host_id,
                                               const std::string& client_instance_id,
                                               const std::string& auth_token) {
    crud::HelloMessage hello;
    hello.host_id = host_id;
    hello.client_instance_id = client_instance_id;
    hello.auth_token = auth_token;

    const auto hello_ack_json = client.request(crud::to_json(hello));
    const auto hello_ack = crud::parse_hello_ack(hello_ack_json);
    require(hello_ack.has_value(), "hello_ack message must parse");
    return *hello_ack;
}

std::string fixture_root_path(const std::string_view name) {
    return (std::filesystem::temp_directory_path() / "muninn_fixture" / std::string(name))
        .lexically_normal()
        .generic_string();
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
    snapshot.schema = muninn::domain::kCurrentSchemaHeader;
    snapshot.snapshot_id = 1001;
    snapshot.created_at = 1'700'000'001;
    snapshot.host_identifier = "host-a";
    snapshot.root_entry_id = 1;
    const auto root_path = fixture_root_path("data");

    snapshot.entries.push_back(
        make_entry(
            1,
            0,
            root_path,
            "data",
            EntryType::Directory,
            make_metadata(0, 0755, 10, 1, 1'700'000'001)
        )
    );
    return snapshot;
}

Snapshot make_deep_tree_fixture(const std::size_t depth) {
    Snapshot snapshot;
    snapshot.schema = muninn::domain::kCurrentSchemaHeader;
    snapshot.snapshot_id = 1002;
    snapshot.created_at = 1'700'000'010;
    snapshot.host_identifier = "host-b";
    snapshot.root_entry_id = 1;
    const auto root_path = fixture_root_path("deep");

    snapshot.entries.push_back(
        make_entry(
            1,
            0,
            root_path,
            "deep",
            EntryType::Directory,
            make_metadata(0, 0755, 20, 1, 1'700'000'010)
        )
    );

    std::uint64_t parent_id = 1;
    std::filesystem::path path(root_path);
    for (std::size_t i = 0; i < depth; ++i) {
        const auto id = static_cast<std::uint64_t>(2 + i);
        path /= "d" + std::to_string(i);
        const auto normalized_path = path.lexically_normal().generic_string();
        snapshot.entries.push_back(
            make_entry(
                id,
                parent_id,
                normalized_path,
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
    snapshot.schema = muninn::domain::kCurrentSchemaHeader;
    snapshot.snapshot_id = 2001;
    snapshot.created_at = 1'700'000'100;
    snapshot.host_identifier = "host-c";
    snapshot.root_entry_id = 1;
    const auto root_path = fixture_root_path("storm");

    snapshot.entries.push_back(
        make_entry(
            1,
            0,
            root_path,
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
                (std::filesystem::path(root_path) / ("file_" + std::to_string(i) + ".txt")).generic_string(),
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
    const std::filesystem::path root_path(target.entries.front().normalized_path);

    for (std::size_t i = 1; i < target.entries.size(); ++i) {
        target.entries[i].normalized_path = (root_path / ("renamed_" + std::to_string(i) + ".txt")).generic_string();
        target.entries[i].name = "renamed_" + std::to_string(i) + ".txt";
    }

    return target;
}

Snapshot make_permission_edge_fixture() {
    Snapshot snapshot = make_empty_fs_fixture();
    snapshot.snapshot_id = 3001;
    const auto root_path = snapshot.entries.front().normalized_path;
    snapshot.entries.push_back(
        make_entry(
            2,
            1,
            (std::filesystem::path(root_path) / "secret.bin").generic_string(),
            "secret.bin",
            EntryType::File,
            make_metadata(17, 0xFFFF0001U, 99, 1, 1'700'000'300)
        )
    );
    return snapshot;
}

bool report_has_code(const muninn::domain::ValidationReport& report, const ValidationErrorCode code) {
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
        if (!muninn::domain::structural_entry_equals(lhs.entries[i], rhs.entries[i])) {
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
        if (!muninn::domain::structural_entry_equals(lhs.added_entries[i].entry, rhs.added_entries[i].entry)) {
            return false;
        }
    }
    for (std::size_t i = 0; i < lhs.removed_entries.size(); ++i) {
        if (!muninn::domain::structural_entry_equals(lhs.removed_entries[i].entry, rhs.removed_entries[i].entry)) {
            return false;
        }
    }
    for (std::size_t i = 0; i < lhs.modified_entries.size(); ++i) {
        const auto& a = lhs.modified_entries[i];
        const auto& b = rhs.modified_entries[i];
        if (a.entry_id != b.entry_id ||
            !muninn::domain::metadata_equals(a.before, b.before) ||
            !muninn::domain::metadata_equals(a.after, b.after)) {
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

bool has_scan_entry_path(const muninn::filesystem::RawScanResult& scan, const std::string& normalized_path) {
    for (const auto& entry : scan.entries) {
        if (entry.normalized_path == normalized_path) {
            return true;
        }
    }
    return false;
}

void test_scanner_include_exclude_globs() {
    const auto root_dir = std::filesystem::temp_directory_path() / make_temp_name("muninn_scan_globs");
    std::error_code fs_error;
    std::filesystem::create_directories(root_dir / "nested", fs_error);
    require(!fs_error, "scan-globs test: failed to create nested directory");
    std::filesystem::create_directories(root_dir / "excluded_dir", fs_error);
    require(!fs_error, "scan-globs test: failed to create excluded directory");

    {
        std::ofstream keep_file(root_dir / "keep.txt", std::ios::binary | std::ios::trunc);
        require(static_cast<bool>(keep_file), "scan-globs test: failed to create keep.txt");
        keep_file << "keep";
    }
    {
        std::ofstream skip_file(root_dir / "skip.tmp", std::ios::binary | std::ios::trunc);
        require(static_cast<bool>(skip_file), "scan-globs test: failed to create skip.tmp");
        skip_file << "skip";
    }
    {
        std::ofstream nested_keep(root_dir / "nested" / "keep2.txt", std::ios::binary | std::ios::trunc);
        require(static_cast<bool>(nested_keep), "scan-globs test: failed to create nested keep2.txt");
        nested_keep << "nested-keep";
    }
    {
        std::ofstream nested_skip(root_dir / "nested" / "skip2.log", std::ios::binary | std::ios::trunc);
        require(static_cast<bool>(nested_skip), "scan-globs test: failed to create nested skip2.log");
        nested_skip << "nested-skip";
    }
    {
        std::ofstream excluded_file(root_dir / "excluded_dir" / "secret.txt", std::ios::binary | std::ios::trunc);
        require(static_cast<bool>(excluded_file), "scan-globs test: failed to create excluded secret.txt");
        excluded_file << "secret";
    }

    muninn::filesystem::ScanOptions scan_options;
    scan_options.include_globs = {"*.txt"};
    scan_options.exclude_globs = {"*excluded_dir*"};
    scan_options.max_entries = 1024U;
    scan_options.follow_symlinks = false;

    muninn::filesystem::LocalFilesystemScanner scanner;
    const auto scan = scanner.scan(root_dir, scan_options);

    const auto root_normalized = std::filesystem::absolute(root_dir).lexically_normal().generic_string();
    const auto keep_path = (std::filesystem::absolute(root_dir) / "keep.txt").lexically_normal().generic_string();
    const auto skip_tmp_path = (std::filesystem::absolute(root_dir) / "skip.tmp").lexically_normal().generic_string();
    const auto nested_dir_path = (std::filesystem::absolute(root_dir) / "nested").lexically_normal().generic_string();
    const auto nested_keep_path = (std::filesystem::absolute(root_dir) / "nested" / "keep2.txt")
        .lexically_normal()
        .generic_string();
    const auto nested_skip_path = (std::filesystem::absolute(root_dir) / "nested" / "skip2.log")
        .lexically_normal()
        .generic_string();
    const auto excluded_dir_path = (std::filesystem::absolute(root_dir) / "excluded_dir")
        .lexically_normal()
        .generic_string();
    const auto excluded_file_path = (std::filesystem::absolute(root_dir) / "excluded_dir" / "secret.txt")
        .lexically_normal()
        .generic_string();

    require(has_scan_entry_path(scan, root_normalized), "scan-globs test: root entry should be present");
    require(has_scan_entry_path(scan, keep_path), "scan-globs test: keep.txt should be present");
    require(has_scan_entry_path(scan, nested_dir_path), "scan-globs test: nested directory should be present");
    require(has_scan_entry_path(scan, nested_keep_path), "scan-globs test: nested keep2.txt should be present");
    require(!has_scan_entry_path(scan, skip_tmp_path), "scan-globs test: skip.tmp should be filtered by include");
    require(!has_scan_entry_path(scan, nested_skip_path), "scan-globs test: nested skip2.log should be filtered by include");
    require(!has_scan_entry_path(scan, excluded_dir_path), "scan-globs test: excluded_dir should be excluded");
    require(!has_scan_entry_path(scan, excluded_file_path), "scan-globs test: excluded secret.txt should be excluded");

    std::filesystem::remove_all(root_dir, fs_error);
}

void test_empty_fixture_validation() {
    const auto snapshot = make_empty_fs_fixture();
    const auto report = muninn::validation::validate_snapshot(snapshot);
    require(!report.has_errors(), "empty fixture should validate");
}

void test_invalid_header_detected() {
    auto snapshot = make_empty_fs_fixture();
    snapshot.snapshot_id = 0;
    snapshot.host_identifier.clear();

    const auto report = muninn::validation::validate_snapshot(snapshot);
    require(report_has_code(report, ValidationErrorCode::SnapshotIdZero), "snapshot id zero must be reported");
    require(
        report_has_code(report, ValidationErrorCode::SnapshotHostIdentifierEmpty),
        "empty host identifier must be reported"
    );
}

void test_deep_tree_fixture() {
    const auto snapshot = make_deep_tree_fixture(256);
    const auto report = muninn::validation::validate_snapshot(snapshot);
    require(!report.has_errors(), "deep tree fixture should validate");
}

void test_permission_edge_fixture() {
    const auto snapshot = make_permission_edge_fixture();
    const auto report = muninn::validation::validate_snapshot(snapshot);
    require(
        report_has_code(report, ValidationErrorCode::EntryPermissionsInvalid),
        "permission edge fixture must trigger permissions warning"
    );
}

void test_delta_rename_storm() {
    const auto base = make_rename_storm_base(100);
    const auto target = make_rename_storm_target(base);

    muninn::snapshot::DeltaEngine engine;
    const auto delta = engine.compute(base, target, {});

    require(delta.added_entries.empty(), "rename storm should not create added entries");
    require(delta.removed_entries.empty(), "rename storm should not create removed entries");
    require(delta.modified_entries.empty(), "rename storm should not create modified entries");
    require(delta.renamed_entries.size() == 100, "rename storm should detect all renames");

    const auto report = muninn::validation::validate_delta(delta, &base, &target);
    require(!report.has_errors(), "rename storm delta should validate");
}

void test_snapshot_round_trip_binary_json() {
    const auto snapshot = make_rename_storm_base(8);

    std::stringstream buffer(std::ios::in | std::ios::out | std::ios::binary);
    auto result = muninn::serialization::serialize_snapshot_binary(snapshot, buffer);
    require(result.ok, "snapshot binary serialization should succeed");

    buffer.seekg(0);
    Snapshot roundtrip_binary;
    result = muninn::serialization::deserialize_snapshot_binary(buffer, roundtrip_binary);
    require(result.ok, "snapshot binary deserialization should succeed");
    require(snapshot_equals(snapshot, roundtrip_binary), "snapshot binary round-trip mismatch");

    const auto json = muninn::serialization::snapshot_to_json(snapshot);
    Snapshot roundtrip_json;
    result = muninn::serialization::snapshot_from_json(json, roundtrip_json);
    require(result.ok, "snapshot json deserialization should succeed");
    require(snapshot_equals(snapshot, roundtrip_json), "snapshot json round-trip mismatch");
}

void test_delta_round_trip_binary_json() {
    const auto base = make_rename_storm_base(12);
    const auto target = make_rename_storm_target(base);
    muninn::snapshot::DeltaEngine engine;
    const auto delta = engine.compute(base, target, {});

    std::stringstream buffer(std::ios::in | std::ios::out | std::ios::binary);
    auto result = muninn::serialization::serialize_delta_binary(delta, buffer);
    require(result.ok, "delta binary serialization should succeed");

    buffer.seekg(0);
    DeltaSnapshot roundtrip_binary;
    result = muninn::serialization::deserialize_delta_binary(buffer, roundtrip_binary);
    require(result.ok, "delta binary deserialization should succeed");
    require(delta_equals(delta, roundtrip_binary), "delta binary round-trip mismatch");

    const auto json = muninn::serialization::delta_to_json(delta);
    DeltaSnapshot roundtrip_json;
    result = muninn::serialization::delta_from_json(json, roundtrip_json);
    require(result.ok, "delta json deserialization should succeed");
    require(delta_equals(delta, roundtrip_json), "delta json round-trip mismatch");
}

void test_migration_contract() {
    const auto source = make_empty_fs_fixture();
    auto target_schema = muninn::domain::kCurrentSchemaHeader;
    target_schema.minor = 1;

    const auto migrated = muninn::migration::migrate_snapshot(source, target_schema);
    require(migrated.status == muninn::migration::MigrationStatus::Success, "migration should succeed");
    require(migrated.snapshot.schema.minor == 1, "migration should update schema");
    require(migrated.reversible, "same-major migration should be reversible");
}

void test_lmdb_store_round_trip() {
    const auto snapshot = make_rename_storm_base(6);
    const auto target = make_rename_storm_target(snapshot);
    muninn::snapshot::DeltaEngine engine;
    const auto delta = engine.compute(snapshot, target, {});

    const auto now_ticks = std::to_string(static_cast<long long>(std::rand()));
    const auto temp_dir = std::filesystem::temp_directory_path() / ("muninn_lmdb_test_" + now_ticks);
    std::filesystem::create_directories(temp_dir);

    muninn::storage::LmdbStore store;
    muninn::storage::LmdbStoreOptions options;
    options.directory = temp_dir;
    options.map_size_bytes = 256ULL * 1024ULL * 1024ULL;

    auto status = store.open(options);
    require(status.ok(), "lmdb open should succeed");

    status = store.put_snapshot(snapshot, true);
    require(status.ok(), "lmdb put snapshot should succeed");

    muninn::domain::Snapshot loaded_snapshot;
    status = store.get_snapshot(snapshot.snapshot_id, loaded_snapshot, true);
    require(status.ok(), "lmdb get snapshot should succeed");
    require(snapshot_equals(snapshot, loaded_snapshot), "lmdb snapshot round-trip mismatch");

    status = store.put_delta(delta, &snapshot, &target, true);
    require(status.ok(), "lmdb put delta should succeed");

    muninn::domain::DeltaSnapshot loaded_delta;
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

    std::vector<muninn::storage::TransportRecord> records;
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

void test_snapshot_retention_prunes_old_snapshots_and_deltas() {
    const auto temp_dir = std::filesystem::temp_directory_path() / make_temp_name("muninn_retention_test");
    std::filesystem::create_directories(temp_dir);

    auto base_snapshot = make_rename_storm_base(4);
    base_snapshot.snapshot_id = 5101;
    base_snapshot.created_at = 1'700'000'100;

    auto middle_snapshot = make_rename_storm_target(base_snapshot);
    middle_snapshot.snapshot_id = 5102;
    middle_snapshot.created_at = 1'700'000'200;

    auto latest_snapshot = middle_snapshot;
    latest_snapshot.snapshot_id = 5103;
    latest_snapshot.created_at = 1'700'000'300;
    if (latest_snapshot.entries.size() > 1) {
        latest_snapshot.entries[1].metadata.size_bytes += 7U;
    }

    muninn::snapshot::DeltaEngine delta_engine;
    const auto delta_base_to_middle = delta_engine.compute(base_snapshot, middle_snapshot, {});
    const auto delta_middle_to_latest = delta_engine.compute(middle_snapshot, latest_snapshot, {});

    muninn::storage::LmdbStore store;
    muninn::storage::LmdbStoreOptions options;
    options.directory = temp_dir;
    options.map_size_bytes = 256ULL * 1024ULL * 1024ULL;

    auto status = store.open(options);
    require(status.ok(), "retention test: lmdb open should succeed");

    status = store.put_snapshot(base_snapshot, true);
    require(status.ok(), "retention test: put base snapshot should succeed");
    status = store.put_snapshot(middle_snapshot, true);
    require(status.ok(), "retention test: put middle snapshot should succeed");
    status = store.put_snapshot(latest_snapshot, true);
    require(status.ok(), "retention test: put latest snapshot should succeed");

    status = store.put_delta(delta_base_to_middle, &base_snapshot, &middle_snapshot, true);
    require(status.ok(), "retention test: put base->middle delta should succeed");
    status = store.put_delta(delta_middle_to_latest, &middle_snapshot, &latest_snapshot, true);
    require(status.ok(), "retention test: put middle->latest delta should succeed");

    muninn::storage::RetentionCleanupStats retention_stats;
    status = store.apply_snapshot_retention(2U, &retention_stats);
    require(status.ok(), "retention test: retention cleanup should succeed");
    require(retention_stats.snapshots_removed == 1U, "retention test: exactly one snapshot should be pruned");
    require(retention_stats.deltas_removed == 1U, "retention test: exactly one delta should be pruned");

    muninn::domain::Snapshot loaded_snapshot;
    status = store.get_snapshot(base_snapshot.snapshot_id, loaded_snapshot, true);
    require(!status.ok(), "retention test: base snapshot should be pruned");
    require(status.code == muninn::storage::StoreErrorCode::NotFound, "retention test: base snapshot should be missing");

    status = store.get_snapshot(middle_snapshot.snapshot_id, loaded_snapshot, true);
    require(status.ok(), "retention test: middle snapshot should remain");
    status = store.get_snapshot(latest_snapshot.snapshot_id, loaded_snapshot, true);
    require(status.ok(), "retention test: latest snapshot should remain");

    muninn::domain::DeltaSnapshot loaded_delta;
    status = store.get_delta(
        delta_base_to_middle.base_snapshot_id,
        delta_base_to_middle.target_snapshot_id,
        loaded_delta,
        true
    );
    require(!status.ok(), "retention test: base->middle delta should be pruned");
    require(status.code == muninn::storage::StoreErrorCode::NotFound, "retention test: pruned delta should be missing");

    status = store.get_delta(
        delta_middle_to_latest.base_snapshot_id,
        delta_middle_to_latest.target_snapshot_id,
        loaded_delta,
        true
    );
    require(status.ok(), "retention test: middle->latest delta should remain");

    store.close();
    std::filesystem::remove_all(temp_dir);
}

void test_delete_snapshot_removes_related_deltas() {
    const auto temp_dir = std::filesystem::temp_directory_path() / make_temp_name("muninn_delete_snapshot");
    std::filesystem::create_directories(temp_dir);

    auto base_snapshot = make_rename_storm_base(3);
    base_snapshot.snapshot_id = 5301;
    base_snapshot.created_at = 1'700'020'100;

    auto middle_snapshot = make_rename_storm_target(base_snapshot);
    middle_snapshot.snapshot_id = 5302;
    middle_snapshot.created_at = 1'700'020'200;

    auto latest_snapshot = middle_snapshot;
    latest_snapshot.snapshot_id = 5303;
    latest_snapshot.created_at = 1'700'020'300;
    if (latest_snapshot.entries.size() > 1U) {
        latest_snapshot.entries[1].metadata.size_bytes += 11U;
        latest_snapshot.entries[1].metadata.modified_at += 11;
        latest_snapshot.entries[1].metadata.accessed_at += 11;
    }

    muninn::snapshot::DeltaEngine delta_engine;
    const auto delta_base_to_middle = delta_engine.compute(base_snapshot, middle_snapshot, {});
    const auto delta_middle_to_latest = delta_engine.compute(middle_snapshot, latest_snapshot, {});

    muninn::storage::LmdbStore store;
    muninn::storage::LmdbStoreOptions options;
    options.directory = temp_dir;
    options.map_size_bytes = 256ULL * 1024ULL * 1024ULL;

    auto status = store.open(options);
    require(status.ok(), "delete-snapshot test: lmdb open should succeed");

    status = store.put_snapshot(base_snapshot, true);
    require(status.ok(), "delete-snapshot test: put base snapshot should succeed");
    status = store.put_snapshot(middle_snapshot, true);
    require(status.ok(), "delete-snapshot test: put middle snapshot should succeed");
    status = store.put_snapshot(latest_snapshot, true);
    require(status.ok(), "delete-snapshot test: put latest snapshot should succeed");

    status = store.put_delta(delta_base_to_middle, &base_snapshot, &middle_snapshot, true);
    require(status.ok(), "delete-snapshot test: put base->middle delta should succeed");
    status = store.put_delta(delta_middle_to_latest, &middle_snapshot, &latest_snapshot, true);
    require(status.ok(), "delete-snapshot test: put middle->latest delta should succeed");

    muninn::storage::SnapshotDeleteStats delete_stats;
    status = store.delete_snapshot(middle_snapshot.snapshot_id, &delete_stats);
    require(status.ok(), "delete-snapshot test: delete_snapshot should succeed");
    require(delete_stats.snapshots_removed == 1U, "delete-snapshot test: exactly one snapshot should be removed");
    require(delete_stats.deltas_removed == 2U, "delete-snapshot test: both related deltas should be removed");

    muninn::domain::Snapshot loaded_snapshot;
    status = store.get_snapshot(middle_snapshot.snapshot_id, loaded_snapshot, true);
    require(!status.ok(), "delete-snapshot test: deleted snapshot should be missing");
    require(
        status.code == muninn::storage::StoreErrorCode::NotFound,
        "delete-snapshot test: deleted snapshot must return not found"
    );

    status = store.get_snapshot(base_snapshot.snapshot_id, loaded_snapshot, true);
    require(status.ok(), "delete-snapshot test: base snapshot should remain");
    status = store.get_snapshot(latest_snapshot.snapshot_id, loaded_snapshot, true);
    require(status.ok(), "delete-snapshot test: latest snapshot should remain");

    muninn::domain::DeltaSnapshot loaded_delta;
    status = store.get_delta(delta_base_to_middle.base_snapshot_id, delta_base_to_middle.target_snapshot_id, loaded_delta, true);
    require(!status.ok(), "delete-snapshot test: base->middle delta should be removed");
    require(
        status.code == muninn::storage::StoreErrorCode::NotFound,
        "delete-snapshot test: deleted base->middle delta must return not found"
    );

    status = store.get_delta(
        delta_middle_to_latest.base_snapshot_id,
        delta_middle_to_latest.target_snapshot_id,
        loaded_delta,
        true
    );
    require(!status.ok(), "delete-snapshot test: middle->latest delta should be removed");
    require(
        status.code == muninn::storage::StoreErrorCode::NotFound,
        "delete-snapshot test: deleted middle->latest delta must return not found"
    );

    store.close();
    std::filesystem::remove_all(temp_dir);
}

void test_store_stats_counts_records() {
    const auto temp_dir = std::filesystem::temp_directory_path() / make_temp_name("muninn_store_stats");
    std::filesystem::create_directories(temp_dir);

    auto snapshot_a = make_rename_storm_base(3);
    snapshot_a.snapshot_id = 5201;
    snapshot_a.created_at = 1'700'010'100;
    auto snapshot_b = make_rename_storm_target(snapshot_a);
    snapshot_b.snapshot_id = 5202;
    snapshot_b.created_at = 1'700'010'200;

    muninn::snapshot::DeltaEngine delta_engine;
    const auto delta = delta_engine.compute(snapshot_a, snapshot_b, {});

    muninn::storage::LmdbStore store;
    muninn::storage::LmdbStoreOptions options;
    options.directory = temp_dir;
    options.map_size_bytes = 256ULL * 1024ULL * 1024ULL;

    auto status = store.open(options);
    require(status.ok(), "store-stats test: lmdb open should succeed");

    muninn::storage::StoreStats stats;
    status = store.get_stats(stats);
    require(status.ok(), "store-stats test: get_stats should succeed for empty store");
    require(stats.snapshots_count == 0U, "store-stats test: empty snapshots count should be zero");
    require(stats.deltas_count == 0U, "store-stats test: empty deltas count should be zero");
    require(stats.transport_outbox_count == 0U, "store-stats test: empty outbox count should be zero");
    require(stats.transport_consumers_count == 0U, "store-stats test: empty consumer count should be zero");
    require(stats.next_transport_sequence >= 1U, "store-stats test: next sequence should be initialized");

    status = store.put_snapshot(snapshot_a, true);
    require(status.ok(), "store-stats test: put snapshot A should succeed");
    status = store.put_snapshot(snapshot_b, true);
    require(status.ok(), "store-stats test: put snapshot B should succeed");
    status = store.put_delta(delta, &snapshot_a, &snapshot_b, true);
    require(status.ok(), "store-stats test: put delta should succeed");
    status = store.enqueue_snapshot(snapshot_b, true, nullptr);
    require(status.ok(), "store-stats test: enqueue snapshot should succeed");
    status = store.register_transport_consumer("stats-consumer");
    require(status.ok(), "store-stats test: register consumer should succeed");

    status = store.get_stats(stats);
    require(status.ok(), "store-stats test: get_stats should succeed after writes");
    require(stats.snapshots_count == 2U, "store-stats test: snapshots count should be two");
    require(stats.deltas_count == 1U, "store-stats test: deltas count should be one");
    require(stats.transport_outbox_count == 1U, "store-stats test: outbox count should be one");
    require(stats.transport_consumers_count == 1U, "store-stats test: consumer count should be one");
    require(stats.next_transport_sequence >= 2U, "store-stats test: next sequence should advance");

    store.close();
    std::filesystem::remove_all(temp_dir);
}

void test_update_with_missing_base_has_no_side_effects() {
    const auto test_name = make_temp_name("muninn_missing_base");
    const auto root_dir = std::filesystem::temp_directory_path() / (test_name + "_root");
    const auto db_dir = std::filesystem::temp_directory_path() / (test_name + "_db");

    std::error_code fs_error;
    std::filesystem::create_directories(root_dir, fs_error);
    require(!fs_error, "failed to create missing-base root dir");
    std::filesystem::create_directories(db_dir, fs_error);
    require(!fs_error, "failed to create missing-base db dir");

    {
        std::ofstream file(root_dir / "seed.txt", std::ios::binary | std::ios::trunc);
        require(static_cast<bool>(file), "failed to create missing-base seed file");
        file << "seed";
    }

    muninn::storage::LmdbStore store;
    muninn::storage::LmdbStoreOptions options;
    options.directory = db_dir;
    options.map_size_bytes = 256ULL * 1024ULL * 1024ULL;

    auto status = store.open(options);
    require(status.ok(), "missing-base test: lmdb open should succeed");

    muninn::api::IndexingPipeline pipeline(store);
    const auto result = pipeline.capture_snapshot_and_delta(root_dir, 999999999ULL, {}, true);
    require(!result.ok(), "missing-base update must fail");
    require(
        result.delta_store_status.code == muninn::storage::StoreErrorCode::NotFound,
        "missing-base update must report base snapshot not found"
    );

    require(
        result.snapshot_build.snapshot.snapshot_id != 0U,
        "missing-base update should still build deterministic target snapshot id"
    );

    muninn::domain::Snapshot persisted_snapshot;
    status = store.get_snapshot(result.snapshot_build.snapshot.snapshot_id, persisted_snapshot, true);
    require(!status.ok(), "missing-base update must not persist target snapshot");
    require(
        status.code == muninn::storage::StoreErrorCode::NotFound,
        "missing-base update should leave no persisted target snapshot"
    );

    std::vector<muninn::storage::TransportRecord> records;
    status = store.fetch_transport_batch(16, records);
    require(status.ok(), "missing-base update should still allow outbox fetch");
    require(records.empty(), "missing-base update must not enqueue transport records");

    store.close();
    std::filesystem::remove_all(db_dir, fs_error);
    fs_error.clear();
    std::filesystem::remove_all(root_dir, fs_error);
}

void test_incremental_capture_recovers_after_pruned_base() {
    const auto test_name = make_temp_name("muninn_watch_recovery");
    const auto root_dir = std::filesystem::temp_directory_path() / (test_name + "_root");
    const auto db_dir = std::filesystem::temp_directory_path() / (test_name + "_db");

    std::error_code fs_error;
    std::filesystem::create_directories(root_dir, fs_error);
    require(!fs_error, "watch-recovery test: failed to create root dir");
    std::filesystem::create_directories(db_dir, fs_error);
    require(!fs_error, "watch-recovery test: failed to create db dir");

    {
        std::ofstream file(root_dir / "seed.txt", std::ios::binary | std::ios::trunc);
        require(static_cast<bool>(file), "watch-recovery test: failed to create seed file");
        file << "seed";
    }

    muninn::storage::LmdbStore store;
    muninn::storage::LmdbStoreOptions options;
    options.directory = db_dir;
    options.map_size_bytes = 256ULL * 1024ULL * 1024ULL;

    auto status = store.open(options);
    require(status.ok(), "watch-recovery test: lmdb open should succeed");

    muninn::api::IndexingPipeline pipeline(store);

    const auto initial_create = pipeline.capture_snapshot(root_dir, {}, true);
    require(initial_create.ok(), "watch-recovery test: initial create should succeed");
    const auto base_snapshot_id = initial_create.snapshot_build.snapshot.snapshot_id;
    require(base_snapshot_id != 0U, "watch-recovery test: base snapshot id should be non-zero");

    std::this_thread::sleep_for(std::chrono::seconds(1));
    {
        std::ofstream mutate(root_dir / "seed.txt", std::ios::binary | std::ios::app);
        require(static_cast<bool>(mutate), "watch-recovery test: failed to mutate file before incremental update");
        mutate << "-v2";
    }

    const auto first_incremental = pipeline.capture_snapshot_and_delta(root_dir, base_snapshot_id, {}, true);
    require(first_incremental.ok(), "watch-recovery test: first incremental update should succeed");
    require(
        first_incremental.snapshot_build.snapshot.snapshot_id != 0U,
        "watch-recovery test: target snapshot id should be non-zero"
    );

    muninn::storage::RetentionCleanupStats retention_stats;
    status = store.apply_snapshot_retention(1U, &retention_stats);
    require(status.ok(), "watch-recovery test: retention should succeed");
    require(retention_stats.snapshots_removed >= 1U, "watch-recovery test: retention should prune at least one snapshot");

    muninn::domain::Snapshot pruned_base;
    status = store.get_snapshot(base_snapshot_id, pruned_base, true);
    require(!status.ok(), "watch-recovery test: base snapshot should be pruned");
    require(
        status.code == muninn::storage::StoreErrorCode::NotFound,
        "watch-recovery test: pruned base snapshot should return not found"
    );

    muninn::storage::StoreStats before_failed_update_stats;
    status = store.get_stats(before_failed_update_stats);
    require(status.ok(), "watch-recovery test: get_stats before failed update should succeed");

    std::this_thread::sleep_for(std::chrono::seconds(1));
    {
        std::ofstream mutate(root_dir / "seed.txt", std::ios::binary | std::ios::app);
        require(static_cast<bool>(mutate), "watch-recovery test: failed to mutate file before failed update");
        mutate << "-v3";
    }

    const auto failed_incremental = pipeline.capture_snapshot_and_delta(root_dir, base_snapshot_id, {}, true);
    require(!failed_incremental.ok(), "watch-recovery test: incremental update with pruned base should fail");
    require(
        failed_incremental.delta_store_status.code == muninn::storage::StoreErrorCode::NotFound,
        "watch-recovery test: failed incremental update should report base not found"
    );

    muninn::storage::StoreStats after_failed_update_stats;
    status = store.get_stats(after_failed_update_stats);
    require(status.ok(), "watch-recovery test: get_stats after failed update should succeed");
    require(
        after_failed_update_stats.snapshots_count == before_failed_update_stats.snapshots_count,
        "watch-recovery test: failed incremental update should not change snapshots count"
    );
    require(
        after_failed_update_stats.deltas_count == before_failed_update_stats.deltas_count,
        "watch-recovery test: failed incremental update should not change deltas count"
    );

    const auto recovered_full = pipeline.capture_snapshot(root_dir, {}, true);
    require(recovered_full.ok(), "watch-recovery test: full snapshot recovery should succeed");
    require(
        recovered_full.snapshot_build.snapshot.snapshot_id != 0U,
        "watch-recovery test: recovered full snapshot id should be non-zero"
    );

    store.close();
    std::filesystem::remove_all(db_dir, fs_error);
    fs_error.clear();
    std::filesystem::remove_all(root_dir, fs_error);
}

void test_transport_ack_is_consumer_scoped() {
    const auto snapshot = make_rename_storm_base(5);
    const auto target = make_rename_storm_target(snapshot);
    muninn::snapshot::DeltaEngine engine;
    const auto delta = engine.compute(snapshot, target, {});

    const auto temp_dir = std::filesystem::temp_directory_path() / make_temp_name("muninn_outbox_consumers");
    std::filesystem::create_directories(temp_dir);

    muninn::storage::LmdbStore store;
    muninn::storage::LmdbStoreOptions options;
    options.directory = temp_dir;
    options.map_size_bytes = 256ULL * 1024ULL * 1024ULL;

    auto status = store.open(options);
    require(status.ok(), "consumer-ack test: lmdb open should succeed");

    std::uint64_t seq_snapshot = 0U;
    std::uint64_t seq_delta = 0U;
    status = store.enqueue_snapshot(snapshot, true, &seq_snapshot);
    require(status.ok(), "consumer-ack test: enqueue snapshot should succeed");
    status = store.enqueue_delta(delta, &snapshot, &target, true, &seq_delta);
    require(status.ok(), "consumer-ack test: enqueue delta should succeed");
    require(seq_delta > seq_snapshot, "consumer-ack test: sequence should increase");

    status = store.register_transport_consumer("consumer-a");
    require(status.ok(), "consumer-ack test: register consumer-a should succeed");
    status = store.register_transport_consumer("consumer-b");
    require(status.ok(), "consumer-ack test: register consumer-b should succeed");

    std::vector<muninn::storage::TransportRecord> consumer_a_records;
    std::vector<muninn::storage::TransportRecord> consumer_b_records;
    status = store.fetch_transport_batch_for_consumer("consumer-a", 16U, consumer_a_records);
    require(status.ok(), "consumer-ack test: fetch for consumer-a should succeed");
    require(consumer_a_records.size() == 2U, "consumer-ack test: consumer-a should see two records");

    status = store.fetch_transport_batch_for_consumer("consumer-b", 16U, consumer_b_records);
    require(status.ok(), "consumer-ack test: fetch for consumer-b should succeed");
    require(consumer_b_records.size() == 2U, "consumer-ack test: consumer-b should see two records");

    status = store.ack_transport_until_for_consumer("consumer-a", seq_delta);
    require(status.ok(), "consumer-ack test: ack for consumer-a should succeed");
    consumer_a_records.clear();
    status = store.fetch_transport_batch_for_consumer("consumer-a", 16U, consumer_a_records);
    require(status.ok(), "consumer-ack test: fetch after ack for consumer-a should succeed");
    require(consumer_a_records.empty(), "consumer-ack test: consumer-a should have no unread records after ack");

    consumer_b_records.clear();
    status = store.fetch_transport_batch_for_consumer("consumer-b", 16U, consumer_b_records);
    require(status.ok(), "consumer-ack test: fetch for consumer-b after consumer-a ack should succeed");
    require(
        consumer_b_records.size() == 2U,
        "consumer-ack test: consumer-b should still see records after consumer-a ack"
    );

    status = store.compact_transport_up_to_min_acked();
    require(status.ok(), "consumer-ack test: compaction with consumer-b unacked should succeed");
    consumer_b_records.clear();
    status = store.fetch_transport_batch_for_consumer("consumer-b", 16U, consumer_b_records);
    require(status.ok(), "consumer-ack test: fetch for consumer-b after guarded compaction should succeed");
    require(consumer_b_records.size() == 2U, "consumer-ack test: guarded compaction must keep consumer-b records");

    status = store.ack_transport_until_for_consumer("consumer-b", seq_delta);
    require(status.ok(), "consumer-ack test: ack for consumer-b should succeed");
    status = store.compact_transport_up_to_min_acked();
    require(status.ok(), "consumer-ack test: compaction after both acks should succeed");

    std::vector<muninn::storage::TransportRecord> consumer_c_records;
    status = store.fetch_transport_batch_for_consumer("consumer-c", 16U, consumer_c_records);
    require(status.ok(), "consumer-ack test: fetch for consumer-c should succeed");
    require(consumer_c_records.empty(), "consumer-ack test: compaction should clear fully-acked outbox records");

    store.close();
    std::filesystem::remove_all(temp_dir);
}

void test_websocket_path_enforced() {
    WebsocketServerHarness harness;
    harness.start("/muninn-expected");

    WebsocketCrudTestClient client(harness.port());
    boost::system::error_code error;
    const bool connected = client.connect("/muninn-wrong", error);
    require(!connected, "websocket handshake should fail on unexpected path");
}

void test_http_operational_endpoints() {
    WebsocketServerHarness harness;
    harness.start("/muninn-ops");

    const auto health = http_get(harness.port(), "/healthz");
    require(health.status_code == 200U, "operational endpoints test: /healthz must return 200");
    const auto health_json = crud::text_to_json(health.body);
    require(health_json.has_value(), "operational endpoints test: /healthz response must be valid json");
    require(health_json->value("status", "") == "ok", "operational endpoints test: /healthz status must be ok");
    require(health_json->value("mode", "") == "live", "operational endpoints test: /healthz mode must be live");

    const auto readiness = http_get(harness.port(), "/readyz");
    require(readiness.status_code == 200U, "operational endpoints test: /readyz must return 200");
    const auto readiness_json = crud::text_to_json(readiness.body);
    require(readiness_json.has_value(), "operational endpoints test: /readyz response must be valid json");
    require(readiness_json->value("ready", false), "operational endpoints test: /readyz ready must be true");

    const auto non_upgrade = http_get(harness.port(), "/muninn-ops");
    require(non_upgrade.status_code == 400U, "operational endpoints test: non-upgrade websocket path must return 400");

    WebsocketCrudTestClient client(harness.port());
    boost::system::error_code error;
    const bool connected = client.connect("/muninn-ops", error);
    require(connected, "operational endpoints test: websocket connect should succeed");

    const auto hello_ack = send_hello_and_parse_ack(client, "ops-host", "test-client", "test-token");
    require(hello_ack.accepted, "operational endpoints test: hello should be accepted");

    crud::CrudCommandMessage create;
    create.request_id = "ops-create";
    create.operation = crud::Operation::Create;
    create.root_path = harness.root_dir().string();
    create.outbox_batch_size = 64U;
    const auto create_json = client.request(crud::to_json(create));
    const auto create_result = crud::parse_crud_result(create_json);
    require(create_result.has_value(), "operational endpoints test: create result should parse");
    require(create_result->ok, "operational endpoints test: create should succeed");

    const auto metrics = http_get(harness.port(), "/metrics");
    require(metrics.status_code == 200U, "operational endpoints test: /metrics must return 200");
    require(
        metrics.body.find("muninn_metrics_up 1") != std::string::npos,
        "operational endpoints test: /metrics must include muninn_metrics_up"
    );
    require(
        metrics.body.find("muninn_snapshots_count ") != std::string::npos,
        "operational endpoints test: /metrics must include muninn_snapshots_count"
    );
    require(
        metrics.body.find("muninn_transport_outbox_count ") != std::string::npos,
        "operational endpoints test: /metrics must include muninn_transport_outbox_count"
    );

    client.close();
}

void test_websocket_prod_requires_token() {
    WebsocketServerHarness harness;
    harness.start(
        "/muninn-prod",
        muninn::transport::TransportSecurityMode::Prod,
        "prod-token"
    );

    WebsocketCrudTestClient client(harness.port());
    boost::system::error_code error;
    const bool connected = client.connect("/muninn-prod", error);
    require(connected, "websocket handshake should succeed in prod mode");

    auto hello_ack = send_hello_and_parse_ack(client, "test-host", "test-client", "");
    const auto missing_token_ack = hello_ack;
    require(!missing_token_ack.accepted, "prod mode must reject missing auth token");
    require(
        missing_token_ack.reason.find("required in production mode") != std::string::npos,
        "prod missing-token rejection reason should mention required token"
    );

    hello_ack = send_hello_and_parse_ack(client, "test-host", "test-client", "wrong-token");
    const auto wrong_token_ack = hello_ack;
    require(!wrong_token_ack.accepted, "prod mode must reject wrong auth token");

    hello_ack = send_hello_and_parse_ack(client, "test-host", "test-client", "prod-token");
    const auto valid_token_ack = hello_ack;
    require(valid_token_ack.accepted, "prod mode should accept valid auth token");

    client.close();
}

void test_websocket_server_rejects_prod_without_token() {
    const auto test_name = make_temp_name("muninn_prod_no_token");
    const auto db_dir = std::filesystem::temp_directory_path() / (test_name + "_db");
    std::error_code fs_error;
    std::filesystem::create_directories(db_dir, fs_error);
    require(!fs_error, "prod-no-token test: failed to create db directory");

    muninn::storage::LmdbStore store;
    muninn::storage::LmdbStoreOptions options;
    options.directory = db_dir;
    options.map_size_bytes = 256ULL * 1024ULL * 1024ULL;
    auto status = store.open(options);
    require(status.ok(), "prod-no-token test: lmdb open should succeed");

    muninn::api::IndexingPipeline pipeline(store);
    boost::asio::io_context io_context;

    muninn::transport::WebsocketCrudServerOptions server_options;
    server_options.listen_address = "127.0.0.1";
    server_options.port = reserve_free_port();
    server_options.websocket_path = "/muninn-prod-no-token";
    server_options.security_mode = muninn::transport::TransportSecurityMode::Prod;
    server_options.allow_plain_websocket_in_prod = true;
    server_options.allowed_client_instance_id = "test-client";
    server_options.bridge_auth_token.clear();

    muninn::transport::WebsocketCrudServer server(io_context, store, pipeline, server_options);
    status = server.start();
    require(!status.ok(), "prod-no-token test: server start must fail");
    require(
        status.code == muninn::storage::StoreErrorCode::InvalidArgument,
        "prod-no-token test: server start should fail with invalid argument"
    );

    store.close();
    std::filesystem::remove_all(db_dir, fs_error);
}

void test_websocket_server_rejects_prod_without_transport_override() {
    const auto test_name = make_temp_name("muninn_prod_no_transport_override");
    const auto db_dir = std::filesystem::temp_directory_path() / (test_name + "_db");
    std::error_code fs_error;
    std::filesystem::create_directories(db_dir, fs_error);
    require(!fs_error, "prod-no-transport-override test: failed to create db directory");

    muninn::storage::LmdbStore store;
    muninn::storage::LmdbStoreOptions options;
    options.directory = db_dir;
    options.map_size_bytes = 256ULL * 1024ULL * 1024ULL;
    auto status = store.open(options);
    require(status.ok(), "prod-no-transport-override test: lmdb open should succeed");

    muninn::api::IndexingPipeline pipeline(store);
    boost::asio::io_context io_context;

    muninn::transport::WebsocketCrudServerOptions server_options;
    server_options.listen_address = "127.0.0.1";
    server_options.port = reserve_free_port();
    server_options.websocket_path = "/muninn-prod-no-transport-override";
    server_options.security_mode = muninn::transport::TransportSecurityMode::Prod;
    server_options.allow_plain_websocket_in_prod = false;
    server_options.allowed_client_instance_id = "test-client";
    server_options.bridge_auth_token = "prod-token";

    muninn::transport::WebsocketCrudServer server(io_context, store, pipeline, server_options);
    status = server.start();
    require(!status.ok(), "prod-no-transport-override test: server start must fail");
    require(
        status.code == muninn::storage::StoreErrorCode::InvalidArgument,
        "prod-no-transport-override test: server start should fail with invalid argument"
    );

    store.close();
    std::filesystem::remove_all(db_dir, fs_error);
}

void test_websocket_dev_allows_missing_token_when_unconfigured() {
    WebsocketServerHarness harness;
    harness.start(
        "/muninn-dev-open",
        muninn::transport::TransportSecurityMode::Dev,
        ""
    );

    WebsocketCrudTestClient client(harness.port());
    boost::system::error_code error;
    const bool connected = client.connect("/muninn-dev-open", error);
    require(connected, "dev-open test: websocket handshake should succeed");

    const auto hello_ack = send_hello_and_parse_ack(client, "test-host", "test-client", "");
    require(hello_ack.accepted, "dev-open test: hello without token should be accepted when no server token is set");

    client.close();
}

void test_websocket_multi_consumer_ack_isolation() {
    WebsocketServerHarness harness;
    harness.start("/muninn-multi");

    WebsocketCrudTestClient client_a(harness.port());
    WebsocketCrudTestClient client_b(harness.port());
    boost::system::error_code error;
    require(client_a.connect("/muninn-multi", error), "multi-consumer test: client A connect should succeed");
    require(client_b.connect("/muninn-multi", error), "multi-consumer test: client B connect should succeed");

    const auto hello_ack_a = send_hello_and_parse_ack(client_a, "host-a", "test-client", "test-token");
    require(hello_ack_a.accepted, "multi-consumer test: client A hello should be accepted");
    const auto hello_ack_b = send_hello_and_parse_ack(client_b, "host-b", "test-client", "test-token");
    require(hello_ack_b.accepted, "multi-consumer test: client B hello should be accepted");

    crud::CrudCommandMessage create;
    create.request_id = "multi-create";
    create.operation = crud::Operation::Create;
    create.root_path = harness.root_dir().string();
    create.outbox_batch_size = 64U;
    const auto create_json = client_a.request(crud::to_json(create));
    const auto create_result = crud::parse_crud_result(create_json);
    require(create_result.has_value(), "multi-consumer test: create result should parse");
    require(create_result->ok, "multi-consumer test: create command should succeed");
    require(!create_result->records.empty(), "multi-consumer test: create should return transport records");

    const std::uint64_t ack_sequence = max_record_sequence(*create_result);
    require(ack_sequence != 0U, "multi-consumer test: ack sequence should be non-zero");

    crud::IngestAckMessage ack_a;
    ack_a.request_id = "multi-ack-a";
    ack_a.ok = true;
    ack_a.ack_sequence = ack_sequence;
    ack_a.message = "ok";
    client_a.send_json(crud::to_json(ack_a));

    crud::CrudCommandMessage read_a;
    read_a.request_id = "multi-read-a";
    read_a.operation = crud::Operation::Read;
    read_a.outbox_batch_size = 64U;
    const auto read_a_json = client_a.request(crud::to_json(read_a));
    const auto read_a_result = crud::parse_crud_result(read_a_json);
    require(read_a_result.has_value(), "multi-consumer test: read-a result should parse");
    require(read_a_result->ok, "multi-consumer test: read-a should succeed");
    require(read_a_result->records.empty(), "multi-consumer test: client A should have no unread records after ack");

    crud::CrudCommandMessage read_b;
    read_b.request_id = "multi-read-b";
    read_b.operation = crud::Operation::Read;
    read_b.outbox_batch_size = 64U;
    const auto read_b_json = client_b.request(crud::to_json(read_b));
    const auto read_b_result = crud::parse_crud_result(read_b_json);
    require(read_b_result.has_value(), "multi-consumer test: read-b result should parse");
    require(read_b_result->ok, "multi-consumer test: read-b should succeed");
    require(!read_b_result->records.empty(), "multi-consumer test: client B should still see unread records");

    crud::IngestAckMessage ack_b;
    ack_b.request_id = "multi-ack-b";
    ack_b.ok = true;
    ack_b.ack_sequence = ack_sequence;
    ack_b.message = "ok";
    client_b.send_json(crud::to_json(ack_b));

    const auto read_b_after_ack_json = client_b.request(crud::to_json(read_b));
    const auto read_b_after_ack_result = crud::parse_crud_result(read_b_after_ack_json);
    require(read_b_after_ack_result.has_value(), "multi-consumer test: read-b-after-ack result should parse");
    require(read_b_after_ack_result->ok, "multi-consumer test: read-b-after-ack should succeed");
    require(
        read_b_after_ack_result->records.empty(),
        "multi-consumer test: client B should have no unread records after its own ack"
    );

    client_a.close();
    client_b.close();
}

void test_websocket_crud_flow() {
    WebsocketServerHarness harness;
    harness.start("/muninn");

    WebsocketCrudTestClient client(harness.port());
    boost::system::error_code error;
    const bool connected = client.connect("/muninn", error);
    require(connected, "websocket handshake should succeed on configured path");

    crud::CrudCommandMessage before_hello;
    before_hello.request_id = "before-hello";
    before_hello.operation = crud::Operation::Read;
    const auto before_hello_json = client.request(crud::to_json(before_hello));
    const auto before_hello_result = crud::parse_crud_result(before_hello_json);
    require(before_hello_result.has_value(), "crud_result should be returned for pre-hello command");
    require(!before_hello_result->ok, "pre-hello crud_command must be rejected");

    const auto hello_ack = send_hello_and_parse_ack(client, "test-host", "test-client", "test-token");
    require(hello_ack.accepted, "hello should be accepted for valid credentials");

    constexpr auto snapshot_record_type =
        static_cast<std::uint8_t>(muninn::storage::TransportRecordType::Snapshot);
    constexpr auto delta_record_type = static_cast<std::uint8_t>(muninn::storage::TransportRecordType::Delta);

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
    require(create_result->message == "snapshot_created", "create should return snapshot_created");
    require(create_result->records.size() == 1U, "create result should include exactly one unread record");
    require(
        count_records_of_type(*create_result, snapshot_record_type) == 1U,
        "create result should return exactly one snapshot record"
    );
    require(
        count_records_of_type(*create_result, delta_record_type) == 0U,
        "create result should not return a delta record"
    );

    const auto* create_snapshot_record = find_first_record_of_type(*create_result, snapshot_record_type);
    require(create_snapshot_record != nullptr, "create result should include a snapshot record");
    const auto created_snapshot = decode_snapshot_record(*create_snapshot_record);
    require(created_snapshot.has_value(), "create snapshot record should deserialize");
    require(
        created_snapshot->snapshot_id == create_result->snapshot_id,
        "create snapshot payload should match create result snapshot id"
    );

    {
        std::ostringstream text;
        text << "create snapshot_id=" << create_result->snapshot_id
             << " entries=" << created_snapshot->entries.size()
             << " transport_records=" << create_result->records.size();
        log_test("crud-flow", text.str());
    }

    crud::CrudCommandMessage read_before_ack;
    read_before_ack.request_id = "read-before-ack";
    read_before_ack.operation = crud::Operation::Read;
    read_before_ack.outbox_batch_size = 64U;
    const auto read_before_ack_json = client.request(crud::to_json(read_before_ack));
    const auto read_before_ack_result = crud::parse_crud_result(read_before_ack_json);
    require(read_before_ack_result.has_value(), "read-before-ack result should parse");
    require(read_before_ack_result->ok, "read-before-ack should succeed");
    require(read_before_ack_result->message == "ok", "read-before-ack should return ok");
    require(read_before_ack_result->records.size() == 1U, "read-before-ack should replay one unread snapshot");

    const auto* replay_snapshot_record = find_first_record_of_type(*read_before_ack_result, snapshot_record_type);
    require(replay_snapshot_record != nullptr, "read-before-ack should return a snapshot record");
    const auto replayed_snapshot = decode_snapshot_record(*replay_snapshot_record);
    require(replayed_snapshot.has_value(), "replayed snapshot record should deserialize");
    require(
        snapshot_equals(*created_snapshot, *replayed_snapshot),
        "read-before-ack should replay the same snapshot payload returned by create"
    );

    {
        std::ostringstream text;
        text << "read-before-ack returned_records=" << read_before_ack_result->records.size()
             << " max_sequence=" << max_record_sequence(*read_before_ack_result);
        log_test("crud-flow", text.str());
    }

    crud::IngestAckMessage ack_create;
    ack_create.request_id = "ack-create";
    ack_create.ok = true;
    ack_create.ack_sequence = max_record_sequence(*create_result);
    ack_create.message = "ok";
    client.send_json(crud::to_json(ack_create));

    crud::CrudCommandMessage read_after_create_ack;
    read_after_create_ack.request_id = "read-after-create-ack";
    read_after_create_ack.operation = crud::Operation::Read;
    read_after_create_ack.outbox_batch_size = 64U;
    const auto read_after_create_ack_json = client.request(crud::to_json(read_after_create_ack));
    const auto read_after_create_ack_result = crud::parse_crud_result(read_after_create_ack_json);
    require(read_after_create_ack_result.has_value(), "read-after-create-ack result should parse");
    require(read_after_create_ack_result->ok, "read-after-create-ack should succeed");
    require(
        read_after_create_ack_result->records.empty(),
        "outbox should be empty for this consumer after acking the create snapshot"
    );

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
    require(update_result->message == "snapshot_updated", "update should return snapshot_updated");
    require(update_result->base_snapshot_id == create_result->snapshot_id, "update should echo the base snapshot id");
    require(update_result->target_snapshot_id != 0U, "update should return target snapshot id");
    require(update_result->records.size() == 2U, "update should return one snapshot and one delta record");
    require(
        count_records_of_type(*update_result, snapshot_record_type) == 1U,
        "update should return exactly one snapshot record"
    );
    require(
        count_records_of_type(*update_result, delta_record_type) == 1U,
        "update should return exactly one delta record"
    );

    const auto* updated_snapshot_record = find_first_record_of_type(*update_result, snapshot_record_type);
    const auto* updated_delta_record = find_first_record_of_type(*update_result, delta_record_type);
    require(updated_snapshot_record != nullptr, "update should include a snapshot record");
    require(updated_delta_record != nullptr, "update should include a delta record");

    const auto target_snapshot = decode_snapshot_record(*updated_snapshot_record);
    const auto delta_snapshot = decode_delta_record(*updated_delta_record);
    require(target_snapshot.has_value(), "update snapshot record should deserialize");
    require(delta_snapshot.has_value(), "update delta record should deserialize");
    require(
        target_snapshot->snapshot_id == update_result->target_snapshot_id,
        "update snapshot payload should match target snapshot id"
    );
    require(
        delta_snapshot->base_snapshot_id == create_result->snapshot_id,
        "update delta payload should reference the base snapshot"
    );
    require(
        delta_snapshot->target_snapshot_id == update_result->target_snapshot_id,
        "update delta payload should reference the target snapshot"
    );

    const auto seed_path = (harness.root_dir() / "seed.txt").lexically_normal().generic_string();
    const auto* base_seed_entry = find_entry_by_path(*created_snapshot, seed_path);
    const auto* target_seed_entry = find_entry_by_path(*target_snapshot, seed_path);
    require(base_seed_entry != nullptr, "created snapshot should contain seed.txt");
    require(target_seed_entry != nullptr, "updated snapshot should contain seed.txt");
    require(
        target_seed_entry->metadata.size_bytes > base_seed_entry->metadata.size_bytes,
        "updated snapshot should reflect the larger seed.txt payload"
    );

    const auto delta_modified_it = std::find_if(
        delta_snapshot->modified_entries.begin(),
        delta_snapshot->modified_entries.end(),
        [&](const muninn::domain::ModifiedEntry& entry) {
            return entry.entry_id == target_seed_entry->id;
        }
    );
    require(
        delta_modified_it != delta_snapshot->modified_entries.end(),
        "update delta should include a modified entry for seed.txt"
    );
    require(
        delta_modified_it->before.size_bytes == base_seed_entry->metadata.size_bytes,
        "update delta should capture the previous seed.txt size"
    );
    require(
        delta_modified_it->after.size_bytes == target_seed_entry->metadata.size_bytes,
        "update delta should capture the new seed.txt size"
    );

    const auto applied_snapshot = apply_delta_for_test(*created_snapshot, *delta_snapshot);
    require(
        snapshot_entry_graph_equals(applied_snapshot, *target_snapshot),
        "applying the returned delta to the created snapshot should reproduce the updated snapshot entries"
    );

    {
        std::ostringstream text;
        text << "update base_snapshot_id=" << delta_snapshot->base_snapshot_id
             << " target_snapshot_id=" << delta_snapshot->target_snapshot_id
             << " delta_added=" << delta_snapshot->added_entries.size()
             << " delta_removed=" << delta_snapshot->removed_entries.size()
             << " delta_modified=" << delta_snapshot->modified_entries.size()
             << " delta_renamed=" << delta_snapshot->renamed_entries.size()
             << " seed_size=" << base_seed_entry->metadata.size_bytes
             << "->" << target_seed_entry->metadata.size_bytes;
        log_test("crud-flow", text.str());
    }

    const std::uint64_t ack_sequence = max_record_sequence(*update_result);
    require(ack_sequence != 0U, "update ack sequence must be non-zero");

    crud::IngestAckMessage ingest_ack;
    ingest_ack.request_id = "ack-update";
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
    del.snapshot_id = update_result->target_snapshot_id;
    const auto delete_json = client.request(crud::to_json(del));
    const auto delete_result = crud::parse_crud_result(delete_json);
    require(delete_result.has_value(), "delete result should parse");
    require(delete_result->ok, "delete command should succeed");
    require(
        delete_result->message == "snapshot_deleted",
        "delete command should return snapshot_deleted"
    );
    require(
        delete_result->snapshot_id == update_result->target_snapshot_id,
        "delete should echo the deleted snapshot id"
    );
    require(delete_result->records.empty(), "delete should not enqueue transport records");

    muninn::domain::Snapshot loaded_snapshot;
    auto status = harness.store().get_snapshot(update_result->target_snapshot_id, loaded_snapshot, true);
    require(!status.ok(), "deleted target snapshot should be removed from the store");
    require(
        status.code == muninn::storage::StoreErrorCode::NotFound,
        "deleted target snapshot should return not found"
    );

    status = harness.store().get_snapshot(create_result->snapshot_id, loaded_snapshot, true);
    require(status.ok(), "base snapshot should remain after deleting the target snapshot");

    muninn::domain::DeltaSnapshot loaded_delta;
    status = harness.store().get_delta(create_result->snapshot_id, update_result->target_snapshot_id, loaded_delta, true);
    require(!status.ok(), "delete should remove the delta tied to the deleted snapshot");
    require(
        status.code == muninn::storage::StoreErrorCode::NotFound,
        "deleted delta should return not found"
    );

    muninn::storage::StoreStats stats;
    status = harness.store().get_stats(stats);
    require(status.ok(), "get_stats should succeed after delete");
    require(stats.snapshots_count == 1U, "only the base snapshot should remain after delete");
    require(stats.deltas_count == 0U, "all deltas for the deleted snapshot should be removed");

    {
        std::ostringstream text;
        text << "delete snapshot_id=" << delete_result->snapshot_id
             << " remaining_snapshots=" << stats.snapshots_count
             << " remaining_deltas=" << stats.deltas_count;
        log_test("crud-flow", text.str());
    }

    client.close();
}

} // namespace

int main() {
    test_empty_fixture_validation();
    test_invalid_header_detected();
    test_deep_tree_fixture();
    test_scanner_include_exclude_globs();
    test_permission_edge_fixture();
    test_delta_rename_storm();
    test_snapshot_round_trip_binary_json();
    test_delta_round_trip_binary_json();
    test_migration_contract();
    test_lmdb_store_round_trip();
    test_snapshot_retention_prunes_old_snapshots_and_deltas();
    test_delete_snapshot_removes_related_deltas();
    test_store_stats_counts_records();
    test_update_with_missing_base_has_no_side_effects();
    test_incremental_capture_recovers_after_pruned_base();
    test_transport_ack_is_consumer_scoped();
    test_websocket_path_enforced();
    test_http_operational_endpoints();
    test_websocket_prod_requires_token();
    test_websocket_server_rejects_prod_without_token();
    test_websocket_server_rejects_prod_without_transport_override();
    test_websocket_dev_allows_missing_token_when_unconfigured();
    test_websocket_multi_consumer_ack_isolation();
    test_websocket_crud_flow();

    std::cout << "All Muninn tests passed.\n";
    return 0;
}
