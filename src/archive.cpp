#include "backup/archive.hpp"
#include "backup/symlink.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace backup {
namespace {

namespace fs = std::filesystem;

constexpr std::array<char, 8> kMagic{'B', 'K', 'M', 'A', 'S', 'T', 'R', '1'};
constexpr std::uint32_t kLegacyVersion = 1;
constexpr std::uint32_t kCurrentVersion = 2;
constexpr std::uint32_t kMaxPathBytes = 1024U * 1024U;
constexpr std::uint64_t kMaxSymlinkTargetBytes = 32U * 1024U;
constexpr std::uint64_t kMaxEntries = 10'000'000ULL;
constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
constexpr std::size_t kBufferSize = 64U * 1024U;
const fs::path kDefaultBackupDirectory = "backup";

struct ArchiveHeader {
    std::uint32_t version;
    std::uint64_t entry_count;
};

struct SourceEntry {
    fs::path absolute;
    std::string relative;
    EntryType type;
    std::uint64_t size;
    std::string link_target;
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

std::uint64_t checksum_bytes(const std::string& data) {
    return update_checksum(kFnvOffset, data.data(), data.size());
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

std::string referenced_path(const std::string& link_path,
                            const std::string& link_target) {
    if (link_target.empty() || link_target.size() > kMaxSymlinkTargetBytes ||
        link_target.find('\\') != std::string::npos ||
        link_target.find('\0') != std::string::npos) {
        fail("archive contains an invalid symlink target: " + link_path);
    }

    const auto target = fs::u8path(link_target);
    if (target.is_absolute() || target.has_root_name() ||
        target.has_root_directory()) {
        fail("archive contains an absolute symlink target: " + link_path);
    }

    const auto link = checked_relative_path(link_path);
    const auto resolved = (link.parent_path() / target).lexically_normal();
    if (resolved.empty() || resolved == ".") {
        fail("archive symlink target does not name an entry: " + link_path);
    }
    for (const auto& component : resolved) {
        if (component == ".." || component.empty()) {
            fail("archive symlink target escapes the archive: " + link_path);
        }
    }
    return portable_path(resolved);
}

void validate_manifest(const std::vector<EntryInfo>& entries) {
    std::map<std::string, EntryType> types;
    for (const auto& entry : entries) {
        if (!types.emplace(entry.path, entry.type).second) {
            fail("archive contains a duplicate path: " + entry.path);
        }
    }

    for (const auto& entry : entries) {
        auto parent = fs::u8path(entry.path).parent_path();
        while (!parent.empty()) {
            const auto found = types.find(portable_path(parent));
            if (found != types.end() && !is_directory(found->second)) {
                fail("archive entry has a non-directory parent: " + entry.path);
            }
            parent = parent.parent_path();
        }

        if (!is_symlink(entry.type)) {
            continue;
        }
        const auto target_path = referenced_path(entry.path, entry.link_target);
        const auto target = types.find(target_path);
        if (target == types.end()) {
            fail("archive symlink target is not an archived entry: " +
                 entry.path);
        }
        const bool expects_directory =
            entry.type == EntryType::directory_symlink;
        const bool target_is_directory =
            target->second == EntryType::directory ||
            target->second == EntryType::directory_symlink;
        if (expects_directory != target_is_directory) {
            fail("archive symlink target type does not match: " + entry.path);
        }
    }
}

// 写文件头：魔数、版本号、条目数量
void write_header(std::ostream& out, std::uint64_t entry_count) {
    out.write(kMagic.data(), static_cast<std::streamsize>(kMagic.size()));
    write_unsigned(out, kCurrentVersion);
    write_unsigned(out, entry_count);
}
// 读文件头：校验魔数、版本号、条目数量
ArchiveHeader read_header(std::istream& in) {
    std::array<char, kMagic.size()> magic{};
    in.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (!in || magic != kMagic) {
        fail("not a BackupMaster archive");
    }
    const auto version = read_unsigned<std::uint32_t>(in, "version");
    if (version != kLegacyVersion && version != kCurrentVersion) {
        fail("unsupported archive version: " + std::to_string(version));
    }
    const auto count = read_unsigned<std::uint64_t>(in, "entry count");
    if (count > kMaxEntries) {
        fail("archive entry count is unreasonable");
    }
    return {version, count};
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

EntryInfo read_entry_header(std::istream& in, std::uint32_t version) {
    const auto raw_type = read_unsigned<std::uint8_t>(in, "entry type");
    const bool legacy_type =
        raw_type == static_cast<std::uint8_t>(EntryType::file) ||
        raw_type == static_cast<std::uint8_t>(EntryType::directory);
    const bool link_type =
        raw_type == static_cast<std::uint8_t>(EntryType::file_symlink) ||
        raw_type == static_cast<std::uint8_t>(EntryType::directory_symlink);
    if (!legacy_type && (version < kCurrentVersion || !link_type)) {
        fail("archive contains an unknown entry type");
    }
    const auto path_size = read_unsigned<std::uint32_t>(in, "path length");
    const auto data_size = read_unsigned<std::uint64_t>(in, "data size");
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
    const auto type = static_cast<EntryType>(raw_type);
    if (is_directory(type) && data_size != 0) {
        fail("directory entry has non-zero size");
    }
    if (is_symlink(type) && data_size > kMaxSymlinkTargetBytes) {
        fail("archive symlink target is too long: " + path);
    }
    return {path, type, data_size, checksum, {}};
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
        const bool symbolic_link = symlink::is_symbolic_link(path, ec);
        if (ec) {
            fail("cannot inspect source reparse point: " + portable_path(path) +
                 ": " + ec.message());
        }

        EntryType type;
        std::uint64_t size = 0;
        std::string link_target;
        if (symbolic_link) {
            const auto info = symlink::inspect(path, ec);
            if (ec) {
                fail("cannot inspect symlink target: " + portable_path(path) +
                     ": " + ec.message());
            }
            link_target = portable_path(info.target);
            if (link_target.empty() ||
                link_target.size() > kMaxSymlinkTargetBytes) {
                fail("symlink target is too long: " + portable_path(path));
            }
            size = static_cast<std::uint64_t>(link_target.size());
            if (info.kind == symlink::Kind::directory) {
                type = EntryType::directory_symlink;
            } else {
                type = EntryType::file_symlink;
            }
        } else if (fs::is_directory(status)) {
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

        const auto relative = portable_path(path.lexically_relative(root));
        if (relative.empty() || relative.size() > kMaxPathBytes) {
            fail("cannot represent source path in archive");
        }
        entries.push_back({path, relative, type, size, link_target});
        iterator.increment(ec);
        if (ec) {
            fail("cannot continue scanning source directory: " + ec.message());
        }
    }

    std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
        return left.relative < right.relative;
    });

    std::vector<EntryInfo> manifest;
    manifest.reserve(entries.size());
    for (const auto& entry : entries) {
        manifest.push_back({entry.relative, entry.type, entry.size, 0,
                            entry.link_target});
    }
    validate_manifest(manifest);
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

std::string read_symlink_target(std::istream& input, const EntryInfo& entry) {
    std::string target(static_cast<std::size_t>(entry.size), '\0');
    input.read(target.data(), static_cast<std::streamsize>(target.size()));
    if (!input) {
        fail("truncated symlink target in archive: " + entry.path);
    }
    if (checksum_bytes(target) != entry.checksum) {
        fail("checksum mismatch: " + entry.path);
    }
    return target;
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
            const auto checksum =
                entry.type == EntryType::file
                    ? checksum_file(entry.absolute)
                    : (is_symlink(entry.type)
                           ? checksum_bytes(entry.link_target)
                           : kFnvOffset);
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
            } else if (is_symlink(entry.type)) {
                std::error_code ec;
                const auto current = symlink::inspect(entry.absolute, ec);
                const bool current_is_directory =
                    !ec && current.kind == symlink::Kind::directory;
                const bool expected_is_directory =
                    entry.type == EntryType::directory_symlink;
                if (ec || portable_path(current.target) != entry.link_target ||
                    current_is_directory != expected_is_directory) {
                    fail("source symlink changed during backup: " +
                         portable_path(entry.absolute));
                }
                output.write(entry.link_target.data(),
                             static_cast<std::streamsize>(entry.link_target.size()));
                if (!output) {
                    fail("failed to write symlink target: " + entry.relative);
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
    const auto header = read_header(input);
    std::vector<EntryInfo> entries;
    entries.reserve(static_cast<std::size_t>(header.entry_count));
    for (std::uint64_t i = 0; i < header.entry_count; ++i) {
        auto entry = read_entry_header(input, header.version);
        if (entry.type == EntryType::file) {
            if (entry.size > static_cast<std::uint64_t>(
                                 std::numeric_limits<std::streamoff>::max())) {
                fail("archive entry is too large for this platform");
            }
            input.seekg(static_cast<std::streamoff>(entry.size), std::ios::cur);
            if (!input) {
                fail("truncated archive while skipping file data");
            }
        } else if (is_symlink(entry.type)) {
            entry.link_target = read_symlink_target(input, entry);
        } else if (entry.checksum != kFnvOffset) {
            fail("checksum mismatch: " + entry.path);
        }
        entries.push_back(std::move(entry));
    }
    ensure_archive_finished(input);
    validate_manifest(entries);
    return entries;
}

void ArchiveService::verify(const fs::path& archive) const {
    auto input = open_archive(archive);
    const auto header = read_header(input);
    std::vector<EntryInfo> entries;
    entries.reserve(static_cast<std::size_t>(header.entry_count));
    for (std::uint64_t i = 0; i < header.entry_count; ++i) {
        auto entry = read_entry_header(input, header.version);
        std::uint64_t actual = kFnvOffset;
        if (entry.type == EntryType::file) {
            copy_exact(input, nullptr, entry.size, actual);
        } else if (is_symlink(entry.type)) {
            entry.link_target = read_symlink_target(input, entry);
            actual = entry.checksum;
        }
        if (actual != entry.checksum) {
            fail("checksum mismatch: " + entry.path);
        }
        entries.push_back(std::move(entry));
    }
    ensure_archive_finished(input);
    validate_manifest(entries);
}

void ArchiveService::restore(const fs::path& archive,
                             const fs::path& destination) const {
    if (fs::exists(destination) && !fs::is_empty(destination)) {
        fail("destination must not exist or must be empty");
    }
    const auto manifest = list(archive);

    std::error_code ec;
    fs::create_directories(destination, ec);
    if (ec) {
        fail("cannot create destination directory: " + ec.message());
    }

    auto input = open_archive(archive);
    const auto header = read_header(input);
    struct PendingSymlink {
        fs::path path;
        fs::path target;
        symlink::Kind kind;
    };
    std::vector<PendingSymlink> pending_symlinks;
    pending_symlinks.reserve(manifest.size());
    for (std::uint64_t i = 0; i < header.entry_count; ++i) {
        auto entry = read_entry_header(input, header.version);
        const fs::path output_path = destination / checked_relative_path(entry.path);
        if (is_directory(entry.type)) {
            fs::create_directories(output_path, ec);
            if (ec) {
                fail("cannot create restored directory: " + ec.message());
            }
            if (entry.checksum != kFnvOffset) {
                fail("checksum mismatch: " + entry.path);
            }
            continue;
        }

        if (is_symlink(entry.type)) {
            entry.link_target = read_symlink_target(input, entry);
            fs::create_directories(output_path.parent_path(), ec);
            if (ec) {
                fail("cannot create restored parent directory: " + ec.message());
            }
            if (fs::exists(output_path, ec) || fs::is_symlink(output_path, ec)) {
                fail("restore target already exists: " + entry.path);
            }
            pending_symlinks.push_back(
                {output_path, fs::u8path(entry.link_target),
                 entry.type == EntryType::directory_symlink
                     ? symlink::Kind::directory
                     : symlink::Kind::file});
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

    for (const auto& pending : pending_symlinks) {
        symlink::create(pending.target, pending.path, pending.kind, ec);
        if (ec) {
            fail("cannot create restored symlink: " + portable_path(pending.path) +
                 ": " + ec.message());
        }
    }
}

}  // namespace backup
