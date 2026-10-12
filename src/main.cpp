#include "backup/archive.hpp"

#include <filesystem>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

const char* type_name(backup::EntryType type) {
    switch (type) {
    case backup::EntryType::file:
        return "FILE";
    case backup::EntryType::directory:
        return "DIR";
    case backup::EntryType::file_symlink:
        return "SYMLINK";
    case backup::EntryType::directory_symlink:
        return "DIRLINK";
    }
    return "UNKNOWN";
}

void print_usage(std::ostream& out) {
    out << "BackupMaster 0.2.0\n"
        << "Usage:\n"
        << "  backup-master backup <source-directory> [archive.bkm]\n"
        << "  backup-master restore <archive.bkm> <destination-directory>\n"
        << "  backup-master list <archive.bkm>\n"
        << "  backup-master verify <archive.bkm>\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(std::cerr);
        return 2;
    }

    try {
        const std::string command = argv[1];
        const backup::ArchiveService service;

        if (command == "backup" && (argc == 3 || argc == 4)) {
            const std::filesystem::path requested_archive =
                argc == 4 ? std::filesystem::path(argv[3])
                          : std::filesystem::path{};
            const auto archive = service.backup(argv[2], requested_archive);
            std::cout << "Backup created: " << archive.string() << '\n';
            return 0;
        }
        if (command == "restore" && argc == 4) {
            service.restore(argv[2], argv[3]);
            std::cout << "Archive restored to: " << argv[3] << '\n';
            return 0;
        }
        if (command == "list" && argc == 3) {
            const auto entries = service.list(argv[2]);
            std::cout << "TYPE  " << std::setw(12) << "SIZE" << "  PATH\n";
            for (const auto& entry : entries) {
                std::cout << std::left << std::setw(8) << type_name(entry.type)
                          << std::right << std::setw(12) << entry.size << "  "
                          << entry.path;
                if (backup::is_symlink(entry.type)) {
                    std::cout << " -> " << entry.link_target;
                }
                std::cout << '\n';
            }
            std::cout << entries.size() << " entries\n";
            return 0;
        }
        if (command == "verify" && argc == 3) {
            service.verify(argv[2]);
            std::cout << "Archive is valid: " << argv[2] << '\n';
            return 0;
        }

        print_usage(std::cerr);
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
