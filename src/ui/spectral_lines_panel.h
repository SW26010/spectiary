#pragma once

#include "domain/spectrum_snapshot.h"
#include "ui/spectral_lines_grouping_view.h"
#include "ui/spectral_lines_panel_controller.h"
#include "ui/ui_text.h"

#include <array>
#include <optional>
#include <string>

namespace specforge {

class SpectralLinesPanelUi {
public:
    [[nodiscard]] static const char* WindowName();

    void Render(SpectralLinesPanelController& panel, const SpectrumSnapshotHandle& snapshot, bool* open);
    void Render(
        SpectralLinesPanelController& panel,
        const SpectrumSnapshotHandle& snapshot,
        UiLanguage language,
        bool* open);

private:
    std::array<char, 96> grouping_view_search_ = {};
    bool grouping_view_search_initialized_ = false;
    std::optional<std::string> renaming_grouping_view_id_;
    std::array<char, 128> renaming_grouping_view_name_ = {};
    std::optional<std::string> deleting_grouping_view_id_;
    std::string deleting_grouping_view_name_;
    SpectralLinesGroupingViewUi grouping_view_ui_;
};

}  // namespace specforge
