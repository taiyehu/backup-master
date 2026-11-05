#include "compression.hpp"

#include "archive_internal.hpp"

#include <algorithm>
#include <array>
#include <fstream>

namespace backup::compression {
namespace {

void emit(std::ostream* output, const char* bytes, std::size_t count,
          EncodedFile& result) {
    result.checksum = internal::update_checksum(result.checksum, bytes, count);
    result.size += count;
    if (output) {
        output->write(bytes, static_cast<std::streamsize>(count));
        if (!*output) internal::fail("failed to write compressed archive data");
    }
}

}  // namespace

EncodedFile encode_file(const std::filesystem::path& path,
                        std::uint64_t original_size, std::uint64_t seed,
                        std::ostream* output) {
    std::ifstream input(path, std::ios::binary);
    if (!input) internal::fail("cannot open source file for compression: " +
                               internal::portable_path(path));
    EncodedFile result{0, seed};
    std::array<char, internal::kBufferSize> buffer{};
    std::uint64_t remaining = original_size;
    while (remaining) {
        const auto wanted = static_cast<std::streamsize>(
            std::min<std::uint64_t>(remaining, buffer.size()));
        input.read(buffer.data(), wanted);
        if (input.gcount() != wanted) internal::fail("source file changed during compression");
        const auto count = static_cast<std::size_t>(wanted);
        std::size_t position = 0;
        while (position < count) {
            std::size_t run = 1;
            while (position + run < count && run < 128 &&
                   buffer[position + run] == buffer[position]) ++run;
            if (run >= 3) {
                const char packet[2] = {
                    static_cast<char>(127U + run), buffer[position]};
                emit(output, packet, 2, result);
                position += run;
            } else {
                const auto start = position;
                position += run;
                while (position < count && position - start < 128) {
                    run = 1;
                    while (position + run < count && run < 3 &&
                           buffer[position + run] == buffer[position]) ++run;
                    if (run >= 3) break;
                    position += std::min(run, 128 - (position - start));
                }
                const char length = static_cast<char>(position - start - 1);
                emit(output, &length, 1, result);
                emit(output, buffer.data() + start, position - start, result);
            }
        }
        remaining -= count;
    }
    if (input.peek() != std::char_traits<char>::eof()) {
        internal::fail("source file changed during compression");
    }
    return result;
}

void decode_rle(std::istream& input, std::ostream* output,
                std::uint64_t stored_size, std::uint64_t original_size,
                std::uint64_t& checksum) {
    std::uint64_t consumed = 0;
    std::uint64_t produced = 0;
    std::array<char, 128> buffer{};
    while (consumed < stored_size) {
        const int token = input.get();
        if (token == std::char_traits<char>::eof())
            internal::fail("truncated compressed data");
        const char token_byte = static_cast<char>(token);
        checksum = internal::update_checksum(checksum, &token_byte, 1);
        ++consumed;
        const auto length = static_cast<std::uint64_t>(
            token < 128 ? token + 1 : token - 127);
        const auto payload_size = token < 128 ? length : 1U;
        if (payload_size > stored_size - consumed ||
            length > original_size - produced)
            internal::fail("invalid compressed length or expansion limit");
        input.read(buffer.data(), static_cast<std::streamsize>(payload_size));
        if (input.gcount() != static_cast<std::streamsize>(payload_size))
            internal::fail("truncated compressed data");
        checksum = internal::update_checksum(checksum, buffer.data(),
                                             static_cast<std::size_t>(payload_size));
        consumed += payload_size;
        if (token >= 128) buffer.fill(buffer[0]);
        if (output) {
            output->write(buffer.data(), static_cast<std::streamsize>(length));
            if (!*output) internal::fail("failed to write restored file");
        }
        produced += length;
    }
    if (produced != original_size) internal::fail("compressed size mismatch");
}

}  // namespace backup::compression
