#include "overlays/spectral_line_user_state_cache_io.h"

#include "app/local_user_state.h"
#include "app/local_user_state_json.h"
#include "app/local_user_state_paths.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace specforge {
namespace {

constexpr const char* kCacheFormatKind = "specforge.catalog_user_state.cache";
constexpr int kCacheSchemaVersion = 6;
constexpr std::string_view kExplicitColorMode =
    "explicit-color";

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

const nlohmann::json* ObjectMember(const nlohmann::json& value, std::string_view key)
{
    return JsonObjectMember(value, key);
}

std::string ReadStringMember(const nlohmann::json& value, std::string_view key)
{
    return ReadJsonStringMember(value, key).value_or(std::string{});
}

bool ReadBoolMember(const nlohmann::json& value, std::string_view key, bool fallback)
{
    return ReadJsonBoolMember(value, key, fallback);
}

std::optional<double> ParseFiniteDouble(
    const nlohmann::json& object,
    std::string_view key)
{
    const std::optional<std::string> text =
        ReadJsonStringMember(object, key);
    if (!text || text->empty()) {
        return std::nullopt;
    }
    double value = 0.0;
    const std::from_chars_result parsed = std::from_chars(
        text->data(),
        text->data() + text->size(),
        value,
        std::chars_format::general);
    if (parsed.ec != std::errc{} ||
        parsed.ptr != text->data() + text->size() ||
        !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::string> EncodeFiniteFloat(float value)
{
    if (!std::isfinite(value)) {
        return std::nullopt;
    }
    std::array<char, 64> buffer{};
    const std::to_chars_result encoded = std::to_chars(
        buffer.data(),
        buffer.data() + buffer.size(),
        value,
        std::chars_format::general,
        std::numeric_limits<float>::max_digits10);
    if (encoded.ec != std::errc{}) {
        return std::nullopt;
    }
    return std::string(buffer.data(), encoded.ptr);
}

bool EncodedColorChannelIsUsable(
    const std::optional<double>& value)
{
    return value && *value >= 0.0 && *value <= 1.0;
}

GeneratedNameSource ReadGeneratedNameSource(
    const nlohmann::json& value)
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
    const nlohmann::json& value)
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
    nlohmann::json::value_t kind)
{
    switch (kind) {
    case nlohmann::json::value_t::null:
        return "null";
    case nlohmann::json::value_t::object:
        return "an object";
    case nlohmann::json::value_t::array:
        return "an array";
    case nlohmann::json::value_t::string:
        return "a string";
    case nlohmann::json::value_t::boolean:
        return "a boolean";
    case nlohmann::json::value_t::number_integer:
        return "an integer";
    }
    return "the expected JSON type";
}

std::optional<std::string> ValidateRequiredMemberKind(
    const nlohmann::json& object,
    std::string_view member_name,
    nlohmann::json::value_t expected_kind,
    std::string_view object_path)
{
    const nlohmann::json* member =
        ObjectMember(object, member_name);
    const std::string member_path =
        std::string(object_path) + "." +
        std::string(member_name);
    if (member == nullptr) {
        return member_path + " is missing";
    }
    if ((expected_kind == nlohmann::json::value_t::number_integer
            ? !JsonIsInt64(*member) : member->type() != expected_kind)) {
        return member_path + " must be " +
            std::string(
                   JsonKindDescription(
                       expected_kind));
    }
    return std::nullopt;
}

std::optional<std::string> ValidateOptionalMemberKind(
    const nlohmann::json& object,
    std::string_view member_name,
    nlohmann::json::value_t expected_kind,
    std::string_view object_path)
{
    const nlohmann::json* member =
        ObjectMember(object, member_name);
    if (member == nullptr) {
        return std::nullopt;
    }
    if ((expected_kind == nlohmann::json::value_t::number_integer
            ? !JsonIsInt64(*member) : member->type() != expected_kind)) {
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
    const nlohmann::json& object,
    std::string_view object_path)
{
    if (std::optional<std::string> issue =
            ValidateOptionalMemberKind(
                object,
                "name_source",
                nlohmann::json::value_t::string,
                object_path)) {
        return issue;
    }
    if (const nlohmann::json* source =
            ObjectMember(object, "name_source");
        source != nullptr &&
        source->get_ref<const std::string&>() !=
            "catalog_grouping_view" &&
        source->get_ref<const std::string&>() !=
            "default_grouping_view" &&
        source->get_ref<const std::string&>() !=
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
                    nlohmann::json::value_t::number_integer,
                    object_path)) {
            return issue;
        }
        if (const nlohmann::json* member =
                ObjectMember(
                    object,
                    member_name);
            member != nullptr &&
            member->get<std::int64_t>() < 0) {
            return std::string(object_path) +
                "." +
                std::string(member_name) +
                " must not be negative";
        }
        if (member_name ==
                "generated_copy_count") {
            if (const nlohmann::json* member =
                    ObjectMember(
                        object,
                        member_name);
                member != nullptr &&
                member->get<std::int64_t>() >
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
        nlohmann::json::value_t::string,
        object_path);
}

std::optional<std::string> ValidateStringArrayShape(
    const nlohmann::json& value,
    std::string_view array_path,
    bool require_unique = false)
{
    if (value.type() != nlohmann::json::value_t::array) {
        return std::string(array_path) +
            " must be an array";
    }
    std::unordered_set<std::string> values;
    for (std::size_t index = 0;
         index < value.size();
         ++index) {
        if (value[index].type() !=
            nlohmann::json::value_t::string) {
            return std::string(array_path) +
                "[" + std::to_string(index) +
                "] must be a string";
        }
        if (require_unique &&
            !values.insert(value[index].get_ref<const std::string&>()).second) {
            return std::string(array_path) +
                " contains duplicate identity " +
                value[index].get_ref<const std::string&>();
        }
    }
    return std::nullopt;
}

std::optional<std::string>
ValidateMarkerReferencesShape(
    const nlohmann::json& value,
    std::string_view references_path)
{
    if (value.type() != nlohmann::json::value_t::array) {
        return std::string(references_path) +
            " must be an array";
    }
    for (std::size_t index = 0;
         index < value.size();
         ++index) {
        const nlohmann::json& reference =
            value[index];
        const std::string reference_path =
            std::string(references_path) + "[" +
            std::to_string(index) + "]";
        if (reference.type() ==
            nlohmann::json::value_t::string) {
            continue;
        }
        if (reference.type() !=
            nlohmann::json::value_t::object) {
            return reference_path +
                " must be a string or an object";
        }
        if (std::optional<std::string> issue =
                ValidateRequiredMemberKind(
                    reference,
                    "marker_id",
                    nlohmann::json::value_t::string,
                    reference_path)) {
            return issue;
        }
        if (std::optional<std::string> issue =
                ValidateOptionalMemberKind(
                    reference,
                    "catalog_identity",
                    nlohmann::json::value_t::string,
                    reference_path)) {
            return issue;
        }
    }
    return std::nullopt;
}

std::optional<std::string> ValidateUserGroupsShape(
    const nlohmann::json& value,
    std::string_view groups_path,
    bool require_unassigned_flag)
{
    if (value.type() != nlohmann::json::value_t::array) {
        return std::string(groups_path) +
            " must be an array";
    }
    for (std::size_t index = 0;
         index < value.size();
         ++index) {
        const nlohmann::json& group = value[index];
        const std::string group_path =
            std::string(groups_path) + "[" +
            std::to_string(index) + "]";
        if (group.type() !=
            nlohmann::json::value_t::object) {
            return group_path +
                " must be an object";
        }
        for (const std::string_view member_name :
             {"id", "name"}) {
            if (std::optional<std::string> issue =
                    ValidateRequiredMemberKind(
                        group,
                        member_name,
                        nlohmann::json::value_t::string,
                        group_path)) {
                return issue;
            }
        }
        if (std::optional<std::string> issue =
                (require_unassigned_flag
                     ? ValidateRequiredMemberKind(
                           group,
                           "is_unassigned",
                           nlohmann::json::value_t::boolean,
                           group_path)
                     : ValidateOptionalMemberKind(
                           group,
                           "is_unassigned",
                           nlohmann::json::value_t::boolean,
                           group_path))) {
            return issue;
        }
        if (std::optional<std::string> issue =
                ValidateGeneratedNameMetadataShape(
                    group,
                    group_path)) {
            return issue;
        }
        const nlohmann::json* references =
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
    const nlohmann::json& value,
    std::string_view views_path,
    bool require_unassigned_flag)
{
    if (value.type() != nlohmann::json::value_t::array) {
        return std::string(views_path) +
            " must be an array";
    }
    for (std::size_t index = 0;
         index < value.size();
         ++index) {
        const nlohmann::json& view = value[index];
        const std::string view_path =
            std::string(views_path) + "[" +
            std::to_string(index) + "]";
        if (view.type() !=
            nlohmann::json::value_t::object) {
            return view_path +
                " must be an object";
        }
        for (const std::string_view member_name :
             {"id", "name"}) {
            if (std::optional<std::string> issue =
                    ValidateRequiredMemberKind(
                        view,
                        member_name,
                        nlohmann::json::value_t::string,
                        view_path)) {
                return issue;
            }
        }
        if (std::optional<std::string> issue =
                ValidateOptionalMemberKind(
                    view,
                    "read_only",
                    nlohmann::json::value_t::boolean,
                    view_path)) {
            return issue;
        }
        if (std::optional<std::string> issue =
                ValidateGeneratedNameMetadataShape(
                    view,
                    view_path)) {
            return issue;
        }
        const nlohmann::json* groups =
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
    const nlohmann::json& catalogs,
    const nlohmann::json* panel_state,
    bool require_unassigned_flag,
    bool require_marker_colors)
{
    for (const auto& [identity_id, catalog] :
         catalogs.get_ref<const nlohmann::json::object_t&>()) {
        if (identity_id.empty()) {
            return "catalogs contains an empty catalog identity key";
        }
        const std::string catalog_path =
            "catalogs[" + identity_id + "]";
        if (catalog.type() !=
            nlohmann::json::value_t::object) {
            return catalog_path +
                " must be an object";
        }
        if (std::optional<std::string> issue =
                ValidateRequiredMemberKind(
                    catalog,
                    "active_view_id",
                    nlohmann::json::value_t::string,
                    catalog_path)) {
            return issue;
        }
        const nlohmann::json* marker_visibility =
            ObjectMember(
                catalog,
                "marker_visibility");
        if (marker_visibility == nullptr) {
            return catalog_path +
                ".marker_visibility is missing";
        }
        if (marker_visibility->type() !=
            nlohmann::json::value_t::object) {
            return catalog_path +
                ".marker_visibility must be an object";
        }
        for (const auto& [marker_id, visible] :
             marker_visibility->get_ref<const nlohmann::json::object_t&>()) {
            if (visible.type() !=
                nlohmann::json::value_t::boolean) {
                return catalog_path +
                    ".marker_visibility[" +
                    marker_id +
                    "] must be a boolean";
            }
        }
        const nlohmann::json* marker_colors =
            ObjectMember(catalog, "marker_colors");
        if (marker_colors == nullptr && require_marker_colors) {
            return catalog_path + ".marker_colors is missing";
        }
        if (marker_colors != nullptr) {
            if (marker_colors->type() != nlohmann::json::value_t::object) {
                return catalog_path +
                    ".marker_colors must be an object";
            }
            for (const auto& [marker_id, encoded] :
                 marker_colors->get_ref<const nlohmann::json::object_t&>()) {
                const std::string color_path =
                    catalog_path + ".marker_colors[" +
                    marker_id + "]";
                if (marker_id.empty()) {
                    return catalog_path +
                        ".marker_colors contains an empty marker identity key";
                }
                if (encoded.type() != nlohmann::json::value_t::object) {
                    return color_path + " must be an object";
                }
                const std::optional<std::string> mode =
                    ReadJsonStringMember(encoded, "mode");
                if (!mode || *mode != kExplicitColorMode) {
                    return color_path +
                        ".mode must be \"explicit-color\"";
                }
                const std::optional<double> red =
                    ParseFiniteDouble(encoded, "red");
                const std::optional<double> green =
                    ParseFiniteDouble(encoded, "green");
                const std::optional<double> blue =
                    ParseFiniteDouble(encoded, "blue");
                const std::optional<double> alpha =
                    ParseFiniteDouble(encoded, "alpha");
                if (!EncodedColorChannelIsUsable(red) ||
                    !EncodedColorChannelIsUsable(green) ||
                    !EncodedColorChannelIsUsable(blue) ||
                    !EncodedColorChannelIsUsable(alpha)) {
                    return color_path +
                        " RGBA channels must be finite strings from zero to one";
                }
            }
        }
        if (const nlohmann::json* expanded =
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
        const nlohmann::json* grouping_views =
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
         panel_state->get_ref<const nlohmann::json::object_t&>()) {
        if (identity_id.empty()) {
            return "catalog_panel_state contains an empty catalog identity key";
        }
        const std::string panel_path =
            "catalog_panel_state[" +
            identity_id + "]";
        if (panel.type() !=
            nlohmann::json::value_t::object) {
            return panel_path +
                " must be an object";
        }
        const nlohmann::json* expanded =
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
    const nlohmann::json& value,
    const CatalogIdentity& fallback_identity)
{
    if (value.type() == nlohmann::json::value_t::string) {
        return MakeReference(fallback_identity, value.get_ref<const std::string&>());
    }
    if (value.type() != nlohmann::json::value_t::object) {
        return std::nullopt;
    }

    const nlohmann::json* marker_id = ObjectMember(value, "marker_id");
    if (marker_id == nullptr || marker_id->type() != nlohmann::json::value_t::string) {
        return std::nullopt;
    }
    CatalogIdentity identity = fallback_identity;
    if (const nlohmann::json* identity_id =
            ObjectMember(value, "catalog_identity");
        identity_id != nullptr &&
        identity_id->type() == nlohmann::json::value_t::string) {
        identity.id = identity_id->get_ref<const std::string&>();
        identity.display_name =
            DisplayNameForCatalogIdentity(identity.id);
    }
    return MakeReference(identity, marker_id->get_ref<const std::string&>());
}

std::vector<MarkerReference> ReadMarkerReferences(
    const nlohmann::json& value,
    const CatalogIdentity& fallback_identity)
{
    std::vector<MarkerReference> references;
    if (value.type() != nlohmann::json::value_t::array) {
        return references;
    }
    for (const nlohmann::json& item : value) {
        if (std::optional<MarkerReference> reference = ReadMarkerReference(item, fallback_identity)) {
            references.push_back(std::move(*reference));
        }
    }
    return references;
}

std::vector<UserGroup> ReadUserGroups(
    const nlohmann::json& value,
    const CatalogIdentity& identity,
    bool read_generated_name_metadata)
{
    std::vector<UserGroup> groups;
    if (value.type() != nlohmann::json::value_t::array) {
        return groups;
    }
    for (const nlohmann::json& item : value) {
        if (item.type() != nlohmann::json::value_t::object) {
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
        if (const nlohmann::json* references = ObjectMember(item, "marker_references")) {
            group.marker_references = ReadMarkerReferences(*references, identity);
        }
        groups.push_back(std::move(group));
    }
    return groups;
}

std::vector<GroupingView> ReadGroupingViews(
    const nlohmann::json& value,
    const CatalogIdentity& identity,
    bool read_generated_name_metadata)
{
    std::vector<GroupingView> views;
    if (value.type() != nlohmann::json::value_t::array) {
        return views;
    }
    for (const nlohmann::json& item : value) {
        if (item.type() != nlohmann::json::value_t::object) {
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
        if (const nlohmann::json* groups = ObjectMember(item, "groups")) {
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

std::unordered_set<std::string> ReadExpandedGroupIds(const nlohmann::json& value)
{
    std::unordered_set<std::string> expanded;
    if (value.type() != nlohmann::json::value_t::array) {
        return expanded;
    }
    for (const nlohmann::json& item : value) {
        if (item.type() == nlohmann::json::value_t::string) {
            expanded.insert(item.get_ref<const std::string&>());
        }
    }
    return expanded;
}

std::unordered_set<std::string> ReadStringSet(const nlohmann::json& value)
{
    std::unordered_set<std::string> result;
    if (value.type() != nlohmann::json::value_t::array) {
        return result;
    }
    for (const nlohmann::json& item : value) {
        if (item.type() == nlohmann::json::value_t::string) {
            result.insert(item.get_ref<const std::string&>());
        }
    }
    return result;
}

std::unordered_map<std::string, bool> ReadMarkerVisibility(const nlohmann::json& value)
{
    std::unordered_map<std::string, bool> visibility;
    if (value.type() != nlohmann::json::value_t::object) {
        return visibility;
    }
    for (const auto& [marker_id, item] : value.get_ref<const nlohmann::json::object_t&>()) {
        if (item.type() == nlohmann::json::value_t::boolean) {
            visibility.emplace(marker_id, item.get<bool>());
        }
    }
    return visibility;
}

std::unordered_map<std::string, PlotSeriesColor>
ReadMarkerColors(const nlohmann::json& value)
{
    std::unordered_map<std::string, PlotSeriesColor> colors;
    if (value.type() != nlohmann::json::value_t::object) {
        return colors;
    }
    for (const auto& [marker_id, encoded] : value.get_ref<const nlohmann::json::object_t&>()) {
        const std::optional<double> red =
            ParseFiniteDouble(encoded, "red");
        const std::optional<double> green =
            ParseFiniteDouble(encoded, "green");
        const std::optional<double> blue =
            ParseFiniteDouble(encoded, "blue");
        const std::optional<double> alpha =
            ParseFiniteDouble(encoded, "alpha");
        if (red && green && blue && alpha) {
            colors.emplace(
                marker_id,
                PlotSeriesColor::ExplicitColor({
                    .red = static_cast<float>(*red),
                    .green = static_cast<float>(*green),
                    .blue = static_cast<float>(*blue),
                    .alpha = static_cast<float>(*alpha),
                }));
        }
    }
    return colors;
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

std::filesystem::path DefaultCatalogUserStateCachePath(const RuntimePaths& runtime_paths)
{
    return DefaultLocalUserStatePath(
        local_user_state_paths::kSpectralLineUserState, runtime_paths);
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
        {1, 2, 3, 4, 5, kCacheSchemaVersion},
        "spectral-line user-state cache");
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

    const nlohmann::json* catalogs = ObjectMember(cache.document->root, "catalogs");
    if (catalogs == nullptr || catalogs->type() != nlohmann::json::value_t::object) {
        result.issue_kind =
            CatalogUserStateCacheLoadIssueKind::
                InvalidDocument;
        result.diagnostic_detail =
            catalogs == nullptr
                ? "missing object member \"catalogs\""
                : "member \"catalogs\" must be an object";
        result.warning =
            "Ignored invalid spectral-line user-state cache.";
        return result;
    }
    const nlohmann::json* panel_state =
        ObjectMember(
            cache.document->root,
            "catalog_panel_state");
    if (panel_state != nullptr &&
        panel_state->type() != nlohmann::json::value_t::object) {
        result.issue_kind =
            CatalogUserStateCacheLoadIssueKind::
                InvalidDocument;
        result.diagnostic_detail =
            "member \"catalog_panel_state\" must be an object";
        result.warning =
            "Ignored invalid spectral-line user-state cache.";
        return result;
    }
    if (std::optional<std::string> body_issue =
            ValidateCatalogCacheBodyShape(
                *catalogs,
                panel_state,
                cache.document->schema_version >= 3,
                cache.document->schema_version >= 5)) {
        result.issue_kind =
            CatalogUserStateCacheLoadIssueKind::
                InvalidDocument;
        result.diagnostic_detail =
            std::move(*body_issue);
        result.warning =
            "Ignored invalid spectral-line user-state cache.";
        return result;
    }
    result.requires_save =
        cache.document->schema_version <
        kCacheSchemaVersion;

    for (const auto& [identity_id, catalog_value] : catalogs->get_ref<const nlohmann::json::object_t&>()) {
        if (identity_id.empty() || catalog_value.type() != nlohmann::json::value_t::object) {
            continue;
        }

        CatalogIdentity identity;
        identity.id = identity_id;
        identity.display_name = DisplayNameForCatalogIdentity(identity_id);
        CatalogUserState state;
        state.catalog_identity = identity;
        if (const nlohmann::json* active_view_id =
                ObjectMember(catalog_value, "active_view_id");
            active_view_id != nullptr &&
            active_view_id->type() == nlohmann::json::value_t::string) {
            state.active_view_id = active_view_id->get_ref<const std::string&>();
        }
        if (const nlohmann::json* marker_visibility = ObjectMember(catalog_value, "marker_visibility")) {
            state.marker_visibility = ReadMarkerVisibility(*marker_visibility);
        }
        if (const nlohmann::json* marker_colors =
                ObjectMember(catalog_value, "marker_colors")) {
            state.marker_colors = ReadMarkerColors(*marker_colors);
        }
        if (const nlohmann::json* expanded = ObjectMember(catalog_value, "expanded_group_ids")) {
            result.cache.catalog_panel_state[identity_id].expanded_group_ids = ReadExpandedGroupIds(*expanded);
        }
        if (const nlohmann::json* grouping_views = ObjectMember(catalog_value, "grouping_views")) {
            state.grouping_views =
                ReadGroupingViews(
                    *grouping_views,
                    identity,
                    cache.document->schema_version >= 3);
        }
        result.cache.catalogs.emplace(identity_id, std::move(state));
    }
    if (panel_state != nullptr) {
        if (panel_state->type() == nlohmann::json::value_t::object) {
            for (const auto& [identity_id, panel_value] : panel_state->get_ref<const nlohmann::json::object_t&>()) {
                if (identity_id.empty() || panel_value.type() != nlohmann::json::value_t::object) {
                    continue;
                }
                if (const nlohmann::json* expanded = ObjectMember(panel_value, "expanded_group_ids")) {
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
        error = "spectral-line user-state cache path is empty";
        return false;
    }

    for (const auto& [catalog_id, state] : cache.catalogs) {
        for (const auto& [marker_id, selection] :
             state.marker_colors) {
            if (marker_id.empty() ||
                !selection.explicit_color() ||
                !IsValidRgbaColor(*selection.explicit_color())) {
                error =
                    "catalog marker color override is invalid for " +
                    catalog_id + "/" + marker_id;
                return false;
            }
        }
    }

    const std::vector<std::string> catalog_keys = SortedCacheKeys(cache.catalogs);
    const std::vector<std::string> panel_keys = SortedCacheKeys(cache.catalog_panel_state);
    return WriteVersionedJsonCacheFile(
        path,
        kCacheFormatKind,
        kCacheSchemaVersion,
        "spectral-line user-state cache",
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

                stream << "      \"marker_colors\": {";
                const std::vector<std::string> color_marker_ids =
                    SortedCacheKeys(state.marker_colors);
                if (!color_marker_ids.empty()) {
                    stream << "\n";
                }
                for (std::size_t index = 0;
                     index < color_marker_ids.size();
                     ++index) {
                    const std::string& marker_id =
                        color_marker_ids[index];
                    const RgbaColor& color =
                        *state.marker_colors.at(marker_id).explicit_color();
                    const std::optional<std::string> red =
                        EncodeFiniteFloat(color.red);
                    const std::optional<std::string> green =
                        EncodeFiniteFloat(color.green);
                    const std::optional<std::string> blue =
                        EncodeFiniteFloat(color.blue);
                    const std::optional<std::string> alpha =
                        EncodeFiniteFloat(color.alpha);
                    if (!red || !green || !blue || !alpha) {
                        return false;
                    }
                    stream << "        ";
                    WriteJsonString(stream, marker_id);
                    stream << ": { \"mode\": \"explicit-color\", \"red\": ";
                    WriteJsonString(stream, *red);
                    stream << ", \"green\": ";
                    WriteJsonString(stream, *green);
                    stream << ", \"blue\": ";
                    WriteJsonString(stream, *blue);
                    stream << ", \"alpha\": ";
                    WriteJsonString(stream, *alpha);
                    stream << " }";
                    stream <<
                        (index + 1 == color_marker_ids.size()
                             ? "\n"
                             : ",\n");
                }
                if (!color_marker_ids.empty()) {
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
