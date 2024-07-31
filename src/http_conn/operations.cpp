#include "pch.h"
#include <http_conn/operations.h>

namespace HTML {

http::response<http::string_body> Operations::handle_request(const http::request<http::string_body>& req, const std::string& doc_root) {
    if (req.method() == http::verb::get) {
        std::string path{req.target().data(), req.target().size()}; 
        
        if (path == "/") {
            path = "/index.html";
        }

        std::string full_path = doc_root + path;

        if (fs::is_directory(full_path)) {
            return create_directory_listing(full_path, req);
        } 

        std::ifstream ifs(full_path, std::ios::binary);
        if (!ifs) {
            BOOST_LOG_TRIVIAL(error) << "File not found: " << full_path;
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
        BOOST_LOG_TRIVIAL(error) << "Unsupported HTTP method: " << req.method_string();
        http::response<http::string_body> res{http::status::bad_request, req.version()};
        res.set(http::field::server, "Beast");
        res.set(http::field::content_type, "text/plain");
        res.body() = "Unsupported HTTP method";
        res.prepare_payload();
        return res;
    }
}

http::response<http::string_body> Operations::create_directory_listing(const std::string& dir_path, const http::request<http::string_body>& req) {
    std::string body;
    std::string base_path{req.target().data(), req.target().size()};
    
    if (base_path.back() != '/') {
        base_path += '/';
    }

    for (const auto& entry : fs::directory_iterator(dir_path)) {
        std::string filename = entry.path().filename().string();
        std::string link = base_path + filename;

        body += "<a href=\"" + link + "\">" + filename + "</a><br>\n";
    }

    http::response<http::string_body> res{http::status::ok, req.version()};
    res.set(http::field::server, "Beast");
    res.set(http::field::content_type, "text/html");
    res.body() = "<html><body>" + body + "</body></html>";
    res.prepare_payload();
    return res;
}

std::vector<std::string> Operations::list_directory_contents(const std::string& dir_path) {
    std::vector<std::string> filenames;
    try {
        for (const auto& entry : fs::directory_iterator(dir_path)) {
            filenames.push_back(entry.path().filename().string());
        }
    } catch (const fs::filesystem_error& e) {
        BOOST_LOG_TRIVIAL(error) << "Filesystem error: " << e.what();
    }
    return filenames;
}


std::string Operations::get_content_type(const std::string& path) {
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
}

}
