#pragma once

#include "domain/spectrum_snapshot.h"
#include "domain/sample_filter.h"
#include "plot/spectrum_plot.h"
#include "ui/sample_labeling_controller.h"
#include "ui/sample_navigation_controller.h"
#include "ui/sample_workflow_panel.h"
#include "ui/spectral_lines_panel.h"
#include "ui/spectral_lines_panel_controller.h"

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
    struct SourceListEntry {
        std::filesystem::path path;
        std::string key;
        std::string display_name;
        std::string type_label;
        std::string state_label;
        // Stores the last domain snapshot for this source so reactivation can use
        // an explicit cache instead of reloading. Do not remove as a summary-only
        // optimization without retesting CSV/folder error snapshots: that change
        // reproduced 0xc0000005 shared_ptr refcount crashes.
        SpectrumSnapshotHandle cached_snapshot;
        std::size_t last_spectrum_index = 0;
    };

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

    std::size_t AddOrUpdateSource(
        const std::filesystem::path& path,
        SpectrumSnapshotHandle snapshot,
        std::size_t spectrum_index);
    void ActivateSource(std::size_t source_index);
    void RemoveSource(std::size_t source_index);
    void OpenSourceFromFilePicker();
    void OpenSourceFromFolderPicker();
    void OpenAnnotationFromFilePicker();
    void RenderDockHost(const ShellStatus& status);
    void RenderMainMenuBar();
    void RenderFilesPanel();
    void RenderInfoTagsPanel();
    void RenderNavigationPanel();
    void RenderAnnotationsPanel();
    void RenderLabelingPanel();
    void RenderFiltersPanel();
    void RenderSmoothingPanel();
    void RenderMainPlot(const ShellStatus& status);
    void RenderSpectralLinesPanel();
    void SeedInitialDockLayout(ImGuiID dockspace_id, const ImVec2& size);
    void SetSnapshot(SpectrumSnapshotHandle snapshot);
    void EnsureSnapshotMatchesNavigation();
    void LoadActiveSourceAt(std::size_t spectrum_index);
    [[nodiscard]] SampleNavigationResult RequestSampleNavigation(const SampleNavigationRequest& request);
    void SyncNavigationInputs();
    void SyncSampleNavigationSession();
    void SyncSampleWorkflowSession();
    [[nodiscard]] std::vector<SampleFilterSource> BuildSampleFilterSources() const;
    void ApplySampleFiltersToNavigation();

    SpectrumSnapshotHandle snapshot_;
    SpectrumPlotState plot_state_;
    SpectrumPlotStyle plot_style_;
    SampleLabelingController sample_labeling_;
    SampleFilterController sample_filters_;
    SampleNavigationController sample_navigation_;
    SpectralLinesPanelController spectral_lines_panel_;
    SpectralLinesPanelUi spectral_lines_panel_ui_;
    std::vector<SourceListEntry> sources_;
    std::optional<std::size_t> current_source_index_;
    std::array<char, 32> row_index_buffer_ = {};
    std::array<char, 128> sample_name_query_buffer_ = {};
    std::optional<std::string> active_sample_workflow_identity_;
    bool label_shortcut_context_active_ = false;
    bool layout_seeded_ = false;
    PanelVisibility panel_visibility_;
    SampleWorkflowPanelUi sample_workflow_panel_ui_;
};

}  // namespace specforge
