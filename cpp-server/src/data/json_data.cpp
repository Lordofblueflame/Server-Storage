#include "pch.h"
#include <data/json_data.h>
#include <data/json_structs.h>

Json_data::Json_data() {
    try {
        if (fs::exists(root_path) && fs::is_directory(root_path)) {
            traverse_directory(root_path, root_dir);

            pt::ptree root_ptree;
            to_ptree(root_dir, root_ptree);

            std::ofstream ofs(json_file_path);
            if (!ofs) {
                std::cerr << "Error opening file for writing: " << json_file_path << std::endl;
                return;
            }
            pt::write_json(ofs, root_ptree);
        } else {
            std::cerr << "Invalid path: " << root_path << std::endl;
        }
    } catch (const fs::filesystem_error& ex) {
        std::cerr << "Filesystem error: " << ex.what() << std::endl;
    }
}

void Json_data::traverse_directory(const fs::path& dir_path, Directory& dir) {
    dir.path = dir_path;

    try {
        for (fs::directory_iterator it(dir_path, fs::directory_options::skip_permission_denied), end; it != end; ++it) {
            const auto& entry = *it;
            if (fs::is_regular_file(entry.path())) {
                std::time_t last_write_time = fs::last_write_time(entry.path());
                uintmax_t file_size = fs::file_size(entry.path());
                dir.files.push_back(File{entry.path(), last_write_time, file_size});
            } else if (fs::is_directory(entry.path())) {
                Directory subdir;
                traverse_directory(entry.path(), subdir);
                dir.subdirectories.push_back(subdir);
            }
        }
    } catch (const fs::filesystem_error& ex) {
        std::cerr << "Error accessing directory: " << dir_path.string() << " (" << ex.what() << ")" << std::endl;
    }
}

void Json_data::to_ptree(const File& file, pt::ptree& pt) {
    pt.put("path", file.path.string());
    pt.put("file_size", file.file_size);

    std::time_t t = file.last_write_time;
    std::tm tm = *std::localtime(&t);
    std::stringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    pt.put("last_write_time", ss.str());
}

void Json_data::to_ptree(const Directory& dir, pt::ptree& pt) {
    pt.put("path", dir.path.string());

    pt::ptree files;
    for (const auto& file : dir.files) {
        pt::ptree file_node;
        to_ptree(file, file_node);
        files.push_back(std::make_pair("", file_node));
    }
    pt.add_child("files", files);

    pt::ptree subdirectories;
    for (const auto& subdir : dir.subdirectories) {
        pt::ptree subdir_node;
        to_ptree(subdir, subdir_node);
        subdirectories.push_back(std::make_pair(subdir.path.filename().string(), subdir_node));
    }
    pt.add_child("subdirectories", subdirectories);
}