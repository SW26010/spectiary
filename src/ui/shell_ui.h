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
    std::string active_source_id;
    std::filesystem::path active_source_path;
    std::string load_error;

    [[nodiscard]] bool source_load_idle() const noexcept
    {
        return load_activity.active_task_count == 0;
    }

    [[nodiscard]] bool
    pending_completion_idle() const noexcept
    {
        return load_activity.completed_count == 0 &&
               pending_load_count == 0;
    }

    [[nodiscard]] bool
    background_retirement_idle() const noexcept
    {
        return load_activity.retirement_idle();
    }

    [[nodiscard]] bool idle() const noexcept
    {
        return source_load_idle() &&
               pending_completion_idle() &&
               background_retirement_idle();
    }
};

struct ShellLocalStateFlushResult {
    ApplicationSettingsFlushResult application_settings;
    SourceCollectionStateFlushResult source_collection;
    bool spectral_lines_saved = true;

    [[nodiscard]] bool all_saved() const noexcept
    {
        return application_settings.all_saved() &&
               source_collection.all_saved() &&
               spectral_lines_saved;
    }

    [[nodiscard]] std::string FailureMessage(
        UiLanguage language =
            UiLanguage::English) const;
};

struct ShellAutomationSpectrumView {
    bool present = false;
    std::size_t index = 0;
    std::string name;
    std::size_t count = 0;
};

struct ShellAutomationLabelingView {
    bool has_active_task = false;
    std::string task_id;
    std::string task_name;
    std::vector<std::string> task_ids;
    int current_spectrum_code =
        kUnlabeledSampleLabelCode;
};

struct ShellAutomationView {
    std::string source_id;
    std::filesystem::path source_path;
    bool loading = false;
    std::filesystem::path loading_source_path;
    ShellAutomationSpectrumView spectrum;
    ShellAutomationLabelingView labeling;
};

struct ShellWindowTitleView {
    // Keeps source_path and sample_name alive while the App compares the
    // non-owning title view. The loading path remains valid until the next
    // non-const Shell operation on the same UI thread.
    SpectrumSnapshotHandle snapshot_owner;
    const std::filesystem::path* source_path = nullptr;
    const std::filesystem::path* loading_source_path = nullptr;
    bool sample_present = false;
    std::string_view sample_name;
    std::size_t sample_index = 0;
    std::size_t sample_count = 0;
};

struct ShellAutomationPanelPresentation {
    std::array<
        std::uint64_t,
        kApplicationPanelCount>
        frame_indices{};
    PanelVisibilityState visibility;

    [[nodiscard]] std::uint64_t FrameIndex(
        ApplicationPanel panel) const noexcept
    {
        return frame_indices[
            static_cast<std::size_t>(panel)];
    }

    [[nodiscard]] bool operator==(
        const ShellAutomationPanelPresentation&) const = default;
};

struct ShellAutomationPanelPresentationStatus {
    std::array<
        std::uint64_t,
        kApplicationPanelCount>
        frame_indices{};
    std::array<
        bool,
        kApplicationPanelCount>
        blocked{};

    [[nodiscard]] bool BlockedAfter(
        ApplicationPanel panel,
        std::uint64_t frame_index) const noexcept
    {
        const std::size_t panel_index =
            static_cast<std::size_t>(panel);
        return frame_indices[panel_index] >
                   frame_index &&
               blocked[panel_index];
    }

    [[nodiscard]] bool Blocked(
        ApplicationPanel panel) const noexcept
    {
        return blocked[
            static_cast<std::size_t>(panel)];
    }
};

struct ShellAutomationViewportPresentationState {
    unsigned int viewport_id = 0;
    bool renderable = true;
};

enum class ShellAutomationNavigationError {
    None,
    NoActiveSource,
    SourceNotReady,
    IndexOutOfRange,
    NameUnavailable,
    NameNotFound,
    NameAmbiguous,
    FilteredOut,
    Rejected,
};

struct ShellAutomationNavigationResult {
    ShellAutomationNavigationError error =
        ShellAutomationNavigationError::None;
    std::string source_id;
    ShellAutomationSpectrumView target;
    bool changed = false;
    bool pending = false;
};

enum class ShellAutomationLabelError {
    None,
    NoCurrentSpectrum,
    NoActiveTask,
    LabelNotFound,
    Rejected,
};

struct ShellAutomationLabelAssignmentResult {
    ShellAutomationLabelError error =
        ShellAutomationLabelError::None;
    std::string source_id;
    std::string task_id;
    ShellAutomationSpectrumView spectrum;
    int previous_code = kUnlabeledSampleLabelCode;
    int new_code = kUnlabeledSampleLabelCode;
    bool changed = false;
    bool state_save_scheduled = false;
    bool state_save_attempted = false;
    bool state_saved = false;
    bool output_save_attempted = false;
    bool output_saved = false;
    bool output_retry_scheduled = false;
    bool navigation_pending = false;
};

class ShellUi {
public:
    ShellUi(
        const SpecForgeStartup& startup,
        PlotTouchpadGestureSource* touchpad_gestures = nullptr,
        SampleLabelingStateCacheLoadPolicy
            labeling_state_cache_load_policy =
                SampleLabelingStateCacheLoadPolicy::
                    AllowPersistentOutputs);
    ~ShellUi();

    void Render(const ShellStatus& status);
    void RunMaintenance(LocalUserStateSaveScheduler::TimePoint now);
    [[nodiscard]] std::optional<LocalUserStateSaveScheduler::TimePoint> NextMaintenanceDeadline() const;
    [[nodiscard]] ShellLocalStateFlushResult FlushLocalState();
    void RegisterSourceLoadCompletionReadyCallback(
        SourceCollectionLoadQueue::CompletionReadyCallback callback);
    void UnregisterSourceLoadCompletionReadyCallback();
    void OpenSource(const std::filesystem::path& path, std::size_t spectrum_index = 0);
    void OpenExternalSource(
        const std::filesystem::path& path,
        std::size_t spectrum_index = 0);
    [[nodiscard]] SourceCollectionActivationTransaction::
        SourceOpenOperation
    OpenSourceForAutomation(
        const std::filesystem::path& path);
    [[nodiscard]] SourceCollectionActivationTransaction::
        SourceOpenOperationOutcome
    ObserveSourceOpenForAutomation(
        const SourceCollectionActivationTransaction::
            SourceOpenOperation& operation) const;
    [[nodiscard]] const
        SourceCollectionActivationTransaction::
            PresentedSpectrumObservation&
    PresentedSpectrumForAutomation() const noexcept;
    [[nodiscard]] std::uint64_t
    ActivationGenerationForAutomation() const noexcept;
    [[nodiscard]] ShellAutomationNavigationResult
    GotoSpectrumForAutomation(
        std::optional<std::size_t> index,
        std::optional<std::string_view> name);
    [[nodiscard]] ShellAutomationLabelAssignmentResult
    AssignLabelForAutomation(int code);
    [[nodiscard]] ApplicationSettingsResult
    SetUiLanguageForAutomation(UiLanguage language);
    [[nodiscard]] ApplicationSettingsResult
    SetUiScaleForAutomation(int percentage);
    [[nodiscard]] ApplicationSettingsResult
    SetPanelVisibilityForAutomation(
        ApplicationPanel panel,
        bool visible);
    [[nodiscard]] PanelVisibilityState
    PanelVisibilityForAutomation() const;
    [[nodiscard]] ShellAutomationView AutomationView();
    [[nodiscard]] ShellWindowTitleView
    WindowTitleView() const;
    [[nodiscard]] const ShellAutomationView&
    PresentedAutomationView() const noexcept;
    [[nodiscard]] const
        ShellAutomationPanelPresentation&
    PresentedPanelVisibilityForAutomation() const noexcept;
    [[nodiscard]] const
        ShellAutomationPanelPresentationStatus&
    PanelPresentationStatusForAutomation() const noexcept;
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
        std::span<const NavigationLatencyPresentation> presentations,
        std::span<
            const ShellAutomationViewportPresentationState>
            viewport_states = {});

private:
    ShellUi(
        SourceCollectionSession session,
        SourceCollectionLoadQueue source_load_queue);
    void OpenSourceFromFilePicker();
    void OpenSourceFromFilePicker(
        const SourceCollectionPathPicker& choose_source_file);
    void OpenSourceFromFolderPicker();
    void OpenAnnotationFromFilePicker();
    void RenderDockHost(const ShellStatus& status);
    void RenderImmersivePlot(const ShellStatus& status);
    void RenderMainMenuBar(const ShellStatus& status);
    void RenderMainMenuBar(
        const ShellStatus& status,
        const SourceCollectionPathPicker& choose_source_file);
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
    [[nodiscard]] ApplicationSettingsResult
    ApplyApplicationSettingsIntent(
        ApplicationSettingsIntent intent,
        ApplicationSettingsRuntimeState runtime);
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
    void RecordPanelVisibilityDrawSubmission(
        std::uint64_t frame_index,
        unsigned int viewport_id,
        PanelVisibilityState visibility);
    void RecordPanelDrawSubmission(
        ApplicationPanel panel,
        unsigned int viewport_id);
    void RecordPanelWindowDrawSubmission(
        ApplicationPanel panel);
    [[nodiscard]] ShellAutomationView
    AutomationViewForSnapshot(
        const SpectrumSnapshotHandle& snapshot);

    struct AutomationPresentationCandidate {
        std::uint64_t frame_index = 0;
        ShellAutomationView view;
    };

    struct AutomationPanelPresentationCandidate {
        std::uint64_t frame_index = 0;
        unsigned int main_viewport_id = 0;
        PanelVisibilityState visibility;
        std::array<
            std::optional<unsigned int>,
            kApplicationPanelCount>
            draw_viewport_ids;
        bool main_viewport_presented = false;
    };

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
    std::optional<AutomationPresentationCandidate>
        automation_presentation_candidate_;
    ShellAutomationView
        presented_automation_view_;
    std::optional<AutomationPanelPresentationCandidate>
        automation_panel_presentation_candidate_;
    std::array<
        std::vector<unsigned int>,
        kApplicationPanelCount>
        automation_panel_visible_viewports_;
    ShellAutomationPanelPresentation
        presented_panel_visibility_;
    ShellAutomationPanelPresentationStatus
        automation_panel_presentation_status_;
};

}  // namespace specforge
