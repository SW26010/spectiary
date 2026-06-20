#pragma once

#include "domain/spectrum_snapshot.h"
#include "plot/spectrum_plot.h"
#include "ui/sample_workflow_panel.h"
#include "ui/spectral_lines_panel.h"
#include "ui/spectral_lines_panel_controller.h"
#include "ui/source_collection_session.h"

#include <imgui.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace specforge {

class ProfileSink;

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
    ShellUi();
    ~ShellUi();

    void Render(const ShellStatus& status);
    void OpenSource(const std::filesystem::path& path, std::size_t spectrum_index = 0);
    void RefreshSystemColors();
    [[nodiscard]] SpectrumSnapshotHandle current_snapshot() const;

private:
    struct PanelVisibility {
        bool files = true;
        bool navigation = true;
        bool annotations = true;
        bool labeling = true;
        bool filters = true;
        bool smoothing = true;
        bool info = true;
        bool spectral_lines = true;
    };

    void OpenSourceFromFilePicker();
    void OpenSourceFromFolderPicker();
    void OpenAnnotationFromFilePicker();
    void RenderDockHost(const ShellStatus& status);
    void RenderMainMenuBar();
    void RenderFilesPanel();
    void RenderInfoTagsPanel();
    void RenderNavigationPanel();
    void RenderSampleNameSearch(const SourceCollectionNavigationView& navigation_view);
    void RenderAnnotationsPanel();
    void RenderLabelingPanel();
    void RenderFiltersPanel();
    void RenderSmoothingPanel();
    void RenderMainPlot(const ShellStatus& status);
    void RenderSpectralLinesPanel();
    void SeedInitialDockLayout(ImGuiID dockspace_id, const ImVec2& size);
    [[nodiscard]] SourceCollectionSessionResult SubmitSessionCommand(SourceCollectionSessionCommand command);
    void HandleSessionAction(const SourceCollectionSessionAction& action);
    void ResetPlotStateForSnapshotChange();
    [[nodiscard]] SampleNavigationResult RequestSampleNavigation(const SampleNavigationRequest& request);
    void SyncNavigationInputs();
    void BeginSampleNameSearch();
    void ClearSampleNameSearch();
    void RestoreFailedSampleNameSearch();
    void CommitSampleNameSearch(std::size_t target_row, const std::string& matched_name);

    SourceCollectionSession session_;
    SpectrumPlotState plot_state_;
    SpectrumPlotStyle plot_style_;
    SpectralLinesPanelController spectral_lines_panel_;
    SpectralLinesPanelUi spectral_lines_panel_ui_;
    std::array<char, 32> row_index_buffer_ = {};
    std::array<char, 128> sample_name_query_buffer_ = {};
    bool sample_name_matches_open_ = false;
    bool sample_name_search_active_ = false;
    std::string sample_name_search_restore_name_;
    bool label_shortcut_context_active_ = false;
    bool layout_seeded_ = false;
    PanelVisibility panel_visibility_;
    SampleWorkflowPanelUi sample_workflow_panel_ui_;
};

}  // namespace specforge
