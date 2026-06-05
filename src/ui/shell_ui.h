#pragma once

#include "domain/spectrum_snapshot.h"
#include "plot/spectrum_plot.h"

#include <imgui.h>

#include <cstdint>
#include <filesystem>

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
    void RenderDockHost(const ShellStatus& status);
    void RenderFilesPanel();
    void RenderInfoTagsPanel();
    void RenderMainPlot(const ShellStatus& status);
    void RenderSpectralLinesPanel();
    void SeedInitialDockLayout(ImGuiID dockspace_id, const ImVec2& size);
    void SetSnapshot(SpectrumSnapshotHandle snapshot);
    void SwitchSpectrum(int direction);

    SpectrumSnapshotHandle snapshot_;
    SpectrumPlotState plot_state_;
    SpectrumPlotStyle plot_style_;
    bool layout_seeded_ = false;
};

}  // namespace specforge
