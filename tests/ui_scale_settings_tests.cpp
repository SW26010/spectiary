#include "ui/ui_scale_settings.h"

#include <imgui.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
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

class TemporaryDirectory {
public:
    TemporaryDirectory()
    {
        const auto suffix = std::chrono::steady_clock::now()
                                .time_since_epoch()
                                .count();
        path_ = std::filesystem::temp_directory_path() /
                ("specforge-ui-scale-settings-tests-" +
                 std::to_string(suffix));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream stream(path);
    return std::string(
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>());
}

void WriteFile(
    const std::filesystem::path& path,
    std::string_view contents)
{
    std::ofstream stream(path);
    stream << contents;
    Require(stream.good(), "UI scale fixture should write");
}

void TestDefaultPathAndMissingFile()
{
    Require(
        specforge::DefaultUiScaleSettingsPath().filename() ==
            "ui-scale.json",
        "UI scale should use a dedicated settings file");

    TemporaryDirectory temporary;
    const specforge::UiScaleSettingsLoadResult loaded =
        specforge::LoadUiScaleSettings(
            temporary.path() / "missing.json");
    Require(
        loaded.percentage == 100 && loaded.warning.empty(),
        "missing UI scale settings should silently default to 100%");
}

void TestSupportedPercentagesRoundTrip()
{
    TemporaryDirectory temporary;
    const std::filesystem::path path =
        temporary.path() / "ui-scale.json";

    for (const int percentage : {80, 100, 150}) {
        std::string error;
        Require(
            specforge::SaveUiScaleSettings(
                path,
                percentage,
                &error),
            "supported UI scale should save");
        Require(
            error.empty(),
            "successful UI scale save should not report an error");
        Require(
            ReadFile(path).find(
                "\"percentage\": " +
                std::to_string(percentage)) !=
                std::string::npos,
            "UI scale should persist as an integer percentage");
        const specforge::UiScaleSettingsLoadResult loaded =
            specforge::LoadUiScaleSettings(path);
        Require(
            loaded.percentage == percentage &&
                loaded.warning.empty(),
            "supported UI scale should reload without warning");
    }
}

void TestInvalidDocumentsFallBackWithWarning()
{
    TemporaryDirectory temporary;
    const std::filesystem::path path =
        temporary.path() / "ui-scale.json";

    WriteFile(path, R"({"format_kind":)");
    specforge::UiScaleSettingsLoadResult loaded =
        specforge::LoadUiScaleSettings(path);
    Require(
        loaded.percentage == 100 && !loaded.warning.empty(),
        "damaged UI scale JSON should warn and fall back");

    WriteFile(
        path,
        R"({"format_kind":"specforge.ui_scale.settings","schema_version":2,"percentage":125})");
    loaded = specforge::LoadUiScaleSettings(path);
    Require(
        loaded.percentage == 100 && !loaded.warning.empty(),
        "unsupported UI scale schema should warn and fall back");

    WriteFile(
        path,
        R"({"format_kind":"specforge.ui_scale.settings","schema_version":1,"percentage":151})");
    loaded = specforge::LoadUiScaleSettings(path);
    Require(
        loaded.percentage == 100 && !loaded.warning.empty(),
        "out-of-range saved UI scale should warn and fall back");
}

void TestInvalidPercentageCannotBeSaved()
{
    TemporaryDirectory temporary;
    std::string error;
    Require(
        !specforge::SaveUiScaleSettings(
            temporary.path() / "ui-scale.json",
            79,
            &error) &&
            !error.empty(),
        "out-of-range UI scale should not be saved");
}

void TestSystemAndUserScaleComposition()
{
    const specforge::UiScaleFactors default_scale =
        specforge::CalculateUiScaleFactors(1.5f, 100);
    Require(
        std::abs(default_scale.system - 1.5f) < 0.0001f &&
            std::abs(default_scale.user - 1.0f) < 0.0001f &&
            std::abs(default_scale.effective - 1.5f) <
                0.0001f,
        "100% should preserve the Windows DPI scale");

    const specforge::UiScaleFactors scaled =
        specforge::CalculateUiScaleFactors(1.5f, 125);
    Require(
        std::abs(scaled.system - 1.5f) < 0.0001f &&
            std::abs(scaled.user - 1.25f) < 0.0001f &&
            std::abs(scaled.effective - 1.875f) <
                0.0001f,
        "system and user scales should multiply");

    const specforge::UiScaleFactors moved =
        specforge::CalculateUiScaleFactors(1.0f, 125);
    Require(
        std::abs(moved.user - 1.25f) < 0.0001f &&
            std::abs(moved.effective - 1.25f) < 0.0001f,
        "monitor DPI changes should preserve the user scale");
}

void TestStyleScalingPreservesVisibleHairlines()
{
    const ImGuiStyle base_style;
    ImGuiStyle scaled_style;
    specforge::ApplyUiScaleToImGuiStyle(
        scaled_style,
        base_style,
        specforge::CalculateUiScaleFactors(1.0f, 80));

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
        TestDefaultPathAndMissingFile();
        TestSupportedPercentagesRoundTrip();
        TestInvalidDocumentsFallBackWithWarning();
        TestInvalidPercentageCannotBeSaved();
        TestSystemAndUserScaleComposition();
        TestStyleScalingPreservesVisibleHairlines();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
