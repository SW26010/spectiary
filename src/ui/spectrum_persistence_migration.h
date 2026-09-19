#pragma once

#include "ui/spectrum_plot_preferences_io.h"
#include "ui/spectrum_viewport_state_io.h"

namespace specforge {

// One startup cutover, only into missing targets. Existing split documents,
// including unreadable/unsupported ones, always retain their own authority.
// Failed imports keep the destination/defaults and never queue normal autosave.
void MigrateLegacySpectrumViewState(
    const RuntimePaths& paths,
    SpectrumPlotPreferencesLoadResult& preferences,
    SpectrumViewportStateLoadResult& viewport);

}  // namespace specforge
