#include "backup/archive.hpp"

#include "metadata.hpp"

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

class UsageError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

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
    case backup::EntryType::junction:
        return "JUNCTION";
    case backup::EntryType::hardlink:
        return "HARDLINK";
    }
    return "UNKNOWN";
}

void print_usage(std::ostream& output) {
    output
        << "BackupMaster 0.3.0\nUsage:\n"
        << "  backup-master backup <source> [archive.bkm] [filters]\n"
        << "  backup-master restore <archive.bkm> <destination> "
           "[--metadata best-effort|strict|none]\n"
        << "  backup-master list <archive.bkm>\n"
        << "  backup-master verify <archive.bkm>\n\n"
        << "Backup filters (repeatable where noted):\n"
        << "  --include-path <glob>  --exclude-path <glob>  --name <glob>\n"
        << "  --type <file|directory|file-symlink|directory-symlink|"
           "junction|hardlink>\n"
        << "  --created-after <UTC>  --created-before <UTC>\n"
        << "  --modified-after <UTC> --modified-before <UTC>\n"
        << "  --accessed-after <UTC> --accessed-before <UTC>\n"
        << "  --owner <SID-or-account> --min-size <size> --max-size <size>\n";
}

std::string require_value(int argc, char* argv[], int& index,
                          const std::string& option) {
    if (++index >= argc) {
        throw UsageError("missing value for " + option);
    }
    return argv[index];
}

backup::EntryType parse_type(const std::string& value) {
    if (value == "file") return backup::EntryType::file;
    if (value == "directory") return backup::EntryType::directory;
    if (value == "file-symlink") return backup::EntryType::file_symlink;
    if (value == "directory-symlink") {
        return backup::EntryType::directory_symlink;
    }
    if (value == "junction") return backup::EntryType::junction;
    if (value == "hardlink") return backup::EntryType::hardlink;
    throw UsageError("unknown entry type: " + value);
}

int digits(const std::string& value, std::size_t offset, std::size_t count) {
    int result = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const char character = value[offset + i];
        if (!std::isdigit(static_cast<unsigned char>(character))) {
            throw UsageError("invalid UTC timestamp: " + value);
        }
        result = result * 10 + (character - '0');
    }
    return result;
}

bool leap_year(int year) {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

std::int64_t days_from_civil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
    const unsigned shifted_month = month > 2 ? month - 3 : month + 9;
    const unsigned day_of_year =
        (153U * shifted_month + 2U) /
            5U +
        day - 1U;
    const unsigned day_of_era =
        year_of_era * 365U + year_of_era / 4U - year_of_era / 100U +
        day_of_year;
    return static_cast<std::int64_t>(era) * 146097 +
           static_cast<std::int64_t>(day_of_era) - 719468;
}

std::int64_t parse_utc(const std::string& value) {
    if (value.size() != 20 || value[4] != '-' || value[7] != '-' ||
        value[10] != 'T' || value[13] != ':' || value[16] != ':' ||
        value[19] != 'Z') {
        throw UsageError("timestamp must use YYYY-MM-DDTHH:MM:SSZ");
    }
    const int year = digits(value, 0, 4);
    const int month = digits(value, 5, 2);
    const int day = digits(value, 8, 2);
    const int hour = digits(value, 11, 2);
    const int minute = digits(value, 14, 2);
    const int second = digits(value, 17, 2);
    constexpr int month_days[] = {0, 31, 28, 31, 30, 31, 30,
                                  31, 31, 30, 31, 30, 31};
    const int maximum_day =
        month == 2 && leap_year(year)
            ? 29
            : (month >= 1 && month <= 12 ? month_days[month] : 0);
    if (year < 1601 || day < 1 || day > maximum_day || hour > 23 ||
        minute > 59 || second > 59) {
        throw UsageError("invalid UTC timestamp: " + value);
    }
    constexpr std::int64_t kSecondsToWindowsEpoch = 11644473600LL;
    const auto seconds = days_from_civil(year, static_cast<unsigned>(month),
                                         static_cast<unsigned>(day)) *
                             86400LL +
                         hour * 3600LL + minute * 60LL + second +
                         kSecondsToWindowsEpoch;
    return seconds * 10'000'000LL;
}

std::uint64_t parse_size(const std::string& value) {
    std::size_t digits_end = 0;
    while (digits_end < value.size() &&
           std::isdigit(static_cast<unsigned char>(value[digits_end]))) {
        ++digits_end;
    }
    if (digits_end == 0) {
        throw UsageError("invalid size: " + value);
    }
    std::uint64_t multiplier = 1;
    const auto suffix = value.substr(digits_end);
    if (suffix == "B" || suffix.empty()) multiplier = 1;
    else if (suffix == "KiB") multiplier = 1024ULL;
    else if (suffix == "MiB") multiplier = 1024ULL * 1024ULL;
    else if (suffix == "GiB") multiplier = 1024ULL * 1024ULL * 1024ULL;
    else throw UsageError("invalid size suffix: " + value);
    std::uint64_t number = 0;
    try {
        number = std::stoull(value.substr(0, digits_end));
    } catch (const std::exception&) {
        throw UsageError("invalid size: " + value);
    }
    if (number > std::numeric_limits<std::uint64_t>::max() / multiplier) {
        throw UsageError("size is too large: " + value);
    }
    return number * multiplier;
}

backup::BackupOptions parse_backup_options(int argc, char* argv[]) {
    backup::BackupOptions options;
    int index = 3;
    if (index < argc && std::string(argv[index]).rfind("--", 0) != 0) {
        options.archive = argv[index++];
    }
    for (; index < argc; ++index) {
        const std::string option = argv[index];
        const auto value = require_value(argc, argv, index, option);
        if (option == "--include-path") options.filter.include_paths.push_back(value);
        else if (option == "--exclude-path") options.filter.exclude_paths.push_back(value);
        else if (option == "--name") options.filter.names.push_back(value);
        else if (option == "--type") options.filter.types.push_back(parse_type(value));
        else if (option == "--owner") options.filter.owners.push_back(value);
        else if (option == "--created-after") options.filter.creation_time.after = parse_utc(value);
        else if (option == "--created-before") options.filter.creation_time.before = parse_utc(value);
        else if (option == "--modified-after") options.filter.modification_time.after = parse_utc(value);
        else if (option == "--modified-before") options.filter.modification_time.before = parse_utc(value);
        else if (option == "--accessed-after") options.filter.access_time.after = parse_utc(value);
        else if (option == "--accessed-before") options.filter.access_time.before = parse_utc(value);
        else if (option == "--min-size") options.filter.minimum_size = parse_size(value);
        else if (option == "--max-size") options.filter.maximum_size = parse_size(value);
        else throw UsageError("unknown backup option: " + option);
    }
    if (options.filter.minimum_size && options.filter.maximum_size &&
        *options.filter.minimum_size > *options.filter.maximum_size) {
        throw UsageError("--min-size must not exceed --max-size");
    }
    return options;
}

backup::RestoreOptions parse_restore_options(int argc, char* argv[]) {
    backup::RestoreOptions options;
    for (int index = 4; index < argc; ++index) {
        const std::string option = argv[index];
        if (option != "--metadata") {
            throw UsageError("unknown restore option: " + option);
        }
        const auto value = require_value(argc, argv, index, option);
        if (value == "best-effort") {
            options.metadata = backup::MetadataRestoreMode::best_effort;
        } else if (value == "strict") {
            options.metadata = backup::MetadataRestoreMode::strict;
        } else if (value == "none") {
            options.metadata = backup::MetadataRestoreMode::none;
        } else {
            throw UsageError("unknown metadata mode: " + value);
        }
    }
    return options;
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
        if (command == "backup" && argc >= 3) {
            const auto archive =
                service.backup(argv[2], parse_backup_options(argc, argv));
            std::cout << "Backup created: " << archive.string() << '\n';
            return 0;
        }
        if (command == "restore" && argc >= 4) {
            const auto result = service.restore(
                argv[2], argv[3], parse_restore_options(argc, argv));
            for (const auto& warning : result.warnings) {
                std::cerr << "Warning: " << warning << '\n';
            }
            std::cout << "Archive restored to: " << argv[3] << '\n';
            return 0;
        }
        if (command == "list" && argc == 3) {
            const auto entries = service.list(argv[2]);
            std::cout << std::left << std::setw(10) << "TYPE" << std::right
                      << std::setw(12) << "SIZE" << "  " << std::left
                      << std::setw(28) << "MODIFIED" << std::setw(46)
                      << "OWNER" << "PATH\n";
            for (const auto& entry : entries) {
                const auto modified = entry.metadata.windows
                                          ? backup::metadata::format_utc(
                                                entry.metadata.windows->write_time)
                                          : "-";
                const auto owner = entry.metadata.windows
                                       ? entry.metadata.windows->owner_sid
                                       : "-";
                std::cout << std::left << std::setw(10) << type_name(entry.type)
                          << std::right << std::setw(12) << entry.size << "  "
                          << std::left << std::setw(28) << modified
                          << std::setw(46) << owner << entry.path;
                if (backup::has_reference_target(entry.type)) {
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
        throw UsageError("invalid command or argument count");
    } catch (const UsageError& error) {
        std::cerr << "Error: " << error.what() << '\n';
        print_usage(std::cerr);
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
