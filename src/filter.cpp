#include "filter.hpp"

#include <algorithm>

namespace backup::internal {
namespace {

bool glob_match_at(const std::string& pattern, std::size_t pattern_index,
                   const std::string& value, std::size_t value_index,
                   std::vector<std::vector<std::int8_t>>& memo) {
    auto& cached = memo[pattern_index][value_index];
    if (cached != -1) {
        return cached != 0;
    }
    bool matched = false;
    if (pattern_index == pattern.size()) {
        matched = value_index == value.size();
    } else if (pattern[pattern_index] == '*') {
        const bool double_star = pattern_index + 1 < pattern.size() &&
                                 pattern[pattern_index + 1] == '*';
        const auto next = pattern_index + (double_star ? 2U : 1U);
        matched = glob_match_at(pattern, next, value, value_index, memo);
        if (!matched && double_star && next < pattern.size() &&
            pattern[next] == '/') {
            matched = glob_match_at(pattern, next + 1, value, value_index,
                                    memo);
        }
        if (!matched && value_index < value.size() &&
            (double_star || value[value_index] != '/')) {
            matched = glob_match_at(pattern, pattern_index, value,
                                    value_index + 1, memo);
        }
    } else if (value_index < value.size() &&
               (pattern[pattern_index] == '?' && value[value_index] != '/')) {
        matched = glob_match_at(pattern, pattern_index + 1, value,
                                value_index + 1, memo);
    } else if (value_index < value.size() &&
               pattern[pattern_index] == value[value_index]) {
        matched = glob_match_at(pattern, pattern_index + 1, value,
                                value_index + 1, memo);
    }
    cached = matched ? 1 : 0;
    return matched;
}

bool matches_any(const std::vector<std::string>& patterns,
                 const std::string& value) {
    return patterns.empty() ||
           std::any_of(patterns.begin(), patterns.end(),
                       [&](const auto& pattern) {
                           return glob_match(pattern, value);
                       });
}

bool in_range(std::int64_t value, const TimeRange& range) {
    return (!range.after || value >= *range.after) &&
           (!range.before || value < *range.before);
}

bool has_time_filter(const TimeRange& range) {
    return range.after.has_value() || range.before.has_value();
}

}  // namespace

bool glob_match(const std::string& pattern, const std::string& value) {
    std::vector<std::vector<std::int8_t>> memo(
        pattern.size() + 1,
        std::vector<std::int8_t>(value.size() + 1, -1));
    return glob_match_at(pattern, 0, value, 0, memo);
}

bool is_explicitly_excluded(const std::string& path,
                            const std::vector<std::string>& patterns) {
    return std::any_of(patterns.begin(), patterns.end(),
                       [&](const auto& pattern) {
                           return glob_match(pattern, path);
                       });
}

bool matches_filter(const SourceEntry& entry, EntryType source_type,
                    const BackupFilter& filter,
                    const std::vector<std::string>& canonical_owners) {
    if (!matches_any(filter.include_paths, entry.relative) ||
        is_explicitly_excluded(entry.relative, filter.exclude_paths)) {
        return false;
    }
    if (!filter.types.empty() &&
        std::find(filter.types.begin(), filter.types.end(), source_type) ==
            filter.types.end()) {
        return false;
    }
    const auto name = portable_path(fs::u8path(entry.relative).filename());
    if (!matches_any(filter.names, name)) {
        return false;
    }

    const bool content_type = source_type == EntryType::file ||
                              source_type == EntryType::hardlink;
    if (filter.minimum_size || filter.maximum_size) {
        if (!content_type ||
            (filter.minimum_size &&
             entry.logical_size < *filter.minimum_size) ||
            (filter.maximum_size &&
             entry.logical_size > *filter.maximum_size)) {
            return false;
        }
    }

    if (has_time_filter(filter.creation_time) ||
        has_time_filter(filter.modification_time) ||
        has_time_filter(filter.access_time) || !canonical_owners.empty()) {
        if (entry.metadata.platform != MetadataPlatform::windows ||
            !entry.metadata.windows) {
            return false;
        }
        const auto& windows = *entry.metadata.windows;
        if (!in_range(windows.creation_time, filter.creation_time) ||
            !in_range(windows.write_time, filter.modification_time) ||
            !in_range(windows.access_time, filter.access_time)) {
            return false;
        }
        if (!canonical_owners.empty() &&
            std::find(canonical_owners.begin(), canonical_owners.end(),
                      windows.owner_sid) == canonical_owners.end()) {
            return false;
        }
    }
    return true;
}

}  // namespace backup::internal
