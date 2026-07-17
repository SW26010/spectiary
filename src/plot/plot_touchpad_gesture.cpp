#include "plot/plot_touchpad_gesture.h"

#include <algorithm>
#include <cmath>

namespace specforge {
namespace {

bool UsesXAxis(PlotGestureAxes axes)
{
    return axes != PlotGestureAxes::YOnly;
}

bool UsesYAxis(PlotGestureAxes axes)
{
    return axes != PlotGestureAxes::XOnly;
}

bool LimitsAreUsable(const PlotViewLimits& limits)
{
    return std::isfinite(limits.x_min) && std::isfinite(limits.x_max) &&
           std::isfinite(limits.y_min) && std::isfinite(limits.y_max) &&
           limits.x_min < limits.x_max && limits.y_min < limits.y_max;
}

void ZoomRangeAround(double& min, double& max, double anchor, double factor)
{
    min = anchor - (anchor - min) / factor;
    max = anchor + (max - anchor) / factor;
}

}  // namespace

bool PlotPixelRect::IsValid() const
{
    return std::isfinite(left) && std::isfinite(top) && std::isfinite(right) &&
           std::isfinite(bottom) && right > left && bottom > top;
}

bool PlotPixelRect::Contains(float x, float y) const
{
    return IsValid() && std::isfinite(x) && std::isfinite(y) &&
           x >= left && x <= right && y >= top && y <= bottom;
}

float PlotPixelRect::Width() const
{
    return right - left;
}

float PlotPixelRect::Height() const
{
    return bottom - top;
}

PlotGestureAxes HitTestPlotTouchpadTarget(
    const PlotTouchpadTarget& target,
    float screen_x,
    float screen_y,
    bool* hit)
{
    if (target.x_axis_rect.Contains(screen_x, screen_y)) {
        if (hit != nullptr) {
            *hit = true;
        }
        return PlotGestureAxes::XOnly;
    }
    if (target.y_axis_rect.Contains(screen_x, screen_y)) {
        if (hit != nullptr) {
            *hit = true;
        }
        return PlotGestureAxes::YOnly;
    }
    if (target.plot_rect.Contains(screen_x, screen_y)) {
        if (hit != nullptr) {
            *hit = true;
        }
        return PlotGestureAxes::Both;
    }

    if (hit != nullptr) {
        *hit = false;
    }
    return PlotGestureAxes::Both;
}

bool ApplyPlotTouchpadGesture(
    PlotViewLimits& limits,
    const PlotTouchpadGestureDelta& gesture)
{
    if (!LimitsAreUsable(limits) || !gesture.plot_rect.IsValid()) {
        return false;
    }

    PlotViewLimits candidate = limits;
    const double x_span = candidate.x_max - candidate.x_min;
    const double y_span = candidate.y_max - candidate.y_min;

    if (gesture.kind == PlotTouchpadGestureKind::Pan) {
        if (!std::isfinite(gesture.pan_x) || !std::isfinite(gesture.pan_y)) {
            return false;
        }

        bool changed = false;
        if (UsesXAxis(gesture.axes) && gesture.pan_x != 0.0f) {
            const double shift =
                -static_cast<double>(gesture.pan_x) * x_span /
                static_cast<double>(gesture.plot_rect.Width());
            candidate.x_min += shift;
            candidate.x_max += shift;
            changed = true;
        }
        if (UsesYAxis(gesture.axes) && gesture.pan_y != 0.0f) {
            const double shift =
                static_cast<double>(gesture.pan_y) * y_span /
                static_cast<double>(gesture.plot_rect.Height());
            candidate.y_min += shift;
            candidate.y_max += shift;
            changed = true;
        }
        if (!changed || !LimitsAreUsable(candidate)) {
            return false;
        }
        limits = candidate;
        return true;
    }

    if (!std::isfinite(gesture.zoom_factor) || gesture.zoom_factor <= 0.0 ||
        gesture.zoom_factor == 1.0) {
        return false;
    }

    bool changed = false;
    if (UsesXAxis(gesture.axes)) {
        const double ratio = std::clamp(
            static_cast<double>((gesture.anchor_x - gesture.plot_rect.left) /
                                gesture.plot_rect.Width()),
            0.0,
            1.0);
        const double anchor = candidate.x_min + x_span * ratio;
        ZoomRangeAround(candidate.x_min, candidate.x_max, anchor, gesture.zoom_factor);
        changed = true;
    }
    if (UsesYAxis(gesture.axes)) {
        const double ratio = std::clamp(
            static_cast<double>((gesture.anchor_y - gesture.plot_rect.top) /
                                gesture.plot_rect.Height()),
            0.0,
            1.0);
        const double anchor = candidate.y_max - y_span * ratio;
        ZoomRangeAround(candidate.y_min, candidate.y_max, anchor, gesture.zoom_factor);
        changed = true;
    }
    if (!changed || !LimitsAreUsable(candidate)) {
        return false;
    }
    limits = candidate;
    return true;
}

bool ApplyPlotTouchpadGestures(
    PlotViewLimits& limits,
    const PlotTouchpadGestureBatch& gestures)
{
    bool changed = false;
    for (const PlotTouchpadGestureDelta& gesture : gestures.deltas) {
        changed = ApplyPlotTouchpadGesture(limits, gesture) || changed;
    }
    return changed;
}

}  // namespace specforge
