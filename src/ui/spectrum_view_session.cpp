#include "ui/spectrum_view_session.h"

#include "plot/spectrum_plot_renderer.h"
#include "ui/source_collection_activation_transaction.h"
#include "ui/source_collection_session_types.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

namespace specforge {
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

SpectrumViewSessionCommand SpectrumViewSessionCommand::SetShowPoints(bool enabled)
{
    SpectrumViewSessionCommand command;
    command.kind = SpectrumViewSessionCommandKind::SetShowPoints;
    command.enabled = enabled;
    return command;
}

SpectrumViewSessionCommand SpectrumViewSessionCommand::SetShowSmoothed(bool enabled)
{
    SpectrumViewSessionCommand command;
    command.kind = SpectrumViewSessionCommandKind::SetShowSmoothed;
    command.enabled = enabled;
    return command;
}

SpectrumViewSessionCommand SpectrumViewSessionCommand::SetShowRawWhenSmoothed(bool enabled)
{
    SpectrumViewSessionCommand command;
    command.kind = SpectrumViewSessionCommandKind::SetShowRawWhenSmoothed;
    command.enabled = enabled;
    return command;
}

SpectrumViewSessionCommand SpectrumViewSessionCommand::ResetSmoothing()
{
    SpectrumViewSessionCommand command;
    command.kind = SpectrumViewSessionCommandKind::ResetSmoothing;
    return command;
}

SpectrumViewSessionCommand SpectrumViewSessionCommand::SetSmoothingMethod(SpectrumSmoothingMethod method)
{
    SpectrumViewSessionCommand command;
    command.kind = SpectrumViewSessionCommandKind::SetSmoothingMethod;
    command.smoothing_method = method;
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

SpectrumViewSessionCommand SpectrumViewSessionCommand::SetPlotStyle(SpectrumPlotStyle style)
{
    SpectrumViewSessionCommand command;
    command.kind = SpectrumViewSessionCommandKind::SetPlotStyle;
    command.plot_style = style;
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
    case SpectrumViewSessionCommandKind::SetShowPoints:
        state_->plot.show_points = command.enabled;
        break;
    case SpectrumViewSessionCommandKind::SetShowSmoothed:
        state_->plot.show_smoothed = command.enabled;
        break;
    case SpectrumViewSessionCommandKind::SetShowRawWhenSmoothed:
        state_->plot.show_raw_when_smoothed = command.enabled;
        break;
    case SpectrumViewSessionCommandKind::ResetSmoothing:
        ResetSmoothing();
        break;
    case SpectrumViewSessionCommandKind::SetSmoothingMethod:
        if (state_->plot.smoothing.method != command.smoothing_method) {
            state_->plot.smoothing.method = command.smoothing_method;
            ClearSmoothingCache();
        }
        break;
    case SpectrumViewSessionCommandKind::SetGaussianSigma: {
        const double sigma = std::max(0.01, command.gaussian_sigma);
        if (state_->plot.smoothing.gaussian_sigma != sigma) {
            state_->plot.smoothing.gaussian_sigma = sigma;
            ClearSmoothingCache();
        }
        break;
    }
    case SpectrumViewSessionCommandKind::SetMedianKernelSize: {
        const int kernel_size = NormalizeMedianKernelSize(command.median_kernel_size);
        if (state_->plot.smoothing.median_kernel_size != kernel_size) {
            state_->plot.smoothing.median_kernel_size = kernel_size;
            ClearSmoothingCache();
        }
        break;
    }
    case SpectrumViewSessionCommandKind::SetPlotStyle:
        state_->style = command.plot_style;
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
    view.show_points = state_->plot.show_points;
    view.show_smoothed = state_->plot.show_smoothed;
    view.show_raw_when_smoothed = state_->plot.show_raw_when_smoothed;
    view.smoothing_active = SmoothingActive();
    view.smoothing = state_->plot.smoothing;
    view.viewport_range_mode = state_->plot.viewport_range_mode;
    return view;
}

int SpectrumViewSession::EffectiveMedianKernelSize(std::size_t point_count) const
{
    return specforge::EffectiveMedianKernelSize(
        state_->plot.smoothing.median_kernel_size,
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
    resources.reserve(2);
    if (state_->plot.smoothing_cache_source) {
        resources.push_back(state_->plot.smoothing_cache_source);
    }
    if (state_->plot.smoothed_y_values) {
        resources.push_back(state_->plot.smoothed_y_values);
    }
    return resources;
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
    const bool show_points = state_->plot.show_points;
    const bool show_smoothed = state_->plot.show_smoothed;
    const bool show_raw_when_smoothed = state_->plot.show_raw_when_smoothed;
    const SpectrumSmoothingSettings smoothing = state_->plot.smoothing;

    state_->plot = SpectrumPlotState{};
    state_->plot.viewport_range_mode =
        viewport_transition.range_mode;
    state_->plot.show_points = show_points;
    state_->plot.show_smoothed = show_smoothed;
    state_->plot.show_raw_when_smoothed = show_raw_when_smoothed;
    state_->plot.smoothing = smoothing;
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
    state_->plot.show_smoothed = false;
    state_->plot.show_raw_when_smoothed = true;
    state_->plot.smoothing = SpectrumSmoothingSettings{};
    ClearSmoothingCache();
}

void SpectrumViewSession::ClearSmoothingCache()
{
    state_->plot.smoothing_cache_source.reset();
    state_->plot.smoothed_y_values.reset();
}

bool SpectrumViewSession::SmoothingActive() const
{
    return state_->plot.show_smoothed &&
           state_->plot.smoothing.method != SpectrumSmoothingMethod::None;
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

}  // namespace specforge
