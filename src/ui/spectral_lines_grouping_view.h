#pragma once

#include "domain/spectrum_snapshot.h"
#include "ui/spectral_lines_panel_controller.h"

#include <array>
#include <optional>
#include <string>

namespace specforge {

class SpectralLinesGroupingViewUi {
public:
    void Render(
        SpectralLinesPanelController& panel,
        const SpectrumSnapshotHandle& snapshot,
        const GroupingView& view,
        GroupingView* editable_view);
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
