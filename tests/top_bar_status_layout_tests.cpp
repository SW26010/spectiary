#include "ui/top_bar_status_layout.h"

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

constexpr specforge::TopBarStatusWidths kWidths{
    .operation = 10.0f,
    .frame = 10.0f,
    .dimensions = 10.0f,
    .profile = 10.0f,
    .separator = 2.0f,
};

void TestWideBarShowsEveryStatusComponent()
{
    const specforge::TopBarStatusLayout layout =
        specforge::ResolveTopBarStatusLayout(46.0f, kWidths, false, false);

    Require(layout.show_operation, "wide bar should show the operation state");
    Require(layout.show_frame, "wide bar should show the frame counter");
    Require(layout.show_dimensions, "wide bar should show the client dimensions");
    Require(layout.show_profile, "wide bar should show the inactive profile state");
    Require(layout.width == 46.0f, "wide layout should account for every separator");
}

void TestRoutineStatusDropsLowPriorityDetailsFirst()
{
    const specforge::TopBarStatusLayout layout =
        specforge::ResolveTopBarStatusLayout(22.0f, kWidths, false, false);

    Require(layout.show_operation, "routine operation state should remain visible");
    Require(layout.show_dimensions, "dimensions should survive before lower-priority diagnostics");
    Require(!layout.show_frame, "frame counter should yield before dimensions");
    Require(!layout.show_profile, "inactive profile state should yield first");
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
        TestWideBarShowsEveryStatusComponent();
        TestRoutineStatusDropsLowPriorityDetailsFirst();
        TestActiveRecordingOutranksRoutineReadyState();
        TestLoadFailureOutranksActiveRecording();
        TestStatusDisappearsInsteadOfOverlappingMenus();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
