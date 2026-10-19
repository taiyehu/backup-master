#include "archive_internal.hpp"

#include <functional>
#include <map>

namespace backup::internal {
namespace {

std::string resolve_symlink_reference(const EntryInfo& entry) {
    if (entry.link_target.empty() ||
        entry.link_target.size() > kMaxReferenceBytes ||
        entry.link_target.find('\\') != std::string::npos ||
        entry.link_target.find('\0') != std::string::npos) {
        fail("archive contains an invalid symlink target: " + entry.path);
    }
    const auto target = fs::u8path(entry.link_target);
    if (target.is_absolute() || target.has_root_name() ||
        target.has_root_directory()) {
        fail("archive contains an absolute symlink target: " + entry.path);
    }
    const auto resolved =
        (fs::u8path(entry.path).parent_path() / target).lexically_normal();
    if (resolved.empty() || resolved == ".") {
        fail("archive symlink target does not name an entry: " + entry.path);
    }
    for (const auto& component : resolved) {
        if (component == ".." || component.empty()) {
            fail("archive symlink target escapes the archive: " + entry.path);
        }
    }
    return portable_path(resolved);
}

std::string referenced_path(const EntryInfo& entry) {
    if (is_symlink(entry.type)) {
        return resolve_symlink_reference(entry);
    }
    checked_relative_path(entry.link_target);
    return entry.link_target;
}

bool target_is_directory_like(EntryType type) {
    return type == EntryType::directory ||
           type == EntryType::directory_symlink ||
           type == EntryType::junction;
}

bool is_ancestor_or_same(const fs::path& ancestor, const fs::path& path) {
    auto ancestor_part = ancestor.begin();
    auto path_part = path.begin();
    for (; ancestor_part != ancestor.end(); ++ancestor_part, ++path_part) {
        if (path_part == path.end() || *ancestor_part != *path_part) {
            return false;
        }
    }
    return true;
}

}  // namespace

void validate_manifest(std::vector<EntryInfo>& entries) {
    std::map<std::string, std::size_t> indices;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (!indices.emplace(entries[i].path, i).second) {
            fail("archive contains a duplicate path: " + entries[i].path);
        }
    }

    std::vector<std::optional<std::size_t>> reference_edges(entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        auto& entry = entries[i];
        auto parent = fs::u8path(entry.path).parent_path();
        while (!parent.empty()) {
            const auto found = indices.find(portable_path(parent));
            if (found != indices.end() &&
                !is_directory(entries[found->second].type)) {
                fail("archive entry has a non-directory parent: " + entry.path);
            }
            parent = parent.parent_path();
        }
        if (!has_reference_target(entry.type)) {
            continue;
        }
        const auto target_path = referenced_path(entry);
        const auto target = indices.find(target_path);
        if (target == indices.end()) {
            fail("archive reference target does not exist: " + entry.path);
        }
        const auto target_type = entries[target->second].type;
        if (entry.type == EntryType::hardlink) {
            if (target_type != EntryType::file) {
                fail("hardlink must reference a regular file: " + entry.path);
            }
            entry.size = entries[target->second].size;
        } else if (entry.type == EntryType::junction) {
            if (target_type != EntryType::directory) {
                fail("junction must reference a real directory: " + entry.path);
            }
            if (is_ancestor_or_same(fs::u8path(target_path),
                                    fs::u8path(entry.path))) {
                fail("junction would create a directory cycle: " + entry.path);
            }
            reference_edges[i] = target->second;
        } else {
            const bool expects_directory =
                entry.type == EntryType::directory_symlink;
            if (expects_directory != target_is_directory_like(target_type)) {
                fail("symlink target type does not match: " + entry.path);
            }
            if (expects_directory &&
                is_ancestor_or_same(fs::u8path(target_path),
                                    fs::u8path(entry.path))) {
                fail("symlink would create a directory cycle: " + entry.path);
            }
            reference_edges[i] = target->second;
        }
    }

    enum class Visit : std::uint8_t { unseen, active, done };
    std::vector<Visit> visits(entries.size(), Visit::unseen);
    std::function<void(std::size_t)> visit = [&](std::size_t index) {
        if (visits[index] == Visit::active) {
            fail("archive contains a reference cycle: " + entries[index].path);
        }
        if (visits[index] == Visit::done) {
            return;
        }
        visits[index] = Visit::active;
        if (reference_edges[index]) {
            visit(*reference_edges[index]);
        }
        visits[index] = Visit::done;
    };
    for (std::size_t i = 0; i < entries.size(); ++i) {
        visit(i);
    }
}

}  // namespace backup::internal
