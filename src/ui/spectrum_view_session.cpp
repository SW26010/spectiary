#include "ui/spectrum_view_session.h"

#include <algorithm>
#include <utility>

namespace specforge {

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

void SpectrumViewSession::Submit(SpectrumViewSessionCommand command)
{
    switch (command.kind) {
    case SpectrumViewSessionCommandKind::ResetForSnapshotChange:
        ResetForSnapshotChange();
        break;
    case SpectrumViewSessionCommandKind::RequestFitView:
        plot_state_.fit_next_frame = true;
        break;
    case SpectrumViewSessionCommandKind::SetShowPoints:
        plot_state_.show_points = command.enabled;
        break;
    case SpectrumViewSessionCommandKind::SetShowSmoothed:
        plot_state_.show_smoothed = command.enabled;
        break;
    case SpectrumViewSessionCommandKind::SetShowRawWhenSmoothed:
        plot_state_.show_raw_when_smoothed = command.enabled;
        break;
    case SpectrumViewSessionCommandKind::ResetSmoothing:
        ResetSmoothing();
        break;
    case SpectrumViewSessionCommandKind::SetSmoothingMethod:
        if (plot_state_.smoothing.method != command.smoothing_method) {
            plot_state_.smoothing.method = command.smoothing_method;
            ClearSmoothingCache();
        }
        break;
    case SpectrumViewSessionCommandKind::SetGaussianSigma: {
        const double sigma = std::max(0.01, command.gaussian_sigma);
        if (plot_state_.smoothing.gaussian_sigma != sigma) {
            plot_state_.smoothing.gaussian_sigma = sigma;
            ClearSmoothingCache();
        }
        break;
    }
    case SpectrumViewSessionCommandKind::SetMedianKernelSize: {
        const int kernel_size = NormalizeMedianKernelSize(command.median_kernel_size);
        if (plot_state_.smoothing.median_kernel_size != kernel_size) {
            plot_state_.smoothing.median_kernel_size = kernel_size;
            ClearSmoothingCache();
        }
        break;
    }
    case SpectrumViewSessionCommandKind::SetPlotStyle:
        plot_style_ = command.plot_style;
        break;
    case SpectrumViewSessionCommandKind::SyncPlotLimitsOnNextRender:
        if (plot_state_.has_last_limits) {
            plot_state_.sync_last_limits_next_frame = true;
        }
        break;
    }
}

SpectrumViewSessionView SpectrumViewSession::View() const
{
    SpectrumViewSessionView view;
    view.show_points = plot_state_.show_points;
    view.show_smoothed = plot_state_.show_smoothed;
    view.show_raw_when_smoothed = plot_state_.show_raw_when_smoothed;
    view.smoothing_active = SmoothingActive();
    view.smoothing = plot_state_.smoothing;
    return view;
}

int SpectrumViewSession::EffectiveMedianKernelSize(std::size_t point_count) const
{
    return specforge::EffectiveMedianKernelSize(plot_state_.smoothing.median_kernel_size, point_count);
}

SpectrumPlotState& SpectrumViewSession::PlotStateForRender()
{
    return plot_state_;
}

const SpectrumPlotStyle& SpectrumViewSession::PlotStyleForRender() const
{
    return plot_style_;
}

bool SpectrumViewSession::PlotPanActive() const
{
    return plot_state_.pan_drag_active;
}

std::vector<SpectrumValueVector> SpectrumViewSession::RetainHeavySnapshotResources() const
{
    std::vector<SpectrumValueVector> resources;
    resources.reserve(2);
    if (plot_state_.smoothing_cache_source) {
        resources.push_back(plot_state_.smoothing_cache_source);
    }
    if (plot_state_.smoothed_y_values) {
        resources.push_back(plot_state_.smoothed_y_values);
    }
    return resources;
}

void SpectrumViewSession::ResetForSnapshotChange()
{
    const bool show_points = plot_state_.show_points;
    const bool show_smoothed = plot_state_.show_smoothed;
    const bool show_raw_when_smoothed = plot_state_.show_raw_when_smoothed;
    const SpectrumSmoothingSettings smoothing = plot_state_.smoothing;

    plot_state_ = SpectrumPlotState{};
    plot_state_.show_points = show_points;
    plot_state_.show_smoothed = show_smoothed;
    plot_state_.show_raw_when_smoothed = show_raw_when_smoothed;
    plot_state_.smoothing = smoothing;
}

void SpectrumViewSession::ResetSmoothing()
{
    plot_state_.show_smoothed = false;
    plot_state_.show_raw_when_smoothed = true;
    plot_state_.smoothing = SpectrumSmoothingSettings{};
    ClearSmoothingCache();
}

void SpectrumViewSession::ClearSmoothingCache()
{
    plot_state_.smoothing_cache_source.reset();
    plot_state_.smoothed_y_values.reset();
}

bool SpectrumViewSession::SmoothingActive() const
{
    return plot_state_.show_smoothed && plot_state_.smoothing.method != SpectrumSmoothingMethod::None;
}

}  // namespace specforge
