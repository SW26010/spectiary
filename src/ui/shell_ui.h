#pragma once

#include "domain/spectrum_snapshot.h"
#include "ui/source_collection_panel.h"
#include "ui/panel_visibility_state_cache_io.h"
#include "ui/sample_workflow_panel.h"
#include "ui/spectral_lines_panel.h"
#include "ui/spectral_lines_panel_controller.h"
#include "ui/source_collection_session.h"
#include "ui/spectrum_view_session.h"

#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace specforge {

class ProfileSink;
class PlotTouchpadGestureSource;

struct ShellStatus {
    bool profile_open = false;
    ProfileSink* profile = nullptr;
    const std::filesystem::path* profile_path = nullptr;
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
    [[nodiscard]] bool immersive_plot_mode() const;
    [[nodiscard]] bool latency_sensitive_plot_interaction_active() const;
    [[nodiscard]] SpectrumSnapshotHandle current_snapshot() const;

private:
    void OpenSourceFromFilePicker();
    void OpenSourceFromFolderPicker();
    void OpenAnnotationFromFilePicker();
    void RenderDockHost(const ShellStatus& status);
    void RenderImmersivePlot(const ShellStatus& status);
    void RenderMainMenuBar();
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
    [[nodiscard]] const SourceCollectionSessionView& SessionView();
    [[nodiscard]] SourceCollectionSessionResult SubmitSessionCommand(SourceCollectionSessionIntent command);
    [[nodiscard]] SourceCollectionSessionResult SubmitSessionCommandForPanel(SourceCollectionSessionIntent command);
    void HandleSessionAction(const SourceCollectionSessionAction& action);

    SourceCollectionSession session_;
    SpectrumViewSession spectrum_view_session_;
    SpectralLinesPanelController spectral_lines_panel_;
    SpectralLinesPanelUi spectral_lines_panel_ui_;
    SourceCollectionPanelUi source_collection_panel_ui_;
    PlotTouchpadGestureSource* touchpad_gestures_ = nullptr;
    bool immersive_plot_mode_ = false;
    bool immersive_plot_toggle_requested_ = false;
    bool label_shortcut_context_active_ = false;
    bool layout_seeded_ = false;
    PanelVisibilityStatePersistence panel_visibility_state_;
    PanelVisibilityState panel_visibility_;
    SampleWorkflowPanelUi sample_workflow_panel_ui_;
    // SessionView() can derive state across every sample; retain it until a session mutation.
    std::optional<SourceCollectionSessionView> session_view_cache_;
    bool session_view_cache_dirty_ = false;
};

}  // namespace specforge
