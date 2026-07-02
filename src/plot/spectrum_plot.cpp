#include "plot/spectrum_plot.h"

#include "profile/profile_sink.h"

#include <imgui.h>
#include <implot.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
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

struct ScopedEdgeAxisPlotStyle {
    explicit ScopedEdgeAxisPlotStyle(bool enabled)
        : enabled_(enabled)
    {
        if (!enabled_) {
            return;
        }

        const ImVec4 transparent(0.0f, 0.0f, 0.0f, 0.0f);
        ImPlot::PushStyleColor(ImPlotCol_FrameBg, transparent);
        ImPlot::PushStyleColor(ImPlotCol_PlotBg, transparent);
        ImPlot::PushStyleColor(ImPlotCol_PlotBorder, transparent);
        ImPlot::PushStyleColor(ImPlotCol_AxisBg, transparent);
        ImPlot::PushStyleColor(ImPlotCol_AxisBgHovered, transparent);
        ImPlot::PushStyleColor(ImPlotCol_AxisBgActive, transparent);
        ImPlot::PushStyleColor(ImPlotCol_Crosshairs, ImVec4(0.72f, 0.78f, 0.82f, 0.48f));
        ImPlot::PushStyleVar(ImPlotStyleVar_PlotBorderSize, 0.0f);
        ImPlot::PushStyleVar(ImPlotStyleVar_PlotPadding, ImVec2(0.0f, 0.0f));
        ImPlot::PushStyleVar(ImPlotStyleVar_LabelPadding, ImVec2(0.0f, 0.0f));
    }

    ~ScopedEdgeAxisPlotStyle()
    {
        if (!enabled_) {
            return;
        }

        ImPlot::PopStyleVar(3);
        ImPlot::PopStyleColor(7);
    }

    ScopedEdgeAxisPlotStyle(const ScopedEdgeAxisPlotStyle&) = delete;
    ScopedEdgeAxisPlotStyle& operator=(const ScopedEdgeAxisPlotStyle&) = delete;

private:
    bool enabled_ = false;
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

bool SmoothingActive(const SpectrumPlotState& state)
{
    return state.show_smoothed && state.smoothing.method != SpectrumSmoothingMethod::None;
}

const char* SmoothingLabel(const SpectrumSmoothingSettings& settings)
{
    switch (settings.method) {
    case SpectrumSmoothingMethod::Gaussian:
        return "Gaussian smoothing";
    case SpectrumSmoothingMethod::Median:
        return "Median smoothing";
    case SpectrumSmoothingMethod::None:
    default:
        return "current spectrum";
    }
}

SpectrumValueVector SmoothedValuesFor(const SpectrumValueVector& y_values, SpectrumPlotState& state)
{
    if (!y_values) {
        state.smoothing_cache_source.reset();
        state.smoothed_y_values.reset();
        return {};
    }

    const bool cache_valid = state.smoothing_cache_source == y_values &&
                             state.smoothing_cache_settings == state.smoothing && state.smoothed_y_values &&
                             state.smoothed_y_values->size() == y_values->size();
    if (cache_valid) {
        return state.smoothed_y_values;
    }

    auto smoothed = std::make_shared<std::vector<double>>(SmoothSpectrumValues(*y_values, state.smoothing));
    state.smoothing_cache_source = y_values;
    state.smoothing_cache_settings = state.smoothing;
    state.smoothed_y_values = std::move(smoothed);
    return state.smoothed_y_values;
}

void ExpandYBounds(Bounds& bounds, const std::vector<double>& y_values, bool& has_y_bounds)
{
    if (y_values.empty()) {
        return;
    }

    const auto [y_min, y_max] = std::minmax_element(y_values.begin(), y_values.end());
    if (!has_y_bounds) {
        bounds.y_min = *y_min;
        bounds.y_max = *y_max;
        has_y_bounds = true;
        return;
    }

    bounds.y_min = std::min(bounds.y_min, *y_min);
    bounds.y_max = std::max(bounds.y_max, *y_max);
}

Bounds ComputeBounds(const SpectrumSnapshot& snapshot, SpectrumPlotState& state)
{
    Bounds bounds;
    const SpectrumValueVector& x_values = snapshot.current_spectrum.x_values;
    const SpectrumValueVector& y_values = snapshot.current_spectrum.y_values;
    if (!x_values || !y_values || x_values->empty() || y_values->empty()) {
        return bounds;
    }

    const auto [x_min, x_max] = std::minmax_element(x_values->begin(), x_values->end());
    bounds.x_min = *x_min;
    bounds.x_max = *x_max;

    bool has_y_bounds = false;
    if (SmoothingActive(state)) {
        const SpectrumValueVector smoothed_values = SmoothedValuesFor(y_values, state);
        if (smoothed_values && smoothed_values->size() == y_values->size()) {
            ExpandYBounds(bounds, *smoothed_values, has_y_bounds);
            if (state.show_raw_when_smoothed) {
                ExpandYBounds(bounds, *y_values, has_y_bounds);
            }
        }
    }
    if (!has_y_bounds) {
        ExpandYBounds(bounds, *y_values, has_y_bounds);
    }

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

int DesiredTickCount(float pixel_length, float target_spacing, int minimum, int maximum)
{
    if (pixel_length <= 0.0f || target_spacing <= 0.0f) {
        return minimum;
    }
    return std::clamp(static_cast<int>(std::floor(pixel_length / target_spacing)), minimum, maximum);
}

double NiceTickStep(double range, int desired_count)
{
    if (!std::isfinite(range) || range <= 0.0 || desired_count <= 1) {
        return 1.0;
    }

    const double raw_step = range / static_cast<double>(desired_count - 1);
    if (!std::isfinite(raw_step) || raw_step <= 0.0) {
        return 1.0;
    }

    const double magnitude = std::pow(10.0, std::floor(std::log10(raw_step)));
    const double fraction = raw_step / magnitude;
    double nice_fraction = 10.0;
    if (fraction <= 1.0) {
        nice_fraction = 1.0;
    } else if (fraction <= 2.0) {
        nice_fraction = 2.0;
    } else if (fraction <= 5.0) {
        nice_fraction = 5.0;
    }
    return nice_fraction * magnitude;
}

std::string FormatTickValue(double value, double step)
{
    if (std::abs(value) < std::abs(step) * 1.0e-6) {
        value = 0.0;
    }

    char buffer[64] = {};
    const double abs_value = std::abs(value);
    const char* format = (abs_value >= 10000.0 || (abs_value > 0.0 && abs_value < 0.01)) ? "%.3g" : "%.4g";
    std::snprintf(buffer, sizeof(buffer), format, value);
    return std::string(buffer);
}

float ClampTextStart(float desired, float minimum, float maximum)
{
    if (maximum < minimum) {
        return minimum;
    }
    return std::clamp(desired, minimum, maximum);
}

void RenderEdgeAxisTicks()
{
    const ImPlotRect limits = ImPlot::GetPlotLimits();
    const ImVec2 plot_pos = ImPlot::GetPlotPos();
    const ImVec2 plot_size = ImPlot::GetPlotSize();
    if (plot_size.x <= 0.0f || plot_size.y <= 0.0f) {
        return;
    }

    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    if (draw_list == nullptr) {
        return;
    }

    const ImVec2 plot_min = plot_pos;
    const ImVec2 plot_max(plot_pos.x + plot_size.x, plot_pos.y + plot_size.y);
    const ImU32 tick_color = ImGui::GetColorU32(ImVec4(0.72f, 0.78f, 0.82f, 0.58f));
    const ImU32 label_color = ImGui::GetColorU32(ImVec4(0.78f, 0.84f, 0.88f, 0.74f));
    constexpr float kMajorTickLength = 9.0f;
    constexpr float kTickLabelGap = 4.0f;

    ImPlot::PushPlotClipRect();

    const int x_tick_count = DesiredTickCount(plot_size.x, 88.0f, 6, 14);
    const double x_step = NiceTickStep(limits.X.Max - limits.X.Min, x_tick_count);
    const double x_start = std::ceil(limits.X.Min / x_step) * x_step;
    for (int index = 0; index < 128; ++index) {
        const double x = x_start + x_step * static_cast<double>(index);
        if (x > limits.X.Max + x_step * 0.5) {
            break;
        }
        if (x < limits.X.Min - x_step * 0.5) {
            continue;
        }

        const float pixel_x = ImPlot::PlotToPixels(x, limits.Y.Min).x;
        draw_list->AddLine(
            ImVec2(pixel_x, plot_max.y),
            ImVec2(pixel_x, plot_max.y - kMajorTickLength),
            tick_color,
            1.0f);

        const std::string label = FormatTickValue(x, x_step);
        const ImVec2 label_size = ImGui::CalcTextSize(label.c_str());
        const float label_x = ClampTextStart(
            pixel_x - label_size.x * 0.5f,
            plot_min.x + 2.0f,
            plot_max.x - label_size.x - 2.0f);
        const float label_y = plot_max.y - kMajorTickLength - kTickLabelGap - label_size.y;
        draw_list->AddText(ImVec2(label_x, label_y), label_color, label.c_str());
    }

    const int y_tick_count = DesiredTickCount(plot_size.y, 72.0f, 5, 12);
    const double y_step = NiceTickStep(limits.Y.Max - limits.Y.Min, y_tick_count);
    const double y_start = std::ceil(limits.Y.Min / y_step) * y_step;
    for (int index = 0; index < 128; ++index) {
        const double y = y_start + y_step * static_cast<double>(index);
        if (y > limits.Y.Max + y_step * 0.5) {
            break;
        }
        if (y < limits.Y.Min - y_step * 0.5) {
            continue;
        }

        const float pixel_y = ImPlot::PlotToPixels(limits.X.Min, y).y;
        draw_list->AddLine(
            ImVec2(plot_min.x, pixel_y),
            ImVec2(plot_min.x + kMajorTickLength, pixel_y),
            tick_color,
            1.0f);

        const std::string label = FormatTickValue(y, y_step);
        const ImVec2 label_size = ImGui::CalcTextSize(label.c_str());
        const float label_x = plot_min.x + kMajorTickLength + kTickLabelGap;
        const float label_y = ClampTextStart(
            pixel_y - label_size.y * 0.5f,
            plot_min.y + 2.0f,
            plot_max.y - label_size.y - 2.0f);
        draw_list->AddText(ImVec2(label_x, label_y), label_color, label.c_str());
    }

    ImPlot::PopPlotClipRect();
}

void StoreLastLimits(const ImPlotRect& limits, SpectrumPlotState& state)
{
    state.has_last_limits = true;
    state.last_x_min = limits.X.Min;
    state.last_x_max = limits.X.Max;
    state.last_y_min = limits.Y.Min;
    state.last_y_max = limits.Y.Max;
}

bool LastLimitsAreUsable(const SpectrumPlotState& state)
{
    return state.has_last_limits && std::isfinite(state.last_x_min) && std::isfinite(state.last_x_max) &&
           std::isfinite(state.last_y_min) && std::isfinite(state.last_y_max) &&
           state.last_x_min < state.last_x_max && state.last_y_min < state.last_y_max;
}

bool ContainsPoint(const ImVec2& min, const ImVec2& max, const ImVec2& point)
{
    return point.x >= min.x && point.x <= max.x && point.y >= min.y && point.y <= max.y;
}

void ZoomRangeAround(double& min, double& max, double anchor, float wheel_delta)
{
    const double span = max - min;
    if (!std::isfinite(span) || span <= 0.0 || !std::isfinite(anchor) || wheel_delta == 0.0f) {
        return;
    }

    constexpr double kZoomBase = 0.88;
    const double scale = std::pow(kZoomBase, static_cast<double>(wheel_delta));
    min = anchor - (anchor - min) * scale;
    max = anchor + (max - anchor) * scale;
}

bool ApplyEdgeAxisWheelZoom(
    SpectrumPlotState& state,
    const ImVec2& widget_pos,
    const ImVec2& widget_size,
    float edge_band)
{
    if (!state.has_last_limits || widget_size.x <= 0.0f || widget_size.y <= 0.0f) {
        return false;
    }

    const ImGuiIO& io = ImGui::GetIO();
    if (io.MouseWheel == 0.0f) {
        return false;
    }

    const ImVec2 widget_max(widget_pos.x + widget_size.x, widget_pos.y + widget_size.y);
    if (!ContainsPoint(widget_pos, widget_max, io.MousePos)) {
        return false;
    }

    const bool in_x_axis_band = io.MousePos.y >= widget_max.y - edge_band;
    const bool in_y_axis_band = io.MousePos.x <= widget_pos.x + edge_band;
    if (!in_x_axis_band && !in_y_axis_band) {
        return false;
    }

    if (in_x_axis_band) {
        const double ratio = std::clamp(
            static_cast<double>((io.MousePos.x - widget_pos.x) / widget_size.x),
            0.0,
            1.0);
        const double anchor = state.last_x_min + (state.last_x_max - state.last_x_min) * ratio;
        double min = state.last_x_min;
        double max = state.last_x_max;
        ZoomRangeAround(min, max, anchor, io.MouseWheel);
        ImPlot::SetNextAxisLimits(ImAxis_X1, min, max, ImPlotCond_Always);
        return true;
    }

    const double ratio = std::clamp(
        static_cast<double>((io.MousePos.y - widget_pos.y) / widget_size.y),
        0.0,
        1.0);
    const double anchor = state.last_y_max - (state.last_y_max - state.last_y_min) * ratio;
    double min = state.last_y_min;
    double max = state.last_y_max;
    ZoomRangeAround(min, max, anchor, io.MouseWheel);
    ImPlot::SetNextAxisLimits(ImAxis_Y1, min, max, ImPlotCond_Always);
    return true;
}

ImVec2 PlotSizeForDisplay(const SpectrumPlotDisplayOptions& display, const ImVec2& available_size)
{
    if (!display.include_edge_pixels || available_size.x <= 0.0f || available_size.y <= 0.0f) {
        return ImVec2(-1.0f, -1.0f);
    }

    // Dear ImGui's rectangle hit test includes Min but excludes Max. The immersive plot
    // intentionally reaches the window edge, so give ImPlot one clipped pixel past the
    // visible right/bottom edges and keep native hover, crosshair, mouse text, and pan.
    return ImVec2(available_size.x + 1.0f, available_size.y + 1.0f);
}

bool ApplyStoredLimitsOnNextRender(SpectrumPlotState& state)
{
    if (!state.sync_last_limits_next_frame) {
        return false;
    }

    state.sync_last_limits_next_frame = false;
    if (!LastLimitsAreUsable(state)) {
        return false;
    }

    ImPlot::SetNextAxesLimits(
        state.last_x_min,
        state.last_x_max,
        state.last_y_min,
        state.last_y_max,
        ImPlotCond_Always);
    return true;
}

}  // namespace

void RenderSpectrumPlot(
    const SpectrumSnapshotHandle& snapshot,
    SpectrumPlotState& state,
    const SpectrumPlotProfileContext& profile,
    const SpectrumPlotStyle& style,
    const SpectrumPlotOverlays& overlays,
    const SpectrumPlotDisplayOptions& display)
{
    if (!CanPlotSnapshot(snapshot)) {
        ImGui::TextDisabled("No plottable spectrum.");
        return;
    }

    const bool fit_requested = state.fit_next_frame;
    if (state.fit_next_frame) {
        const Bounds bounds = ComputeBounds(*snapshot, state);
        ImPlot::SetNextAxesLimits(bounds.x_min, bounds.x_max, bounds.y_min, bounds.y_max, ImPlotCond_Always);
        state.fit_next_frame = false;
        state.sync_last_limits_next_frame = false;
    } else {
        ApplyStoredLimitsOnNextRender(state);
    }

    constexpr float kEdgeAxisBandPixels = 44.0f;
    const ImVec2 plot_widget_pos = ImGui::GetCursorScreenPos();
    const ImVec2 plot_widget_size = ImGui::GetContentRegionAvail();
    const ImVec2 plot_size = PlotSizeForDisplay(display, plot_widget_size);
    const bool edge_axis_wheel_zoomed =
        display.edge_axis_overlay && !fit_requested &&
        ApplyEdgeAxisWheelZoom(state, plot_widget_pos, plot_widget_size, kEdgeAxisBandPixels);

    const ScopedEdgeAxisPlotStyle edge_axis_style(display.edge_axis_overlay);
    ImPlotFlags plot_flags = ImPlotFlags_Crosshairs;
    ImPlotAxisFlags axis_flags = ImPlotAxisFlags_None;
    if (display.edge_axis_overlay) {
        plot_flags |= ImPlotFlags_NoFrame | ImPlotFlags_NoTitle | ImPlotFlags_NoLegend;
        axis_flags |= ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoTickMarks |
                      ImPlotAxisFlags_NoTickLabels | ImPlotAxisFlags_NoMenus |
                      ImPlotAxisFlags_NoSideSwitch | ImPlotAxisFlags_NoHighlight;
    }
    if (edge_axis_wheel_zoomed) {
        plot_flags |= ImPlotFlags_NoInputs;
    }

    if (ImPlot::BeginPlot("Spectrum##main_spectrum", plot_size, plot_flags)) {
        const char* x_label = snapshot->axis.x_label.empty() ? "x" : snapshot->axis.x_label.c_str();
        const char* y_label = snapshot->axis.y_label.empty() ? "y" : snapshot->axis.y_label.c_str();
        ImPlot::SetupAxis(ImAxis_X1, x_label, axis_flags);
        ImPlot::SetupAxis(ImAxis_Y1, y_label, axis_flags);

        const SpectrumValueVector& x_values = snapshot->current_spectrum.x_values;
        const SpectrumValueVector& y_values = snapshot->current_spectrum.y_values;
        const std::string& name = snapshot->current_spectrum.name;

        ImPlotSpec base_spec;
        base_spec.LineColor = style.line_color;
        base_spec.LineWeight = style.line_weight;
        base_spec.MarkerLineColor = base_spec.LineColor;
        base_spec.MarkerFillColor = base_spec.LineColor;

        if (SmoothingActive(state)) {
            if (state.show_raw_when_smoothed) {
                ImPlotSpec raw_spec = base_spec;
                raw_spec.LineColor.w = 0.30f;
                raw_spec.LineWeight = std::max(1.0f, style.line_weight * 0.80f);
                raw_spec.MarkerLineColor = raw_spec.LineColor;
                raw_spec.MarkerFillColor = raw_spec.LineColor;
                ImPlot::PlotLine(
                    "raw spectrum",
                    x_values->data(),
                    y_values->data(),
                    static_cast<int>(x_values->size()),
                    raw_spec);
            }

            const SpectrumValueVector smoothed_values = SmoothedValuesFor(y_values, state);
            if (smoothed_values && smoothed_values->size() == x_values->size()) {
                ImPlotSpec smoothed_spec = base_spec;
                smoothed_spec.LineColor = ImVec4(0.94f, 0.36f, 0.22f, 1.0f);
                smoothed_spec.LineWeight = std::max(1.0f, style.line_weight * 1.08f);
                smoothed_spec.MarkerLineColor = smoothed_spec.LineColor;
                smoothed_spec.MarkerFillColor = smoothed_spec.LineColor;
                if (state.show_points) {
                    smoothed_spec.Marker = ImPlotMarker_Circle;
                    smoothed_spec.MarkerSize = 2.0f;
                }
                ImPlot::PlotLine(
                    SmoothingLabel(state.smoothing),
                    x_values->data(),
                    smoothed_values->data(),
                    static_cast<int>(x_values->size()),
                    smoothed_spec);
            }
        } else {
            if (state.show_points) {
                base_spec.Marker = ImPlotMarker_Circle;
                base_spec.MarkerSize = 2.0f;
            }
            ImPlot::PlotLine(
                name.empty() ? "current spectrum" : name.c_str(),
                x_values->data(),
                y_values->data(),
                static_cast<int>(x_values->size()),
                base_spec);
        }

        if (snapshot->capabilities.can_show_spectral_lines) {
            RenderSpectralLineOverlays(overlays);
        }
        if (display.edge_axis_overlay) {
            RenderEdgeAxisTicks();
        }

        const ImPlotRect limits = ImPlot::GetPlotLimits();
        StoreLastLimits(limits, state);

        if (ProfileSink* sink = ActiveProfileSink(profile)) {
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
