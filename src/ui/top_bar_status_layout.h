#pragma once

#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace spectiary {

struct TopBarStatusWidths {
    float operation = 0.0f;
    float frame = 0.0f;
    float dimensions = 0.0f;
    float profile = 0.0f;
    float separator = 0.0f;
};

struct TopBarStatusLayout {
    bool show_operation = false;
    bool show_frame = false;
    bool show_dimensions = false;
    bool show_profile = false;
    float width = 0.0f;
};

struct TopBarFrameTimingSample {
    float delta_time_seconds = 0.0f;
};

inline constexpr float kMaximumTopBarFrameDeltaTime = 0.25f;

[[nodiscard]] inline std::optional<TopBarFrameTimingSample>
TryMakeTopBarFrameTimingSample(
    float delta_time_seconds,
    bool first_frame) noexcept
{
    if (first_frame ||
        !std::isfinite(delta_time_seconds) ||
        delta_time_seconds <= 0.0f ||
        delta_time_seconds > kMaximumTopBarFrameDeltaTime) {
        return std::nullopt;
    }
    return TopBarFrameTimingSample{.delta_time_seconds = delta_time_seconds};
}

[[nodiscard]] inline std::optional<std::string> FormatTopBarFrameRate(
    const TopBarFrameTimingSample& sample,
    std::string_view format)
{
    if (format.empty() ||
        !std::isfinite(sample.delta_time_seconds) ||
        sample.delta_time_seconds <= 0.0f ||
        sample.delta_time_seconds > kMaximumTopBarFrameDeltaTime) {
        return std::nullopt;
    }

    const float frame_time_ms = sample.delta_time_seconds * 1000.0f;
    const float framerate = 1.0f / sample.delta_time_seconds;
    if (!std::isfinite(frame_time_ms) || frame_time_ms <= 0.0f ||
        !std::isfinite(framerate) || framerate <= 0.0f) {
        return std::nullopt;
    }

    const std::string format_string(format);
    const int required = std::snprintf(
        nullptr,
        0,
        format_string.c_str(),
        frame_time_ms,
        framerate);
    if (required <= 0) {
        return std::nullopt;
    }

    std::string result(static_cast<std::size_t>(required), '\0');
    const int written = std::snprintf(
        result.data(),
        result.size() + 1,
        format_string.c_str(),
        frame_time_ms,
        framerate);
    if (written != required) {
        return std::nullopt;
    }
    return result;
}

[[nodiscard]] constexpr TopBarStatusLayout ResolveTopBarStatusLayout(
    float available_width,
    const TopBarStatusWidths& widths,
    bool operation_important,
    bool profile_important) noexcept
{
    TopBarStatusLayout layout;

    const auto try_show = [&](bool& visible, float width) {
        if (width <= 0.0f) {
            return;
        }
        const float required_width = width + (layout.width > 0.0f ? widths.separator : 0.0f);
        if (layout.width + required_width > available_width) {
            return;
        }
        visible = true;
        layout.width += required_width;
    };

    // Loading and failure states outrank recording. When the source is idle,
    // active recording outranks the routine "Ready" state.
    if (operation_important) {
        try_show(layout.show_operation, widths.operation);
    }
    if (profile_important) {
        try_show(layout.show_profile, widths.profile);
    }
    if (!operation_important) {
        try_show(layout.show_operation, widths.operation);
    }

    // Routine diagnostics yield from least to most useful as the bar narrows.
    // An inactive profile is intentionally absent from the everyday status bar.
    try_show(layout.show_dimensions, widths.dimensions);
    try_show(layout.show_frame, widths.frame);

    return layout;
}

}  // namespace spectiary
