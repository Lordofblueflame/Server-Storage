#ifndef SESSION_H
#define SESSION_H

#include "pch.h"

namespace HTML {

class Session : public std::enable_shared_from_this<Session> {
public:
    explicit Session(tcp::socket socket);

    void start();

private:
    void do_read();
    void do_write();
    http::response<http::string_body> handle_request(const http::request<http::string_body>& req);

    tcp::socket socket_;
    beast::flat_buffer buffer_;
    http::request<http::string_body> request_;
    http::response<http::string_body> response_;
    const std::string doc_root_ = fs::absolute(getProjectDir() + "/web").string();
};

}

#endif // SESSION_H
