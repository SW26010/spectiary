#include "ui/panel_visibility_state_cache_io.h"

#include "app/local_user_state.h"
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

    const nlohmann::json& root = result.document->root;
    PanelVisibilityState& state = loaded.state;
    std::string invalid_fields;
    const auto read_visibility =
        [&root, &invalid_fields](
            std::string_view key,
            bool& visible) {
            const nlohmann::json* member =
                JsonObjectMember(root, key);
            if (member == nullptr) {
                return;
            }
            if (member->type() != nlohmann::json::value_t::boolean) {
                if (!invalid_fields.empty()) {
                    invalid_fields += ", ";
                }
                invalid_fields += "'";
                invalid_fields += key;
                invalid_fields += "'";
                return;
            }
            visible = member->get<bool>();
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
        nlohmann::json::object({
            {"files", nlohmann::json(state.files)},
            {"navigation",
             nlohmann::json(state.navigation)},
            {"annotations",
             nlohmann::json(state.annotations)},
            {"labeling",
             nlohmann::json(state.labeling)},
            {"filters", nlohmann::json(state.filters)},
            {"sorting", nlohmann::json(state.sorting)},
            {"smoothing",
             nlohmann::json(state.smoothing)},
            {"information",
             nlohmann::json(state.information)},
            {"spectral_lines",
             nlohmann::json(state.spectral_lines)},
        }));
}

}  // namespace specforge
