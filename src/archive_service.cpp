#include "backup/archive.hpp"

#include "archive_internal.hpp"
#include "backup/symlink.hpp"
#include "metadata.hpp"
#include "special_files.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>

namespace backup {
namespace {

namespace fs = std::filesystem;
using namespace internal;

const fs::path kDefaultBackupDirectory = "backup";

std::uint64_t checksum_file(const fs::path& path, std::uint64_t seed) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        fail("cannot open source file: " + portable_path(path));
    }
    std::array<char, kBufferSize> buffer{};
    std::uint64_t checksum = seed;
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        checksum = update_checksum(checksum, buffer.data(),
                                   static_cast<std::size_t>(count));
    }
    if (!input.eof()) {
        fail("failed while reading source file: " + portable_path(path));
    }
    return checksum;
}

std::string current_time_string() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm local_time{};
#ifdef _WIN32
    localtime_s(&local_time, &time);
#else
    localtime_r(&time, &local_time);
#endif
    std::ostringstream output;
    output << std::put_time(&local_time, "%Y%m%d-%H%M%S");
    return output.str();
}

fs::path make_default_archive_path() {
    const std::string stem = "backup-" + current_time_string();
    fs::path candidate = kDefaultBackupDirectory / (stem + ".bkm");
    for (std::uint32_t suffix = 1; fs::exists(candidate); ++suffix) {
        candidate = kDefaultBackupDirectory /
                    (stem + "-" + std::to_string(suffix) + ".bkm");
    }
    return candidate;
}

std::vector<EntryInfo> read_manifest(const fs::path& archive,
                                     bool verify_file_data) {
    auto input = open_archive(archive);
    const auto header = read_header(input);
    std::vector<EntryInfo> entries;
    entries.reserve(static_cast<std::size_t>(header.entry_count));
    for (std::uint64_t i = 0; i < header.entry_count; ++i) {
        auto entry = read_entry_header(input, header.version);
        std::uint64_t checksum = checksum_bytes(entry.encoded_metadata);
        if (entry.info.type == EntryType::file) {
            if (verify_file_data) {
                copy_exact(input, nullptr, entry.data_size, checksum);
                if (checksum != entry.info.checksum) {
                    fail("checksum mismatch: " + entry.info.path);
                }
            } else {
                if (entry.data_size > static_cast<std::uint64_t>(
                                          std::numeric_limits<
                                              std::streamoff>::max())) {
                    fail("archive entry is too large for this platform");
                }
                input.seekg(static_cast<std::streamoff>(entry.data_size),
                            std::ios::cur);
                if (!input) {
                    fail("truncated archive while skipping file data");
                }
            }
        } else if (has_reference_target(entry.info.type)) {
            entry.info.link_target =
                read_reference(input, entry, checksum, true);
        } else if (checksum != entry.info.checksum) {
            fail("checksum mismatch: " + entry.info.path);
        }
        entries.push_back(std::move(entry.info));
    }
    ensure_archive_finished(input);
    validate_manifest(entries);
    return entries;
}

class RestoreGuard {
public:
    RestoreGuard(fs::path destination, bool existed)
        : destination_(std::move(destination)), existed_(existed) {}

    ~RestoreGuard() {
        if (committed_) {
            return;
        }
        std::error_code ignored;
        if (!existed_) {
            fs::remove_all(destination_, ignored);
            return;
        }
        fs::directory_iterator iterator(destination_, ignored);
        const fs::directory_iterator end;
        while (!ignored && iterator != end) {
            fs::remove_all(iterator->path(), ignored);
            iterator.increment(ignored);
        }
    }

    void commit() noexcept { committed_ = true; }

private:
    fs::path destination_;
    bool existed_{};
    bool committed_{};
};

std::size_t path_depth(const std::string& path) {
    const auto parsed = fs::u8path(path);
    return static_cast<std::size_t>(
        std::distance(parsed.begin(), parsed.end()));
}

}  // namespace

fs::path ArchiveService::backup(const fs::path& source,
                                const fs::path& archive) const {
    BackupOptions options;
    options.archive = archive;
    return backup(source, options);
}

fs::path ArchiveService::backup(const fs::path& source,
                                const BackupOptions& options) const {
    const auto entries = scan_source(source, options);
    const fs::path archive = options.archive.empty()
                                 ? make_default_archive_path()
                                 : options.archive;
    if (fs::exists(archive)) {
        fail("archive already exists; choose a new path or remove it explicitly");
    }
    if (archive.has_parent_path()) {
        std::error_code error;
        fs::create_directories(archive.parent_path(), error);
        if (error) {
            fail("cannot create archive directory: " + error.message());
        }
    }

    std::ofstream output(archive, std::ios::binary | std::ios::trunc);
    if (!output) {
        fail("cannot create archive: " + portable_path(archive));
    }
    try {
        write_header(output, entries.size());
        for (const auto& entry : entries) {
            const auto metadata_checksum =
                checksum_bytes(entry.encoded_metadata);
            const auto checksum = entry.type == EntryType::file
                                      ? checksum_file(entry.absolute,
                                                      metadata_checksum)
                                      : (has_reference_target(entry.type)
                                             ? checksum_bytes(
                                                   entry.reference_target,
                                                   metadata_checksum)
                                             : metadata_checksum);
            write_entry_header(output, entry, checksum);
            if (entry.type == EntryType::file) {
                std::ifstream input(entry.absolute, std::ios::binary);
                if (!input) {
                    fail("cannot open source file: " +
                         portable_path(entry.absolute));
                }
                std::uint64_t copied_checksum = metadata_checksum;
                copy_exact(input, &output, entry.data_size, copied_checksum);
                if (copied_checksum != checksum ||
                    input.peek() != std::char_traits<char>::eof()) {
                    fail("source file changed during backup: " +
                         portable_path(entry.absolute));
                }
            } else if (has_reference_target(entry.type)) {
                output.write(entry.reference_target.data(),
                             static_cast<std::streamsize>(
                                 entry.reference_target.size()));
                if (!output) {
                    fail("failed to write reference target: " + entry.relative);
                }
            }
        }
        output.close();
        if (!output) {
            fail("failed to finalize archive");
        }
    } catch (...) {
        output.close();
        std::error_code ignored;
        fs::remove(archive, ignored);
        throw;
    }
    return archive;
}

std::vector<EntryInfo> ArchiveService::list(const fs::path& archive) const {
    return read_manifest(archive, false);
}

void ArchiveService::verify(const fs::path& archive) const {
    static_cast<void>(read_manifest(archive, true));
}

RestoreResult ArchiveService::restore(const fs::path& archive,
                                      const fs::path& destination,
                                      const RestoreOptions& options) const {
    const bool destination_existed = fs::exists(destination);
    if (destination_existed && !fs::is_empty(destination)) {
        fail("destination must not exist or must be empty");
    }
    const auto manifest = read_manifest(archive, false);

    std::error_code error;
    fs::create_directories(destination, error);
    if (error) {
        fail("cannot create destination directory: " + error.message());
    }
    RestoreGuard guard(destination, destination_existed);

    struct PendingReference {
        EntryInfo info;
        fs::path output_path;
    };
    std::vector<PendingReference> hardlinks;
    std::vector<PendingReference> other_links;
    std::vector<EntryInfo> restored_entries;
    restored_entries.reserve(manifest.size());

    auto input = open_archive(archive);
    const auto header = read_header(input);
    for (std::uint64_t i = 0; i < header.entry_count; ++i) {
        auto entry = read_entry_header(input, header.version);
        const fs::path output_path =
            destination / checked_relative_path(entry.info.path);
        std::uint64_t checksum = checksum_bytes(entry.encoded_metadata);
        if (is_directory(entry.info.type)) {
            fs::create_directories(output_path, error);
            if (error) {
                fail("cannot create restored directory: " + error.message());
            }
            if (checksum != entry.info.checksum) {
                fail("checksum mismatch: " + entry.info.path);
            }
        } else if (entry.info.type == EntryType::file) {
            fs::create_directories(output_path.parent_path(), error);
            if (error) {
                fail("cannot create restored parent directory: " +
                     error.message());
            }
            if (fs::exists(output_path)) {
                fail("restore target already exists: " + entry.info.path);
            }
            std::ofstream output(output_path,
                                 std::ios::binary | std::ios::trunc);
            if (!output) {
                fail("cannot create restored file: " + entry.info.path);
            }
            copy_exact(input, &output, entry.data_size, checksum);
            output.close();
            if (!output || checksum != entry.info.checksum) {
                std::error_code ignored;
                fs::remove(output_path, ignored);
                fail(checksum != entry.info.checksum
                         ? "checksum mismatch: " + entry.info.path
                         : "failed to finalize restored file: " +
                               entry.info.path);
            }
        } else {
            entry.info.link_target =
                read_reference(input, entry, checksum, true);
            fs::create_directories(output_path.parent_path(), error);
            if (error) {
                fail("cannot create restored parent directory: " +
                     error.message());
            }
            PendingReference pending{entry.info, output_path};
            if (is_hardlink(entry.info.type)) {
                hardlinks.push_back(std::move(pending));
            } else {
                other_links.push_back(std::move(pending));
            }
        }
        restored_entries.push_back(std::move(entry.info));
    }
    ensure_archive_finished(input);
    validate_manifest(restored_entries);

    for (const auto& pending : hardlinks) {
        const auto existing =
            destination / checked_relative_path(pending.info.link_target);
        special_files::create_hardlink(existing, pending.output_path, error);
        if (error) {
            fail("cannot create restored hardlink: " + pending.info.path +
                 ": " + error.message());
        }
    }
    for (const auto& pending : other_links) {
        if (is_junction(pending.info.type)) {
            const auto target =
                destination / checked_relative_path(pending.info.link_target);
            special_files::create_junction(target, pending.output_path, error);
        } else {
            symlink::create(
                fs::u8path(pending.info.link_target), pending.output_path,
                pending.info.type == EntryType::directory_symlink
                    ? symlink::Kind::directory
                    : symlink::Kind::file,
                error);
        }
        if (error) {
            fail("cannot create restored link: " + pending.info.path + ": " +
                 error.message());
        }
    }

    RestoreResult result;
    if (options.metadata != MetadataRestoreMode::none) {
        std::vector<const EntryInfo*> ordered;
        ordered.reserve(manifest.size());
        for (const auto& entry : manifest) {
            ordered.push_back(&entry);
        }
        std::stable_sort(ordered.begin(), ordered.end(),
                         [](const auto* left, const auto* right) {
                             if (is_directory(left->type) !=
                                 is_directory(right->type)) {
                                 return !is_directory(left->type);
                             }
                             return path_depth(left->path) >
                                    path_depth(right->path);
                         });
        for (const auto* entry : ordered) {
            if (is_hardlink(entry->type)) {
                continue;
            }
            const auto path =
                destination / checked_relative_path(entry->path);
            metadata::apply(path, entry->metadata,
                            is_symlink(entry->type) || is_junction(entry->type),
                            error);
            if (!error) {
                continue;
            }
            const std::string warning =
                "cannot restore metadata for " + entry->path + ": " +
                error.message();
            if (options.metadata == MetadataRestoreMode::strict) {
                fail(warning);
            }
            result.warnings.push_back(warning);
        }
    }

    guard.commit();
    return result;
}

}  // namespace backup
