#include "platform/secure_random_bytes.h"

#include <Windows.h>
#include <bcrypt.h>

#include <limits>

namespace spectiary {

bool FillSecureRandomBytes(std::span<std::uint8_t> destination) noexcept
{
    if (destination.empty()) {
        return true;
    }
    if (destination.size() > std::numeric_limits<ULONG>::max()) {
        return false;
    }
    return BCryptGenRandom(
               nullptr,
               destination.data(),
               static_cast<ULONG>(destination.size()),
               BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

}  // namespace spectiary
