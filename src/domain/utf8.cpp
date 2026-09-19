#include "domain/utf8.h"

namespace spectiary {
namespace {

[[nodiscard]] bool IsUnicodeWhitespace(std::uint32_t codepoint) noexcept
{
    if (codepoint >= 0x0009U && codepoint <= 0x000dU) {
        return true;
    }
    if (codepoint >= 0x2000U && codepoint <= 0x200aU) {
        return true;
    }
    switch (codepoint) {
    case 0x0020U:
    case 0x0085U:
    case 0x00a0U:
    case 0x1680U:
    case 0x2028U:
    case 0x2029U:
    case 0x202fU:
    case 0x205fU:
    case 0x3000U:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool DecodeUtf8Scalar(
    std::string_view text,
    std::size_t offset,
    Utf8Scalar& scalar) noexcept
{
    if (offset >= text.size()) {
        return false;
    }

    const unsigned char lead = static_cast<unsigned char>(text[offset]);
    if (lead <= 0x7fU) {
        scalar = Utf8Scalar{.codepoint = lead, .width = 1};
        return true;
    }

    std::size_t continuation = 0;
    std::uint32_t codepoint = 0;
    if ((lead & 0xe0U) == 0xc0U) {
        continuation = 1;
        codepoint = lead & 0x1fU;
    } else if ((lead & 0xf0U) == 0xe0U) {
        continuation = 2;
        codepoint = lead & 0x0fU;
    } else if ((lead & 0xf8U) == 0xf0U) {
        continuation = 3;
        codepoint = lead & 0x07U;
    } else {
        return false;
    }

    if (continuation > text.size() - offset - 1U) {
        return false;
    }
    for (std::size_t part = 0; part < continuation; ++part) {
        const unsigned char byte =
            static_cast<unsigned char>(text[offset + part + 1U]);
        if ((byte & 0xc0U) != 0x80U) {
            return false;
        }
        codepoint = (codepoint << 6U) | (byte & 0x3fU);
    }

    const std::uint32_t minimum = continuation == 1   ? 0x80U
                                  : continuation == 2 ? 0x800U
                                                      : 0x10000U;
    if (codepoint < minimum || codepoint > 0x10ffffU ||
        (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
        return false;
    }

    scalar = Utf8Scalar{
        .codepoint = codepoint, .width = continuation + 1U};
    return true;
}

}  // namespace

bool IsValidUtf8(std::string_view text) noexcept
{
    for (std::size_t offset = 0; offset < text.size();) {
        Utf8Scalar scalar;
        if (!DecodeUtf8Scalar(text, offset, scalar)) {
            return false;
        }
        offset += scalar.width;
    }
    return true;
}

bool IsValidUtf8WithNonWhitespace(std::string_view text) noexcept
{
    bool has_non_whitespace = false;
    for (std::size_t offset = 0; offset < text.size();) {
        Utf8Scalar scalar;
        if (!DecodeUtf8Scalar(text, offset, scalar)) {
            return false;
        }
        has_non_whitespace =
            has_non_whitespace || !IsUnicodeWhitespace(scalar.codepoint);
        offset += scalar.width;
    }
    return has_non_whitespace;
}

Utf8Scalar DecodeValidUtf8Scalar(
    std::string_view text,
    std::size_t offset) noexcept
{
    Utf8Scalar scalar;
    static_cast<void>(DecodeUtf8Scalar(text, offset, scalar));
    return scalar;
}

}  // namespace spectiary
