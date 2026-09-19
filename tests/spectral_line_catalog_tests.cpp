#include "app/runtime_paths.h"
#include "overlays/spectral_line_catalog.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

const spectiary::SpectralLineMarker& FindMarker(
    const spectiary::SpectralLineCatalog& catalog,
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
        std::filesystem::path(SPECTIARY_SOURCE_DIR) / "config" / "spectral_lines.public.tsv";
    const spectiary::SpectralLineCatalog catalog = spectiary::LoadPublicSpectralLineCatalogFromPath(path);
    Require(catalog.load_error.empty(), catalog.load_error);
    Require(catalog.markers.size() >= 30, "public catalog should contain the default reference markers");

    double previous_position = 0.0;
    bool first = true;
    for (const spectiary::SpectralLineMarker& marker : catalog.markers) {
        const double position = spectiary::SpectralLineMarkerPosition(marker);
        Require(first || position >= previous_position, "catalog markers should load in wavelength order");
        first = false;
        previous_position = position;
    }
}

void RequireScientificLabelUtf8(const spectiary::SpectralLineCatalog& catalog)
{
    Require(FindMarker(catalog, "h_alpha").label == "H\xCE\xB1", "H alpha should use compact Greek notation");
    Require(FindMarker(catalog, "h_beta").label == "H\xCE\xB2", "H beta should use compact Greek notation");
    Require(FindMarker(catalog, "h_gamma").label == "H\xCE\xB3", "H gamma should use compact Greek notation");
    Require(FindMarker(catalog, "h_delta").label == "H\xCE\xB4", "H delta should use compact Greek notation");
    Require(FindMarker(catalog, "c2_4383").label == "C\xE2\x82\x82", "C2 should use a subscript atom count");
    Require(
        FindMarker(catalog, "na_i_d2").label == "Na I D\xE2\x82\x82",
        "Na I D2 should use a subscript transition index");
    Require(
        FindMarker(catalog, "c13_c12_6100").label ==
            "\xC2\xB9\xC2\xB3"
            "C"
            "\xC2\xB9\xC2\xB2"
            "C",
        "the carbon isotopologue should use superscript mass numbers");
    Require(
        FindMarker(catalog, "c13_cn_6260").label ==
            "\xC2\xB9\xC2\xB3"
            "CN",
        "13CN should use a superscript mass number");
}

void TestPublicCatalogUsesScientificLabelTypography()
{
    const std::filesystem::path path =
        std::filesystem::path(SPECTIARY_SOURCE_DIR) / "config" / "spectral_lines.public.tsv";
    const spectiary::SpectralLineCatalog catalog = spectiary::LoadPublicSpectralLineCatalogFromPath(path);
    Require(catalog.load_error.empty(), catalog.load_error);
    RequireScientificLabelUtf8(catalog);
}

void TestUsesVacuumWavelengthsForAtomicMarkers()
{
    const std::filesystem::path path =
        std::filesystem::path(SPECTIARY_SOURCE_DIR) / "config" / "spectral_lines.public.tsv";
    const spectiary::SpectralLineCatalog catalog = spectiary::LoadPublicSpectralLineCatalogFromPath(path);
    Require(catalog.load_error.empty(), catalog.load_error);

    Require(NearlyEqual(*FindMarker(catalog, "h_alpha").vacuum_angstrom, 6564.608, 1.0e-6), "H alpha should use Atomic Line List vacuum wavelength");
    Require(NearlyEqual(*FindMarker(catalog, "h_beta").vacuum_angstrom, 4862.683, 1.0e-6), "H beta should use vacuum wavelength");
    Require(NearlyEqual(*FindMarker(catalog, "na_i_d2").vacuum_angstrom, 5891.583, 1.0e-6), "Na I D2 should use vacuum wavelength");
    Require(NearlyEqual(*FindMarker(catalog, "na_i_d1").vacuum_angstrom, 5897.558, 1.0e-6), "Na I D1 should use vacuum wavelength");
    const spectiary::SpectralLineMarker& lithium = FindMarker(catalog, "li_i_6708");
    Require(lithium.kind == spectiary::SpectralLineMarkerKind::Band, "Li I doublet should be one unresolved band marker");
    Require(
        NearlyEqual(*lithium.start_vacuum_angstrom, 6709.613, 1.0e-6) &&
            NearlyEqual(*lithium.end_vacuum_angstrom, 6709.764, 1.0e-6),
        "Li I band should preserve both Atomic Line List vacuum transitions");
    Require(Contains(lithium.notes, "unresolved"), "Li I notes should state that the doublet is unresolved");
    Require(NearlyEqual(*FindMarker(catalog, "k_i_7667").vacuum_angstrom, 7667.021, 1.0e-6), "K I 7667 should use Atomic Line List vacuum wavelength");
    Require(
        Contains(FindMarker(catalog, "k_i_7667").notes, "telluric O2"),
        "K I 7667 should carry a telluric O2 warning");
    Require(NearlyEqual(*FindMarker(catalog, "k_i_7701").vacuum_angstrom, 7701.093, 1.0e-6), "K I 7701 should use Atomic Line List vacuum wavelength");
    Require(NearlyEqual(*FindMarker(catalog, "ca_ii_8500").vacuum_angstrom, 8500.358, 1.0e-6), "Ca II triplet should use Atomic Line List vacuum wavelength");
    Require(NearlyEqual(*FindMarker(catalog, "mg_i_8809").vacuum_angstrom, 8809.175, 1.0e-6), "Mg I 8809 should use Atomic Line List vacuum wavelength");
    Require(NearlyEqual(*FindMarker(catalog, "sr_ii_4078").vacuum_angstrom, 4078.9, 1.0e-6), "Sr II should remain the curated approximate vacuum marker");
    Require(NearlyEqual(*FindMarker(catalog, "ba_ii_4555").vacuum_angstrom, 4555.301, 1.0e-6), "Ba II should use the converted observed wavelength");
    Require(NearlyEqual(*FindMarker(catalog, "ba_ii_6499").vacuum_angstrom, 6498.686, 1.0e-6), "Ba II should use the converted observed wavelength");

    Require(
        !NearlyEqual(*FindMarker(catalog, "h_alpha").vacuum_angstrom, 6562.801, 1.0e-3),
        "H alpha must not regress to air wavelength");
}

void TestPublicCatalogProvidesDqCarbonAtomicMarkers()
{
    const std::filesystem::path path =
        std::filesystem::path(SPECTIARY_SOURCE_DIR) / "config" / "spectral_lines.public.tsv";
    const spectiary::SpectralLineCatalog catalog = spectiary::LoadPublicSpectralLineCatalogFromPath(path);
    Require(catalog.load_error.empty(), catalog.load_error);

    const auto group_count = [&](std::string_view group) {
        return static_cast<std::size_t>(std::count_if(
            catalog.markers.begin(),
            catalog.markers.end(),
            [group](const spectiary::SpectralLineMarker& marker) {
                return marker.group == group;
            }));
    };
    Require(group_count("C I") == 9, "public catalog should expose nine selected C I markers");
    Require(group_count("C II") == 15, "public catalog should expose fifteen selected C II markers");

    const auto require_line = [&](std::string_view id, std::string_view group, double wavelength) {
        const spectiary::SpectralLineMarker& marker = FindMarker(catalog, id);
        Require(marker.kind == spectiary::SpectralLineMarkerKind::Line, "selected carbon line should remain a line marker");
        Require(marker.group == group, "selected carbon line should use its ionization-stage group");
        Require(marker.source_ref == "atll_v3_00b5", "selected carbon line should retain Atomic Line List provenance");
        Require(
            marker.vacuum_angstrom && NearlyEqual(*marker.vacuum_angstrom, wavelength, 1.0e-6),
            "selected carbon line should use the expected vacuum wavelength");
    };
    const auto require_multiplet = [&catalog](
                                       std::string_view id,
                                       std::string_view group,
                                       double start,
                                       double end) {
        const spectiary::SpectralLineMarker& marker = FindMarker(catalog, id);
        Require(marker.kind == spectiary::SpectralLineMarkerKind::Band, "unresolved carbon multiplet should use one band marker");
        Require(marker.group == group, "carbon multiplet should use its ionization-stage group");
        Require(marker.source_ref == "atll_v3_00b5", "carbon multiplet should retain Atomic Line List provenance");
        Require(
            marker.start_vacuum_angstrom && marker.end_vacuum_angstrom &&
                NearlyEqual(*marker.start_vacuum_angstrom, start, 1.0e-6) &&
                NearlyEqual(*marker.end_vacuum_angstrom, end, 1.0e-6),
            "carbon multiplet should preserve its vacuum component bounds");
        Require(Contains(marker.notes, "low-resolution multiplet"), "carbon multiplet notes should explain the combined marker");
        Require(Contains(marker.notes, "not for wavelength calibration"), "carbon multiplet should reject calibration use");
    };

    const std::array c_i_lines = {
        std::pair{"c_i_4270", 4270.221},
        std::pair{"c_i_4373", 4372.596},
        std::pair{"c_i_4771", 4771.361},
        std::pair{"c_i_4933", 4933.426},
        std::pair{"c_i_5054", 5053.575},
        std::pair{"c_i_5382", 5381.833},
        std::pair{"c_i_8337", 8337.440},
    };
    for (const auto& [id, wavelength] : c_i_lines) {
        require_line(id, "C I", wavelength);
    }
    require_multiplet("c_i_6015_multiplet", "C I", 6014.831, 6014.878);
    require_multiplet("c_i_7117_multiplet", "C I", 7117.134, 7117.144);

    const std::array c_ii_lines = {
        std::pair{"c_ii_3920", 3920.077},
        std::pair{"c_ii_3922", 3921.792},
        std::pair{"c_ii_4411", 4411.229},
        std::pair{"c_ii_5147", 5146.598},
        std::pair{"c_ii_5153", 5152.520},
        std::pair{"c_ii_5891", 5891.408},
        std::pair{"c_ii_5893", 5893.231},
        std::pair{"c_ii_6580", 6579.869},
        std::pair{"c_ii_6585", 6584.700},
    };
    for (const auto& [id, wavelength] : c_ii_lines) {
        require_line(id, "C II", wavelength);
    }
    Require(
        Contains(FindMarker(catalog, "c_ii_5891").notes, "Na I D2"),
        "C II 5891 should warn about its low-resolution Na I blend");
    require_multiplet("c_ii_4076_multiplet", "C II", 4075.631, 4075.991);
    require_multiplet("c_ii_4268_multiplet", "C II", 4268.202, 4268.462);
    require_multiplet("c_ii_4374_multiplet", "C II", 4373.604, 4373.743);
    require_multiplet("c_ii_4620_multiplet", "C II", 4619.853, 4620.543);
    require_multiplet("c_ii_6153_multiplet", "C II", 6152.968, 6153.237);
    require_multiplet("c_ii_6464_multiplet", "C II", 6463.736, 6463.915);

    Require(
        !NearlyEqual(*FindMarker(catalog, "c_i_7117_multiplet").start_vacuum_angstrom, 7117.2, 1.0e-3),
        "C I 7117 must not retain the approximate candidate wavelength");
    Require(
        !NearlyEqual(*FindMarker(catalog, "c_ii_4620_multiplet").start_vacuum_angstrom, 4619.7, 1.0e-3),
        "C II 4620 must not retain the unsupported candidate wavelength");
    Require(
        !NearlyEqual(*FindMarker(catalog, "c_ii_5891").vacuum_angstrom, 5891.6, 1.0e-3),
        "C II 5891 must not retain the Na I-like candidate wavelength");
}

void TestPublicCatalogDoesNotContainPrivateOverlayConcepts()
{
    const std::filesystem::path path =
        std::filesystem::path(SPECTIARY_SOURCE_DIR) / "config" / "spectral_lines.public.tsv";
    const spectiary::SpectralLineCatalog catalog = spectiary::LoadPublicSpectralLineCatalogFromPath(path);
    Require(catalog.load_error.empty(), catalog.load_error);

    for (const spectiary::SpectralLineMarker& marker : catalog.markers) {
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
        std::filesystem::temp_directory_path() / "spectiary_spectral_line_catalog_without_notes.tsv";
    {
        std::ofstream stream(path);
        stream << "id\tlabel\tkind\tgroup\tvacuum_angstrom\tstart_vacuum_angstrom\tend_vacuum_angstrom\t"
                  "display_label\tsource_ref\n";
        stream << "h_alpha\tH alpha\tline\tBalmer\t6564.614\t\t\tHa\tNIST vacuum\n";
    }

    const spectiary::SpectralLineCatalog catalog = spectiary::LoadPublicSpectralLineCatalogFromPath(path);
    std::error_code remove_error;
    std::filesystem::remove(path, remove_error);

    Require(catalog.load_error.empty(), catalog.load_error);
    Require(catalog.markers.size() == 1, "catalog without notes should load one marker");
    Require(catalog.markers.front().notes.empty(), "missing optional notes column should default to empty");
}

void TestGenericCatalogMayOmitGrouping()
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "spectiary_spectral_line_catalog_without_group.tsv";
    {
        std::ofstream stream(path);
        stream << "id\tlabel\tkind\tvacuum_angstrom\tstart_vacuum_angstrom\tend_vacuum_angstrom\t"
                  "display_label\tsource_ref\n";
        stream << "marker_a\tMarker A\tline\t4100.0\t\t\tA\ttest\n";
        stream << "marker_b\tMarker B\tline\t4200.0\t\t\tB\ttest\n";
    }

    const spectiary::SpectralLineCatalog catalog = spectiary::LoadSpectralLineCatalogFromPath(path);
    std::error_code remove_error;
    std::filesystem::remove(path, remove_error);

    Require(catalog.load_error.empty(), catalog.load_error);
    Require(catalog.markers.size() == 2, "generic ungrouped catalog should load markers");
    Require(catalog.markers.front().group.empty(), "missing generic group column should default to empty group");
}

void TestPublicCatalogRequiresGrouping()
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "spectiary_public_spectral_line_catalog_empty_group.tsv";
    {
        std::ofstream stream(path);
        stream << "id\tlabel\tkind\tgroup\tvacuum_angstrom\tstart_vacuum_angstrom\tend_vacuum_angstrom\t"
                  "display_label\tsource_ref\n";
        stream << "h_alpha\tH alpha\tline\t\t6564.614\t\t\tHa\tNIST vacuum\n";
    }

    const spectiary::SpectralLineCatalog catalog = spectiary::LoadPublicSpectralLineCatalogFromPath(path);
    std::error_code remove_error;
    std::filesystem::remove(path, remove_error);

    Require(!catalog.load_error.empty(), "public catalog should reject empty groups");
    Require(Contains(catalog.load_error, "marker group is empty"), "public group error should be explicit");
    Require(catalog.markers.empty(), "public group failure should clear partial catalog results");
}

void TestPackagedCatalogPathIsIndependentOfCurrentDirectory()
{
    const std::filesystem::path path =
        spectiary::DefaultRuntimePaths().public_spectral_line_catalog_path;
    const ScopedCurrentPath scoped_current_path(std::filesystem::temp_directory_path());

    const spectiary::SpectralLineCatalog catalog =
        spectiary::LoadPackagedPublicSpectralLineCatalog(path);

    Require(catalog.load_error.empty(), catalog.load_error);
    Require(
        !catalog.markers.empty(),
        "packaged catalog should load from its explicit runtime path when cwd differs");
    RequireScientificLabelUtf8(catalog);
#ifdef SPECTIARY_EXPECT_EMBEDDED_PUBLIC_SPECTRAL_LINES
    Require(
        catalog.path.is_relative(),
        "static-release catalog test should load the embedded resource, not an external TSV");
#else
    Require(
        catalog.path == path,
        "external packaged catalog should report the exact RuntimePaths location");
#endif
}

void TestRejectsDuplicateMarkerIds()
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "spectiary_spectral_line_catalog_duplicate_ids.tsv";
    {
        std::ofstream stream(path);
        stream << "id\tlabel\tkind\tgroup\tvacuum_angstrom\tstart_vacuum_angstrom\tend_vacuum_angstrom\t"
                  "display_label\tsource_ref\n";
        stream << "h_alpha\tH alpha\tline\tBalmer\t6564.614\t\t\tHa\tNIST vacuum\n";
        stream << "h_alpha\tH alpha duplicate\tline\tBalmer\t6564.614\t\t\tHa\tNIST vacuum\n";
    }

    const spectiary::SpectralLineCatalog catalog = spectiary::LoadPublicSpectralLineCatalogFromPath(path);
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
        std::filesystem::temp_directory_path() / "spectiary_spectral_line_catalog_empty_source_ref.tsv";
    {
        std::ofstream stream(path);
        stream << "id\tlabel\tkind\tgroup\tvacuum_angstrom\tstart_vacuum_angstrom\tend_vacuum_angstrom\t"
                  "display_label\tsource_ref\n";
        stream << "h_alpha\tH alpha\tline\tBalmer\t6564.614\t\t\tHa\t\n";
    }

    const spectiary::SpectralLineCatalog catalog = spectiary::LoadPublicSpectralLineCatalogFromPath(path);
    std::error_code remove_error;
    std::filesystem::remove(path, remove_error);

    Require(!catalog.load_error.empty(), "empty source_ref should fail catalog loading");
    Require(Contains(catalog.load_error, "source_ref"), "empty source_ref error should be explicit");
    Require(catalog.markers.empty(), "empty source_ref should clear partial catalog results");
}

void TestRejectsNonPositiveWavelengths()
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "spectiary_spectral_line_catalog_non_positive_wavelength.tsv";
    {
        std::ofstream stream(path);
        stream << "id\tlabel\tkind\tgroup\tvacuum_angstrom\tstart_vacuum_angstrom\tend_vacuum_angstrom\t"
                  "display_label\tsource_ref\n";
        stream << "h_alpha\tH alpha\tline\tBalmer\t0\t\t\tHa\tNIST vacuum\n";
    }

    const spectiary::SpectralLineCatalog catalog = spectiary::LoadPublicSpectralLineCatalogFromPath(path);
    std::error_code remove_error;
    std::filesystem::remove(path, remove_error);

    Require(!catalog.load_error.empty(), "non-positive wavelength should fail catalog loading");
    Require(Contains(catalog.load_error, "finite and positive"), "non-positive wavelength error should be explicit");
    Require(catalog.markers.empty(), "non-positive wavelength should clear partial catalog results");
}

}  // namespace

int main()
{
    try {
        TestLoadsPublicCatalog();
        TestPublicCatalogUsesScientificLabelTypography();
        TestUsesVacuumWavelengthsForAtomicMarkers();
        TestPublicCatalogProvidesDqCarbonAtomicMarkers();
        TestPublicCatalogDoesNotContainPrivateOverlayConcepts();
        TestLoadsCatalogWithoutOptionalNotesColumn();
        TestGenericCatalogMayOmitGrouping();
        TestPublicCatalogRequiresGrouping();
        TestPackagedCatalogPathIsIndependentOfCurrentDirectory();
        TestRejectsDuplicateMarkerIds();
        TestRejectsEmptySourceRef();
        TestRejectsNonPositiveWavelengths();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
