#pragma once

#include "domain/spectrum_snapshot.h"
#include "plot/spectrum_plot.h"
#include "ui/spectral_lines_panel_controller.h"

#include <imgui.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_set>
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

    std::size_t AddOrUpdateSource(
        const std::filesystem::path& path,
        SpectrumSnapshotHandle snapshot,
        std::size_t spectrum_index);
    void ActivateSource(std::size_t source_index);
    void RemoveSource(std::size_t source_index);
    void RenderDockHost(const ShellStatus& status);
    void RenderFilesPanel();
    void RenderInfoTagsPanel();
    void RenderMainPlot(const ShellStatus& status);
    void RenderSpectralLinesPanel();
    void SeedInitialDockLayout(ImGuiID dockspace_id, const ImVec2& size);
    void SetSnapshot(SpectrumSnapshotHandle snapshot);
    void SwitchSpectrum(int direction);
    void RenderSpectralLineGroupingView(const GroupingView& view, GroupingView* editable_view);

    SpectrumSnapshotHandle snapshot_;
    SpectrumPlotState plot_state_;
    SpectrumPlotStyle plot_style_;
    SpectralLinesPanelController spectral_lines_panel_;
    std::optional<std::string> renaming_grouping_view_id_;
    std::array<char, 128> renaming_grouping_view_name_ = {};
    std::optional<std::string> renaming_group_view_id_;
    std::optional<std::string> renaming_group_id_;
    std::array<char, 128> renaming_group_name_ = {};
    bool renaming_group_popup_requested_ = false;
    std::optional<std::string> group_context_view_id_;
    std::optional<std::string> group_context_group_id_;
    std::optional<std::string> deleting_grouping_view_id_;
    std::string deleting_grouping_view_name_;
    std::vector<SourceListEntry> sources_;
    std::optional<std::size_t> current_source_index_;
    bool layout_seeded_ = false;
};

}  // namespace specforge
