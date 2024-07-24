#include <http_conn\html_server.h>
#include "pch.h"
#include <fstream>
WebServer::WebServer(const std::string& address, unsigned short port, const std::string& doc_root)
        : address_(net::ip::make_address(address)), port_(port), doc_root_(doc_root), ioc_{1} {}

void WebServer::run() {
    tcp::acceptor acceptor{ioc_, {address_, port_}};
    for (;;) {
        tcp::socket socket{ioc_};
        acceptor.accept(socket);
        std::thread{&WebServer::do_session, this, std::move(socket)}.detach();
    }
}

http::response<http::string_body> WebServer::handle_request(http::request<http::string_body> const& req) {
    if (req.method() == http::verb::get) {
        std::string path = doc_root_ + "/index.html";
        std::ifstream ifs(path);
        if (!ifs) {
            http::response<http::string_body> res{http::status::not_found, req.version()};
            res.set(http::field::server, "Beast");
            res.set(http::field::content_type, "text/plain");
            res.body() = "File not found";
            res.prepare_payload();
            return res;
        }

        std::stringstream buffer;
        buffer << ifs.rdbuf();

        http::response<http::string_body> res{http::status::ok, req.version()};
        res.set(http::field::server, "Beast");
        res.set(http::field::content_type, "text/html");
        res.body() = buffer.str();
        res.prepare_payload();
        return res;
    } else {
        http::response<http::string_body> res{http::status::bad_request, req.version()};
        res.set(http::field::server, "Beast");
        res.set(http::field::content_type, "text/plain");
        res.body() = "Unsupported HTTP method";
        res.prepare_payload();
        return res;
    }
}
void WebServer::do_session(tcp::socket socket) {
    try {
        beast::flat_buffer buffer;
        http::request<http::string_body> req;

        http::read(socket, buffer, req);

        http::response<http::string_body> const res = handle_request(req);
        http::write(socket, res);

        beast::error_code ec;
        socket.shutdown(tcp::socket::shutdown_send, ec);
    } catch (std::exception const& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
}