#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace spectiary {

[[nodiscard]] std::optional<std::string> GenerateUuidV4();
[[nodiscard]] bool IsCanonicalUuidV4(std::string_view value) noexcept;

}  // namespace spectiary
