#include "helpers/source_load_test_support.h"
#include "ui/spectrum_view_session.h"

#include "ui/immersive_context_overlay.h"
#include "ui/sample_workflow_preparation.h"
#include "ui/source_collection_activation_transaction.h"
#include "ui/source_collection_load_queue_internal.h"
#include "ui/source_collection_roster.h"
#include "ui/spectrum_plot_preferences_io.h"
#include "ui/spectrum_viewport_state_io.h"
#include "ui/legacy_spectrum_view_state_io.h"
#include "ui/spectrum_persistence_migration.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <implot.h>
#include <implot_internal.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

void Require(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void RequireNear(double actual, double expected, const std::string& message)
{
    Require(
        std::abs(actual - expected) < 1.0e-9,
        message + ": expected " + std::to_string(expected) + ", got " + std::to_string(actual));
}

spectiary::SpectrumSnapshotHandle MakeSnapshot(
    std::vector<double> x_values,
    spectiary::SpectrumValueVector y_values)
{
    auto snapshot = std::make_shared<spectiary::SpectrumSnapshot>();
    snapshot->current_spectrum.name = "test spectrum";
    snapshot->current_spectrum.x_values =
        std::make_shared<const std::vector<double>>(std::move(x_values));
    snapshot->current_spectrum.y_values = std::move(y_values);
    snapshot->current_spectrum.point_count = snapshot->current_spectrum.x_values->size();
    snapshot->axis.x_label = "Wavelength (Å)";
    snapshot->axis.y_label = "Flux";
    snapshot->capabilities.can_plot_current_spectrum = true;
    return snapshot;
}

spectiary::SpectrumSnapshotHandle MakeSnapshot(
    std::vector<double> x_values,
    std::vector<double> y_values)
{
    return MakeSnapshot(
        std::move(x_values),
        std::make_shared<const std::vector<double>>(
            std::move(y_values)));
}

std::filesystem::path UniqueTempPath()
{
    static std::atomic_uint64_t next_id = 1;
    return std::filesystem::temp_directory_path() /
        ("spectiary_activation_presentation_binding_" +
         std::to_string(next_id.fetch_add(1)) +
         ".csv");
}

spectiary::SpectrumSnapshotHandle MakeActivationSnapshot(
    const std::filesystem::path& path,
    std::size_t spectrum_index)
{
    const spectiary::SpectrumSnapshotHandle source =
        MakeSnapshot(
            {1.0, 2.0, 3.0},
            {3.0, 2.0, 1.0});
    auto snapshot =
        std::make_shared<spectiary::SpectrumSnapshot>(
            *source);
    snapshot->source.id = "binding-fixture";
    snapshot->source.display_name = "binding-fixture";
    snapshot->source.path = path;
    snapshot->collection.spectrum_count = 1;
    snapshot->collection.current_index =
        spectrum_index;
    snapshot->capabilities.can_switch_spectrum = false;
    return snapshot;
}

class QueuedTouchpadGestureSource final : public spectiary::PlotTouchpadGestureSource {
public:
    [[nodiscard]] spectiary::PlotTouchpadGestureBatch Poll(std::uintptr_t) override
    {
        spectiary::PlotTouchpadGestureBatch result = std::move(next_batch_);
        next_batch_ = {};
        return result;
    }

    void SetTarget(const spectiary::PlotTouchpadTarget& target) override
    {
        target_ = target;
    }

    void ClearTarget() override
    {
        target_ = {};
    }

    void QueueZoom(double factor)
    {
        Require(target_.plot_rect.IsValid(), "touchpad target should be available after rendering");
        QueueZoomAt(
            ImVec2(
                (target_.plot_rect.left + target_.plot_rect.right) * 0.5f,
                (target_.plot_rect.top + target_.plot_rect.bottom) * 0.5f),
            factor);
    }

    void QueuePanAt(const ImVec2& anchor, float pan_x, float pan_y)
    {
        Require(target_.plot_rect.IsValid(), "touchpad target should be available after rendering");
        spectiary::PlotTouchpadGestureDelta pan;
        pan.kind = spectiary::PlotTouchpadGestureKind::Pan;
        pan.axes = spectiary::PlotGestureAxes::Both;
        pan.plot_rect = target_.plot_rect;
        pan.anchor_x = anchor.x;
        pan.anchor_y = anchor.y;
        pan.pan_x = pan_x;
        pan.pan_y = pan_y;
        next_batch_.deltas.push_back(pan);
        next_batch_.active = true;
    }

    void QueueZoomAt(const ImVec2& anchor, double factor)
    {
        Require(target_.plot_rect.IsValid(), "touchpad target should be available after rendering");
        spectiary::PlotTouchpadGestureDelta zoom;
        zoom.kind = spectiary::PlotTouchpadGestureKind::Zoom;
        zoom.axes = spectiary::PlotGestureAxes::Both;
        zoom.plot_rect = target_.plot_rect;
        zoom.anchor_x = anchor.x;
        zoom.anchor_y = anchor.y;
        zoom.zoom_factor = factor;
        next_batch_.deltas.push_back(zoom);
        next_batch_.active = true;
    }

    [[nodiscard]] ImVec2 ViewportLockCenter() const
    {
        Require(
            target_.input_exclusion_rect.IsValid(),
            "touchpad target should expose the viewport lock exclusion");
        return ImVec2(
            (target_.input_exclusion_rect.left + target_.input_exclusion_rect.right) * 0.5f,
            (target_.input_exclusion_rect.top + target_.input_exclusion_rect.bottom) * 0.5f);
    }

private:
    spectiary::PlotTouchpadTarget target_;
    spectiary::PlotTouchpadGestureBatch next_batch_;
};

class ScopedPlotUi {
public:
    ScopedPlotUi()
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImPlot::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        unsigned char* font_pixels = nullptr;
        int font_width = 0;
        int font_height = 0;
        io.Fonts->GetTexDataAsRGBA32(&font_pixels, &font_width, &font_height);
        Require(
            font_pixels != nullptr && font_width > 0 && font_height > 0,
            "ImGui font atlas should build");
        ImGui::GetMainViewport()->PlatformHandleRaw = reinterpret_cast<void*>(1);
    }

    ~ScopedPlotUi()
    {
        ImPlot::DestroyContext();
        ImGui::DestroyContext();
    }

    ScopedPlotUi(const ScopedPlotUi&) = delete;
    ScopedPlotUi& operator=(const ScopedPlotUi&) = delete;

    spectiary::SpectrumViewRenderFeedback RenderFrame(
        spectiary::SpectrumViewSession& session,
        const spectiary::SpectrumSnapshotHandle& snapshot,
        ImVec2 mouse_position = ImVec2(400.0f, 300.0f),
        bool left_button_down = false,
        spectiary::PlotTouchpadGestureSource* touchpad_gestures = nullptr,
        spectiary::SpectrumPlotDisplayOptions display = {},
        float mouse_wheel = 0.0f,
        bool check_y_tick_labels = false,
        ImVec2 window_size = ImVec2(800.0f, 600.0f),
        const std::function<void(ImPlotPlot&)>& inspect_plot = {})
    {
        ImGuiIO& io = ImGui::GetIO();
        io.DeltaTime = 1.0f / 60.0f;
        io.DisplaySize = window_size;
        io.AddMousePosEvent(mouse_position.x, mouse_position.y);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, left_button_down);
        if (mouse_wheel != 0.0f) {
            io.AddMouseWheelEvent(0.0f, mouse_wheel);
        }
        ImGui::NewFrame();

        constexpr ImGuiWindowFlags kWindowFlags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(window_size, ImGuiCond_Always);
        Require(
            ImGui::Begin("Spectrum view session test", nullptr, kWindowFlags),
            "test plot window should be visible");
        const spectiary::SpectrumViewRenderFeedback feedback = session.Render(
            snapshot,
            spectiary::UiLanguage::English,
            {},
            {},
            display,
            touchpad_gestures);
        if (inspect_plot) {
            ImPlotPlot* plot = ImPlot::GetPlot("###main_spectrum");
            Require(plot != nullptr, "production plot must exist for inspection");
            inspect_plot(*plot);
        }
        if (check_y_tick_labels) {
            // Inspect generated labels only in tests; production uses public ImPlot APIs.
            ImPlotPlot* plot = ImPlot::GetPlot("###main_spectrum");
            Require(plot != nullptr, "the production spectrum plot should exist");
            const ImPlotAxis& axis = plot->Axes[ImAxis_Y1];
            int readable_ticks = 0;
            for (const ImPlotTick& tick : axis.Ticker.Ticks) {
                if (!tick.ShowLabel || tick.PlotPos < axis.Range.Min ||
                    tick.PlotPos > axis.Range.Max) {
                    continue;
                }
                const char* label = axis.Ticker.GetText(tick.Idx);
                char* end = nullptr;
                const double value = std::strtod(label, &end);
                Require(end != label && *end == '\0' && std::isfinite(value),
                    "visible Y ticks must contain a finite numeric label");
                Require(std::abs(value - tick.PlotPos) <=
                        axis.Range.Size() * 1.0e-4,
                    "Y tick labels must represent their source coordinates");
                ++readable_ticks;
            }
            Require(readable_ticks >= 2, "Y axis must retain useful numeric ticks");
        }
        ImGui::End();
        ImGui::EndFrame();
        return feedback;
    }

    struct OverlayFrameFeedback {
        spectiary::ImmersiveContextOverlayRenderResult render;
        ImGuiID plot_input_id_before_overlay = 0;
        ImGuiID plot_input_id_after_overlay = 0;
    };

    OverlayFrameFeedback RenderOverlayFrame(
        const spectiary::ImmersiveContextOverlayView& overlay,
        ImVec2 mouse_position)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.DeltaTime = 1.0f / 60.0f;
        io.DisplaySize = ImVec2(800.0f, 600.0f);
        io.AddMousePosEvent(mouse_position.x, mouse_position.y);
        ImGui::NewFrame();

        constexpr ImGuiWindowFlags kWindowFlags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(800.0f, 600.0f), ImGuiCond_Always);
        Require(
            ImGui::Begin("Immersive context overlay test", nullptr, kWindowFlags),
            "test overlay window should be visible");
        ImGui::InvisibleButton(
            "##PlotInputSurface",
            ImGui::GetContentRegionAvail());
        OverlayFrameFeedback feedback;
        feedback.plot_input_id_before_overlay =
            GImGui->LastItemData.ID;
        feedback.render =
            spectiary::RenderImmersiveContextOverlay(
                overlay);
        feedback.plot_input_id_after_overlay =
            GImGui->LastItemData.ID;
        ImGui::End();
        ImGui::EndFrame();
        return feedback;
    }
};

spectiary::SourceCollectionSessionView
MakeImmersiveContextSessionView()
{
    spectiary::SourceCollectionSessionView view;
    view.current_sample_snapshot =
        MakeSnapshot(
            {1.0, 2.0, 3.0},
            {3.0, 2.0, 1.0});
    view.navigation.resolved_sequence_position = {
        .zero_based_position = 4,
        .sequence_length = 1000,
    };
    // Conflicting legacy fields make accidental fallback immediately visible.
    view.navigation.sequence_count = 7;
    view.navigation.current_sequence_position = 1;
    view.labeling.has_active_task = true;
    view.labeling.current_index = 4;
    view.labeling.current_code = 3;
    view.labeling.label_set.labels = {
        {3, "C", 'c'},
        {4, "D", 'd'},
    };
    return view;
}

void TestImmersiveContextOverlayUsesResolvedPositionAndCurrentLabel()
{
    spectiary::SourceCollectionSessionView view =
        MakeImmersiveContextSessionView();
    const auto overlay =
        spectiary::BuildImmersiveContextOverlayView(
            true,
            view,
            spectiary::UiLanguage::English);
    Require(
        overlay.has_value(),
        "immersive context should be available for a resolved sample");
    Require(
        overlay->sequence_position_text == "5 / 1000",
        "immersive position should use only the resolved sequence pair");
    Require(
        overlay->labeling_context_text == "Label: C" &&
            !overlay->presents_previous_label,
        "immersive context should present the current compact label");

    view.labeling.current_code =
        spectiary::kUnlabeledSampleLabelCode;
    const auto unset =
        spectiary::BuildImmersiveContextOverlayView(
            true,
            view,
            spectiary::UiLanguage::English);
    Require(
        unset &&
            unset->labeling_context_text ==
                "Label: Unlabeled (-1)",
        "immersive unset labels should reuse the labeling-panel sentinel semantics");
}

void TestImmersiveContextOverlayTracksAutoAdvanceProvenance()
{
    spectiary::SourceCollectionSessionView view =
        MakeImmersiveContextSessionView();
    view.labeling.auto_advance = true;
    view.labeling.current_index = 5;
    view.labeling.current_code = 4;
    view.sample_transition =
        spectiary::SourceCollectionSampleTransitionView{
            .reason = spectiary::SourceCollectionSampleTransitionReason::LabelingAutoAdvance,
            .from_sample_index = 4,
            .current_sample_index = 5,
            .accepted_label_value = 3,
        };
    const auto auto_advance =
        spectiary::BuildImmersiveContextOverlayView(
            true,
            view,
            spectiary::UiLanguage::English);
    Require(
        auto_advance &&
            auto_advance->labeling_context_text ==
                "Previous label: C" &&
            auto_advance->presents_previous_label,
        "labeling auto-advance should present the accepted previous label");

    view.sample_transition =
        spectiary::SourceCollectionSampleTransitionView{
            .reason = spectiary::SourceCollectionSampleTransitionReason::LocateRow,
            .from_sample_index = 5,
            .current_sample_index = 4,
        };
    const auto manual =
        spectiary::BuildImmersiveContextOverlayView(
            true,
            view,
            spectiary::UiLanguage::English);
    Require(
        manual &&
            manual->labeling_context_text == "Label: D" &&
            !manual->presents_previous_label,
        "manual navigation should restore current-label presentation");
}

void TestImmersiveContextOverlayIsImmersiveOnlyAndHandlesUnavailableContext()
{
    spectiary::SourceCollectionSessionView view =
        MakeImmersiveContextSessionView();
    Require(
        !spectiary::BuildImmersiveContextOverlayView(
            false,
            view,
            spectiary::UiLanguage::English),
        "normal plot mode must not expose the immersive context overlay");

    view.labeling.has_active_task = false;
    const auto position_only =
        spectiary::BuildImmersiveContextOverlayView(
            true,
            view,
            spectiary::UiLanguage::English);
    Require(
        position_only &&
            position_only->sequence_position_text == "5 / 1000" &&
            position_only->labeling_context_text.empty(),
        "without an active labeling task the overlay should show position only");

    view.navigation.resolved_sequence_position.zero_based_position.reset();
    Require(
        !spectiary::BuildImmersiveContextOverlayView(
            true,
            view,
            spectiary::UiLanguage::English),
        "an unavailable resolved position must not fall back to legacy navigation fields");

    view.navigation.resolved_sequence_position.zero_based_position = 4;
    view.current_sample_snapshot.reset();
    Require(
        !spectiary::BuildImmersiveContextOverlayView(
            true,
            view,
            spectiary::UiLanguage::English),
        "a pending navigation cursor must not present a position before its sample is shown");
}

void TestImmersiveContextOverlayDrawsWithoutCapturingPlotInput()
{
    ScopedPlotUi ui;
    const ScopedPlotUi::OverlayFrameFeedback feedback =
        ui.RenderOverlayFrame(
            spectiary::ImmersiveContextOverlayView{
                .sequence_position_text = "5 / 1000",
                .labeling_context_text = "Label: C",
            },
            ImVec2(30.0f, 30.0f));
    Require(
        feedback.render.rendered,
        "immersive context component should submit draw commands");
    Require(
        feedback.plot_input_id_before_overlay != 0 &&
            feedback.plot_input_id_after_overlay ==
                feedback.plot_input_id_before_overlay,
        "draw-list overlay should not register an item over the plot input surface");
    Require(
        feedback.render.min.x < 50.0f &&
            feedback.render.min.y < 50.0f &&
            feedback.render.max.y < 150.0f,
        "immersive context should stay in the upper-left, away from Keep View in the lower-left");
}

void ConfigureGaussianSmoothing(spectiary::SpectrumViewSession& session)
{
    session.Submit(spectiary::SpectrumViewSessionCommand::SetShowPoints(true));
    session.Submit(spectiary::SpectrumViewSessionCommand::SetShowRawCurve(false));
    session.Submit(spectiary::SpectrumViewSessionCommand::SetShowGaussianSmoothed(true));
    session.Submit(spectiary::SpectrumViewSessionCommand::SetGaussianSigma(3.25));
}

void TestViewportTransitionPolicyUsesChangeReason()
{
    const spectiary::SpectrumViewportTransition same_collection =
        spectiary::ResolveViewportTransition(
            spectiary::SpectrumViewportRangeMode::Locked,
            spectiary::SourceCollectionSnapshotChangeReason::SampleChangedWithinCollection);
    Require(
        same_collection.range_mode == spectiary::SpectrumViewportRangeMode::Locked &&
            same_collection.range_action == spectiary::SpectrumViewportRangeAction::Preserve,
        "a locked view should preserve its range for samples in the same collection");

    const spectiary::SpectrumViewportTransition same_sample_reload =
        spectiary::ResolveViewportTransition(
            spectiary::SpectrumViewportRangeMode::Locked,
            spectiary::SourceCollectionSnapshotChangeReason::SnapshotReloadedWithinCollection);
    Require(
        same_sample_reload.range_mode == spectiary::SpectrumViewportRangeMode::Locked &&
            same_sample_reload.range_action == spectiary::SpectrumViewportRangeAction::Fit,
        "reloading the current sample should fit without leaving Keep View mode");

    const spectiary::SpectrumViewportTransition different_collection =
        spectiary::ResolveViewportTransition(
            spectiary::SpectrumViewportRangeMode::Locked,
            spectiary::SourceCollectionSnapshotChangeReason::SourceCollectionChanged);
    Require(
        different_collection.range_mode == spectiary::SpectrumViewportRangeMode::Automatic &&
            different_collection.range_action == spectiary::SpectrumViewportRangeAction::Fit,
        "switching collections should unlock and fit the viewport");

    const spectiary::SpectrumViewportTransition cleared =
        spectiary::ResolveViewportTransition(
            spectiary::SpectrumViewportRangeMode::Locked,
            spectiary::SourceCollectionSnapshotChangeReason::SourceCollectionCleared);
    Require(
        cleared.range_mode == spectiary::SpectrumViewportRangeMode::Automatic &&
            cleared.range_action == spectiary::SpectrumViewportRangeAction::Fit,
        "clearing the active collection should unlock and fit the viewport");
}

void TestLockedViewportStateCacheRoundTripsAndClears()
{
    const std::filesystem::path path = UniqueTempPath();
    const spectiary::SpectrumViewportState locked{
        .locked = true,
        .source_collection_identity =
            "viewport-cache-collection",
        .limits = {
            .x_min = 4100.125,
            .x_max = 4900.875,
            .y_min = -0.03125,
            .y_max = 2.0625,
        },
    };
    const spectiary::SpectrumPlotPreferences preferences{
        .plot_colors = {
            .raw_spectrum =
                spectiary::PlotSeriesColor::ExplicitColor({
                    .red = 0.125f,
                    .green = 0.25f,
                    .blue = 0.5f,
                    .alpha = 0.75f,
                }),
            .median_smoothing =
                spectiary::PlotSeriesColor::ExplicitColor({
                    .red = 0.9f,
                    .green = 0.7f,
                    .blue = 0.3f,
                    .alpha = 0.6f,
                }),
        },
    };
    std::string error;
    Require(
        spectiary::SaveSpectrumViewportState(
            path,
            locked,
            &error),
        error.empty()
            ? "locked viewport cache should save"
            : error);
    const spectiary::SpectrumViewportStateLoadResult loaded =
        spectiary::LoadSpectrumViewportState(path);
    Require(
        loaded.warning.empty() && loaded.state.locked &&
            loaded.state.source_collection_identity ==
                locked.source_collection_identity &&
            loaded.state.limits.x_min ==
                locked.limits.x_min &&
            loaded.state.limits.x_max ==
                locked.limits.x_max &&
            loaded.state.limits.y_min ==
                locked.limits.y_min &&
            loaded.state.limits.y_max ==
                locked.limits.y_max,
        "viewport state should round-trip lock, identity and all axis limits");
    {
        std::ifstream stream(path);
        const auto document = nlohmann::json::parse(stream);
        Require(document.contains("locked") && !document.contains("series_colors"),
            "viewport JSON must contain no preference-owned fields");
    }

    const auto preferences_path = path.string() + ".preferences";
    Require(spectiary::SaveSpectrumPlotPreferences(preferences_path, preferences, &error),
        "preferences save should not require a source or locked viewport");
    Require(spectiary::LoadSpectrumPlotPreferences(preferences_path).state.plot_colors == preferences.plot_colors,
        "all Auto and explicit RGBA colors should round trip independently");
    {
        std::ifstream stream(preferences_path);
        const auto document = nlohmann::json::parse(stream);
        Require(document.contains("series_colors") && !document.contains("locked") &&
                !document.contains("source_collection_identity") && !document.contains("x_min"),
            "preference JSON must contain no viewport-owned fields");
    }
    std::filesystem::remove(preferences_path);

    Require(
        spectiary::SaveSpectrumViewportState(
            path,
            {},
            &error),
        "an unlocked shutdown should clear the persisted locked viewport");
    Require(
        !spectiary::LoadSpectrumViewportState(path)
             .state.locked,
        "the cleared viewport cache should reload in automatic mode");
    std::filesystem::remove(path);
}

void TestLegacySpectrumMigrationCompletesPartialCutover()
{
    using namespace spectiary;
    const auto root = UniqueTempPath();
    std::filesystem::create_directories(root);
    RuntimePaths paths;
    paths.legacy_spectrum_view_state_path = root / "spectrum-view-state.json";
    paths.spectrum_plot_preferences_path = root / "config" / "spectrum-plot-preferences.json";
    paths.spectrum_viewport_state_path = root / "state" / "spectrum-viewport-state.json";
    const std::string legacy = R"({"format_kind":"specforge.spectrum_view.state","schema_version":2,
        "locked":true,"source_collection_identity":"source-A",
        "x_min":"1.25","x_max":"8.5","y_min":"-2","y_max":"3",
        "series_colors":{"spectrum.raw":{"mode":"explicit-color","red":"0.25",
        "green":"0.5","blue":"0.75","alpha":"1"},
        "spectrum.smoothing.gaussian":{"mode":"auto"},
        "spectrum.smoothing.median":{"mode":"auto"}}})";
    std::ofstream(paths.legacy_spectrum_view_state_path) << legacy;
    auto preferences = LoadSpectrumPlotPreferences(paths.spectrum_plot_preferences_path);
    auto viewport = LoadSpectrumViewportState(paths.spectrum_viewport_state_path);
    // A destination becomes unwritable after the initial missing-file probe.
    std::filesystem::create_directories(paths.spectrum_viewport_state_path);
    MigrateLegacySpectrumViewState(paths, preferences, viewport);
    Require(std::filesystem::exists(paths.legacy_spectrum_view_state_path),
        "partial migration must retain the legacy input for a later startup");
    Require(preferences.document_present && !viewport.state.locked &&
            viewport.issue_kind == VersionedJsonCacheLoadIssueKind::ReadFailed &&
            LoadSpectrumPlotPreferences(paths.spectrum_plot_preferences_path).state.plot_colors.raw_spectrum ==
                PlotSeriesColor::ExplicitColor({0.25f, 0.5f, 0.75f, 1}),
        "failed migration must reset only its destination and never schedule a stale autosave");
    // A user changes the established config before the next startup.
    preferences.state.plot_colors.raw_spectrum = PlotSeriesColor::Auto();
    Require(SaveSpectrumPlotPreferences(paths.spectrum_plot_preferences_path, preferences.state),
        "established preferences should remain editable");
    std::filesystem::remove(paths.spectrum_viewport_state_path);
    preferences = LoadSpectrumPlotPreferences(paths.spectrum_plot_preferences_path);
    viewport = LoadSpectrumViewportState(paths.spectrum_viewport_state_path);
    MigrateLegacySpectrumViewState(paths, preferences, viewport);
    Require(!std::filesystem::exists(paths.legacy_spectrum_view_state_path) &&
            preferences.state.plot_colors.raw_spectrum.mode() == PlotSeriesColorMode::Auto &&
            LoadSpectrumViewportState(paths.spectrum_viewport_state_path).state == viewport.state,
        "restart must fill only the missing viewport and retire legacy after both targets exist");
    std::filesystem::remove(paths.spectrum_plot_preferences_path);
    std::filesystem::remove(paths.spectrum_viewport_state_path);
    std::ofstream(paths.legacy_spectrum_view_state_path) <<
        R"({"format_kind":"specforge.spectrum_view.state","schema_version":1,"locked":false})";
    preferences = LoadSpectrumPlotPreferences(paths.spectrum_plot_preferences_path);
    viewport = LoadSpectrumViewportState(paths.spectrum_viewport_state_path);
    MigrateLegacySpectrumViewState(paths, preferences, viewport);
    Require(preferences.document_present && viewport.document_present &&
            preferences.state.plot_colors == SpectrumPlotColors{} && !viewport.state.locked &&
            !std::filesystem::exists(paths.legacy_spectrum_view_state_path),
        "legacy schema 1 should establish independent Auto preferences and automatic viewport");
    // A normal writer can establish a destination after our initial load.
    std::filesystem::remove(paths.spectrum_plot_preferences_path);
    std::filesystem::remove(paths.spectrum_viewport_state_path);
    preferences = LoadSpectrumPlotPreferences(paths.spectrum_plot_preferences_path);
    viewport = LoadSpectrumViewportState(paths.spectrum_viewport_state_path);
    const SpectrumPlotPreferences winner{{.raw_spectrum = PlotSeriesColor::ExplicitColor({1, 0, 0, 1})}};
    Require(SaveSpectrumPlotPreferences(paths.spectrum_plot_preferences_path, winner), "publish concurrent winner");
    std::ofstream(paths.legacy_spectrum_view_state_path) << legacy;
    MigrateLegacySpectrumViewState(paths, preferences, viewport);
    Require(preferences.state.plot_colors == winner.plot_colors &&
            LoadSpectrumPlotPreferences(paths.spectrum_plot_preferences_path).state.plot_colors == winner.plot_colors,
        "migration must preserve a destination created after the initial missing-file snapshot");
    // Established split files must not decode even an unreadable legacy input.
    std::filesystem::create_directory(paths.legacy_spectrum_view_state_path);
    preferences = LoadSpectrumPlotPreferences(paths.spectrum_plot_preferences_path);
    viewport = LoadSpectrumViewportState(paths.spectrum_viewport_state_path);
    MigrateLegacySpectrumViewState(paths, preferences, viewport);
    Require(preferences.warning.empty() && viewport.warning.empty(),
        "both established owners must bypass legacy decoding entirely");
    std::filesystem::remove(paths.legacy_spectrum_view_state_path);
    // A present unsupported destination must not be replaced with legacy data.
    const std::string future = R"({"format_kind":"spectiary.spectrum_plot.preferences","schema_version":999})";
    std::ofstream(paths.spectrum_plot_preferences_path) << future;
    std::ofstream(paths.legacy_spectrum_view_state_path) << legacy;
    std::filesystem::remove(paths.spectrum_viewport_state_path);
    preferences = LoadSpectrumPlotPreferences(paths.spectrum_plot_preferences_path);
    viewport = LoadSpectrumViewportState(paths.spectrum_viewport_state_path);
    MigrateLegacySpectrumViewState(paths, preferences, viewport);
    {
        std::ifstream stream(paths.spectrum_plot_preferences_path);
        Require(std::string((std::istreambuf_iterator<char>(stream)), {}) == future &&
                preferences.issue_kind == VersionedJsonCacheLoadIssueKind::UnsupportedFormatOrSchema &&
                viewport.document_present && viewport.state.locked &&
                std::filesystem::exists(paths.legacy_spectrum_view_state_path),
            "migration must preserve protected split config while establishing the missing viewport");
    }
    std::filesystem::remove_all(root);
}

void TestSpectrumColorCacheSupportsLegacyAndDamagedEntries()
{
    const std::filesystem::path path = UniqueTempPath();
    {
        std::ofstream stream(
            path,
            std::ios::binary | std::ios::trunc);
        stream <<
            "{\"format_kind\":\"specforge.spectrum_view.state\","
            "\"schema_version\":1,\"locked\":false}\n";
    }
    const spectiary::LegacySpectrumViewStateLoadResult legacy =
        spectiary::LoadLegacySpectrumViewState(path);
    Require(
        legacy.warning.empty() &&
            legacy.issue_kind == spectiary::VersionedJsonCacheLoadIssueKind::None &&
            legacy.state.plot_colors ==
                spectiary::SpectrumPlotColors{},
        "schema 1 viewport state should migrate to canonical Auto colors without a warning");

    {
        std::ofstream stream(
            path,
            std::ios::binary | std::ios::trunc);
        stream <<
            "{\"format_kind\":\"spectiary.spectrum_plot.preferences\","
            "\"schema_version\":1,"
            "\"series_colors\":{"
            "\"spectrum.raw\":{\"mode\":\"explicit-color\","
            "\"red\":\"0.1\",\"green\":\"0.2\","
            "\"blue\":\"0.3\",\"alpha\":\"damaged\"},"
            "\"spectrum.smoothing.gaussian\":{\"mode\":\"auto\"},"
            "\"spectrum.smoothing.median\":{\"mode\":\"explicit-color\","
            "\"red\":\"0.4\",\"green\":\"0.5\","
            "\"blue\":\"0.6\",\"alpha\":\"0.7\"}}}\n";
    }
    const spectiary::SpectrumPlotPreferencesLoadResult damaged =
        spectiary::LoadSpectrumPlotPreferences(path);
    const std::optional<spectiary::RgbaColor>& median =
        damaged.state.plot_colors.median_smoothing.
            explicit_color();
    Require(
        !damaged.warning.empty() &&
            damaged.issue_kind == spectiary::VersionedJsonCacheLoadIssueKind::None &&
            damaged.state.plot_colors.raw_spectrum.mode() ==
                spectiary::PlotSeriesColorMode::Auto &&
            damaged.state.plot_colors.gaussian_smoothing.mode() ==
                spectiary::PlotSeriesColorMode::Auto &&
            median && median->red == 0.4f &&
            median->green == 0.5f &&
            median->blue == 0.6f &&
            median->alpha == 0.7f,
        "one damaged color should warn and fall back independently without discarding valid curve colors");
    std::filesystem::remove(path);
}

void TestSpectrumViewSessionOwnsCustomCurveColors()
{
    spectiary::SpectrumViewSession automatic;
    const spectiary::SemanticPalette& palette =
        spectiary::FindBuiltInThemeDescriptor(
            spectiary::BuiltInDarkThemeId())
            ->palette;
    const ImVec4 raw = automatic.ResolveSeriesColor(
        spectiary::SpectrumPlotSeries::RawSpectrum,
        palette);
    const ImVec4 gaussian = automatic.ResolveSeriesColor(
        spectiary::SpectrumPlotSeries::GaussianSmoothing,
        palette);
    const ImVec4 median = automatic.ResolveSeriesColor(
        spectiary::SpectrumPlotSeries::MedianSmoothing,
        palette);
    Require(
        raw.x == palette.plot_auto_series[0].x &&
            gaussian.x == palette.plot_auto_series[1].x &&
            median.x == palette.plot_auto_series[2].x,
        "built-in curves should reserve distinct Auto slots in their stable rendering order");

    spectiary::SpectrumViewSession session;
    spectiary::SpectrumPlotColors colors;
    colors.raw_spectrum =
        spectiary::PlotSeriesColor::ExplicitColor({
            .red = 0.11f,
            .green = 0.22f,
            .blue = 0.33f,
            .alpha = 0.44f,
        });
    session.Submit(
        spectiary::SpectrumViewSessionCommand::SetPlotColors(
            colors));
    session.Submit(
        spectiary::SpectrumViewSessionCommand::SetPlotSeriesColor(
            spectiary::SpectrumPlotSeries::GaussianSmoothing,
            spectiary::PlotSeriesColor::ExplicitColor({
                .red = 0.55f,
                .green = 0.66f,
                .blue = 0.77f,
                .alpha = 0.88f,
            })));
    session.Submit(
        spectiary::SpectrumViewSessionCommand::SetPlotSeriesColor(
            spectiary::SpectrumPlotSeries::MedianSmoothing,
            spectiary::PlotSeriesColor::Auto()));

    const spectiary::SpectrumPlotColors expected =
        session.View().plot_colors;
    Require(
        expected.raw_spectrum == colors.raw_spectrum &&
            expected.gaussian_smoothing.mode() ==
                spectiary::PlotSeriesColorMode::ExplicitColor &&
            expected.median_smoothing.mode() ==
                spectiary::PlotSeriesColorMode::Auto,
        "series color commands should target the requested raw or smoothing curve without index mapping");

    session.Submit(
        spectiary::SpectrumViewSessionCommand::ResetSmoothing());
    session.Submit(
        spectiary::SpectrumViewSessionCommand::ApplySnapshotChange(
            spectiary::SourceCollectionSnapshotChangeReason::
                SourceCollectionChanged));
    Require(
        session.View().plot_colors == expected,
        "smoothing resets and source changes should preserve global curve color preferences");
}

void TestSpectrumViewSessionCapturesAndRestoresLockedLimits()
{
    spectiary::SpectrumViewSession session;
    const spectiary::PlotViewLimits expected{
        .x_min = 12.5,
        .x_max = 18.75,
        .y_min = -4.0,
        .y_max = 9.5,
    };
    Require(
        session.RestoreLockedViewport(expected),
        "valid persisted limits should restore");
    const std::optional<spectiary::PlotViewLimits> restored =
        session.LockedViewportLimits();
    Require(
        session.View().viewport_range_mode ==
                spectiary::SpectrumViewportRangeMode::Locked &&
            restored && restored->x_min == expected.x_min &&
            restored->x_max == expected.x_max &&
            restored->y_min == expected.y_min &&
            restored->y_max == expected.y_max,
        "restored limits should become the logical view's locked viewport");

    session.Submit(
        spectiary::SpectrumViewSessionCommand::ApplySnapshotChange(
            spectiary::SourceCollectionSnapshotChangeReason::
                SourceCollectionChanged));
    Require(
        session.View().viewport_range_mode ==
                spectiary::SpectrumViewportRangeMode::Automatic &&
            !session.LockedViewportLimits(),
        "a later collection change should still unlock a restored viewport");
}

void TestIndependentSpectrumViewsDoNotShareViewportLock()
{
    spectiary::SpectrumViewSession first;
    spectiary::SpectrumViewSession second;
    first.Submit(
        spectiary::SpectrumViewSessionCommand::SetViewportRangeMode(
            spectiary::SpectrumViewportRangeMode::Locked));

    Require(
        first.View().viewport_range_mode == spectiary::SpectrumViewportRangeMode::Locked,
        "the owning spectrum view should observe its viewport lock");
    Require(
        second.View().viewport_range_mode == spectiary::SpectrumViewportRangeMode::Automatic,
        "independent spectrum views must not share viewport lock state");
}

void TestSourceRosterClassifiesSnapshotChanges()
{
    spectiary::SourceCollectionRoster roster;
    const std::filesystem::path first_path = "viewport-source-a.csv";
    const std::filesystem::path second_path = "viewport-source-b.csv";
    spectiary::SourceCollectionContextReuseProof first_proof;
    first_proof.identity.id = "viewport-collection-a";
    spectiary::SourceCollectionContextReuseProof second_proof;
    second_proof.identity.id = "viewport-collection-b";

    const spectiary::SourceCollectionRosterOpenResult first =
        roster.OpenPreparedSource(
            first_path,
            0,
            MakeActivationSnapshot(first_path, 0),
            {},
            first_proof);
    Require(
        first.action.snapshot_change_reason ==
            spectiary::SourceCollectionSnapshotChangeReason::SourceCollectionChanged,
        "opening the first source should be classified as a collection change");
    Require(first.action.source_roster_changed, "adding a Files source must notify shell projections");

    const spectiary::SourceCollectionRosterOpenResult next_sample =
        roster.OpenPreparedSource(
            first_path,
            1,
            MakeActivationSnapshot(first_path, 1),
            {},
            first_proof);
    Require(
        next_sample.action.snapshot_change_reason ==
            spectiary::SourceCollectionSnapshotChangeReason::SampleChangedWithinCollection,
        "loading another sample with the same collection identity should preserve its scope");
    Require(!next_sample.action.source_roster_changed, "sample navigation must not republish the Files roster");

    const spectiary::SpectrumSnapshotHandle current_snapshot =
        roster.snapshot();
    const spectiary::SourceCollectionRosterOpenResult unchanged =
        roster.OpenPreparedSource(
            first_path,
            1,
            current_snapshot,
            {},
            first_proof);
    Require(
        !unchanged.action.snapshot_changed &&
            unchanged.action.snapshot_change_reason ==
                spectiary::SourceCollectionSnapshotChangeReason::None,
        "re-presenting the same snapshot handle and index should be a no-op");
    Require(!unchanged.action.source_roster_changed, "unchanged source content must not republish the roster");

    const spectiary::SourceCollectionRosterOpenResult reloaded =
        roster.OpenPreparedSource(
            first_path,
            1,
            MakeActivationSnapshot(first_path, 1),
            {},
            first_proof);
    Require(
        reloaded.action.snapshot_change_reason ==
            spectiary::SourceCollectionSnapshotChangeReason::SnapshotReloadedWithinCollection,
        "replacing the current sample should retain an explicit reload reason");

    roster.RememberActiveSourceIndex(2);
    const spectiary::SourceCollectionRosterOpenResult corrective_follow_up =
        roster.OpenPreparedSource(
            first_path,
            2,
            MakeActivationSnapshot(first_path, 2),
            {},
            first_proof);
    Require(
        corrective_follow_up.action.snapshot_change_reason ==
            spectiary::SourceCollectionSnapshotChangeReason::SampleChangedWithinCollection,
        "a corrective follow-up should compare against the displayed snapshot index, not the roster target");

    const spectiary::SourceCollectionRosterOpenResult second =
        roster.OpenPreparedSource(
            second_path,
            0,
            MakeActivationSnapshot(second_path, 0),
            {},
            second_proof);
    Require(
        second.action.snapshot_change_reason ==
            spectiary::SourceCollectionSnapshotChangeReason::SourceCollectionChanged,
        "opening a different collection identity should be classified as a collection change");

    const spectiary::SourceCollectionSessionAction reactivated =
        roster.ActivateSource(0);
    Require(
        reactivated.snapshot_change_reason ==
            spectiary::SourceCollectionSnapshotChangeReason::SourceCollectionChanged,
        "reactivating another roster source should be classified as a collection change");

    const spectiary::SourceCollectionSessionAction unchanged_activation =
        roster.ActivateSource(0);
    Require(
        !unchanged_activation.snapshot_changed &&
            unchanged_activation.snapshot_change_reason ==
                spectiary::SourceCollectionSnapshotChangeReason::None,
        "activating the current roster source should be a no-op");
}

void TestSnapshotResetPreservesControlsAndFitsNewData()
{
    ScopedPlotUi ui;
    spectiary::SpectrumViewSession session;
    ConfigureGaussianSmoothing(session);

    const spectiary::SpectrumViewRenderFeedback first =
        ui.RenderFrame(session, MakeSnapshot({1.0, 2.0, 3.0}, {2.0, 4.0, 3.0}));
    Require(
        first.plot_submitted && first.fit_applied && first.visible_limits,
        "first render should submit and fit the spectrum");
    std::vector<spectiary::SpectrumValueVector> retained =
        session.RetainHeavySnapshotResources();
    Require(
        retained.size() == 2,
        "smoothed render should expose both heavy cache resources for retention");

    session.Submit(
        spectiary::SpectrumViewSessionCommand::ApplySnapshotChange(
            spectiary::SourceCollectionSnapshotChangeReason::SampleChangedWithinCollection));

    const spectiary::SpectrumViewSessionView view = session.View();
    Require(view.show_points, "snapshot reset should preserve show-points state");
    Require(!view.show_raw_curve, "snapshot reset should preserve raw-curve visibility");
    Require(
        view.show_gaussian_smoothed && !view.show_median_smoothed,
        "snapshot reset should preserve independent smoothing visibility");
    RequireNear(
        view.smoothing_parameters.gaussian_sigma,
        3.25,
        "snapshot reset should preserve gaussian sigma");
    Require(!session.PlotPanActive(), "snapshot reset should clear plot interaction feedback");
    Require(
        session.RetainHeavySnapshotResources().empty(),
        "snapshot reset should release current smoothing caches");
    Require(retained[0] && retained[1], "retained handles should keep retired resources alive");

    const spectiary::SpectrumViewRenderFeedback second =
        ui.RenderFrame(session, MakeSnapshot({100.0, 200.0, 300.0}, {5.0, 6.0, 7.0}));
    Require(
        second.plot_submitted && second.fit_applied && second.visible_limits,
        "the first render after a snapshot reset should fit new data");
    Require(
        second.visible_limits->x_min > first.visible_limits->x_max,
        "snapshot reset should discard old plot limits");
}

void TestCoincidentWavelengthsFitOnActivation()
{
    ScopedPlotUi ui;
    for (const bool immersive : {false, true}) {
        for (const double wavelength : {5000.0, 1.0e20}) {
            for (const std::size_t count : {1u, 3u}) {
                spectiary::SpectrumViewSession session;
                session.Submit(spectiary::SpectrumViewSessionCommand::SetShowPoints(true));
                const auto snapshot = MakeSnapshot(
                    std::vector<double>(count, wavelength), std::vector<double>(count, 3.0));
                spectiary::SpectrumPlotDisplayOptions display;
                display.native_transparent_axes = immersive;
                display.include_edge_pixels = immersive;
                const auto inspect = [&](ImPlotPlot& plot) {
                    for (const ImAxis index : {ImAxis_X1, ImAxis_Y1}) {
                        const auto& axis = plot.Axes[index];
                        const double value = index == ImAxis_X1 ? wavelength : 3.0;
                        Require(std::isfinite(axis.Range.Min) && std::isfinite(axis.Range.Max) &&
                            axis.Range.Min < value && value < axis.Range.Max,
                            "coincident data must lie inside finite nondegenerate fit bounds");
                        Require(axis.Ticker.TickCount() > 1 && axis.Ticker.TickCount() < 1000,
                            "coincident data must produce useful bounded axis ticks");
                        if (index == ImAxis_X1 && wavelength == 5000.0) {
                            RequireNear(axis.Range.Min, 4999.5,
                                "single-wavelength fit should provide a readable lower bound");
                            RequireNear(axis.Range.Max, 5000.5,
                                "single-wavelength fit should provide a readable upper bound");
                        }
                        const float pixel = axis.PlotToPixels(value);
                        Require(std::isfinite(pixel) &&
                            pixel > std::min(axis.PixelMin, axis.PixelMax) &&
                            pixel < std::max(axis.PixelMin, axis.PixelMax),
                            "data point must map inside the visible plot");
                    }
                };
                const auto render = [&]() {
                    const auto feedback = ui.RenderFrame(session, snapshot,
                        ImVec2(-100, -100), false, nullptr, display, 0, false,
                        ImVec2(800, 600), inspect);
                    Require(feedback.plot_submitted && feedback.visible_limits,
                        "coincident wavelengths must present a usable plot");
                    return feedback;
                };
                Require(render().fit_applied, "initial activation must fit coincident wavelengths");
                render();
                session.Submit(spectiary::SpectrumViewSessionCommand::ApplySnapshotChange(
                    spectiary::SourceCollectionSnapshotChangeReason::SourceCollectionChanged));
                ui.RenderFrame(session, MakeSnapshot({1.0, 2.0}, {1.0, 2.0}));
                session.Submit(spectiary::SpectrumViewSessionCommand::ApplySnapshotChange(
                    spectiary::SourceCollectionSnapshotChangeReason::SourceCollectionChanged));
                Require(render().fit_applied, "reactivation must fit coincident wavelengths");
                session.Submit(spectiary::SpectrumViewSessionCommand::RequestFitView());
                Require(render().fit_applied, "explicit fit must retain usable bounds");
                Require(snapshot->current_spectrum.x_values->front() == wavelength &&
                    snapshot->current_spectrum.y_values->front() == 3.0 &&
                    snapshot->current_spectrum.point_count == count,
                    "fit must preserve original spectrum values and point count");
            }
        }
    }
}

void TestHiddenCurvesStillReportPresentedPlotFrame()
{
    ScopedPlotUi ui;
    spectiary::SpectrumViewSession session;
    const spectiary::SpectrumSnapshotHandle snapshot =
        MakeSnapshot({1.0, 2.0, 3.0}, {2.0, 4.0, 3.0});

    session.Submit(
        spectiary::SpectrumViewSessionCommand::SetShowRawCurve(false));
    session.Submit(
        spectiary::SpectrumViewSessionCommand::SetShowPoints(false));
    session.Submit(
        spectiary::SpectrumViewSessionCommand::SetShowGaussianSmoothed(false));
    session.Submit(
        spectiary::SpectrumViewSessionCommand::SetShowMedianSmoothed(false));

    const spectiary::SpectrumViewRenderFeedback feedback =
        ui.RenderFrame(session, snapshot);
    Require(
        feedback.plot_submitted,
        "a valid plot frame should count as presented when every curve is hidden");
    Require(
        feedback.fit_applied && feedback.visible_limits,
        "a curve-free plot frame should still complete its initial fit");
}

void TestNativeYTicksSurviveImmersiveModeTransitions()
{
    ScopedPlotUi ui;
    for (const double scale : {0.001, 1.0, 1.0e12}) {
        spectiary::SpectrumViewSession session;
        const auto snapshot = MakeSnapshot(
            {1.0, 2.0, 3.0}, {-2.0 * scale, scale, 3.0 * scale});
        std::optional<spectiary::PlotViewLimits> previous_limits;
        for (const bool immersive : {false, true, false, true}) {
            spectiary::SpectrumPlotDisplayOptions display;
            display.native_transparent_axes = immersive;
            display.include_edge_pixels = immersive;
            if (previous_limits) {
                session.Submit(spectiary::SpectrumViewSessionCommand::SyncPlotLimitsOnNextRender());
            }
            const auto feedback = ui.RenderFrame(session, snapshot,
                ImVec2(-100.0f, -100.0f), false, nullptr, display, 0.0f, true);
            Require(feedback.plot_submitted && feedback.visible_limits,
                "each axis mode must present the spectrum");
            if (previous_limits) {
                RequireNear(feedback.visible_limits->x_min, previous_limits->x_min,
                    "axis mode switch preserves x minimum");
                RequireNear(feedback.visible_limits->x_max, previous_limits->x_max,
                    "axis mode switch preserves x maximum");
                RequireNear(feedback.visible_limits->y_min, previous_limits->y_min,
                    "axis mode switch preserves y minimum");
                RequireNear(feedback.visible_limits->y_max, previous_limits->y_max,
                    "axis mode switch preserves y maximum");
            }
            previous_limits = feedback.visible_limits;
        }
    }
}

void TestNearlyCoincidentAxisEndpointsRemainResponsive()
{
    ScopedPlotUi ui;
    const auto snapshot = MakeSnapshot({4000.0, 5000.0, 6000.0}, {1.0, 3.0, 2.0});
    for (const double center : {2.6, -2.6, 5000.0, 1e-20, -1e-20, 1e-200, 1e200, 0.0}) {
        for (const bool immersive : {false, true}) {
            spectiary::SpectrumViewSession session;
            // Exercise axes without out-of-range curve coordinates at extreme scales.
            session.Submit(spectiary::SpectrumViewSessionCommand::SetShowRawCurve(false));
            const double lo = std::nextafter(center, -std::numeric_limits<double>::infinity());
            Require(session.RestoreLockedViewport({lo, center, lo, center}),
                "distinct finite endpoints must be restorable");
            spectiary::SpectrumPlotDisplayOptions display;
            display.native_transparent_axes = immersive;
            display.include_edge_pixels = immersive;
            const auto check_ticks = [](ImPlotPlot& plot) {
                for (const ImAxis index : {ImAxis_X1, ImAxis_Y1}) {
                    const auto& axis = plot.Axes[index];
                    Require(axis.Range.Min < axis.Range.Max &&
                        std::isfinite(axis.Range.Min) && std::isfinite(axis.Range.Max),
                        "protected range must be finite and ordered");
                    Require(axis.Ticker.TickCount() > 1 && axis.Ticker.TickCount() < 1000,
                        "nearly coincident limits must produce bounded useful ticks");
                }
            };
            auto render = [&](const std::function<void(ImPlotPlot&)>& inspect) {
                return ui.RenderFrame(session, snapshot, ImVec2(-100, -100), false,
                    nullptr, display, 0, false, ImVec2(1800, 1400), inspect);
            };
            const auto restored = render(check_ticks);
            Require(restored.visible_limits.has_value(), "narrow restored viewport must render");
            Require(restored.visible_limits->x_max - restored.visible_limits->x_min > center - lo &&
                restored.visible_limits->y_max - restored.visible_limits->y_min > center - lo,
                "unrepresentable tick intervals must expand before rendering");
            // Invoke precisely the setters used by ImPlot's axis menu, then render
            // the following frame. Internal API access stays confined to tests.
            render([&](ImPlotPlot& plot) {
                check_ticks(plot);
                for (const ImAxis index : {ImAxis_X1, ImAxis_Y1}) {
                    auto& axis = plot.Axes[index];
                    axis.SetMin(std::nextafter(axis.Range.Max, axis.Range.Min), true);
                }
            });
            render(check_ticks);
            render([&](ImPlotPlot& plot) {
                for (const ImAxis index : {ImAxis_X1, ImAxis_Y1}) {
                    auto& axis = plot.Axes[index];
                    axis.SetMax(std::nextafter(axis.Range.Min, axis.Range.Max), true);
                }
            });
            render(check_ticks);
        }
    }
    // Small flux units must not inherit an absolute epsilon or stale large-scale
    // constraint from the previous viewport.
    spectiary::SpectrumViewSession session;
    const spectiary::PlotViewLimits ordinary{4000, 6000, -2e-20, 3e-20};
    Require(session.RestoreLockedViewport(ordinary), "small flux viewport must restore");
    const auto normal = ui.RenderFrame(session, snapshot);
    Require(normal.visible_limits && normal.visible_limits->x_min == ordinary.x_min &&
        normal.visible_limits->x_max == ordinary.x_max &&
        normal.visible_limits->y_min == ordinary.y_min && normal.visible_limits->y_max == ordinary.y_max,
        "ordinary ranges including small flux units must remain unchanged");

    for (const bool points : {false, true}) {
        spectiary::SpectrumViewSession original;
        original.Submit(spectiary::SpectrumViewSessionCommand::SetShowPoints(points));
        auto render = [&](const std::function<void(ImPlotPlot&)>& inspect) {
            return ui.RenderFrame(original, snapshot, ImVec2(-100, -100), false,
                nullptr, {}, 0, false, ImVec2(1800, 1400), inspect);
        };
        render([](ImPlotPlot& plot) {
            auto& y = plot.Axes[ImAxis_Y1];
            y.SetMin(1.77, true);
            y.SetMax(2.6, true);
        });
        render([](ImPlotPlot& plot) {
            auto& y = plot.Axes[ImAxis_Y1];
            y.SetMin(y.Range.Max - std::numeric_limits<double>::epsilon(), true);
        });
        const auto result = render([](ImPlotPlot& plot) {
            Require(plot.Axes[ImAxis_Y1].Ticker.TickCount() < 1000,
                "issue 132 menu sequence must not produce unbounded ticks");
        });
        Require(result.visible_limits && result.visible_limits->y_max == 2.6 &&
            result.visible_limits->y_min < 2.6 && result.visible_limits->y_min > 2.59,
            "menu must stop near the other endpoint without moving that endpoint");
        original.Submit(spectiary::SpectrumViewSessionCommand::RequestFitView());
        const auto fitted = render({});
        Require(fitted.fit_applied && fitted.visible_limits &&
            fitted.visible_limits->y_min < 1 && fitted.visible_limits->y_max > 3,
            "fit must still recover the full spectrum after the boundary drag");
    }
}

void TestFitAndStoredLimitReuseAreObservable()
{
    ScopedPlotUi ui;
    spectiary::SpectrumViewSession session;
    const spectiary::SpectrumSnapshotHandle snapshot =
        MakeSnapshot({4100.0, 4500.0, 4900.0}, {-0.25, 1.75, 0.5});

    session.Submit(spectiary::SpectrumViewSessionCommand::SyncPlotLimitsOnNextRender());
    const spectiary::SpectrumViewRenderFeedback first = ui.RenderFrame(session, snapshot);
    Require(
        first.fit_applied && !first.stored_limits_reused && first.visible_limits,
        "limit sync before the first render should remain a no-op");

    session.Submit(spectiary::SpectrumViewSessionCommand::SyncPlotLimitsOnNextRender());
    const spectiary::SpectrumViewRenderFeedback reused = ui.RenderFrame(session, snapshot);
    Require(
        reused.stored_limits_reused && !reused.fit_applied && reused.visible_limits,
        "mode-switch sync should reuse stored limits");
    RequireNear(
        reused.visible_limits->x_min,
        first.visible_limits->x_min,
        "stored limit sync should preserve x min");
    RequireNear(
        reused.visible_limits->x_max,
        first.visible_limits->x_max,
        "stored limit sync should preserve x max");
    RequireNear(
        reused.visible_limits->y_min,
        first.visible_limits->y_min,
        "stored limit sync should preserve y min");
    RequireNear(
        reused.visible_limits->y_max,
        first.visible_limits->y_max,
        "stored limit sync should preserve y max");

    session.Submit(spectiary::SpectrumViewSessionCommand::RequestFitView());
    const spectiary::SpectrumViewRenderFeedback fitted = ui.RenderFrame(session, snapshot);
    Require(
        fitted.fit_applied && !fitted.stored_limits_reused,
        "fit command should supersede stored-limit reuse");
}

void TestViewportLockOverlayTogglesInAxisCorner()
{
    ScopedPlotUi ui;
    spectiary::SpectrumViewSession session;
    QueuedTouchpadGestureSource touchpad_gestures;
    const spectiary::SpectrumSnapshotHandle snapshot =
        MakeSnapshot({4100.0, 4500.0, 4900.0}, {-0.25, 1.75, 0.5});

    Require(
        ui.RenderFrame(
              session,
              snapshot,
              ImVec2(400.0f, 300.0f),
              false,
              &touchpad_gestures)
            .plot_submitted,
        "viewport lock overlay test should render a plot");
    const ImVec2 lock_center = touchpad_gestures.ViewportLockCenter();
    (void)ui.RenderFrame(session, snapshot, lock_center, false);
    (void)ui.RenderFrame(session, snapshot, lock_center, true);
    (void)ui.RenderFrame(session, snapshot, lock_center, false);
    Require(
        session.View().viewport_range_mode ==
            spectiary::SpectrumViewportRangeMode::Locked,
        "clicking the axis-corner overlay should lock the viewport");

    (void)ui.RenderFrame(session, snapshot, lock_center, true);
    (void)ui.RenderFrame(session, snapshot, lock_center, false);
    Require(
        session.View().viewport_range_mode ==
            spectiary::SpectrumViewportRangeMode::Automatic,
        "clicking the axis-corner overlay again should unlock the viewport");
}

void TestViewportLockConsumesImmersiveAxisAndTouchpadInput()
{
    ScopedPlotUi ui;
    spectiary::SpectrumViewSession session;
    QueuedTouchpadGestureSource touchpad_gestures;
    const spectiary::SpectrumSnapshotHandle snapshot =
        MakeSnapshot({0.0, 50.0, 100.0}, {-10.0, 0.0, 10.0});
    const spectiary::SpectrumPlotDisplayOptions immersive{
        .edge_axis_overlay = true,
        .include_edge_pixels = true,
    };

    const spectiary::SpectrumViewRenderFeedback initial =
        ui.RenderFrame(
            session,
            snapshot,
            ImVec2(400.0f, 300.0f),
            false,
            &touchpad_gestures,
            immersive);
    Require(
        initial.visible_limits.has_value(),
        "immersive wheel regression should establish initial plot limits");
    const ImVec2 lock_center =
        touchpad_gestures.ViewportLockCenter();
    const spectiary::SpectrumViewRenderFeedback wheel_over_lock =
        ui.RenderFrame(
            session,
            snapshot,
            lock_center,
            false,
            &touchpad_gestures,
            immersive,
            1.0f);
    Require(
        wheel_over_lock.visible_limits.has_value(),
        "immersive wheel regression should retain visible limits");
    RequireNear(
        wheel_over_lock.visible_limits->x_min,
        initial.visible_limits->x_min,
        "Keep View hit rect should consume immersive x-axis wheel min");
    RequireNear(
        wheel_over_lock.visible_limits->x_max,
        initial.visible_limits->x_max,
        "Keep View hit rect should consume immersive x-axis wheel max");

    touchpad_gestures.QueuePanAt(lock_center, 40.0f, 30.0f);
    const spectiary::SpectrumViewRenderFeedback pan_over_lock =
        ui.RenderFrame(
            session,
            snapshot,
            lock_center,
            false,
            &touchpad_gestures,
            immersive);
    Require(
        pan_over_lock.visible_limits.has_value(),
        "immersive touchpad pan regression should retain visible limits");
    RequireNear(
        pan_over_lock.visible_limits->x_min,
        initial.visible_limits->x_min,
        "Keep View hit rect should consume immersive touchpad pan x min");
    RequireNear(
        pan_over_lock.visible_limits->x_max,
        initial.visible_limits->x_max,
        "Keep View hit rect should consume immersive touchpad pan x max");
    RequireNear(
        pan_over_lock.visible_limits->y_min,
        initial.visible_limits->y_min,
        "Keep View hit rect should consume immersive touchpad pan y min");
    RequireNear(
        pan_over_lock.visible_limits->y_max,
        initial.visible_limits->y_max,
        "Keep View hit rect should consume immersive touchpad pan y max");

    touchpad_gestures.QueueZoomAt(lock_center, 2.0);
    const spectiary::SpectrumViewRenderFeedback pinch_over_lock =
        ui.RenderFrame(
            session,
            snapshot,
            lock_center,
            false,
            &touchpad_gestures,
            immersive);
    Require(
        pinch_over_lock.visible_limits.has_value(),
        "immersive touchpad pinch regression should retain visible limits");
    RequireNear(
        pinch_over_lock.visible_limits->x_min,
        initial.visible_limits->x_min,
        "Keep View hit rect should consume immersive touchpad pinch x min");
    RequireNear(
        pinch_over_lock.visible_limits->x_max,
        initial.visible_limits->x_max,
        "Keep View hit rect should consume immersive touchpad pinch x max");
    RequireNear(
        pinch_over_lock.visible_limits->y_min,
        initial.visible_limits->y_min,
        "Keep View hit rect should consume immersive touchpad pinch y min");
    RequireNear(
        pinch_over_lock.visible_limits->y_max,
        initial.visible_limits->y_max,
        "Keep View hit rect should consume immersive touchpad pinch y max");
}

void TestViewportLockPreservesAdjustedLimitsAndUnlockRestoresFit()
{
    ScopedPlotUi ui;
    spectiary::SpectrumViewSession session;
    QueuedTouchpadGestureSource touchpad_gestures;
    const spectiary::SpectrumSnapshotHandle first_snapshot =
        MakeSnapshot({0.0, 50.0, 100.0}, {-10.0, 0.0, 10.0});

    const spectiary::SpectrumViewRenderFeedback initial =
        ui.RenderFrame(
            session,
            first_snapshot,
            ImVec2(400.0f, 300.0f),
            false,
            &touchpad_gestures);
    Require(
        initial.fit_applied && initial.visible_limits,
        "initial unlocked render should fit the first sample");
    Require(
        session.ViewportMutationRevision() == 0,
        "automatic initial fit should not count as a user viewport mutation");

    session.Submit(
        spectiary::SpectrumViewSessionCommand::SetViewportRangeMode(
            spectiary::SpectrumViewportRangeMode::Locked));
    Require(
        session.View().viewport_range_mode ==
            spectiary::SpectrumViewportRangeMode::Locked,
        "viewport lock mode should be observable");
    const std::uint64_t locked_revision =
        session.ViewportMutationRevision();

    touchpad_gestures.QueueZoom(2.0);
    const spectiary::SpectrumViewRenderFeedback adjusted =
        ui.RenderFrame(
            session,
            first_snapshot,
            ImVec2(400.0f, 300.0f),
            false,
            &touchpad_gestures);
    Require(
        adjusted.visible_limits &&
            adjusted.visible_limits->x_max - adjusted.visible_limits->x_min <
                initial.visible_limits->x_max - initial.visible_limits->x_min,
        "viewport lock should not prevent plot navigation from adjusting the visible range");
    Require(
        session.ViewportMutationRevision() >
            locked_revision,
        "user viewport navigation should advance the mutation revision");

    session.Submit(
        spectiary::SpectrumViewSessionCommand::ApplySnapshotChange(
            spectiary::SourceCollectionSnapshotChangeReason::SampleChangedWithinCollection));
    const spectiary::SpectrumViewRenderFeedback second =
        ui.RenderFrame(
            session,
            MakeSnapshot({1000.0, 1500.0, 2000.0}, {50.0, 75.0, 100.0}));
    Require(
        second.stored_limits_reused && !second.fit_applied && second.visible_limits,
        "locked sample change should reuse the adjusted viewport");
    RequireNear(second.visible_limits->x_min, adjusted.visible_limits->x_min, "locked x min");
    RequireNear(second.visible_limits->x_max, adjusted.visible_limits->x_max, "locked x max");
    RequireNear(second.visible_limits->y_min, adjusted.visible_limits->y_min, "locked y min");
    RequireNear(second.visible_limits->y_max, adjusted.visible_limits->y_max, "locked y max");

    session.Submit(
        spectiary::SpectrumViewSessionCommand::ApplySnapshotChange(
            spectiary::SourceCollectionSnapshotChangeReason::SampleChangedWithinCollection));
    const spectiary::SpectrumViewRenderFeedback third =
        ui.RenderFrame(
            session,
            MakeSnapshot({3000.0, 3500.0, 4000.0}, {-200.0, 0.0, 200.0}));
    Require(
        third.stored_limits_reused && !third.fit_applied && third.visible_limits,
        "consecutive locked sample changes should keep reusing the viewport");
    RequireNear(third.visible_limits->x_min, adjusted.visible_limits->x_min, "stable locked x min");
    RequireNear(third.visible_limits->x_max, adjusted.visible_limits->x_max, "stable locked x max");
    RequireNear(third.visible_limits->y_min, adjusted.visible_limits->y_min, "stable locked y min");
    RequireNear(third.visible_limits->y_max, adjusted.visible_limits->y_max, "stable locked y max");

    session.Submit(
        spectiary::SpectrumViewSessionCommand::SetViewportRangeMode(
            spectiary::SpectrumViewportRangeMode::Automatic));
    session.Submit(
        spectiary::SpectrumViewSessionCommand::ApplySnapshotChange(
            spectiary::SourceCollectionSnapshotChangeReason::SampleChangedWithinCollection));
    const spectiary::SpectrumViewRenderFeedback unlocked =
        ui.RenderFrame(
            session,
            MakeSnapshot({5000.0, 5500.0, 6000.0}, {500.0, 600.0, 700.0}));
    Require(
        unlocked.fit_applied && !unlocked.stored_limits_reused && unlocked.visible_limits,
        "unlocked sample change should restore automatic fitting");
    Require(
        unlocked.visible_limits->x_min > adjusted.visible_limits->x_max,
        "unlocked fit should use the new sample bounds");
}

void TestSmoothingCommandsOwnCacheInvalidation()
{
    ScopedPlotUi ui;
    spectiary::SpectrumViewSession session;
    const spectiary::SpectrumSnapshotHandle snapshot =
        MakeSnapshot({1.0, 2.0, 3.0, 4.0, 5.0}, {2.0, 4.0, 3.0, 5.0, 1.0});
    session.Submit(spectiary::SpectrumViewSessionCommand::SetShowMedianSmoothed(true));
    session.Submit(spectiary::SpectrumViewSessionCommand::SetMedianKernelSize(4));

    Require(
        session.View().smoothing_parameters.median_kernel_size == 5,
        "median kernel command should normalize to an odd size");
    Require(
        session.EffectiveMedianKernelSize(4) == 3,
        "effective median kernel should clamp to data length");
    Require(ui.RenderFrame(session, snapshot).plot_submitted, "smoothed spectrum should render");
    Require(
        session.RetainHeavySnapshotResources().size() == 2,
        "render should populate smoothing resources");

    session.Submit(spectiary::SpectrumViewSessionCommand::SetMedianKernelSize(9));
    Require(
        session.RetainHeavySnapshotResources().empty(),
        "changing the median kernel should clear smoothing resources");
    Require(
        session.View().smoothing_parameters.median_kernel_size == 9,
        "normalized median kernel should be observable");
    Require(ui.RenderFrame(session, snapshot).plot_submitted, "smoothing should rebuild");
    Require(
        session.RetainHeavySnapshotResources().size() == 2,
        "render should rebuild smoothing resources");

    session.Submit(spectiary::SpectrumViewSessionCommand::SetShowGaussianSmoothed(true));
    Require(
        ui.RenderFrame(session, snapshot).plot_submitted,
        "gaussian and median smoothing should render together");
    const std::vector<spectiary::SpectrumValueVector> both_smoothing_resources =
        session.RetainHeavySnapshotResources();
    Require(
        both_smoothing_resources.size() == 3,
        "simultaneous smoothing should retain one source and both smoothed curves");

    session.Submit(spectiary::SpectrumViewSessionCommand::SetGaussianSigma(4.0));
    Require(ui.RenderFrame(session, snapshot).plot_submitted, "gaussian smoothing should rebuild");
    const std::vector<spectiary::SpectrumValueVector> gaussian_changed_resources =
        session.RetainHeavySnapshotResources();
    Require(
        gaussian_changed_resources.size() == 3 &&
            gaussian_changed_resources[1] != both_smoothing_resources[1] &&
            gaussian_changed_resources[2] == both_smoothing_resources[2],
        "changing sigma should rebuild only the gaussian smoothing cache");

    session.Submit(spectiary::SpectrumViewSessionCommand::SetMedianKernelSize(11));
    Require(ui.RenderFrame(session, snapshot).plot_submitted, "median smoothing should rebuild");
    const std::vector<spectiary::SpectrumValueVector> median_changed_resources =
        session.RetainHeavySnapshotResources();
    Require(
        median_changed_resources.size() == 3 &&
            median_changed_resources[1] == gaussian_changed_resources[1] &&
            median_changed_resources[2] != gaussian_changed_resources[2],
        "changing kernel size should rebuild only the median smoothing cache");

    session.Submit(spectiary::SpectrumViewSessionCommand::ResetSmoothing());
    const spectiary::SpectrumViewSessionView reset = session.View();
    Require(
        reset.show_raw_curve && !reset.show_gaussian_smoothed &&
            !reset.show_median_smoothed,
        "reset smoothing should restore display defaults");
    Require(
        session.RetainHeavySnapshotResources().empty(),
        "reset smoothing should clear smoothing resources");
}

void TestRenderFeedbackTracksPanLifecycle()
{
    ScopedPlotUi ui;
    spectiary::SpectrumViewSession session;
    const spectiary::SpectrumSnapshotHandle snapshot =
        MakeSnapshot({1.0, 2.0, 3.0}, {2.0, 4.0, 3.0});

    Require(!ui.RenderFrame(session, snapshot).pan_active, "plot pan should be inactive initially");
    Require(
        !ui.RenderFrame(session, snapshot, ImVec2(400.0f, 300.0f), true).pan_active,
        "mouse press without movement should not start a pan");
    const spectiary::SpectrumViewRenderFeedback dragging =
        ui.RenderFrame(session, snapshot, ImVec2(430.0f, 300.0f), true);
    Require(
        dragging.pan_active && session.PlotPanActive(),
        "render feedback should expose an active plot pan");

    const spectiary::SpectrumViewRenderFeedback released =
        ui.RenderFrame(session, snapshot, ImVec2(430.0f, 300.0f), false);
    Require(
        !released.pan_active && !session.PlotPanActive(),
        "mouse release should clear plot pan feedback");
}

void TestActivationPresentationBindingResetsAndRetiresHeavyViewResources()
{
    const std::filesystem::path path =
        UniqueTempPath();
    {
        std::ofstream stream(
            path,
            std::ios::binary | std::ios::trunc);
        Require(
            stream.good(),
            "activation binding fixture should be created");
        stream << "fixture";
    }

    ScopedPlotUi ui;
    spectiary::SourceCollectionSession source_session(
        {},
        {},
        {},
        {});
    spectiary::SpectrumViewSession presentation;
    ConfigureGaussianSmoothing(presentation);

    auto destroyed_promise =
        std::make_shared<
            std::promise<std::thread::id>>();
    std::future<std::thread::id> destroyed =
        destroyed_promise->get_future();
    spectiary::SpectrumValueVector old_y_values(
        new const std::vector<double>{
            2.0,
            4.0,
            3.0},
        [destroyed_promise](
            const std::vector<double>* values) {
            delete values;
            destroyed_promise->set_value(
                std::this_thread::get_id());
        });
    spectiary::SpectrumSnapshotHandle old_snapshot =
        MakeSnapshot(
            {1.0, 2.0, 3.0},
            old_y_values);
    Require(
        ui.RenderFrame(
              presentation,
              old_snapshot)
            .plot_submitted,
        "old snapshot should populate the presentation cache");
    Require(
        presentation.
                RetainHeavySnapshotResources()
            .size() == 2,
        "binding regression requires both heavy smoothing resources");
    presentation.Submit(
        spectiary::SpectrumViewSessionCommand::SetViewportRangeMode(
            spectiary::SpectrumViewportRangeMode::Locked));
    old_snapshot.reset();
    old_y_values.reset();

    spectiary::SourceCollectionLoadDependencies dependencies;

    dependencies.workflow_cache_paths = spectiary::test_support::EmptyWorkflowCachePaths();
    dependencies.snapshot_loader =
        [](const std::filesystem::path& source,
           std::size_t index,
           const auto&) {
            return MakeActivationSnapshot(
                source,
                index);
        };
    dependencies.workflow_cache_paths = spectiary::test_support::EmptyWorkflowCachePaths();
    spectiary::SourceCollectionActivationTransaction
        activation(
            source_session,
            spectiary::
                MakeSourceCollectionLoadQueueForTesting(
                    std::move(dependencies)));
    spectiary::
        BindSourceCollectionActivationPresentationLifecycle(
            activation,
            presentation);

    const std::thread::id caller_thread =
        std::this_thread::get_id();
    (void)activation.OpenSource(path, 0);
    const auto deadline =
        std::chrono::steady_clock::now() + 2s;
    bool activated = false;
    while (std::chrono::steady_clock::now() <
           deadline) {
        (void)activation.Drain(false);
        const spectiary::SpectrumSnapshotHandle current =
            source_session.CurrentSampleSnapshot();
        activated =
            current && current->source.path == path &&
            !activation.status().loading;
        if (activated) {
            break;
        }
        std::this_thread::sleep_for(2ms);
    }
    const bool retired =
        destroyed.wait_for(2s) ==
        std::future_status::ready;
    const std::thread::id retirement_thread =
        retired ? destroyed.get() : std::thread::id{};

    std::filesystem::remove(path);
    Require(
        activated,
        "prepared source activation should commit through the bound transaction");
    Require(
        presentation.
            RetainHeavySnapshotResources()
            .empty(),
        "snapshot activation should reset the bound presentation view");
    Require(
        presentation.View().viewport_range_mode ==
            spectiary::SpectrumViewportRangeMode::Automatic,
        "activating a different source collection should unlock the viewport");
    Require(
        retired,
        "old presentation resources should reach the background reclaimer");
    Require(
        retirement_thread != caller_thread,
        "old presentation resources must not be destroyed on the activation caller");
}

}  // namespace

int main()
{
    TestImmersiveContextOverlayUsesResolvedPositionAndCurrentLabel();
    TestImmersiveContextOverlayTracksAutoAdvanceProvenance();
    TestImmersiveContextOverlayIsImmersiveOnlyAndHandlesUnavailableContext();
    TestImmersiveContextOverlayDrawsWithoutCapturingPlotInput();
    TestViewportTransitionPolicyUsesChangeReason();
    TestLockedViewportStateCacheRoundTripsAndClears();
    TestLegacySpectrumMigrationCompletesPartialCutover();
    TestSpectrumColorCacheSupportsLegacyAndDamagedEntries();
    TestSpectrumViewSessionOwnsCustomCurveColors();
    TestSpectrumViewSessionCapturesAndRestoresLockedLimits();
    TestIndependentSpectrumViewsDoNotShareViewportLock();
    TestSourceRosterClassifiesSnapshotChanges();
    TestSnapshotResetPreservesControlsAndFitsNewData();
    TestCoincidentWavelengthsFitOnActivation();
    TestHiddenCurvesStillReportPresentedPlotFrame();
    TestNativeYTicksSurviveImmersiveModeTransitions();
    TestNearlyCoincidentAxisEndpointsRemainResponsive();
    TestFitAndStoredLimitReuseAreObservable();
    TestViewportLockOverlayTogglesInAxisCorner();
    TestViewportLockConsumesImmersiveAxisAndTouchpadInput();
    TestViewportLockPreservesAdjustedLimitsAndUnlockRestoresFit();
    TestSmoothingCommandsOwnCacheInvalidation();
    TestRenderFeedbackTracksPanLifecycle();
    TestActivationPresentationBindingResetsAndRetiresHeavyViewResources();
    return 0;
}
