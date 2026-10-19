#include "special_files.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>

#include <vector>
#else
#include <sys/stat.h>
#endif

namespace backup::special_files {
namespace fs = std::filesystem;

#ifdef _WIN32
namespace {

struct MountPointReparseData {
    ULONG reparse_tag;
    USHORT reparse_data_length;
    USHORT reserved;
    USHORT substitute_name_offset;
    USHORT substitute_name_length;
    USHORT print_name_offset;
    USHORT print_name_length;
    WCHAR path_buffer[1];
};

static_assert(offsetof(MountPointReparseData, path_buffer) == 16,
              "unexpected mount-point buffer layout");

std::error_code windows_error(DWORD value = GetLastError()) {
    return {static_cast<int>(value), std::system_category()};
}

HANDLE open_reparse(const fs::path& path, DWORD access,
                    std::error_code& error) {
    const HANDLE handle = CreateFileW(
        path.c_str(), access,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = windows_error();
    } else {
        error.clear();
    }
    return handle;
}

bool read_reparse(const fs::path& path,
                  std::vector<std::uint8_t>& buffer,
                  DWORD& returned, std::error_code& error) {
    const HANDLE handle = open_reparse(path, 0, error);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    buffer.resize(MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
    const BOOL ok = DeviceIoControl(
        handle, FSCTL_GET_REPARSE_POINT, nullptr, 0, buffer.data(),
        static_cast<DWORD>(buffer.size()), &returned, nullptr);
    const auto operation_error = ok ? std::error_code{} : windows_error();
    CloseHandle(handle);
    error = operation_error;
    return ok != FALSE;
}

}  // namespace

bool is_junction(const fs::path& path, std::error_code& error) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        error = windows_error();
        return false;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
        error.clear();
        return false;
    }
    std::vector<std::uint8_t> buffer;
    DWORD returned = 0;
    if (!read_reparse(path, buffer, returned, error)) {
        return false;
    }
    if (returned < sizeof(ULONG)) {
        error = std::make_error_code(std::errc::bad_message);
        return false;
    }
    const auto* data =
        reinterpret_cast<const MountPointReparseData*>(buffer.data());
    error.clear();
    return data->reparse_tag == IO_REPARSE_TAG_MOUNT_POINT;
}

fs::path inspect_junction(const fs::path& path, std::error_code& error) {
    std::vector<std::uint8_t> buffer;
    DWORD returned = 0;
    if (!read_reparse(path, buffer, returned, error)) {
        return {};
    }
    if (returned < offsetof(MountPointReparseData, path_buffer)) {
        error = std::make_error_code(std::errc::bad_message);
        return {};
    }
    const auto* data =
        reinterpret_cast<const MountPointReparseData*>(buffer.data());
    if (data->reparse_tag != IO_REPARSE_TAG_MOUNT_POINT) {
        error = std::make_error_code(std::errc::not_supported);
        return {};
    }
    const auto end = offsetof(MountPointReparseData, path_buffer) +
                     data->print_name_offset + data->print_name_length;
    if (end > returned) {
        error = std::make_error_code(std::errc::bad_message);
        return {};
    }
    const wchar_t* begin =
        data->path_buffer + data->print_name_offset / sizeof(wchar_t);
    std::wstring target(begin,
                        data->print_name_length / sizeof(wchar_t));
    if (target.empty()) {
        const auto substitute_end =
            offsetof(MountPointReparseData, path_buffer) +
            data->substitute_name_offset + data->substitute_name_length;
        if (substitute_end > returned) {
            error = std::make_error_code(std::errc::bad_message);
            return {};
        }
        begin = data->path_buffer +
                data->substitute_name_offset / sizeof(wchar_t);
        target.assign(begin,
                      data->substitute_name_length / sizeof(wchar_t));
        constexpr wchar_t kNtPrefix[] = L"\\??\\";
        if (target.rfind(kNtPrefix, 0) == 0) {
            target.erase(0, 4);
        }
    }
    error.clear();
    return fs::path(target);
}

void create_junction(const fs::path& target, const fs::path& link,
                     std::error_code& error) {
    const fs::path absolute_target = fs::absolute(target, error);
    if (error) {
        return;
    }
    fs::create_directory(link, error);
    if (error) {
        return;
    }
    const std::wstring print_name = absolute_target.wstring();
    const std::wstring substitute = L"\\??\\" + print_name;
    const std::size_t path_bytes =
        (substitute.size() + 1 + print_name.size() + 1) * sizeof(wchar_t);
    const std::size_t total =
        offsetof(MountPointReparseData, path_buffer) + path_bytes;
    std::vector<std::uint8_t> buffer(total);
    auto* data = reinterpret_cast<MountPointReparseData*>(buffer.data());
    data->reparse_tag = IO_REPARSE_TAG_MOUNT_POINT;
    data->reparse_data_length = static_cast<USHORT>(8U + path_bytes);
    data->reserved = 0;
    data->substitute_name_offset = 0;
    data->substitute_name_length =
        static_cast<USHORT>(substitute.size() * sizeof(wchar_t));
    data->print_name_offset = static_cast<USHORT>(
        (substitute.size() + 1) * sizeof(wchar_t));
    data->print_name_length =
        static_cast<USHORT>(print_name.size() * sizeof(wchar_t));
    std::memcpy(data->path_buffer, substitute.c_str(),
                (substitute.size() + 1) * sizeof(wchar_t));
    std::memcpy(reinterpret_cast<std::uint8_t*>(data->path_buffer) +
                    data->print_name_offset,
                print_name.c_str(),
                (print_name.size() + 1) * sizeof(wchar_t));

    const HANDLE handle = open_reparse(link, GENERIC_WRITE, error);
    if (handle == INVALID_HANDLE_VALUE) {
        fs::remove(link, error);
        return;
    }
    DWORD returned = 0;
    const BOOL ok = DeviceIoControl(
        handle, FSCTL_SET_REPARSE_POINT, buffer.data(),
        static_cast<DWORD>(total), nullptr, 0, &returned, nullptr);
    const auto operation_error = ok ? std::error_code{} : windows_error();
    CloseHandle(handle);
    if (!ok) {
        std::error_code ignored;
        fs::remove(link, ignored);
    }
    error = operation_error;
}

FileIdentity file_identity(const fs::path& path, std::error_code& error) {
    const HANDLE handle = CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = windows_error();
        return {};
    }
    FILE_ID_INFO id_info{};
    BY_HANDLE_FILE_INFORMATION basic{};
    if (!GetFileInformationByHandleEx(handle, FileIdInfo, &id_info,
                                      sizeof(id_info)) ||
        !GetFileInformationByHandle(handle, &basic)) {
        error = windows_error();
        CloseHandle(handle);
        return {};
    }
    CloseHandle(handle);
    FileIdentity result;
    result.volume = id_info.VolumeSerialNumber;
    std::copy(std::begin(id_info.FileId.Identifier),
              std::end(id_info.FileId.Identifier), result.id.begin());
    result.link_count = basic.nNumberOfLinks;
    error.clear();
    return result;
}

void create_hardlink(const fs::path& existing, const fs::path& link,
                     std::error_code& error) {
    if (!CreateHardLinkW(link.c_str(), existing.c_str(), nullptr)) {
        error = windows_error();
    } else {
        error.clear();
    }
}

#else

bool is_junction(const fs::path&, std::error_code& error) {
    error.clear();
    return false;
}

fs::path inspect_junction(const fs::path&, std::error_code& error) {
    error = std::make_error_code(std::errc::not_supported);
    return {};
}

void create_junction(const fs::path&, const fs::path&,
                     std::error_code& error) {
    error = std::make_error_code(std::errc::not_supported);
}

FileIdentity file_identity(const fs::path& path, std::error_code& error) {
    struct stat status {};
    if (::stat(path.c_str(), &status) != 0) {
        error = std::error_code(errno, std::generic_category());
        return {};
    }
    FileIdentity result;
    result.volume = static_cast<std::uint64_t>(status.st_dev);
    const auto inode = static_cast<std::uint64_t>(status.st_ino);
    std::memcpy(result.id.data(), &inode, sizeof(inode));
    result.link_count = static_cast<std::uint32_t>(status.st_nlink);
    error.clear();
    return result;
}

void create_hardlink(const fs::path& existing, const fs::path& link,
                     std::error_code& error) {
    fs::create_hard_link(existing, link, error);
}

#endif

}  // namespace backup::special_files
