#include "overlays/spectral_line_user_state.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <iterator>
#include <map>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace specforge {
namespace {

constexpr const char* kPublicCatalogIdentity = "specforge.public";
constexpr const char* kPublicCatalogDisplayName = "Public catalog";
constexpr const char* kCatalogGroupingViewId = "__catalog_grouping_view__";
constexpr const char* kUnassignedUserGroupId = "__unassigned__";

std::string LowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool ContainsCaseInsensitive(std::string_view text, std::string_view pattern)
{
    if (pattern.empty()) {
        return true;
    }
    return LowerAscii(std::string(text)).find(LowerAscii(std::string(pattern))) != std::string::npos;
}

std::string SanitizeId(std::string_view value)
{
    std::string id;
    id.reserve(value.size());
    for (const char character : value) {
        const unsigned char unsigned_character = static_cast<unsigned char>(character);
        if (std::isalnum(unsigned_character)) {
            id.push_back(static_cast<char>(std::tolower(unsigned_character)));
        } else if (!id.empty() && id.back() != '_') {
            id.push_back('_');
        }
    }
    while (!id.empty() && id.back() == '_') {
        id.pop_back();
    }
    return id.empty() ? "group" : id;
}

std::string UniqueCatalogGroupId(std::string_view group_name, std::unordered_set<std::string>& used_group_ids)
{
    const std::string base_id = "catalog_" + SanitizeId(group_name);
    std::string candidate = base_id;
    int suffix = 2;
    while (!used_group_ids.insert(candidate).second) {
        candidate = base_id + "_" + std::to_string(suffix++);
    }
    return candidate;
}

MarkerReference MakeReference(const CatalogIdentity& identity, std::string marker_id)
{
    MarkerReference reference;
    reference.catalog_identity = identity;
    reference.marker_id = std::move(marker_id);
    return reference;
}

bool SameReference(const MarkerReference& reference, const CatalogIdentity& identity, std::string_view marker_id)
{
    return SameCatalogIdentity(reference.catalog_identity, identity) && reference.marker_id == marker_id;
}

bool ContainsReference(
    const std::vector<MarkerReference>& references,
    const CatalogIdentity& identity,
    std::string_view marker_id)
{
    return std::any_of(references.begin(), references.end(), [&identity, marker_id](const auto& reference) {
        return SameReference(reference, identity, marker_id);
    });
}

std::vector<MarkerReference>::iterator FindReference(
    std::vector<MarkerReference>& references,
    const CatalogIdentity& identity,
    std::string_view marker_id)
{
    return std::find_if(references.begin(), references.end(), [&identity, marker_id](const auto& reference) {
        return SameReference(reference, identity, marker_id);
    });
}

UserGroup* FindGroup(GroupingView& view, std::string_view group_id)
{
    const auto match = std::find_if(view.groups.begin(), view.groups.end(), [group_id](const auto& group) {
        return group.id == group_id;
    });
    return match == view.groups.end() ? nullptr : &(*match);
}

const UserGroup* FindGroup(const GroupingView& view, std::string_view group_id)
{
    const auto match = std::find_if(view.groups.begin(), view.groups.end(), [group_id](const auto& group) {
        return group.id == group_id;
    });
    return match == view.groups.end() ? nullptr : &(*match);
}

UserGroup& EnsureUnassignedGroup(GroupingView& view)
{
    for (UserGroup& group : view.groups) {
        if (group.is_unassigned || group.id == kUnassignedUserGroupId) {
            group.id = kUnassignedUserGroupId;
            group.name = "Unassigned";
            group.is_unassigned = true;
            return group;
        }
    }

    UserGroup group;
    group.id = kUnassignedUserGroupId;
    group.name = "Unassigned";
    group.is_unassigned = true;
    view.groups.push_back(std::move(group));
    return view.groups.back();
}

void SortReferencesByCatalogPosition(
    std::vector<MarkerReference>& references,
    const SpectralLineCatalog& catalog,
    const CatalogIdentity& identity)
{
    std::stable_sort(references.begin(), references.end(), [&catalog, &identity](const auto& left, const auto& right) {
        const SpectralLineMarker* left_marker = FindCatalogMarker(catalog, identity, left);
        const SpectralLineMarker* right_marker = FindCatalogMarker(catalog, identity, right);
        if (left_marker != nullptr && right_marker != nullptr) {
            return SpectralLineMarkerPosition(*left_marker) < SpectralLineMarkerPosition(*right_marker);
        }
        if (left_marker != nullptr) {
            return true;
        }
        if (right_marker != nullptr) {
            return false;
        }
        return left.marker_id < right.marker_id;
    });
}

}  // namespace

const CatalogIdentity& PublicSpectralLineCatalogIdentity()
{
    static const CatalogIdentity identity{kPublicCatalogIdentity, kPublicCatalogDisplayName};
    return identity;
}

const char* CatalogGroupingViewId()
{
    return kCatalogGroupingViewId;
}

const char* UnassignedUserGroupId()
{
    return kUnassignedUserGroupId;
}

bool SameCatalogIdentity(const CatalogIdentity& left, const CatalogIdentity& right)
{
    return !left.id.empty() && left.id == right.id;
}

CatalogUserState MakeCatalogUserState(const CatalogIdentity& identity)
{
    CatalogUserState state;
    state.catalog_identity = identity;
    state.active_view_id = kCatalogGroupingViewId;
    return state;
}

CatalogUserState& EnsureCatalogUserState(CatalogUserStateCache& cache, const CatalogIdentity& identity)
{
    auto [iterator, inserted] = cache.catalogs.emplace(identity.id, MakeCatalogUserState(identity));
    if (!inserted && iterator->second.catalog_identity.id.empty()) {
        iterator->second.catalog_identity = identity;
    }
    return iterator->second;
}

CatalogPanelState& EnsureCatalogPanelState(CatalogUserStateCache& cache, const CatalogIdentity& identity)
{
    return cache.catalog_panel_state[identity.id];
}

const SpectralLineMarker* FindCatalogMarker(
    const SpectralLineCatalog& catalog,
    const CatalogIdentity& identity,
    const MarkerReference& reference)
{
    if (!SameCatalogIdentity(reference.catalog_identity, identity)) {
        return nullptr;
    }
    for (const SpectralLineMarker& marker : catalog.markers) {
        if (marker.id == reference.marker_id) {
            return &marker;
        }
    }
    return nullptr;
}

bool CatalogHasGrouping(const SpectralLineCatalog& catalog)
{
    return std::any_of(catalog.markers.begin(), catalog.markers.end(), [](const auto& marker) {
        return !marker.group.empty();
    });
}

std::optional<GroupingView> BuildCatalogGroupingView(
    const SpectralLineCatalog& catalog,
    const CatalogIdentity& identity)
{
    if (!CatalogHasGrouping(catalog)) {
        return std::nullopt;
    }

    std::map<std::string, std::vector<MarkerReference>> grouped_references;
    for (const SpectralLineMarker& marker : catalog.markers) {
        if (marker.group.empty()) {
            continue;
        }
        grouped_references[marker.group].push_back(MakeReference(identity, marker.id));
    }

    GroupingView view;
    view.id = kCatalogGroupingViewId;
    view.name = "Catalog grouping view";
    view.read_only = true;
    std::unordered_set<std::string> group_ids;
    for (auto& [group_name, references] : grouped_references) {
        UserGroup group;
        group.id = UniqueCatalogGroupId(group_name, group_ids);
        group.name = group_name;
        group.marker_references = std::move(references);
        view.groups.push_back(std::move(group));
    }
    return view;
}

GroupingView CreateUserGroupingViewFromCatalog(
    const SpectralLineCatalog& catalog,
    const CatalogIdentity& identity,
    std::string id,
    std::string name)
{
    GroupingView view;
    view.id = std::move(id);
    view.name = std::move(name);
    view.read_only = false;

    UserGroup unassigned;
    unassigned.id = kUnassignedUserGroupId;
    unassigned.name = "Unassigned";
    unassigned.is_unassigned = true;
    unassigned.marker_references.reserve(catalog.markers.size());
    for (const SpectralLineMarker& marker : catalog.markers) {
        unassigned.marker_references.push_back(MakeReference(identity, marker.id));
    }
    view.groups.push_back(std::move(unassigned));
    return view;
}

GroupingView DuplicateGroupingView(
    const GroupingView& source,
    const SpectralLineCatalog& catalog,
    const CatalogIdentity& identity,
    std::string id,
    std::string name)
{
    GroupingView view = source;
    view.id = std::move(id);
    view.name = std::move(name);
    view.read_only = false;
    return EffectiveUserGroupingView(view, catalog, identity);
}

GroupingView EffectiveUserGroupingView(
    const GroupingView& source,
    const SpectralLineCatalog& catalog,
    const CatalogIdentity& identity)
{
    GroupingView view = source;
    view.read_only = false;
    UserGroup unassigned = EnsureUnassignedGroup(view);

    std::unordered_set<std::string> ordinary_marker_ids;
    for (const UserGroup& group : view.groups) {
        if (group.is_unassigned) {
            continue;
        }
        for (const MarkerReference& reference : group.marker_references) {
            if (SameCatalogIdentity(reference.catalog_identity, identity)) {
                ordinary_marker_ids.insert(reference.marker_id);
            }
        }
    }

    std::vector<MarkerReference> unassigned_references;
    std::unordered_set<std::string> unassigned_marker_ids;
    for (const MarkerReference& reference : unassigned.marker_references) {
        if (!SameCatalogIdentity(reference.catalog_identity, identity)) {
            continue;
        }
        if (ordinary_marker_ids.find(reference.marker_id) != ordinary_marker_ids.end()) {
            continue;
        }
        if (unassigned_marker_ids.insert(reference.marker_id).second) {
            unassigned_references.push_back(reference);
        }
    }
    for (const SpectralLineMarker& marker : catalog.markers) {
        if (ordinary_marker_ids.find(marker.id) == ordinary_marker_ids.end() &&
            unassigned_marker_ids.insert(marker.id).second) {
            unassigned_references.push_back(MakeReference(identity, marker.id));
        }
    }
    SortReferencesByCatalogPosition(unassigned_references, catalog, identity);

    for (UserGroup& group : view.groups) {
        if (group.is_unassigned) {
            group.id = kUnassignedUserGroupId;
            group.name = "Unassigned";
            group.marker_references = unassigned_references;
        } else {
            SortReferencesByCatalogPosition(group.marker_references, catalog, identity);
        }
    }
    return view;
}

bool IsMarkerVisible(const CatalogUserState& state, const std::string& marker_id)
{
    const auto match = state.marker_visibility.find(marker_id);
    return match == state.marker_visibility.end() ? true : match->second;
}

void SetMarkerVisible(CatalogUserState& state, const std::string& marker_id, bool visible)
{
    if (!marker_id.empty()) {
        state.marker_visibility[marker_id] = visible;
    }
}

GroupVisibilityState VisibilityStateForGroup(
    const CatalogUserState& state,
    const UserGroup& group,
    const SpectralLineCatalog& catalog,
    const CatalogIdentity& identity,
    bool search_active)
{
    if (search_active) {
        return GroupVisibilityState::SearchFiltered;
    }

    bool has_resolved = false;
    bool any_visible = false;
    bool any_hidden = false;
    for (const MarkerReference& reference : group.marker_references) {
        if (FindCatalogMarker(catalog, identity, reference) == nullptr) {
            continue;
        }
        has_resolved = true;
        if (IsMarkerVisible(state, reference.marker_id)) {
            any_visible = true;
        } else {
            any_hidden = true;
        }
    }

    if (!has_resolved) {
        return GroupVisibilityState::Empty;
    }
    if (any_visible && any_hidden) {
        return GroupVisibilityState::Mixed;
    }
    return any_visible ? GroupVisibilityState::AllVisible : GroupVisibilityState::AllHidden;
}

bool SetGroupMarkerVisibility(
    CatalogUserState& state,
    const UserGroup& group,
    const SpectralLineCatalog& catalog,
    const CatalogIdentity& identity,
    bool visible,
    bool search_active)
{
    if (search_active) {
        return false;
    }

    bool changed = false;
    for (const MarkerReference& reference : group.marker_references) {
        if (FindCatalogMarker(catalog, identity, reference) == nullptr) {
            continue;
        }
        if (IsMarkerVisible(state, reference.marker_id) != visible) {
            SetMarkerVisible(state, reference.marker_id, visible);
            changed = true;
        }
    }
    return changed;
}

bool MarkerMatchesSearch(
    const SpectralLineCatalog& catalog,
    const CatalogIdentity& identity,
    const MarkerReference& reference,
    std::string_view search)
{
    if (search.empty()) {
        return true;
    }

    const SpectralLineMarker* marker = FindCatalogMarker(catalog, identity, reference);
    if (marker == nullptr) {
        return ContainsCaseInsensitive(reference.marker_id, search);
    }
    return ContainsCaseInsensitive(marker->id, search) || ContainsCaseInsensitive(marker->label, search) ||
           ContainsCaseInsensitive(marker->group, search) || ContainsCaseInsensitive(marker->display_label, search);
}

std::unordered_map<std::string, int> MarkerReferenceCounts(
    const GroupingView& view,
    const CatalogIdentity& identity)
{
    std::unordered_map<std::string, int> counts;
    for (const UserGroup& group : view.groups) {
        for (const MarkerReference& reference : group.marker_references) {
            if (SameCatalogIdentity(reference.catalog_identity, identity)) {
                ++counts[reference.marker_id];
            }
        }
    }
    return counts;
}

bool IsSharedMarkerReference(
    const std::unordered_map<std::string, int>& counts,
    const MarkerReference& reference)
{
    const auto match = counts.find(reference.marker_id);
    return match != counts.end() && match->second > 1;
}

bool AddUserGroup(GroupingView& view, std::string id, std::string name)
{
    if (id.empty() || name.empty() || FindGroup(view, id) != nullptr) {
        return false;
    }

    UserGroup group;
    group.id = std::move(id);
    group.name = std::move(name);

    const auto unassigned = std::find_if(view.groups.begin(), view.groups.end(), [](const auto& existing) {
        return existing.is_unassigned || existing.id == kUnassignedUserGroupId;
    });
    view.groups.insert(unassigned, std::move(group));
    return true;
}

bool ReorderUserGroupBefore(
    GroupingView& view,
    std::string_view source_group_id,
    std::string_view target_group_id)
{
    if (source_group_id.empty() || target_group_id.empty() || source_group_id == target_group_id) {
        return false;
    }

    const auto is_unassigned = [](const UserGroup& group) {
        return group.is_unassigned || group.id == kUnassignedUserGroupId;
    };
    const auto find_index = [&view](std::string_view group_id) -> std::optional<std::size_t> {
        for (std::size_t index = 0; index < view.groups.size(); ++index) {
            if (view.groups[index].id == group_id) {
                return index;
            }
        }
        return std::nullopt;
    };

    std::optional<std::size_t> source_index = find_index(source_group_id);
    std::optional<std::size_t> target_index = find_index(target_group_id);
    if (!source_index || !target_index || is_unassigned(view.groups[*source_index])) {
        return false;
    }

    std::size_t insert_index = *target_index;
    if (*source_index < *target_index) {
        --insert_index;
    }
    if (insert_index == *source_index) {
        return false;
    }

    UserGroup moving_group = std::move(view.groups[*source_index]);
    view.groups.erase(view.groups.begin() + static_cast<std::ptrdiff_t>(*source_index));

    const auto unassigned = std::find_if(view.groups.begin(), view.groups.end(), is_unassigned);
    if (unassigned != view.groups.end()) {
        const auto unassigned_index = static_cast<std::size_t>(std::distance(view.groups.begin(), unassigned));
        if (insert_index > unassigned_index) {
            insert_index = unassigned_index;
        }
    }

    view.groups.insert(view.groups.begin() + static_cast<std::ptrdiff_t>(insert_index), std::move(moving_group));
    return true;
}

bool MoveMarkerReference(
    GroupingView& view,
    const CatalogIdentity& identity,
    std::string_view marker_id,
    std::string_view source_group_id,
    std::string_view target_group_id)
{
    if (marker_id.empty() || source_group_id == target_group_id) {
        return false;
    }

    UserGroup* target_group = FindGroup(view, target_group_id);
    if (target_group == nullptr) {
        return false;
    }

    bool removed_from_source = false;
    if (UserGroup* source_group = FindGroup(view, source_group_id)) {
        const auto reference = FindReference(source_group->marker_references, identity, marker_id);
        if (reference != source_group->marker_references.end()) {
            source_group->marker_references.erase(reference);
            removed_from_source = true;
        }
    }

    if (!ContainsReference(target_group->marker_references, identity, marker_id)) {
        target_group->marker_references.push_back(MakeReference(identity, std::string(marker_id)));
        return true;
    }
    return removed_from_source;
}

bool CopyMarkerReference(
    GroupingView& view,
    const CatalogIdentity& identity,
    std::string_view marker_id,
    std::string_view target_group_id)
{
    if (marker_id.empty()) {
        return false;
    }
    UserGroup* target_group = FindGroup(view, target_group_id);
    if (target_group == nullptr || ContainsReference(target_group->marker_references, identity, marker_id)) {
        return false;
    }
    target_group->marker_references.push_back(MakeReference(identity, std::string(marker_id)));
    return true;
}

std::string GroupExpansionKey(std::string_view view_id, std::string_view group_id)
{
    std::string key;
    key.reserve(view_id.size() + group_id.size() + 1);
    key.append(view_id);
    key.push_back('/');
    key.append(group_id);
    return key;
}

}  // namespace specforge
