#include "plot/spectrum_plot.h"

#include "profile/profile_sink.h"

#include <imgui.h>
#include <implot.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>

namespace specforge {
namespace {

struct Bounds {
    double x_min = 0.0;
    double x_max = 1.0;
    double y_min = 0.0;
    double y_max = 1.0;
};

bool CanPlotSnapshot(const SpectrumSnapshotHandle& snapshot)
{
    if (!snapshot || !snapshot->capabilities.can_plot_current_spectrum) {
        return false;
    }

    const SpectrumValueVector& x_values = snapshot->current_spectrum.x_values;
    const SpectrumValueVector& y_values = snapshot->current_spectrum.y_values;
    return x_values && y_values && !x_values->empty() && x_values->size() == y_values->size() &&
           snapshot->current_spectrum.point_count == x_values->size() &&
           x_values->size() <= static_cast<std::size_t>(std::numeric_limits<int>::max());
}

Bounds ComputeBounds(const SpectrumSnapshot& snapshot)
{
    Bounds bounds;
    const SpectrumValueVector& x_values = snapshot.current_spectrum.x_values;
    const SpectrumValueVector& y_values = snapshot.current_spectrum.y_values;
    if (!x_values || !y_values || x_values->empty() || y_values->empty()) {
        return bounds;
    }

    const auto [x_min, x_max] = std::minmax_element(x_values->begin(), x_values->end());
    const auto [y_min, y_max] = std::minmax_element(y_values->begin(), y_values->end());
    bounds.x_min = *x_min;
    bounds.x_max = *x_max;
    bounds.y_min = *y_min;
    bounds.y_max = *y_max;

    const double y_padding = (bounds.y_max - bounds.y_min) * 0.08;
    if (y_padding > 0.0) {
        bounds.y_min -= y_padding;
        bounds.y_max += y_padding;
    } else {
        bounds.y_min -= 1.0;
        bounds.y_max += 1.0;
    }
    return bounds;
}

ProfileSink* ActiveProfileSink(const SpectrumPlotProfileContext& profile)
{
    if (profile.sink == nullptr || !profile.sink->is_open()) {
        return nullptr;
    }
    return profile.sink;
}

ProfileSink::Field NumberField(std::string_view name, double value)
{
    return ProfileSink::Field::Number(name, std::to_string(value));
}

bool LimitsChanged(const ImPlotRect& limits, const SpectrumPlotState& state)
{
    constexpr double kEpsilon = 1.0e-9;
    return !state.has_profile_limits || std::abs(limits.X.Min - state.profiled_x_min) > kEpsilon ||
           std::abs(limits.X.Max - state.profiled_x_max) > kEpsilon ||
           std::abs(limits.Y.Min - state.profiled_y_min) > kEpsilon ||
           std::abs(limits.Y.Max - state.profiled_y_max) > kEpsilon;
}

void StoreProfiledLimits(const ImPlotRect& limits, SpectrumPlotState& state)
{
    state.has_profile_limits = true;
    state.profiled_x_min = limits.X.Min;
    state.profiled_x_max = limits.X.Max;
    state.profiled_y_min = limits.Y.Min;
    state.profiled_y_max = limits.Y.Max;
}

void WritePanDragEvent(
    ProfileSink& sink,
    std::string_view event_name,
    std::uint64_t frame_index,
    bool active,
    bool hovered,
    const ImVec2& mouse,
    const ImPlotPoint& plot_point,
    const ImPlotRect& limits)
{
    sink.WriteEvent(event_name, {
                                    ProfileSink::Field::Number("frame", std::to_string(frame_index)),
                                    ProfileSink::Field::Bool("active", active),
                                    ProfileSink::Field::Bool("hovered", hovered),
                                    NumberField("mouse_x", mouse.x),
                                    NumberField("mouse_y", mouse.y),
                                    NumberField("plot_x", plot_point.x),
                                    NumberField("plot_y", plot_point.y),
                                    NumberField("x_min", limits.X.Min),
                                    NumberField("x_max", limits.X.Max),
                                    NumberField("y_min", limits.Y.Min),
                                    NumberField("y_max", limits.Y.Max),
                                });
}

bool IsVisibleInPlot(const SpectralLineMarker& marker, const ImPlotRect& limits)
{
    if (marker.kind == SpectralLineMarkerKind::Line) {
        return marker.vacuum_angstrom && *marker.vacuum_angstrom >= limits.X.Min &&
               *marker.vacuum_angstrom <= limits.X.Max;
    }
    return marker.start_vacuum_angstrom && marker.end_vacuum_angstrom &&
           *marker.end_vacuum_angstrom >= limits.X.Min && *marker.start_vacuum_angstrom <= limits.X.Max;
}

ImVec4 SpectralLineColor(const SpectralLineMarker& marker)
{
    if (marker.group == "Balmer") {
        return ImVec4(0.95f, 0.42f, 0.35f, 0.78f);
    }
    if (marker.group == "CN" || marker.group == "CH" || marker.group == "C2" || marker.group == "Isotope") {
        return ImVec4(0.43f, 0.78f, 0.64f, 0.76f);
    }
    if (marker.group == "Ba II" || marker.group == "Sr II") {
        return ImVec4(0.95f, 0.72f, 0.32f, 0.78f);
    }
    return ImVec4(0.66f, 0.72f, 0.82f, 0.72f);
}

void RenderSpectralLineOverlays(const SpectrumPlotOverlays& overlays)
{
    if (overlays.spectral_lines == nullptr || overlays.spectral_line_count == 0) {
        return;
    }

    const ImPlotRect limits = ImPlot::GetPlotLimits();
    const double y_span = limits.Y.Max - limits.Y.Min;
    if (y_span <= 0.0) {
        return;
    }

    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    if (draw_list == nullptr) {
        return;
    }

    ImPlot::PushPlotClipRect();
    int visible_index = 0;
    for (std::size_t index = 0; index < overlays.spectral_line_count; ++index) {
        const SpectralLineMarker* marker = overlays.spectral_lines[index];
        if (marker == nullptr || !IsVisibleInPlot(*marker, limits)) {
            continue;
        }

        const ImVec4 color = SpectralLineColor(*marker);
        const double label_y = limits.Y.Min + y_span * (0.92 - 0.08 * static_cast<double>(visible_index % 3));

        if (marker->kind == SpectralLineMarkerKind::Band) {
            const ImVec2 start_min = ImPlot::PlotToPixels(*marker->start_vacuum_angstrom, limits.Y.Min);
            const ImVec2 end_max = ImPlot::PlotToPixels(*marker->end_vacuum_angstrom, limits.Y.Max);
            const ImVec2 rect_min(std::min(start_min.x, end_max.x), std::min(start_min.y, end_max.y));
            const ImVec2 rect_max(std::max(start_min.x, end_max.x), std::max(start_min.y, end_max.y));
            draw_list->AddRectFilled(rect_min, rect_max, ImGui::GetColorU32(ImVec4(color.x, color.y, color.z, 0.10f)));
            draw_list->AddRect(rect_min, rect_max, ImGui::GetColorU32(ImVec4(color.x, color.y, color.z, 0.34f)));
            if (overlays.show_spectral_line_labels) {
                const double x_mid = (*marker->start_vacuum_angstrom + *marker->end_vacuum_angstrom) * 0.5;
                const ImVec2 label_pos = ImPlot::PlotToPixels(x_mid, label_y);
                draw_list->AddText(
                    ImVec2(label_pos.x + 4.0f, label_pos.y),
                    ImGui::GetColorU32(ImVec4(color.x, color.y, color.z, 0.92f)),
                    marker->display_label.c_str());
            }
        } else if (marker->vacuum_angstrom) {
            const double x = *marker->vacuum_angstrom;
            const ImVec2 bottom = ImPlot::PlotToPixels(x, limits.Y.Min);
            const ImVec2 top = ImPlot::PlotToPixels(x, limits.Y.Max);
            draw_list->AddLine(bottom, top, ImGui::GetColorU32(color), 1.0f);
            if (overlays.show_spectral_line_labels) {
                const ImVec2 label_pos = ImPlot::PlotToPixels(x, label_y);
                draw_list->AddText(
                    ImVec2(label_pos.x + 4.0f, label_pos.y),
                    ImGui::GetColorU32(ImVec4(color.x, color.y, color.z, 0.95f)),
                    marker->display_label.c_str());
            }
        }
        ++visible_index;
    }
    ImPlot::PopPlotClipRect();
}

}  // namespace

void RenderSpectrumPlot(
    const SpectrumSnapshotHandle& snapshot,
    SpectrumPlotState& state,
    const SpectrumPlotProfileContext& profile,
    const SpectrumPlotStyle& style,
    const SpectrumPlotOverlays& overlays)
{
    if (!CanPlotSnapshot(snapshot)) {
        ImGui::TextDisabled("No plottable spectrum.");
        return;
    }

    if (state.fit_next_frame) {
        const Bounds bounds = ComputeBounds(*snapshot);
        ImPlot::SetNextAxesLimits(bounds.x_min, bounds.x_max, bounds.y_min, bounds.y_max, ImPlotCond_Always);
        state.fit_next_frame = false;
    }

    if (ImPlot::BeginPlot("Spectrum##main_spectrum", ImVec2(-1.0f, -1.0f), ImPlotFlags_Crosshairs)) {
        const char* x_label = snapshot->axis.x_label.empty() ? "x" : snapshot->axis.x_label.c_str();
        const char* y_label = snapshot->axis.y_label.empty() ? "y" : snapshot->axis.y_label.c_str();
        ImPlot::SetupAxis(ImAxis_X1, x_label);
        ImPlot::SetupAxis(ImAxis_Y1, y_label);

        ImPlotSpec spec;
        spec.LineColor = style.line_color;
        spec.LineWeight = style.line_weight;
        spec.MarkerLineColor = spec.LineColor;
        spec.MarkerFillColor = spec.LineColor;
        if (state.show_points) {
            spec.Marker = ImPlotMarker_Circle;
            spec.MarkerSize = 2.0f;
        }
        const SpectrumValueVector& x_values = snapshot->current_spectrum.x_values;
        const SpectrumValueVector& y_values = snapshot->current_spectrum.y_values;
        const std::string& name = snapshot->current_spectrum.name;
        ImPlot::PlotLine(
            name.empty() ? "current spectrum" : name.c_str(),
            x_values->data(),
            y_values->data(),
            static_cast<int>(x_values->size()),
            spec);

        if (snapshot->capabilities.can_show_spectral_lines) {
            RenderSpectralLineOverlays(overlays);
        }

        if (ProfileSink* sink = ActiveProfileSink(profile)) {
            const ImPlotRect limits = ImPlot::GetPlotLimits();
            const bool hovered = ImPlot::IsPlotHovered();
            const bool left_down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
            const bool left_dragging = ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f);
            const bool pan_drag_active = left_down && (state.pan_drag_active || (hovered && left_dragging));
            const ImVec2 mouse = ImGui::GetMousePos();
            const ImPlotPoint plot_point = ImPlot::PixelsToPlot(mouse);

            if (pan_drag_active != state.pan_drag_active) {
                WritePanDragEvent(
                    *sink,
                    "implot.pan_drag.state",
                    profile.frame_index,
                    pan_drag_active,
                    hovered,
                    mouse,
                    plot_point,
                    limits);
            }
            if (pan_drag_active) {
                WritePanDragEvent(
                    *sink,
                    "implot.pan_drag.sample",
                    profile.frame_index,
                    pan_drag_active,
                    hovered,
                    mouse,
                    plot_point,
                    limits);
            }
            if (LimitsChanged(limits, state)) {
                sink->WriteEvent("implot.axis_limits_changed", {
                                                                  ProfileSink::Field::Number(
                                                                      "frame",
                                                                      std::to_string(profile.frame_index)),
                                                                  ProfileSink::Field::Bool(
                                                                      "pan_drag_active",
                                                                      pan_drag_active),
                                                                  NumberField("x_min", limits.X.Min),
                                                                  NumberField("x_max", limits.X.Max),
                                                                  NumberField("y_min", limits.Y.Min),
                                                                  NumberField("y_max", limits.Y.Max),
                                                              });
                StoreProfiledLimits(limits, state);
            }
            state.pan_drag_active = pan_drag_active;
        }

        ImPlot::EndPlot();
    }
}

}  // namespace specforge
