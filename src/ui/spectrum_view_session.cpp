#include "ui/spectrum_view_session.h"

#include "plot/spectrum_plot_renderer.h"
#include "ui/source_collection_activation_transaction.h"
#include "ui/source_collection_session_types.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

namespace spectiary {
namespace {

bool LimitsAreUsable(const PlotViewLimits& limits)
{
    return std::isfinite(limits.x_min) &&
           std::isfinite(limits.x_max) &&
           std::isfinite(limits.y_min) &&
           std::isfinite(limits.y_max) &&
           limits.x_min < limits.x_max &&
           limits.y_min < limits.y_max;
}

}  // namespace

SpectrumViewportTransition ResolveViewportTransition(
    SpectrumViewportRangeMode range_mode,
    SourceCollectionSnapshotChangeReason change_reason) noexcept
{
    if (range_mode == SpectrumViewportRangeMode::Locked &&
        change_reason ==
            SourceCollectionSnapshotChangeReason::SampleChangedWithinCollection) {
        return {
            .range_mode = SpectrumViewportRangeMode::Locked,
            .range_action = SpectrumViewportRangeAction::Preserve,
        };
    }
    if (range_mode == SpectrumViewportRangeMode::Locked &&
        change_reason ==
            SourceCollectionSnapshotChangeReason::SnapshotReloadedWithinCollection) {
        return {
            .range_mode = SpectrumViewportRangeMode::Locked,
            .range_action = SpectrumViewportRangeAction::Fit,
        };
    }
    return {
        .range_mode = SpectrumViewportRangeMode::Automatic,
        .range_action = SpectrumViewportRangeAction::Fit,
    };
}

struct SpectrumViewSession::State {
    SpectrumPlotState plot;
    SpectrumPlotStyle style;
    SpectrumViewRenderFeedback last_render;
    std::uint64_t viewport_mutation_revision = 0;
};

SpectrumViewSessionCommand SpectrumViewSessionCommand::ApplySnapshotChange(
    SourceCollectionSnapshotChangeReason reason)
{
    SpectrumViewSessionCommand command;
    command.kind = SpectrumViewSessionCommandKind::ApplySnapshotChange;
    command.snapshot_change_reason = reason;
    return command;
}

SpectrumViewSessionCommand SpectrumViewSessionCommand::RequestFitView()
{
    SpectrumViewSessionCommand command;
    command.kind = SpectrumViewSessionCommandKind::RequestFitView;
    return command;
}

SpectrumViewSessionCommand SpectrumViewSessionCommand::SetShowRawCurve(bool enabled)
{
    SpectrumViewSessionCommand command;
    command.kind = SpectrumViewSessionCommandKind::SetShowRawCurve;
    command.enabled = enabled;
    return command;
}

SpectrumViewSessionCommand SpectrumViewSessionCommand::SetShowPoints(bool enabled)
{
    SpectrumViewSessionCommand command;
    command.kind = SpectrumViewSessionCommandKind::SetShowPoints;
    command.enabled = enabled;
    return command;
}

SpectrumViewSessionCommand SpectrumViewSessionCommand::SetShowGaussianSmoothed(bool enabled)
{
    SpectrumViewSessionCommand command;
    command.kind = SpectrumViewSessionCommandKind::SetShowGaussianSmoothed;
    command.enabled = enabled;
    return command;
}

SpectrumViewSessionCommand SpectrumViewSessionCommand::SetShowMedianSmoothed(bool enabled)
{
    SpectrumViewSessionCommand command;
    command.kind = SpectrumViewSessionCommandKind::SetShowMedianSmoothed;
    command.enabled = enabled;
    return command;
}

SpectrumViewSessionCommand SpectrumViewSessionCommand::ResetSmoothing()
{
    SpectrumViewSessionCommand command;
    command.kind = SpectrumViewSessionCommandKind::ResetSmoothing;
    return command;
}

SpectrumViewSessionCommand SpectrumViewSessionCommand::SetGaussianSigma(double sigma)
{
    SpectrumViewSessionCommand command;
    command.kind = SpectrumViewSessionCommandKind::SetGaussianSigma;
    command.gaussian_sigma = sigma;
    return command;
}

SpectrumViewSessionCommand SpectrumViewSessionCommand::SetMedianKernelSize(int kernel_size)
{
    SpectrumViewSessionCommand command;
    command.kind = SpectrumViewSessionCommandKind::SetMedianKernelSize;
    command.median_kernel_size = kernel_size;
    return command;
}

SpectrumViewSessionCommand SpectrumViewSessionCommand::SetPlotColors(
    SpectrumPlotColors colors)
{
    SpectrumViewSessionCommand command;
    command.kind = SpectrumViewSessionCommandKind::SetPlotColors;
    command.plot_colors = std::move(colors);
    return command;
}

SpectrumViewSessionCommand SpectrumViewSessionCommand::SetPlotSeriesColor(
    SpectrumPlotSeries series,
    PlotSeriesColor color)
{
    SpectrumViewSessionCommand command;
    command.kind =
        SpectrumViewSessionCommandKind::SetPlotSeriesColor;
    command.plot_series = series;
    command.plot_series_color = std::move(color);
    return command;
}

SpectrumViewSessionCommand SpectrumViewSessionCommand::SetViewportRangeMode(
    SpectrumViewportRangeMode mode)
{
    SpectrumViewSessionCommand command;
    command.kind = SpectrumViewSessionCommandKind::SetViewportRangeMode;
    command.viewport_range_mode = mode;
    return command;
}

SpectrumViewSessionCommand SpectrumViewSessionCommand::SyncPlotLimitsOnNextRender()
{
    SpectrumViewSessionCommand command;
    command.kind = SpectrumViewSessionCommandKind::SyncPlotLimitsOnNextRender;
    return command;
}

SpectrumViewSession::SpectrumViewSession()
    : state_(std::make_unique<State>())
{
}

SpectrumViewSession::~SpectrumViewSession() = default;

SpectrumViewSession::SpectrumViewSession(SpectrumViewSession&&) noexcept = default;

SpectrumViewSession& SpectrumViewSession::operator=(SpectrumViewSession&&) noexcept = default;

void SpectrumViewSession::Submit(SpectrumViewSessionCommand command)
{
    switch (command.kind) {
    case SpectrumViewSessionCommandKind::ApplySnapshotChange:
        ApplySnapshotChange(command.snapshot_change_reason);
        break;
    case SpectrumViewSessionCommandKind::RequestFitView:
        state_->plot.fit_next_frame = true;
        ++state_->viewport_mutation_revision;
        break;
    case SpectrumViewSessionCommandKind::SetShowRawCurve:
        state_->plot.show_raw_curve = command.enabled;
        break;
    case SpectrumViewSessionCommandKind::SetShowPoints:
        state_->plot.show_points = command.enabled;
        break;
    case SpectrumViewSessionCommandKind::SetShowGaussianSmoothed:
        state_->plot.show_gaussian_smoothed = command.enabled;
        break;
    case SpectrumViewSessionCommandKind::SetShowMedianSmoothed:
        state_->plot.show_median_smoothed = command.enabled;
        break;
    case SpectrumViewSessionCommandKind::ResetSmoothing:
        ResetSmoothing();
        break;
    case SpectrumViewSessionCommandKind::SetGaussianSigma: {
        const double sigma = std::max(0.01, command.gaussian_sigma);
        if (state_->plot.smoothing_parameters.gaussian_sigma != sigma) {
            state_->plot.smoothing_parameters.gaussian_sigma = sigma;
            ClearGaussianSmoothingCache();
        }
        break;
    }
    case SpectrumViewSessionCommandKind::SetMedianKernelSize: {
        const int kernel_size = NormalizeMedianKernelSize(command.median_kernel_size);
        if (state_->plot.smoothing_parameters.median_kernel_size != kernel_size) {
            state_->plot.smoothing_parameters.median_kernel_size = kernel_size;
            ClearMedianSmoothingCache();
        }
        break;
    }
    case SpectrumViewSessionCommandKind::SetPlotColors:
        state_->style.colors =
            std::move(command.plot_colors);
        break;
    case SpectrumViewSessionCommandKind::SetPlotSeriesColor:
        SpectrumSeriesColor(
            state_->style.colors,
            command.plot_series) =
            std::move(command.plot_series_color);
        break;
    case SpectrumViewSessionCommandKind::SetViewportRangeMode:
        if (state_->plot.viewport_range_mode !=
            command.viewport_range_mode) {
            state_->plot.viewport_range_mode =
                command.viewport_range_mode;
            ++state_->viewport_mutation_revision;
        }
        break;
    case SpectrumViewSessionCommandKind::SyncPlotLimitsOnNextRender:
        if (state_->plot.has_last_limits) {
            state_->plot.sync_last_limits_next_frame = true;
        }
        break;
    }
}

SpectrumViewSessionView SpectrumViewSession::View() const
{
    SpectrumViewSessionView view;
    view.show_raw_curve = state_->plot.show_raw_curve;
    view.show_points = state_->plot.show_points;
    view.show_gaussian_smoothed = state_->plot.show_gaussian_smoothed;
    view.show_median_smoothed = state_->plot.show_median_smoothed;
    view.smoothing_parameters = state_->plot.smoothing_parameters;
    view.plot_colors = state_->style.colors;
    view.viewport_range_mode = state_->plot.viewport_range_mode;
    return view;
}

int SpectrumViewSession::EffectiveMedianKernelSize(std::size_t point_count) const
{
    return spectiary::EffectiveMedianKernelSize(
        state_->plot.smoothing_parameters.median_kernel_size,
        point_count);
}

SpectrumViewRenderFeedback SpectrumViewSession::Render(
    const SpectrumSnapshotHandle& snapshot,
    UiLanguage language,
    const SpectrumPlotProfileContext& profile,
    const SpectrumPlotOverlays& overlays,
    const SpectrumPlotDisplayOptions& display,
    PlotTouchpadGestureSource* touchpad_gestures)
{
    const SpectrumViewportRangeMode previous_range_mode =
        state_->plot.viewport_range_mode;
    const bool previously_had_limits =
        state_->plot.has_last_limits;
    const PlotViewLimits previous_limits{
        state_->plot.last_x_min,
        state_->plot.last_x_max,
        state_->plot.last_y_min,
        state_->plot.last_y_max,
    };
    const SpectrumPlotRenderResult result = RenderSpectrumPlot(
        snapshot,
        state_->plot,
        language,
        profile,
        state_->style,
        overlays,
        display,
        touchpad_gestures);
    const bool range_mode_changed =
        state_->plot.viewport_range_mode !=
        previous_range_mode;
    const bool limits_changed =
        state_->plot.has_last_limits !=
            previously_had_limits ||
        (state_->plot.has_last_limits &&
         previously_had_limits &&
         (state_->plot.last_x_min != previous_limits.x_min ||
          state_->plot.last_x_max != previous_limits.x_max ||
          state_->plot.last_y_min != previous_limits.y_min ||
          state_->plot.last_y_max != previous_limits.y_max));
    if (range_mode_changed ||
        (limits_changed && !result.fit_applied &&
         !result.stored_limits_reused)) {
        ++state_->viewport_mutation_revision;
    }
    state_->last_render = {
        .plot_submitted = result.plot_submitted,
        .fit_applied = result.fit_applied,
        .stored_limits_reused = result.stored_limits_reused,
        .pan_active = result.pan_active,
        .visible_limits = result.visible_limits,
    };
    return state_->last_render;
}

bool SpectrumViewSession::PlotPanActive() const
{
    return state_->last_render.pan_active;
}

std::uint64_t
SpectrumViewSession::ViewportMutationRevision() const noexcept
{
    return state_->viewport_mutation_revision;
}

std::vector<SpectrumValueVector> SpectrumViewSession::RetainHeavySnapshotResources() const
{
    std::vector<SpectrumValueVector> resources;
    resources.reserve(3);
    const auto retain_unique = [&resources](const SpectrumValueVector& resource) {
        if (resource && std::find(resources.begin(), resources.end(), resource) == resources.end()) {
            resources.push_back(resource);
        }
    };
    retain_unique(state_->plot.gaussian_smoothing_cache.source);
    retain_unique(state_->plot.gaussian_smoothing_cache.values);
    retain_unique(state_->plot.median_smoothing_cache.source);
    retain_unique(state_->plot.median_smoothing_cache.values);
    return resources;
}

ImVec4 SpectrumViewSession::ResolveSeriesColor(
    SpectrumPlotSeries series,
    const SemanticPalette& theme_palette)
{
    return ResolveSpectrumSeriesColor(
        state_->style.colors,
        series,
        theme_palette,
        state_->plot.series_color_assignments);
}

std::optional<PlotViewLimits>
SpectrumViewSession::LockedViewportLimits() const
{
    if (state_->plot.viewport_range_mode !=
            SpectrumViewportRangeMode::Locked ||
        !state_->plot.has_last_limits) {
        return std::nullopt;
    }
    const PlotViewLimits limits{
        .x_min = state_->plot.last_x_min,
        .x_max = state_->plot.last_x_max,
        .y_min = state_->plot.last_y_min,
        .y_max = state_->plot.last_y_max,
    };
    return LimitsAreUsable(limits)
        ? std::optional<PlotViewLimits>{limits}
        : std::nullopt;
}

bool SpectrumViewSession::RestoreLockedViewport(
    const PlotViewLimits& limits)
{
    if (!LimitsAreUsable(limits)) {
        return false;
    }
    state_->plot.viewport_range_mode =
        SpectrumViewportRangeMode::Locked;
    state_->plot.fit_next_frame = false;
    state_->plot.has_last_limits = true;
    state_->plot.last_x_min = limits.x_min;
    state_->plot.last_x_max = limits.x_max;
    state_->plot.last_y_min = limits.y_min;
    state_->plot.last_y_max = limits.y_max;
    state_->plot.sync_last_limits_next_frame = true;
    state_->last_render = {};
    return true;
}

void SpectrumViewSession::ApplySnapshotChange(
    SourceCollectionSnapshotChangeReason reason)
{
    const SpectrumViewportTransition viewport_transition =
        ResolveViewportTransition(
            state_->plot.viewport_range_mode,
            reason);
    const bool preserve_last_limits =
        viewport_transition.range_action ==
            SpectrumViewportRangeAction::Preserve &&
        state_->plot.has_last_limits;
    const PlotViewLimits last_limits{
        state_->plot.last_x_min,
        state_->plot.last_x_max,
        state_->plot.last_y_min,
        state_->plot.last_y_max};
    const bool show_raw_curve = state_->plot.show_raw_curve;
    const bool show_points = state_->plot.show_points;
    const bool show_gaussian_smoothed = state_->plot.show_gaussian_smoothed;
    const bool show_median_smoothed = state_->plot.show_median_smoothed;
    const SpectrumSmoothingParameters smoothing_parameters =
        state_->plot.smoothing_parameters;

    state_->plot = SpectrumPlotState{};
    state_->plot.viewport_range_mode =
        viewport_transition.range_mode;
    state_->plot.show_raw_curve = show_raw_curve;
    state_->plot.show_points = show_points;
    state_->plot.show_gaussian_smoothed = show_gaussian_smoothed;
    state_->plot.show_median_smoothed = show_median_smoothed;
    state_->plot.smoothing_parameters = smoothing_parameters;
    if (preserve_last_limits) {
        state_->plot.fit_next_frame = false;
        state_->plot.has_last_limits = true;
        state_->plot.last_x_min = last_limits.x_min;
        state_->plot.last_x_max = last_limits.x_max;
        state_->plot.last_y_min = last_limits.y_min;
        state_->plot.last_y_max = last_limits.y_max;
        state_->plot.sync_last_limits_next_frame = true;
    }
    state_->last_render = {};
}

void SpectrumViewSession::ResetSmoothing()
{
    state_->plot.show_raw_curve = true;
    state_->plot.show_gaussian_smoothed = false;
    state_->plot.show_median_smoothed = false;
    state_->plot.smoothing_parameters = SpectrumSmoothingParameters{};
    ClearSmoothingCaches();
}

void SpectrumViewSession::ClearGaussianSmoothingCache()
{
    state_->plot.gaussian_smoothing_cache = {};
}

void SpectrumViewSession::ClearMedianSmoothingCache()
{
    state_->plot.median_smoothing_cache = {};
}

void SpectrumViewSession::ClearSmoothingCaches()
{
    ClearGaussianSmoothingCache();
    ClearMedianSmoothingCache();
}

void BindSourceCollectionActivationPresentationLifecycle(
    SourceCollectionActivationTransaction& activation,
    SpectrumViewSession& presentation,
    std::function<void(std::optional<std::string>)>
        deferred_restore_finished)
{
    activation.BindPresentationLifecycle(
        [&presentation]() {
            std::vector<BackgroundRetirementHandle>
                resources;
            for (SpectrumValueVector& resource :
                 presentation.
                     RetainHeavySnapshotResources()) {
                resources.push_back(
                    std::move(resource));
            }
            return resources;
        },
        [&presentation](
            SourceCollectionSnapshotChangeReason reason) {
            presentation.Submit(
                SpectrumViewSessionCommand::
                    ApplySnapshotChange(reason));
        },
        std::move(deferred_restore_finished));
}

}  // namespace spectiary
