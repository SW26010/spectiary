#pragma once

#include "domain/spectrum_smoothing.h"
#include "domain/spectrum_snapshot.h"
#include "plot/spectrum_plot.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace specforge {

class SourceCollectionActivationTransaction;

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
    SyncPlotLimitsOnNextRender,
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
    [[nodiscard]] static SpectrumViewSessionCommand SyncPlotLimitsOnNextRender();

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

struct SpectrumViewRenderFeedback {
    bool plot_submitted = false;
    bool fit_applied = false;
    bool stored_limits_reused = false;
    bool pan_active = false;
    std::optional<PlotViewLimits> visible_limits;
};

class SpectrumViewSession {
public:
    SpectrumViewSession();
    ~SpectrumViewSession();

    SpectrumViewSession(const SpectrumViewSession&) = delete;
    SpectrumViewSession& operator=(const SpectrumViewSession&) = delete;
    SpectrumViewSession(SpectrumViewSession&&) noexcept;
    SpectrumViewSession& operator=(SpectrumViewSession&&) noexcept;

    void Submit(SpectrumViewSessionCommand command);

    [[nodiscard]] SpectrumViewSessionView View() const;
    [[nodiscard]] int EffectiveMedianKernelSize(std::size_t point_count) const;
    [[nodiscard]] SpectrumViewRenderFeedback Render(
        const SpectrumSnapshotHandle& snapshot,
        const SpectrumPlotProfileContext& profile = {},
        const SpectrumPlotOverlays& overlays = {},
        const SpectrumPlotDisplayOptions& display = {},
        PlotTouchpadGestureSource* touchpad_gestures = nullptr);
    [[nodiscard]] bool PlotPanActive() const;
    [[nodiscard]] std::vector<SpectrumValueVector> RetainHeavySnapshotResources() const;

private:
    struct State;

    void ResetForSnapshotChange();
    void ResetSmoothing();
    void ClearSmoothingCache();
    [[nodiscard]] bool SmoothingActive() const;

    std::unique_ptr<State> state_;
};

// Connects source activation to the presentation resources that must survive
// UI-thread snapshot replacement and to the view reset caused by that change.
void BindSourceCollectionActivationPresentationLifecycle(
    SourceCollectionActivationTransaction& activation,
    SpectrumViewSession& presentation);

}  // namespace specforge
