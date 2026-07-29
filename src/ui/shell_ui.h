#pragma once

#include "app/application_settings.h"
#include "domain/spectrum_snapshot.h"
#include "profile/navigation_latency_trace.h"
#include "ui/panel_session_interaction.h"
#include "ui/sample_workflow_shortcut.h"
#include "ui/sample_workflow_panel.h"
#include "ui/settings_panel.h"
#include "ui/source_collection_panel.h"
#include "ui/spectral_lines_panel.h"
#include "ui/spectral_lines_panel_controller.h"
#include "ui/source_collection_activation_transaction.h"
#include "ui/source_collection_session.h"
#include "ui/spectrum_view_session.h"

#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace specforge {

class ProfileSink;
class PlotTouchpadGestureSource;

struct ShellStatus {
    bool profile_open = false;
    bool profile_stopping = false;
    bool latency_trace_recording_active = false;
    ProfileSink* profile = nullptr;
    const std::filesystem::path* profile_path = nullptr;
    std::string_view profile_status_message;
    bool frame_capture_enabled = false;
    bool frame_capture_pending = false;
    bool window_renderable = true;
    const std::filesystem::path*
        frame_capture_output_directory = nullptr;
    const std::filesystem::path*
        last_frame_capture_path = nullptr;
    std::string_view frame_capture_status_message;
    OnDemandFrameCaptureStatus frame_capture_status =
        OnDemandFrameCaptureStatus::None;
    std::string_view frame_capture_status_operation;
    std::string_view frame_capture_status_result;
    unsigned int client_width = 0;
    unsigned int client_height = 0;
    std::uint64_t frame_index = 0;
};

struct ShellRuntimeResourceObservation {
    SourceCollectionLoadActivitySnapshot load_activity;
    SourceCollectionActivationTransaction::
        PresentedSourceLoadObservation
            presented_source_load;
    std::size_t pending_load_count = 0;
    std::filesystem::path active_source_path;
    std::string load_error;

    [[nodiscard]] bool idle() const noexcept
    {
        return load_activity.load_idle() &&
               load_activity.retirement_idle() &&
               pending_load_count == 0;
    }
};

struct ShellLocalStateFlushResult {
    bool application_settings_saved = true;
    SourceCollectionStateFlushResult source_collection;
    bool spectral_lines_saved = true;

    [[nodiscard]] bool all_saved() const noexcept
    {
        return application_settings_saved &&
               source_collection.all_saved() &&
               spectral_lines_saved;
    }

    [[nodiscard]] std::string FailureMessage(
        UiLanguage language =
            UiLanguage::English) const;
};

class ShellUi {
public:
    ShellUi(
        const SpecForgeStartup& startup,
        PlotTouchpadGestureSource* touchpad_gestures = nullptr);
    ~ShellUi();

    void Render(const ShellStatus& status);
    void RunMaintenance(LocalUserStateSaveScheduler::TimePoint now);
    [[nodiscard]] std::optional<LocalUserStateSaveScheduler::TimePoint> NextMaintenanceDeadline() const;
    [[nodiscard]] ShellLocalStateFlushResult FlushLocalState();
    void RegisterSourceLoadCompletionReadyCallback(
        SourceCollectionLoadQueue::CompletionReadyCallback callback);
    void UnregisterSourceLoadCompletionReadyCallback();
    void OpenSource(const std::filesystem::path& path, std::size_t spectrum_index = 0);
    [[nodiscard]] bool
    ArmRuntimeResourceCancellationCheckpoint();
    void RefreshSystemColors();
    void SetSpectralLineLabelFont(ImFont* font);
    void EnterImmersivePlotMode();
    void ExitImmersivePlotMode();
    [[nodiscard]] bool TakeImmersivePlotModeToggleRequest();
    [[nodiscard]] bool TakeProfileRecordingToggleRequest();
    [[nodiscard]] bool TakeFrameCaptureRequest();
    [[nodiscard]] std::optional<int>
    TakeAppliedUiScalePercentage();
    [[nodiscard]] std::optional<UiLanguage>
    TakeAppliedUiLanguage();
    [[nodiscard]] int ui_scale_percentage() const;
    [[nodiscard]] UiLanguage ui_language() const;
    [[nodiscard]] std::filesystem::path profile_output_directory() const;
    [[nodiscard]] bool immersive_plot_mode() const;
    [[nodiscard]] bool latency_sensitive_plot_interaction_active() const;
    [[nodiscard]] SpectrumSnapshotHandle current_snapshot() const;
    [[nodiscard]] ShellRuntimeResourceObservation
    runtime_resource_observation() const;
    void RecordNavigationKeyInput(
        NavigationLatencyInputKind kind,
        NavigationLatencyTimePoint at = NavigationLatencyTrace::Now());
    void PresentFrame(
        std::uint64_t frame_index,
        std::span<const NavigationLatencyPresentation> presentations);

private:
    ShellUi(
        SourceCollectionSession session,
        SourceCollectionLoadQueue source_load_queue);
    void OpenSourceFromFilePicker();
    void OpenSourceFromFolderPicker();
    void OpenAnnotationFromFilePicker();
    void RenderDockHost(const ShellStatus& status);
    void RenderImmersivePlot(const ShellStatus& status);
    void RenderMainMenuBar(const ShellStatus& status);
    void RenderFilesPanel(
        bool panel_open,
        UiLanguage language);
    void RenderInfoTagsPanel(bool panel_open);
    void RenderNavigationPanel(bool panel_open);
    void RenderAnnotationsPanel(bool panel_open);
    void RenderLabelingPanel(bool panel_open);
    void RenderFiltersPanel(bool panel_open);
    void RenderSortingPanel(bool panel_open);
    void RenderSmoothingPanel(bool panel_open);
    void RenderMainPlot(const ShellStatus& status);
    void RenderSettingsPanel(const ShellStatus& status);
    void RenderSpectralLinesPanel(bool panel_open);
    void SetPanelVisibility(
        ApplicationPanel panel,
        bool visible);
    void SeedInitialDockLayout(ImGuiID dockspace_id, const ImVec2& size);
    void QueueSampleWorkflowShortcut(SampleWorkflowShortcut shortcut);
    void HandleSampleWorkflowShortcut();
    [[nodiscard]] const SourceCollectionSessionView& SessionView();
    [[nodiscard]] LocalUserStateHealthView PersistenceHealth();
    [[nodiscard]] SourceCollectionSessionResult SubmitSessionCommand(
        SourceCollectionSessionIntent command,
        std::optional<
            SourceCollectionActivationTransaction::NavigationIntent>
            navigation = std::nullopt);
    void HandleSessionAction(const SourceCollectionSessionAction& action);
    [[nodiscard]] std::optional<NavigationLatencyTimePoint> TakeNavigationKeyInput(
        NavigationLatencyInputKind kind);
    void DrainSourceLoads(
        bool allow_snapshot_prefetch = true);
    void BeginDeferredSourceRestore();
    void RecordSpectrumDrawSubmission(
        std::uint64_t frame_index,
        unsigned int viewport_id,
        SpectrumSnapshotHandle snapshot);

    friend struct ShellUiTestAccess;

    SourceCollectionSession session_;
    SpectrumViewSession spectrum_view_session_;
    SourceCollectionActivationTransaction source_activation_;
    PanelSessionInteraction panel_session_interaction_;
    SpectralLinesPanelController spectral_lines_panel_;
    SpectralLinesPanelUi spectral_lines_panel_ui_;
    SourceCollectionPanelUi source_collection_panel_ui_;
    SettingsPanelUi settings_panel_ui_;
    ApplicationSettings application_settings_;
    PlotTouchpadGestureSource* touchpad_gestures_ = nullptr;
    ImFont* spectral_line_label_font_ = nullptr;
    bool immersive_plot_mode_ = false;
    bool immersive_plot_toggle_requested_ = false;
    SampleWorkflowShortcut sample_workflow_shortcut_;
    bool persist_local_state_ = true;
    bool layout_seeded_ = false;
    SampleWorkflowPanelUi sample_workflow_panel_ui_;
    std::optional<NavigationLatencyTimePoint> pending_keyboard_previous_at_;
    std::optional<NavigationLatencyTimePoint> pending_keyboard_next_at_;
    std::optional<int> applied_ui_scale_percentage_;
    std::optional<UiLanguage> applied_ui_language_;
    std::optional<ShellLocalStateFlushResult>
        local_state_flush_result_;
};

}  // namespace specforge
