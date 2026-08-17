#include "backup/archive.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <system_error>
#include <vector>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>

namespace backup {
namespace {

namespace fs = std::filesystem;

constexpr std::array<char, 8> kMagic{'B', 'K', 'M', 'A', 'S', 'T', 'R', '1'};
constexpr std::uint32_t kVersion = 1;
constexpr std::uint32_t kMaxPathBytes = 1024U * 1024U;
constexpr std::uint64_t kMaxEntries = 10'000'000ULL;
constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
constexpr std::size_t kBufferSize = 64U * 1024U;
const fs::path kDefaultBackupDirectory = "backup";

enum class EntryType : std::uint8_t { file = 1, directory = 2 };

struct SourceEntry {
    fs::path absolute;
    std::string relative;
    EntryType type; // 文件或目录
    std::uint64_t size;
};

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

template <typename UInt>
// 统一小端方式写
void write_unsigned(std::ostream& out, UInt value) {
    static_assert(std::is_unsigned<UInt>::value, "unsigned integer required");
    for (std::size_t i = 0; i < sizeof(UInt); ++i) {
        out.put(static_cast<char>((value >> (i * 8U)) & 0xffU));
    }
    if (!out) {
        fail("failed to write archive");
    }
}

template <typename UInt>
// 统一小端方式读
UInt read_unsigned(std::istream& in, const char* field) {
    static_assert(std::is_unsigned<UInt>::value, "unsigned integer required");
    UInt value = 0;
    for (std::size_t i = 0; i < sizeof(UInt); ++i) {
        const int byte = in.get();
        if (byte == std::char_traits<char>::eof()) {
            fail(std::string("truncated archive while reading ") + field);
        }
        value |= static_cast<UInt>(static_cast<unsigned char>(byte)) << (i * 8U);
    }
    return value;
}
// 计算校验和
std::uint64_t update_checksum(std::uint64_t checksum, const char* data,
                              std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) {
        checksum ^= static_cast<unsigned char>(data[i]);
        checksum *= kFnvPrime;
    }
    return checksum;
}

std::string portable_path(const fs::path& path) {
    return path.generic_u8string();
}
// 路径安全检查
fs::path checked_relative_path(const std::string& stored) {
    if (stored.empty() || stored.size() > kMaxPathBytes ||
        stored.find('\\') != std::string::npos) {
        fail("archive contains an invalid path");
    }
    const fs::path path = fs::u8path(stored);
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory()) {
        fail("archive contains an absolute path");
    }
    for (const auto& component : path) {
        if (component == ".." || component == "." || component.empty()) {
            fail("archive contains an unsafe path");
        }
    }
    return path;
}

// 写文件头：魔数、版本号、条目数量
void write_header(std::ostream& out, std::uint64_t entry_count) {
    out.write(kMagic.data(), static_cast<std::streamsize>(kMagic.size()));
    write_unsigned(out, kVersion);
    write_unsigned(out, entry_count);
}
// 读文件头：校验魔数、版本号、条目数量
std::uint64_t read_header(std::istream& in) {
    std::array<char, kMagic.size()> magic{};
    in.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (!in || magic != kMagic) {
        fail("not a BackupMaster archive");
    }
    const auto version = read_unsigned<std::uint32_t>(in, "version");
    if (version != kVersion) {
        fail("unsupported archive version: " + std::to_string(version));
    }
    const auto count = read_unsigned<std::uint64_t>(in, "entry count");
    if (count > kMaxEntries) {
        fail("archive entry count is unreasonable");
    }
    return count;
}
// 条目头包括：类型、路径长度、文件大小、校验和、路径
void write_entry_header(std::ostream& out, const SourceEntry& entry,
                        std::uint64_t checksum) {
    write_unsigned(out, static_cast<std::uint8_t>(entry.type));
    write_unsigned(out, static_cast<std::uint32_t>(entry.relative.size()));
    write_unsigned(out, entry.size);
    write_unsigned(out, checksum);
    out.write(entry.relative.data(), static_cast<std::streamsize>(entry.relative.size()));
    if (!out) {
        fail("failed to write archive entry");
    }
}

EntryInfo read_entry_header(std::istream& in) {
    const auto raw_type = read_unsigned<std::uint8_t>(in, "entry type");
    if (raw_type != static_cast<std::uint8_t>(EntryType::file) &&
        raw_type != static_cast<std::uint8_t>(EntryType::directory)) {
        fail("archive contains an unknown entry type");
    }
    const auto path_size = read_unsigned<std::uint32_t>(in, "path length");
    const auto file_size = read_unsigned<std::uint64_t>(in, "file size");
    const auto checksum = read_unsigned<std::uint64_t>(in, "checksum");
    if (path_size == 0 || path_size > kMaxPathBytes) {
        fail("archive contains an invalid path length");
    }
    std::string path(path_size, '\0');
    in.read(path.data(), static_cast<std::streamsize>(path.size()));
    if (!in) {
        fail("truncated archive while reading path");
    }
    checked_relative_path(path);
    const bool is_directory = raw_type == static_cast<std::uint8_t>(EntryType::directory);
    if (is_directory && file_size != 0) {
        fail("directory entry has non-zero size");
    }
    return {path, is_directory, file_size, checksum};
}

// 扫描源目录
// 处理过程：
// 将源目录转换成规范化绝对路径。
// 检查源路径是否为目录。
// 使用 recursive_directory_iterator 递归遍历。
// 判断条目是目录还是普通文件。
// 获取文件大小。
// 计算相对于源目录的路径。
// 将结果放入 SourceEntry 数组。
// 按相对路径排序（很重要）
std::vector<SourceEntry> scan_source(const fs::path& source) {
    std::error_code ec;
    const fs::path root = fs::canonical(source, ec);
    if (ec || !fs::is_directory(root)) {
        fail("source is not a readable directory");
    }

    std::vector<SourceEntry> entries;
    fs::recursive_directory_iterator iterator(
        root, fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;
    if (ec) {
        fail("cannot scan source directory: " + ec.message());
    }

    while (iterator != end) {
        const auto path = iterator->path();
        const auto status = iterator->symlink_status(ec);
        if (ec) {
            fail("cannot inspect source entry: " + ec.message());
        }

        EntryType type;
        std::uint64_t size = 0;
        if (fs::is_directory(status)) {
            type = EntryType::directory;
        } else if (fs::is_regular_file(status)) {
            type = EntryType::file;
            size = fs::file_size(path, ec);
            if (ec) {
                fail("cannot read file size: " + ec.message());
            }
        } else {
            fail("unsupported special file: " + portable_path(path));
        }

        const auto relative = portable_path(fs::relative(path, root, ec));
        if (ec || relative.empty() || relative.size() > kMaxPathBytes) {
            fail("cannot represent source path in archive");
        }
        entries.push_back({path, relative, type, size});
        iterator.increment(ec);
        if (ec) {
            fail("cannot continue scanning source directory: " + ec.message());
        }
    }

    std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
        return left.relative < right.relative;
    });
    return entries;
}

// 计算校验和，以 64 KiB 为单位读取源文件并计算校验值，不会把整个文件一次性放入内存，因此可以处理大文件
std::uint64_t checksum_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        fail("cannot open source file: " + portable_path(path));
    }
    std::array<char, kBufferSize> buffer{};
    std::uint64_t checksum = kFnvOffset;
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        checksum = update_checksum(checksum, buffer.data(), static_cast<std::size_t>(count));
    }
    if (!input.eof()) {
        fail("failed while reading source file: " + portable_path(path));
    }
    return checksum;
}

/*
    这里copy_exact被备份，恢复和校验方法调用，其中output作为指针类型，允许输入是nullptr
        指向一个输出流 → 写出数据 （backup / restore）
        nullptr     → 不写出数据 （verify）
*/
void copy_exact(std::istream& input, std::ostream* output, std::uint64_t size,
                std::uint64_t& checksum) {
    std::array<char, kBufferSize> buffer{};
    std::uint64_t remaining = size;
    while (remaining != 0) {
        const auto wanted = static_cast<std::streamsize>(
            std::min<std::uint64_t>(remaining, buffer.size()));
        input.read(buffer.data(), wanted);
        const auto count = input.gcount();
        if (count <= 0) {
            fail("truncated file data in archive");
        }
        if (output != nullptr) {
            output->write(buffer.data(), count);
            if (!*output) {
                fail("failed to write restored file");
            }
        }
        checksum = update_checksum(checksum, buffer.data(), static_cast<std::size_t>(count));
        remaining -= static_cast<std::uint64_t>(count);
    }
}

void ensure_archive_finished(std::istream& in) {
    if (in.peek() != std::char_traits<char>::eof()) {
        fail("archive contains trailing data");
    }
}

std::ifstream open_archive(const fs::path& archive) {
    std::ifstream input(archive, std::ios::binary);
    if (!input) {
        fail("cannot open archive: " + portable_path(archive));
    }
    return input;
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

    std::ostringstream stream;
    stream << std::put_time(&local_time, "%Y%m%d-%H%M%S");
    return stream.str();
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

}  // namespace

fs::path ArchiveService::backup(const fs::path& source,
                                const fs::path& requested_archive) const {
    const auto entries = scan_source(source);
    const fs::path archive = requested_archive.empty()
                                 ? make_default_archive_path()
                                 : requested_archive;
    if (fs::exists(archive)) {
        fail("archive already exists; choose a new path or remove it explicitly");
    }
    if (archive.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(archive.parent_path(), ec);
        if (ec) {
            fail("cannot create archive directory: " + ec.message());
        }
    }

    std::ofstream output(archive, std::ios::binary | std::ios::trunc);
    if (!output) {
        fail("cannot create archive: " + portable_path(archive));
    }
    try {
        write_header(output, entries.size());
        for (const auto& entry : entries) {
            const auto checksum = entry.type == EntryType::file
                                      ? checksum_file(entry.absolute)
                                      : kFnvOffset;
            write_entry_header(output, entry, checksum);
            if (entry.type == EntryType::file) {
                std::ifstream input(entry.absolute, std::ios::binary);
                if (!input) {
                    fail("cannot open source file: " + portable_path(entry.absolute));
                }
                std::uint64_t copied_checksum = kFnvOffset;
                copy_exact(input, &output, entry.size, copied_checksum);
                if (copied_checksum != checksum || input.peek() != std::char_traits<char>::eof()) {
                    fail("source file changed during backup: " + portable_path(entry.absolute));
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
    auto input = open_archive(archive);
    const auto count = read_header(input);
    std::vector<EntryInfo> entries;
    entries.reserve(static_cast<std::size_t>(count));
    std::set<std::string> paths;
    for (std::uint64_t i = 0; i < count; ++i) {
        auto entry = read_entry_header(input);
        if (!paths.insert(entry.path).second) {
            fail("archive contains a duplicate path: " + entry.path);
        }
        if (!entry.is_directory) {
            if (entry.size > static_cast<std::uint64_t>(
                                 std::numeric_limits<std::streamoff>::max())) {
                fail("archive entry is too large for this platform");
            }
            input.seekg(static_cast<std::streamoff>(entry.size), std::ios::cur);
            if (!input) {
                fail("truncated archive while skipping file data");
            }
        }
        entries.push_back(std::move(entry));
    }
    ensure_archive_finished(input);
    return entries;
}

void ArchiveService::verify(const fs::path& archive) const {
    auto input = open_archive(archive);
    const auto count = read_header(input);
    std::set<std::string> paths;
    for (std::uint64_t i = 0; i < count; ++i) {
        const auto entry = read_entry_header(input);
        if (!paths.insert(entry.path).second) {
            fail("archive contains a duplicate path: " + entry.path);
        }
        std::uint64_t actual = kFnvOffset;
        if (!entry.is_directory) {
            copy_exact(input, nullptr, entry.size, actual);
        }
        if (actual != entry.checksum) {
            fail("checksum mismatch: " + entry.path);
        }
    }
    ensure_archive_finished(input);
}

void ArchiveService::restore(const fs::path& archive,
                             const fs::path& destination) const {
    if (fs::exists(destination) && !fs::is_empty(destination)) {
        fail("destination must not exist or must be empty");
    }
    std::error_code ec;
    fs::create_directories(destination, ec);
    if (ec) {
        fail("cannot create destination directory: " + ec.message());
    }

    auto input = open_archive(archive);
    const auto count = read_header(input);
    std::set<std::string> paths;
    for (std::uint64_t i = 0; i < count; ++i) {
        const auto entry = read_entry_header(input);
        if (!paths.insert(entry.path).second) {
            fail("archive contains a duplicate path: " + entry.path);
        }
        const fs::path output_path = destination / checked_relative_path(entry.path);
        if (entry.is_directory) {
            fs::create_directories(output_path, ec);
            if (ec) {
                fail("cannot create restored directory: " + ec.message());
            }
            if (entry.checksum != kFnvOffset) {
                fail("checksum mismatch: " + entry.path);
            }
            continue;
        }

        fs::create_directories(output_path.parent_path(), ec);
        if (ec) {
            fail("cannot create restored parent directory: " + ec.message());
        }
        if (fs::exists(output_path)) {
            fail("restore target already exists: " + entry.path);
        }
        std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
        if (!output) {
            fail("cannot create restored file: " + entry.path);
        }
        std::uint64_t actual = kFnvOffset;
        copy_exact(input, &output, entry.size, actual);
        output.close();
        if (!output) {
            fail("failed to finalize restored file: " + entry.path);
        }
        if (actual != entry.checksum) {
            std::error_code ignored;
            fs::remove(output_path, ignored);
            fail("checksum mismatch: " + entry.path);
        }
    }
    ensure_archive_finished(input);
}

}  // namespace backup
