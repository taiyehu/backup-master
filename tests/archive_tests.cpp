#include "backup/archive.hpp"
#include "backup/symlink.hpp"
#include "metadata.hpp"
#include "special_files.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Function>
void require_throws(Function function, const std::string& message) {
    try {
        function();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(message);
}

std::string read_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

template <typename UInt>
void write_unsigned(std::ostream& output, UInt value) {
    for (std::size_t i = 0; i < sizeof(UInt); ++i) {
        output.put(static_cast<char>((value >> (i * 8U)) & 0xffU));
    }
}

template <typename UInt>
void append_unsigned(std::string& output, UInt value) {
    for (std::size_t i = 0; i < sizeof(UInt); ++i) {
        output.push_back(static_cast<char>((value >> (i * 8U)) & 0xffU));
    }
}

std::uint64_t checksum(const std::string& data) {
    std::uint64_t value = 14695981039346656037ULL;
    for (const unsigned char byte : data) {
        value ^= byte;
        value *= 1099511628211ULL;
    }
    return value;
}

void write_missing_target_archive(const fs::path& archive) {
    const std::string path = "link";
    const std::string target = "missing.txt";
    std::ofstream output(archive, std::ios::binary);
    output.write("BKMASTR1", 8);
    write_unsigned<std::uint32_t>(output, 2);
    write_unsigned<std::uint64_t>(output, 1);
    write_unsigned<std::uint8_t>(
        output, static_cast<std::uint8_t>(backup::EntryType::file_symlink));
    write_unsigned<std::uint32_t>(output,
                                  static_cast<std::uint32_t>(path.size()));
    write_unsigned<std::uint64_t>(output, target.size());
    write_unsigned<std::uint64_t>(output, checksum(target));
    output.write(path.data(), static_cast<std::streamsize>(path.size()));
    output.write(target.data(), static_cast<std::streamsize>(target.size()));
}

void write_legacy_archive(const fs::path& archive) {
    const std::string path = "legacy.txt";
    const std::string data = "legacy data";
    std::ofstream output(archive, std::ios::binary);
    output.write("BKMASTR1", 8);
    write_unsigned<std::uint32_t>(output, 1);
    write_unsigned<std::uint64_t>(output, 1);
    write_unsigned<std::uint8_t>(
        output, static_cast<std::uint8_t>(backup::EntryType::file));
    write_unsigned<std::uint32_t>(output,
                                  static_cast<std::uint32_t>(path.size()));
    write_unsigned<std::uint64_t>(output, data.size());
    write_unsigned<std::uint64_t>(output, checksum(data));
    output.write(path.data(), static_cast<std::streamsize>(path.size()));
    output.write(data.data(), static_cast<std::streamsize>(data.size()));
}

void write_valid_symlink_archive(const fs::path& archive) {
    const std::string file_path = "target.txt";
    const std::string file_data = "target data";
    const std::string link_path = "link";
    const std::string link_target = "target.txt";
    std::ofstream output(archive, std::ios::binary);
    output.write("BKMASTR1", 8);
    write_unsigned<std::uint32_t>(output, 2);
    write_unsigned<std::uint64_t>(output, 2);

    write_unsigned<std::uint8_t>(
        output, static_cast<std::uint8_t>(backup::EntryType::file));
    write_unsigned<std::uint32_t>(
        output, static_cast<std::uint32_t>(file_path.size()));
    write_unsigned<std::uint64_t>(output, file_data.size());
    write_unsigned<std::uint64_t>(output, checksum(file_data));
    output.write(file_path.data(),
                 static_cast<std::streamsize>(file_path.size()));
    output.write(file_data.data(),
                 static_cast<std::streamsize>(file_data.size()));

    write_unsigned<std::uint8_t>(
        output, static_cast<std::uint8_t>(backup::EntryType::file_symlink));
    write_unsigned<std::uint32_t>(
        output, static_cast<std::uint32_t>(link_path.size()));
    write_unsigned<std::uint64_t>(output, link_target.size());
    write_unsigned<std::uint64_t>(output, checksum(link_target));
    output.write(link_path.data(),
                 static_cast<std::streamsize>(link_path.size()));
    output.write(link_target.data(),
                 static_cast<std::streamsize>(link_target.size()));
}

void write_invalid_v3_archive(const fs::path& archive,
                              std::uint32_t metadata_size) {
    std::ofstream output(archive, std::ios::binary);
    output.write("BKMASTR1", 8);
    write_unsigned<std::uint32_t>(output, 3);
    write_unsigned<std::uint64_t>(output, 1);
    write_unsigned<std::uint8_t>(
        output, static_cast<std::uint8_t>(backup::EntryType::directory));
    write_unsigned<std::uint32_t>(output, 1);
    write_unsigned<std::uint32_t>(output, metadata_size);
    write_unsigned<std::uint64_t>(output, 0);
    write_unsigned<std::uint64_t>(output, 14695981039346656037ULL);
    output.put('d');
}

void write_unknown_version_archive(const fs::path& archive) {
    std::ofstream output(archive, std::ios::binary);
    output.write("BKMASTR1", 8);
    write_unsigned<std::uint32_t>(output, 99);
    write_unsigned<std::uint64_t>(output, 0);
}

void write_restore_failure_archive(const fs::path& archive) {
    std::ofstream output(archive, std::ios::binary);
    output.write("BKMASTR1", 8);
    write_unsigned<std::uint32_t>(output, 1);
    write_unsigned<std::uint64_t>(output, 2);
    const auto write_file_entry = [&](const std::string& path,
                                      const std::string& data,
                                      std::uint64_t stored_checksum) {
        write_unsigned<std::uint8_t>(
            output, static_cast<std::uint8_t>(backup::EntryType::file));
        write_unsigned<std::uint32_t>(
            output, static_cast<std::uint32_t>(path.size()));
        write_unsigned<std::uint64_t>(output, data.size());
        write_unsigned<std::uint64_t>(output, stored_checksum);
        output.write(path.data(), static_cast<std::streamsize>(path.size()));
        output.write(data.data(), static_cast<std::streamsize>(data.size()));
    };
    write_file_entry("first.txt", "first", checksum("first"));
    write_file_entry("second.txt", "second", checksum("second") + 1U);
}

void write_unrestorable_metadata_archive(const fs::path& archive) {
    const std::string path = "metadata.txt";
    const std::string data = "metadata restore modes";
    std::string metadata;
    append_unsigned<std::uint8_t>(metadata, 1);   // Windows platform.
    append_unsigned<std::uint8_t>(metadata, 1);   // Metadata schema.
    append_unsigned<std::uint16_t>(metadata, 7);  // All fields present.
    for (int i = 0; i < 4; ++i) {
        append_unsigned<std::uint64_t>(metadata, 0);
    }
    append_unsigned<std::uint32_t>(metadata, 0x20);  // FILE_ATTRIBUTE_ARCHIVE.
    append_unsigned<std::uint32_t>(metadata, 0);     // Empty owner text.
    append_unsigned<std::uint32_t>(metadata, 20);    // Descriptor size.
    metadata.push_back(1);                            // SD revision.
    metadata.push_back(0);
    append_unsigned<std::uint16_t>(metadata, 0x8000); // SE_SELF_RELATIVE.
    for (int i = 0; i < 4; ++i) {
        append_unsigned<std::uint32_t>(metadata, 0);  // No owner/group/ACL.
    }

    std::ofstream output(archive, std::ios::binary);
    output.write("BKMASTR1", 8);
    write_unsigned<std::uint32_t>(output, 3);
    write_unsigned<std::uint64_t>(output, 1);
    write_unsigned<std::uint8_t>(
        output, static_cast<std::uint8_t>(backup::EntryType::file));
    write_unsigned<std::uint32_t>(output,
                                  static_cast<std::uint32_t>(path.size()));
    write_unsigned<std::uint32_t>(
        output, static_cast<std::uint32_t>(metadata.size()));
    write_unsigned<std::uint64_t>(output, data.size());
    write_unsigned<std::uint64_t>(output, checksum(metadata + data));
    output.write(path.data(), static_cast<std::streamsize>(path.size()));
    output.write(metadata.data(),
                 static_cast<std::streamsize>(metadata.size()));
    output.write(data.data(), static_cast<std::streamsize>(data.size()));
}

bool can_create_symlinks(const fs::path& root) {
    const auto target = root / "symlink-probe-target";
    const auto link = root / "symlink-probe-link";
    std::ofstream(target) << "probe";
    std::error_code error;
    backup::symlink::create("symlink-probe-target", link,
                            backup::symlink::Kind::file, error);
    if (!error) {
        fs::remove(link, error);
        fs::remove(target, error);
        return true;
    }
    const auto creation_error = error;
    fs::remove(target, error);
    if (creation_error == std::errc::permission_denied ||
        creation_error == std::errc::operation_not_permitted) {
        return false;
    }
    throw std::system_error(creation_error, "cannot probe symbolic-link support");
}

const backup::EntryInfo* find_entry(
    const std::vector<backup::EntryInfo>& entries, const std::string& path) {
    const auto found = std::find_if(entries.begin(), entries.end(),
                                    [&](const auto& entry) {
                                        return entry.path == path;
                                    });
    return found == entries.end() ? nullptr : &*found;
}

class CurrentPathGuard {
public:
    CurrentPathGuard() : original_(fs::current_path()) {}
    ~CurrentPathGuard() {
        std::error_code ignored;
        fs::current_path(original_, ignored);
    }

private:
    fs::path original_;
};

int main() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = fs::temp_directory_path() /
                      ("backup-master-test-" + std::to_string(unique));
    try {
        const auto source = root / "source";
        const auto archive = root / "sample.bkm";
        const auto restored = root / "restored";
        fs::create_directories(source / "nested" / "empty");
        {
            std::ofstream(source / "hello.txt", std::ios::binary) << "hello\n";
            std::ofstream(source / "nested" / "data.bin", std::ios::binary)
                << std::string("a\0b\0c", 5);
            std::ofstream(source / "empty-file", std::ios::binary);
        }

        const backup::ArchiveService service;
        require(service.backup(source, archive) == archive,
                "backup did not return the requested archive path");
        service.verify(archive);
        const auto entries = service.list(archive);
        require(entries.size() == 5, "unexpected entry count");
        require(backup::is_directory(backup::EntryType::directory),
                "is_directory rejected a directory");
        require(!backup::is_directory(backup::EntryType::directory_symlink),
                "is_directory treated a symlink as a real directory");
#ifdef _WIN32
        require(entries.front().metadata.platform ==
                    backup::MetadataPlatform::windows &&
                    entries.front().metadata.windows.has_value() &&
                    !entries.front().metadata.windows->owner_sid.empty() &&
                    !entries.front()
                         .metadata.windows->security_descriptor.empty(),
                "v3 archive did not expose Windows metadata");
#endif

        const auto restore_result = service.restore(archive, restored);
        static_cast<void>(restore_result);
#ifdef _WIN32
        const auto* archived_hello = find_entry(entries, "hello.txt");
        std::error_code restored_metadata_error;
        const auto restored_metadata = backup::metadata::capture(
            restored / "hello.txt", false, restored_metadata_error);
        require(!restored_metadata_error && archived_hello != nullptr &&
                    archived_hello->metadata.windows &&
                    restored_metadata.windows &&
                    restored_metadata.windows->write_time ==
                        archived_hello->metadata.windows->write_time &&
                    restored_metadata.windows->owner_sid ==
                        archived_hello->metadata.windows->owner_sid,
                "Windows owner or modification time was not restored");
#else
        require(restore_result.warnings.empty(),
                "metadata-free restore emitted warnings");
#endif
        require(read_file(restored / "hello.txt") == "hello\n",
                "text file differs after restore");
        require(read_file(restored / "nested" / "data.bin") ==
                    std::string("a\0b\0c", 5),
                "binary file differs after restore");
        require(fs::is_directory(restored / "nested" / "empty"),
                "empty directory was not restored");
        require(fs::file_size(restored / "empty-file") == 0,
                "empty file was not restored");

        require_throws([&] { service.backup(source, archive); },
                       "backup unexpectedly overwrote an existing archive");

        {
            std::ofstream corrupt(archive, std::ios::binary | std::ios::app);
            corrupt.put('x');
        }
        require_throws([&] { service.verify(archive); },
                       "verify accepted an archive with trailing data");

        const auto missing_target_archive = root / "missing-target.bkm";
        write_missing_target_archive(missing_target_archive);
        require_throws([&] { static_cast<void>(service.list(missing_target_archive)); },
                       "list accepted a symlink to a missing archive entry");
        require_throws([&] { service.verify(missing_target_archive); },
                       "verify accepted a symlink to a missing archive entry");

        const auto legacy_archive = root / "legacy.bkm";
        const auto legacy_restored = root / "legacy-restored";
        write_legacy_archive(legacy_archive);
        service.verify(legacy_archive);
        const auto legacy_entries = service.list(legacy_archive);
        require(legacy_entries.size() == 1 &&
                    legacy_entries.front().type == backup::EntryType::file,
                "list did not read a v1 archive");
        service.restore(legacy_archive, legacy_restored);
        require(read_file(legacy_restored / "legacy.txt") == "legacy data",
                "v1 archive was not restored correctly");

        const auto valid_link_archive = root / "valid-link.bkm";
        write_valid_symlink_archive(valid_link_archive);
        service.verify(valid_link_archive);
        const auto valid_link_entries = service.list(valid_link_archive);
        require(valid_link_entries.size() == 2 &&
                    valid_link_entries.back().type ==
                        backup::EntryType::file_symlink &&
                    valid_link_entries.back().link_target == "target.txt",
                "list did not read a valid symlink entry");

        const auto invalid_metadata_archive = root / "invalid-metadata.bkm";
        write_invalid_v3_archive(invalid_metadata_archive, 1);
        require_throws(
            [&] { static_cast<void>(service.list(invalid_metadata_archive)); },
            "list accepted malformed v3 metadata");
        const auto oversized_metadata_archive =
            root / "oversized-metadata.bkm";
        write_invalid_v3_archive(oversized_metadata_archive,
                                 1024U * 1024U + 1U);
        require_throws(
            [&] { service.verify(oversized_metadata_archive); },
            "verify accepted oversized v3 metadata");
        const auto unknown_version_archive = root / "unknown-version.bkm";
        write_unknown_version_archive(unknown_version_archive);
        require_throws([&] { service.verify(unknown_version_archive); },
                       "verify accepted an unknown archive version");

        const auto restore_failure_archive = root / "restore-failure.bkm";
        const auto failed_destination = root / "failed-destination";
        write_restore_failure_archive(restore_failure_archive);
        require_throws(
            [&] { service.restore(restore_failure_archive, failed_destination); },
            "restore accepted corrupt file data");
        require(!fs::exists(failed_destination),
                "failed restore left a partial destination");

        const auto unrestorable_metadata_archive =
            root / "unrestorable-metadata.bkm";
        write_unrestorable_metadata_archive(unrestorable_metadata_archive);
        service.verify(unrestorable_metadata_archive);

        backup::RestoreOptions best_effort;
        best_effort.metadata = backup::MetadataRestoreMode::best_effort;
        const auto best_effort_destination = root / "best-effort-metadata";
        const auto best_effort_result = service.restore(
            unrestorable_metadata_archive, best_effort_destination,
            best_effort);
        require(!best_effort_result.warnings.empty() &&
                    read_file(best_effort_destination / "metadata.txt") ==
                        "metadata restore modes",
                "best-effort metadata restore did not preserve content and warn");

        backup::RestoreOptions strict_metadata;
        strict_metadata.metadata = backup::MetadataRestoreMode::strict;
        const auto strict_destination = root / "strict-metadata";
        require_throws(
            [&] {
                service.restore(unrestorable_metadata_archive,
                                strict_destination, strict_metadata);
            },
            "strict metadata restore accepted an unappliable descriptor");
        require(!fs::exists(strict_destination),
                "strict metadata failure did not roll back the destination");

        backup::RestoreOptions no_metadata_restore;
        no_metadata_restore.metadata = backup::MetadataRestoreMode::none;
        const auto no_metadata_destination = root / "no-metadata";
        const auto no_metadata_result = service.restore(
            unrestorable_metadata_archive, no_metadata_destination,
            no_metadata_restore);
        require(no_metadata_result.warnings.empty() &&
                    read_file(no_metadata_destination / "metadata.txt") ==
                        "metadata restore modes",
                "metadata none mode did not skip unappliable metadata");

        const auto hardlink_source = root / "hardlink-source";
        const auto hardlink_archive = root / "hardlinks.bkm";
        const auto hardlink_restored = root / "hardlink-restored";
        fs::create_directories(hardlink_source);
        std::ofstream(hardlink_source / "original.txt") << "hardlink data";
        std::error_code hardlink_error;
        fs::create_hard_link(hardlink_source / "original.txt",
                             hardlink_source / "alias.txt", hardlink_error);
        require(!hardlink_error, "cannot create hardlink for testing");
        service.backup(hardlink_source, hardlink_archive);
        service.verify(hardlink_archive);
        const auto hardlink_entries = service.list(hardlink_archive);
        require(hardlink_entries.size() == 2,
                "hardlink archive has the wrong entry count");
        const auto hardlink_count = std::count_if(
            hardlink_entries.begin(), hardlink_entries.end(),
            [](const auto& entry) { return backup::is_hardlink(entry.type); });
        require(hardlink_count == 1,
                "hardlinked files were not represented by one reference");
        backup::RestoreOptions no_metadata;
        no_metadata.metadata = backup::MetadataRestoreMode::none;
        service.restore(hardlink_archive, hardlink_restored, no_metadata);
        require(fs::equivalent(hardlink_restored / "original.txt",
                               hardlink_restored / "alias.txt"),
                "restored hardlinks do not share a file identity");

        const auto filter_source = root / "filter-source";
        fs::create_directories(filter_source / "nested");
        fs::create_directories(filter_source / "excluded");
        std::ofstream(filter_source / "alpha.txt") << "alpha";
        std::ofstream(filter_source / "beta.bin") << std::string(20, 'b');
        std::ofstream(filter_source / "nested" / "gamma.txt")
            << std::string(30, 'g');
        std::ofstream(filter_source / "excluded" / "hidden.txt")
            << "hidden";

        backup::BackupOptions filter_options;
        filter_options.archive = root / "filtered.bkm";
        filter_options.filter.include_paths = {"**/*.txt"};
        filter_options.filter.exclude_paths = {"excluded/**"};
        filter_options.filter.names = {"*.txt"};
        filter_options.filter.minimum_size = 5;
        filter_options.filter.maximum_size = 30;
        service.backup(filter_source, filter_options);
        const auto filtered_entries = service.list(filter_options.archive);
        require(find_entry(filtered_entries, "alpha.txt") != nullptr &&
                    find_entry(filtered_entries, "nested/gamma.txt") != nullptr &&
                    find_entry(filtered_entries, "nested") != nullptr &&
                    find_entry(filtered_entries, "beta.bin") == nullptr &&
                    find_entry(filtered_entries, "excluded/hidden.txt") == nullptr,
                "combined backup filters selected the wrong entries");

#ifdef _WIN32
        const auto* filtered_alpha = find_entry(filtered_entries, "alpha.txt");
        require(filtered_alpha != nullptr && filtered_alpha->metadata.windows,
                "filtered entry has no Windows metadata");
        backup::BackupOptions metadata_filter_options;
        metadata_filter_options.archive = root / "metadata-filtered.bkm";
        metadata_filter_options.filter.names = {"alpha.txt", "gamma.txt"};
        metadata_filter_options.filter.owners = {
            filtered_alpha->metadata.windows->owner_sid};
        metadata_filter_options.filter.creation_time.after = 0;
        metadata_filter_options.filter.creation_time.before =
            std::numeric_limits<std::int64_t>::max();
        metadata_filter_options.filter.modification_time.after = 0;
        metadata_filter_options.filter.modification_time.before =
            std::numeric_limits<std::int64_t>::max();
        metadata_filter_options.filter.access_time.after = 0;
        metadata_filter_options.filter.access_time.before =
            std::numeric_limits<std::int64_t>::max();
        service.backup(filter_source, metadata_filter_options);
        const auto metadata_filtered =
            service.list(metadata_filter_options.archive);
        require(find_entry(metadata_filtered, "alpha.txt") != nullptr &&
                    find_entry(metadata_filtered, "nested/gamma.txt") != nullptr &&
                    find_entry(metadata_filtered, "beta.bin") == nullptr,
                "owner or time filters selected the wrong entries");
#endif

        backup::BackupOptions type_filter_options;
        type_filter_options.archive = root / "type-filtered.bkm";
        type_filter_options.filter.types = {backup::EntryType::directory};
        service.backup(filter_source, type_filter_options);
        const auto type_filtered = service.list(type_filter_options.archive);
        require(!type_filtered.empty() &&
                    std::all_of(type_filtered.begin(), type_filtered.end(),
                                [](const auto& entry) {
                                    return backup::is_directory(entry.type);
                                }),
                "type filter included a non-directory entry");

        backup::BackupOptions single_hardlink_options;
        single_hardlink_options.archive = root / "single-hardlink.bkm";
        single_hardlink_options.filter.names = {"original.txt"};
        service.backup(hardlink_source, single_hardlink_options);
        const auto single_hardlink_entries =
            service.list(single_hardlink_options.archive);
        require(single_hardlink_entries.size() == 1 &&
                    single_hardlink_entries.front().type ==
                        backup::EntryType::file,
                "a single selected hardlink path was not stored as a file");

        backup::BackupOptions no_match_options;
        no_match_options.archive = root / "no-match.bkm";
        no_match_options.filter.names = {"does-not-exist"};
        require_throws(
            [&] { service.backup(filter_source, no_match_options); },
            "backup accepted a filter with no matching entries");
        require(!fs::exists(no_match_options.archive),
                "empty filtered backup created an archive");

#ifdef _WIN32
        const auto junction_source = root / "junction-source";
        const auto junction_archive = root / "junctions.bkm";
        const auto junction_restored = root / "junction-restored";
        fs::create_directories(junction_source / "target");
        std::ofstream(junction_source / "target" / "inside.txt")
            << "junction data";
        std::error_code junction_error;
        backup::special_files::create_junction(
            junction_source / "target", junction_source / "joined",
            junction_error);
        if (!junction_error) {
            service.backup(junction_source, junction_archive);
            service.verify(junction_archive);
            const auto junction_entries = service.list(junction_archive);
            const auto* junction = find_entry(junction_entries, "joined");
            require(junction != nullptr &&
                        junction->type == backup::EntryType::junction &&
                        junction->link_target == "target",
                    "list did not report the internal junction");
            service.restore(junction_archive, junction_restored, no_metadata);
            require(backup::special_files::is_junction(
                        junction_restored / "joined", junction_error),
                    "junction was not restored as a junction");
            require(read_file(junction_restored / "joined" / "inside.txt") ==
                        "junction data",
                    "restored junction points to the wrong target");

            backup::BackupOptions junction_filter_options;
            junction_filter_options.archive = root / "junction-filtered.bkm";
            junction_filter_options.filter.types = {
                backup::EntryType::junction};
            service.backup(junction_source, junction_filter_options);
            const auto junction_filtered =
                service.list(junction_filter_options.archive);
            require(find_entry(junction_filtered, "joined") != nullptr &&
                        find_entry(junction_filtered, "target") != nullptr,
                    "junction filter did not add its target dependency");

            backup::BackupOptions excluded_dependency_options;
            excluded_dependency_options.archive =
                root / "excluded-junction-target.bkm";
            excluded_dependency_options.filter.types = {
                backup::EntryType::junction};
            excluded_dependency_options.filter.exclude_paths = {"target"};
            require_throws(
                [&] {
                    service.backup(junction_source,
                                   excluded_dependency_options);
                },
                "filter silently crossed an explicit dependency exclusion");

            const auto external_target = root / "external-junction-target";
            const auto external_source = root / "external-junction-source";
            fs::create_directories(external_target);
            fs::create_directories(external_source);
            backup::special_files::create_junction(
                external_target, external_source / "joined", junction_error);
            require(!junction_error,
                    "cannot create external junction for testing");
            require_throws(
                [&] {
                    service.backup(external_source,
                                   root / "external-junction.bkm");
                },
                "backup accepted a junction outside the source tree");

            const auto cycle_source = root / "junction-cycle-source";
            fs::create_directories(cycle_source / "anchor");
            backup::special_files::create_junction(
                cycle_source / "anchor", cycle_source / "anchor" / "loop",
                junction_error);
            require(!junction_error,
                    "cannot create junction cycle for testing");
            require_throws(
                [&] {
                    service.backup(cycle_source,
                                   root / "junction-cycle.bkm");
                },
                "backup accepted a junction directory cycle");
            fs::remove(cycle_source / "anchor" / "loop", junction_error);
            require(!junction_error,
                    "cannot remove junction cycle after testing");
        } else {
            std::cout << "Junction tests skipped: "
                      << junction_error.message() << '\n';
        }
#endif

        if (can_create_symlinks(root)) {
            const auto link_source = root / "link-source";
            const auto link_archive = root / "links.bkm";
            const auto link_restored = root / "link-restored";
            fs::create_directories(link_source / "folder");
            std::ofstream(link_source / "target.txt") << "linked file";
            std::ofstream(link_source / "folder" / "inside.txt") << "linked dir";
            std::error_code link_error;
            backup::symlink::create("target.txt", link_source / "file-link",
                                    backup::symlink::Kind::file, link_error);
            require(!link_error, "cannot create file symlink for testing");
            backup::symlink::create("folder", link_source / "dir-link",
                                    backup::symlink::Kind::directory, link_error);
            require(!link_error, "cannot create directory symlink for testing");

            service.backup(link_source, link_archive);
            service.verify(link_archive);
            const auto link_entries = service.list(link_archive);
            const auto file_link = std::find_if(
                link_entries.begin(), link_entries.end(), [](const auto& entry) {
                    return entry.path == "file-link";
                });
            require(file_link != link_entries.end() &&
                        file_link->type == backup::EntryType::file_symlink &&
                        file_link->link_target == "target.txt",
                    "list did not report the file symlink");
            const auto directory_link = std::find_if(
                link_entries.begin(), link_entries.end(), [](const auto& entry) {
                    return entry.path == "dir-link";
                });
            require(directory_link != link_entries.end() &&
                        directory_link->type ==
                            backup::EntryType::directory_symlink &&
                        directory_link->link_target == "folder",
                    "list did not report the directory symlink");

            service.restore(link_archive, link_restored);
            require(backup::symlink::is_symbolic_link(
                        link_restored / "file-link", link_error),
                    "file symlink was not restored as a symlink");
            require(backup::symlink::is_symbolic_link(
                        link_restored / "dir-link", link_error),
                    "directory symlink was not restored as a symlink");
            require(read_file(link_restored / "file-link") == "linked file",
                    "restored file symlink has the wrong target");
            require(read_file(link_restored / "dir-link" / "inside.txt") ==
                        "linked dir",
                    "restored directory symlink has the wrong target");

            const auto outside = root / "outside.txt";
            std::ofstream(outside) << "outside";
            const auto unsafe_source = root / "unsafe-source";
            fs::create_directories(unsafe_source);
            backup::symlink::create("../outside.txt",
                                    unsafe_source / "outside-link",
                                    backup::symlink::Kind::file, link_error);
            require(!link_error, "cannot create unsafe symlink for testing");
            require_throws(
                [&] { service.backup(unsafe_source, root / "unsafe.bkm"); },
                "backup accepted a symlink target outside the archive");
        } else {
            require_throws(
                [&] {
                    service.restore(valid_link_archive,
                                    root / "link-restore-without-permission");
                },
                "restore unexpectedly created a symlink without permission");
            std::cout << "Symbolic-link tests skipped: permission unavailable\n";
        }

        {
            CurrentPathGuard current_path_guard;
            fs::current_path(root);
            const auto default_archive = service.backup(source);
            require(default_archive.parent_path() == fs::path("backup"),
                    "default archive was created outside the backup directory");
            require(default_archive.extension() == ".bkm",
                    "default archive does not use the .bkm extension");
            require(fs::is_regular_file(default_archive),
                    "default archive was not created");
            service.verify(default_archive);
        }

        fs::remove_all(root);
        std::cout << "All archive tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failure: " << error.what() << '\n';
        std::error_code ignored;
        fs::remove_all(root, ignored);
        return 1;
    }
}
