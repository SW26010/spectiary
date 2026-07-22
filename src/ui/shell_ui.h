#pragma once

#include "domain/spectrum_snapshot.h"
#include "ui/source_collection_panel.h"
#include "ui/panel_visibility_state_cache_io.h"
#include "ui/sample_workflow_shortcut.h"
#include "ui/sample_workflow_panel.h"
#include "ui/spectral_lines_panel.h"
#include "ui/spectral_lines_panel_controller.h"
#include "ui/source_collection_session.h"
#include "ui/source_collection_load_queue.h"
#include "ui/spectrum_view_session.h"

#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace specforge {

class ProfileSink;
class PlotTouchpadGestureSource;

struct ShellStatus {
    bool profile_open = false;
    bool profile_stopping = false;
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
    void OpenSource(const std::filesystem::path& path, std::size_t spectrum_index = 0);
    void RefreshSystemColors();
    void EnterImmersivePlotMode();
    void ExitImmersivePlotMode();
    [[nodiscard]] bool TakeImmersivePlotModeToggleRequest();
    [[nodiscard]] bool TakeProfileRecordingToggleRequest();
    [[nodiscard]] bool immersive_plot_mode() const;
    [[nodiscard]] bool latency_sensitive_plot_interaction_active() const;
    [[nodiscard]] SpectrumSnapshotHandle current_snapshot() const;

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
    void RenderSpectralLinesPanel();
    void SeedInitialDockLayout(ImGuiID dockspace_id, const ImVec2& size);
    void QueueSampleWorkflowShortcut(SampleWorkflowShortcut shortcut);
    void HandleSampleWorkflowShortcut();
    [[nodiscard]] const SourceCollectionSessionView& SessionView();
    [[nodiscard]] SourceCollectionSessionResult SubmitSessionCommand(SourceCollectionSessionIntent command);
    [[nodiscard]] SourceCollectionSessionResult SubmitSessionCommandForPanel(SourceCollectionSessionIntent command);
    void HandleSessionAction(const SourceCollectionSessionAction& action);

    enum class PendingSourceLoadPurpose {
        ExplicitOpen,
        SessionFollowUp,
        DeferredRestore,
    };

    [[nodiscard]] std::uint64_t QueueSourceLoad(
        const std::filesystem::path& path,
        std::size_t spectrum_index,
        std::vector<std::filesystem::path> annotation_paths,
        PendingSourceLoadPurpose purpose);
    void BeginSourceActivationIntent(bool preserve_pending_explicit_opens);
    void QueueSessionFollowUp(
        const SourceCollectionSessionResult& result,
        bool deferred_restore = false);
    void CancelSourceFollowUps(const SourceCollectionSessionResult& result);
    void RetireSessionResources(SourceCollectionSessionResult& result);
    void DrainSourceLoads();
    void DrainSourceLoadCompletions(
        std::vector<SourceCollectionLoadCompletion> completions);
    void BeginDeferredSourceRestore();
    void RestoreDeferredActiveSourceIfAvailable();
    void FinishDeferredSourceRestoreIfReady();

    struct PendingSourceLoad {
        std::filesystem::path path;
        std::string path_key;
        std::size_t spectrum_index = 0;
        std::uint64_t generation = 0;
        std::uint64_t activation_epoch = 0;
        PendingSourceLoadPurpose purpose = PendingSourceLoadPurpose::ExplicitOpen;
    };

    [[nodiscard]] static std::optional<PendingSourceLoad> TakeCurrentPendingSourceLoad(
        const SourceCollectionLoadCompletion& completion,
        std::uint64_t activation_epoch,
        std::unordered_map<std::uint64_t, PendingSourceLoad>& pending_loads,
        const std::unordered_map<std::string, std::uint64_t>& source_load_generations);
    static void AdvanceSourceActivationIntent(
        std::uint64_t& activation_epoch,
        std::unordered_map<std::uint64_t, PendingSourceLoad>& pending_loads,
        bool preserve_pending_explicit_opens,
        const std::function<void(std::uint64_t)>& cancel);
    static void CancelSourceFollowUpsForPathInState(
        const std::filesystem::path& path,
        std::unordered_map<std::uint64_t, PendingSourceLoad>& pending_loads,
        std::unordered_set<std::uint64_t>& deferred_restore_task_ids,
        const std::function<void(std::uint64_t)>& cancel);
    static void CancelSourceFollowUpsForResultInState(
        const SourceCollectionSessionResult& result,
        std::unordered_map<std::uint64_t, PendingSourceLoad>& pending_loads,
        std::unordered_set<std::uint64_t>& deferred_restore_task_ids,
        const std::function<void(std::uint64_t)>& cancel);
    [[nodiscard]] static bool HasMatchingSourceFollowUp(
        const std::filesystem::path& path,
        std::size_t spectrum_index,
        const std::unordered_map<std::uint64_t, PendingSourceLoad>& pending_loads);
    [[nodiscard]] static bool CancelFailedPendingSampleNavigation(
        SourceCollectionSession& session,
        const PendingSourceLoad& ticket);
    [[nodiscard]] static bool CompletionStartsSourceActivationIntent(
        PendingSourceLoadPurpose purpose,
        bool loaded);

    friend struct ShellUiTestAccess;

    SourceCollectionSession session_;
    SourceCollectionLoadQueue source_load_queue_;
    SpectrumViewSession spectrum_view_session_;
    SpectralLinesPanelController spectral_lines_panel_;
    SpectralLinesPanelUi spectral_lines_panel_ui_;
    SourceCollectionPanelUi source_collection_panel_ui_;
    PlotTouchpadGestureSource* touchpad_gestures_ = nullptr;
    bool immersive_plot_mode_ = false;
    bool immersive_plot_toggle_requested_ = false;
    bool profile_recording_toggle_requested_ = false;
    SampleWorkflowShortcut sample_workflow_shortcut_;
    bool persist_local_state_ = true;
    bool layout_seeded_ = false;
    PanelVisibilityStatePersistence panel_visibility_state_;
    PanelVisibilityState panel_visibility_;
    SampleWorkflowPanelUi sample_workflow_panel_ui_;
    // SessionView() can derive state across every sample; retain it until a session mutation.
    std::optional<SourceCollectionSessionView> session_view_cache_;
    bool session_view_cache_dirty_ = false;
    std::unordered_map<std::uint64_t, PendingSourceLoad> pending_source_loads_;
    std::unordered_map<std::string, std::uint64_t> source_load_generations_;
    std::unordered_set<std::uint64_t> deferred_restore_task_ids_;
    std::optional<std::filesystem::path> deferred_restore_active_path_;
    mutable std::optional<LocalUserStateSaveScheduler::TimePoint> source_load_service_deadline_;
    std::string source_load_error_;
    std::uint64_t source_activation_epoch_ = 0;
    bool deferred_restore_active_ = false;
};

}  // namespace specforge
