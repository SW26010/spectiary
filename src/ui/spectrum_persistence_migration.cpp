#include "ui/spectrum_persistence_migration.h"

#include "ui/legacy_spectrum_view_state_io.h"

#include "platform/atomic_file.h"
#include <Windows.h>
#include <system_error>

namespace specforge {
namespace {
// Stage the domain-validated document, then publish only if still absent.
// Ordinary writers do not need a migration lock and can win this race safely.
template<class State, class Save, class Load, class Result>
bool ImportMissing(const std::filesystem::path& path, const State& state,
    Save save, Load load, Result& loaded, std::string& error)
{
    const auto staged = TemporarySiblingPath(path);
    if (!save(staged, state, &error)) return false;
    const bool published = MoveFileExW(staged.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
    std::error_code ignored;
    if (!published) {
        std::filesystem::remove(staged, ignored);
        if (std::filesystem::is_regular_file(path, ignored)) {
            loaded = load(path);
            return true; // An existing owner, even corrupt, outranks legacy.
        }
        error = "Could not publish the missing spectrum migration destination.";
    }
    return published;
}
} // namespace


void MigrateLegacySpectrumViewState(
    const RuntimePaths& paths,
    SpectrumPlotPreferencesLoadResult& preferences,
    SpectrumViewportStateLoadResult& viewport)
{
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
        return;
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
        return;
    }
    if (!legacy.document_present) {
        // Another startup may have published both owners and retired legacy
        // after this caller took its initial missing-file snapshots.
        if (preferences_missing) preferences = LoadSpectrumPlotPreferences(paths.spectrum_plot_preferences_path);
        if (viewport_missing) viewport = LoadSpectrumViewportState(paths.spectrum_viewport_state_path);
        return;
    }

    if (preferences_missing) {
        preferences.state.plot_colors = legacy.state.plot_colors;
        preferences.warning = legacy.warning;
        std::string error;
        const bool established = ImportMissing(paths.spectrum_plot_preferences_path,
            preferences.state, SaveSpectrumPlotPreferences, LoadSpectrumPlotPreferences, preferences, error);
        if (established && preferences.issue_kind == healthy) preferences.document_present = true;
        if (!established) {
            preferences = LoadSpectrumPlotPreferences(paths.spectrum_plot_preferences_path);
            preferences.warning += " Legacy spectrum preference import was skipped; the destination or defaults remain active.";
            preferences.diagnostic_detail += " " + error;
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
        const bool established = ImportMissing(paths.spectrum_viewport_state_path,
            viewport.state, SaveSpectrumViewportState, LoadSpectrumViewportState, viewport, error);
        if (established && viewport.issue_kind == healthy) viewport.document_present = true;
        if (!established) {
            viewport = LoadSpectrumViewportState(paths.spectrum_viewport_state_path);
            viewport.warning += " Legacy spectrum viewport import was skipped; the destination or defaults remain active.";
            viewport.diagnostic_detail += " " + error;
        }
    }
    retire_legacy();
    return;
}

}  // namespace specforge
