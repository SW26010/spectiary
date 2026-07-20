#include "domain/source_path_identity.h"

#include <limits>
#include <system_error>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace specforge {
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

}  // namespace specforge
