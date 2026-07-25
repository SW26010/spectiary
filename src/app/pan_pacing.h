#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace specforge {

enum class PanPacingMode {
    Display,
    Uncapped,
};

struct PanPacingConfiguration {
    std::string requested = "display";
    PanPacingMode effective = PanPacingMode::Display;
    bool recognized = true;
};

[[nodiscard]] PanPacingConfiguration ResolvePanPacing(
    std::optional<std::string_view> requested);
[[nodiscard]] PanPacingConfiguration ResolvePanPacingEnvironment();
[[nodiscard]] const char* PanPacingModeName(PanPacingMode mode) noexcept;

}  // namespace specforge
