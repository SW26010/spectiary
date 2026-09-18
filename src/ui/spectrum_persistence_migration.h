#pragma once

#include "ui/spectrum_plot_preferences_io.h"
#include "ui/spectrum_viewport_state_io.h"

namespace specforge {

struct SpectrumPersistenceMigrationResult {
    bool preferences_save_pending = false;
    bool viewport_save_pending = false;
};

// One startup cutover, only into missing targets. Existing split documents,
// including unreadable/unsupported ones, always retain their own authority.
[[nodiscard]] SpectrumPersistenceMigrationResult MigrateLegacySpectrumViewState(
    const RuntimePaths& paths,
    SpectrumPlotPreferencesLoadResult& preferences,
    SpectrumViewportStateLoadResult& viewport);

}  // namespace specforge
