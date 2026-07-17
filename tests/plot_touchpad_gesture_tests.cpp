#include "plot/plot_touchpad_gesture.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

namespace {

void Require(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

void RequireNear(double actual, double expected, const std::string& message)
{
    Require(std::abs(actual - expected) < 1.0e-9, message);
}

specforge::PlotTouchpadGestureDelta Pan(float x, float y, specforge::PlotGestureAxes axes)
{
    specforge::PlotTouchpadGestureDelta gesture;
    gesture.kind = specforge::PlotTouchpadGestureKind::Pan;
    gesture.axes = axes;
    gesture.plot_rect = {100.0f, 200.0f, 500.0f, 400.0f};
    gesture.pan_x = x;
    gesture.pan_y = y;
    return gesture;
}

specforge::PlotTouchpadGestureDelta Zoom(
    double factor,
    float anchor_x,
    float anchor_y,
    specforge::PlotGestureAxes axes)
{
    specforge::PlotTouchpadGestureDelta gesture;
    gesture.kind = specforge::PlotTouchpadGestureKind::Zoom;
    gesture.axes = axes;
    gesture.plot_rect = {100.0f, 200.0f, 500.0f, 400.0f};
    gesture.anchor_x = anchor_x;
    gesture.anchor_y = anchor_y;
    gesture.zoom_factor = factor;
    return gesture;
}

void TestTargetHitTestingUsesAxisZonesBeforePlotBody()
{
    specforge::PlotTouchpadTarget target;
    target.plot_rect = {0.0f, 0.0f, 500.0f, 400.0f};
    target.x_axis_rect = {0.0f, 356.0f, 500.0f, 400.0f};
    target.y_axis_rect = {0.0f, 0.0f, 44.0f, 400.0f};

    bool hit = false;
    Require(
        specforge::HitTestPlotTouchpadTarget(target, 200.0f, 380.0f, &hit) ==
                specforge::PlotGestureAxes::XOnly &&
            hit,
        "bottom edge should select only the x axis");
    Require(
        specforge::HitTestPlotTouchpadTarget(target, 20.0f, 200.0f, &hit) ==
                specforge::PlotGestureAxes::YOnly &&
            hit,
        "left edge should select only the y axis");
    Require(
        specforge::HitTestPlotTouchpadTarget(target, 200.0f, 200.0f, &hit) ==
                specforge::PlotGestureAxes::Both &&
            hit,
        "plot body should select both axes");
    (void)specforge::HitTestPlotTouchpadTarget(target, 700.0f, 200.0f, &hit);
    Require(!hit, "outside point should not capture a touchpad gesture");
}

void TestPanMovesBothAxesWithDirectManipulationContent()
{
    specforge::PlotViewLimits limits{0.0, 100.0, -10.0, 10.0};
    Require(
        specforge::ApplyPlotTouchpadGesture(
            limits,
            Pan(40.0f, 20.0f, specforge::PlotGestureAxes::Both)),
        "two-axis pan should change limits");

    RequireNear(limits.x_min, -10.0, "rightward content motion should reveal lower x values");
    RequireNear(limits.x_max, 90.0, "x span should be preserved while panning");
    RequireNear(limits.y_min, -8.0, "downward content motion should reveal higher y values");
    RequireNear(limits.y_max, 12.0, "y span should be preserved while panning");
}

void TestAxisConstrainedPanLeavesOtherAxisUnchanged()
{
    specforge::PlotViewLimits x_limits{0.0, 100.0, -10.0, 10.0};
    (void)specforge::ApplyPlotTouchpadGesture(
        x_limits,
        Pan(40.0f, 20.0f, specforge::PlotGestureAxes::XOnly));
    RequireNear(x_limits.x_min, -10.0, "x-only pan should update x");
    RequireNear(x_limits.y_min, -10.0, "x-only pan should preserve y");

    specforge::PlotViewLimits y_limits{0.0, 100.0, -10.0, 10.0};
    (void)specforge::ApplyPlotTouchpadGesture(
        y_limits,
        Pan(40.0f, 20.0f, specforge::PlotGestureAxes::YOnly));
    RequireNear(y_limits.x_min, 0.0, "y-only pan should preserve x");
    RequireNear(y_limits.y_min, -8.0, "y-only pan should update y");
}

void TestPinchZoomKeepsGestureAnchorFixed()
{
    specforge::PlotViewLimits limits{0.0, 100.0, 0.0, 20.0};
    Require(
        specforge::ApplyPlotTouchpadGesture(
            limits,
            Zoom(2.0, 200.0f, 250.0f, specforge::PlotGestureAxes::Both)),
        "pinch zoom should change limits");

    RequireNear(limits.x_min, 12.5, "x zoom should remain anchored at one quarter width");
    RequireNear(limits.x_max, 62.5, "x zoom should halve the span");
    RequireNear(limits.y_min, 7.5, "y zoom should use top-origin screen coordinates");
    RequireNear(limits.y_max, 17.5, "y zoom should halve the span");
}

void TestGestureBatchPreservesEventOrder()
{
    specforge::PlotTouchpadGestureBatch batch;
    batch.deltas.push_back(Pan(40.0f, 0.0f, specforge::PlotGestureAxes::Both));
    batch.deltas.push_back(Zoom(2.0, 300.0f, 300.0f, specforge::PlotGestureAxes::Both));

    specforge::PlotViewLimits limits{0.0, 100.0, 0.0, 20.0};
    Require(specforge::ApplyPlotTouchpadGestures(limits, batch), "gesture batch should change limits");
    RequireNear(limits.x_min, 15.0, "zoom should apply after pan using the updated range");
    RequireNear(limits.x_max, 65.0, "ordered batch should preserve final x span");
}

void TestInvalidGestureIsIgnored()
{
    specforge::PlotViewLimits limits{0.0, 100.0, 0.0, 20.0};
    specforge::PlotTouchpadGestureDelta gesture =
        Zoom(0.0, 300.0f, 300.0f, specforge::PlotGestureAxes::Both);
    Require(!specforge::ApplyPlotTouchpadGesture(limits, gesture), "zero zoom factor should be ignored");
    RequireNear(limits.x_min, 0.0, "invalid gesture should preserve limits");
    RequireNear(limits.x_max, 100.0, "invalid gesture should preserve limits");
}

void TestOverflowingGestureIsTransactional()
{
    specforge::PlotViewLimits limits{0.0, 100.0, 0.0, 20.0};
    const specforge::PlotTouchpadGestureDelta gesture = Zoom(
        std::numeric_limits<double>::min(),
        300.0f,
        300.0f,
        specforge::PlotGestureAxes::Both);
    Require(
        !specforge::ApplyPlotTouchpadGesture(limits, gesture),
        "overflowing zoom should be rejected");
    RequireNear(limits.x_min, 0.0, "rejected zoom should preserve x minimum");
    RequireNear(limits.x_max, 100.0, "rejected zoom should preserve x maximum");
    RequireNear(limits.y_min, 0.0, "rejected zoom should preserve y minimum");
    RequireNear(limits.y_max, 20.0, "rejected zoom should preserve y maximum");
}

}  // namespace

int main()
{
    TestTargetHitTestingUsesAxisZonesBeforePlotBody();
    TestPanMovesBothAxesWithDirectManipulationContent();
    TestAxisConstrainedPanLeavesOtherAxisUnchanged();
    TestPinchZoomKeepsGestureAnchorFixed();
    TestGestureBatchPreservesEventOrder();
    TestInvalidGestureIsIgnored();
    TestOverflowingGestureIsTransactional();
    return 0;
}
