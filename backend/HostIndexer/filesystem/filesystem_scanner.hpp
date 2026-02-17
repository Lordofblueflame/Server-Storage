#ifndef HOSTINDEXER_FILESYSTEM_SCANNER_HPP
#define HOSTINDEXER_FILESYSTEM_SCANNER_HPP

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "../domain/model/models.hpp"

namespace host_indexer::filesystem {

enum class ScanSeverity : std::uint8_t {
    Info = 0,
    Warning = 1,
    Error = 2,
    Fatal = 3
};

enum class ScanErrorCode : std::uint16_t {
    None = 0,
    RootPathMissing = 1,
    RootPathNotDirectory = 2,
    RootPathResolveFailure = 3,
    TraversalFailure = 4,
    MetadataUnavailable = 5,
    EntryLimitExceeded = 6
};

struct ScanDiagnostic {
    ScanErrorCode code {ScanErrorCode::None};
    ScanSeverity severity {ScanSeverity::Info};
    std::string message {};
    std::string path {};
};

struct RawEntry {
    std::string normalized_path {};
    std::string parent_path {};
    std::string name {};
    domain::EntryType type {domain::EntryType::File};
    domain::FileMetadata metadata {};
};

struct RawScanResult {
    std::string host_identifier {};
    std::string root_path {};
    domain::UnixTimestamp scanned_at {0};
    std::vector<RawEntry> entries {};
    std::vector<ScanDiagnostic> diagnostics {};
};

struct ScanOptions {
    bool follow_symlinks {false};
    std::size_t max_entries {5'000'000};
};

class IFilesystemScanner {
public:
    virtual ~IFilesystemScanner() = default;
    [[nodiscard]] virtual RawScanResult scan(const std::filesystem::path& root_path,
                                             const ScanOptions& options) const = 0;
};

class LocalFilesystemScanner final : public IFilesystemScanner {
public:
    [[nodiscard]] RawScanResult scan(const std::filesystem::path& root_path,
                                     const ScanOptions& options) const override;
};

} // namespace host_indexer::filesystem

#endif // HOSTINDEXER_FILESYSTEM_SCANNER_HPP
