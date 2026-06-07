#pragma once

#include "domain/spectrum_snapshot.h"
#include "overlays/spectral_line_catalog.h"
#include "plot/spectrum_plot.h"

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
    [[nodiscard]] std::vector<const SpectralLineMarker*> FilteredSpectralLineMarkers(bool include_disabled) const;
    [[nodiscard]] bool IsSpectralLineEnabled(const SpectralLineMarker& marker) const;
    void SetSpectralLineEnabled(const SpectralLineMarker& marker, bool enabled);

    SpectrumSnapshotHandle snapshot_;
    SpectrumPlotState plot_state_;
    SpectrumPlotStyle plot_style_;
    SpectralLineCatalog spectral_line_catalog_;
    std::vector<std::string> spectral_line_groups_;
    int spectral_line_group_index_ = 0;
    bool show_public_spectral_lines_ = true;
    bool show_spectral_line_labels_ = true;
    std::array<char, 96> spectral_line_filter_ = {};
    std::unordered_set<std::string> disabled_spectral_line_ids_;
    std::vector<SourceListEntry> sources_;
    std::optional<std::size_t> current_source_index_;
    bool layout_seeded_ = false;
};

}  // namespace specforge
