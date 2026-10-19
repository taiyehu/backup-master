#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace backup {

enum class EntryType : std::uint8_t {
    file = 1,
    directory = 2,
    file_symlink = 3,
    directory_symlink = 4,
    junction = 5,
    hardlink = 6
};

[[nodiscard]] constexpr bool is_directory(EntryType type) noexcept {
    return type == EntryType::directory;
}

[[nodiscard]] constexpr bool is_symlink(EntryType type) noexcept {
    return type == EntryType::file_symlink ||
           type == EntryType::directory_symlink;
}

[[nodiscard]] constexpr bool is_junction(EntryType type) noexcept {
    return type == EntryType::junction;
}

[[nodiscard]] constexpr bool is_hardlink(EntryType type) noexcept {
    return type == EntryType::hardlink;
}

[[nodiscard]] constexpr bool has_reference_target(EntryType type) noexcept {
    return is_symlink(type) || is_junction(type) || is_hardlink(type);
}

[[nodiscard]] constexpr bool is_directory_like(EntryType type) noexcept {
    return is_directory(type) || type == EntryType::directory_symlink ||
           is_junction(type);
}

enum class MetadataPlatform : std::uint8_t { none = 0, windows = 1 };

struct WindowsMetadata {
    std::int64_t creation_time{};
    std::int64_t access_time{};
    std::int64_t write_time{};
    std::int64_t change_time{};
    std::uint32_t attributes{};
    std::string owner_sid;
    std::vector<std::uint8_t> security_descriptor;
};

struct FileMetadata {
    MetadataPlatform platform{MetadataPlatform::none};
    std::optional<WindowsMetadata> windows;
};

struct TimeRange {
    std::optional<std::int64_t> after;
    std::optional<std::int64_t> before;
};

struct BackupFilter {
    std::vector<std::string> include_paths;
    std::vector<std::string> exclude_paths;
    std::vector<EntryType> types;
    std::vector<std::string> names;
    TimeRange creation_time;
    TimeRange modification_time;
    TimeRange access_time;
    std::vector<std::string> owners;
    std::optional<std::uint64_t> minimum_size;
    std::optional<std::uint64_t> maximum_size;
};

enum class JunctionPolicy { archive_internal_only };

struct BackupOptions {
    std::filesystem::path archive;
    BackupFilter filter;
    bool preserve_metadata{true};
    JunctionPolicy junction_policy{JunctionPolicy::archive_internal_only};
};

enum class MetadataRestoreMode { best_effort, strict, none };

struct RestoreOptions {
    MetadataRestoreMode metadata{MetadataRestoreMode::best_effort};
};

struct RestoreResult {
    std::vector<std::string> warnings;
};

struct EntryInfo {
    std::string path;
    EntryType type{};
    std::uint64_t size{};
    std::uint64_t checksum{};
    std::string link_target;
    FileMetadata metadata;
};

class ArchiveService {
public:
    std::filesystem::path backup(
        const std::filesystem::path& source,
        const std::filesystem::path& archive = {}) const;

    std::filesystem::path backup(const std::filesystem::path& source,
                                 const BackupOptions& options) const;

    RestoreResult restore(const std::filesystem::path& archive,
                          const std::filesystem::path& destination,
                          const RestoreOptions& options = {}) const;

    [[nodiscard]] std::vector<EntryInfo>
    list(const std::filesystem::path& archive) const;

    void verify(const std::filesystem::path& archive) const;
};

}  // namespace backup
