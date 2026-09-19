#pragma once

#include "domain/spectrum_smoothing.h"
#include "domain/spectrum_snapshot.h"
#include "plot/spectrum_plot.h"
#include "ui/ui_text.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace spectiary {

class SourceCollectionActivationTransaction;
enum class SourceCollectionSnapshotChangeReason;

enum class SpectrumViewportRangeAction {
    Fit,
    Preserve,
};

struct SpectrumViewportTransition {
    SpectrumViewportRangeMode range_mode =
        SpectrumViewportRangeMode::Automatic;
    SpectrumViewportRangeAction range_action =
        SpectrumViewportRangeAction::Fit;
};

[[nodiscard]] SpectrumViewportTransition ResolveViewportTransition(
    SpectrumViewportRangeMode range_mode,
    SourceCollectionSnapshotChangeReason change_reason) noexcept;

enum class SpectrumViewSessionCommandKind {
    ApplySnapshotChange,
    RequestFitView,
    SetShowRawCurve,
    SetShowPoints,
    SetShowGaussianSmoothed,
    SetShowMedianSmoothed,
    ResetSmoothing,
    SetGaussianSigma,
    SetMedianKernelSize,
    SetPlotColors,
    SetPlotSeriesColor,
    SetViewportRangeMode,
    SyncPlotLimitsOnNextRender,
};

struct SpectrumViewSessionCommand {
    [[nodiscard]] static SpectrumViewSessionCommand ApplySnapshotChange(
        SourceCollectionSnapshotChangeReason reason);
    [[nodiscard]] static SpectrumViewSessionCommand RequestFitView();
    [[nodiscard]] static SpectrumViewSessionCommand SetShowRawCurve(bool enabled);
    [[nodiscard]] static SpectrumViewSessionCommand SetShowPoints(bool enabled);
    [[nodiscard]] static SpectrumViewSessionCommand SetShowGaussianSmoothed(bool enabled);
    [[nodiscard]] static SpectrumViewSessionCommand SetShowMedianSmoothed(bool enabled);
    [[nodiscard]] static SpectrumViewSessionCommand ResetSmoothing();
    [[nodiscard]] static SpectrumViewSessionCommand SetGaussianSigma(double sigma);
    [[nodiscard]] static SpectrumViewSessionCommand SetMedianKernelSize(int kernel_size);
    [[nodiscard]] static SpectrumViewSessionCommand SetPlotColors(
        SpectrumPlotColors colors);
    [[nodiscard]] static SpectrumViewSessionCommand SetPlotSeriesColor(
        SpectrumPlotSeries series,
        PlotSeriesColor color);
    [[nodiscard]] static SpectrumViewSessionCommand SetViewportRangeMode(
        SpectrumViewportRangeMode mode);
    [[nodiscard]] static SpectrumViewSessionCommand SyncPlotLimitsOnNextRender();

private:
    friend class SpectrumViewSession;

    SpectrumViewSessionCommand() = default;

    SpectrumViewSessionCommandKind kind = SpectrumViewSessionCommandKind::RequestFitView;
    bool enabled = false;
    double gaussian_sigma = 0.0;
    int median_kernel_size = 0;
    SpectrumPlotColors plot_colors;
    SpectrumPlotSeries plot_series =
        SpectrumPlotSeries::RawSpectrum;
    PlotSeriesColor plot_series_color;
    SpectrumViewportRangeMode viewport_range_mode =
        SpectrumViewportRangeMode::Automatic;
    SourceCollectionSnapshotChangeReason snapshot_change_reason{};
};

struct SpectrumViewSessionView {
    bool show_raw_curve = true;
    bool show_points = false;
    bool show_gaussian_smoothed = false;
    bool show_median_smoothed = false;
    SpectrumSmoothingParameters smoothing_parameters;
    SpectrumPlotColors plot_colors;
    SpectrumViewportRangeMode viewport_range_mode =
        SpectrumViewportRangeMode::Automatic;
};

struct SpectrumViewRenderFeedback {
    // True when a valid plot frame was presented, even if every data series is hidden.
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
    [[nodiscard]] ImVec4 ResolveSeriesColor(
        SpectrumPlotSeries series,
        const SemanticPalette& theme_palette);
    [[nodiscard]] SpectrumViewRenderFeedback Render(
        const SpectrumSnapshotHandle& snapshot,
        UiLanguage language,
        const SpectrumPlotProfileContext& profile = {},
        const SpectrumPlotOverlays& overlays = {},
        const SpectrumPlotDisplayOptions& display = {},
        PlotTouchpadGestureSource* touchpad_gestures = nullptr);
    [[nodiscard]] bool PlotPanActive() const;
    [[nodiscard]] std::uint64_t ViewportMutationRevision() const noexcept;
    [[nodiscard]] std::vector<SpectrumValueVector> RetainHeavySnapshotResources() const;
    [[nodiscard]] std::optional<PlotViewLimits>
    LockedViewportLimits() const;
    [[nodiscard]] bool RestoreLockedViewport(
        const PlotViewLimits& limits);

private:
    struct State;

    void ApplySnapshotChange(SourceCollectionSnapshotChangeReason reason);
    void ResetSmoothing();
    void ClearGaussianSmoothingCache();
    void ClearMedianSmoothingCache();
    void ClearSmoothingCaches();

    std::unique_ptr<State> state_;
};

// Connects source activation to the presentation resources that must survive
// UI-thread snapshot replacement and to the view reset caused by that change.
void BindSourceCollectionActivationPresentationLifecycle(
    SourceCollectionActivationTransaction& activation,
    SpectrumViewSession& presentation,
    std::function<void(std::optional<std::string>)>
        deferred_restore_finished = {});

}  // namespace spectiary
