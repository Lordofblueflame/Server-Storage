#ifndef SESSION_H
#define SESSION_H

#include <boost/beast.hpp>
#include <boost/asio.hpp>
#include <memory>
#include <string>

namespace HTML {

namespace beast = boost::beast;
namespace http = beast::http;

class Session : public std::enable_shared_from_this<Session> {
public:
    explicit Session(boost::asio::ip::tcp::socket socket, const std::string& doc_root);

    void start();

private:
    void do_read();
    void do_write();
    http::response<http::string_body> handle_request(const http::request<http::string_body>& req);

    boost::asio::ip::tcp::socket socket_;
    beast::flat_buffer buffer_;
    http::request<http::string_body> request_;
    http::response<http::string_body> response_;
    std::string doc_root_;
};

}

#endif // SESSION_H
