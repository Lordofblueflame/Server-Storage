#include <http_conn/web_server.h>
#include <http_conn/session.h>

namespace HTML {

WebServer::WebServer(const std::string& address, unsigned short port, const std::string& doc_root)
    : address_(address), port_(port), doc_root_(doc_root), ioc_{1}, acceptor_(ioc_, {boost::asio::ip::make_address(address), port}) {}

void WebServer::run() {
    start_accept();
    ioc_.run();
}

void WebServer::start_accept() {
    acceptor_.async_accept([this](boost::system::error_code ec, boost::asio::ip::tcp::socket socket) {
        if (!ec) {
            std::make_shared<Session>(std::move(socket), doc_root_)->start();
        }
        start_accept(); 
    });
}

} 
