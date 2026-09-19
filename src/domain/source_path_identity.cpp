#include "domain/source_path_identity.h"
#include "platform/win32_file_identity.h"

#include <algorithm>
#include <limits>
#include <system_error>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace spectiary {
namespace {

std::wstring FoldWindowsPathCase(std::wstring value)
{
    if (value.empty() || value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return value;
    }
    const int source_size = static_cast<int>(value.size());
    const int required = LCMapStringEx(
        LOCALE_NAME_INVARIANT,
        LCMAP_UPPERCASE,
        value.data(),
        source_size,
        nullptr,
        0,
        nullptr,
        nullptr,
        0);
    if (required <= 0) {
        return value;
    }
    std::wstring folded(static_cast<std::size_t>(required), L'\0');
    const int written = LCMapStringEx(
        LOCALE_NAME_INVARIANT,
        LCMAP_UPPERCASE,
        value.data(),
        source_size,
        folded.data(),
        required,
        nullptr,
        nullptr,
        0);
    if (written <= 0) {
        return value;
    }
    folded.resize(static_cast<std::size_t>(written));
    return folded;
}

std::string WideToUtf8(const std::wstring& value)
{
    if (value.empty() || value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return {};
    }
    const int source_size = static_cast<int>(value.size());
    const int required = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        source_size,
        nullptr,
        0,
        nullptr,
        nullptr);
    if (required <= 0) {
        return {};
    }
    std::string utf8(static_cast<std::size_t>(required), '\0');
    const int written = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        source_size,
        utf8.data(),
        required,
        nullptr,
        nullptr);
    if (written <= 0) {
        return {};
    }
    utf8.resize(static_cast<std::size_t>(written));
    return utf8;
}

HANDLE OpenIdentityHandle(
    const std::filesystem::path& path,
    DWORD* error_out)
{
    if (error_out != nullptr) {
        *error_out = ERROR_SUCCESS;
    }
    const std::filesystem::path extended =
        Win32ExtendedLengthPath(path);
    if (extended.empty()) {
        if (error_out != nullptr) {
            *error_out = ERROR_INVALID_NAME;
        }
        return INVALID_HANDLE_VALUE;
    }
    const HANDLE handle = CreateFileW(
        extended.c_str(),
        0,
        FILE_SHARE_READ |
            FILE_SHARE_WRITE |
            FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        if (error_out != nullptr) {
            *error_out = GetLastError();
        }
        return INVALID_HANDLE_VALUE;
    }
    return handle;
}

std::string FileObjectIdentityKey(HANDLE handle)
{
    FILE_ID_INFO information = {};
    if (GetFileInformationByHandleEx(
            handle,
            FileIdInfo,
            &information,
            sizeof(information)) == FALSE) {
        return {};
    }
    constexpr char kHex[] = "0123456789abcdef";
    std::string key = "file-object:";
    key.reserve(
        key.size() + 16 + 1 +
        sizeof(information.FileId.Identifier) * 2);
    for (int shift = 60; shift >= 0; shift -= 4) {
        key.push_back(
            kHex[(information.VolumeSerialNumber >> shift) & 0x0f]);
    }
    key.push_back(':');
    for (const unsigned char byte :
         information.FileId.Identifier) {
        key.push_back(kHex[(byte >> 4) & 0x0f]);
        key.push_back(kHex[byte & 0x0f]);
    }
    return key;
}

std::string DirectoryEntryIdentityKey(
    const std::filesystem::path& full_path)
{
    const std::filesystem::path leaf =
        full_path.filename();
    if (leaf.empty()) {
        return {};
    }
    const std::string leaf_key =
        WideToUtf8(
            FoldWindowsPathCase(
                leaf.generic_wstring()));
    if (leaf_key.empty()) {
        return {};
    }
    DWORD parent_error = ERROR_SUCCESS;
    const HANDLE parent = OpenIdentityHandle(
        full_path.parent_path(),
        &parent_error);
    if (parent == INVALID_HANDLE_VALUE) {
        return {};
    }
    std::string parent_key =
        FileObjectIdentityKey(parent);
    CloseHandle(parent);
    if (parent_key.empty()) {
        return {};
    }
    return "directory-entry:" +
        parent_key + ":" + leaf_key;
}

}  // namespace

std::string SourcePathIdentityKey(const std::filesystem::path& path)
{
    if (path.empty()) {
        return {};
    }

    std::error_code error;
    std::filesystem::path absolute_path = path.is_absolute() ? path : std::filesystem::absolute(path, error);
    if (error) {
        absolute_path = path;
    }
    const std::wstring normalized = absolute_path.lexically_normal().generic_wstring();
    return WideToUtf8(FoldWindowsPathCase(normalized));
}

std::vector<std::string> OutputPathIdentityKeys(
    const std::filesystem::path& path)
{
    if (path.empty()) {
        return {};
    }
    const std::filesystem::path full_path =
        Win32FullPath(path);
    if (full_path.empty()) {
        return {};
    }

    DWORD target_error = ERROR_SUCCESS;
    const HANDLE target = OpenIdentityHandle(
        full_path,
        &target_error);
    if (target != INVALID_HANDLE_VALUE) {
        std::string object_key =
            FileObjectIdentityKey(target);
        CloseHandle(target);
        if (object_key.empty()) {
            return {};
        }
        const std::string directory_entry_key =
            DirectoryEntryIdentityKey(full_path);
        if (directory_entry_key.empty()) {
            return full_path.filename().empty()
                ? std::vector<std::string>{
                      std::move(object_key)}
                : std::vector<std::string>{};
        }
        std::vector<std::string> keys{
            std::move(object_key),
            directory_entry_key};
        std::sort(keys.begin(), keys.end());
        keys.erase(
            std::unique(keys.begin(), keys.end()),
            keys.end());
        return keys;
    }
    if (target_error != ERROR_FILE_NOT_FOUND &&
        target_error != ERROR_PATH_NOT_FOUND) {
        return {};
    }
    const std::string directory_entry_key =
        DirectoryEntryIdentityKey(full_path);
    if (directory_entry_key.empty()) {
        return {};
    }
    return {directory_entry_key};
}

std::string OutputPathIdentityKey(
    const std::filesystem::path& path)
{
    std::vector<std::string> keys =
        OutputPathIdentityKeys(path);
    return keys.empty()
        ? SourcePathIdentityKey(path)
        : std::move(keys.front());
}

}  // namespace spectiary
