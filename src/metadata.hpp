#pragma once

#include "backup/archive.hpp"

#include <filesystem>
#include <string>
#include <system_error>

namespace backup::metadata {

[[nodiscard]] FileMetadata capture(const std::filesystem::path& path,
                                   bool open_reparse_point,
                                   std::error_code& error);

void apply(const std::filesystem::path& path, const FileMetadata& metadata,
           bool open_reparse_point, std::error_code& error);

[[nodiscard]] std::string canonical_owner(const std::string& owner,
                                          std::error_code& error);

[[nodiscard]] std::string format_utc(std::int64_t timestamp);

}  // namespace backup::metadata
