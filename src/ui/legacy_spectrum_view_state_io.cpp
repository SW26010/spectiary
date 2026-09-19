#include "ui/legacy_spectrum_view_state_io.h"

#include "app/local_user_state_json.h"

#include <charconv>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace spectiary {
namespace {

constexpr const char* kStateFormatKind =
    "specforge.spectrum_view.state";
constexpr int kStateSchemaVersion = 2;
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

bool LimitsAreUsable(const PlotViewLimits& limits)
{
    return std::isfinite(limits.x_min) &&
           std::isfinite(limits.x_max) &&
           std::isfinite(limits.y_min) &&
           std::isfinite(limits.y_max) &&
           limits.x_min < limits.x_max &&
           limits.y_min < limits.y_max;
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

bool ColorChannelIsUsable(double value)
{
    return std::isfinite(value) && value >= 0.0 &&
           value <= 1.0;
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

}  // namespace

LegacySpectrumViewStateLoadResult LoadLegacySpectrumViewState(
    const std::filesystem::path& path)
{
    LegacySpectrumViewStateLoadResult loaded;
    VersionedJsonCacheLoadResult result =
        LoadVersionedJsonCacheFile(
            path,
            kStateFormatKind,
            {1, kStateSchemaVersion},
            "spectrum view state cache");
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
    } else if (result.document->schema_version >= 2) {
        AppendWarning(
            loaded.warning,
            "Saved spectrum series colors were invalid; Auto colors were used.");
    }

    const nlohmann::json* locked =
        JsonObjectMember(root, "locked");
    if (locked == nullptr ||
        locked->type() != nlohmann::json::value_t::boolean) {
        AppendWarning(
            loaded.warning,
            "Spectrum view state cache member 'locked' must be boolean; automatic range was used.");
        return loaded;
    }
    if (!locked->get<bool>()) {
        return loaded;
    }

    const std::optional<std::string> identity =
        ReadJsonStringMember(
            root,
            "source_collection_identity");
    const std::optional<double> x_min =
        ParseFiniteDouble(root, "x_min");
    const std::optional<double> x_max =
        ParseFiniteDouble(root, "x_max");
    const std::optional<double> y_min =
        ParseFiniteDouble(root, "y_min");
    const std::optional<double> y_max =
        ParseFiniteDouble(root, "y_max");
    if (!identity || identity->empty() || !x_min ||
        !x_max || !y_min || !y_max) {
        AppendWarning(
            loaded.warning,
            "Locked spectrum view state is incomplete; automatic range was used.");
        return loaded;
    }

    const PlotViewLimits restored_limits{
        .x_min = *x_min,
        .x_max = *x_max,
        .y_min = *y_min,
        .y_max = *y_max,
    };
    if (!LimitsAreUsable(restored_limits)) {
        AppendWarning(
            loaded.warning,
            "Locked spectrum view state has invalid axis ranges; automatic range was used.");
        return loaded;
    }
    loaded.state.locked = true;
    loaded.state.source_collection_identity =
        *identity;
    loaded.state.limits = restored_limits;
    return loaded;
}

}  // namespace spectiary
