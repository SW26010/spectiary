#include "overlays/spectral_line_catalog.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

namespace specforge {
namespace {

constexpr const char* kDefaultCatalogPath = "config/spectral_lines.public.tsv";

std::string Trim(std::string_view value)
{
    const auto is_space = [](char character) {
        return character == ' ' || character == '\t' || character == '\r' || character == '\n';
    };

    while (!value.empty() && is_space(value.front())) {
        value.remove_prefix(1);
    }
    while (!value.empty() && is_space(value.back())) {
        value.remove_suffix(1);
    }
    return std::string(value);
}

std::vector<std::string> SplitTabLine(std::string_view line)
{
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (start <= line.size()) {
        const std::size_t end = line.find('\t', start);
        if (end == std::string_view::npos) {
            fields.push_back(Trim(line.substr(start)));
            break;
        }
        fields.push_back(Trim(line.substr(start, end - start)));
        start = end + 1;
    }
    return fields;
}

std::optional<double> ParseOptionalDouble(const std::string& value, std::string& error)
{
    if (value.empty()) {
        return std::nullopt;
    }

    double parsed = 0.0;
    const char* begin = value.data();
    const char* end = value.data() + value.size();
    const std::from_chars_result result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end) {
        error = "invalid numeric value: " + value;
        return std::nullopt;
    }
    return parsed;
}

std::string RequiredField(
    const std::vector<std::string>& fields,
    const std::unordered_map<std::string, std::size_t>& columns,
    std::string_view name,
    std::string& error)
{
    const auto match = columns.find(std::string(name));
    if (match == columns.end() || match->second >= fields.size()) {
        error = "missing required column: " + std::string(name);
        return {};
    }
    return fields[match->second];
}

std::string OptionalField(
    const std::vector<std::string>& fields,
    const std::unordered_map<std::string, std::size_t>& columns,
    std::string_view name)
{
    const auto match = columns.find(std::string(name));
    if (match == columns.end() || match->second >= fields.size()) {
        return {};
    }
    return fields[match->second];
}

bool IsPositiveFinite(double value)
{
    return std::isfinite(value) && value > 0.0;
}

bool ValidateMarker(const SpectralLineMarker& marker, std::string& error)
{
    if (marker.id.empty()) {
        error = "marker id is empty";
        return false;
    }
    if (marker.label.empty()) {
        error = "marker label is empty for " + marker.id;
        return false;
    }
    if (marker.group.empty()) {
        error = "marker group is empty for " + marker.id;
        return false;
    }
    if (marker.display_label.empty()) {
        error = "marker display_label is empty for " + marker.id;
        return false;
    }
    if (marker.source_ref.empty()) {
        error = "marker source_ref is empty for " + marker.id;
        return false;
    }

    if (marker.kind == SpectralLineMarkerKind::Line) {
        if (!marker.vacuum_angstrom) {
            error = "line marker missing vacuum_angstrom: " + marker.id;
            return false;
        }
        if (!IsPositiveFinite(*marker.vacuum_angstrom)) {
            error = "line marker vacuum_angstrom must be finite and positive: " + marker.id;
            return false;
        }
        return true;
    }

    if (!marker.start_vacuum_angstrom || !marker.end_vacuum_angstrom) {
        error = "band marker missing start/end vacuum Angstrom: " + marker.id;
        return false;
    }
    if (!IsPositiveFinite(*marker.start_vacuum_angstrom) || !IsPositiveFinite(*marker.end_vacuum_angstrom)) {
        error = "band marker start/end vacuum Angstrom must be finite and positive: " + marker.id;
        return false;
    }
    if (*marker.start_vacuum_angstrom >= *marker.end_vacuum_angstrom) {
        error = "band marker start must be less than end: " + marker.id;
        return false;
    }
    return true;
}

std::optional<std::filesystem::path> ExecutableDirectory()
{
#ifdef _WIN32
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return std::nullopt;
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            return std::filesystem::path(buffer).parent_path();
        }
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            return std::nullopt;
        }
        buffer.resize(buffer.size() * 2);
    }
#else
    return std::nullopt;
#endif
}

std::vector<std::filesystem::path> DefaultCatalogCandidates()
{
    std::vector<std::filesystem::path> candidates;
    std::error_code error;
    const std::filesystem::path current = std::filesystem::current_path(error);
    const std::filesystem::path relative(kDefaultCatalogPath);
    if (const std::optional<std::filesystem::path> executable_dir = ExecutableDirectory()) {
        candidates.push_back(*executable_dir / relative);
    }
    if (!error) {
        std::filesystem::path base = current;
        for (int index = 0; index < 5; ++index) {
            candidates.push_back(base / relative);
            base /= "..";
        }
    }
    candidates.push_back(relative);
    return candidates;
}

}  // namespace

double SpectralLineMarkerPosition(const SpectralLineMarker& marker)
{
    if (marker.kind == SpectralLineMarkerKind::Line && marker.vacuum_angstrom) {
        return *marker.vacuum_angstrom;
    }
    if (marker.start_vacuum_angstrom && marker.end_vacuum_angstrom) {
        return (*marker.start_vacuum_angstrom + *marker.end_vacuum_angstrom) * 0.5;
    }
    return 0.0;
}

const char* SpectralLineMarkerKindLabel(SpectralLineMarkerKind kind)
{
    switch (kind) {
    case SpectralLineMarkerKind::Band:
        return "band";
    case SpectralLineMarkerKind::Line:
    default:
        return "line";
    }
}

SpectralLineCatalog LoadSpectralLineCatalogFromPath(const std::filesystem::path& path)
{
    SpectralLineCatalog catalog;
    catalog.path = path;

    std::ifstream stream(path);
    if (!stream.good()) {
        catalog.load_error = "could not open spectral line catalog: " + path.string();
        return catalog;
    }

    std::unordered_map<std::string, std::size_t> columns;
    std::unordered_set<std::string> marker_ids;
    bool found_header = false;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(stream, line)) {
        ++line_number;
        const std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed.front() == '#') {
            continue;
        }

        const std::vector<std::string> fields = SplitTabLine(line);
        if (!found_header) {
            for (std::size_t index = 0; index < fields.size(); ++index) {
                columns.emplace(fields[index], index);
            }
            found_header = true;
            continue;
        }

        std::string error;
        SpectralLineMarker marker;
        marker.id = RequiredField(fields, columns, "id", error);
        marker.label = RequiredField(fields, columns, "label", error);
        const std::string kind = RequiredField(fields, columns, "kind", error);
        marker.group = RequiredField(fields, columns, "group", error);
        marker.vacuum_angstrom =
            ParseOptionalDouble(RequiredField(fields, columns, "vacuum_angstrom", error), error);
        marker.start_vacuum_angstrom =
            ParseOptionalDouble(RequiredField(fields, columns, "start_vacuum_angstrom", error), error);
        marker.end_vacuum_angstrom =
            ParseOptionalDouble(RequiredField(fields, columns, "end_vacuum_angstrom", error), error);
        marker.display_label = RequiredField(fields, columns, "display_label", error);
        marker.source_ref = RequiredField(fields, columns, "source_ref", error);
        marker.notes = OptionalField(fields, columns, "notes");

        if (!error.empty()) {
            catalog.load_error = "line " + std::to_string(line_number) + ": " + error;
            catalog.markers.clear();
            return catalog;
        }

        if (kind == "line") {
            marker.kind = SpectralLineMarkerKind::Line;
        } else if (kind == "band") {
            marker.kind = SpectralLineMarkerKind::Band;
        } else {
            catalog.load_error = "line " + std::to_string(line_number) + ": unknown marker kind: " + kind;
            catalog.markers.clear();
            return catalog;
        }

        if (!ValidateMarker(marker, error)) {
            catalog.load_error = "line " + std::to_string(line_number) + ": " + error;
            catalog.markers.clear();
            return catalog;
        }
        if (!marker_ids.insert(marker.id).second) {
            catalog.load_error = "line " + std::to_string(line_number) + ": duplicate marker id: " + marker.id;
            catalog.markers.clear();
            return catalog;
        }
        catalog.markers.push_back(std::move(marker));
    }

    if (!found_header) {
        catalog.load_error = "spectral line catalog is missing a header row: " + path.string();
        return catalog;
    }
    if (catalog.markers.empty()) {
        catalog.load_error = "spectral line catalog has no markers: " + path.string();
        return catalog;
    }

    std::sort(catalog.markers.begin(), catalog.markers.end(), [](const auto& left, const auto& right) {
        return SpectralLineMarkerPosition(left) < SpectralLineMarkerPosition(right);
    });
    return catalog;
}

SpectralLineCatalog LoadDefaultSpectralLineCatalog()
{
    for (const std::filesystem::path& candidate : DefaultCatalogCandidates()) {
        std::error_code error;
        if (std::filesystem::exists(candidate, error)) {
            return LoadSpectralLineCatalogFromPath(candidate);
        }
    }

    SpectralLineCatalog catalog;
    catalog.load_error = "could not find default spectral line catalog: config/spectral_lines.public.tsv";
    return catalog;
}

}  // namespace specforge
