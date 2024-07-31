#include <http_conn/web_server.h>
#include <http_conn/session.h>

namespace HTML {

WebServer::WebServer(const std::string& address, unsigned short port)
    : address_(address), port_(port), ioc_(), acceptor_(ioc_) {
    
    boost::asio::ip::tcp::resolver resolver(ioc_);
    boost::asio::ip::tcp::endpoint endpoint = *resolver.resolve(address_, std::to_string(port_)).begin();

    acceptor_.open(endpoint.protocol());
    acceptor_.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
    acceptor_.bind(endpoint);
    acceptor_.listen();
    BOOST_LOG_TRIVIAL(info) << "WebServer initialized at address: " << address_ << " on port: " << port_;
    }

void WebServer::run() {
    start_accept();
    BOOST_LOG_TRIVIAL(info) << "WebServer is running at address: " << address_ << " on port: " << port_;
    ioc_.run();
}

void WebServer::start_accept() {
    acceptor_.async_accept([this](boost::system::error_code ec, tcp::socket socket) {
        if (!ec) {
            auto remote_endpoint = socket.remote_endpoint();
            BOOST_LOG_TRIVIAL(info) << "Accepted connection from " << remote_endpoint.address().to_string() << ":" << remote_endpoint.port();
            std::make_shared<Session>(std::move(socket))->start();
        } else {
            BOOST_LOG_TRIVIAL(error) << "Error accepting connection: " << ec.message();
        }
        start_accept(); 
    });
}

} 
