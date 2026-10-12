#pragma once

#include <filesystem>
#include <system_error>

namespace backup::symlink {

enum class Kind {
    file,
    directory
};

struct Info {
    std::filesystem::path target;
    Kind kind;
};

[[nodiscard]] bool is_symbolic_link(const std::filesystem::path& path,
                                    std::error_code& error);

[[nodiscard]] Info inspect(const std::filesystem::path& path,
                           std::error_code& error);

void create(const std::filesystem::path& target,
            const std::filesystem::path& link,
            Kind kind,
            std::error_code& error);

}  // namespace backup::symlink
