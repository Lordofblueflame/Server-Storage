#include <http_conn/session.h>
#include <http_conn/operations.h>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <fstream>
#include <sstream>

namespace HTML {

Session::Session(boost::asio::ip::tcp::socket socket, const std::string& doc_root)
    : socket_(std::move(socket)), doc_root_(doc_root) {}

void Session::start() {
    do_read();
}

void Session::do_read() {
    auto self(shared_from_this());
    http::async_read(socket_, buffer_, request_,
        [self](boost::system::error_code ec, std::size_t) {
            if (!ec) {
                self->do_write();
            }
        });
}

void Session::do_write() {
    auto self(shared_from_this());
    response_ = Operations::handle_request(request_, doc_root_);
    http::async_write(socket_, response_,
        [self](boost::system::error_code ec, std::size_t) {
            if (!ec) {
                boost::system::error_code shutdown_ec;
                self->socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_send, shutdown_ec);
            }
        });
}

http::response<http::string_body> Session::handle_request(const http::request<http::string_body>& req) {
    return Operations::handle_request(req, doc_root_);
}

} 
