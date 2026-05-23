#ifndef MUNINN_TRANSPORT_WS_CRUD_SERVER_HPP
#define MUNINN_TRANSPORT_WS_CRUD_SERVER_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include "crud_command_processor.hpp"

namespace muninn::transport {
using WebsocketCrudServerOptions = CrudServerOptions;

class WebsocketCrudServer final : public CrudTransportServer {
public:
    WebsocketCrudServer(boost::asio::io_context& io_context,
                        storage::LmdbStore& store,
                        api::IndexingPipeline& pipeline,
                        WebsocketCrudServerOptions options = {});

    storage::StoreStatus start() override;
    void stop() override;
    [[nodiscard]] bool running() const noexcept override;

private:
    void do_accept();

    boost::asio::io_context& io_context_;
    boost::asio::ip::tcp::acceptor acceptor_;
    storage::LmdbStore* store_ {nullptr};
    WebsocketCrudServerOptions options_ {};
    std::mutex pipeline_mutex_ {};
    CrudCommandProcessor command_processor_;
    std::atomic<bool> running_ {false};
};

} // namespace muninn::transport

#endif // MUNINN_TRANSPORT_WS_CRUD_SERVER_HPP
