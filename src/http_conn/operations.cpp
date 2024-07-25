#include <http_conn/operations.h>
#include <fstream>
#include <sstream>
#include <map>
#include <string>

namespace HTML {

http::response<http::string_body> Operations::handle_request(const http::request<http::string_body>& req, const std::string& doc_root) {
    // Helper function to determine content type based on file extension
    auto get_content_type = [](const std::string& path) -> std::string {
        static const std::map<std::string, std::string> mime_types = {
            {".html", "text/html"},
            {".css", "text/css"},
            {".js", "application/javascript"},
            {".png", "image/png"},
            {".jpg", "image/jpeg"},
        };

        auto dot_pos = path.rfind('.');
        if (dot_pos != std::string::npos) {
            std::string extension = path.substr(dot_pos);
            auto it = mime_types.find(extension);
            if (it != mime_types.end()) {
                return it->second;
            }
        }
        return "application/octet-stream"; 
    };

    if (req.method() == http::verb::get) {
        std::string path{req.target()};
        if (path == "/") {
            path = "/index.html";
        }

        std::string full_path = doc_root + path;

        std::ifstream ifs(full_path, std::ios::binary);
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
        res.set(http::field::content_type, get_content_type(full_path));
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
