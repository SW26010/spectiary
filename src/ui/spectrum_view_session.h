#pragma once

#include "domain/spectrum_smoothing.h"
#include "plot/spectrum_plot.h"

#include <cstddef>

namespace specforge {

enum class SpectrumViewSessionCommandKind {
    ResetForSnapshotChange,
    RequestFitView,
    SetShowPoints,
    SetShowSmoothed,
    SetShowRawWhenSmoothed,
    ResetSmoothing,
    SetSmoothingMethod,
    SetGaussianSigma,
    SetMedianKernelSize,
    SetPlotStyle,
};

struct SpectrumViewSessionCommand {
    [[nodiscard]] static SpectrumViewSessionCommand ResetForSnapshotChange();
    [[nodiscard]] static SpectrumViewSessionCommand RequestFitView();
    [[nodiscard]] static SpectrumViewSessionCommand SetShowPoints(bool enabled);
    [[nodiscard]] static SpectrumViewSessionCommand SetShowSmoothed(bool enabled);
    [[nodiscard]] static SpectrumViewSessionCommand SetShowRawWhenSmoothed(bool enabled);
    [[nodiscard]] static SpectrumViewSessionCommand ResetSmoothing();
    [[nodiscard]] static SpectrumViewSessionCommand SetSmoothingMethod(SpectrumSmoothingMethod method);
    [[nodiscard]] static SpectrumViewSessionCommand SetGaussianSigma(double sigma);
    [[nodiscard]] static SpectrumViewSessionCommand SetMedianKernelSize(int kernel_size);
    [[nodiscard]] static SpectrumViewSessionCommand SetPlotStyle(SpectrumPlotStyle style);

private:
    friend class SpectrumViewSession;

    SpectrumViewSessionCommand() = default;

    SpectrumViewSessionCommandKind kind = SpectrumViewSessionCommandKind::RequestFitView;
    bool enabled = false;
    SpectrumSmoothingMethod smoothing_method = SpectrumSmoothingMethod::None;
    double gaussian_sigma = 0.0;
    int median_kernel_size = 0;
    SpectrumPlotStyle plot_style;
};

struct SpectrumViewSessionView {
    bool show_points = false;
    bool show_smoothed = false;
    bool show_raw_when_smoothed = true;
    bool smoothing_active = false;
    SpectrumSmoothingSettings smoothing;
};

class SpectrumViewSession {
public:
    void Submit(SpectrumViewSessionCommand command);

    [[nodiscard]] SpectrumViewSessionView View() const;
    [[nodiscard]] int EffectiveMedianKernelSize(std::size_t point_count) const;
    [[nodiscard]] SpectrumPlotState& PlotStateForRender();
    [[nodiscard]] const SpectrumPlotStyle& PlotStyleForRender() const;

private:
    void ResetForSnapshotChange();
    void ResetSmoothing();
    void ClearSmoothingCache();
    [[nodiscard]] bool SmoothingActive() const;

    SpectrumPlotState plot_state_;
    SpectrumPlotStyle plot_style_;
};

}  // namespace specforge
