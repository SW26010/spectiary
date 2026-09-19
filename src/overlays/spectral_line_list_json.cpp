#include "overlays/spectral_line_list_json.h"
#include "app/local_user_state_json.h"
#include "platform/atomic_file.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace spectiary {
namespace {
using Json = nlohmann::json;

void Fields(const Json& value, std::initializer_list<std::string_view> required,
            std::initializer_list<std::string_view> optional = {})
{
    if (!value.is_object()) throw std::runtime_error("expected a JSON record");
    for (const auto key : required) {
        if (!value.contains(key)) throw std::runtime_error("missing required field: " + std::string(key));
    }
    for (const auto& [key, child] : value.items()) {
        if (std::find(required.begin(), required.end(), key) == required.end() &&
            std::find(optional.begin(), optional.end(), key) == optional.end())
            throw std::runtime_error("unknown field: " + key);
        if (child.is_null()) throw std::runtime_error("null is not allowed: " + key);
    }
}

const Json& Array(const Json& value)
{
    if (!value.is_array()) throw std::runtime_error("expected an array");
    return value;
}

std::optional<std::string> OptionalText(const Json& value, const char* key)
{
    if (!value.contains(key)) return std::nullopt;
    return value.at(key).get<std::string>();
}

double Number(const Json& value)
{
    if (!value.is_number()) throw std::runtime_error("expected a numeric coordinate");
    return value.get<double>();
}

Json Encode(const SpectralLineList& list)
{
    const char* unit = list.coordinate.unit == line_list::Unit::Angstrom ? "angstrom" :
        list.coordinate.unit == line_list::Unit::Nanometer ? "nm" : "um";
    Json root = {{"format_kind", kSpectralLineListFormat}, {"schema_version", kSpectralLineListSchemaVersion},
        {"id", list.id}, {"name", list.name},
        {"coordinate", {{"unit", unit}, {"medium", list.coordinate.medium == line_list::Medium::Air ? "air" : "vacuum"},
                        {"laboratory_rest", true}}},
        {"markers", Json::array()}, {"grouping_views", Json::array()}, {"color_schemes", Json::array()}};
    const auto text = [&](const char* key, const std::optional<std::string>& value) {
        if (value) root[key] = *value;
    };
    text("description", list.description); text("creator", list.creator);
    text("created_at", list.created_at); text("modified_at", list.modified_at);
    for (const auto& marker : list.markers) {
        Json value = {{"id", marker.id}, {"name", marker.name},
                      {"kind", marker.kind == line_list::MarkerKind::Line ? "line" : "band"}};
        if (marker.coordinate) value["coordinate"] = *marker.coordinate;
        if (marker.start) value["start"] = *marker.start;
        if (marker.end) value["end"] = *marker.end;
        if (marker.note) value["note"] = *marker.note;
        root["markers"].push_back(std::move(value));
    }
    for (const auto& view : list.grouping_views) root["grouping_views"].push_back(EncodeLineListGroupingView(view));
    for (const auto& scheme : list.color_schemes) root["color_schemes"].push_back(EncodeLineListColorScheme(scheme));
    return root;
}
}

line_list::GroupingView DecodeLineListGroupingView(const nlohmann::json& value)
{
    Fields(value, {"id", "name", "groups"});
    line_list::GroupingView view;
    view.id = value.at("id").get<std::string>();
    view.name = value.at("name").get<std::string>();
    for (const auto& entry : Array(value.at("groups"))) {
        Fields(entry, {"id", "name", "marker_ids"});
        line_list::Group group;
        group.id = entry.at("id").get<std::string>();
        group.name = entry.at("name").get<std::string>();
        for (const auto& id : Array(entry.at("marker_ids"))) group.marker_ids.push_back(id.get<std::string>());
        view.groups.push_back(std::move(group));
    }
    return view;
}

line_list::ColorScheme DecodeLineListColorScheme(const nlohmann::json& value)
{
    Fields(value, {"id", "name", "colors"});
    line_list::ColorScheme scheme;
    scheme.id = value.at("id").get<std::string>();
    scheme.name = value.at("name").get<std::string>();
    const auto& colors = value.at("colors");
    if (!colors.is_object()) throw std::runtime_error("colors must be a marker-id mapping");
    for (const auto& [id, color] : colors.items()) scheme.colors.emplace(id, color.get<std::string>());
    return scheme;
}

nlohmann::json EncodeLineListGroupingView(const line_list::GroupingView& value)
{
    Json result = {{"id", value.id}, {"name", value.name}, {"groups", Json::array()}};
    for (const auto& group : value.groups)
        result["groups"].push_back({{"id", group.id}, {"name", group.name}, {"marker_ids", group.marker_ids}});
    return result;
}

nlohmann::json EncodeLineListColorScheme(const line_list::ColorScheme& value)
{
    return {{"id", value.id}, {"name", value.name}, {"colors", value.colors}};
}

SpectralLineListParseResult ParseSpectralLineListJson(std::string_view bytes)
{
    SpectralLineListParseResult result;
    const auto parsed = ParseJson(bytes, result.error);
    if (!parsed) return result;
    try {
        const auto& root = *parsed;
        Fields(root, {"format_kind", "schema_version", "id", "name", "coordinate", "markers"},
               {"description", "creator", "created_at", "modified_at", "grouping_views", "color_schemes"});
        if (root.at("format_kind").get<std::string>() != kSpectralLineListFormat ||
            !root.at("schema_version").is_number_integer() || root.at("schema_version") != kSpectralLineListSchemaVersion)
            throw std::runtime_error("unsupported Spectral Line List format or schema version");
        SpectralLineList list;
        list.id = root.at("id").get<std::string>();
        list.name = root.at("name").get<std::string>();
        list.description = OptionalText(root, "description"); list.creator = OptionalText(root, "creator");
        list.created_at = OptionalText(root, "created_at"); list.modified_at = OptionalText(root, "modified_at");
        const auto& coordinate = root.at("coordinate");
        Fields(coordinate, {"unit", "medium", "laboratory_rest"});
        const auto unit = coordinate.at("unit").get<std::string>();
        if (unit == "angstrom") list.coordinate.unit = line_list::Unit::Angstrom;
        else if (unit == "nm") list.coordinate.unit = line_list::Unit::Nanometer;
        else if (unit == "um") list.coordinate.unit = line_list::Unit::Micrometer;
        else throw std::runtime_error("unsupported wavelength unit");
        const auto medium = coordinate.at("medium").get<std::string>();
        if (medium == "air") list.coordinate.medium = line_list::Medium::Air;
        else if (medium == "vacuum") list.coordinate.medium = line_list::Medium::Vacuum;
        else throw std::runtime_error("unsupported wavelength medium");
        list.coordinate.laboratory_rest = coordinate.at("laboratory_rest").get<bool>();
        for (const auto& value : Array(root.at("markers"))) {
            Fields(value, {"id", "name", "kind"}, {"coordinate", "start", "end", "note"});
            line_list::Marker marker;
            marker.id = value.at("id").get<std::string>(); marker.name = value.at("name").get<std::string>();
            const auto kind = value.at("kind").get<std::string>();
            if (kind == "line") marker.kind = line_list::MarkerKind::Line;
            else if (kind == "band") marker.kind = line_list::MarkerKind::Band;
            else throw std::runtime_error("unsupported marker kind");
            if (value.contains("coordinate")) marker.coordinate = Number(value.at("coordinate"));
            if (value.contains("start")) marker.start = Number(value.at("start"));
            if (value.contains("end")) marker.end = Number(value.at("end"));
            marker.note = OptionalText(value, "note");
            list.markers.push_back(std::move(marker));
        }
        if (root.contains("grouping_views"))
            for (const auto& view : Array(root.at("grouping_views"))) list.grouping_views.push_back(DecodeLineListGroupingView(view));
        if (root.contains("color_schemes"))
            for (const auto& scheme : Array(root.at("color_schemes"))) list.color_schemes.push_back(DecodeLineListColorScheme(scheme));
        if (ValidateSpectralLineList(list, result.error)) result.list = std::move(list);
    } catch (const std::exception& error) { result.error = error.what(); }
    return result;
}

SpectralLineListParseResult LoadSpectralLineListFromPath(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    std::string bytes;
    if (!stream || !ReadTextStreamCancelable(stream, bytes)) return {std::nullopt, "could not read bounded Spectral Line List JSON"};
    return ParseSpectralLineListJson(bytes);
}

bool WriteSpectralLineListJson(const SpectralLineList& list, std::ostream& stream, std::string& error)
{
    if (!ValidateSpectralLineList(list, error)) return false;
    try {
        const auto bytes = Encode(list).dump(2) + '\n';
        // Check the same parser resource/UTF-8 limits before exposing output.
        const auto checked = ParseSpectralLineListJson(bytes);
        if (!checked.list) { error = checked.error; return false; }
        stream << bytes;
        if (!stream) { error = "could not write Spectral Line List JSON"; return false; }
        return true;
    } catch (const std::exception& failure) { error = failure.what(); return false; }
}

bool SaveSpectralLineListToPathAtomic(const std::filesystem::path& path, const SpectralLineList& list, std::string& error)
{
    AtomicFileWriteOptions options;
    options.target_description = "Spectral Line List";
    return WriteFileAtomically(path, options, [&](std::ostream& stream, std::string& diagnostic) {
        return WriteSpectralLineListJson(list, stream, diagnostic);
    }, &error);
}
} // namespace spectiary
