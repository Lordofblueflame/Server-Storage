#include "test_common.hpp"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <utility>

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>

#include "../net/muninn_transport.hpp"
#include "shared/crud_protocol.hpp"

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
namespace crud = backend::shared::crud;

std::uint16_t reserve_free_port() {
    asio::io_context io_context;
    tcp::acceptor acceptor(io_context);
    boost::system::error_code error;

    acceptor.open(tcp::v4(), error);
    require_true(!error, "reserve_free_port: open failed");
    acceptor.bind(tcp::endpoint(tcp::v4(), 0), error);
    require_true(!error, "reserve_free_port: bind failed");
    const auto endpoint = acceptor.local_endpoint(error);
    require_true(!error, "reserve_free_port: local endpoint failed");
    acceptor.close(error);
    require_true(!error, "reserve_free_port: close failed");
    return endpoint.port();
}

class MockMuninnServer final {
public:
    explicit MockMuninnServer(const std::uint16_t port)
        : port_(port) {
    }

    void start() {
        worker_ = std::thread([this]() {
            run();
        });
    }

    void join() {
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    [[nodiscard]] bool saw_first_hello() const noexcept {
        return saw_first_hello_.load();
    }

    [[nodiscard]] bool saw_ingest_ack() const noexcept {
        return saw_ingest_ack_.load();
    }

    [[nodiscard]] bool saw_reconnect_hello() const noexcept {
        return saw_reconnect_hello_.load();
    }

private:
    void run() {
        asio::io_context io_context;
        tcp::acceptor acceptor(io_context, tcp::endpoint(tcp::v4(), port_));

        // First connection: hello -> hello_ack -> crud_result -> ingest_ack -> close.
        {
            tcp::socket socket(io_context);
            acceptor.accept(socket);
            websocket::stream<tcp::socket> ws(std::move(socket));
            ws.accept();

            beast::flat_buffer read_buffer;
            ws.read(read_buffer);
            const auto hello_json = crud::text_to_json(beast::buffers_to_string(read_buffer.data()));
            require_true(hello_json.has_value(), "mock server: hello payload must be valid json");
            const auto hello = crud::parse_hello(*hello_json);
            require_true(hello.has_value(), "mock server: expected hello message");
            saw_first_hello_.store(true);

            crud::HelloAckMessage hello_ack;
            hello_ack.accepted = true;
            hello_ack.reason = "ok";
            ws.text(true);
            ws.write(asio::buffer(crud::json_to_text(crud::to_json(hello_ack))));

            const nlohmann::json result = {
                {"type", "crud_result"},
                {"protocol_version", 1},
                {"request_id", "server-seq-1"},
                {"ok", true},
                {"message", "records_ready"},
                {"snapshot_id", 42},
                {"base_snapshot_id", 0},
                {"target_snapshot_id", 42},
                {"records",
                 nlohmann::json::array(
                     {
                         {
                             {"sequence", 5},
                             {"record_type", 1},
                             {"created_at", 1700000001},
                             {"routing_key", "test-host"},
                             {"payload_b64", "AA=="}
                         }
                     }
                 )}
            };
            require_true(crud::parse_crud_result(result).has_value(), "mock server: crud_result fixture must parse");
            ws.text(true);
            ws.write(asio::buffer(crud::json_to_text(result)));

            read_buffer.consume(read_buffer.size());
            ws.read(read_buffer);
            const auto ack_json = crud::text_to_json(beast::buffers_to_string(read_buffer.data()));
            require_true(ack_json.has_value(), "mock server: ingest_ack payload must be valid json");
            const auto ingest_ack = crud::parse_ingest_ack(*ack_json);
            require_true(ingest_ack.has_value(), "mock server: expected ingest_ack message");
            require_true(ingest_ack->ack_sequence == 5U, "mock server: ack sequence must match record sequence");
            saw_ingest_ack_.store(true);

            boost::system::error_code ignored;
            ws.close(websocket::close_code::normal, ignored);
        }

        // Second connection: expect reconnect and hello again.
        {
            tcp::socket socket(io_context);
            acceptor.accept(socket);
            websocket::stream<tcp::socket> ws(std::move(socket));
            ws.accept();

            beast::flat_buffer read_buffer;
            ws.read(read_buffer);
            const auto hello_json = crud::text_to_json(beast::buffers_to_string(read_buffer.data()));
            require_true(hello_json.has_value(), "mock server: reconnect hello payload must be valid json");
            const auto hello = crud::parse_hello(*hello_json);
            require_true(hello.has_value(), "mock server: expected reconnect hello");
            saw_reconnect_hello_.store(true);

            crud::HelloAckMessage hello_ack;
            hello_ack.accepted = true;
            hello_ack.reason = "ok";
            ws.text(true);
            ws.write(asio::buffer(crud::json_to_text(crud::to_json(hello_ack))));

            boost::system::error_code ignored;
            ws.close(websocket::close_code::normal, ignored);
        }
    }

    std::uint16_t port_ {0U};
    std::thread worker_ {};
    std::atomic<bool> saw_first_hello_ {false};
    std::atomic<bool> saw_ingest_ack_ {false};
    std::atomic<bool> saw_reconnect_hello_ {false};
};

} // namespace

bool run_muninn_client_integration_tests() {
    {
        asio::io_context disconnected_io;
        hugin::net::MuninnClientOptions disconnected_options;
        disconnected_options.remote_host = "127.0.0.1";
        disconnected_options.remote_port = reserve_free_port();
        disconnected_options.auto_pull_enabled = false;

        auto disconnected_client = hugin::net::create_muninn_transport_client(
            disconnected_io,
            disconnected_options
        );
        disconnected_client->start();

        crud::CrudCommandMessage command;
        command.operation = crud::Operation::Create;
        command.root_path = "/tmp";
        const auto status = disconnected_client->submit_command(std::move(command));
        require_true(!status.ok(), "integration test: disconnected bridge must reject command submission");

        disconnected_client->stop();
    }

    const std::uint16_t port = reserve_free_port();
    MockMuninnServer server(port);
    server.start();

    asio::io_context io_context;
    hugin::net::MuninnClientOptions options;
    options.remote_host = "127.0.0.1";
    options.remote_port = port;
    options.websocket_path = "/muninn";
    options.host_id = "hugin-test-host";
    options.client_instance_id = "hugin-test-client";
    options.auth_token = "token";
    options.auto_pull_enabled = false;
    options.reconnect_delay = std::chrono::milliseconds(50);
    options.reconnect_max_delay = std::chrono::milliseconds(200);
    options.reconnect_backoff_factor = 2U;
    options.reconnect_jitter_percent = 0U;

    auto client = hugin::net::create_muninn_transport_client(io_context, options);
    client->start();

    std::thread io_thread([&io_context]() {
        io_context.run();
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < deadline) {
        if (server.saw_first_hello() && server.saw_ingest_ack() && server.saw_reconnect_hello()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    require_true(server.saw_first_hello(), "integration test: client must send initial hello");
    require_true(server.saw_ingest_ack(), "integration test: client must send ingest_ack");
    require_true(server.saw_reconnect_hello(), "integration test: client must reconnect and send hello again");

    client->stop();
    io_context.stop();
    if (io_thread.joinable()) {
        io_thread.join();
    }
    server.join();
    return true;
}
