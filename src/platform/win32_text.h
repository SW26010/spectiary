#pragma once

#include <Windows.h>

#include <string>
#include <string_view>

namespace specforge {

[[nodiscard]] inline std::wstring Utf8ToWide(
    std::string_view value)
{
    if (value.empty()) {
        return {};
    }

    const int required = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0);
    if (required <= 0) {
        return {};
    }

    std::wstring converted(
        static_cast<std::size_t>(required),
        L'\0');
    const int written = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        converted.data(),
        required);
    if (written != required) {
        return {};
    }
    return converted;
}

}  // namespace specforge
