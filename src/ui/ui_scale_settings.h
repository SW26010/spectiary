#pragma once

struct ImGuiStyle;

namespace specforge {

inline constexpr int kDefaultUiScalePercentage = 100;
inline constexpr int kMinimumUiScalePercentage = 80;
inline constexpr int kMaximumUiScalePercentage = 150;

struct UiScaleFactors {
    float system = 1.0f;
    float user = 1.0f;
    float effective = 1.0f;
};

[[nodiscard]] bool IsValidUiScalePercentage(int percentage) noexcept;
[[nodiscard]] UiScaleFactors CalculateUiScaleFactors(
    float system_dpi_scale,
    int user_scale_percentage) noexcept;
void ApplyUiScaleToImGuiStyle(
    ImGuiStyle& style,
    const ImGuiStyle& base_style,
    const UiScaleFactors& scales) noexcept;

}  // namespace specforge
