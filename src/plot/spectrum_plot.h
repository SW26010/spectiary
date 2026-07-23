#pragma once

#include "overlays/spectral_line_catalog.h"
#include "plot/plot_touchpad_gesture.h"

#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace specforge {

class ProfileSink;

struct SpectrumPlotProfileContext {
    ProfileSink* sink = nullptr;
    std::uint64_t frame_index = 0;
};

struct SpectrumPlotStyle {
    ImVec4 line_color = ImVec4(0.34f, 0.63f, 0.86f, 1.0f);
    float line_weight = 1.4f;
};

struct SpectrumPlotOverlays {
    const SpectralLineMarker* const* spectral_lines = nullptr;
    std::size_t spectral_line_count = 0;
    bool show_spectral_line_labels = true;
    std::string_view layout_scope_id;
    ImFont* spectral_line_label_font = nullptr;
};

struct SpectrumPlotDisplayOptions {
    bool edge_axis_overlay = false;
    bool include_edge_pixels = false;
    bool native_transparent_axes = false;
};

[[nodiscard]] bool IsPlotPanDragActive(
    bool was_active,
    bool plot_hovered,
    bool left_button_down,
    bool left_button_dragging) noexcept;

}  // namespace specforge
