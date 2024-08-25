#include <data/file_system_traversal.h>

Directory File_System_Traversal::traverse(const fs::path& root_path) {
    if (!fs::exists(root_path) || !fs::is_directory(root_path)) {
        throw std::invalid_argument("Invalid root path: " + root_path.string());
    }
    
    Directory root_dir;
    root_dir.path = root_path;
    traverse_directory(root_path, root_dir);
    
    return root_dir;
}


void File_System_Traversal::traverse_directory(const fs::path& dir_path, Directory& dir) {
    dir.path = dir_path;

    try {
        for (const auto& entry : fs::directory_iterator(dir_path, fs::directory_options::skip_permission_denied)) {
            if (fs::is_regular_file(entry)) {
                auto last_write_time = decltype(fs::last_write_time(entry)){fs::last_write_time(entry)};
                uintmax_t file_size = fs::file_size(entry);
                dir.files.emplace_back(File{entry.path(), last_write_time, file_size});
            } else if (fs::is_directory(entry)) {
                Directory subdir;
                traverse_directory(entry.path(), subdir);
                dir.subdirectories.push_back(std::move(subdir));
            }
        }
    }
    catch (const fs::filesystem_error& ex) {
        std::cerr << "Error accessing directory: " << dir_path.string() << " (" << ex.what() << ")" << std::endl;
    }
}