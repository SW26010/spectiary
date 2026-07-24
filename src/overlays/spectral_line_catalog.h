#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace specforge {

enum class SpectralLineMarkerKind {
    Line,
    Band,
};

struct SpectralLineMarker {
    std::string id;
    std::string label;
    SpectralLineMarkerKind kind = SpectralLineMarkerKind::Line;
    std::string group;
    std::optional<double> vacuum_angstrom;
    std::optional<double> start_vacuum_angstrom;
    std::optional<double> end_vacuum_angstrom;
    std::string display_label;
    std::string source_ref;
    std::string notes;
};

struct SpectralLineCatalog {
    std::filesystem::path path;
    std::vector<SpectralLineMarker> markers;
    std::string load_error;
};

[[nodiscard]] SpectralLineCatalog LoadPackagedPublicSpectralLineCatalog(
    const std::filesystem::path& path);
[[nodiscard]] SpectralLineCatalog LoadSpectralLineCatalogFromPath(const std::filesystem::path& path);
[[nodiscard]] SpectralLineCatalog LoadPublicSpectralLineCatalogFromPath(const std::filesystem::path& path);
[[nodiscard]] double SpectralLineMarkerPosition(const SpectralLineMarker& marker);
[[nodiscard]] const char* SpectralLineMarkerKindLabel(SpectralLineMarkerKind kind);

}  // namespace specforge
