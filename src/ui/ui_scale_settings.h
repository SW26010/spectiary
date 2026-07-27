#pragma once

#include <filesystem>
#include <string>

struct ImGuiStyle;

namespace specforge {

inline constexpr int kDefaultUiScalePercentage = 100;
inline constexpr int kMinimumUiScalePercentage = 80;
inline constexpr int kMaximumUiScalePercentage = 150;

struct UiScaleSettingsLoadResult {
    int percentage = kDefaultUiScalePercentage;
    std::string warning;
};

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
[[nodiscard]] std::filesystem::path DefaultUiScaleSettingsPath();
[[nodiscard]] UiScaleSettingsLoadResult LoadUiScaleSettings(
    const std::filesystem::path& path);
[[nodiscard]] bool SaveUiScaleSettings(
    const std::filesystem::path& path,
    int percentage,
    std::string* error_message = nullptr);

}  // namespace specforge
