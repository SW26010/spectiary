#pragma once

#include "ui/theme.h"

#include <optional>

namespace spectiary {

[[nodiscard]] std::optional<ThemeId> ReadWindowsSystemTheme();

}  // namespace spectiary
