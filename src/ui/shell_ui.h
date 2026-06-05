#pragma once

#include "domain/spectrum_fixture.h"
#include "plot/spectrum_plot.h"

#include <imgui.h>

#include <cstdint>
#include <filesystem>

namespace specforge {

struct ShellStatus {
    bool profile_open = false;
    std::filesystem::path profile_path;
    unsigned int client_width = 0;
    unsigned int client_height = 0;
    std::uint64_t frame_index = 0;
};

class ShellUi {
public:
    ShellUi();

    void Render(const ShellStatus& status);

private:
    void RenderDockHost(const ShellStatus& status);
    void RenderFilesPanel();
    void RenderInfoTagsPanel();
    void RenderMainPlot();
    void RenderSpectralLinesPanel();
    void SeedInitialDockLayout(ImGuiID dockspace_id, const ImVec2& size);

    SpectrumSeries spectrum_;
    SpectrumPlotState plot_state_;
    bool layout_seeded_ = false;
};

}  // namespace specforge
