#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace spectiary {

[[nodiscard]] std::optional<std::string> ComputeFileSha256(
    const std::filesystem::path& path,
    std::string* error_message = nullptr);

}  // namespace spectiary
