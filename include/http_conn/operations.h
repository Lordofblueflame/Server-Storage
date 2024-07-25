#ifndef OPERATIONS_H
#define OPERATIONS_H

#include <boost/beast.hpp>
#include <boost/asio.hpp>
#include <string>

namespace HTML {

namespace beast = boost::beast;
namespace http = beast::http;

class Operations {
public:
    static http::response<http::string_body> handle_request(const http::request<http::string_body>& req, const std::string& doc_root);
};

}

#endif // OPERATIONS_H
