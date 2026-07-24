#pragma once

#include "app/application_settings.h"
#include "domain/spectrum_snapshot.h"
#include "profile/navigation_latency_trace.h"
#include "ui/source_collection_panel.h"
#include "ui/sample_workflow_shortcut.h"
#include "ui/sample_workflow_panel.h"
#include "ui/settings_panel.h"
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
    unsigned int client_width = 0;
    unsigned int client_height = 0;
    std::uint64_t frame_index = 0;
};

class ShellUi {
public:
    explicit ShellUi(PlotTouchpadGestureSource* touchpad_gestures = nullptr);
    ~ShellUi();

    void Render(const ShellStatus& status);
    void RunMaintenance(LocalUserStateSaveScheduler::TimePoint now);
    [[nodiscard]] std::optional<LocalUserStateSaveScheduler::TimePoint> NextMaintenanceDeadline() const;
    void RegisterSourceLoadCompletionReadyCallback(
        SourceCollectionLoadQueue::CompletionReadyCallback callback);
    void UnregisterSourceLoadCompletionReadyCallback();
    void OpenSource(const std::filesystem::path& path, std::size_t spectrum_index = 0);
    void RefreshSystemColors();
    void SetSpectralLineLabelFont(ImFont* font);
    void EnterImmersivePlotMode();
    void ExitImmersivePlotMode();
    [[nodiscard]] bool TakeImmersivePlotModeToggleRequest();
    [[nodiscard]] bool TakeProfileRecordingToggleRequest();
    [[nodiscard]] std::filesystem::path profile_output_directory() const;
    [[nodiscard]] bool immersive_plot_mode() const;
    [[nodiscard]] bool latency_sensitive_plot_interaction_active() const;
    [[nodiscard]] SpectrumSnapshotHandle current_snapshot() const;
    void RecordNavigationKeyInput(
        NavigationLatencyInputKind kind,
        NavigationLatencyTimePoint at = NavigationLatencyTrace::Now());
    [[nodiscard]] std::vector<NavigationLatencyReport> CompleteFramePresentations(
        std::uint64_t frame_index,
        std::span<const NavigationLatencyPresentation> presentations);
    [[nodiscard]] std::vector<SourceLoadLatencyReport>
    CompleteSourceLoadFramePresentations(
        std::uint64_t frame_index,
        std::span<const NavigationLatencyPresentation> presentations);
    [[nodiscard]] std::vector<NavigationPrefetchReport>
        TakeNavigationPrefetchReports();

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
    void RenderFilesPanel();
    void RenderInfoTagsPanel();
    void RenderNavigationPanel();
    void RenderAnnotationsPanel();
    void RenderLabelingPanel();
    void RenderFiltersPanel();
    void RenderSortingPanel();
    void RenderSmoothingPanel();
    void RenderMainPlot(const ShellStatus& status);
    void RenderSettingsPanel(const ShellStatus& status);
    void RenderSpectralLinesPanel();
    void SeedInitialDockLayout(ImGuiID dockspace_id, const ImVec2& size);
    void QueueSampleWorkflowShortcut(SampleWorkflowShortcut shortcut);
    void HandleSampleWorkflowShortcut();
    [[nodiscard]] const SourceCollectionSessionView& SessionView();
    [[nodiscard]] SourceCollectionSessionResult SubmitSessionCommand(
        SourceCollectionSessionIntent command,
        std::optional<
            SourceCollectionActivationTransaction::NavigationIntent>
            navigation = std::nullopt);
    [[nodiscard]] SourceCollectionSessionResult SubmitSessionCommandForPanel(
        SourceCollectionSessionIntent command,
        std::optional<NavigationLatencyInputKind> navigation_kind = std::nullopt);
    void HandleSessionAction(const SourceCollectionSessionAction& action);
    [[nodiscard]] std::optional<NavigationLatencyTimePoint> TakeNavigationKeyInput(
        NavigationLatencyInputKind kind);
    void DrainSourceLoads(
        bool allow_snapshot_prefetch = true);
    void RetireSessionViews();
    void BeginDeferredSourceRestore();
    void RecordSpectrumDrawSubmission(
        std::uint64_t frame_index,
        unsigned int viewport_id,
        SpectrumSnapshotHandle snapshot);

    friend struct ShellUiTestAccess;

    SourceCollectionSession session_;
    SourceCollectionActivationTransaction source_activation_;
    SpectrumViewSession spectrum_view_session_;
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
    mutable std::optional<LocalUserStateSaveScheduler::TimePoint> source_load_service_deadline_;
    std::optional<NavigationLatencyTimePoint> pending_keyboard_previous_at_;
    std::optional<NavigationLatencyTimePoint> pending_keyboard_next_at_;
};

}  // namespace specforge
