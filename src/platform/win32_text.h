#pragma once

#include <Windows.h>

#include <limits>
#include <string>
#include <string_view>

namespace specforge {

[[nodiscard]] inline std::wstring Utf8ToWide(
    std::string_view value)
{
    if (value.empty() ||
        value.size() >
            static_cast<std::size_t>(
                (std::numeric_limits<int>::max)())) {
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

[[nodiscard]] inline std::string WideToUtf8(
    std::wstring_view value)
{
    if (value.empty() ||
        value.size() >
            static_cast<std::size_t>(
                (std::numeric_limits<int>::max)())) {
        return {};
    }

    const int required = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (required <= 0) {
        return {};
    }

    std::string converted(
        static_cast<std::size_t>(required),
        '\0');
    const int written = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        converted.data(),
        required,
        nullptr,
        nullptr);
    if (written != required) {
        return {};
    }
    return converted;
}

[[nodiscard]] inline bool IsWellFormedUtf8(
    std::string_view value)
{
    return value.empty() ||
           !Utf8ToWide(value).empty();
}

}  // namespace specforge
