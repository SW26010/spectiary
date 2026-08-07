#include "ui/top_bar_status_layout.h"

#include <array>
#include <iostream>
#include <limits>
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

constexpr specforge::TopBarStatusWidths kWidths{
    .operation = 10.0f,
    .frame = 10.0f,
    .dimensions = 10.0f,
    .profile = 10.0f,
    .separator = 2.0f,
};

constexpr std::string_view kEnglishFrameRateFormat =
    "%.3f ms/frame · %.1f FPS";
constexpr std::string_view kChineseFrameRateFormat =
    "%.3f 毫秒/帧 · %.1f FPS";

void TestFrameTimingSampleGatingAndFormatting()
{
    constexpr float kNormalDeltaTime = 1.0f / 60.0f;
    const auto first_frame = specforge::TryMakeTopBarFrameTimingSample(
        kNormalDeltaTime,
        true);
    Require(!first_frame, "the first frame should not display a timing sample");

    const auto valid_sample = specforge::TryMakeTopBarFrameTimingSample(
        kNormalDeltaTime,
        false);
    Require(
        valid_sample.has_value(),
        "a normal post-first-frame sample should be accepted");

    const auto english = specforge::FormatTopBarFrameRate(
        *valid_sample,
        kEnglishFrameRateFormat);
    Require(
        english && *english == "16.667 ms/frame · 60.0 FPS",
        "valid application frame rate should use the compact English format");

    const auto chinese = specforge::FormatTopBarFrameRate(
        *valid_sample,
        kChineseFrameRateFormat);
    Require(
        chinese && *chinese == "16.667 毫秒/帧 · 60.0 FPS",
        "valid application frame rate should use the compact Chinese format");
}

void TestInvalidAndGapTimingSamplesAreHidden()
{
    constexpr float kNormalDeltaTime = 1.0f / 60.0f;
    const std::array<float, 4> invalid_delta_times{
        0.0f,
        -1.0f,
        std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(),
    };
    for (const float delta_time : invalid_delta_times) {
        Require(
            !specforge::TryMakeTopBarFrameTimingSample(
                delta_time,
                false),
            "invalid DeltaTime must not produce a timing sample");
    }

    const auto gap = specforge::TryMakeTopBarFrameTimingSample(1.0f, false);
    Require(!gap, "an idle or minimized gap must not become a timing sample");

    Require(
        !specforge::FormatTopBarFrameRate(
            specforge::TopBarFrameTimingSample{.delta_time_seconds = 1.0f},
            kEnglishFrameRateFormat),
        "a timing gap must not produce status text");

    const auto valid_sample = specforge::TryMakeTopBarFrameTimingSample(
        kNormalDeltaTime,
        false);
    std::optional<specforge::TopBarFrameTimingSample> last_valid_sample =
        valid_sample;
    if (gap) {
        last_valid_sample = gap;
    }
    const auto retained = specforge::FormatTopBarFrameRate(
        *last_valid_sample,
        kEnglishFrameRateFormat);
    Require(
        retained && *retained == "16.667 ms/frame · 60.0 FPS",
        "a timing gap should retain the last valid sample");
}

void TestWideBarOmitsInactiveProfileStatus()
{
    const specforge::TopBarStatusLayout layout =
        specforge::ResolveTopBarStatusLayout(46.0f, kWidths, false, false);

    Require(layout.show_operation, "wide bar should show the operation state");
    Require(layout.show_frame, "wide bar should show the frame-rate indicator");
    Require(layout.show_dimensions, "wide bar should show the client dimensions");
    Require(!layout.show_profile, "wide bar should omit the inactive profile state");
    Require(layout.width == 34.0f, "inactive profile state should consume no status-bar width");
}

void TestRoutineStatusDropsLowPriorityDetailsFirst()
{
    const specforge::TopBarStatusLayout layout =
        specforge::ResolveTopBarStatusLayout(22.0f, kWidths, false, false);

    Require(layout.show_operation, "routine operation state should remain visible");
    Require(layout.show_dimensions, "dimensions should survive before lower-priority diagnostics");
    Require(!layout.show_frame, "frame-rate indicator should yield before dimensions");
    Require(!layout.show_profile, "inactive profile state should remain omitted");
}

void TestUnavailableFrameRateDoesNotConsumeWidth()
{
    specforge::TopBarStatusWidths widths = kWidths;
    widths.frame = 0.0f;
    const specforge::TopBarStatusLayout layout =
        specforge::ResolveTopBarStatusLayout(46.0f, widths, false, false);

    Require(layout.show_operation, "operation state should remain visible");
    Require(layout.show_dimensions, "dimensions should remain visible");
    Require(!layout.show_frame, "unavailable frame rate should be hidden");
    Require(layout.width == 22.0f, "hidden frame rate should consume no width");
}

void TestActiveRecordingOutranksRoutineReadyState()
{
    const specforge::TopBarStatusLayout layout =
        specforge::ResolveTopBarStatusLayout(10.0f, kWidths, false, true);

    Require(layout.show_profile, "active recording should remain visible");
    Require(!layout.show_operation, "routine Ready state should yield to active recording");
    Require(!layout.show_frame, "diagnostics should not displace active recording");
    Require(!layout.show_dimensions, "dimensions should not displace active recording");
}

void TestLoadFailureOutranksActiveRecording()
{
    const specforge::TopBarStatusLayout layout =
        specforge::ResolveTopBarStatusLayout(10.0f, kWidths, true, true);

    Require(layout.show_operation, "load failure should remain visible");
    Require(!layout.show_profile, "recording should yield when only the failure fits");
}

void TestStatusDisappearsInsteadOfOverlappingMenus()
{
    const specforge::TopBarStatusLayout layout =
        specforge::ResolveTopBarStatusLayout(9.0f, kWidths, true, true);

    Require(!layout.show_operation, "oversized operation state should not overlap menus");
    Require(!layout.show_profile, "oversized profile state should not overlap menus");
    Require(layout.width == 0.0f, "hidden status should consume no width");
}

}  // namespace

int main()
{
    try {
        TestFrameTimingSampleGatingAndFormatting();
        TestInvalidAndGapTimingSamplesAreHidden();
        TestWideBarOmitsInactiveProfileStatus();
        TestRoutineStatusDropsLowPriorityDetailsFirst();
        TestUnavailableFrameRateDoesNotConsumeWidth();
        TestActiveRecordingOutranksRoutineReadyState();
        TestLoadFailureOutranksActiveRecording();
        TestStatusDisappearsInsteadOfOverlappingMenus();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
