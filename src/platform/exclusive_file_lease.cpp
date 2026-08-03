#include "platform/exclusive_file_lease.h"
#include "platform/win32_file_identity.h"

#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace specforge {

ExclusiveFileLease::ExclusiveFileLease(void* native_handle) noexcept
    : native_handle_(native_handle)
{
}

ExclusiveFileLease::~ExclusiveFileLease()
{
    Reset();
}

ExclusiveFileLease::ExclusiveFileLease(
    ExclusiveFileLease&& other) noexcept
    : native_handle_(std::exchange(other.native_handle_, nullptr))
{
}

ExclusiveFileLease& ExclusiveFileLease::operator=(
    ExclusiveFileLease&& other) noexcept
{
    if (this != &other) {
        Reset();
        native_handle_ =
            std::exchange(other.native_handle_, nullptr);
    }
    return *this;
}

ExclusiveFileLease::operator bool() const noexcept
{
    return native_handle_ != nullptr;
}

void ExclusiveFileLease::Reset() noexcept
{
    if (native_handle_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(native_handle_));
        native_handle_ = nullptr;
    }
}

ExclusiveFileLeaseAcquireResult TryAcquireExclusiveFileLease(
    const std::filesystem::path& path)
{
    ExclusiveFileLeaseAcquireResult result;
    if (path.empty()) {
        result.error = "exclusive file lease path is empty";
        return result;
    }

    const std::filesystem::path lease_path =
        Win32ExtendedLengthPath(path);
    if (lease_path.empty()) {
        result.error =
            "could not resolve exclusive file lease path";
        return result;
    }

    std::error_code directory_error;
    const std::filesystem::path parent =
        lease_path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(
            parent,
            directory_error);
        if (directory_error) {
            result.error =
                "could not create exclusive file lease directory: " +
                directory_error.message();
            return result;
        }
    }

    const HANDLE handle =
        CreateFileW(
            lease_path.c_str(),
            GENERIC_READ | GENERIC_WRITE | DELETE,
            0,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_HIDDEN |
                FILE_ATTRIBUTE_NOT_CONTENT_INDEXED |
                FILE_FLAG_DELETE_ON_CLOSE,
            nullptr);
    if (handle != INVALID_HANDLE_VALUE) {
        result.lease = ExclusiveFileLease(handle);
        result.status =
            ExclusiveFileLeaseAcquireStatus::Acquired;
        return result;
    }

    const DWORD error = GetLastError();
    if (error == ERROR_SHARING_VIOLATION ||
        error == ERROR_LOCK_VIOLATION) {
        result.status =
            ExclusiveFileLeaseAcquireStatus::Unavailable;
        return result;
    }
    result.error =
        std::error_code(
            static_cast<int>(error),
            std::system_category())
            .message();
    return result;
}

}  // namespace specforge
