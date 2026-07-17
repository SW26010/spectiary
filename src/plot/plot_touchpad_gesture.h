#pragma once

#include <cstdint>
#include <vector>

namespace specforge {

struct PlotPixelRect {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;

    [[nodiscard]] bool IsValid() const;
    [[nodiscard]] bool Contains(float x, float y) const;
    [[nodiscard]] float Width() const;
    [[nodiscard]] float Height() const;
};

enum class PlotGestureAxes {
    Both,
    XOnly,
    YOnly,
};

struct PlotTouchpadTarget {
    std::uintptr_t native_window = 0;
    PlotPixelRect plot_rect;
    PlotPixelRect x_axis_rect;
    PlotPixelRect y_axis_rect;
};

enum class PlotTouchpadGestureKind {
    Pan,
    Zoom,
};

struct PlotTouchpadGestureDelta {
    PlotTouchpadGestureKind kind = PlotTouchpadGestureKind::Pan;
    PlotGestureAxes axes = PlotGestureAxes::Both;
    PlotPixelRect plot_rect;
    float anchor_x = 0.0f;
    float anchor_y = 0.0f;
    float pan_x = 0.0f;
    float pan_y = 0.0f;
    double zoom_factor = 1.0;
    bool inertia = false;
    std::int64_t input_steady_ns = 0;
};

struct PlotTouchpadGestureBatch {
    std::vector<PlotTouchpadGestureDelta> deltas;
    bool active = false;
};

class PlotTouchpadGestureSource {
public:
    virtual ~PlotTouchpadGestureSource() = default;

    [[nodiscard]] virtual PlotTouchpadGestureBatch Poll(std::uintptr_t native_window) = 0;
    virtual void SetTarget(const PlotTouchpadTarget& target) = 0;
    virtual void ClearTarget() = 0;
};

struct PlotViewLimits {
    double x_min = 0.0;
    double x_max = 0.0;
    double y_min = 0.0;
    double y_max = 0.0;
};

[[nodiscard]] PlotGestureAxes HitTestPlotTouchpadTarget(
    const PlotTouchpadTarget& target,
    float screen_x,
    float screen_y,
    bool* hit = nullptr);

[[nodiscard]] bool ApplyPlotTouchpadGesture(
    PlotViewLimits& limits,
    const PlotTouchpadGestureDelta& gesture);

[[nodiscard]] bool ApplyPlotTouchpadGestures(
    PlotViewLimits& limits,
    const PlotTouchpadGestureBatch& gestures);

}  // namespace specforge
