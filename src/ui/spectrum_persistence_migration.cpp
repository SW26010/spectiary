#include "ui/spectrum_persistence_migration.h"

#include "ui/legacy_spectrum_view_state_io.h"

#include <system_error>

namespace specforge {

SpectrumPersistenceMigrationResult MigrateLegacySpectrumViewState(
    const RuntimePaths& paths,
    SpectrumPlotPreferencesLoadResult& preferences,
    SpectrumViewportStateLoadResult& viewport)
{
    SpectrumPersistenceMigrationResult result;
    const auto healthy = VersionedJsonCacheLoadIssueKind::None;
    const bool preferences_missing = !preferences.document_present &&
        preferences.issue_kind == healthy;
    const bool viewport_missing = !viewport.document_present &&
        viewport.issue_kind == healthy;

    const auto retire_legacy = [&]() {
        // Both targets must have been established. This also finishes cleanup
        // after an earlier partial migration was saved by a runtime retry.
        if (!preferences.document_present || !viewport.document_present ||
            preferences.issue_kind != healthy || viewport.issue_kind != healthy) {
            return;
        }
        std::error_code error;
        if (std::filesystem::is_regular_file(paths.legacy_spectrum_view_state_path, error)) {
            std::filesystem::remove(paths.legacy_spectrum_view_state_path, error);
            if (error) {
                viewport.warning += " Legacy spectrum state cleanup failed; split files remain authoritative.";
                viewport.diagnostic_detail += " " + error.message();
            }
        }
    };

    if (!preferences_missing && !viewport_missing) {
        retire_legacy();
        return result;
    }

    const auto legacy = LoadLegacySpectrumViewState(paths.legacy_spectrum_view_state_path);
    if (legacy.issue_kind != healthy) {
        // A failed legacy load does not authorize writing fallback values into
        // a missing destination. The other owner keeps its independent status.
        if (preferences_missing) {
            preferences.issue_kind = legacy.issue_kind;
            preferences.warning = legacy.warning;
            preferences.diagnostic_detail = legacy.diagnostic_detail;
        }
        if (viewport_missing) {
            viewport.issue_kind = legacy.issue_kind;
            viewport.warning = legacy.warning;
            viewport.diagnostic_detail = legacy.diagnostic_detail;
        }
        return result;
    }
    if (!legacy.document_present) {
        return result;
    }

    if (preferences_missing) {
        preferences.state.plot_colors = legacy.state.plot_colors;
        preferences.warning = legacy.warning;
        std::string error;
        preferences.document_present = SaveSpectrumPlotPreferences(
            paths.spectrum_plot_preferences_path, preferences.state, &error);
        result.preferences_save_pending = !preferences.document_present;
        if (result.preferences_save_pending) {
            preferences.warning += " Spectrum plot preference migration save will be retried.";
            preferences.diagnostic_detail = std::move(error);
        }
    }
    if (viewport_missing) {
        viewport.state = {
            .locked = legacy.state.locked,
            .source_collection_identity = legacy.state.source_collection_identity,
            .limits = legacy.state.limits,
        };
        viewport.warning = legacy.warning;
        std::string error;
        viewport.document_present = SaveSpectrumViewportState(
            paths.spectrum_viewport_state_path, viewport.state, &error);
        result.viewport_save_pending = !viewport.document_present;
        if (result.viewport_save_pending) {
            viewport.warning += " Spectrum viewport migration save will be retried.";
            viewport.diagnostic_detail = std::move(error);
        }
    }
    retire_legacy();
    return result;
}

}  // namespace specforge
