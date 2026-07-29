#include "ui/spectrum_view_session.h"

#include "plot/spectrum_plot_renderer.h"
#include "ui/source_collection_activation_transaction.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace specforge {

struct SpectrumViewSession::State {
    SpectrumPlotState plot;
    SpectrumPlotStyle style;
    SpectrumViewRenderFeedback last_render;
};

SpectrumViewSessionCommand SpectrumViewSessionCommand::ResetForSnapshotChange()
{
    SpectrumViewSessionCommand command;
    command.kind = SpectrumViewSessionCommandKind::ResetForSnapshotChange;
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
    case SpectrumViewSessionCommandKind::ResetForSnapshotChange:
        ResetForSnapshotChange();
        break;
    case SpectrumViewSessionCommandKind::RequestFitView:
        state_->plot.fit_next_frame = true;
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
    const SpectrumPlotRenderResult result = RenderSpectrumPlot(
        snapshot,
        state_->plot,
        language,
        profile,
        state_->style,
        overlays,
        display,
        touchpad_gestures);
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

void SpectrumViewSession::ResetForSnapshotChange()
{
    const bool show_points = state_->plot.show_points;
    const bool show_smoothed = state_->plot.show_smoothed;
    const bool show_raw_when_smoothed = state_->plot.show_raw_when_smoothed;
    const SpectrumSmoothingSettings smoothing = state_->plot.smoothing;

    state_->plot = SpectrumPlotState{};
    state_->plot.show_points = show_points;
    state_->plot.show_smoothed = show_smoothed;
    state_->plot.show_raw_when_smoothed = show_raw_when_smoothed;
    state_->plot.smoothing = smoothing;
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
    SpectrumViewSession& presentation)
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
        [&presentation]() {
            presentation.Submit(
                SpectrumViewSessionCommand::
                    ResetForSnapshotChange());
        });
}

}  // namespace specforge
