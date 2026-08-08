#pragma once

#include <Windows.h>
#include <winternl.h>

#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace specforge {

[[nodiscard]] inline std::filesystem::path
Win32FullPath(const std::filesystem::path& path)
{
    const DWORD required =
        GetFullPathNameW(
            path.c_str(),
            0,
            nullptr,
            nullptr);
    if (required == 0) {
        return {};
    }
    std::wstring buffer(required, L'\0');
    const DWORD written =
        GetFullPathNameW(
            path.c_str(),
            required,
            buffer.data(),
            nullptr);
    if (written == 0 || written >= required) {
        return {};
    }
    buffer.resize(written);
    std::filesystem::path full_path =
        std::filesystem::path(
            std::move(buffer))
            .lexically_normal();

    const DWORD long_required =
        GetLongPathNameW(
            full_path.c_str(),
            nullptr,
            0);
    if (long_required == 0) {
        return full_path;
    }
    std::wstring long_buffer(
        long_required,
        L'\0');
    const DWORD long_written =
        GetLongPathNameW(
            full_path.c_str(),
            long_buffer.data(),
            long_required);
    if (long_written == 0 ||
        long_written >= long_required) {
        return full_path;
    }
    long_buffer.resize(long_written);
    return std::filesystem::path(
        std::move(long_buffer))
        .lexically_normal();
}

[[nodiscard]] inline std::filesystem::path
Win32ExtendedLengthPath(const std::filesystem::path& path)
{
    std::filesystem::path full_path =
        Win32FullPath(path);
    if (full_path.empty()) {
        return {};
    }

    std::wstring text = full_path.wstring();
    constexpr std::wstring_view kExtendedPrefix =
        L"\\\\?\\";
    constexpr std::wstring_view kUncPrefix =
        L"\\\\";
    if (text.starts_with(kExtendedPrefix)) {
        return full_path;
    }
    if (text.starts_with(kUncPrefix)) {
        text = L"\\\\?\\UNC\\" +
            text.substr(kUncPrefix.size());
    } else {
        text = L"\\\\?\\" + text;
    }
    return std::filesystem::path(
        std::move(text));
}

[[nodiscard]] inline std::filesystem::path
Win32FinalPathByHandle(HANDLE handle)
{
    if (handle == nullptr ||
        handle == INVALID_HANDLE_VALUE) {
        return {};
    }
    constexpr DWORD kFlags =
        FILE_NAME_NORMALIZED |
        VOLUME_NAME_DOS;
    const DWORD required =
        GetFinalPathNameByHandleW(
            handle,
            nullptr,
            0,
            kFlags);
    if (required == 0) {
        return {};
    }
    std::wstring buffer(required, L'\0');
    const DWORD written =
        GetFinalPathNameByHandleW(
            handle,
            buffer.data(),
            required,
            kFlags);
    if (written == 0 || written >= required) {
        return {};
    }
    buffer.resize(written);
    constexpr std::wstring_view kExtendedUnc =
        L"\\\\?\\UNC\\";
    constexpr std::wstring_view kExtended =
        L"\\\\?\\";
    if (buffer.starts_with(kExtendedUnc)) {
        buffer =
            L"\\\\" +
            buffer.substr(kExtendedUnc.size());
    } else if (buffer.starts_with(kExtended)) {
        buffer.erase(0, kExtended.size());
    }
    return std::filesystem::path(
        std::move(buffer))
        .lexically_normal();
}

[[nodiscard]] inline bool Win32PathsEqualOrdinal(
    const std::filesystem::path& left,
    const std::filesystem::path& right)
{
    const auto equal_ordinal =
        [](const std::filesystem::path& lhs,
           const std::filesystem::path& rhs) {
            const std::wstring left_text =
                lhs.wstring();
            const std::wstring right_text =
                rhs.wstring();
            if (left_text.size() >
                    static_cast<std::size_t>(
                        (std::numeric_limits<int>::max)()) ||
                right_text.size() >
                    static_cast<std::size_t>(
                        (std::numeric_limits<int>::max)())) {
                return false;
            }
            return CompareStringOrdinal(
                       left_text.data(),
                       static_cast<int>(
                           left_text.size()),
                       right_text.data(),
                       static_cast<int>(
                           right_text.size()),
                       TRUE) == CSTR_EQUAL;
        };

    if (equal_ordinal(left, right)) {
        return true;
    }

    const std::filesystem::path normalized_left =
        Win32FullPath(left);
    const std::filesystem::path normalized_right =
        Win32FullPath(right);
    return !normalized_left.empty() &&
           !normalized_right.empty() &&
           equal_ordinal(
               normalized_left,
               normalized_right);
}

[[nodiscard]] inline HRESULT
ValidateWin32DirectoryHandleNoFollow(HANDLE handle)
{
    FILE_ATTRIBUTE_TAG_INFO information = {};
    if (GetFileInformationByHandleEx(
            handle,
            FileAttributeTagInfo,
            &information,
            sizeof(information)) == FALSE) {
        return HRESULT_FROM_WIN32(
            GetLastError());
    }
    if ((information.FileAttributes &
         FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (information.FileAttributes &
         FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return HRESULT_FROM_WIN32(
            ERROR_REPARSE_TAG_INVALID);
    }
    return S_OK;
}

[[nodiscard]] inline HRESULT
ValidateWin32RegularFileHandleNoFollow(HANDLE handle)
{
    FILE_ATTRIBUTE_TAG_INFO information = {};
    if (GetFileInformationByHandleEx(
            handle,
            FileAttributeTagInfo,
            &information,
            sizeof(information)) == FALSE) {
        return HRESULT_FROM_WIN32(
            GetLastError());
    }
    if ((information.FileAttributes &
         FILE_ATTRIBUTE_DIRECTORY) != 0 ||
        (information.FileAttributes &
         FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return HRESULT_FROM_WIN32(
            ERROR_REPARSE_TAG_INVALID);
    }
    return S_OK;
}

[[nodiscard]] inline HANDLE OpenWin32Relative(
    HANDLE root,
    std::wstring_view name,
    ACCESS_MASK access,
    ULONG share_access,
    ULONG disposition,
    ULONG options,
    ULONG attributes,
    NTSTATUS* status_out = nullptr)
{
    if (root == nullptr ||
        root == INVALID_HANDLE_VALUE ||
        name.empty() ||
        name.size() >
            (std::numeric_limits<USHORT>::max)() /
                sizeof(wchar_t)) {
        if (status_out != nullptr) {
            *status_out =
                static_cast<NTSTATUS>(
                    0xC000000DL);
        }
        return INVALID_HANDLE_VALUE;
    }
    UNICODE_STRING unicode_name = {};
    unicode_name.Buffer =
        const_cast<wchar_t*>(name.data());
    unicode_name.Length =
        static_cast<USHORT>(
            name.size() * sizeof(wchar_t));
    unicode_name.MaximumLength =
        unicode_name.Length;
    OBJECT_ATTRIBUTES object_attributes = {};
    InitializeObjectAttributes(
        &object_attributes,
        &unicode_name,
        OBJ_CASE_INSENSITIVE,
        root,
        nullptr);
    IO_STATUS_BLOCK status_block = {};
    HANDLE opened = INVALID_HANDLE_VALUE;
    const NTSTATUS status =
        NtCreateFile(
            &opened,
            access,
            &object_attributes,
            &status_block,
            nullptr,
            attributes,
            share_access,
            disposition,
            options,
            nullptr,
            0);
    if (status_out != nullptr) {
        *status_out = status;
    }
    return status < 0
        ? INVALID_HANDLE_VALUE
        : opened;
}

[[nodiscard]] inline HRESULT
MarkWin32HandleForDeletion(HANDLE handle)
{
    FILE_DISPOSITION_INFO disposition = {};
    disposition.DeleteFile = TRUE;
    return SetFileInformationByHandle(
               handle,
               FileDispositionInfo,
               &disposition,
               sizeof(disposition)) != FALSE
        ? S_OK
        : HRESULT_FROM_WIN32(GetLastError());
}

}  // namespace specforge
