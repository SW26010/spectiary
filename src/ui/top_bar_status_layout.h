#pragma once

namespace specforge {

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

[[nodiscard]] constexpr TopBarStatusLayout ResolveTopBarStatusLayout(
    float available_width,
    const TopBarStatusWidths& widths,
    bool operation_important,
    bool profile_important) noexcept
{
    TopBarStatusLayout layout;

    const auto try_show = [&](bool& visible, float width) {
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

}  // namespace specforge
