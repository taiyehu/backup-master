#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

namespace backup::special_files {

struct FileIdentity {
    std::uint64_t volume{};
    std::array<std::uint8_t, 16> id{};
    std::uint32_t link_count{};

    friend bool operator<(const FileIdentity& left,
                          const FileIdentity& right) noexcept {
        if (left.volume != right.volume) {
            return left.volume < right.volume;
        }
        return left.id < right.id;
    }
};

[[nodiscard]] bool is_junction(const std::filesystem::path& path,
                               std::error_code& error);
[[nodiscard]] std::filesystem::path inspect_junction(
    const std::filesystem::path& path, std::error_code& error);
void create_junction(const std::filesystem::path& target,
                     const std::filesystem::path& link,
                     std::error_code& error);

[[nodiscard]] FileIdentity file_identity(const std::filesystem::path& path,
                                         std::error_code& error);
void create_hardlink(const std::filesystem::path& existing,
                     const std::filesystem::path& link,
                     std::error_code& error);

}  // namespace backup::special_files
