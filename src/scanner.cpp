#include "archive_internal.hpp"

#include "backup/symlink.hpp"
#include "filter.hpp"
#include "metadata.hpp"
#include "special_files.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <set>

namespace backup::internal {
namespace {

struct Candidate {
    SourceEntry entry;
    EntryType source_type;
    std::optional<special_files::FileIdentity> identity;
};

bool path_escapes(const fs::path& path) {
    if (path.empty() || path == "." || path.is_absolute() ||
        path.has_root_name() || path.has_root_directory()) {
        return true;
    }
    return std::any_of(path.begin(), path.end(), [](const auto& component) {
        return component == ".." || component.empty();
    });
}

std::string resolve_symlink_target(const std::string& link_path,
                                   const std::string& target) {
    if (target.empty() || target.size() > kMaxReferenceBytes ||
        target.find('\\') != std::string::npos ||
        target.find('\0') != std::string::npos) {
        fail("invalid symlink target: " + link_path);
    }
    const auto target_path = fs::u8path(target);
    if (target_path.is_absolute() || target_path.has_root_name() ||
        target_path.has_root_directory()) {
        fail("symlink target must be relative: " + link_path);
    }
    const auto resolved =
        (fs::u8path(link_path).parent_path() / target_path).lexically_normal();
    if (path_escapes(resolved)) {
        fail("symlink target escapes the source tree: " + link_path);
    }
    return portable_path(resolved);
}

std::vector<std::string> canonicalize_owners(
    const std::vector<std::string>& owners) {
    std::vector<std::string> result;
    result.reserve(owners.size());
    for (const auto& owner : owners) {
        std::error_code error;
        auto canonical = metadata::canonical_owner(owner, error);
        if (error) {
            fail("cannot resolve owner '" + owner + "': " + error.message());
        }
        if (std::find(result.begin(), result.end(), canonical) == result.end()) {
            result.push_back(std::move(canonical));
        }
    }
    return result;
}

void add_ancestors(const std::string& path,
                   const std::map<std::string, std::size_t>& indices,
                   std::set<std::size_t>& selected) {
    auto parent = fs::u8path(path).parent_path();
    while (!parent.empty()) {
        const auto found = indices.find(portable_path(parent));
        if (found != indices.end()) {
            selected.insert(found->second);
        }
        parent = parent.parent_path();
    }
}

}  // namespace

std::vector<SourceEntry> scan_source(const fs::path& source,
                                     const BackupOptions& options) {
    std::error_code error;
    const fs::path root = fs::canonical(source, error);
    if (error || !fs::is_directory(root)) {
        fail("source is not a readable directory");
    }
    const auto canonical_owners = canonicalize_owners(options.filter.owners);

    std::vector<Candidate> candidates;
    fs::recursive_directory_iterator iterator(
        root, fs::directory_options::skip_permission_denied, error);
    const fs::recursive_directory_iterator end;
    if (error) {
        fail("cannot scan source directory: " + error.message());
    }

    while (iterator != end) {
        const auto path = iterator->path();
        const auto relative_path = path.lexically_relative(root);
        const auto relative = portable_path(relative_path);
        if (relative.empty() || relative.size() > kMaxPathBytes) {
            fail("cannot represent source path in archive");
        }
        if (is_explicitly_excluded(relative,
                                   options.filter.exclude_paths)) {
            iterator.disable_recursion_pending();
            iterator.increment(error);
            if (error) {
                fail("cannot continue scanning source directory: " +
                     error.message());
            }
            continue;
        }

        const auto status = iterator->symlink_status(error);
        if (error) {
            fail("cannot inspect source entry: " + error.message());
        }
        const bool junction = special_files::is_junction(path, error);
        if (error) {
            fail("cannot inspect source reparse point: " + relative + ": " +
                 error.message());
        }
        const bool symbolic_link = junction
                                       ? false
                                       : symlink::is_symbolic_link(path, error);
        if (error) {
            fail("unsupported source reparse point: " + relative + ": " +
                 error.message());
        }

        SourceEntry entry{path, relative, EntryType::file, 0, 0, {}, {}, {}};
        EntryType source_type = EntryType::file;
        std::optional<special_files::FileIdentity> identity;
        bool open_reparse = false;

        if (junction) {
            iterator.disable_recursion_pending();
            const auto target = special_files::inspect_junction(path, error);
            if (error) {
                fail("cannot inspect junction target: " + relative + ": " +
                     error.message());
            }
            const auto canonical_target = fs::canonical(target, error);
            if (error) {
                fail("cannot resolve junction target: " + relative + ": " +
                     error.message());
            }
            const auto target_relative = fs::relative(canonical_target, root, error);
            if (error || path_escapes(target_relative)) {
                fail("junction target is outside the source tree: " + relative);
            }
            entry.type = EntryType::junction;
            source_type = entry.type;
            entry.reference_target = portable_path(target_relative);
            entry.data_size = entry.reference_target.size();
            open_reparse = true;
        } else if (symbolic_link) {
            iterator.disable_recursion_pending();
            const auto info = symlink::inspect(path, error);
            if (error) {
                fail("cannot inspect symlink target: " + relative + ": " +
                     error.message());
            }
            entry.type = info.kind == symlink::Kind::directory
                             ? EntryType::directory_symlink
                             : EntryType::file_symlink;
            source_type = entry.type;
            entry.reference_target = portable_path(info.target);
            entry.data_size = entry.reference_target.size();
            open_reparse = true;
        } else if (fs::is_directory(status)) {
            entry.type = EntryType::directory;
            source_type = entry.type;
        } else if (fs::is_regular_file(status)) {
            entry.type = EntryType::file;
            entry.data_size = fs::file_size(path, error);
            entry.logical_size = entry.data_size;
            if (error) {
                fail("cannot read file size: " + relative + ": " +
                     error.message());
            }
            identity = special_files::file_identity(path, error);
            if (error) {
                fail("cannot identify source file: " + relative + ": " +
                     error.message());
            }
            if (identity->link_count > 1) {
                source_type = EntryType::hardlink;
            }
        } else {
            fail("unsupported special file: " + relative);
        }

        entry.metadata = metadata::capture(path, open_reparse, error);
        if (error) {
            fail("cannot capture metadata: " + relative + ": " +
                 error.message());
        }
        candidates.push_back({std::move(entry), source_type, identity});
        iterator.increment(error);
        if (error) {
            fail("cannot continue scanning source directory: " +
                 error.message());
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](const auto& left,
                                                        const auto& right) {
        return left.entry.relative < right.entry.relative;
    });
    std::map<std::string, std::size_t> indices;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        indices.emplace(candidates[i].entry.relative, i);
    }

    std::set<std::size_t> selected;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (matches_filter(candidates[i].entry, candidates[i].source_type,
                           options.filter, canonical_owners)) {
            selected.insert(i);
        }
    }
    if (selected.empty()) {
        fail("no source entries matched the backup filter");
    }

    std::vector<std::size_t> work(selected.begin(), selected.end());
    for (std::size_t cursor = 0; cursor < work.size(); ++cursor) {
        const auto index = work[cursor];
        const auto& entry = candidates[index].entry;
        add_ancestors(entry.relative, indices, selected);
        if (!is_symlink(entry.type) && !is_junction(entry.type)) {
            continue;
        }
        const auto referenced = is_symlink(entry.type)
                                    ? resolve_symlink_target(
                                          entry.relative,
                                          entry.reference_target)
                                    : entry.reference_target;
        if (is_explicitly_excluded(referenced,
                                   options.filter.exclude_paths)) {
            fail("selected link target is explicitly excluded: " +
                 entry.relative);
        }
        const auto found = indices.find(referenced);
        if (found == indices.end()) {
            fail("selected link target is not an archived entry: " +
                 entry.relative);
        }
        if (selected.insert(found->second).second) {
            work.push_back(found->second);
        }
    }
    work.assign(selected.begin(), selected.end());
    for (const auto index : work) {
        add_ancestors(candidates[index].entry.relative, indices, selected);
    }

    std::map<special_files::FileIdentity, std::vector<std::size_t>> hardlinks;
    for (const auto index : selected) {
        if (candidates[index].source_type == EntryType::hardlink &&
            candidates[index].identity) {
            hardlinks[*candidates[index].identity].push_back(index);
        }
    }
    for (auto& group : hardlinks) {
        auto& members = group.second;
        if (members.size() < 2) {
            continue;
        }
        const auto canonical = members.front();
        for (std::size_t i = 1; i < members.size(); ++i) {
            auto& entry = candidates[members[i]].entry;
            entry.type = EntryType::hardlink;
            entry.reference_target = candidates[canonical].entry.relative;
            entry.data_size = entry.reference_target.size();
        }
    }

    std::vector<SourceEntry> entries;
    entries.reserve(selected.size());
    for (const auto index : selected) {
        auto entry = std::move(candidates[index].entry);
        if (!options.preserve_metadata) {
            entry.metadata = {};
        }
        entry.encoded_metadata = encode_metadata(entry.metadata);
        entries.push_back(std::move(entry));
    }
    std::vector<EntryInfo> manifest;
    manifest.reserve(entries.size());
    for (const auto& entry : entries) {
        const auto public_size = entry.type == EntryType::file
                                     ? entry.logical_size
                                     : entry.data_size;
        manifest.push_back({entry.relative, entry.type, public_size, 0,
                            entry.reference_target, entry.metadata});
    }
    validate_manifest(manifest);
    return entries;
}

}  // namespace backup::internal
