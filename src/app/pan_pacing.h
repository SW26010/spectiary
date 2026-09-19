#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace spectiary {

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
[[nodiscard]] bool ShouldBoostForImGuiDrag(
    bool want_capture_mouse,
    bool left_mouse_dragging,
    bool uncapped_pan_active) noexcept;

}  // namespace spectiary
