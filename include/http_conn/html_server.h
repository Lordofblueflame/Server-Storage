#ifndef WEBSERVER_H
#define WEBSERVER_H

class WebServer {
public:
    WebServer(const std::string& address, unsigned short port, const std::string& doc_root);

    void run();
private:
    http::response<http::string_body> handle_request(http::request<http::string_body> const& req);

    void do_session(tcp::socket socket);

private:
    net::ip::address address_;
    unsigned short port_;
    std::string doc_root_;
    net::io_context ioc_;
};


#endif //WEBSERVER_H