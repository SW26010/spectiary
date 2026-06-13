#include "overlays/spectral_line_catalog.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace {

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

const specforge::SpectralLineMarker& FindMarker(
    const specforge::SpectralLineCatalog& catalog,
    std::string_view id)
{
    const auto match = std::find_if(catalog.markers.begin(), catalog.markers.end(), [id](const auto& marker) {
        return marker.id == id;
    });
    Require(match != catalog.markers.end(), "expected marker is missing");
    return *match;
}

bool NearlyEqual(double left, double right, double tolerance)
{
    return std::abs(left - right) <= tolerance;
}

bool Contains(std::string_view text, std::string_view pattern)
{
    return text.find(pattern) != std::string_view::npos;
}

class ScopedCurrentPath {
public:
    explicit ScopedCurrentPath(const std::filesystem::path& next_path)
        : original_path_(std::filesystem::current_path())
    {
        std::filesystem::current_path(next_path);
    }

    ~ScopedCurrentPath()
    {
        std::error_code error;
        std::filesystem::current_path(original_path_, error);
    }

    ScopedCurrentPath(const ScopedCurrentPath&) = delete;
    ScopedCurrentPath& operator=(const ScopedCurrentPath&) = delete;

private:
    std::filesystem::path original_path_;
};

void TestLoadsPublicCatalog()
{
    const std::filesystem::path path =
        std::filesystem::path(SPECFORGE_SOURCE_DIR) / "config" / "spectral_lines.public.tsv";
    const specforge::SpectralLineCatalog catalog = specforge::LoadPublicSpectralLineCatalogFromPath(path);
    Require(catalog.load_error.empty(), catalog.load_error);
    Require(catalog.markers.size() >= 30, "public catalog should contain the default reference markers");

    double previous_position = 0.0;
    bool first = true;
    for (const specforge::SpectralLineMarker& marker : catalog.markers) {
        const double position = specforge::SpectralLineMarkerPosition(marker);
        Require(first || position >= previous_position, "catalog markers should load in wavelength order");
        first = false;
        previous_position = position;
    }
}

void TestUsesVacuumWavelengthsForAtomicMarkers()
{
    const std::filesystem::path path =
        std::filesystem::path(SPECFORGE_SOURCE_DIR) / "config" / "spectral_lines.public.tsv";
    const specforge::SpectralLineCatalog catalog = specforge::LoadPublicSpectralLineCatalogFromPath(path);
    Require(catalog.load_error.empty(), catalog.load_error);

    Require(NearlyEqual(*FindMarker(catalog, "h_alpha").vacuum_angstrom, 6564.614, 1.0e-6), "H alpha should use vacuum wavelength");
    Require(NearlyEqual(*FindMarker(catalog, "h_beta").vacuum_angstrom, 4862.683, 1.0e-6), "H beta should use vacuum wavelength");
    Require(NearlyEqual(*FindMarker(catalog, "na_i_d2").vacuum_angstrom, 5891.583, 1.0e-6), "Na I D2 should use vacuum wavelength");
    Require(NearlyEqual(*FindMarker(catalog, "na_i_d1").vacuum_angstrom, 5897.558, 1.0e-6), "Na I D1 should use vacuum wavelength");
    Require(NearlyEqual(*FindMarker(catalog, "ca_ii_8500").vacuum_angstrom, 8500.360, 1.0e-6), "Ca II triplet should use vacuum wavelength");

    Require(
        !NearlyEqual(*FindMarker(catalog, "h_alpha").vacuum_angstrom, 6562.801, 1.0e-3),
        "H alpha must not regress to air wavelength");
}

void TestPublicCatalogDoesNotContainPrivateOverlayConcepts()
{
    const std::filesystem::path path =
        std::filesystem::path(SPECFORGE_SOURCE_DIR) / "config" / "spectral_lines.public.tsv";
    const specforge::SpectralLineCatalog catalog = specforge::LoadPublicSpectralLineCatalogFromPath(path);
    Require(catalog.load_error.empty(), catalog.load_error);

    for (const specforge::SpectralLineMarker& marker : catalog.markers) {
        Require(!Contains(marker.id, "window"), "public catalog must not contain zoom windows");
        Require(!Contains(marker.group, "subtype"), "public catalog must not contain subtype groups");
        Require(marker.group != "C-H", "public catalog must not contain C-H subtype preset groups");
        Require(marker.group != "C-R", "public catalog must not contain C-R subtype preset groups");
        Require(marker.group != "C-N", "public catalog must not contain C-N subtype preset groups");
        Require(marker.group != "C-J", "public catalog must not contain C-J subtype preset groups");
        Require(marker.group != "C-Hd", "public catalog must not contain C-Hd subtype preset groups");
        Require(marker.group != "Ba", "public catalog must not contain Ba subtype preset groups");
        Require(marker.group != "Binary", "public catalog must not contain Binary subtype preset groups");
        Require(marker.group != "GDQ", "public catalog must not contain GDQ subtype preset groups");
    }
}

void TestLoadsCatalogWithoutOptionalNotesColumn()
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "specforge_spectral_line_catalog_without_notes.tsv";
    {
        std::ofstream stream(path);
        stream << "id\tlabel\tkind\tgroup\tvacuum_angstrom\tstart_vacuum_angstrom\tend_vacuum_angstrom\t"
                  "display_label\tsource_ref\n";
        stream << "h_alpha\tH alpha\tline\tBalmer\t6564.614\t\t\tHa\tNIST vacuum\n";
    }

    const specforge::SpectralLineCatalog catalog = specforge::LoadPublicSpectralLineCatalogFromPath(path);
    std::error_code remove_error;
    std::filesystem::remove(path, remove_error);

    Require(catalog.load_error.empty(), catalog.load_error);
    Require(catalog.markers.size() == 1, "catalog without notes should load one marker");
    Require(catalog.markers.front().notes.empty(), "missing optional notes column should default to empty");
}

void TestGenericCatalogMayOmitGrouping()
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "specforge_spectral_line_catalog_without_group.tsv";
    {
        std::ofstream stream(path);
        stream << "id\tlabel\tkind\tvacuum_angstrom\tstart_vacuum_angstrom\tend_vacuum_angstrom\t"
                  "display_label\tsource_ref\n";
        stream << "marker_a\tMarker A\tline\t4100.0\t\t\tA\ttest\n";
        stream << "marker_b\tMarker B\tline\t4200.0\t\t\tB\ttest\n";
    }

    const specforge::SpectralLineCatalog catalog = specforge::LoadSpectralLineCatalogFromPath(path);
    std::error_code remove_error;
    std::filesystem::remove(path, remove_error);

    Require(catalog.load_error.empty(), catalog.load_error);
    Require(catalog.markers.size() == 2, "generic ungrouped catalog should load markers");
    Require(catalog.markers.front().group.empty(), "missing generic group column should default to empty group");
}

void TestPublicCatalogRequiresGrouping()
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "specforge_public_spectral_line_catalog_empty_group.tsv";
    {
        std::ofstream stream(path);
        stream << "id\tlabel\tkind\tgroup\tvacuum_angstrom\tstart_vacuum_angstrom\tend_vacuum_angstrom\t"
                  "display_label\tsource_ref\n";
        stream << "h_alpha\tH alpha\tline\t\t6564.614\t\t\tHa\tNIST vacuum\n";
    }

    const specforge::SpectralLineCatalog catalog = specforge::LoadPublicSpectralLineCatalogFromPath(path);
    std::error_code remove_error;
    std::filesystem::remove(path, remove_error);

    Require(!catalog.load_error.empty(), "public catalog should reject empty groups");
    Require(Contains(catalog.load_error, "marker group is empty"), "public group error should be explicit");
    Require(catalog.markers.empty(), "public group failure should clear partial catalog results");
}

#ifdef _WIN32
void TestDefaultCatalogLoadsFromExecutableDirectoryWhenCwdDiffers()
{
    const ScopedCurrentPath scoped_current_path(std::filesystem::temp_directory_path());

    const specforge::SpectralLineCatalog catalog = specforge::LoadDefaultSpectralLineCatalog();

    Require(catalog.load_error.empty(), catalog.load_error);
    Require(!catalog.markers.empty(), "default catalog should load from executable directory when cwd differs");
    Require(catalog.path.filename() == "spectral_lines.public.tsv", "default catalog should report the loaded TSV path");
}
#endif

void TestRejectsDuplicateMarkerIds()
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "specforge_spectral_line_catalog_duplicate_ids.tsv";
    {
        std::ofstream stream(path);
        stream << "id\tlabel\tkind\tgroup\tvacuum_angstrom\tstart_vacuum_angstrom\tend_vacuum_angstrom\t"
                  "display_label\tsource_ref\n";
        stream << "h_alpha\tH alpha\tline\tBalmer\t6564.614\t\t\tHa\tNIST vacuum\n";
        stream << "h_alpha\tH alpha duplicate\tline\tBalmer\t6564.614\t\t\tHa\tNIST vacuum\n";
    }

    const specforge::SpectralLineCatalog catalog = specforge::LoadPublicSpectralLineCatalogFromPath(path);
    std::error_code remove_error;
    std::filesystem::remove(path, remove_error);

    Require(!catalog.load_error.empty(), "duplicate marker id should fail catalog loading");
    Require(Contains(catalog.load_error, "duplicate marker id"), "duplicate id error should be explicit");
    Require(Contains(catalog.load_error, "h_alpha"), "duplicate id error should include the id");
    Require(catalog.markers.empty(), "duplicate marker id should clear partial catalog results");
}

void TestRejectsEmptySourceRef()
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "specforge_spectral_line_catalog_empty_source_ref.tsv";
    {
        std::ofstream stream(path);
        stream << "id\tlabel\tkind\tgroup\tvacuum_angstrom\tstart_vacuum_angstrom\tend_vacuum_angstrom\t"
                  "display_label\tsource_ref\n";
        stream << "h_alpha\tH alpha\tline\tBalmer\t6564.614\t\t\tHa\t\n";
    }

    const specforge::SpectralLineCatalog catalog = specforge::LoadPublicSpectralLineCatalogFromPath(path);
    std::error_code remove_error;
    std::filesystem::remove(path, remove_error);

    Require(!catalog.load_error.empty(), "empty source_ref should fail catalog loading");
    Require(Contains(catalog.load_error, "source_ref"), "empty source_ref error should be explicit");
    Require(catalog.markers.empty(), "empty source_ref should clear partial catalog results");
}

void TestRejectsNonPositiveWavelengths()
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "specforge_spectral_line_catalog_non_positive_wavelength.tsv";
    {
        std::ofstream stream(path);
        stream << "id\tlabel\tkind\tgroup\tvacuum_angstrom\tstart_vacuum_angstrom\tend_vacuum_angstrom\t"
                  "display_label\tsource_ref\n";
        stream << "h_alpha\tH alpha\tline\tBalmer\t0\t\t\tHa\tNIST vacuum\n";
    }

    const specforge::SpectralLineCatalog catalog = specforge::LoadPublicSpectralLineCatalogFromPath(path);
    std::error_code remove_error;
    std::filesystem::remove(path, remove_error);

    Require(!catalog.load_error.empty(), "non-positive wavelength should fail catalog loading");
    Require(Contains(catalog.load_error, "finite and positive"), "non-positive wavelength error should be explicit");
    Require(catalog.markers.empty(), "non-positive wavelength should clear partial catalog results");
}

}  // namespace

int main()
{
    TestLoadsPublicCatalog();
    TestUsesVacuumWavelengthsForAtomicMarkers();
    TestPublicCatalogDoesNotContainPrivateOverlayConcepts();
    TestLoadsCatalogWithoutOptionalNotesColumn();
    TestGenericCatalogMayOmitGrouping();
    TestPublicCatalogRequiresGrouping();
#ifdef _WIN32
    TestDefaultCatalogLoadsFromExecutableDirectoryWhenCwdDiffers();
#endif
    TestRejectsDuplicateMarkerIds();
    TestRejectsEmptySourceRef();
    TestRejectsNonPositiveWavelengths();
    return 0;
}
