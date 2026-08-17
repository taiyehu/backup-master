#include "backup/archive.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

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
