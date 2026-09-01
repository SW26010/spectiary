#include "plot/spectrum_plot_renderer.h"

#include "profile/profile_sink.h"
#include "ui/theme.h"

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

PlotSeriesColor& SpectrumSeriesColor(
    SpectrumPlotColors& colors,
    SpectrumPlotSeries series) noexcept
{
    switch (series) {
    case SpectrumPlotSeries::RawSpectrum:
        return colors.raw_spectrum;
    case SpectrumPlotSeries::GaussianSmoothing:
        return colors.gaussian_smoothing;
    case SpectrumPlotSeries::MedianSmoothing:
        return colors.median_smoothing;
    }
    return colors.raw_spectrum;
}

const PlotSeriesColor& SpectrumSeriesColor(
    const SpectrumPlotColors& colors,
    SpectrumPlotSeries series) noexcept
{
    switch (series) {
    case SpectrumPlotSeries::RawSpectrum:
        return colors.raw_spectrum;
    case SpectrumPlotSeries::GaussianSmoothing:
        return colors.gaussian_smoothing;
    case SpectrumPlotSeries::MedianSmoothing:
        return colors.median_smoothing;
    }
    return colors.raw_spectrum;
}

std::string_view SpectrumSeriesStableId(
    SpectrumPlotSeries series) noexcept
{
    switch (series) {
    case SpectrumPlotSeries::RawSpectrum:
        return kRawSpectrumPlotSeriesId;
    case SpectrumPlotSeries::GaussianSmoothing:
        return kGaussianSmoothingPlotSeriesId;
    case SpectrumPlotSeries::MedianSmoothing:
        return kMedianSmoothingPlotSeriesId;
    }
    return kRawSpectrumPlotSeriesId;
}

ImVec4 ResolveSpectrumSeriesColor(
    const SpectrumPlotColors& colors,
    SpectrumPlotSeries series,
    const SemanticPalette& theme_palette,
    StablePlotSeriesColorAssignments& assignments)
{
    return ResolvePlotSeriesColor(
        SpectrumSeriesColor(colors, series),
        theme_palette,
        assignments,
        SpectrumSeriesStableId(series));
}

ImVec4 ApplyRawSpectrumSmoothingEmphasis(
    const PlotSeriesColor& selection,
    const ImVec4& resolved_color) noexcept
{
    if (selection.mode() ==
        PlotSeriesColorMode::ExplicitColor) {
        return resolved_color;
    }
    ImVec4 de_emphasized = resolved_color;
    de_emphasized.w = 0.30f;
    return de_emphasized;
}

SpectralLineVisualColors ResolveSpectralLineVisualColors(
    const SpectralLinePlotMarker& marker,
    const SemanticPalette& theme_palette) noexcept
{
    const ImVec4 resolved = ResolvePlotSeriesColor(
        marker.color,
        theme_palette,
        marker.automatic_color_slot);
    return {
        .marker_and_label = resolved,
        .band_fill = ImVec4(
            resolved.x,
            resolved.y,
            resolved.z,
            resolved.w * 0.12f),
    };
}

namespace {

struct Bounds {
    double x_min = 0.0;
    double x_max = 1.0;
    double y_min = 0.0;
    double y_max = 1.0;
};

struct EdgeAxisMetrics {
    float edge_padding = 0.0f;
    float tick_length = 0.0f;
    float tick_label_gap = 0.0f;
    float x_tick_target_spacing = 0.0f;
    float y_tick_target_spacing = 0.0f;
    float interaction_band = 0.0f;
};

struct SpectrumPlotMetrics {
    SpectralLineLabelMetrics spectral_line_labels;
    EdgeAxisMetrics edge_axis;
};

SpectrumPlotMetrics MakeSpectrumPlotMetrics(float font_size, float text_line_height)
{
    const float em = std::max(1.0f, font_size);
    return {
        .spectral_line_labels = MakeSpectralLineLabelMetrics(font_size, text_line_height),
        .edge_axis = {
            .edge_padding = em * 0.125f,
            .tick_length = em * 0.55f,
            .tick_label_gap = em * 0.25f,
            .x_tick_target_spacing = em * 5.50f,
            .y_tick_target_spacing = em * 4.50f,
            .interaction_band = em * 2.75f,
        },
    };
}

struct ScopedTransparentPlotStyle {
    explicit ScopedTransparentPlotStyle(bool enabled)
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
        ImPlot::PushStyleColor(
            ImPlotCol_Crosshairs,
            ActiveSemanticPalette().plot_crosshair);
        ImPlot::PushStyleVar(ImPlotStyleVar_PlotBorderSize, 0.0f);
        ImPlot::PushStyleVar(ImPlotStyleVar_PlotPadding, ImVec2(0.0f, 0.0f));
        ImPlot::PushStyleVar(ImPlotStyleVar_LabelPadding, ImVec2(0.0f, 0.0f));
    }

    ~ScopedTransparentPlotStyle()
    {
        if (!enabled_) {
            return;
        }

        ImPlot::PopStyleVar(3);
        ImPlot::PopStyleColor(7);
    }

    ScopedTransparentPlotStyle(const ScopedTransparentPlotStyle&) = delete;
    ScopedTransparentPlotStyle& operator=(const ScopedTransparentPlotStyle&) = delete;

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

bool GaussianSmoothingActive(const SpectrumPlotState& state)
{
    return state.show_gaussian_smoothed;
}

bool MedianSmoothingActive(const SpectrumPlotState& state)
{
    return state.show_median_smoothed;
}

bool AnySmoothingActive(const SpectrumPlotState& state)
{
    return GaussianSmoothingActive(state) || MedianSmoothingActive(state);
}

UiTextId SmoothingTextId(SpectrumSmoothingMethod method)
{
    switch (method) {
    case SpectrumSmoothingMethod::Gaussian:
        return UiTextId::GaussianSmoothing;
    case SpectrumSmoothingMethod::Median:
        return UiTextId::MedianSmoothing;
    case SpectrumSmoothingMethod::None:
    default:
        return UiTextId::CurrentSpectrum;
    }
}

SpectrumValueVector SmoothedValuesFor(
    const SpectrumValueVector& y_values,
    SpectrumPlotState& state,
    SpectrumSmoothingMethod method)
{
    SpectrumSmoothingCache& cache =
        method == SpectrumSmoothingMethod::Gaussian
            ? state.gaussian_smoothing_cache
            : state.median_smoothing_cache;
    if (!y_values) {
        cache = {};
        return {};
    }

    const SpectrumSmoothingSettings settings{
        .method = method,
        .parameters = state.smoothing_parameters,
    };
    const bool smoothing_parameter_matches =
        method == SpectrumSmoothingMethod::Gaussian
            ? cache.settings.parameters.gaussian_sigma ==
                  settings.parameters.gaussian_sigma
            : cache.settings.parameters.median_kernel_size ==
                  settings.parameters.median_kernel_size;
    const bool cache_valid =
        cache.source == y_values && smoothing_parameter_matches && cache.values &&
        cache.values->size() == y_values->size();
    if (cache_valid) {
        return cache.values;
    }

    auto smoothed = std::make_shared<std::vector<double>>(
        SmoothSpectrumValues(*y_values, settings));
    cache.source = y_values;
    cache.settings = settings;
    cache.values = std::move(smoothed);
    return cache.values;
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
    if (state.show_raw_curve || state.show_points) {
        ExpandYBounds(bounds, *y_values, has_y_bounds);
    }
    if (GaussianSmoothingActive(state)) {
        const SpectrumValueVector gaussian_values =
            SmoothedValuesFor(y_values, state, SpectrumSmoothingMethod::Gaussian);
        if (gaussian_values && gaussian_values->size() == y_values->size()) {
            ExpandYBounds(bounds, *gaussian_values, has_y_bounds);
        }
    }
    if (MedianSmoothingActive(state)) {
        const SpectrumValueVector median_values =
            SmoothedValuesFor(y_values, state, SpectrumSmoothingMethod::Median);
        if (median_values && median_values->size() == y_values->size()) {
            ExpandYBounds(bounds, *median_values, has_y_bounds);
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

void RenderSpectralLineOverlays(
    const SpectrumPlotOverlays& overlays,
    bool force_new_layout_epoch,
    bool defer_lane_compaction,
    float bottom_reserved_height,
    const SpectralLineLabelMetrics& metrics,
    SpectralLineLabelLayoutWorkspace& name_layout,
    SpectralLineLabelLayoutWorkspace& wavelength_layout,
    ScientificLabelCache& scientific_label_cache)
{
    const ImPlotRect limits = ImPlot::GetPlotLimits();
    const double y_span = limits.Y.Max - limits.Y.Min;
    if (y_span <= 0.0) {
        return;
    }

    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    if (draw_list == nullptr) {
        return;
    }

    const ImVec2 plot_pos = ImPlot::GetPlotPos();
    const ImVec2 plot_size = ImPlot::GetPlotSize();
    const ImVec2 plot_max(plot_pos.x + plot_size.x, plot_pos.y + plot_size.y);
    ImFont& name_font = overlays.spectral_line_label_font != nullptr
                            ? *overlays.spectral_line_label_font
                            : *ImGui::GetFont();
    const float name_font_size = ImGui::GetFontSize();
    const SpectralLineLabelLayoutContext layout_context{
        .scope_id = overlays.layout_scope_id,
        .x_span = limits.X.Max - limits.X.Min,
        .plot_width = plot_size.x,
        .font_size = ImGui::GetFontSize(),
        .force_new_epoch = force_new_layout_epoch,
        .defer_lane_compaction = defer_lane_compaction,
    };
    UpdateSpectralLineLabelLayoutContext(name_layout, layout_context);
    UpdateSpectralLineLabelLayoutContext(wavelength_layout, layout_context);
    name_layout.inputs.clear();
    wavelength_layout.inputs.clear();
    if (overlays.show_spectral_line_labels) {
        name_layout.inputs.reserve(overlays.spectral_line_count);
        wavelength_layout.inputs.reserve(overlays.spectral_line_count);
    }

    const std::size_t marker_count =
        overlays.spectral_lines != nullptr ? overlays.spectral_line_count : 0;
    std::vector<SpectralLineVisualColors> resolved_colors;
    resolved_colors.reserve(marker_count);
    const SemanticPalette& palette = ActiveSemanticPalette();
    for (std::size_t index = 0; index < marker_count; ++index) {
        resolved_colors.push_back(
            ResolveSpectralLineVisualColors(
                overlays.spectral_lines[index],
                palette));
    }
    ImPlot::PushPlotClipRect();
    for (std::size_t index = 0; index < marker_count; ++index) {
        const SpectralLineMarker* marker =
            overlays.spectral_lines[index].marker;
        if (marker == nullptr || !IsVisibleInPlot(*marker, limits)) {
            continue;
        }

        const SpectralLineVisualColors& colors =
            resolved_colors[index];
        const ImVec4 color = colors.marker_and_label;

        if (marker->kind == SpectralLineMarkerKind::Band) {
            const ImVec2 start_min = ImPlot::PlotToPixels(*marker->start_vacuum_angstrom, limits.Y.Min);
            const ImVec2 end_max = ImPlot::PlotToPixels(*marker->end_vacuum_angstrom, limits.Y.Max);
            const ImVec2 rect_min(std::min(start_min.x, end_max.x), std::min(start_min.y, end_max.y));
            const ImVec2 rect_max(std::max(start_min.x, end_max.x), std::max(start_min.y, end_max.y));
            draw_list->AddRectFilled(
                rect_min,
                rect_max,
                ImGui::GetColorU32(colors.band_fill));
            draw_list->AddRect(
                rect_min,
                rect_max,
                ImGui::GetColorU32(color));
        } else if (marker->vacuum_angstrom) {
            const double x = *marker->vacuum_angstrom;
            const ImVec2 bottom = ImPlot::PlotToPixels(x, limits.Y.Min);
            const ImVec2 top = ImPlot::PlotToPixels(x, limits.Y.Max);
            draw_list->AddLine(bottom, top, ImGui::GetColorU32(color), 1.0f);
        }

        const double label_anchor = SpectralLineMarkerPosition(*marker);
        if (overlays.show_spectral_line_labels &&
            IsSpectralLineLabelAnchorInViewport(label_anchor, limits.X.Min, limits.X.Max)) {
            const float label_anchor_x = ImPlot::PlotToPixels(label_anchor, limits.Y.Min).x;
            const std::string& name = marker->label.empty() ? marker->id : marker->label;
            const std::string& wavelength =
                marker->display_label.empty() ? marker->id : marker->display_label;
            const ScientificLabel& scientific_name =
                scientific_label_cache.Resolve(
                    overlays.layout_scope_id,
                    marker->id,
                    name);
            const ScientificLabelSize name_size =
                MeasureScientificLabel(
                    scientific_name,
                    name_font,
                    name_font_size);
            name_layout.inputs.push_back({
                marker->id,
                label_anchor_x,
                name_size.width,
            });
            wavelength_layout.inputs.push_back({
                marker->id,
                label_anchor_x,
                ImGui::CalcTextSize(wavelength.c_str()).x,
            });
        }
    }

    const std::span<const SpectralLineLabelLayoutResult> name_placements = LayoutSpectralLineLabels(
        name_layout,
        plot_pos.x + metrics.edge_padding,
        plot_max.x - metrics.edge_padding,
        metrics.horizontal_gap);
    const std::span<const SpectralLineLabelLayoutResult> wavelength_placements =
        LayoutSpectralLineLabels(
            wavelength_layout,
            plot_pos.x + metrics.edge_padding,
            plot_max.x - metrics.edge_padding,
            metrics.horizontal_gap);

    if (!name_layout.inputs.empty()) {
        const float bottom_inset = metrics.bottom_gap + bottom_reserved_height;
        const float lane_height =
            std::max(
                ImGui::GetTextLineHeight(),
                ScientificLabelLineHeight(name_font, name_font_size)) +
            metrics.lane_gap;
        const float plot_mid_y = plot_pos.y + plot_size.y * 0.5f;
        const SpectralLineVerticalLayoutContext vertical_context{
            .plot_top = plot_pos.y,
            .plot_bottom = plot_max.y,
            .plot_mid_y = plot_mid_y,
            .top_inset = metrics.top_legend_inset,
            .bottom_inset = bottom_inset,
            .lane_height = lane_height,
        };

        std::size_t label_index = 0;
        for (std::size_t index = 0; index < marker_count; ++index) {
            const SpectralLineMarker* marker =
                overlays.spectral_lines[index].marker;
            if (marker == nullptr || !IsVisibleInPlot(*marker, limits)) {
                continue;
            }
            if (!IsSpectralLineLabelAnchorInViewport(
                    SpectralLineMarkerPosition(*marker),
                    limits.X.Min,
                    limits.X.Max)) {
                continue;
            }

            const std::string& name = marker->label.empty() ? marker->id : marker->label;
            const std::string& wavelength =
                marker->display_label.empty() ? marker->id : marker->display_label;
            const ScientificLabel& scientific_name =
                scientific_label_cache.Resolve(
                    overlays.layout_scope_id,
                    marker->id,
                    name);
            const ScientificLabelSize name_size =
                MeasureScientificLabel(
                    scientific_name,
                    name_font,
                    name_font_size);
            const ImVec2 wavelength_size = ImGui::CalcTextSize(wavelength.c_str());
            const SpectralLineLabelLayoutResult& name_placement = name_placements[label_index];
            const SpectralLineLabelLayoutResult& wavelength_placement =
                wavelength_placements[label_index];
            ++label_index;

            const ImU32 text_color = ImGui::GetColorU32(
                resolved_colors[index].marker_and_label);
            const SpectralLineVerticalLabelPlacement name_vertical =
                PlaceSpectralLineNameLabel(
                    vertical_context,
                    name_size.height,
                    name_placement.lane);
            if (name_vertical.visible) {
                DrawScientificLabel(
                    *draw_list,
                    scientific_name,
                    name_font,
                    name_font_size,
                    ImVec2(name_placement.left, name_vertical.y),
                    text_color);
            }

            const SpectralLineVerticalLabelPlacement wavelength_vertical =
                PlaceSpectralLineWavelengthLabel(
                    vertical_context,
                    wavelength_size.y,
                    wavelength_placement.lane);
            if (wavelength_vertical.visible) {
                draw_list->AddText(
                    ImVec2(wavelength_placement.left, wavelength_vertical.y),
                    text_color,
                    wavelength.c_str());
            }
        }
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

int DecimalPlacesForStep(double step)
{
    const double abs_step = std::abs(step);
    if (!std::isfinite(abs_step) || abs_step <= 0.0 || abs_step >= 1.0) {
        return 0;
    }

    return std::clamp(static_cast<int>(std::ceil(-std::log10(abs_step))), 0, 8);
}

std::string TrimFixedDecimalZeros(std::string value)
{
    const std::size_t decimal = value.find('.');
    if (decimal == std::string::npos) {
        return value;
    }

    while (value.size() > decimal + 1 && value.back() == '0') {
        value.pop_back();
    }
    if (!value.empty() && value.back() == '.') {
        value.pop_back();
    }
    return value == "-0" ? "0" : value;
}

std::string FormatTickValue(double value, double step)
{
    if (!std::isfinite(value)) {
        return {};
    }

    if (std::abs(value) < std::abs(step) * 1.0e-6) {
        value = 0.0;
    }

    char buffer[64] = {};
    std::snprintf(buffer, sizeof(buffer), "%.*f", DecimalPlacesForStep(step), value);
    return TrimFixedDecimalZeros(std::string(buffer));
}

int FormatNativeCompactYTick(double value, char* buffer, int size, void*)
{
    if (buffer == nullptr || size <= 0) {
        return 0;
    }

    if (!std::isfinite(value)) {
        buffer[0] = '\0';
        return 0;
    }

    const double tenths = value * 10.0;
    const double rounded_tenths = std::round(tenths);
    if (std::abs(tenths - rounded_tenths) > 1.0e-6) {
        return std::snprintf(buffer, static_cast<std::size_t>(size), "%4s", "");
    }

    double rounded_value = rounded_tenths / 10.0;
    if (std::abs(rounded_value) < 0.05) {
        rounded_value = 0.0;
    }
    return std::snprintf(buffer, static_cast<std::size_t>(size), "%4.1f", rounded_value);
}

float ClampTextStart(float desired, float minimum, float maximum)
{
    if (maximum < minimum) {
        return minimum;
    }
    return std::clamp(desired, minimum, maximum);
}

void RenderEdgeAxisOverlay(const EdgeAxisMetrics& metrics)
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
    const SemanticPalette& palette =
        ActiveSemanticPalette();
    const ImU32 tick_color =
        ImGui::GetColorU32(palette.plot_grid);
    const ImU32 label_color =
        ImGui::GetColorU32(palette.plot_axis);
    ImPlot::PushPlotClipRect();

    const int x_tick_count = DesiredTickCount(plot_size.x, metrics.x_tick_target_spacing, 6, 14);
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
            ImVec2(pixel_x, plot_max.y - metrics.tick_length),
            tick_color,
            1.0f);

        const std::string label = FormatTickValue(x, x_step);
        const ImVec2 label_size = ImGui::CalcTextSize(label.c_str());
        const float label_x = ClampTextStart(
            pixel_x - label_size.x * 0.5f,
            plot_min.x + metrics.edge_padding,
            plot_max.x - label_size.x - metrics.edge_padding);
        const float label_y = plot_max.y - metrics.tick_length -
                              metrics.tick_label_gap - label_size.y;
        draw_list->AddText(ImVec2(label_x, label_y), label_color, label.c_str());
    }

    const int y_tick_count = DesiredTickCount(plot_size.y, metrics.y_tick_target_spacing, 5, 12);
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
            ImVec2(plot_min.x + metrics.tick_length, pixel_y),
            tick_color,
            1.0f);

        const std::string label = FormatTickValue(y, y_step);
        const ImVec2 label_size = ImGui::CalcTextSize(label.c_str());
        const float label_x = plot_min.x + metrics.tick_length + metrics.tick_label_gap;
        const float label_y = ClampTextStart(
            pixel_y - label_size.y * 0.5f,
            plot_min.y + metrics.edge_padding,
            plot_max.y - label_size.y - metrics.edge_padding);
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

void RenderViewportLockOverlay(
    UiLanguage language,
    SpectrumPlotState& state,
    const ImVec2& button_min,
    float side,
    const ImVec2& return_cursor)
{
    const bool locked =
        state.viewport_range_mode ==
        SpectrumViewportRangeMode::Locked;
    ImGui::SetCursorScreenPos(button_min);
    const bool clicked = ImGui::InvisibleButton(
        "##SpecForgeViewportRangeLock",
        ImVec2(side, side));
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();

    const ImGuiStyle& style = ImGui::GetStyle();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const ImVec2 button_max(button_min.x + side, button_min.y + side);
    if (active || hovered) {
        const ImU32 background = ImGui::GetColorU32(
            active
                ? ImGuiCol_ButtonActive
                : ImGuiCol_ButtonHovered);
        draw_list->AddRectFilled(
            button_min,
            button_max,
            background,
            style.FrameRounding);
    }

    const ImU32 icon_color = locked
        ? ImGui::GetColorU32(
              ActiveSemanticPalette().text)
        : ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const float center_x = (button_min.x + button_max.x) * 0.5f;
    const float body_top = button_min.y + side * 0.49f;
    const float body_half_width = side * 0.19f;
    const float stroke = std::max(1.4f, side * 0.075f);
    draw_list->AddRectFilled(
        ImVec2(center_x - body_half_width, body_top),
        ImVec2(center_x + body_half_width, button_min.y + side * 0.77f),
        icon_color,
        side * 0.055f);

    if (locked) {
        const float shackle_top = button_min.y + side * 0.25f;
        draw_list->PathLineTo(ImVec2(center_x - body_half_width * 0.72f, body_top));
        draw_list->PathLineTo(ImVec2(center_x - body_half_width * 0.72f, shackle_top + side * 0.08f));
        draw_list->PathBezierCubicCurveTo(
            ImVec2(center_x - body_half_width * 0.72f, shackle_top),
            ImVec2(center_x + body_half_width * 0.72f, shackle_top),
            ImVec2(center_x + body_half_width * 0.72f, shackle_top + side * 0.08f));
        draw_list->PathLineTo(ImVec2(center_x + body_half_width * 0.72f, body_top));
    } else {
        const float shackle_center_x = center_x + side * 0.08f;
        const float shackle_top = button_min.y + side * 0.18f;
        draw_list->PathLineTo(ImVec2(shackle_center_x - body_half_width * 0.72f, body_top));
        draw_list->PathLineTo(ImVec2(shackle_center_x - body_half_width * 0.72f, shackle_top + side * 0.08f));
        draw_list->PathBezierCubicCurveTo(
            ImVec2(shackle_center_x - body_half_width * 0.72f, shackle_top),
            ImVec2(shackle_center_x + body_half_width * 0.72f, shackle_top),
            ImVec2(shackle_center_x + body_half_width * 0.72f, shackle_top + side * 0.08f));
    }
    draw_list->PathStroke(icon_color, ImDrawFlags_None, stroke);

    if (hovered) {
        const std::string_view tooltip = UiText(
            language,
            locked
                ? UiTextId::ViewportLockedTooltip
                : UiTextId::ViewportUnlockedTooltip);
        ImGui::SetTooltip(
            "%.*s",
            static_cast<int>(tooltip.size()),
            tooltip.data());
    }
    if (clicked) {
        state.viewport_range_mode = locked
            ? SpectrumViewportRangeMode::Automatic
            : SpectrumViewportRangeMode::Locked;
    }
    ImGui::SetCursorScreenPos(ImVec2(
        return_cursor.x,
        return_cursor.y - style.ItemSpacing.y));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
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

    return available_size;
}

std::uintptr_t CurrentNativeWindow()
{
    const ImGuiViewport* viewport = ImGui::GetWindowViewport();
    if (viewport == nullptr) {
        return 0;
    }

    void* handle = viewport->PlatformHandleRaw;
    if (handle == nullptr) {
        handle = viewport->PlatformHandle;
    }
    return reinterpret_cast<std::uintptr_t>(handle);
}

PlotViewLimits StoredViewLimits(const SpectrumPlotState& state)
{
    return {state.last_x_min, state.last_x_max, state.last_y_min, state.last_y_max};
}

void SetNextViewLimits(const PlotViewLimits& limits)
{
    ImPlot::SetNextAxesLimits(
        limits.x_min,
        limits.x_max,
        limits.y_min,
        limits.y_max,
        ImPlotCond_Always);
}

PlotTouchpadTarget MakeTouchpadTarget(
    std::uintptr_t native_window,
    const ImVec2& widget_pos,
    const ImVec2& widget_size,
    bool edge_axis_overlay,
    float edge_axis_band,
    const PlotPixelRect& input_exclusion_rect)
{
    const PlotPixelRect widget_rect{
        widget_pos.x,
        widget_pos.y,
        widget_pos.x + widget_size.x,
        widget_pos.y + widget_size.y};

    PlotTouchpadTarget target;
    target.native_window = native_window;
    target.input_exclusion_rect = input_exclusion_rect;
    if (edge_axis_overlay) {
        target.plot_rect = widget_rect;
        target.x_axis_rect = {
            widget_rect.left,
            std::max(widget_rect.top, widget_rect.bottom - edge_axis_band),
            widget_rect.right,
            widget_rect.bottom};
        target.y_axis_rect = {
            widget_rect.left,
            widget_rect.top,
            std::min(widget_rect.right, widget_rect.left + edge_axis_band),
            widget_rect.bottom};
        return target;
    }

    const ImVec2 plot_pos = ImPlot::GetPlotPos();
    const ImVec2 plot_size = ImPlot::GetPlotSize();
    target.plot_rect = {
        plot_pos.x,
        plot_pos.y,
        plot_pos.x + plot_size.x,
        plot_pos.y + plot_size.y};
    target.x_axis_rect = {
        target.plot_rect.left,
        target.plot_rect.bottom,
        target.plot_rect.right,
        widget_rect.bottom};
    target.y_axis_rect = {
        widget_rect.left,
        target.plot_rect.top,
        target.plot_rect.left,
        target.plot_rect.bottom};
    return target;
}

}  // namespace

bool IsPlotPanDragActive(
    bool was_active,
    bool plot_hovered,
    bool left_button_down,
    bool left_button_dragging) noexcept
{
    return left_button_down && (was_active || (plot_hovered && left_button_dragging));
}

SpectrumPlotRenderResult RenderSpectrumPlot(
    const SpectrumSnapshotHandle& snapshot,
    SpectrumPlotState& state,
    UiLanguage language,
    const SpectrumPlotProfileContext& profile,
    const SpectrumPlotStyle& style,
    const SpectrumPlotOverlays& overlays,
    const SpectrumPlotDisplayOptions& display,
    PlotTouchpadGestureSource* touchpad_gestures)
{
    SpectrumPlotRenderResult result;
    if (!CanPlotSnapshot(snapshot)) {
        state.pan_drag_active = false;
        if (touchpad_gestures != nullptr) {
            touchpad_gestures->ClearTarget();
        }
        const std::string_view no_spectrum = UiText(
            language,
            UiTextId::NoPlottableSpectrum);
        ImGui::TextDisabled(
            "%.*s",
            static_cast<int>(no_spectrum.size()),
            no_spectrum.data());
        return result;
    }

    const std::uintptr_t native_window = CurrentNativeWindow();
    const SpectrumPlotMetrics plot_metrics = MakeSpectrumPlotMetrics(
        ImGui::GetFontSize(),
        ImGui::GetTextLineHeight());
    const float edge_axis_band = plot_metrics.edge_axis.interaction_band;
    const ImVec2 plot_widget_pos = ImGui::GetCursorScreenPos();
    const ImVec2 plot_widget_size = ImGui::GetContentRegionAvail();
    const ImVec2 plot_size = PlotSizeForDisplay(display, plot_widget_size);
    const float viewport_lock_side = std::max(ImGui::GetFrameHeight(), 22.0f);
    const float viewport_lock_padding = 3.0f;
    const bool viewport_lock_fits =
        plot_widget_size.x >= viewport_lock_side + viewport_lock_padding * 2.0f &&
        plot_widget_size.y >= viewport_lock_side + viewport_lock_padding * 2.0f;
    const ImVec2 viewport_lock_min(
        plot_widget_pos.x + viewport_lock_padding,
        plot_widget_pos.y + plot_widget_size.y - viewport_lock_side - viewport_lock_padding);
    const ImVec2 viewport_lock_max(
        viewport_lock_min.x + viewport_lock_side,
        viewport_lock_min.y + viewport_lock_side);
    const PlotPixelRect viewport_lock_rect = viewport_lock_fits
        ? PlotPixelRect{
              viewport_lock_min.x,
              viewport_lock_min.y,
              viewport_lock_max.x,
              viewport_lock_max.y}
        : PlotPixelRect{};
    const bool viewport_lock_hovered =
        viewport_lock_fits &&
        ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
        ContainsPoint(viewport_lock_min, viewport_lock_max, ImGui::GetIO().MousePos);

    PlotTouchpadGestureBatch touchpad_batch;
    if (touchpad_gestures != nullptr && native_window != 0) {
        touchpad_batch = touchpad_gestures->Poll(native_window);
    }
    const auto first_excluded_touchpad_delta = std::remove_if(
        touchpad_batch.deltas.begin(),
        touchpad_batch.deltas.end(),
        [&viewport_lock_rect](const PlotTouchpadGestureDelta& gesture) {
            return viewport_lock_rect.Contains(gesture.anchor_x, gesture.anchor_y);
        });
    const bool touchpad_input_excluded =
        first_excluded_touchpad_delta != touchpad_batch.deltas.end();
    touchpad_batch.deltas.erase(
        first_excluded_touchpad_delta,
        touchpad_batch.deltas.end());
    if (touchpad_input_excluded && touchpad_batch.deltas.empty()) {
        touchpad_batch.active = false;
    }

    const bool fit_requested = state.fit_next_frame;
    const bool stored_limits_requested =
        state.sync_last_limits_next_frame;
    bool stored_limits_reused = false;
    PlotViewLimits requested_limits;
    bool has_requested_limits = false;
    if (state.fit_next_frame) {
        const Bounds bounds = ComputeBounds(*snapshot, state);
        requested_limits = {bounds.x_min, bounds.x_max, bounds.y_min, bounds.y_max};
        has_requested_limits = true;
        state.fit_next_frame = false;
        state.sync_last_limits_next_frame = false;
    } else if (state.sync_last_limits_next_frame) {
        state.sync_last_limits_next_frame = false;
        if (LastLimitsAreUsable(state)) {
            requested_limits = StoredViewLimits(state);
            has_requested_limits = true;
            stored_limits_reused = true;
        }
    } else if (!touchpad_batch.deltas.empty() && LastLimitsAreUsable(state)) {
        requested_limits = StoredViewLimits(state);
        has_requested_limits = true;
    }

    if (has_requested_limits) {
        (void)ApplyPlotTouchpadGestures(requested_limits, touchpad_batch);
        SetNextViewLimits(requested_limits);
    }

    const bool edge_axis_wheel_zoomed =
        display.edge_axis_overlay && !viewport_lock_hovered &&
        !fit_requested && !touchpad_batch.active &&
        touchpad_batch.deltas.empty() &&
        ApplyEdgeAxisWheelZoom(state, plot_widget_pos, plot_widget_size, edge_axis_band);

    const bool transparent_native_axes = display.native_transparent_axes && !display.edge_axis_overlay;
    const ScopedTransparentPlotStyle transparent_plot_style(display.edge_axis_overlay || transparent_native_axes);
    ImPlotFlags plot_flags = ImPlotFlags_Crosshairs;
    ImPlotAxisFlags x_axis_flags = ImPlotAxisFlags_None;
    ImPlotAxisFlags y_axis_flags = ImPlotAxisFlags_None;
    if (display.edge_axis_overlay || transparent_native_axes) {
        plot_flags |= ImPlotFlags_NoFrame | ImPlotFlags_NoTitle | ImPlotFlags_NoLegend;
    }
    if (transparent_native_axes) {
        x_axis_flags |= ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoTickMarks |
                        ImPlotAxisFlags_NoMenus | ImPlotAxisFlags_NoSideSwitch |
                        ImPlotAxisFlags_NoHighlight;
        y_axis_flags |= x_axis_flags;
    }
    if (display.edge_axis_overlay) {
        x_axis_flags |= ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoTickMarks |
                        ImPlotAxisFlags_NoTickLabels | ImPlotAxisFlags_NoMenus |
                        ImPlotAxisFlags_NoSideSwitch | ImPlotAxisFlags_NoHighlight;
        y_axis_flags |= x_axis_flags;
    }
    if (edge_axis_wheel_zoomed || touchpad_batch.active || !touchpad_batch.deltas.empty()) {
        plot_flags |= ImPlotFlags_NoInputs;
    }
    if (viewport_lock_hovered) {
        plot_flags |= ImPlotFlags_NoInputs;
    }

    bool plot_frame_presented = false;
    const std::string plot_label = StableUiLabel(
        language,
        UiTextId::Spectrum,
        "main_spectrum");
    if (viewport_lock_fits) {
        ImGui::SetNextItemAllowOverlap();
    }
    if (ImPlot::BeginPlot(
            plot_label.c_str(),
            plot_size,
            plot_flags)) {
        plot_frame_presented = true;
        const char* x_label = snapshot->axis.x_label.empty() ? nullptr : snapshot->axis.x_label.c_str();
        const char* y_label = snapshot->axis.y_label.empty() ? nullptr : snapshot->axis.y_label.c_str();
        ImPlot::SetupAxis(ImAxis_X1, x_label, x_axis_flags);
        ImPlot::SetupAxis(ImAxis_Y1, y_label, y_axis_flags);
        ImPlot::SetupLegend(
            ImPlotLocation_NorthWest,
            ImPlotLegendFlags_NoButtons |
                ImPlotLegendFlags_NoMenus);
        if (transparent_native_axes) {
            ImPlot::SetupAxisFormat(ImAxis_Y1, FormatNativeCompactYTick);
        }

        const SpectrumValueVector& x_values = snapshot->current_spectrum.x_values;
        const SpectrumValueVector& y_values = snapshot->current_spectrum.y_values;
        const std::string& name = snapshot->current_spectrum.name;

        // Reserve the built-in curves in a fixed order even when a curve is
        // hidden. Visibility changes must not reassign another curve's Auto
        // palette slot.
        const SemanticPalette& palette =
            ActiveSemanticPalette();
        const ImVec4 raw_color =
            ResolveSpectrumSeriesColor(
                style.colors,
                SpectrumPlotSeries::RawSpectrum,
                palette,
                state.series_color_assignments);
        const ImVec4 gaussian_color =
            ResolveSpectrumSeriesColor(
                style.colors,
                SpectrumPlotSeries::GaussianSmoothing,
                palette,
                state.series_color_assignments);
        const ImVec4 median_color =
            ResolveSpectrumSeriesColor(
                style.colors,
                SpectrumPlotSeries::MedianSmoothing,
                palette,
                state.series_color_assignments);

        ImPlotSpec base_spec;
        base_spec.LineColor = raw_color;
        base_spec.LineWeight = style.line_weight;
        base_spec.MarkerLineColor = base_spec.LineColor;
        base_spec.MarkerFillColor = base_spec.LineColor;

        const bool smoothing_active = AnySmoothingActive(state);
        if (state.show_raw_curve || state.show_points) {
            std::string raw_spectrum_label;
            const char* raw_series_label = name.c_str();
            if (smoothing_active) {
                raw_spectrum_label = StableUiLabel(
                    language,
                    UiTextId::RawSpectrum,
                    "SpecForgeRawSpectrum");
                raw_series_label = raw_spectrum_label.c_str();
            } else if (name.empty()) {
                raw_spectrum_label = StableUiLabel(
                    language,
                    UiTextId::CurrentSpectrum,
                    "SpecForgeCurrentSpectrum");
                raw_series_label = raw_spectrum_label.c_str();
            }

            ImPlotSpec raw_spec = base_spec;
            if (smoothing_active) {
                raw_spec.LineColor =
                    ApplyRawSpectrumSmoothingEmphasis(
                        style.colors.raw_spectrum,
                        raw_spec.LineColor);
                raw_spec.LineWeight = std::max(1.0f, style.line_weight * 0.80f);
            }
            if (!state.show_raw_curve) {
                raw_spec.LineWeight = 0.0f;
            }
            if (state.show_points) {
                raw_spec.Marker = ImPlotMarker_Circle;
                raw_spec.MarkerSize = 2.0f;
            }
            ImPlot::PlotLine(
                raw_series_label,
                x_values->data(),
                y_values->data(),
                static_cast<int>(x_values->size()),
                raw_spec);
        }

        const auto plot_smoothed_curve =
            [&](SpectrumSmoothingMethod method,
                const char* stable_id,
                const ImVec4& color) {
                const SpectrumValueVector smoothed_values =
                    SmoothedValuesFor(y_values, state, method);
                if (!smoothed_values || smoothed_values->size() != x_values->size()) {
                    return;
                }

                const std::string smoothed_spectrum_label =
                    StableUiLabel(
                        language,
                        SmoothingTextId(method),
                        stable_id);
                ImPlotSpec smoothed_spec = base_spec;
                smoothed_spec.LineColor = color;
                smoothed_spec.LineWeight = std::max(1.0f, style.line_weight * 1.08f);
                smoothed_spec.MarkerLineColor = smoothed_spec.LineColor;
                smoothed_spec.MarkerFillColor = smoothed_spec.LineColor;
                ImPlot::PlotLine(
                    smoothed_spectrum_label.c_str(),
                    x_values->data(),
                    smoothed_values->data(),
                    static_cast<int>(x_values->size()),
                    smoothed_spec);
            };

        if (GaussianSmoothingActive(state)) {
            plot_smoothed_curve(
                SpectrumSmoothingMethod::Gaussian,
                "SpecForgeGaussianSmoothedSpectrum",
                gaussian_color);
        }
        if (MedianSmoothingActive(state)) {
            plot_smoothed_curve(
                SpectrumSmoothingMethod::Median,
                "SpecForgeMedianSmoothedSpectrum",
                median_color);
        }

        const bool hovered = ImPlot::IsPlotHovered();
        const bool left_down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
        const bool left_dragging = ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f);
        const bool pan_drag_active =
            IsPlotPanDragActive(state.pan_drag_active, hovered, left_down, left_dragging);
        const bool defer_spectral_line_lane_compaction =
            pan_drag_active || touchpad_batch.active;

        if (snapshot->capabilities.can_show_spectral_lines) {
            const float bottom_reserved_height =
                display.edge_axis_overlay
                    ? plot_metrics.edge_axis.tick_length + plot_metrics.edge_axis.tick_label_gap +
                          ImGui::GetTextLineHeight()
                    : 0.0f;
            RenderSpectralLineOverlays(
                overlays,
                fit_requested,
                defer_spectral_line_lane_compaction,
                bottom_reserved_height,
                plot_metrics.spectral_line_labels,
                state.spectral_line_name_layout,
                state.spectral_line_wavelength_layout,
                state.scientific_label_cache);
        }
        if (display.edge_axis_overlay) {
            RenderEdgeAxisOverlay(plot_metrics.edge_axis);
        }

        if (touchpad_gestures != nullptr && native_window != 0) {
            touchpad_gestures->SetTarget(MakeTouchpadTarget(
                native_window,
                plot_widget_pos,
                plot_widget_size,
                display.edge_axis_overlay,
                edge_axis_band,
                viewport_lock_rect));
        }

        const ImPlotRect limits = ImPlot::GetPlotLimits();
        StoreLastLimits(limits, state);
        result.visible_limits = PlotViewLimits{
            limits.X.Min,
            limits.X.Max,
            limits.Y.Min,
            limits.Y.Max};

        if (ProfileSink* sink = ActiveProfileSink(profile)) {
            for (const PlotTouchpadGestureDelta& gesture : touchpad_batch.deltas) {
                sink->WriteEvent("touchpad.gesture", {
                                                          ProfileSink::Field::Number(
                                                              "frame",
                                                              std::to_string(profile.frame_index)),
                                                          ProfileSink::Field::String(
                                                              "kind",
                                                              gesture.kind == PlotTouchpadGestureKind::Pan
                                                                  ? "pan"
                                                                  : "zoom"),
                                                          ProfileSink::Field::Number(
                                                              "pan_x",
                                                              std::to_string(gesture.pan_x)),
                                                          ProfileSink::Field::Number(
                                                              "pan_y",
                                                              std::to_string(gesture.pan_y)),
                                                          ProfileSink::Field::Number(
                                                              "zoom_factor",
                                                              std::to_string(gesture.zoom_factor)),
                                                          ProfileSink::Field::Number(
                                                              "input_steady_ns",
                                                              std::to_string(gesture.input_steady_ns)),
                                                          ProfileSink::Field::Bool(
                                                              "inertia",
                                                              gesture.inertia),
                                                      });
            }
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
        }
        state.pan_drag_active = pan_drag_active;
        result.fit_applied = fit_requested;
        result.stored_limits_reused =
            stored_limits_requested &&
            stored_limits_reused;
        result.pan_active = pan_drag_active;

        ImPlot::EndPlot();
        if (viewport_lock_fits) {
            const ImVec2 return_cursor = ImGui::GetCursorScreenPos();
            RenderViewportLockOverlay(
                language,
                state,
                viewport_lock_min,
                viewport_lock_side,
                return_cursor);
        }
    } else {
        state.pan_drag_active = false;
        if (touchpad_gestures != nullptr) {
            touchpad_gestures->ClearTarget();
        }
    }
    result.plot_submitted = plot_frame_presented;
    return result;
}

}  // namespace specforge
