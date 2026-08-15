#include "backup/archive.hpp"

#include <filesystem>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void print_usage(std::ostream& out) {
    out << "BackupMaster 0.1.0\n"
        << "Usage:\n"
        << "  backup-master backup <source-directory> <archive.bkm>\n"
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

        if (command == "backup" && argc == 4) {
            service.backup(argv[2], argv[3]);
            std::cout << "Backup created: " << argv[3] << '\n';
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
                std::cout << (entry.is_directory ? "DIR " : "FILE")
                          << "  " << std::setw(12) << entry.size << "  "
                          << entry.path << '\n';
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

