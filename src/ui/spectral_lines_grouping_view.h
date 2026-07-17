#pragma once

#include "domain/spectrum_snapshot.h"
#include "ui/spectral_lines_panel_controller.h"

#include <array>
#include <cstddef>
#include <optional>
#include <string>

namespace specforge {

class SpectralLinesGroupingViewUi {
public:
    void Render(
        SpectralLinesPanelController& panel,
        const SpectrumSnapshotHandle& snapshot,
        const SpectralLineGroupingView& view,
        std::size_t catalog_marker_count);
    void RenderPendingPopups(SpectralLinesPanelController& panel);

private:
    std::optional<std::string> renaming_group_view_id_;
    std::optional<std::string> renaming_group_id_;
    std::array<char, 128> renaming_group_name_ = {};
    bool renaming_group_popup_requested_ = false;
    std::optional<std::string> group_context_view_id_;
    std::optional<std::string> group_context_group_id_;
};

}  // namespace specforge
