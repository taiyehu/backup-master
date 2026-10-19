#include "metadata.hpp"

#include "archive_internal.hpp"

#include <cstring>
#include <iomanip>
#include <sstream>
#include <type_traits>

#ifdef _WIN32
#include <aclapi.h>
#include <sddl.h>
#include <windows.h>
#endif

namespace backup::internal {
namespace {

constexpr std::uint8_t kWindowsMetadataSchema = 1;
constexpr std::uint16_t kAllWindowsFields = 0x0007U;
constexpr std::uint32_t kMaxSecurityDescriptorBytes = 64U * 1024U;

template <typename UInt>
void append_unsigned(std::vector<std::uint8_t>& output, UInt value) {
    static_assert(std::is_unsigned<UInt>::value, "unsigned integer required");
    for (std::size_t i = 0; i < sizeof(UInt); ++i) {
        output.push_back(static_cast<std::uint8_t>((value >> (i * 8U)) & 0xffU));
    }
}

template <typename UInt>
UInt take_unsigned(const std::vector<std::uint8_t>& input, std::size_t& offset,
                   const char* field) {
    static_assert(std::is_unsigned<UInt>::value, "unsigned integer required");
    if (offset > input.size() || input.size() - offset < sizeof(UInt)) {
        fail(std::string("truncated metadata while reading ") + field);
    }
    UInt value = 0;
    for (std::size_t i = 0; i < sizeof(UInt); ++i) {
        value |= static_cast<UInt>(input[offset++]) << (i * 8U);
    }
    return value;
}

void append_signed(std::vector<std::uint8_t>& output, std::int64_t value) {
    append_unsigned(output, static_cast<std::uint64_t>(value));
}

std::int64_t take_signed(const std::vector<std::uint8_t>& input,
                         std::size_t& offset, const char* field) {
    return static_cast<std::int64_t>(
        take_unsigned<std::uint64_t>(input, offset, field));
}

}  // namespace

std::vector<std::uint8_t> encode_metadata(const FileMetadata& metadata) {
    if (metadata.platform == MetadataPlatform::none) {
        return {};
    }
    if (metadata.platform != MetadataPlatform::windows ||
        !metadata.windows.has_value()) {
        fail("cannot encode unsupported metadata platform");
    }

    const auto& windows = *metadata.windows;
    if (windows.owner_sid.size() > kMaxPathBytes ||
        windows.security_descriptor.size() > kMaxSecurityDescriptorBytes) {
        fail("Windows metadata is too large");
    }
    std::vector<std::uint8_t> output;
    output.reserve(48U + windows.owner_sid.size() +
                   windows.security_descriptor.size());
    append_unsigned(output, static_cast<std::uint8_t>(MetadataPlatform::windows));
    append_unsigned(output, kWindowsMetadataSchema);
    append_unsigned(output, kAllWindowsFields);
    append_signed(output, windows.creation_time);
    append_signed(output, windows.access_time);
    append_signed(output, windows.write_time);
    append_signed(output, windows.change_time);
    append_unsigned(output, windows.attributes);
    append_unsigned(output,
                    static_cast<std::uint32_t>(windows.owner_sid.size()));
    append_unsigned(output, static_cast<std::uint32_t>(
                                windows.security_descriptor.size()));
    output.insert(output.end(), windows.owner_sid.begin(),
                  windows.owner_sid.end());
    output.insert(output.end(), windows.security_descriptor.begin(),
                  windows.security_descriptor.end());
    if (output.size() > kMaxMetadataBytes) {
        fail("encoded metadata is too large");
    }
    return output;
}

FileMetadata decode_metadata(const std::vector<std::uint8_t>& bytes) {
    if (bytes.empty()) {
        return {};
    }
    std::size_t offset = 0;
    const auto platform = take_unsigned<std::uint8_t>(bytes, offset, "platform");
    if (platform != static_cast<std::uint8_t>(MetadataPlatform::windows)) {
        fail("archive contains unsupported metadata platform");
    }
    const auto schema = take_unsigned<std::uint8_t>(bytes, offset, "schema");
    if (schema != kWindowsMetadataSchema) {
        fail("archive contains unsupported Windows metadata schema");
    }
    const auto flags = take_unsigned<std::uint16_t>(bytes, offset, "flags");
    if (flags != kAllWindowsFields) {
        fail("archive contains unsupported Windows metadata fields");
    }

    WindowsMetadata windows;
    windows.creation_time = take_signed(bytes, offset, "creation time");
    windows.access_time = take_signed(bytes, offset, "access time");
    windows.write_time = take_signed(bytes, offset, "write time");
    windows.change_time = take_signed(bytes, offset, "change time");
    windows.attributes =
        take_unsigned<std::uint32_t>(bytes, offset, "attributes");
    const auto owner_size =
        take_unsigned<std::uint32_t>(bytes, offset, "owner SID length");
    const auto descriptor_size = take_unsigned<std::uint32_t>(
        bytes, offset, "security descriptor length");
    if (owner_size > kMaxPathBytes ||
        descriptor_size > kMaxSecurityDescriptorBytes || offset > bytes.size() ||
        static_cast<std::uint64_t>(owner_size) + descriptor_size >
            bytes.size() - offset) {
        fail("archive contains invalid Windows metadata lengths");
    }
    windows.owner_sid.assign(
        reinterpret_cast<const char*>(bytes.data() + offset), owner_size);
    offset += owner_size;
    windows.security_descriptor.assign(bytes.begin() + offset,
                                       bytes.begin() + offset + descriptor_size);
    offset += descriptor_size;
    if (offset != bytes.size()) {
        fail("archive metadata contains trailing data");
    }
#ifdef _WIN32
    if (windows.security_descriptor.empty() ||
        !IsValidSecurityDescriptor(
            const_cast<std::uint8_t*>(
                windows.security_descriptor.data()))) {
        fail("archive contains an invalid Windows security descriptor");
    }
#endif
    return {MetadataPlatform::windows, std::move(windows)};
}

}  // namespace backup::internal

namespace backup::metadata {
namespace fs = std::filesystem;

#ifdef _WIN32
namespace {

std::error_code windows_error(DWORD value = GetLastError()) {
    return {static_cast<int>(value), std::system_category()};
}

HANDLE open_path(const fs::path& path, DWORD access, bool open_reparse,
                 std::error_code& error) {
    DWORD flags = FILE_FLAG_BACKUP_SEMANTICS;
    if (open_reparse) {
        flags |= FILE_FLAG_OPEN_REPARSE_POINT;
    }
    const HANDLE handle = CreateFileW(
        path.c_str(), access,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, flags, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = windows_error();
    } else {
        error.clear();
    }
    return handle;
}

std::string sid_string(PSID sid, std::error_code& error) {
    LPWSTR text = nullptr;
    if (!ConvertSidToStringSidW(sid, &text)) {
        error = windows_error();
        return {};
    }
    const std::string result = fs::path(text).u8string();
    LocalFree(text);
    error.clear();
    return result;
}

}  // namespace

FileMetadata capture(const fs::path& path, bool open_reparse,
                     std::error_code& error) {
    const HANDLE handle = open_path(path, FILE_READ_ATTRIBUTES | READ_CONTROL,
                                    open_reparse, error);
    if (handle == INVALID_HANDLE_VALUE) {
        return {};
    }

    FILE_BASIC_INFO basic{};
    if (!GetFileInformationByHandleEx(handle, FileBasicInfo, &basic,
                                      sizeof(basic))) {
        error = windows_error();
        CloseHandle(handle);
        return {};
    }

    PSID owner = nullptr;
    PSID group = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD security_error = GetSecurityInfo(
        handle, SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION |
            DACL_SECURITY_INFORMATION,
        &owner, &group, &dacl, nullptr, &descriptor);
    CloseHandle(handle);
    if (security_error != ERROR_SUCCESS) {
        error = windows_error(security_error);
        return {};
    }

    WindowsMetadata windows;
    windows.creation_time = basic.CreationTime.QuadPart;
    windows.access_time = basic.LastAccessTime.QuadPart;
    windows.write_time = basic.LastWriteTime.QuadPart;
    windows.change_time = basic.ChangeTime.QuadPart;
    windows.attributes = basic.FileAttributes;
    windows.owner_sid = sid_string(owner, error);
    if (error) {
        LocalFree(descriptor);
        return {};
    }
    const DWORD descriptor_size = GetSecurityDescriptorLength(descriptor);
    windows.security_descriptor.resize(descriptor_size);
    std::memcpy(windows.security_descriptor.data(), descriptor,
                descriptor_size);
    LocalFree(descriptor);
    error.clear();
    return {MetadataPlatform::windows, std::move(windows)};
}

void apply(const fs::path& path, const FileMetadata& value, bool open_reparse,
           std::error_code& error) {
    if (value.platform == MetadataPlatform::none) {
        error.clear();
        return;
    }
    if (value.platform != MetadataPlatform::windows || !value.windows) {
        error = std::make_error_code(std::errc::not_supported);
        return;
    }
    const auto& windows = *value.windows;

    HANDLE handle = open_path(path, FILE_WRITE_ATTRIBUTES, open_reparse, error);
    if (handle == INVALID_HANDLE_VALUE) {
        return;
    }
    FILE_BASIC_INFO basic{};
    basic.CreationTime.QuadPart = windows.creation_time;
    basic.LastAccessTime.QuadPart = windows.access_time;
    basic.LastWriteTime.QuadPart = windows.write_time;
    basic.ChangeTime.QuadPart = windows.change_time;
    basic.FileAttributes = windows.attributes;
    if (!SetFileInformationByHandle(handle, FileBasicInfo, &basic,
                                    sizeof(basic))) {
        error = windows_error();
        CloseHandle(handle);
        return;
    }
    CloseHandle(handle);

    if (windows.security_descriptor.empty() ||
        !IsValidSecurityDescriptor(const_cast<std::uint8_t*>(
            windows.security_descriptor.data()))) {
        error = std::make_error_code(std::errc::invalid_argument);
        return;
    }
    handle = open_path(path, WRITE_DAC | WRITE_OWNER | READ_CONTROL,
                       open_reparse, error);
    if (handle == INVALID_HANDLE_VALUE) {
        return;
    }
    auto* descriptor = reinterpret_cast<PSECURITY_DESCRIPTOR>(
        const_cast<std::uint8_t*>(windows.security_descriptor.data()));
    PSID owner = nullptr;
    PSID group = nullptr;
    PACL dacl = nullptr;
    BOOL owner_defaulted = FALSE;
    BOOL group_defaulted = FALSE;
    BOOL dacl_present = FALSE;
    BOOL dacl_defaulted = FALSE;
    if (!GetSecurityDescriptorOwner(descriptor, &owner, &owner_defaulted) ||
        !GetSecurityDescriptorGroup(descriptor, &group, &group_defaulted) ||
        !GetSecurityDescriptorDacl(descriptor, &dacl_present, &dacl,
                                   &dacl_defaulted)) {
        error = windows_error();
        CloseHandle(handle);
        return;
    }
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    if (!GetSecurityDescriptorControl(descriptor, &control, &revision)) {
        error = windows_error();
        CloseHandle(handle);
        return;
    }
    SECURITY_INFORMATION information =
        OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION |
        DACL_SECURITY_INFORMATION;
    information |= (control & SE_DACL_PROTECTED) != 0
                       ? PROTECTED_DACL_SECURITY_INFORMATION
                       : UNPROTECTED_DACL_SECURITY_INFORMATION;
    const DWORD result = SetSecurityInfo(
        handle, SE_FILE_OBJECT, information, owner, group,
        dacl_present ? dacl : nullptr, nullptr);
    CloseHandle(handle);
    error = result == ERROR_SUCCESS ? std::error_code{} : windows_error(result);
}

std::string canonical_owner(const std::string& owner,
                            std::error_code& error) {
    PSID sid = nullptr;
    const auto wide = fs::u8path(owner).wstring();
    if (ConvertStringSidToSidW(wide.c_str(), &sid)) {
        const auto result = sid_string(sid, error);
        LocalFree(sid);
        return result;
    }

    DWORD sid_size = 0;
    DWORD domain_size = 0;
    SID_NAME_USE use{};
    LookupAccountNameW(nullptr, wide.c_str(), nullptr, &sid_size, nullptr,
                       &domain_size, &use);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        error = windows_error();
        return {};
    }
    std::vector<std::uint8_t> sid_buffer(sid_size);
    std::vector<wchar_t> domain(domain_size);
    if (!LookupAccountNameW(nullptr, wide.c_str(), sid_buffer.data(), &sid_size,
                            domain.data(), &domain_size, &use)) {
        error = windows_error();
        return {};
    }
    return sid_string(sid_buffer.data(), error);
}

std::string format_utc(std::int64_t timestamp) {
    ULARGE_INTEGER value{};
    value.QuadPart = static_cast<ULONGLONG>(timestamp);
    FILETIME file_time{value.LowPart, value.HighPart};
    SYSTEMTIME system_time{};
    if (!FileTimeToSystemTime(&file_time, &system_time)) {
        return "-";
    }
    std::ostringstream output;
    output << std::setfill('0') << std::setw(4) << system_time.wYear << '-'
           << std::setw(2) << system_time.wMonth << '-' << std::setw(2)
           << system_time.wDay << 'T' << std::setw(2) << system_time.wHour
           << ':' << std::setw(2) << system_time.wMinute << ':'
           << std::setw(2) << system_time.wSecond << 'Z';
    return output.str();
}

#else

FileMetadata capture(const fs::path&, bool, std::error_code& error) {
    error.clear();
    return {};
}

void apply(const fs::path&, const FileMetadata& value, bool,
           std::error_code& error) {
    error = value.platform == MetadataPlatform::none
                ? std::error_code{}
                : std::make_error_code(std::errc::not_supported);
}

std::string canonical_owner(const std::string&, std::error_code& error) {
    error = std::make_error_code(std::errc::not_supported);
    return {};
}

std::string format_utc(std::int64_t timestamp) {
    return std::to_string(timestamp);
}

#endif

}  // namespace backup::metadata
