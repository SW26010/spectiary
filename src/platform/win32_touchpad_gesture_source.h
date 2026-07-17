#pragma once

#include "plot/plot_touchpad_gesture.h"

#include <memory>

namespace specforge {

class Win32TouchpadGestureSource final : public PlotTouchpadGestureSource {
public:
    Win32TouchpadGestureSource();
    ~Win32TouchpadGestureSource() override;

    Win32TouchpadGestureSource(const Win32TouchpadGestureSource&) = delete;
    Win32TouchpadGestureSource& operator=(const Win32TouchpadGestureSource&) = delete;

    [[nodiscard]] PlotTouchpadGestureBatch Poll(std::uintptr_t native_window) override;
    void SetTarget(const PlotTouchpadTarget& target) override;
    void ClearTarget() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace specforge
