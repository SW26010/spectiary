#pragma once

#include <cstdint>
#include <span>

namespace spectiary {

[[nodiscard]] bool FillSecureRandomBytes(
    std::span<std::uint8_t> destination) noexcept;

}  // namespace spectiary
