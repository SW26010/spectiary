#include "overlays/spectral_line_user_state_cache_io.h"

#include "app/local_user_state.h"
#include "app/local_user_state_json.h"
#include "app/local_user_state_paths.h"

#include <algorithm>
#include <cstdint>
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
constexpr int kCacheSchemaVersion = 4;

CatalogUserStateCacheBeforeReplaceHook g_before_replace_hook_for_tests;

CatalogUserStateCacheLoadIssueKind
CatalogCacheLoadIssueKind(
    VersionedJsonCacheLoadIssueKind issue_kind)
{
    switch (issue_kind) {
    case VersionedJsonCacheLoadIssueKind::ReadFailed:
        return CatalogUserStateCacheLoadIssueKind::
            ReadFailed;
    case VersionedJsonCacheLoadIssueKind::InvalidDocument:
        return CatalogUserStateCacheLoadIssueKind::
            InvalidDocument;
    case VersionedJsonCacheLoadIssueKind::
        UnsupportedFormatOrSchema:
        return CatalogUserStateCacheLoadIssueKind::
            UnsupportedFormatOrSchema;
    case VersionedJsonCacheLoadIssueKind::None:
        break;
    }
    return CatalogUserStateCacheLoadIssueKind::None;
}

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

GeneratedNameSource ReadGeneratedNameSource(
    const JsonValue& value)
{
    const std::string source =
        ReadStringMember(value, "name_source");
    if (source == "catalog_grouping_view") {
        return GeneratedNameSource::CatalogGroupingView;
    }
    if (source == "default_grouping_view") {
        return GeneratedNameSource::DefaultGroupingView;
    }
    if (source == "default_group") {
        return GeneratedNameSource::DefaultGroup;
    }
    return GeneratedNameSource::None;
}

GeneratedNameMetadata ReadGeneratedNameMetadata(
    const JsonValue& value)
{
    GeneratedNameMetadata result;
    result.source = ReadGeneratedNameSource(value);
    result.ordinal =
        ReadJsonSizeMember(value, "name_ordinal")
            .value_or(0);
    result.copy_count =
        ReadJsonSizeMember(value, "generated_copy_count")
            .value_or(0);
    result.copy_base_name =
        ReadStringMember(
            value,
            "generated_copy_base_name");
    return result;
}

std::string_view JsonKindDescription(
    JsonValue::Kind kind)
{
    switch (kind) {
    case JsonValue::Kind::Null:
        return "null";
    case JsonValue::Kind::Object:
        return "an object";
    case JsonValue::Kind::Array:
        return "an array";
    case JsonValue::Kind::String:
        return "a string";
    case JsonValue::Kind::Bool:
        return "a boolean";
    case JsonValue::Kind::Integer:
        return "an integer";
    }
    return "the expected JSON type";
}

std::optional<std::string> ValidateRequiredMemberKind(
    const JsonValue& object,
    std::string_view member_name,
    JsonValue::Kind expected_kind,
    std::string_view object_path)
{
    const JsonValue* member =
        ObjectMember(object, member_name);
    const std::string member_path =
        std::string(object_path) + "." +
        std::string(member_name);
    if (member == nullptr) {
        return member_path + " is missing";
    }
    if (member->kind != expected_kind) {
        return member_path + " must be " +
            std::string(
                   JsonKindDescription(
                       expected_kind));
    }
    return std::nullopt;
}

std::optional<std::string> ValidateOptionalMemberKind(
    const JsonValue& object,
    std::string_view member_name,
    JsonValue::Kind expected_kind,
    std::string_view object_path)
{
    const JsonValue* member =
        ObjectMember(object, member_name);
    if (member == nullptr) {
        return std::nullopt;
    }
    if (member->kind != expected_kind) {
        return std::string(object_path) + "." +
            std::string(member_name) +
            " must be " +
            std::string(
                   JsonKindDescription(
                       expected_kind));
    }
    return std::nullopt;
}

std::optional<std::string>
ValidateGeneratedNameMetadataShape(
    const JsonValue& object,
    std::string_view object_path)
{
    if (std::optional<std::string> issue =
            ValidateOptionalMemberKind(
                object,
                "name_source",
                JsonValue::Kind::String,
                object_path)) {
        return issue;
    }
    if (const JsonValue* source =
            ObjectMember(object, "name_source");
        source != nullptr &&
        source->string_value !=
            "catalog_grouping_view" &&
        source->string_value !=
            "default_grouping_view" &&
        source->string_value !=
            "default_group") {
        return std::string(object_path) +
            ".name_source has an unsupported value";
    }
    for (const std::string_view member_name :
         {"name_ordinal",
          "generated_copy_count"}) {
        if (std::optional<std::string> issue =
                ValidateOptionalMemberKind(
                    object,
                    member_name,
                    JsonValue::Kind::Integer,
                    object_path)) {
            return issue;
        }
        if (const JsonValue* member =
                ObjectMember(
                    object,
                    member_name);
            member != nullptr &&
            member->integer_value < 0) {
            return std::string(object_path) +
                "." +
                std::string(member_name) +
                " must not be negative";
        }
        if (member_name ==
                "generated_copy_count") {
            if (const JsonValue* member =
                    ObjectMember(
                        object,
                        member_name);
                member != nullptr &&
                member->integer_value >
                    static_cast<std::int64_t>(
                        kMaximumGeneratedNameCopyCount)) {
                return std::string(object_path) +
                    ".generated_copy_count must not exceed " +
                    std::to_string(
                        kMaximumGeneratedNameCopyCount);
            }
        }
    }
    return ValidateOptionalMemberKind(
        object,
        "generated_copy_base_name",
        JsonValue::Kind::String,
        object_path);
}

std::optional<std::string> ValidateStringArrayShape(
    const JsonValue& value,
    std::string_view array_path,
    bool require_unique = false)
{
    if (value.kind != JsonValue::Kind::Array) {
        return std::string(array_path) +
            " must be an array";
    }
    std::unordered_set<std::string> values;
    for (std::size_t index = 0;
         index < value.array.size();
         ++index) {
        if (value.array[index].kind !=
            JsonValue::Kind::String) {
            return std::string(array_path) +
                "[" + std::to_string(index) +
                "] must be a string";
        }
        if (require_unique &&
            !values.insert(value.array[index].string_value).second) {
            return std::string(array_path) +
                " contains duplicate identity " +
                value.array[index].string_value;
        }
    }
    return std::nullopt;
}

std::optional<std::string>
ValidateMarkerReferencesShape(
    const JsonValue& value,
    std::string_view references_path)
{
    if (value.kind != JsonValue::Kind::Array) {
        return std::string(references_path) +
            " must be an array";
    }
    for (std::size_t index = 0;
         index < value.array.size();
         ++index) {
        const JsonValue& reference =
            value.array[index];
        const std::string reference_path =
            std::string(references_path) + "[" +
            std::to_string(index) + "]";
        if (reference.kind ==
            JsonValue::Kind::String) {
            continue;
        }
        if (reference.kind !=
            JsonValue::Kind::Object) {
            return reference_path +
                " must be a string or an object";
        }
        if (std::optional<std::string> issue =
                ValidateRequiredMemberKind(
                    reference,
                    "marker_id",
                    JsonValue::Kind::String,
                    reference_path)) {
            return issue;
        }
        if (std::optional<std::string> issue =
                ValidateOptionalMemberKind(
                    reference,
                    "catalog_identity",
                    JsonValue::Kind::String,
                    reference_path)) {
            return issue;
        }
    }
    return std::nullopt;
}

std::optional<std::string> ValidateUserGroupsShape(
    const JsonValue& value,
    std::string_view groups_path,
    bool require_unassigned_flag)
{
    if (value.kind != JsonValue::Kind::Array) {
        return std::string(groups_path) +
            " must be an array";
    }
    for (std::size_t index = 0;
         index < value.array.size();
         ++index) {
        const JsonValue& group = value.array[index];
        const std::string group_path =
            std::string(groups_path) + "[" +
            std::to_string(index) + "]";
        if (group.kind !=
            JsonValue::Kind::Object) {
            return group_path +
                " must be an object";
        }
        for (const std::string_view member_name :
             {"id", "name"}) {
            if (std::optional<std::string> issue =
                    ValidateRequiredMemberKind(
                        group,
                        member_name,
                        JsonValue::Kind::String,
                        group_path)) {
                return issue;
            }
        }
        if (std::optional<std::string> issue =
                (require_unassigned_flag
                     ? ValidateRequiredMemberKind(
                           group,
                           "is_unassigned",
                           JsonValue::Kind::Bool,
                           group_path)
                     : ValidateOptionalMemberKind(
                           group,
                           "is_unassigned",
                           JsonValue::Kind::Bool,
                           group_path))) {
            return issue;
        }
        if (std::optional<std::string> issue =
                ValidateGeneratedNameMetadataShape(
                    group,
                    group_path)) {
            return issue;
        }
        const JsonValue* references =
            ObjectMember(
                group,
                "marker_references");
        if (references == nullptr) {
            return group_path +
                ".marker_references is missing";
        }
        if (std::optional<std::string> issue =
                ValidateMarkerReferencesShape(
                    *references,
                    group_path +
                        ".marker_references")) {
            return issue;
        }
    }
    return std::nullopt;
}

std::optional<std::string>
ValidateGroupingViewsShape(
    const JsonValue& value,
    std::string_view views_path,
    bool require_unassigned_flag)
{
    if (value.kind != JsonValue::Kind::Array) {
        return std::string(views_path) +
            " must be an array";
    }
    for (std::size_t index = 0;
         index < value.array.size();
         ++index) {
        const JsonValue& view = value.array[index];
        const std::string view_path =
            std::string(views_path) + "[" +
            std::to_string(index) + "]";
        if (view.kind !=
            JsonValue::Kind::Object) {
            return view_path +
                " must be an object";
        }
        for (const std::string_view member_name :
             {"id", "name"}) {
            if (std::optional<std::string> issue =
                    ValidateRequiredMemberKind(
                        view,
                        member_name,
                        JsonValue::Kind::String,
                        view_path)) {
                return issue;
            }
        }
        if (std::optional<std::string> issue =
                ValidateOptionalMemberKind(
                    view,
                    "read_only",
                    JsonValue::Kind::Bool,
                    view_path)) {
            return issue;
        }
        if (std::optional<std::string> issue =
                ValidateGeneratedNameMetadataShape(
                    view,
                    view_path)) {
            return issue;
        }
        const JsonValue* groups =
            ObjectMember(view, "groups");
        if (groups == nullptr) {
            return view_path +
                ".groups is missing";
        }
        if (std::optional<std::string> issue =
                ValidateUserGroupsShape(
                    *groups,
                    view_path + ".groups",
                    require_unassigned_flag)) {
            return issue;
        }
    }
    return std::nullopt;
}

std::optional<std::string>
ValidateCatalogCacheBodyShape(
    const JsonValue& catalogs,
    const JsonValue* panel_state,
    bool require_unassigned_flag,
    bool require_allocator_history)
{
    for (const auto& [identity_id, catalog] :
         catalogs.object) {
        if (identity_id.empty()) {
            return "catalogs contains an empty catalog identity key";
        }
        const std::string catalog_path =
            "catalogs[" + identity_id + "]";
        if (catalog.kind !=
            JsonValue::Kind::Object) {
            return catalog_path +
                " must be an object";
        }
        if (std::optional<std::string> issue =
                ValidateRequiredMemberKind(
                    catalog,
                    "active_view_id",
                    JsonValue::Kind::String,
                    catalog_path)) {
            return issue;
        }
        for (const std::string_view member_name :
             {"next_view_sequence", "next_group_sequence"}) {
            if (std::optional<std::string> issue =
                    (require_allocator_history
                         ? ValidateRequiredMemberKind(
                               catalog,
                               member_name,
                               JsonValue::Kind::Integer,
                               catalog_path)
                         : ValidateOptionalMemberKind(
                               catalog,
                               member_name,
                               JsonValue::Kind::Integer,
                               catalog_path))) {
                return issue;
            }
            if (const JsonValue* member =
                    ObjectMember(catalog, member_name);
                member != nullptr && member->integer_value < 0) {
                return catalog_path + "." +
                       std::string(member_name) +
                       " must not be negative";
            }
        }
        for (const std::string_view member_name :
             {"reserved_view_ids", "reserved_group_ids"}) {
            const JsonValue* member =
                ObjectMember(catalog, member_name);
            if (member == nullptr && require_allocator_history) {
                return catalog_path + "." +
                       std::string(member_name) +
                       " is missing";
            }
            if (member != nullptr) {
                if (std::optional<std::string> issue =
                        ValidateStringArrayShape(
                            *member,
                            catalog_path + "." +
                                std::string(member_name),
                            require_allocator_history)) {
                    return issue;
                }
            }
        }
        const JsonValue* marker_visibility =
            ObjectMember(
                catalog,
                "marker_visibility");
        if (marker_visibility == nullptr) {
            return catalog_path +
                ".marker_visibility is missing";
        }
        if (marker_visibility->kind !=
            JsonValue::Kind::Object) {
            return catalog_path +
                ".marker_visibility must be an object";
        }
        for (const auto& [marker_id, visible] :
             marker_visibility->object) {
            if (visible.kind !=
                JsonValue::Kind::Bool) {
                return catalog_path +
                    ".marker_visibility[" +
                    marker_id +
                    "] must be a boolean";
            }
        }
        if (const JsonValue* expanded =
                ObjectMember(
                    catalog,
                    "expanded_group_ids");
            expanded != nullptr) {
            if (std::optional<std::string> issue =
                    ValidateStringArrayShape(
                        *expanded,
                        catalog_path +
                            ".expanded_group_ids")) {
                return issue;
            }
        }
        const JsonValue* grouping_views =
            ObjectMember(
                catalog,
                "grouping_views");
        if (grouping_views == nullptr) {
            return catalog_path +
                ".grouping_views is missing";
        }
        if (std::optional<std::string> issue =
                ValidateGroupingViewsShape(
                    *grouping_views,
                    catalog_path +
                        ".grouping_views",
                    require_unassigned_flag)) {
            return issue;
        }
    }

    if (panel_state == nullptr) {
        return std::nullopt;
    }
    for (const auto& [identity_id, panel] :
         panel_state->object) {
        if (identity_id.empty()) {
            return "catalog_panel_state contains an empty catalog identity key";
        }
        const std::string panel_path =
            "catalog_panel_state[" +
            identity_id + "]";
        if (panel.kind !=
            JsonValue::Kind::Object) {
            return panel_path +
                " must be an object";
        }
        const JsonValue* expanded =
            ObjectMember(
                panel,
                "expanded_group_ids");
        if (expanded == nullptr) {
            return panel_path +
                ".expanded_group_ids is missing";
        }
        if (std::optional<std::string> issue =
                ValidateStringArrayShape(
                    *expanded,
                    panel_path +
                        ".expanded_group_ids")) {
            return issue;
        }
    }
    return std::nullopt;
}

std::string_view GeneratedNameSourceValue(
    GeneratedNameSource source)
{
    switch (source) {
    case GeneratedNameSource::CatalogGroupingView:
        return "catalog_grouping_view";
    case GeneratedNameSource::DefaultGroupingView:
        return "default_grouping_view";
    case GeneratedNameSource::DefaultGroup:
        return "default_group";
    case GeneratedNameSource::None:
        break;
    }
    return {};
}

void WriteGeneratedNameMetadataMembers(
    std::ostream& stream,
    const GeneratedNameMetadata& metadata,
    std::string_view item_indent)
{
    const std::string_view source =
        GeneratedNameSourceValue(metadata.source);
    if (!source.empty()) {
        stream << ",\n" << item_indent << "  \"name_source\": ";
        WriteJsonString(stream, source);
    }
    if (metadata.ordinal != 0) {
        stream << ",\n" << item_indent << "  \"name_ordinal\": "
               << metadata.ordinal;
    }
    if (metadata.copy_count != 0) {
        stream << ",\n" << item_indent
               << "  \"generated_copy_count\": "
               << metadata.copy_count;
    }
    if (!metadata.copy_base_name.empty()) {
        stream << ",\n" << item_indent
               << "  \"generated_copy_base_name\": ";
        WriteJsonString(
            stream,
            metadata.copy_base_name);
    }
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

    const JsonValue* marker_id = ObjectMember(value, "marker_id");
    if (marker_id == nullptr || marker_id->kind != JsonValue::Kind::String) {
        return std::nullopt;
    }
    CatalogIdentity identity = fallback_identity;
    if (const JsonValue* identity_id =
            ObjectMember(value, "catalog_identity");
        identity_id != nullptr &&
        identity_id->kind == JsonValue::Kind::String) {
        identity.id = identity_id->string_value;
        identity.display_name =
            DisplayNameForCatalogIdentity(identity.id);
    }
    return MakeReference(identity, marker_id->string_value);
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

std::vector<UserGroup> ReadUserGroups(
    const JsonValue& value,
    const CatalogIdentity& identity,
    bool read_generated_name_metadata)
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
        if (read_generated_name_metadata) {
            group.generated_name =
                ReadGeneratedNameMetadata(item);
        }
        group.is_unassigned =
            ReadBoolMember(item, "is_unassigned", false);
        if (const JsonValue* references = ObjectMember(item, "marker_references")) {
            group.marker_references = ReadMarkerReferences(*references, identity);
        }
        groups.push_back(std::move(group));
    }
    return groups;
}

std::vector<GroupingView> ReadGroupingViews(
    const JsonValue& value,
    const CatalogIdentity& identity,
    bool read_generated_name_metadata)
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
        if (read_generated_name_metadata) {
            view.generated_name =
                ReadGeneratedNameMetadata(item);
        }
        view.read_only = ReadBoolMember(item, "read_only", false);
        if (const JsonValue* groups = ObjectMember(item, "groups")) {
            view.groups =
                ReadUserGroups(
                    *groups,
                    identity,
                    read_generated_name_metadata);
        }
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
        if (item.kind == JsonValue::Kind::String) {
            expanded.insert(item.string_value);
        }
    }
    return expanded;
}

std::unordered_set<std::string> ReadStringSet(const JsonValue& value)
{
    std::unordered_set<std::string> result;
    if (value.kind != JsonValue::Kind::Array) {
        return result;
    }
    for (const JsonValue& item : value.array) {
        if (item.kind == JsonValue::Kind::String) {
            result.insert(item.string_value);
        }
    }
    return result;
}

std::unordered_map<std::string, bool> ReadMarkerVisibility(const JsonValue& value)
{
    std::unordered_map<std::string, bool> visibility;
    if (value.kind != JsonValue::Kind::Object) {
        return visibility;
    }
    for (const auto& [marker_id, item] : value.object) {
        if (item.kind == JsonValue::Kind::Bool) {
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
        WriteGeneratedNameMetadataMembers(
            stream,
            group.generated_name,
            item_indent);
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
        WriteGeneratedNameMetadataMembers(
            stream,
            view.generated_name,
            item_indent);
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
    return DefaultLocalUserStatePath(
        local_user_state_paths::kSpectralLineUserState);
}

std::filesystem::path CatalogUserStateCacheCommitLeasePath(
    const std::filesystem::path& path)
{
    if (path.empty()) {
        return {};
    }
    std::filesystem::path lease_path = path;
#if defined(_WIN32)
    lease_path += std::filesystem::path::string_type(L".commit.lock");
#else
    lease_path += std::filesystem::path::string_type(".commit.lock");
#endif
    return lease_path;
}

void SetCatalogUserStateCacheBeforeReplaceHookForTests(
    CatalogUserStateCacheBeforeReplaceHook hook)
{
    g_before_replace_hook_for_tests = std::move(hook);
}

CatalogUserStateCacheLoadResult LoadCatalogUserStateCache(const std::filesystem::path& path)
{
    CatalogUserStateCacheLoadResult result;
    VersionedJsonCacheLoadResult cache = LoadVersionedJsonCacheFile(
        path,
        kCacheFormatKind,
        {1, 2, 3, kCacheSchemaVersion},
        "spectral-line grouping cache");
    if (!cache.document) {
        result.issue_kind =
            CatalogCacheLoadIssueKind(
                cache.issue_kind);
        result.diagnostic_detail =
            std::move(cache.diagnostic_detail);
        result.warning =
            result.diagnostic_detail.empty()
                ? std::move(cache.warning)
                : result.diagnostic_detail;
        return result;
    }
    result.schema_version = cache.document->schema_version;

    const JsonValue* catalogs = ObjectMember(cache.document->root, "catalogs");
    if (catalogs == nullptr || catalogs->kind != JsonValue::Kind::Object) {
        result.issue_kind =
            CatalogUserStateCacheLoadIssueKind::
                InvalidDocument;
        result.diagnostic_detail =
            catalogs == nullptr
                ? "missing object member \"catalogs\""
                : "member \"catalogs\" must be an object";
        result.warning =
            "Ignored invalid spectral-line grouping cache.";
        return result;
    }
    const JsonValue* panel_state =
        ObjectMember(
            cache.document->root,
            "catalog_panel_state");
    if (panel_state != nullptr &&
        panel_state->kind != JsonValue::Kind::Object) {
        result.issue_kind =
            CatalogUserStateCacheLoadIssueKind::
                InvalidDocument;
        result.diagnostic_detail =
            "member \"catalog_panel_state\" must be an object";
        result.warning =
            "Ignored invalid spectral-line grouping cache.";
        return result;
    }
    if (std::optional<std::string> body_issue =
            ValidateCatalogCacheBodyShape(
                *catalogs,
                panel_state,
                cache.document->schema_version >= 3,
                cache.document->schema_version ==
                    kCacheSchemaVersion)) {
        result.issue_kind =
            CatalogUserStateCacheLoadIssueKind::
                InvalidDocument;
        result.diagnostic_detail =
            std::move(*body_issue);
        result.warning =
            "Ignored invalid spectral-line grouping cache.";
        return result;
    }
    result.requires_save =
        cache.document->schema_version <
        kCacheSchemaVersion;

    for (const auto& [identity_id, catalog_value] : catalogs->object) {
        if (identity_id.empty() || catalog_value.kind != JsonValue::Kind::Object) {
            continue;
        }

        CatalogIdentity identity;
        identity.id = identity_id;
        identity.display_name = DisplayNameForCatalogIdentity(identity_id);
        CatalogUserState state;
        state.catalog_identity = identity;
        if (const JsonValue* active_view_id =
                ObjectMember(catalog_value, "active_view_id");
            active_view_id != nullptr &&
            active_view_id->kind == JsonValue::Kind::String) {
            state.active_view_id = active_view_id->string_value;
        }
        state.next_view_sequence = static_cast<std::uint64_t>(
            ReadJsonSizeMember(catalog_value, "next_view_sequence")
                .value_or(1));
        state.next_group_sequence = static_cast<std::uint64_t>(
            ReadJsonSizeMember(catalog_value, "next_group_sequence")
                .value_or(1));
        if (const JsonValue* reserved_view_ids =
                ObjectMember(catalog_value, "reserved_view_ids")) {
            state.reserved_view_ids = ReadStringSet(*reserved_view_ids);
        }
        if (const JsonValue* reserved_group_ids =
                ObjectMember(catalog_value, "reserved_group_ids")) {
            state.reserved_group_ids = ReadStringSet(*reserved_group_ids);
        }
        if (const JsonValue* marker_visibility = ObjectMember(catalog_value, "marker_visibility")) {
            state.marker_visibility = ReadMarkerVisibility(*marker_visibility);
        }
        if (const JsonValue* expanded = ObjectMember(catalog_value, "expanded_group_ids")) {
            result.cache.catalog_panel_state[identity_id].expanded_group_ids = ReadExpandedGroupIds(*expanded);
        }
        if (const JsonValue* grouping_views = ObjectMember(catalog_value, "grouping_views")) {
            state.grouping_views =
                ReadGroupingViews(
                    *grouping_views,
                    identity,
                    cache.document->schema_version >= 3);
        }
        result.cache.catalogs.emplace(identity_id, std::move(state));
    }
    if (panel_state != nullptr) {
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
                stream << "      \"next_view_sequence\": "
                       << state.next_view_sequence << ",\n";
                stream << "      \"next_group_sequence\": "
                       << state.next_group_sequence << ",\n";

                stream << "      \"reserved_view_ids\": [";
                const std::vector<std::string> reserved_view_ids =
                    SortedSetValues(state.reserved_view_ids);
                for (std::size_t index = 0;
                     index < reserved_view_ids.size();
                     ++index) {
                    if (index != 0) {
                        stream << ", ";
                    }
                    WriteJsonString(stream, reserved_view_ids[index]);
                }
                stream << "],\n";
                stream << "      \"reserved_group_ids\": [";
                const std::vector<std::string> reserved_group_ids =
                    SortedSetValues(state.reserved_group_ids);
                for (std::size_t index = 0;
                     index < reserved_group_ids.size();
                     ++index) {
                    if (index != 0) {
                        stream << ", ";
                    }
                    WriteJsonString(stream, reserved_group_ids[index]);
                }
                stream << "],\n";

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
        },
        &error,
        g_before_replace_hook_for_tests);
}


}  // namespace specforge
