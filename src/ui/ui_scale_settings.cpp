#include "ui/ui_scale_settings.h"

#include "app/local_user_state.h"
#include "app/local_user_state_json.h"
#include "app/local_user_state_paths.h"

#include <imgui.h>

#include <cmath>
#include <optional>
#include <string>
#include <utility>

namespace specforge {
namespace {

constexpr const char* kSettingsFormatKind =
    "specforge.ui_scale.settings";
constexpr int kSettingsSchemaVersion = 1;

}  // namespace

bool IsValidUiScalePercentage(int percentage) noexcept
{
    return percentage >= kMinimumUiScalePercentage &&
        percentage <= kMaximumUiScalePercentage;
}

UiScaleFactors CalculateUiScaleFactors(
    float system_dpi_scale,
    int user_scale_percentage) noexcept
{
    const float system =
        std::isfinite(system_dpi_scale) &&
            system_dpi_scale > 0.0f
        ? system_dpi_scale
        : 1.0f;
    const int percentage =
        IsValidUiScalePercentage(user_scale_percentage)
        ? user_scale_percentage
        : kDefaultUiScalePercentage;
    const float user =
        static_cast<float>(percentage) / 100.0f;
    return {
        .system = system,
        .user = user,
        .effective = system * user,
    };
}

void ApplyUiScaleToImGuiStyle(
    ImGuiStyle& style,
    const ImGuiStyle& base_style,
    const UiScaleFactors& scales) noexcept
{
    style = base_style;
    style.ScaleAllSizes(scales.effective);

    const auto preserve_hairline =
        [](float base_size, float& scaled_size) {
            if (base_size > 0.0f &&
                scaled_size < 1.0f) {
                scaled_size = 1.0f;
            }
        };
    preserve_hairline(
        base_style.WindowBorderSize,
        style.WindowBorderSize);
    preserve_hairline(
        base_style.ChildBorderSize,
        style.ChildBorderSize);
    preserve_hairline(
        base_style.PopupBorderSize,
        style.PopupBorderSize);
    preserve_hairline(
        base_style.FrameBorderSize,
        style.FrameBorderSize);
    preserve_hairline(
        base_style.ImageBorderSize,
        style.ImageBorderSize);
    preserve_hairline(
        base_style.TabBorderSize,
        style.TabBorderSize);
    preserve_hairline(
        base_style.TabBarBorderSize,
        style.TabBarBorderSize);
    preserve_hairline(
        base_style.TabBarOverlineSize,
        style.TabBarOverlineSize);
    preserve_hairline(
        base_style.TreeLinesSize,
        style.TreeLinesSize);
    preserve_hairline(
        base_style.DragDropTargetBorderSize,
        style.DragDropTargetBorderSize);
    preserve_hairline(
        base_style.SeparatorSize,
        style.SeparatorSize);
    preserve_hairline(
        base_style.SeparatorTextBorderSize,
        style.SeparatorTextBorderSize);
    preserve_hairline(
        base_style.DockingSeparatorSize,
        style.DockingSeparatorSize);

    style.FontScaleMain = scales.user;
    style.FontScaleDpi = scales.system;
}

std::filesystem::path DefaultUiScaleSettingsPath()
{
    return DefaultLocalUserStatePath(
        local_user_state_paths::kUiScaleSettings);
}

UiScaleSettingsLoadResult LoadUiScaleSettings(
    const std::filesystem::path& path)
{
    UiScaleSettingsLoadResult settings;
    VersionedJsonCacheLoadResult cache =
        LoadVersionedJsonCacheFile(
            path,
            kSettingsFormatKind,
            {kSettingsSchemaVersion},
            "UI scale settings");
    if (!cache.document) {
        settings.warning = std::move(cache.warning);
        return settings;
    }

    const std::optional<int> percentage =
        ReadJsonIntMember(
            cache.document->root,
            "percentage");
    if (!percentage ||
        !IsValidUiScalePercentage(*percentage)) {
        settings.warning =
            "Ignored UI scale settings: the percentage "
            "must be an integer from 80 through 150.";
        return settings;
    }

    settings.percentage = *percentage;
    return settings;
}

bool SaveUiScaleSettings(
    const std::filesystem::path& path,
    int percentage,
    std::string* error_message)
{
    if (!IsValidUiScalePercentage(percentage)) {
        if (error_message != nullptr) {
            *error_message =
                "The UI scale must be from 80% through 150%.";
        }
        return false;
    }

    return WriteVersionedJsonCacheDocument(
        path,
        kSettingsFormatKind,
        kSettingsSchemaVersion,
        "UI scale settings",
        JsonObjectValue({
            {"percentage", JsonIntegerValue(percentage)},
        }),
        error_message);
}

}  // namespace specforge
