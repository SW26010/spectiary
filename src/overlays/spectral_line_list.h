#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace spectiary {
namespace line_list {

enum class Unit { Angstrom, Nanometer, Micrometer };
enum class Medium { Air, Vacuum };
enum class MarkerKind { Line, Band };

struct WavelengthCoordinate {
    Unit unit = Unit::Angstrom;
    Medium medium = Medium::Vacuum;
    bool laboratory_rest = true;
    bool operator==(const WavelengthCoordinate&) const = default;
};

struct Marker {
    std::string id;
    std::string name;
    MarkerKind kind = MarkerKind::Line;
    std::optional<double> coordinate;
    std::optional<double> start;
    std::optional<double> end;
    std::optional<std::string> note;
    bool operator==(const Marker&) const = default;
};

struct Group {
    std::string id;
    std::string name;
    // Preserve representation order losslessly. v1 assigns no presentation,
    // priority or scientific meaning to this order.
    std::vector<std::string> marker_ids;
    bool operator==(const Group&) const = default;
};

struct GroupingView {
    std::string id;
    std::string name;
    std::vector<Group> groups;
    bool operator==(const GroupingView&) const = default;
};

struct ColorScheme {
    std::string id;
    std::string name;
    // Canonical RGBA8 (#RRGGBBAA); absence is Auto.
    std::map<std::string, std::string> colors;
    bool operator==(const ColorScheme&) const = default;
};
} // namespace line_list

struct SpectralLineList {
    std::string id;
    std::string name;
    line_list::WavelengthCoordinate coordinate;
    std::optional<std::string> description;
    std::optional<std::string> creator;
    std::optional<std::string> created_at;
    std::optional<std::string> modified_at;
    std::vector<line_list::Marker> markers;
    std::vector<line_list::GroupingView> grouping_views;
    std::vector<line_list::ColorScheme> color_schemes;
    bool operator==(const SpectralLineList&) const = default;
};

[[nodiscard]] bool ValidateSpectralLineList(const SpectralLineList& list, std::string& error);
[[nodiscard]] double SpectralLineMarkerPosition(const line_list::Marker& marker);

} // namespace spectiary
