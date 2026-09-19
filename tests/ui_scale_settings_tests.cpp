#include "ui/ui_scale_settings.h"

#include <imgui.h>

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void TestSystemAndUserScaleComposition()
{
    const spectiary::UiScaleFactors default_scale =
        spectiary::CalculateUiScaleFactors(1.5f, 100);
    Require(
        std::abs(default_scale.system - 1.5f) < 0.0001f &&
            std::abs(default_scale.user - 1.0f) < 0.0001f &&
            std::abs(default_scale.effective - 1.5f) <
                0.0001f,
        "100% should preserve the Windows DPI scale");

    const spectiary::UiScaleFactors scaled =
        spectiary::CalculateUiScaleFactors(1.5f, 125);
    Require(
        std::abs(scaled.system - 1.5f) < 0.0001f &&
            std::abs(scaled.user - 1.25f) < 0.0001f &&
            std::abs(scaled.effective - 1.875f) <
                0.0001f,
        "system and user scales should multiply");

    const spectiary::UiScaleFactors moved =
        spectiary::CalculateUiScaleFactors(1.0f, 125);
    Require(
        std::abs(moved.user - 1.25f) < 0.0001f &&
            std::abs(moved.effective - 1.25f) < 0.0001f,
        "monitor DPI changes should preserve the user scale");
}

void TestStyleScalingPreservesVisibleHairlines()
{
    const ImGuiStyle base_style;
    ImGuiStyle scaled_style;
    spectiary::ApplyUiScaleToImGuiStyle(
        scaled_style,
        base_style,
        spectiary::CalculateUiScaleFactors(1.0f, 80));

    Require(
        base_style.WindowBorderSize > 0.0f &&
            scaled_style.WindowBorderSize >= 1.0f,
        "80% UI scale should retain a visible window border");
    Require(
        base_style.ChildBorderSize > 0.0f &&
            scaled_style.ChildBorderSize >= 1.0f,
        "80% UI scale should retain a visible child border");
    Require(
        base_style.PopupBorderSize > 0.0f &&
            scaled_style.PopupBorderSize >= 1.0f,
        "80% UI scale should retain a visible popup border");
    Require(
        base_style.SeparatorSize > 0.0f &&
            scaled_style.SeparatorSize >= 1.0f,
        "80% UI scale should retain a visible separator");
    Require(
        base_style.FrameBorderSize == 0.0f &&
            scaled_style.FrameBorderSize == 0.0f,
        "UI scaling should not enable intentionally disabled borders");
    Require(
        scaled_style.WindowPadding.x <
            base_style.WindowPadding.x,
        "80% UI scale should still shrink ordinary layout dimensions");
}

}  // namespace

int main()
{
    try {
        TestSystemAndUserScaleComposition();
        TestStyleScalingPreservesVisibleHairlines();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
