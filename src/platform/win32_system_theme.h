#pragma once

#include "ui/theme.h"

#include <optional>

namespace specforge {

[[nodiscard]] std::optional<ThemeId> ReadWindowsSystemTheme();

}  // namespace specforge
