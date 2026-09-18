#include "ui/spectrum_plot_preferences_io.h"

#include "app/local_user_state_json.h"

#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace specforge {
namespace {

constexpr const char* kPreferencesFormatKind =
    "specforge.spectrum_plot.preferences";
constexpr int kPreferencesSchemaVersion = 1;
constexpr std::string_view kSeriesColorsMember =
    "series_colors";
constexpr std::string_view kAutoColorMode = "auto";
constexpr std::string_view kExplicitColorMode =
    "explicit-color";

void AppendWarning(
    std::string& warning,
    std::string_view message)
{
    if (!warning.empty()) {
        warning += ' ';
    }
    warning += message;
}

std::optional<double> ParseFiniteDouble(
    const nlohmann::json& root,
    std::string_view key)
{
    const std::optional<std::string> text =
        ReadJsonStringMember(root, key);
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

std::optional<std::string> EncodeFiniteDouble(double value)
{
    if (!std::isfinite(value)) {
        return std::nullopt;
    }
    std::array<char, 128> buffer{};
    const std::to_chars_result encoded = std::to_chars(
        buffer.data(),
        buffer.data() + buffer.size(),
        value,
        std::chars_format::general,
        std::numeric_limits<double>::max_digits10);
    if (encoded.ec != std::errc{}) {
        return std::nullopt;
    }
    return std::string(buffer.data(), encoded.ptr);
}

bool ColorChannelIsUsable(double value)
{
    return std::isfinite(value) && value >= 0.0 &&
           value <= 1.0;
}

std::optional<nlohmann::json> EncodePlotSeriesColor(
    const PlotSeriesColor& selection)
{
    if (selection.mode() ==
        PlotSeriesColorMode::Auto) {
        return nlohmann::json::object({
            {"mode", nlohmann::json(kAutoColorMode)},
        });
    }

    const RgbaColor& color =
        *selection.explicit_color();
    if (!ColorChannelIsUsable(color.red) ||
        !ColorChannelIsUsable(color.green) ||
        !ColorChannelIsUsable(color.blue) ||
        !ColorChannelIsUsable(color.alpha)) {
        return std::nullopt;
    }
    const std::optional<std::string> red =
        EncodeFiniteDouble(color.red);
    const std::optional<std::string> green =
        EncodeFiniteDouble(color.green);
    const std::optional<std::string> blue =
        EncodeFiniteDouble(color.blue);
    const std::optional<std::string> alpha =
        EncodeFiniteDouble(color.alpha);
    if (!red || !green || !blue || !alpha) {
        return std::nullopt;
    }
    return nlohmann::json::object({
        {"mode", nlohmann::json(kExplicitColorMode)},
        {"red", nlohmann::json(*red)},
        {"green", nlohmann::json(*green)},
        {"blue", nlohmann::json(*blue)},
        {"alpha", nlohmann::json(*alpha)},
    });
}

PlotSeriesColor ParsePlotSeriesColor(
    const nlohmann::json& colors,
    std::string_view stable_series_id,
    std::string& warning)
{
    const nlohmann::json* encoded =
        JsonObjectMember(colors, stable_series_id);
    const std::optional<std::string> mode = encoded
        ? ReadJsonStringMember(*encoded, "mode")
        : std::nullopt;
    if (!encoded || encoded->type() != nlohmann::json::value_t::object ||
        !mode) {
        AppendWarning(
            warning,
            "A saved spectrum series color was invalid; Auto was used for that curve.");
        return PlotSeriesColor::Auto();
    }
    if (*mode == kAutoColorMode) {
        return PlotSeriesColor::Auto();
    }
    if (*mode != kExplicitColorMode) {
        AppendWarning(
            warning,
            "A saved spectrum series color mode was invalid; Auto was used for that curve.");
        return PlotSeriesColor::Auto();
    }

    const std::optional<double> red =
        ParseFiniteDouble(*encoded, "red");
    const std::optional<double> green =
        ParseFiniteDouble(*encoded, "green");
    const std::optional<double> blue =
        ParseFiniteDouble(*encoded, "blue");
    const std::optional<double> alpha =
        ParseFiniteDouble(*encoded, "alpha");
    if (!red || !green || !blue || !alpha ||
        !ColorChannelIsUsable(*red) ||
        !ColorChannelIsUsable(*green) ||
        !ColorChannelIsUsable(*blue) ||
        !ColorChannelIsUsable(*alpha)) {
        AppendWarning(
            warning,
            "A saved explicit spectrum color had invalid RGBA channels; Auto was used for that curve.");
        return PlotSeriesColor::Auto();
    }
    return PlotSeriesColor::ExplicitColor({
        .red = static_cast<float>(*red),
        .green = static_cast<float>(*green),
        .blue = static_cast<float>(*blue),
        .alpha = static_cast<float>(*alpha),
    });
}

std::optional<nlohmann::json> EncodeSpectrumPlotColors(
    const SpectrumPlotColors& colors)
{
    std::optional<nlohmann::json> raw =
        EncodePlotSeriesColor(colors.raw_spectrum);
    std::optional<nlohmann::json> gaussian =
        EncodePlotSeriesColor(colors.gaussian_smoothing);
    std::optional<nlohmann::json> median =
        EncodePlotSeriesColor(colors.median_smoothing);
    if (!raw || !gaussian || !median) {
        return std::nullopt;
    }

    nlohmann::json encoded = nlohmann::json::object();
    encoded.get_ref<nlohmann::json::object_t&>().emplace(
        std::string(kRawSpectrumPlotSeriesId),
        std::move(*raw));
    encoded.get_ref<nlohmann::json::object_t&>().emplace(
        std::string(kGaussianSmoothingPlotSeriesId),
        std::move(*gaussian));
    encoded.get_ref<nlohmann::json::object_t&>().emplace(
        std::string(kMedianSmoothingPlotSeriesId),
        std::move(*median));
    return encoded;
}

}  // namespace

std::filesystem::path DefaultSpectrumPlotPreferencesPath(const RuntimePaths& runtime_paths)
{
    return runtime_paths.spectrum_plot_preferences_path;
}

SpectrumPlotPreferencesLoadResult LoadSpectrumPlotPreferences(
    const std::filesystem::path& path)
{
    SpectrumPlotPreferencesLoadResult loaded;
    VersionedJsonCacheLoadResult result =
        LoadVersionedJsonCacheFile(
            path,
            kPreferencesFormatKind,
            {kPreferencesSchemaVersion},
            "spectrum plot preferences");
    loaded.document_present = result.document.has_value();
    loaded.issue_kind = result.issue_kind;
    loaded.warning = std::move(result.warning);
    loaded.diagnostic_detail =
        std::move(result.diagnostic_detail);
    if (!result.document) {
        return loaded;
    }

    const nlohmann::json& root = result.document->root;
    const nlohmann::json* series_colors =
        JsonObjectMember(root, kSeriesColorsMember);
    if (series_colors != nullptr &&
        series_colors->type() == nlohmann::json::value_t::object) {
        loaded.state.plot_colors.raw_spectrum =
            ParsePlotSeriesColor(
                *series_colors,
                kRawSpectrumPlotSeriesId,
                loaded.warning);
        loaded.state.plot_colors.gaussian_smoothing =
            ParsePlotSeriesColor(
                *series_colors,
                kGaussianSmoothingPlotSeriesId,
                loaded.warning);
        loaded.state.plot_colors.median_smoothing =
            ParsePlotSeriesColor(
                *series_colors,
                kMedianSmoothingPlotSeriesId,
                loaded.warning);
    } else {
        AppendWarning(
            loaded.warning,
            "Saved spectrum series colors were invalid; Auto colors were used.");
    }

    return loaded;
}

bool SaveSpectrumPlotPreferences(
    const std::filesystem::path& path,
    const SpectrumPlotPreferences& state,
    std::string* error_message)
{
    if (path.empty()) {
        if (error_message != nullptr) {
            *error_message =
                "Spectrum plot preferences path is empty.";
        }
        return false;
    }

    const std::optional<nlohmann::json> series_colors =
        EncodeSpectrumPlotColors(state.plot_colors);
    if (!series_colors) {
        if (error_message != nullptr) {
            *error_message =
                "Spectrum series colors contain invalid RGBA channels.";
        }
        return false;
    }

    nlohmann::json body = nlohmann::json::object({
        {std::string(kSeriesColorsMember), *series_colors},
    });

    return WriteVersionedJsonCacheDocument(
        path,
        kPreferencesFormatKind,
        kPreferencesSchemaVersion,
        "spectrum plot preferences",
        body,
        error_message);
}

}  // namespace specforge
