#include "pch.h"
#include <http_conn/session.h>
#include <http_conn/operations.h>

namespace HTML {

Session::Session(tcp::socket socket)
    : socket_(std::move(socket)) {}

void Session::start() {
    BOOST_LOG_TRIVIAL(info) << "Starting session with client: " << socket_.remote_endpoint().address().to_string() << ":" << socket_.remote_endpoint().port();
    do_read();
}

void Session::do_read() {
    auto self(shared_from_this());
    http::async_read(socket_, buffer_, request_,
        [self](boost::system::error_code ec, std::size_t) {
            if (!ec) {
                BOOST_LOG_TRIVIAL(info) << "Received request: " << self->request_.method() << " " << self->request_.target() << " " << self->request_.version();
                self->do_write();
            } else {
                BOOST_LOG_TRIVIAL(error) << "Error reading request: " << ec.message();
            }
        });
}

void Session::do_write() {
    auto self(shared_from_this());
    response_ = Operations::handle_request(request_, doc_root_);
    BOOST_LOG_TRIVIAL(info) << "Sending response: " << response_.result() << " for request: " << request_.method() << " " << request_.target();
    
    http::async_write(socket_, response_,
        [self](boost::system::error_code ec, std::size_t) {
            if (!ec) {
                BOOST_LOG_TRIVIAL(info) << "Response successfully sent to client: " << self->socket_.remote_endpoint().address().to_string() << ":" << self->socket_.remote_endpoint().port();
                
                boost::system::error_code shutdown_ec;
                self->socket_.shutdown(tcp::socket::shutdown_send, shutdown_ec);
                if (shutdown_ec) {
                    BOOST_LOG_TRIVIAL(error) << "Error shutting down connection: " << shutdown_ec.message();
                } else {
                    BOOST_LOG_TRIVIAL(info) << "Connection closed with client: " << self->socket_.remote_endpoint().address().to_string() << ":" << self->socket_.remote_endpoint().port();
                }
            } else {
                BOOST_LOG_TRIVIAL(error) << "Error sending response: " << ec.message();
            }
        });
}

http::response<http::string_body> Session::handle_request(const http::request<http::string_body>& req) {
    return Operations::handle_request(req, doc_root_);
}

} 
