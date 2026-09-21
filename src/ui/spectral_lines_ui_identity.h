#pragma once

#include <string>
#include <string_view>

namespace spectiary {
// Canonical IDs are opaque UTF-8 bytes, not ImGui label syntax. Encode every
// byte (including embedded NUL) so ### cannot reset hashing or truncate an ID.
// Only widget identities use this spelling; domain intents retain the original.
[[nodiscard]] inline std::string SpectralLineUiId(std::string_view id)
{
    constexpr char hex[] = "0123456789abcdef";
    std::string encoded = "line-id-";
    encoded.reserve(encoded.size() + id.size() * 2);
    for (const unsigned char byte : id) {
        encoded.push_back(hex[byte >> 4]);
        encoded.push_back(hex[byte & 15]);
    }
    return encoded;
}
} // namespace spectiary
