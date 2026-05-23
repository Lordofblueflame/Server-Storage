#ifndef MUNINN_TRANSPORT_CRUD_SERVER_HPP
#define MUNINN_TRANSPORT_CRUD_SERVER_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include <boost/asio/io_context.hpp>

#include "../api/indexing_pipeline.hpp"
#include "../storage/lmdb_store.hpp"

namespace muninn::transport {

enum class CrudTransportKind : std::uint8_t {
    WebSocket = 0
};

inline constexpr std::string_view crud_transport_kind_name(const CrudTransportKind kind) noexcept {
    switch (kind) {
        case CrudTransportKind::WebSocket:
            return "websocket";
    }
    return "websocket";
}

enum class TransportSecurityMode : std::uint8_t {
    Dev = 0,
    Prod = 1
};

struct CrudServerOptions {
    CrudTransportKind transport_kind {CrudTransportKind::WebSocket};
    std::string listen_address {"0.0.0.0"};
    std::uint16_t port {9460};
    std::string websocket_path {"/muninn"};
    std::string health_path {"/healthz"};
    std::string readiness_path {"/readyz"};
    std::string metrics_path {"/metrics"};
    std::filesystem::path default_root_path {};
    std::size_t default_outbox_batch_size {64U};
    filesystem::ScanOptions scan_options {};
    std::size_t retention_keep_latest_snapshots {0U};
    TransportSecurityMode security_mode {TransportSecurityMode::Dev};
    bool allow_plain_websocket_in_prod {false};
    bool allow_remote_mutating_commands {false};
    std::string allowed_client_instance_id {"hugin-gateway-1"};
    std::string bridge_auth_token {};
};

class CrudTransportServer {
public:
    virtual ~CrudTransportServer() = default;

    virtual storage::StoreStatus start() = 0;
    virtual void stop() = 0;
    [[nodiscard]] virtual bool running() const noexcept = 0;
};

std::unique_ptr<CrudTransportServer> create_crud_transport_server(boost::asio::io_context& io_context,
                                                                  storage::LmdbStore& store,
                                                                  api::IndexingPipeline& pipeline,
                                                                  CrudServerOptions options = {});

} // namespace muninn::transport

#endif // MUNINN_TRANSPORT_CRUD_SERVER_HPP
