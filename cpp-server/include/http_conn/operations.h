#ifndef OPERATIONS_H
#define OPERATIONS_H

#include "pch.h"

namespace HTML {

class Operations {
public:
    static http::response<http::string_body> handle_request(const http::request<http::string_body>& req, const std::string& doc_root);

private: 
    static std::string get_content_type(const std::string& path);
    static http::response<http::string_body> create_directory_listing(const std::string& dir_path, const http::request<http::string_body>& req);
    static std::vector<std::string> list_directory_contents(const std::string& dir_path);
    static http::response<http::string_body> unsupported_http_method(const http::request<http::string_body>& req);
};

}

#endif // OPERATIONS_H
