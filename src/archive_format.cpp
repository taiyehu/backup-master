#include "archive_internal.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace backup::internal {
namespace {

constexpr std::array<char, 8> kMagic{'B', 'K', 'M', 'A', 'S', 'T', 'R', '1'};
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

template <typename UInt>
void write_unsigned(std::ostream& output, UInt value) {
    static_assert(std::is_unsigned<UInt>::value, "unsigned integer required");
    for (std::size_t i = 0; i < sizeof(UInt); ++i) {
        output.put(static_cast<char>((value >> (i * 8U)) & 0xffU));
    }
    if (!output) {
        fail("failed to write archive");
    }
}

template <typename UInt>
UInt read_unsigned(std::istream& input, const char* field) {
    static_assert(std::is_unsigned<UInt>::value, "unsigned integer required");
    UInt value = 0;
    for (std::size_t i = 0; i < sizeof(UInt); ++i) {
        const int byte = input.get();
        if (byte == std::char_traits<char>::eof()) {
            fail(std::string("truncated archive while reading ") + field);
        }
        value |= static_cast<UInt>(static_cast<unsigned char>(byte)) <<
                 (i * 8U);
    }
    return value;
}

bool valid_type(std::uint8_t raw_type, std::uint32_t version) {
    if (raw_type == static_cast<std::uint8_t>(EntryType::file) ||
        raw_type == static_cast<std::uint8_t>(EntryType::directory)) {
        return true;
    }
    if (version >= kSymlinkVersion &&
        (raw_type == static_cast<std::uint8_t>(EntryType::file_symlink) ||
         raw_type == static_cast<std::uint8_t>(EntryType::directory_symlink))) {
        return true;
    }
    return version >= kCurrentVersion &&
           (raw_type == static_cast<std::uint8_t>(EntryType::junction) ||
            raw_type == static_cast<std::uint8_t>(EntryType::hardlink));
}

}  // namespace

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

std::string portable_path(const fs::path& path) {
    return path.generic_u8string();
}

fs::path checked_relative_path(const std::string& stored) {
    if (stored.empty() || stored.size() > kMaxPathBytes ||
        stored.find('\\') != std::string::npos ||
        stored.find('\0') != std::string::npos) {
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

std::uint64_t update_checksum(std::uint64_t checksum, const char* data,
                              std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) {
        checksum ^= static_cast<unsigned char>(data[i]);
        checksum *= kFnvPrime;
    }
    return checksum;
}

std::uint64_t checksum_bytes(const std::string& data, std::uint64_t seed) {
    return update_checksum(seed, data.data(), data.size());
}

std::uint64_t checksum_bytes(const std::vector<std::uint8_t>& data,
                             std::uint64_t seed) {
    return update_checksum(seed, reinterpret_cast<const char*>(data.data()),
                           data.size());
}

void write_header(std::ostream& output, std::uint64_t entry_count) {
    output.write(kMagic.data(), static_cast<std::streamsize>(kMagic.size()));
    write_unsigned(output, kCurrentVersion);
    write_unsigned(output, entry_count);
}

ArchiveHeader read_header(std::istream& input) {
    std::array<char, kMagic.size()> magic{};
    input.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (!input || magic != kMagic) {
        fail("not a BackupMaster archive");
    }
    const auto version = read_unsigned<std::uint32_t>(input, "version");
    if (version < kLegacyVersion || version > kCurrentVersion) {
        fail("unsupported archive version: " + std::to_string(version));
    }
    const auto count = read_unsigned<std::uint64_t>(input, "entry count");
    if (count > kMaxEntries) {
        fail("archive entry count is unreasonable");
    }
    return {version, count};
}

void write_entry_header(std::ostream& output, const SourceEntry& entry,
                        std::uint64_t checksum) {
    write_unsigned(output, static_cast<std::uint8_t>(entry.type));
    write_unsigned(output, static_cast<std::uint32_t>(entry.relative.size()));
    write_unsigned(output,
                   static_cast<std::uint32_t>(entry.encoded_metadata.size()));
    write_unsigned(output, entry.data_size);
    write_unsigned(output, checksum);
    output.write(entry.relative.data(),
                 static_cast<std::streamsize>(entry.relative.size()));
    if (!entry.encoded_metadata.empty()) {
        output.write(reinterpret_cast<const char*>(entry.encoded_metadata.data()),
                     static_cast<std::streamsize>(entry.encoded_metadata.size()));
    }
    if (!output) {
        fail("failed to write archive entry");
    }
}

DecodedEntry read_entry_header(std::istream& input, std::uint32_t version) {
    const auto raw_type = read_unsigned<std::uint8_t>(input, "entry type");
    if (!valid_type(raw_type, version)) {
        fail("archive contains an unknown entry type");
    }
    const auto path_size = read_unsigned<std::uint32_t>(input, "path length");
    const auto metadata_size = version >= kCurrentVersion
                                   ? read_unsigned<std::uint32_t>(
                                         input, "metadata length")
                                   : 0U;
    const auto data_size = read_unsigned<std::uint64_t>(input, "data size");
    const auto checksum = read_unsigned<std::uint64_t>(input, "checksum");
    if (path_size == 0 || path_size > kMaxPathBytes) {
        fail("archive contains an invalid path length");
    }
    if (metadata_size > kMaxMetadataBytes) {
        fail("archive metadata is too large");
    }

    std::string path(path_size, '\0');
    input.read(path.data(), static_cast<std::streamsize>(path.size()));
    if (!input) {
        fail("truncated archive while reading path");
    }
    checked_relative_path(path);

    std::vector<std::uint8_t> encoded_metadata(metadata_size);
    if (metadata_size != 0) {
        input.read(reinterpret_cast<char*>(encoded_metadata.data()),
                   static_cast<std::streamsize>(encoded_metadata.size()));
        if (!input) {
            fail("truncated archive while reading metadata");
        }
    }

    const auto type = static_cast<EntryType>(raw_type);
    if (is_directory(type) && data_size != 0) {
        fail("directory entry has non-zero data size");
    }
    if (has_reference_target(type) && data_size > kMaxReferenceBytes) {
        fail("archive reference target is too long: " + path);
    }

    EntryInfo info{path, type, data_size, checksum, {},
                   decode_metadata(encoded_metadata)};
    return {std::move(info), data_size, std::move(encoded_metadata)};
}

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
            fail("truncated entry data in archive");
        }
        if (output != nullptr) {
            output->write(buffer.data(), count);
            if (!*output) {
                fail("failed to write restored file");
            }
        }
        checksum = update_checksum(checksum, buffer.data(),
                                   static_cast<std::size_t>(count));
        remaining -= static_cast<std::uint64_t>(count);
    }
}

std::string read_reference(std::istream& input, const DecodedEntry& entry,
                           std::uint64_t checksum_seed,
                           bool require_checksum) {
    std::string target(static_cast<std::size_t>(entry.data_size), '\0');
    input.read(target.data(), static_cast<std::streamsize>(target.size()));
    if (!input) {
        fail("truncated reference target in archive: " + entry.info.path);
    }
    if (require_checksum &&
        checksum_bytes(target, checksum_seed) != entry.info.checksum) {
        fail("checksum mismatch: " + entry.info.path);
    }
    return target;
}

void ensure_archive_finished(std::istream& input) {
    if (input.peek() != std::char_traits<char>::eof()) {
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

}  // namespace backup::internal
