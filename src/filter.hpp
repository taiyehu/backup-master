#pragma once

#include "archive_internal.hpp"

#include <string>
#include <vector>

namespace backup::internal {

[[nodiscard]] bool glob_match(const std::string& pattern,
                              const std::string& value);
[[nodiscard]] bool is_explicitly_excluded(
    const std::string& path, const std::vector<std::string>& patterns);
[[nodiscard]] bool matches_filter(
    const SourceEntry& entry, EntryType source_type,
    const BackupFilter& filter,
    const std::vector<std::string>& canonical_owners);

}  // namespace backup::internal
