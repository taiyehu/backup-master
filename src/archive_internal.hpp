#pragma once

#include "backup/archive.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iosfwd>
#include <string>
#include <vector>

namespace backup::internal {

namespace fs = std::filesystem;

constexpr std::uint32_t kLegacyVersion = 1;
constexpr std::uint32_t kSymlinkVersion = 2;
constexpr std::uint32_t kCurrentVersion = 3;
constexpr std::uint32_t kMaxPathBytes = 1024U * 1024U;
constexpr std::uint32_t kMaxMetadataBytes = 1024U * 1024U;
constexpr std::uint64_t kMaxReferenceBytes = 32U * 1024U;
constexpr std::uint64_t kMaxEntries = 10'000'000ULL;
constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::size_t kBufferSize = 64U * 1024U;

struct ArchiveHeader {
    std::uint32_t version;
    std::uint64_t entry_count;
};

struct SourceEntry {
    fs::path absolute;
    std::string relative;
    EntryType type;
    std::uint64_t data_size{};
    std::uint64_t logical_size{};
    std::string reference_target;
    FileMetadata metadata;
    std::vector<std::uint8_t> encoded_metadata;
};

struct DecodedEntry {
    EntryInfo info;
    std::uint64_t data_size{};
    std::vector<std::uint8_t> encoded_metadata;
};

[[noreturn]] void fail(const std::string& message);
std::string portable_path(const fs::path& path);
fs::path checked_relative_path(const std::string& stored);

std::uint64_t update_checksum(std::uint64_t checksum, const char* data,
                              std::size_t size);
std::uint64_t checksum_bytes(const std::string& data,
                             std::uint64_t seed = kFnvOffset);
std::uint64_t checksum_bytes(const std::vector<std::uint8_t>& data,
                             std::uint64_t seed = kFnvOffset);

void write_header(std::ostream& output, std::uint64_t entry_count);
ArchiveHeader read_header(std::istream& input);
void write_entry_header(std::ostream& output, const SourceEntry& entry,
                        std::uint64_t checksum);
DecodedEntry read_entry_header(std::istream& input, std::uint32_t version);
void copy_exact(std::istream& input, std::ostream* output, std::uint64_t size,
                std::uint64_t& checksum);
std::string read_reference(std::istream& input, const DecodedEntry& entry,
                           std::uint64_t checksum_seed,
                           bool require_checksum = true);
void ensure_archive_finished(std::istream& input);
std::ifstream open_archive(const fs::path& archive);

std::vector<std::uint8_t> encode_metadata(const FileMetadata& metadata);
FileMetadata decode_metadata(const std::vector<std::uint8_t>& bytes);

std::vector<SourceEntry> scan_source(const fs::path& source,
                                     const BackupOptions& options);
void validate_manifest(std::vector<EntryInfo>& entries);

}  // namespace backup::internal
