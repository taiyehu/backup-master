#include "backup/archive.hpp"
#include "backup/symlink.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
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

        service.restore(archive, restored);
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
