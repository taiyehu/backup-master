#include "backup/symlink.hpp"

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>

#include <array>
#include <cstddef>
#endif

namespace backup::symlink {

namespace fs = std::filesystem;

#ifdef _WIN32
namespace {

struct SymbolicLinkReparseData {
    ULONG reparse_tag;
    USHORT reparse_data_length;
    USHORT reserved;
    USHORT substitute_name_offset;
    USHORT substitute_name_length;
    USHORT print_name_offset;
    USHORT print_name_length;
    ULONG flags;
    WCHAR path_buffer[1];
};

static_assert(offsetof(SymbolicLinkReparseData, path_buffer) == 20,
              "unexpected reparse buffer layout");

struct ReparseBuffer {
    alignas(SymbolicLinkReparseData)
        std::array<unsigned char, MAXIMUM_REPARSE_DATA_BUFFER_SIZE> bytes{};
};

std::error_code windows_error() {
    return {static_cast<int>(GetLastError()), std::system_category()};
}

HANDLE open_reparse_point(const fs::path& path, std::error_code& error) {
    const HANDLE handle = CreateFileW(
        path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = windows_error();
    }
    return handle;
}

bool read_reparse_data(const fs::path& path,
                       ReparseBuffer& buffer,
                       DWORD& bytes_returned, std::error_code& error) {
    const HANDLE handle = open_reparse_point(path, error);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    const BOOL succeeded = DeviceIoControl(
        handle, FSCTL_GET_REPARSE_POINT, nullptr, 0, buffer.bytes.data(),
        static_cast<DWORD>(buffer.bytes.size()), &bytes_returned, nullptr);
    const auto operation_error = succeeded ? std::error_code{} : windows_error();
    CloseHandle(handle);
    error = operation_error;
    return succeeded != FALSE;
}

}  // namespace

bool is_symbolic_link(const fs::path& path, std::error_code& error) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        error = windows_error();
        return false;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
        error.clear();
        return false;
    }

    ReparseBuffer buffer;
    DWORD bytes_returned = 0;
    if (!read_reparse_data(path, buffer, bytes_returned, error)) {
        if (error.value() == ERROR_NOT_A_REPARSE_POINT) {
            error.clear();
            return false;
        }
        return false;
    }
    if (bytes_returned < sizeof(ULONG)) {
        error = std::make_error_code(std::errc::bad_message);
        return false;
    }
    const auto* data =
        reinterpret_cast<const SymbolicLinkReparseData*>(buffer.bytes.data());
    if (data->reparse_tag != IO_REPARSE_TAG_SYMLINK) {
        error = std::make_error_code(std::errc::not_supported);
        return false;
    }
    error.clear();
    return true;
}

Info inspect(const fs::path& path, std::error_code& error) {
    ReparseBuffer buffer;
    DWORD bytes_returned = 0;
    if (!read_reparse_data(path, buffer, bytes_returned, error)) {
        return {};
    }
    if (bytes_returned < offsetof(SymbolicLinkReparseData, path_buffer)) {
        error = std::make_error_code(std::errc::bad_message);
        return {};
    }
    const auto* data =
        reinterpret_cast<const SymbolicLinkReparseData*>(buffer.bytes.data());
    if (data->reparse_tag != IO_REPARSE_TAG_SYMLINK) {
        error = std::make_error_code(std::errc::not_supported);
        return {};
    }

    const auto target_end = offsetof(SymbolicLinkReparseData, path_buffer) +
                            data->print_name_offset + data->print_name_length;
    if (data->print_name_length == 0 || target_end > bytes_returned) {
        error = std::make_error_code(std::errc::bad_message);
        return {};
    }

    const auto* target_begin =
        data->path_buffer + data->print_name_offset / sizeof(wchar_t);
    const auto target_length = data->print_name_length / sizeof(wchar_t);
    const fs::path target(std::wstring(target_begin, target_length));

    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        error = windows_error();
        return {};
    }
    error.clear();
    return {target, (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0
                        ? Kind::directory
                        : Kind::file};
}

void create(const fs::path& target, const fs::path& link, Kind kind,
            std::error_code& error) {
    DWORD flags = kind == Kind::directory ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0;
#ifdef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
    flags |= SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
#endif
    if (CreateSymbolicLinkW(link.c_str(), target.c_str(), flags) == FALSE) {
        error = windows_error();
        return;
    }
    error.clear();
}

#else

bool is_symbolic_link(const fs::path& path, std::error_code& error) {
    const auto status = fs::symlink_status(path, error);
    return !error && fs::is_symlink(status);
}

Info inspect(const fs::path& path, std::error_code& error) {
    error.clear();
    const auto target = fs::read_symlink(path, error);
    if (error) {
        return {};
    }

    const auto target_status = fs::status(path, error);
    if (error) {
        return {};
    }
    if (fs::is_directory(target_status)) {
        return {target, Kind::directory};
    }
    if (fs::is_regular_file(target_status)) {
        return {target, Kind::file};
    }

    error = std::make_error_code(std::errc::not_supported);
    return {};
}

void create(const fs::path& target, const fs::path& link, Kind kind,
            std::error_code& error) {
    error.clear();
    if (kind == Kind::directory) {
        fs::create_directory_symlink(target, link, error);
    } else {
        fs::create_symlink(target, link, error);
    }
}

#endif

}  // namespace backup::symlink
