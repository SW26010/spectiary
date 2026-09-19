#include "domain/uuid_v4.h"

#include "platform/secure_random_bytes.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace spectiary {

std::optional<std::string> GenerateUuidV4()
{
    std::array<std::uint8_t, 16> bytes = {};
    if (!FillSecureRandomBytes(bytes)) {
        return std::nullopt;
    }

    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0fU) | 0x40U);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3fU) | 0x80U);

    constexpr char digits[] = "0123456789abcdef";
    std::string value;
    value.reserve(36);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index == 4U || index == 6U || index == 8U || index == 10U) {
            value.push_back('-');
        }
        value.push_back(digits[bytes[index] >> 4U]);
        value.push_back(digits[bytes[index] & 0x0fU]);
    }
    return value;
}

bool IsCanonicalUuidV4(std::string_view value) noexcept
{
    if (value.size() != 36U) {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        const bool hyphen =
            index == 8U || index == 13U || index == 18U || index == 23U;
        if (hyphen) {
            if (value[index] != '-') {
                return false;
            }
            continue;
        }
        const char character = value[index];
        if (!((character >= '0' && character <= '9') ||
                (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }
    if (value[14] != '4') {
        return false;
    }
    return value[19] == '8' || value[19] == '9' || value[19] == 'a' ||
        value[19] == 'b';
}

}  // namespace spectiary
