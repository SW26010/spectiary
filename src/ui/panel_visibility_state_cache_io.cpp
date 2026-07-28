#include "ui/panel_visibility_state_cache_io.h"

#include "app/local_user_state_json.h"
#include "app/local_user_state_paths.h"

#include <string>
#include <string_view>
#include <utility>

namespace specforge {
namespace {

constexpr const char* kStateFormatKind = "specforge.panel_visibility.cache";
constexpr int kStateSchemaVersion = 1;

}  // namespace

std::filesystem::path DefaultPanelVisibilityStateCachePath()
{
    return DefaultLocalUserStatePath(
        local_user_state_paths::kPanelVisibilityState);
}

PanelVisibilityStateCacheLoadResult
LoadPanelVisibilityStateCache(
    const std::filesystem::path& path)
{
    PanelVisibilityStateCacheLoadResult loaded;
    VersionedJsonCacheLoadResult result =
        LoadVersionedJsonCacheFile(path, kStateFormatKind, {kStateSchemaVersion}, "panel visibility state cache");
    loaded.warning = std::move(result.warning);
    if (!result.document) {
        return loaded;
    }

    const JsonValue& root = result.document->root;
    PanelVisibilityState& state = loaded.state;
    std::string invalid_fields;
    const auto read_visibility =
        [&root, &invalid_fields](
            std::string_view key,
            bool& visible) {
            const JsonValue* member =
                JsonObjectMember(root, key);
            if (member == nullptr) {
                return;
            }
            if (member->kind != JsonValue::Kind::Bool) {
                if (!invalid_fields.empty()) {
                    invalid_fields += ", ";
                }
                invalid_fields += "'";
                invalid_fields += key;
                invalid_fields += "'";
                return;
            }
            visible = member->bool_value;
        };
    read_visibility("files", state.files);
    read_visibility("navigation", state.navigation);
    read_visibility("annotations", state.annotations);
    read_visibility("labeling", state.labeling);
    read_visibility("filters", state.filters);
    read_visibility("sorting", state.sorting);
    read_visibility("smoothing", state.smoothing);
    read_visibility("information", state.information);
    read_visibility("spectral_lines", state.spectral_lines);
    if (!invalid_fields.empty()) {
        loaded.warning =
            "Panel visibility state cache member(s) " +
            invalid_fields +
            " must be boolean; defaults were used for "
            "those members.";
    }
    return loaded;
}

bool SavePanelVisibilityStateCache(
    const std::filesystem::path& path,
    const PanelVisibilityState& state)
{
    if (path.empty()) {
        return false;
    }

    return WriteVersionedJsonCacheDocument(
        path,
        kStateFormatKind,
        kStateSchemaVersion,
        "panel visibility state cache",
        JsonObjectValue({
            {"files", JsonBoolValue(state.files)},
            {"navigation",
             JsonBoolValue(state.navigation)},
            {"annotations",
             JsonBoolValue(state.annotations)},
            {"labeling",
             JsonBoolValue(state.labeling)},
            {"filters", JsonBoolValue(state.filters)},
            {"sorting", JsonBoolValue(state.sorting)},
            {"smoothing",
             JsonBoolValue(state.smoothing)},
            {"information",
             JsonBoolValue(state.information)},
            {"spectral_lines",
             JsonBoolValue(state.spectral_lines)},
        }));
}

PanelVisibilityStatePersistence::PanelVisibilityStatePersistence(
    std::filesystem::path cache_path,
    LocalUserStateSaveScheduler::Duration debounce,
    LocalUserStateSaveScheduler::Duration retry)
    : cache_path_(std::move(cache_path)),
      save_scheduler_(debounce, retry)
{
}

PanelVisibilityState PanelVisibilityStatePersistence::Load()
{
    PanelVisibilityStateCacheLoadResult loaded =
        LoadPanelVisibilityStateCache(cache_path_);
    load_warning_ = std::move(loaded.warning);
    return std::move(loaded.state);
}

void PanelVisibilityStatePersistence::MarkDirtyIfChanged(
    const PanelVisibilityState& previous,
    const PanelVisibilityState& current)
{
    if (!(current == previous)) {
        save_status_.ClearRecovered();
        save_scheduler_.MarkDirty();
    }
}

std::optional<bool> PanelVisibilityStatePersistence::RunMaintenance(
    const PanelVisibilityState& state,
    LocalUserStateSaveScheduler::TimePoint now)
{
    if (!save_scheduler_.ShouldAttemptSave(now)) {
        return std::nullopt;
    }
    if (SavePanelVisibilityStateCache(cache_path_, state)) {
        load_warning_.clear();
        save_scheduler_.MarkSaveSucceeded(save_status_);
        return true;
    }
    save_scheduler_.MarkSaveFailed(
        save_status_,
        "Could not save panel visibility.");
    return false;
}

std::optional<LocalUserStateSaveScheduler::TimePoint>
PanelVisibilityStatePersistence::NextMaintenanceDeadline() const
{
    return save_scheduler_.next_attempt_time();
}

bool PanelVisibilityStatePersistence::Flush(const PanelVisibilityState& state)
{
    if (!save_scheduler_.dirty()) {
        return true;
    }
    if (SavePanelVisibilityStateCache(cache_path_, state)) {
        load_warning_.clear();
        save_scheduler_.MarkSaveSucceeded(save_status_);
        return true;
    }
    save_scheduler_.MarkSaveFailed(
        save_status_,
        "Could not save panel visibility.");
    return false;
}

LocalUserStatePersistenceStatus
PanelVisibilityStatePersistence::PersistenceStatus() const
{
    return {
        .retrying = save_status_.failed(),
        .recovered = save_status_.recovered(),
        .load_warning = load_warning_,
        .save_message = save_status_.message(),
    };
}

}  // namespace specforge
