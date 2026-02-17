#ifndef HOSTINDEXER_TRANSPORT_WS_CRUD_SERVER_HPP
#define HOSTINDEXER_TRANSPORT_WS_CRUD_SERVER_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include "../api/indexing_pipeline.hpp"
#include "../storage/lmdb_store.hpp"

namespace host_indexer::transport {

struct WebsocketCrudServerOptions {
    std::string listen_address {"0.0.0.0"};
    std::uint16_t port {9460};
    std::string websocket_path {"/hostindexer"};
    std::filesystem::path default_root_path {};
    std::size_t default_outbox_batch_size {64U};
    std::string allowed_client_instance_id {"localedge-gateway-1"};
    std::string bridge_auth_token {};
};

class WebsocketCrudServer final {
public:
    WebsocketCrudServer(boost::asio::io_context& io_context,
                        storage::LmdbStore& store,
                        api::IndexingPipeline& pipeline,
                        WebsocketCrudServerOptions options = {});

    storage::StoreStatus start();
    void stop();
    [[nodiscard]] bool running() const noexcept;

private:
    void do_accept();

    boost::asio::io_context& io_context_;
    boost::asio::ip::tcp::acceptor acceptor_;
    storage::LmdbStore* store_ {nullptr};
    api::IndexingPipeline* pipeline_ {nullptr};
    WebsocketCrudServerOptions options_ {};
    std::mutex pipeline_mutex_ {};
    std::atomic<bool> running_ {false};
};

} // namespace host_indexer::transport

#endif // HOSTINDEXER_TRANSPORT_WS_CRUD_SERVER_HPP
