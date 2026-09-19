#include "app/runtime_paths.h"
#include "overlays/spectral_line_list_json.h"

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

const spectiary::line_list::Marker& FindMarker(
    const spectiary::SpectralLineList& catalog,
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

std::string MarkerGroup(const spectiary::SpectralLineList& list, std::string_view id) {
    for (const auto& view : list.grouping_views) for (const auto& group : view.groups)
        if (std::find(group.marker_ids.begin(), group.marker_ids.end(), id) != group.marker_ids.end()) return group.name;
    return {};
}
void TestLoadsPublicCatalog()
{
    const std::filesystem::path path =
        std::filesystem::path(SPECTIARY_SOURCE_DIR) / "config" / "spectral_lines.public.json";
    const auto loaded = spectiary::LoadSpectralLineListFromPath(path);
    Require(loaded.list.has_value(), loaded.error);
    const auto& catalog = *loaded.list;

    Require(catalog.markers.size() >= 30, "public catalog should contain the default reference markers");

}

void RequireScientificLabelUtf8(const spectiary::SpectralLineList& catalog)
{
    Require(FindMarker(catalog, "h_alpha").name == "H\xCE\xB1", "H alpha should use compact Greek notation");
    Require(FindMarker(catalog, "h_beta").name == "H\xCE\xB2", "H beta should use compact Greek notation");
    Require(FindMarker(catalog, "h_gamma").name == "H\xCE\xB3", "H gamma should use compact Greek notation");
    Require(FindMarker(catalog, "h_delta").name == "H\xCE\xB4", "H delta should use compact Greek notation");
    Require(FindMarker(catalog, "c2_4383").name == "C\xE2\x82\x82", "C2 should use a subscript atom count");
    Require(
        FindMarker(catalog, "na_i_d2").name == "Na I D\xE2\x82\x82",
        "Na I D2 should use a subscript transition index");
    Require(
        FindMarker(catalog, "c13_c12_6100").name ==
            "\xC2\xB9\xC2\xB3"
            "C"
            "\xC2\xB9\xC2\xB2"
            "C",
        "the carbon isotopologue should use superscript mass numbers");
    Require(
        FindMarker(catalog, "c13_cn_6260").name ==
            "\xC2\xB9\xC2\xB3"
            "CN",
        "13CN should use a superscript mass number");
}

void TestPublicCatalogUsesScientificLabelTypography()
{
    const std::filesystem::path path =
        std::filesystem::path(SPECTIARY_SOURCE_DIR) / "config" / "spectral_lines.public.json";
    const auto loaded = spectiary::LoadSpectralLineListFromPath(path);
    Require(loaded.list.has_value(), loaded.error);
    const auto& catalog = *loaded.list;

    RequireScientificLabelUtf8(catalog);
}

void TestUsesVacuumWavelengthsForAtomicMarkers()
{
    const std::filesystem::path path =
        std::filesystem::path(SPECTIARY_SOURCE_DIR) / "config" / "spectral_lines.public.json";
    const auto loaded = spectiary::LoadSpectralLineListFromPath(path);
    Require(loaded.list.has_value(), loaded.error);
    const auto& catalog = *loaded.list;


    Require(NearlyEqual(*FindMarker(catalog, "h_alpha").coordinate, 6564.608, 1.0e-6), "H alpha should use Atomic Line List vacuum wavelength");
    Require(NearlyEqual(*FindMarker(catalog, "h_beta").coordinate, 4862.683, 1.0e-6), "H beta should use vacuum wavelength");
    Require(NearlyEqual(*FindMarker(catalog, "na_i_d2").coordinate, 5891.583, 1.0e-6), "Na I D2 should use vacuum wavelength");
    Require(NearlyEqual(*FindMarker(catalog, "na_i_d1").coordinate, 5897.558, 1.0e-6), "Na I D1 should use vacuum wavelength");
    const spectiary::line_list::Marker& lithium = FindMarker(catalog, "li_i_6708");
    Require(lithium.kind == spectiary::line_list::MarkerKind::Band, "Li I doublet should be one unresolved band marker");
    Require(
        NearlyEqual(*lithium.start, 6709.613, 1.0e-6) &&
            NearlyEqual(*lithium.end, 6709.764, 1.0e-6),
        "Li I band should preserve both Atomic Line List vacuum transitions");
    Require(Contains(lithium.note.value_or(""), "unresolved"), "Li I notes should state that the doublet is unresolved");
    Require(NearlyEqual(*FindMarker(catalog, "k_i_7667").coordinate, 7667.021, 1.0e-6), "K I 7667 should use Atomic Line List vacuum wavelength");
    Require(
        Contains(FindMarker(catalog, "k_i_7667").note.value_or(""), "telluric O2"),
        "K I 7667 should carry a telluric O2 warning");
    Require(NearlyEqual(*FindMarker(catalog, "k_i_7701").coordinate, 7701.093, 1.0e-6), "K I 7701 should use Atomic Line List vacuum wavelength");
    Require(NearlyEqual(*FindMarker(catalog, "ca_ii_8500").coordinate, 8500.358, 1.0e-6), "Ca II triplet should use Atomic Line List vacuum wavelength");
    Require(NearlyEqual(*FindMarker(catalog, "mg_i_8809").coordinate, 8809.175, 1.0e-6), "Mg I 8809 should use Atomic Line List vacuum wavelength");
    Require(NearlyEqual(*FindMarker(catalog, "sr_ii_4078").coordinate, 4078.9, 1.0e-6), "Sr II should remain the curated approximate vacuum marker");
    Require(NearlyEqual(*FindMarker(catalog, "ba_ii_4555").coordinate, 4555.301, 1.0e-6), "Ba II should use the converted observed wavelength");
    Require(NearlyEqual(*FindMarker(catalog, "ba_ii_6499").coordinate, 6498.686, 1.0e-6), "Ba II should use the converted observed wavelength");

    Require(
        !NearlyEqual(*FindMarker(catalog, "h_alpha").coordinate, 6562.801, 1.0e-3),
        "H alpha must not regress to air wavelength");
}

void TestPublicCatalogProvidesDqCarbonAtomicMarkers()
{
    const std::filesystem::path path =
        std::filesystem::path(SPECTIARY_SOURCE_DIR) / "config" / "spectral_lines.public.json";
    const auto loaded = spectiary::LoadSpectralLineListFromPath(path);
    Require(loaded.list.has_value(), loaded.error);
    const auto& catalog = *loaded.list;


    const auto group_count = [&](std::string_view group) {
        return static_cast<std::size_t>(std::count_if(
            catalog.markers.begin(),
            catalog.markers.end(),
            [&catalog, group](const spectiary::line_list::Marker& marker) {
                return MarkerGroup(catalog, marker.id) == group;
            }));
    };
    Require(group_count("C I") == 9, "public catalog should expose nine selected C I markers");
    Require(group_count("C II") == 15, "public catalog should expose fifteen selected C II markers");

    const auto require_line = [&](std::string_view id, std::string_view group, double wavelength) {
        const spectiary::line_list::Marker& marker = FindMarker(catalog, id);
        Require(marker.kind == spectiary::line_list::MarkerKind::Line, "selected carbon line should remain a line marker");
        Require(MarkerGroup(catalog, marker.id) == group, "selected carbon line should use its ionization-stage group");
        Require(
            marker.coordinate && NearlyEqual(*marker.coordinate, wavelength, 1.0e-6),
            "selected carbon line should use the expected vacuum wavelength");
    };
    const auto require_multiplet = [&catalog](
                                       std::string_view id,
                                       std::string_view group,
                                       double start,
                                       double end) {
        const spectiary::line_list::Marker& marker = FindMarker(catalog, id);
        Require(marker.kind == spectiary::line_list::MarkerKind::Band, "unresolved carbon multiplet should use one band marker");
        Require(MarkerGroup(catalog, marker.id) == group, "carbon multiplet should use its ionization-stage group");
        Require(
            marker.start && marker.end &&
                NearlyEqual(*marker.start, start, 1.0e-6) &&
                NearlyEqual(*marker.end, end, 1.0e-6),
            "carbon multiplet should preserve its vacuum component bounds");
        Require(Contains(marker.note.value_or(""), "low-resolution multiplet"), "carbon multiplet notes should explain the combined marker");
        Require(Contains(marker.note.value_or(""), "not for wavelength calibration"), "carbon multiplet should reject calibration use");
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
        Contains(FindMarker(catalog, "c_ii_5891").note.value_or(""), "Na I D2"),
        "C II 5891 should warn about its low-resolution Na I blend");
    require_multiplet("c_ii_4076_multiplet", "C II", 4075.631, 4075.991);
    require_multiplet("c_ii_4268_multiplet", "C II", 4268.202, 4268.462);
    require_multiplet("c_ii_4374_multiplet", "C II", 4373.604, 4373.743);
    require_multiplet("c_ii_4620_multiplet", "C II", 4619.853, 4620.543);
    require_multiplet("c_ii_6153_multiplet", "C II", 6152.968, 6153.237);
    require_multiplet("c_ii_6464_multiplet", "C II", 6463.736, 6463.915);

    Require(
        !NearlyEqual(*FindMarker(catalog, "c_i_7117_multiplet").start, 7117.2, 1.0e-3),
        "C I 7117 must not retain the approximate candidate wavelength");
    Require(
        !NearlyEqual(*FindMarker(catalog, "c_ii_4620_multiplet").start, 4619.7, 1.0e-3),
        "C II 4620 must not retain the unsupported candidate wavelength");
    Require(
        !NearlyEqual(*FindMarker(catalog, "c_ii_5891").coordinate, 5891.6, 1.0e-3),
        "C II 5891 must not retain the Na I-like candidate wavelength");
}

void TestPublicCatalogDoesNotContainPrivateOverlayConcepts()
{
    const std::filesystem::path path =
        std::filesystem::path(SPECTIARY_SOURCE_DIR) / "config" / "spectral_lines.public.json";
    const auto loaded = spectiary::LoadSpectralLineListFromPath(path);
    Require(loaded.list.has_value(), loaded.error);
    const auto& catalog = *loaded.list;


    for (const spectiary::line_list::Marker& marker : catalog.markers) {
        Require(!Contains(marker.id, "window"), "public catalog must not contain zoom windows");
        Require(!Contains(MarkerGroup(catalog, marker.id), "subtype"), "public catalog must not contain subtype groups");
        Require(MarkerGroup(catalog, marker.id) != "C-H", "public catalog must not contain C-H subtype preset groups");
        Require(MarkerGroup(catalog, marker.id) != "C-R", "public catalog must not contain C-R subtype preset groups");
        Require(MarkerGroup(catalog, marker.id) != "C-N", "public catalog must not contain C-N subtype preset groups");
        Require(MarkerGroup(catalog, marker.id) != "C-J", "public catalog must not contain C-J subtype preset groups");
        Require(MarkerGroup(catalog, marker.id) != "C-Hd", "public catalog must not contain C-Hd subtype preset groups");
        Require(MarkerGroup(catalog, marker.id) != "Ba", "public catalog must not contain Ba subtype preset groups");
        Require(MarkerGroup(catalog, marker.id) != "Binary", "public catalog must not contain Binary subtype preset groups");
        Require(MarkerGroup(catalog, marker.id) != "GDQ", "public catalog must not contain GDQ subtype preset groups");
    }
}

void TestPackagedListUsesProductionCodec() {
    const auto path = std::filesystem::path(SPECTIARY_SOURCE_DIR) / "config/spectral_lines.public.json";
    const auto file = spectiary::LoadSpectralLineListFromPath(path);
    const auto packaged = spectiary::LoadPackagedPublicSpectralLineList(path);
    Require(file.list && packaged.list && file.list == packaged.list, "packaged and file codecs must agree");
#ifdef SPECTIARY_EXPECT_EMBEDDED_PUBLIC_SPECTRAL_LINES
    const auto embedded = spectiary::LoadPackagedPublicSpectralLineList(path / "not-a-file");
    Require(embedded.list == file.list, "embedded release loader must not depend on an external data file");
#else
    Require(!spectiary::LoadPackagedPublicSpectralLineList(path / "not-a-file").list,
            "external package loader must report a missing JSON resource");
#endif
}
} // namespace
int main() {
    try {
        TestLoadsPublicCatalog(); TestPublicCatalogUsesScientificLabelTypography();
        TestUsesVacuumWavelengthsForAtomicMarkers(); TestPublicCatalogProvidesDqCarbonAtomicMarkers();
        TestPublicCatalogDoesNotContainPrivateOverlayConcepts(); TestPackagedListUsesProductionCodec();
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
