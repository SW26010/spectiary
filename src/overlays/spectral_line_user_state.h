#pragma once

#include "overlays/spectral_line_catalog.h"
#include "plot/series_color_model.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace specforge {

inline constexpr std::size_t kMaximumGeneratedNameCopyCount = 1024;

struct CatalogIdentity {
    std::string id;
    std::string display_name;
};

struct MarkerReference {
    CatalogIdentity catalog_identity;
    std::string marker_id;
};

enum class GeneratedNameSource {
    None,
    CatalogGroupingView,
    DefaultGroupingView,
    DefaultGroup,
};

struct GeneratedNameMetadata {
    GeneratedNameSource source = GeneratedNameSource::None;
    std::size_t ordinal = 0;
    std::size_t copy_count = 0;
    std::string copy_base_name;

    [[nodiscard]] bool operator==(const GeneratedNameMetadata&) const = default;
};

struct UserGroup {
    std::string id;
    std::string name;
    GeneratedNameMetadata generated_name;
    bool is_unassigned = false;
    std::vector<MarkerReference> marker_references;
};

struct GroupingView {
    std::string id;
    std::string name;
    GeneratedNameMetadata generated_name;
    bool read_only = false;
    std::vector<UserGroup> groups;
};

struct CatalogUserState {
    CatalogIdentity catalog_identity;
    std::string active_view_id;
    std::unordered_map<std::string, bool> marker_visibility;
    // Absence is the canonical Auto value. Only per-marker explicit
    // overrides are persisted, so Auto never carries stale RGBA payload.
    std::unordered_map<std::string, PlotSeriesColor> marker_colors;
    std::vector<GroupingView> grouping_views;


};

struct CatalogPanelState {
    std::unordered_set<std::string> expanded_group_ids;
};

struct CatalogUserStateCache {
    std::unordered_map<std::string, CatalogUserState> catalogs;
    std::unordered_map<std::string, CatalogPanelState> catalog_panel_state;
};

struct CatalogUserStateCanonicalizationResult {
    bool changed = false;
    bool active_view_changed = false;
};

enum class GroupVisibilityState {
    Empty,
    AllVisible,
    AllHidden,
    Mixed,
    SearchFiltered,
};

[[nodiscard]] const CatalogIdentity& PublicSpectralLineCatalogIdentity();
[[nodiscard]] const char* CatalogGroupingViewId();
[[nodiscard]] const char* UnassignedUserGroupId();
[[nodiscard]] bool SameCatalogIdentity(const CatalogIdentity& left, const CatalogIdentity& right);
[[nodiscard]] CatalogUserState MakeCatalogUserState(const CatalogIdentity& identity);
[[nodiscard]] CatalogUserState& EnsureCatalogUserState(
    CatalogUserStateCache& cache,
    const CatalogIdentity& identity);
[[nodiscard]] CatalogPanelState& EnsureCatalogPanelState(
    CatalogUserStateCache& cache,
    const CatalogIdentity& identity);
[[nodiscard]] CatalogUserStateCanonicalizationResult
CanonicalizeCatalogUserState(
    CatalogUserState& state,
    CatalogPanelState& panel_state,
    const SpectralLineCatalog& catalog,
    const CatalogIdentity& identity,
    const std::optional<GroupingView>& catalog_grouping_view);

// Validation used immediately before merging an explicit task into the
// latest durable cache. Startup migration may canonicalize legacy state, but
// an explicit task must not silently repair semantic identity corruption and
// then overwrite the evidence that caused it.
[[nodiscard]] bool ValidateCatalogUserStateCacheForReconciliation(
    const CatalogUserStateCache& cache,
    std::string& diagnostic);

// Legacy schema one/two/three/four state is validated before startup
// canonicalization.  It intentionally checks identity and reference
// invariants only: those schemas predate the explicit unassigned-group flag
// and allocator history, so those values are migrated after this gate.
[[nodiscard]] bool ValidateCatalogUserStateCacheForLegacyMigration(
    const CatalogUserStateCache& cache,
    std::string& diagnostic,
    int schema_version = 0);

[[nodiscard]] const SpectralLineMarker* FindCatalogMarker(
    const SpectralLineCatalog& catalog,
    const CatalogIdentity& identity,
    const MarkerReference& reference);
[[nodiscard]] bool CatalogHasGrouping(const SpectralLineCatalog& catalog);
[[nodiscard]] std::optional<GroupingView> BuildCatalogGroupingView(
    const SpectralLineCatalog& catalog,
    const CatalogIdentity& identity);
[[nodiscard]] GroupingView CreateUserGroupingViewFromCatalog(
    const SpectralLineCatalog& catalog,
    const CatalogIdentity& identity,
    std::string id,
    std::string name,
    GeneratedNameMetadata generated_name = {});
[[nodiscard]] GroupingView DuplicateGroupingView(
    const GroupingView& source,
    const SpectralLineCatalog& catalog,
    const CatalogIdentity& identity,
    std::string id,
    std::string name);
[[nodiscard]] GroupingView EffectiveUserGroupingView(
    const GroupingView& source,
    const SpectralLineCatalog& catalog,
    const CatalogIdentity& identity);

[[nodiscard]] bool IsMarkerVisible(const CatalogUserState& state, const std::string& marker_id);
void SetMarkerVisible(CatalogUserState& state, const std::string& marker_id, bool visible);
[[nodiscard]] PlotSeriesColor MarkerColor(
    const CatalogUserState& state,
    std::string_view marker_id);
void SetMarkerColor(
    CatalogUserState& state,
    std::string marker_id,
    PlotSeriesColor color);
[[nodiscard]] GroupVisibilityState VisibilityStateForGroup(
    const CatalogUserState& state,
    const UserGroup& group,
    const SpectralLineCatalog& catalog,
    const CatalogIdentity& identity,
    bool search_active);
bool SetGroupMarkerVisibility(
    CatalogUserState& state,
    const UserGroup& group,
    const SpectralLineCatalog& catalog,
    const CatalogIdentity& identity,
    bool visible,
    bool search_active);

[[nodiscard]] bool MarkerMatchesSearch(
    const SpectralLineCatalog& catalog,
    const CatalogIdentity& identity,
    const MarkerReference& reference,
    std::string_view search);
[[nodiscard]] std::unordered_map<std::string, int> MarkerReferenceCounts(
    const GroupingView& view,
    const CatalogIdentity& identity);
[[nodiscard]] bool IsSharedMarkerReference(
    const std::unordered_map<std::string, int>& counts,
    const MarkerReference& reference);

bool AddUserGroup(
    GroupingView& view,
    std::string id,
    std::string name,
    GeneratedNameMetadata generated_name = {});
bool RemoveUserGroup(GroupingView& view, std::string_view group_id);
bool ReorderUserGroupBefore(
    GroupingView& view,
    std::string_view source_group_id,
    std::string_view target_group_id);
bool MoveMarkerReference(
    GroupingView& view,
    const CatalogIdentity& identity,
    std::string_view marker_id,
    std::string_view source_group_id,
    std::string_view target_group_id);
bool RemoveMarkerReferenceFromGroup(
    GroupingView& view,
    const CatalogIdentity& identity,
    std::string_view marker_id,
    std::string_view group_id);
bool CopyMarkerReference(
    GroupingView& view,
    const CatalogIdentity& identity,
    std::string_view marker_id,
    std::string_view target_group_id);
[[nodiscard]] std::string GroupExpansionKey(std::string_view view_id, std::string_view group_id);

}  // namespace specforge
