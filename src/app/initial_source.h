#pragma once

#include <filesystem>
#include <optional>

namespace specforge {

class ShellUi;

void OpenInitialSource(
    ShellUi& shell,
    const std::optional<std::filesystem::path>& initial_source);

}  // namespace specforge
