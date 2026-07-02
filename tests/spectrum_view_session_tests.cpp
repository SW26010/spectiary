#include "ui/spectrum_view_session.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

void Require(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void TestSnapshotResetPreservesDisplayControls()
{
    specforge::SpectrumViewSession session;
    session.Submit(specforge::SpectrumViewSessionCommand::SetShowPoints(true));
    session.Submit(specforge::SpectrumViewSessionCommand::SetShowSmoothed(true));
    session.Submit(specforge::SpectrumViewSessionCommand::SetShowRawWhenSmoothed(false));
    session.Submit(
        specforge::SpectrumViewSessionCommand::SetSmoothingMethod(specforge::SpectrumSmoothingMethod::Gaussian));
    session.Submit(specforge::SpectrumViewSessionCommand::SetGaussianSigma(3.25));

    specforge::SpectrumPlotState& render_state = session.PlotStateForRender();
    render_state.fit_next_frame = false;
    render_state.pan_drag_active = true;
    render_state.has_profile_limits = true;
    render_state.profiled_x_min = 1.0;
    render_state.profiled_x_max = 2.0;
    render_state.has_last_limits = true;
    render_state.sync_last_limits_next_frame = true;
    render_state.smoothing_cache_source = std::make_shared<std::vector<double>>(std::vector<double>{1.0, 2.0});
    render_state.smoothing_cache_settings = render_state.smoothing;
    render_state.smoothed_y_values = std::make_shared<std::vector<double>>(std::vector<double>{1.5, 2.5});

    session.Submit(specforge::SpectrumViewSessionCommand::ResetForSnapshotChange());

    const specforge::SpectrumViewSessionView view = session.View();
    const specforge::SpectrumPlotState& reset_render_state = session.PlotStateForRender();

    Require(view.show_points, "snapshot reset should preserve show-points display state");
    Require(view.show_smoothed, "snapshot reset should preserve smoothing visibility");
    Require(!view.show_raw_when_smoothed, "snapshot reset should preserve raw-overlay visibility");
    Require(
        view.smoothing.method == specforge::SpectrumSmoothingMethod::Gaussian,
        "snapshot reset should preserve smoothing method");
    Require(
        std::abs(view.smoothing.gaussian_sigma - 3.25) < 1.0e-12,
        "snapshot reset should preserve gaussian sigma");
    Require(reset_render_state.fit_next_frame, "snapshot reset should request a fit on the next render");
    Require(!reset_render_state.pan_drag_active, "snapshot reset should clear pan drag runtime state");
    Require(!reset_render_state.has_profile_limits, "snapshot reset should clear profiled plot limits");
    Require(!reset_render_state.has_last_limits, "snapshot reset should clear reusable plot limits");
    Require(
        !reset_render_state.sync_last_limits_next_frame,
        "snapshot reset should clear pending plot-limit sync");
    Require(!reset_render_state.smoothing_cache_source, "snapshot reset should clear smoothing cache source");
    Require(!reset_render_state.smoothed_y_values, "snapshot reset should clear smoothed value cache");
}

void TestPlotLimitSyncUsesStoredLimitsForModeSwitch()
{
    specforge::SpectrumViewSession session;

    session.Submit(specforge::SpectrumViewSessionCommand::SyncPlotLimitsOnNextRender());
    Require(
        !session.PlotStateForRender().sync_last_limits_next_frame,
        "plot limit sync should be a no-op before any reusable limits exist");

    specforge::SpectrumPlotState& render_state = session.PlotStateForRender();
    render_state.fit_next_frame = false;
    render_state.has_last_limits = true;
    render_state.last_x_min = 4100.0;
    render_state.last_x_max = 4900.0;
    render_state.last_y_min = -0.25;
    render_state.last_y_max = 1.75;

    session.Submit(specforge::SpectrumViewSessionCommand::SyncPlotLimitsOnNextRender());

    Require(
        render_state.sync_last_limits_next_frame,
        "plot limit sync should request the next render to reuse stored limits");
    Require(!render_state.fit_next_frame, "plot limit sync should not request fit-view");
    Require(std::abs(render_state.last_x_min - 4100.0) < 1.0e-12, "plot limit sync should preserve x min");
    Require(std::abs(render_state.last_x_max - 4900.0) < 1.0e-12, "plot limit sync should preserve x max");
    Require(std::abs(render_state.last_y_min + 0.25) < 1.0e-12, "plot limit sync should preserve y min");
    Require(std::abs(render_state.last_y_max - 1.75) < 1.0e-12, "plot limit sync should preserve y max");
}

void TestSmoothingCommandsNormalizeAndClearCache()
{
    specforge::SpectrumViewSession session;
    session.Submit(specforge::SpectrumViewSessionCommand::SetSmoothingMethod(specforge::SpectrumSmoothingMethod::Median));
    session.Submit(specforge::SpectrumViewSessionCommand::SetMedianKernelSize(4));

    specforge::SpectrumViewSessionView view = session.View();
    Require(view.smoothing.median_kernel_size == 5, "median kernel command should normalize to an odd size");
    Require(session.EffectiveMedianKernelSize(4) == 3, "effective median kernel should clamp to data length");

    specforge::SpectrumPlotState& render_state = session.PlotStateForRender();
    render_state.smoothing_cache_source = std::make_shared<std::vector<double>>(std::vector<double>{1.0, 2.0});
    render_state.smoothing_cache_settings = render_state.smoothing;
    render_state.smoothed_y_values = std::make_shared<std::vector<double>>(std::vector<double>{1.0, 2.0});

    session.Submit(specforge::SpectrumViewSessionCommand::SetMedianKernelSize(9));

    Require(!render_state.smoothing_cache_source, "changing median kernel should clear smoothing cache source");
    Require(!render_state.smoothed_y_values, "changing median kernel should clear smoothed value cache");
    Require(session.View().smoothing.median_kernel_size == 9, "median kernel command should apply normalized value");
}

void TestResetSmoothingRestoresDefaults()
{
    specforge::SpectrumViewSession session;
    session.Submit(specforge::SpectrumViewSessionCommand::SetShowSmoothed(true));
    session.Submit(specforge::SpectrumViewSessionCommand::SetShowRawWhenSmoothed(false));
    session.Submit(
        specforge::SpectrumViewSessionCommand::SetSmoothingMethod(specforge::SpectrumSmoothingMethod::Gaussian));
    session.Submit(specforge::SpectrumViewSessionCommand::SetGaussianSigma(2.0));

    session.Submit(specforge::SpectrumViewSessionCommand::ResetSmoothing());

    const specforge::SpectrumViewSessionView view = session.View();
    Require(!view.show_smoothed, "reset smoothing should hide smoothed curve");
    Require(view.show_raw_when_smoothed, "reset smoothing should restore raw overlay default");
    Require(
        view.smoothing.method == specforge::SpectrumSmoothingMethod::None,
        "reset smoothing should restore smoothing method");
    Require(
        std::abs(view.smoothing.gaussian_sigma - specforge::SpectrumSmoothingSettings{}.gaussian_sigma) < 1.0e-12,
        "reset smoothing should restore gaussian sigma");
}

}  // namespace

int main()
{
    TestSnapshotResetPreservesDisplayControls();
    TestPlotLimitSyncUsesStoredLimitsForModeSwitch();
    TestSmoothingCommandsNormalizeAndClearCache();
    TestResetSmoothingRestoresDefaults();
    return 0;
}
