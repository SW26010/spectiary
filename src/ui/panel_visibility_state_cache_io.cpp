#include "ui/panel_visibility_state_cache_io.h"

#include "app/local_user_state_json.h"

#include <ostream>
#include <string>
#include <utility>

namespace specforge {
namespace {

constexpr const char* kStateFormatKind = "specforge.panel_visibility.cache";
constexpr int kStateSchemaVersion = 1;

void WriteBoolMember(std::ostream& stream, const char* name, bool value, bool last = false)
{
    stream << "  \"" << name << "\": " << (value ? "true" : "false");
    stream << (last ? "\n" : ",\n");
}

}  // namespace

std::filesystem::path DefaultPanelVisibilityStateCachePath()
{
    return DefaultLocalUserStatePath("panel-visibility.json");
}

PanelVisibilityState LoadPanelVisibilityStateCache(const std::filesystem::path& path)
{
    PanelVisibilityState state;
    const VersionedJsonCacheLoadResult result =
        LoadVersionedJsonCacheFile(path, kStateFormatKind, {kStateSchemaVersion}, "panel visibility state cache");
    if (!result.document) {
        return state;
    }

    const JsonValue& root = result.document->root;
    state.files = ReadJsonBoolMember(root, "files", state.files);
    state.navigation = ReadJsonBoolMember(root, "navigation", state.navigation);
    state.annotations = ReadJsonBoolMember(root, "annotations", state.annotations);
    state.labeling = ReadJsonBoolMember(root, "labeling", state.labeling);
    state.filters = ReadJsonBoolMember(root, "filters", state.filters);
    state.sorting = ReadJsonBoolMember(root, "sorting", state.sorting);
    state.smoothing = ReadJsonBoolMember(root, "smoothing", state.smoothing);
    state.information = ReadJsonBoolMember(root, "information", state.information);
    state.spectral_lines = ReadJsonBoolMember(root, "spectral_lines", state.spectral_lines);
    return state;
}

bool SavePanelVisibilityStateCache(
    const std::filesystem::path& path,
    const PanelVisibilityState& state)
{
    if (path.empty()) {
        return false;
    }

    return WriteVersionedJsonCacheFile(
        path,
        kStateFormatKind,
        kStateSchemaVersion,
        "panel visibility state cache",
        [&](std::ostream& stream, std::string&) {
            stream << ",\n";
            WriteBoolMember(stream, "files", state.files);
            WriteBoolMember(stream, "navigation", state.navigation);
            WriteBoolMember(stream, "annotations", state.annotations);
            WriteBoolMember(stream, "labeling", state.labeling);
            WriteBoolMember(stream, "filters", state.filters);
            WriteBoolMember(stream, "sorting", state.sorting);
            WriteBoolMember(stream, "smoothing", state.smoothing);
            WriteBoolMember(stream, "information", state.information);
            WriteBoolMember(stream, "spectral_lines", state.spectral_lines, true);
            return true;
        });
}

PanelVisibilityStatePersistence::PanelVisibilityStatePersistence(
    std::filesystem::path cache_path,
    std::uint64_t debounce_frames,
    std::uint64_t retry_frames)
    : cache_path_(std::move(cache_path)),
      save_scheduler_(debounce_frames, retry_frames)
{
}

PanelVisibilityState PanelVisibilityStatePersistence::Load() const
{
    return LoadPanelVisibilityStateCache(cache_path_);
}

void PanelVisibilityStatePersistence::MarkDirtyIfChanged(
    const PanelVisibilityState& previous,
    const PanelVisibilityState& current,
    std::uint64_t frame_index)
{
    if (!(current == previous)) {
        save_scheduler_.MarkDirty(frame_index);
    }
}

void PanelVisibilityStatePersistence::MaybeSave(
    const PanelVisibilityState& state,
    std::uint64_t frame_index)
{
    if (!save_scheduler_.ShouldAttemptSave(frame_index)) {
        return;
    }
    if (SavePanelVisibilityStateCache(cache_path_, state)) {
        save_scheduler_.MarkSaveSucceeded();
        return;
    }
    save_scheduler_.MarkSaveFailed(frame_index);
}

bool PanelVisibilityStatePersistence::Flush(const PanelVisibilityState& state)
{
    if (!save_scheduler_.dirty()) {
        return true;
    }
    if (SavePanelVisibilityStateCache(cache_path_, state)) {
        save_scheduler_.MarkSaveSucceeded();
        return true;
    }
    save_scheduler_.MarkSaveFailed(0);
    return false;
}

}  // namespace specforge
