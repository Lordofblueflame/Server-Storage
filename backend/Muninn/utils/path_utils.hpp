#ifndef MUNINN_PATH_UTILS_HPP
#define MUNINN_PATH_UTILS_HPP

#include <filesystem>
#include <string>
#include <string_view>

namespace muninn::utils {

inline std::string normalize_path_lexical(const std::string_view path) {
    return std::filesystem::path(path).lexically_normal().generic_string();
}

inline std::string filename_from_path(const std::string_view path) {
    return std::filesystem::path(path).filename().generic_string();
}

inline std::string parent_from_path(const std::string_view path) {
    return std::filesystem::path(path).parent_path().generic_string();
}

inline bool path_is_absolute(const std::string_view path) {
    return std::filesystem::path(path).is_absolute();
}

} // namespace muninn::utils

#endif // MUNINN_PATH_UTILS_HPP
