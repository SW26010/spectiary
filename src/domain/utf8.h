#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace specforge {

struct Utf8Scalar {
    std::uint32_t codepoint = 0;
    std::size_t width = 0;
};

[[nodiscard]] bool IsValidUtf8(std::string_view text) noexcept;

// Returns true only when the complete input is valid UTF-8 and contains at
// least one scalar outside the Unicode White_Space property. No Unicode
// normalization is performed.
[[nodiscard]] bool IsValidUtf8WithNonWhitespace(
    std::string_view text) noexcept;

// The complete string and offset must already have been validated by
// IsValidUtf8. This allocation-free decoder is shared with codecs that walk a
// validated string scalar by scalar.
[[nodiscard]] Utf8Scalar DecodeValidUtf8Scalar(
    std::string_view text,
    std::size_t offset) noexcept;

}  // namespace specforge
