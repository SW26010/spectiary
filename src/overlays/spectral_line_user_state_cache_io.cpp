#include "overlays/spectral_line_user_state.h"

#include "app/local_user_state.h"
#include "app/local_user_state_json.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace specforge {
namespace {

constexpr const char* kCacheFormatKind = "specforge.catalog_user_state.cache";
constexpr int kCacheSchemaVersion = 2;

std::string DisplayNameForCatalogIdentity(std::string_view identity_id)
{
    const CatalogIdentity& public_identity = PublicSpectralLineCatalogIdentity();
    return identity_id == public_identity.id ? public_identity.display_name : std::string(identity_id);
}

MarkerReference MakeReference(const CatalogIdentity& identity, std::string marker_id)
{
    MarkerReference reference;
    reference.catalog_identity = identity;
    reference.marker_id = std::move(marker_id);
    return reference;
}

UserGroup& EnsureUnassignedGroup(GroupingView& view)
{
    for (UserGroup& group : view.groups) {
        if (group.is_unassigned || group.id == UnassignedUserGroupId()) {
            group.id = UnassignedUserGroupId();
            group.name = "Unassigned";
            group.is_unassigned = true;
            return group;
        }
    }

    UserGroup group;
    group.id = UnassignedUserGroupId();
    group.name = "Unassigned";
    group.is_unassigned = true;
    view.groups.push_back(std::move(group));
    return view.groups.back();
}

const JsonValue* ObjectMember(const JsonValue& value, std::string_view key)
{
    return JsonObjectMember(value, key);
}

std::string ReadStringMember(const JsonValue& value, std::string_view key)
{
    return ReadJsonStringMember(value, key).value_or(std::string{});
}

bool ReadBoolMember(const JsonValue& value, std::string_view key, bool fallback)
{
    return ReadJsonBoolMember(value, key, fallback);
}

std::optional<MarkerReference> ReadMarkerReference(
    const JsonValue& value,
    const CatalogIdentity& fallback_identity)
{
    if (value.kind == JsonValue::Kind::String) {
        return MakeReference(fallback_identity, value.string_value);
    }
    if (value.kind != JsonValue::Kind::Object) {
        return std::nullopt;
    }

    const std::string marker_id = ReadStringMember(value, "marker_id");
    if (marker_id.empty()) {
        return std::nullopt;
    }
    CatalogIdentity identity = fallback_identity;
    const std::string identity_id = ReadStringMember(value, "catalog_identity");
    if (!identity_id.empty()) {
        identity.id = identity_id;
        identity.display_name = DisplayNameForCatalogIdentity(identity_id);
    }
    return MakeReference(identity, marker_id);
}

std::vector<MarkerReference> ReadMarkerReferences(
    const JsonValue& value,
    const CatalogIdentity& fallback_identity)
{
    std::vector<MarkerReference> references;
    if (value.kind != JsonValue::Kind::Array) {
        return references;
    }
    for (const JsonValue& item : value.array) {
        if (std::optional<MarkerReference> reference = ReadMarkerReference(item, fallback_identity)) {
            references.push_back(std::move(*reference));
        }
    }
    return references;
}

std::vector<UserGroup> ReadUserGroups(const JsonValue& value, const CatalogIdentity& identity)
{
    std::vector<UserGroup> groups;
    if (value.kind != JsonValue::Kind::Array) {
        return groups;
    }
    for (const JsonValue& item : value.array) {
        if (item.kind != JsonValue::Kind::Object) {
            continue;
        }
        UserGroup group;
        group.id = ReadStringMember(item, "id");
        group.name = ReadStringMember(item, "name");
        group.is_unassigned = ReadBoolMember(item, "is_unassigned", false) || group.id == UnassignedUserGroupId();
        if (group.is_unassigned) {
            group.id = UnassignedUserGroupId();
            group.name = "Unassigned";
        }
        if (group.id.empty() || group.name.empty()) {
            continue;
        }
        if (const JsonValue* references = ObjectMember(item, "marker_references")) {
            group.marker_references = ReadMarkerReferences(*references, identity);
        }
        groups.push_back(std::move(group));
    }
    return groups;
}

std::vector<GroupingView> ReadGroupingViews(const JsonValue& value, const CatalogIdentity& identity)
{
    std::vector<GroupingView> views;
    if (value.kind != JsonValue::Kind::Array) {
        return views;
    }
    for (const JsonValue& item : value.array) {
        if (item.kind != JsonValue::Kind::Object) {
            continue;
        }
        GroupingView view;
        view.id = ReadStringMember(item, "id");
        view.name = ReadStringMember(item, "name");
        view.read_only = ReadBoolMember(item, "read_only", false);
        if (view.id.empty() || view.name.empty() || view.read_only) {
            continue;
        }
        if (const JsonValue* groups = ObjectMember(item, "groups")) {
            view.groups = ReadUserGroups(*groups, identity);
        }
        EnsureUnassignedGroup(view);
        views.push_back(std::move(view));
    }
    return views;
}

std::unordered_set<std::string> ReadExpandedGroupIds(const JsonValue& value)
{
    std::unordered_set<std::string> expanded;
    if (value.kind != JsonValue::Kind::Array) {
        return expanded;
    }
    for (const JsonValue& item : value.array) {
        if (item.kind == JsonValue::Kind::String && !item.string_value.empty()) {
            expanded.insert(item.string_value);
        }
    }
    return expanded;
}

std::unordered_map<std::string, bool> ReadMarkerVisibility(const JsonValue& value)
{
    std::unordered_map<std::string, bool> visibility;
    if (value.kind != JsonValue::Kind::Object) {
        return visibility;
    }
    for (const auto& [marker_id, item] : value.object) {
        if (!marker_id.empty() && item.kind == JsonValue::Kind::Bool) {
            visibility.emplace(marker_id, item.bool_value);
        }
    }
    return visibility;
}

void WriteMarkerReferences(std::ostream& stream, const std::vector<MarkerReference>& references, int indent)
{
    const std::string base(static_cast<std::size_t>(indent), ' ');
    const std::string item_indent(static_cast<std::size_t>(indent + 2), ' ');
    stream << "[";
    if (!references.empty()) {
        stream << "\n";
    }
    for (std::size_t index = 0; index < references.size(); ++index) {
        const MarkerReference& reference = references[index];
        stream << item_indent << "{ \"catalog_identity\": ";
        WriteJsonString(stream, reference.catalog_identity.id);
        stream << ", \"marker_id\": ";
        WriteJsonString(stream, reference.marker_id);
        stream << " }";
        stream << (index + 1 == references.size() ? "\n" : ",\n");
    }
    if (!references.empty()) {
        stream << base;
    }
    stream << "]";
}

void WriteUserGroups(std::ostream& stream, const std::vector<UserGroup>& groups, int indent)
{
    const std::string base(static_cast<std::size_t>(indent), ' ');
    const std::string item_indent(static_cast<std::size_t>(indent + 2), ' ');
    stream << "[";
    if (!groups.empty()) {
        stream << "\n";
    }
    for (std::size_t index = 0; index < groups.size(); ++index) {
        const UserGroup& group = groups[index];
        stream << item_indent << "{\n";
        stream << item_indent << "  \"id\": ";
        WriteJsonString(stream, group.id);
        stream << ",\n";
        stream << item_indent << "  \"name\": ";
        WriteJsonString(stream, group.name);
        stream << ",\n";
        stream << item_indent << "  \"is_unassigned\": " << (group.is_unassigned ? "true" : "false") << ",\n";
        stream << item_indent << "  \"marker_references\": ";
        WriteMarkerReferences(stream, group.marker_references, indent + 2);
        stream << "\n" << item_indent << "}";
        stream << (index + 1 == groups.size() ? "\n" : ",\n");
    }
    if (!groups.empty()) {
        stream << base;
    }
    stream << "]";
}

void WriteGroupingViews(std::ostream& stream, const std::vector<GroupingView>& views, int indent)
{
    const std::string base(static_cast<std::size_t>(indent), ' ');
    const std::string item_indent(static_cast<std::size_t>(indent + 2), ' ');
    std::vector<const GroupingView*> writable_views;
    for (const GroupingView& view : views) {
        if (!view.read_only) {
            writable_views.push_back(&view);
        }
    }

    stream << "[";
    if (!writable_views.empty()) {
        stream << "\n";
    }
    for (std::size_t index = 0; index < writable_views.size(); ++index) {
        const GroupingView& view = *writable_views[index];
        stream << item_indent << "{\n";
        stream << item_indent << "  \"id\": ";
        WriteJsonString(stream, view.id);
        stream << ",\n";
        stream << item_indent << "  \"name\": ";
        WriteJsonString(stream, view.name);
        stream << ",\n";
        stream << item_indent << "  \"groups\": ";
        WriteUserGroups(stream, view.groups, indent + 2);
        stream << "\n" << item_indent << "}";
        stream << (index + 1 == writable_views.size() ? "\n" : ",\n");
    }
    if (!writable_views.empty()) {
        stream << base;
    }
    stream << "]";
}

std::vector<std::string> SortedSetValues(const std::unordered_set<std::string>& values)
{
    std::vector<std::string> sorted(values.begin(), values.end());
    std::sort(sorted.begin(), sorted.end());
    return sorted;
}

}  // namespace

std::filesystem::path DefaultCatalogUserStateCachePath()
{
    return DefaultLocalUserStatePath("spectral-line-grouping-views.json");
}

CatalogUserStateCacheLoadResult LoadCatalogUserStateCache(const std::filesystem::path& path)
{
    CatalogUserStateCacheLoadResult result;
    VersionedJsonCacheLoadResult cache = LoadVersionedJsonCacheFile(
        path,
        kCacheFormatKind,
        {1, kCacheSchemaVersion},
        "spectral-line grouping cache");
    if (!cache.document) {
        result.warning = std::move(cache.warning);
        return result;
    }

    const JsonValue* catalogs = ObjectMember(cache.document->root, "catalogs");
    if (catalogs == nullptr || catalogs->kind != JsonValue::Kind::Object) {
        return result;
    }

    for (const auto& [identity_id, catalog_value] : catalogs->object) {
        if (identity_id.empty() || catalog_value.kind != JsonValue::Kind::Object) {
            continue;
        }

        CatalogIdentity identity;
        identity.id = identity_id;
        identity.display_name = DisplayNameForCatalogIdentity(identity_id);
        CatalogUserState state = MakeCatalogUserState(identity);
        const std::string active_view_id = ReadStringMember(catalog_value, "active_view_id");
        if (!active_view_id.empty()) {
            state.active_view_id = active_view_id;
        }
        if (const JsonValue* marker_visibility = ObjectMember(catalog_value, "marker_visibility")) {
            state.marker_visibility = ReadMarkerVisibility(*marker_visibility);
        }
        if (const JsonValue* expanded = ObjectMember(catalog_value, "expanded_group_ids")) {
            result.cache.catalog_panel_state[identity_id].expanded_group_ids = ReadExpandedGroupIds(*expanded);
        }
        if (const JsonValue* grouping_views = ObjectMember(catalog_value, "grouping_views")) {
            state.grouping_views = ReadGroupingViews(*grouping_views, identity);
        }
        result.cache.catalogs.emplace(identity_id, std::move(state));
    }
    if (const JsonValue* panel_state = ObjectMember(cache.document->root, "catalog_panel_state")) {
        if (panel_state->kind == JsonValue::Kind::Object) {
            for (const auto& [identity_id, panel_value] : panel_state->object) {
                if (identity_id.empty() || panel_value.kind != JsonValue::Kind::Object) {
                    continue;
                }
                if (const JsonValue* expanded = ObjectMember(panel_value, "expanded_group_ids")) {
                    result.cache.catalog_panel_state[identity_id].expanded_group_ids = ReadExpandedGroupIds(*expanded);
                }
            }
        }
    }
    return result;
}

bool SaveCatalogUserStateCache(
    const std::filesystem::path& path,
    const CatalogUserStateCache& cache,
    std::string& error)
{
    error.clear();
    if (path.empty()) {
        error = "spectral-line grouping cache path is empty";
        return false;
    }

    const std::vector<std::string> catalog_keys = SortedCacheKeys(cache.catalogs);
    const std::vector<std::string> panel_keys = SortedCacheKeys(cache.catalog_panel_state);
    return WriteVersionedJsonCacheFile(
        path,
        kCacheFormatKind,
        kCacheSchemaVersion,
        "spectral-line grouping cache",
        [&](std::ostream& stream, std::string&) {
            stream << ",\n";
            stream << "  \"catalogs\": {\n";

            for (std::size_t catalog_index = 0; catalog_index < catalog_keys.size(); ++catalog_index) {
                const std::string& catalog_id = catalog_keys[catalog_index];
                const CatalogUserState& state = cache.catalogs.at(catalog_id);

                stream << "    ";
                WriteJsonString(stream, catalog_id);
                stream << ": {\n";
                stream << "      \"active_view_id\": ";
                WriteJsonString(stream, state.active_view_id);
                stream << ",\n";

                stream << "      \"marker_visibility\": {";
                const std::vector<std::string> marker_ids = SortedCacheKeys(state.marker_visibility);
                if (!marker_ids.empty()) {
                    stream << "\n";
                }
                for (std::size_t index = 0; index < marker_ids.size(); ++index) {
                    const std::string& marker_id = marker_ids[index];
                    stream << "        ";
                    WriteJsonString(stream, marker_id);
                    stream << ": " << (state.marker_visibility.at(marker_id) ? "true" : "false");
                    stream << (index + 1 == marker_ids.size() ? "\n" : ",\n");
                }
                if (!marker_ids.empty()) {
                    stream << "      ";
                }
                stream << "},\n";

                stream << "      \"grouping_views\": ";
                WriteGroupingViews(stream, state.grouping_views, 6);
                stream << "\n    }";
                stream << (catalog_index + 1 == catalog_keys.size() ? "\n" : ",\n");
            }

            stream << "  },\n";
            stream << "  \"catalog_panel_state\": {\n";

            for (std::size_t catalog_index = 0; catalog_index < panel_keys.size(); ++catalog_index) {
                const std::string& catalog_id = panel_keys[catalog_index];
                const CatalogPanelState& state = cache.catalog_panel_state.at(catalog_id);
                stream << "    ";
                WriteJsonString(stream, catalog_id);
                stream << ": {\n";
                stream << "      \"expanded_group_ids\": [";
                const std::vector<std::string> expanded = SortedSetValues(state.expanded_group_ids);
                if (!expanded.empty()) {
                    stream << "\n";
                }
                for (std::size_t index = 0; index < expanded.size(); ++index) {
                    stream << "        ";
                    WriteJsonString(stream, expanded[index]);
                    stream << (index + 1 == expanded.size() ? "\n" : ",\n");
                }
                if (!expanded.empty()) {
                    stream << "      ";
                }
                stream << "]\n";
                stream << "    }";
                stream << (catalog_index + 1 == panel_keys.size() ? "\n" : ",\n");
            }

            stream << "  }\n";
            return true;
        }, &error);
}


}  // namespace specforge
