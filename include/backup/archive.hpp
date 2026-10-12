#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace backup {

enum class EntryType : std::uint8_t {
    file = 1,
    directory = 2,
    file_symlink = 3,
    directory_symlink = 4
};

[[nodiscard]] constexpr bool is_directory(EntryType type) noexcept {
    return type == EntryType::directory;
}

[[nodiscard]] constexpr bool is_symlink(EntryType type) noexcept {
    return type == EntryType::file_symlink ||
           type == EntryType::directory_symlink;
}

struct EntryInfo {
    std::string path;
    EntryType type{};
    std::uint64_t size{};
    std::uint64_t checksum{};
    std::string link_target;
};

class ArchiveService {
public:
    std::filesystem::path backup(
        const std::filesystem::path& source,
        const std::filesystem::path& archive = {}) const;

    void restore(const std::filesystem::path& archive,
                 const std::filesystem::path& destination) const;

    [[nodiscard]] std::vector<EntryInfo>
    list(const std::filesystem::path& archive) const;

    void verify(const std::filesystem::path& archive) const;
};

}  // namespace backup
