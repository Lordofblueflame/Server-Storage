#include <http_conn/operations.h>
#include <fstream>
#include <sstream>

namespace HTML {

http::response<http::string_body> Operations::handle_request(const http::request<http::string_body>& req, const std::string& doc_root) {
    auto get_content_type = [](const std::string& path) -> std::string {
        static const std::map<std::string, std::string> mime_types = {
            {".html", "text/html"},
            {".css", "text/css"},
            {".js", "application/javascript"},
            {".png", "image/png"},
            {".jpg", "image/jpeg"},
        };

    if (req.method() == http::verb::get) {
        std::string path = doc_root + "/index.html";
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

} 
