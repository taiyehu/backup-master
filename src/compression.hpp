#pragma once

#include <cstdint>
#include <filesystem>
#include <iosfwd>

namespace backup::compression {

struct EncodedFile {
    std::uint64_t size{};
    std::uint64_t checksum{};
};

EncodedFile encode_file(const std::filesystem::path& path,
                        std::uint64_t original_size, std::uint64_t seed,
                        std::ostream* output);
void decode_rle(std::istream& input, std::ostream* output,
                std::uint64_t stored_size, std::uint64_t original_size,
                std::uint64_t& checksum);

}  // namespace backup::compression
