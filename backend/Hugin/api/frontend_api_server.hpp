#ifndef HUGIN_API_FRONTEND_API_SERVER_HPP
#define HUGIN_API_FRONTEND_API_SERVER_HPP

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <nlohmann/json.hpp>

#include "../common/status.hpp"
#include "shared/crud_protocol.hpp"

namespace hugin::api {

class FrontendApiSharedState;

enum class BackpressurePolicy : std::uint8_t {
    DropOldest = 1,
    DropNewest = 2,
    Disconnect = 3
};

struct FrontendApiServerOptions {
    std::string listen_address {"0.0.0.0"};
    std::uint16_t port {8080};

    std::string health_path {"/api/v1/health"};
    std::string command_path {"/api/v1/commands"};
    std::string stream_path {"/api/v1/stream"};
    std::string openapi_path {"/api/v1/openapi.json"};
    std::string docs_path {"/api/v1/docs"};
    std::string realtime_tree_path {"/api/v1/realtime/tree"};
    std::string realtime_snapshot_path {"/api/v1/realtime/snapshot"};
    std::string file_download_path {"/api/v1/files/download"};
    std::string file_upload_path {"/api/v1/files/upload"};
    std::string directory_upload_path {"/api/v1/files/upload-directory"};
    std::filesystem::path allowed_read_root {};
    std::filesystem::path allowed_write_root {};
    std::size_t max_upload_file_bytes {16U * 1024U * 1024U};
    std::size_t max_upload_directory_files {1024U};
    std::size_t max_upload_total_decoded_bytes {64U * 1024U * 1024U};
    std::size_t directory_upload_threads {4U};

    bool require_jwt {true};
    std::string jwt_secret {};
    std::string jwt_issuer {};
    std::string jwt_audience {};
    std::uint64_t jwt_clock_skew_seconds {30U};
    bool jwt_require_exp_claim {true};
    std::string local_access_token {};
    std::size_t max_http_request_body_bytes {8U * 1024U * 1024U};

    std::size_t ws_max_pending_messages {1024U};
    std::size_t ws_max_pending_bytes {8U * 1024U * 1024U};
    BackpressurePolicy ws_backpressure_policy {BackpressurePolicy::DropOldest};

    std::size_t command_worker_threads {4U};
    std::size_t command_max_queue_per_worker {1024U};
    std::chrono::milliseconds command_execution_timeout {1'500};
};

using CommandHandler = std::function<common::Status(const backend::shared::crud::CrudCommandMessage&)>;
using RealtimeTreeHandler = std::function<common::StatusOr<nlohmann::json>(std::string_view host_id,
                                                                            std::string_view path)>;
using RealtimeSnapshotHandler = std::function<common::StatusOr<nlohmann::json>(std::string_view host_id,
                                                                                std::string_view path,
                                                                                bool include_entries)>;

class FrontendApiServer final {
public:
    FrontendApiServer(boost::asio::io_context& io_context,
                      FrontendApiServerOptions options,
                      CommandHandler command_handler,
                      RealtimeTreeHandler realtime_tree_handler = {},
                      RealtimeSnapshotHandler realtime_snapshot_handler = {});

    common::Status start();
    void stop();
    [[nodiscard]] bool running() const noexcept;

    void broadcast_crud_result(const backend::shared::crud::CrudResultMessage& result);

private:
    void do_accept();

    boost::asio::io_context& io_context_;
    boost::asio::ip::tcp::acceptor acceptor_;
    FrontendApiServerOptions options_ {};
    CommandHandler command_handler_ {};
    RealtimeTreeHandler realtime_tree_handler_ {};
    RealtimeSnapshotHandler realtime_snapshot_handler_ {};
    std::shared_ptr<FrontendApiSharedState> shared_state_ {};
    std::atomic<bool> running_ {false};
};

} // namespace hugin::api

#endif // HUGIN_API_FRONTEND_API_SERVER_HPP
