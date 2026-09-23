#include "overlays/built_in_spectral_line_state_io.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <stdexcept>
#include <unordered_set>

namespace spectiary {
namespace {
using Json = nlohmann::json;
constexpr const char* kFormat = "spectiary.catalog_user_state.cache";
constexpr int kSchema = 8;

void Fields(const Json& value, std::initializer_list<std::string_view> required,
            std::initializer_list<std::string_view> optional = {})
{
    if (!value.is_object()) throw std::runtime_error("expected state record");
    for (auto key : required) if (!value.contains(key)) throw std::runtime_error("missing state field: " + std::string(key));
    for (const auto& [key, child] : value.items()) {
        if (child.is_null() || (std::find(required.begin(), required.end(), key) == required.end() &&
            std::find(optional.begin(), optional.end(), key) == optional.end()))
            throw std::runtime_error("unknown or invalid state field: " + key);
    }
}
const Json& Array(const Json& value)
{
    if (!value.is_array()) throw std::runtime_error("expected state array");
    return value;
}
const Json& Object(const Json& value)
{
    if (!value.is_object()) throw std::runtime_error("expected state mapping");
    return value;
}
std::size_t Count(const Json& value)
{
    if (!JsonIsInt64(value) || value.get<std::int64_t>() < 0) throw std::runtime_error("invalid generated-name count");
    return value.get<std::size_t>();
}
GeneratedNameMetadata ReadName(const Json& value)
{
    GeneratedNameMetadata result;
    if (value.contains("name_source")) {
        auto source = value.at("name_source").get<std::string>();
        if (source == "catalog_grouping_view") result.source = GeneratedNameSource::CatalogGroupingView;
        else if (source == "default_grouping_view") result.source = GeneratedNameSource::DefaultGroupingView;
        else if (source == "default_group") result.source = GeneratedNameSource::DefaultGroup;
        else throw std::runtime_error("unsupported generated-name source");
    }
    if (value.contains("name_ordinal")) result.ordinal = Count(value.at("name_ordinal"));
    if (value.contains("generated_copy_count")) result.copy_count = Count(value.at("generated_copy_count"));
    if (result.copy_count > kMaximumGeneratedNameCopyCount) throw std::runtime_error("generated-name copy count exceeds limit");
    if (value.contains("generated_copy_base_name")) result.copy_base_name = value.at("generated_copy_base_name").get<std::string>();
    return result;
}
Json WriteName(const GeneratedNameMetadata& value)
{
    Json result = Json::object();
    switch (value.source) {
    case GeneratedNameSource::CatalogGroupingView: result["name_source"] = "catalog_grouping_view"; break;
    case GeneratedNameSource::DefaultGroupingView: result["name_source"] = "default_grouping_view"; break;
    case GeneratedNameSource::DefaultGroup: result["name_source"] = "default_group"; break;
    case GeneratedNameSource::None: break;
    }
    if (value.ordinal) result["name_ordinal"] = value.ordinal;
    if (value.copy_count) result["generated_copy_count"] = value.copy_count;
    if (!value.copy_base_name.empty()) result["generated_copy_base_name"] = value.copy_base_name;
    return result;
}
void Visibility(const Json& value, SpectralLineSessionState& session)
{
    for (const auto& [id, visible] : Object(value).items()) {
        if (id.empty() || !visible.is_boolean()) throw std::runtime_error("invalid marker visibility");
        session.marker_visibility[id] = visible.get<bool>();
    }
}
// Delimiter encoding is accepted only at the schema 6/7 migration boundary.
Json MigrateExpandedGroups(const Json& value)
{
    Json expanded = Json::array();
    for (const auto& entry : Array(value)) {
        const auto id = entry.get<std::string>();
        const auto separator = id.find('/');
        if (separator == std::string::npos || separator == 0 || separator + 1 == id.size() ||
            id.find('/', separator + 1) != std::string::npos)
            throw std::runtime_error("invalid or ambiguous legacy expansion key");
        expanded.push_back({{"view_id", id.substr(0, separator)}, {"group_id", id.substr(separator + 1)}});
    }
    return expanded;
}
void Expanded(const Json& value, SpectralLineSessionState& session)
{
    for (const auto& entry : Array(value)) {
        Fields(entry, {"view_id", "group_id"});
        SpectralLineGroupExpansionKey key{entry.at("view_id").get<std::string>(), entry.at("group_id").get<std::string>()};
        if (key.view_id.empty() || key.group_id.empty() || !session.expanded_groups.insert(key).second)
            throw std::runtime_error("invalid or repeated expansion key");
    }
}
SpectralLineSessionState ReadSession(const Json& value)
{
    Fields(value, {"active_view_id", "active_color_scheme_id", "marker_visibility", "expanded_groups", "view_names", "group_names"});
    SpectralLineSessionState session;
    session.active_view_id = value.at("active_view_id").get<std::string>();
    session.active_color_scheme_id = value.at("active_color_scheme_id").get<std::string>();
    Visibility(value.at("marker_visibility"), session);
    Expanded(value.at("expanded_groups"), session);
    for (const char* key : {"view_names", "group_names"}) {
        auto& names = std::string_view(key) == "view_names" ? session.view_names : session.group_names;
        for (const auto& [id, metadata] : Object(value.at(key)).items()) {
            if (id.empty()) throw std::runtime_error("empty generated-name identity");
            Fields(metadata, {}, {"name_source", "name_ordinal", "generated_copy_count", "generated_copy_base_name"});
            names[id] = ReadName(metadata);
        }
    }
    return session;
}
Json WriteSession(const SpectralLineSessionState& value)
{
    Json expanded = Json::array();
    for (const auto& key : value.expanded_groups)
        expanded.push_back({{"view_id", key.view_id}, {"group_id", key.group_id}});
    Json result = {{"active_view_id", value.active_view_id}, {"active_color_scheme_id", value.active_color_scheme_id},
        {"marker_visibility", value.marker_visibility}, {"expanded_groups", expanded},
        {"view_names", Json::object()}, {"group_names", Json::object()}};
    for (const auto& [id, name] : value.view_names) result["view_names"][id] = WriteName(name);
    for (const auto& [id, name] : value.group_names) result["group_names"][id] = WriteName(name);
    return result;
}

SpectralLineSessionState MigrateSevenSession(Json value)
{
    Fields(value, {"active_view_id", "active_color_scheme_id", "marker_visibility", "expanded_group_ids", "view_names", "group_names"});
    value["expanded_groups"] = MigrateExpandedGroups(value.at("expanded_group_ids"));
    value.erase("expanded_group_ids");
    return ReadSession(value);
}

float LegacyChannel(const Json& value)
{
    const auto text = value.get<std::string>();
    float number = 0;
    auto result = std::from_chars(text.data(), text.data() + text.size(), number);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || !std::isfinite(number) || number < 0 || number > 1)
        throw std::runtime_error("invalid legacy color channel");
    return number;
}
BuiltInSpectralLineState MigrateSix(const Json& root, const SpectralLineList& base)
{
    Fields(root, {"format_kind", "schema_version", "catalogs"}, {"catalog_panel_state"});
    const auto& catalogs = Object(root.at("catalogs"));
    for (const auto& [id, value] : catalogs.items()) {
        (void)value;
        if (id != base.id) throw std::runtime_error("legacy state belongs to an unavailable line list: " + id);
    }
    BuiltInSpectralLineState state;
    if (catalogs.contains(base.id)) {
        const auto& value = catalogs.at(base.id);
        Fields(value, {"active_view_id", "marker_visibility", "grouping_views", "marker_colors"});
        state.session.active_view_id = value.at("active_view_id").get<std::string>();
        Visibility(value.at("marker_visibility"), state.session);
        std::unordered_map<std::string, double> positions;
        for (const auto& marker : base.markers) positions[marker.id] = SpectralLineMarkerPosition(marker);
        for (const auto& old_view : Array(value.at("grouping_views"))) {
            Fields(old_view, {"id", "name", "groups"}, {"name_source", "name_ordinal", "generated_copy_count", "generated_copy_base_name"});
            line_list::GroupingView view;
            view.id = old_view.at("id").get<std::string>(); view.name = old_view.at("name").get<std::string>();
            state.session.view_names[view.id] = ReadName(old_view);
            bool unassigned_seen = false;
            for (const auto& old_group : Array(old_view.at("groups"))) {
                Fields(old_group, {"id", "name", "is_unassigned", "marker_references"},
                       {"name_source", "name_ordinal", "generated_copy_count", "generated_copy_base_name"});
                line_list::Group group;
                group.id = old_group.at("id").get<std::string>(); group.name = old_group.at("name").get<std::string>();
                const bool unassigned = old_group.at("is_unassigned").get<bool>();
                if (unassigned != (group.id == "__unassigned__") || (unassigned && unassigned_seen))
                    throw std::runtime_error("invalid legacy Unassigned identity/flag");
                unassigned_seen |= unassigned;
                const auto metadata = ReadName(old_group);
                std::unordered_set<std::string> references;
                for (const auto& reference : Array(old_group.at("marker_references"))) {
                    Fields(reference, {"catalog_identity", "marker_id"});
                    if (reference.at("catalog_identity").get<std::string>() != base.id)
                        throw std::runtime_error("legacy reference belongs to another line list");
                    auto id = reference.at("marker_id").get<std::string>();
                    if (!positions.contains(id) || !references.insert(id).second)
                        throw std::runtime_error("unresolved or repeated legacy marker reference: " + id);
                    group.marker_ids.push_back(std::move(id));
                }
                if (unassigned) continue;
                state.session.group_names[group.id] = metadata;
                view.groups.push_back(std::move(group));
            }
            state.overlay.grouping_views.push_back(std::move(view));
        }
        if (value.contains("marker_colors")) {
            line_list::ColorScheme scheme{"builtin.user-colors", "Custom colors", {}};
            for (const auto& [id, color] : Object(value.at("marker_colors")).items()) {
                Fields(color, {"mode", "red", "green", "blue", "alpha"});
                if (color.at("mode") != "explicit-color") throw std::runtime_error("invalid legacy color mode");
                scheme.colors[id] = EncodeLineListColor({LegacyChannel(color.at("red")), LegacyChannel(color.at("green")),
                    LegacyChannel(color.at("blue")), LegacyChannel(color.at("alpha"))});
            }
            if (!scheme.colors.empty()) {
                state.session.active_color_scheme_id = scheme.id;
                state.overlay.color_schemes = std::vector<line_list::ColorScheme>{std::move(scheme)};
            }
        }
    }
    if (root.contains("catalog_panel_state")) {
        for (const auto& [id, value] : Object(root.at("catalog_panel_state")).items()) {
            if (id != base.id) throw std::runtime_error("legacy panel state belongs to another line list");
            Fields(value, {"expanded_group_ids"});
            Expanded(MigrateExpandedGroups(value.at("expanded_group_ids")), state.session);
        }
    }
    return state;
}
}

BuiltInSpectralLineStateLoadResult LoadBuiltInSpectralLineState(const std::filesystem::path& path, const SpectralLineList& base)
{
    BuiltInSpectralLineStateLoadResult result;
    auto loaded = LoadVersionedJsonCacheFile(path, kFormat, {6, 7, kSchema}, "built-in spectral-line state");
    if (!loaded.document) {
        result.issue_kind = loaded.issue_kind;
        result.error = loaded.diagnostic_detail.empty() ? loaded.warning : loaded.diagnostic_detail;
        return result;
    }
    try {
        BuiltInSpectralLineState state;
        const auto& root = loaded.document->root;
        if (loaded.document->schema_version == 6) state = MigrateSix(root, base);
        else {
            Fields(root, {"format_kind", "schema_version", "catalogs"});
            const auto& records = Object(root.at("catalogs"));
            if (records.size() != 1 || !records.contains(base.id)) throw std::runtime_error("overlay is scoped to the wrong base identity");
            const auto& record = records.at(base.id);
            Fields(record, {"overlay", "session"});
            state.overlay = DecodeBuiltInSpectralLineOverlay(record.at("overlay"));
            state.session = loaded.document->schema_version == 7
                ? MigrateSevenSession(record.at("session")) : ReadSession(record.at("session"));
        }
        const auto effective = ComposeBuiltInSpectralLineList(base, state.overlay);
        if (!effective.list) throw std::runtime_error(effective.error);
        NormalizeSpectralLineSession(state.session, *effective.list);
        result.state = std::move(state);
        result.requires_save = loaded.document->schema_version != kSchema;
    } catch (const std::exception& error) {
        result.issue_kind = VersionedJsonCacheLoadIssueKind::InvalidDocument;
        result.error = error.what();
    }
    return result;
}

bool SaveBuiltInSpectralLineState(const std::filesystem::path& path, const SpectralLineList& base,
    const BuiltInSpectralLineState& state, std::string& error, AtomicFileWriteCheckpoint before_replace)
{
    auto effective = ComposeBuiltInSpectralLineList(base, state.overlay);
    if (!effective.list) { error = effective.error; return false; }
    try {
        const auto session = WriteSession(state.session);
        (void)ReadSession(session);
        Json records = {{base.id, {{"overlay", EncodeBuiltInSpectralLineOverlay(state.overlay)}, {"session", session}}}};
        return WriteVersionedJsonCacheFile(path, kFormat, kSchema, "built-in spectral-line state",
            [&](std::ostream& stream, std::string&) { stream << ",\n  \"catalogs\": " << records.dump(2); return true; },
            &error, std::move(before_replace));
    } catch (const std::exception& failure) { error = failure.what(); return false; }
}
} // namespace spectiary
