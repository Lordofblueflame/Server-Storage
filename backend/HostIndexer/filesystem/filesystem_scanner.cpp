#include "filesystem_scanner.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <system_error>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace host_indexer::filesystem {
namespace {

domain::UnixTimestamp unix_now() {
    return static_cast<domain::UnixTimestamp>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
}

domain::UnixTimestamp to_unix_timestamp(const std::filesystem::file_time_type file_time) {
    const auto system_time = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        file_time - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now()
    );
    return static_cast<domain::UnixTimestamp>(
        std::chrono::duration_cast<std::chrono::seconds>(system_time.time_since_epoch()).count()
    );
}

std::string detect_host_identifier() {
#if defined(__unix__) || defined(__APPLE__)
    std::array<char, 256> buffer {};
    if (::gethostname(buffer.data(), buffer.size()) == 0) {
        buffer.back() = '\0';
        return std::string(buffer.data());
    }
#endif

    if (const char* hostname = std::getenv("HOSTNAME"); hostname != nullptr) {
        return std::string(hostname);
    }
    if (const char* computer_name = std::getenv("COMPUTERNAME"); computer_name != nullptr) {
        return std::string(computer_name);
    }
    return "unknown-host";
}

std::string normalize_absolute_path(const std::filesystem::path& path, std::error_code& ec) {
    const auto absolute = std::filesystem::absolute(path, ec);
    if (ec) {
        return {};
    }
    return absolute.lexically_normal().generic_string();
}

domain::EntryType map_entry_type(const std::filesystem::file_status status) {
    const auto type = status.type();
    switch (type) {
        case std::filesystem::file_type::directory:
            return domain::EntryType::Directory;
        case std::filesystem::file_type::symlink:
            return domain::EntryType::Symlink;
        case std::filesystem::file_type::regular:
            return domain::EntryType::File;
        default:
            return domain::EntryType::Special;
    }
}

bool try_fill_posix_metadata(const std::filesystem::path& path, domain::FileMetadata& metadata) {
#if defined(__unix__) || defined(__APPLE__)
    struct stat stat_buffer {};
    if (::lstat(path.c_str(), &stat_buffer) != 0) {
        return false;
    }

    metadata.file_id = static_cast<std::uint64_t>(stat_buffer.st_ino);
    metadata.device_id = static_cast<std::uint64_t>(stat_buffer.st_dev);
    metadata.created_at = static_cast<domain::UnixTimestamp>(stat_buffer.st_ctime);
    metadata.modified_at = static_cast<domain::UnixTimestamp>(stat_buffer.st_mtime);
    metadata.accessed_at = static_cast<domain::UnixTimestamp>(stat_buffer.st_atime);
    metadata.permissions = static_cast<std::uint32_t>(stat_buffer.st_mode & 0x0FFFU);
    return true;
#else
    (void)path;
    (void)metadata;
    return false;
#endif
}

void append_diagnostic(RawScanResult& result,
                       const ScanErrorCode code,
                       const ScanSeverity severity,
                       const std::string_view message,
                       const std::string_view path = {}) {
    result.diagnostics.push_back(
        ScanDiagnostic {
            code,
            severity,
            std::string(message),
            std::string(path)
        }
    );
}

void fill_entry_metadata(const std::filesystem::path& path,
                         const std::filesystem::file_status status,
                         RawEntry& entry,
                         RawScanResult& result) {
    entry.type = map_entry_type(status);
    entry.metadata.permissions = static_cast<std::uint32_t>(status.permissions());

    std::error_code ec;
    if (entry.type == domain::EntryType::File) {
        const auto file_size = std::filesystem::file_size(path, ec);
        if (!ec) {
            entry.metadata.size_bytes = file_size;
        } else {
            append_diagnostic(
                result,
                ScanErrorCode::MetadataUnavailable,
                ScanSeverity::Warning,
                "Failed to read file size.",
                path.generic_string()
            );
        }
    }

    ec.clear();
    const auto last_write = std::filesystem::last_write_time(path, ec);
    if (!ec) {
        const auto modified = to_unix_timestamp(last_write);
        entry.metadata.created_at = modified;
        entry.metadata.modified_at = modified;
        entry.metadata.accessed_at = modified;
    }

    (void)try_fill_posix_metadata(path, entry.metadata);
}

bool collect_one_entry(const std::filesystem::path& path,
                       const bool is_root,
                       RawScanResult& result,
                       const std::size_t max_entries) {
    if (result.entries.size() >= max_entries) {
        append_diagnostic(
            result,
            ScanErrorCode::EntryLimitExceeded,
            ScanSeverity::Error,
            "Scanner entry limit exceeded."
        );
        return false;
    }

    std::error_code ec;
    const auto normalized_path = normalize_absolute_path(path, ec);
    if (ec) {
        append_diagnostic(
            result,
            ScanErrorCode::RootPathResolveFailure,
            ScanSeverity::Error,
            "Failed to normalize path.",
            path.generic_string()
        );
        return false;
    }

    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec) {
        append_diagnostic(
            result,
            ScanErrorCode::MetadataUnavailable,
            ScanSeverity::Warning,
            "Failed to read entry status.",
            normalized_path
        );
        return false;
    }

    RawEntry entry;
    entry.normalized_path = normalized_path;
    entry.parent_path = is_root ? std::string() : std::filesystem::path(normalized_path).parent_path().generic_string();
    entry.name = std::filesystem::path(normalized_path).filename().generic_string();
    fill_entry_metadata(path, status, entry, result);
    result.entries.push_back(std::move(entry));
    return true;
}

} // namespace

RawScanResult LocalFilesystemScanner::scan(const std::filesystem::path& root_path,
                                           const ScanOptions& options) const {
    RawScanResult result;
    result.host_identifier = detect_host_identifier();
    result.scanned_at = unix_now();

    std::error_code ec;
    if (!std::filesystem::exists(root_path, ec)) {
        append_diagnostic(
            result,
            ScanErrorCode::RootPathMissing,
            ScanSeverity::Fatal,
            "Root path does not exist.",
            root_path.generic_string()
        );
        return result;
    }

    if (!std::filesystem::is_directory(root_path, ec)) {
        append_diagnostic(
            result,
            ScanErrorCode::RootPathNotDirectory,
            ScanSeverity::Fatal,
            "Root path is not a directory.",
            root_path.generic_string()
        );
        return result;
    }

    result.root_path = normalize_absolute_path(root_path, ec);
    if (ec || result.root_path.empty()) {
        append_diagnostic(
            result,
            ScanErrorCode::RootPathResolveFailure,
            ScanSeverity::Fatal,
            "Failed to normalize root path.",
            root_path.generic_string()
        );
        return result;
    }

    if (!collect_one_entry(root_path, true, result, options.max_entries)) {
        return result;
    }

    auto directory_options = std::filesystem::directory_options::skip_permission_denied;
    if (options.follow_symlinks) {
        directory_options |= std::filesystem::directory_options::follow_directory_symlink;
    }

    std::filesystem::recursive_directory_iterator iterator(root_path, directory_options, ec);
    std::filesystem::recursive_directory_iterator end;

    for (; iterator != end; iterator.increment(ec)) {
        if (ec) {
            append_diagnostic(
                result,
                ScanErrorCode::TraversalFailure,
                ScanSeverity::Warning,
                "Traversal encountered an error and skipped an entry.",
                root_path.generic_string()
            );
            ec.clear();
            continue;
        }

        if (!collect_one_entry(iterator->path(), false, result, options.max_entries)) {
            break;
        }
    }

    std::sort(
        result.entries.begin(),
        result.entries.end(),
        [](const RawEntry& lhs, const RawEntry& rhs) {
            if (lhs.normalized_path == rhs.normalized_path) {
                return lhs.name < rhs.name;
            }
            return lhs.normalized_path < rhs.normalized_path;
        }
    );

    return result;
}

} // namespace host_indexer::filesystem
