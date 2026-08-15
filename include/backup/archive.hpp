#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace backup {

struct EntryInfo {
    std::string path;
    bool is_directory{};
    std::uint64_t size{};
    std::uint64_t checksum{};
};

class ArchiveService {
public:
    void backup(const std::filesystem::path& source,
                const std::filesystem::path& archive) const;

    void restore(const std::filesystem::path& archive,
                 const std::filesystem::path& destination) const;

    [[nodiscard]] std::vector<EntryInfo>
    list(const std::filesystem::path& archive) const;

    void verify(const std::filesystem::path& archive) const;
};

}  // namespace backup

