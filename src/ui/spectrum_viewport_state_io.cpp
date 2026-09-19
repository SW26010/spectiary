#include "ui/spectrum_viewport_state_io.h"

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

namespace spectiary {
namespace {

constexpr const char* kStateFormatKind =
    "spectiary.spectrum_viewport.state";
constexpr int kStateSchemaVersion = 1;

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

}  // namespace

std::filesystem::path DefaultSpectrumViewportStatePath(const RuntimePaths& runtime_paths)
{
    return runtime_paths.spectrum_viewport_state_path;
}

SpectrumViewportStateLoadResult LoadSpectrumViewportState(
    const std::filesystem::path& path)
{
    SpectrumViewportStateLoadResult loaded;
    VersionedJsonCacheLoadResult result =
        LoadVersionedJsonCacheFile(
            path,
            kStateFormatKind,
            {kStateSchemaVersion},
            "spectrum viewport state");
    loaded.document_present = result.document.has_value();
    loaded.issue_kind = result.issue_kind;
    loaded.warning = std::move(result.warning);
    loaded.diagnostic_detail =
        std::move(result.diagnostic_detail);
    if (!result.document) {
        return loaded;
    }

    const nlohmann::json& root = result.document->root;
    const nlohmann::json* locked =
        JsonObjectMember(root, "locked");
    if (locked == nullptr ||
        locked->type() != nlohmann::json::value_t::boolean) {
        AppendWarning(
            loaded.warning,
            "Spectrum viewport state member 'locked' must be boolean; automatic range was used.");
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
            "Locked spectrum viewport state is incomplete; automatic range was used.");
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
            "Locked spectrum viewport state has invalid axis ranges; automatic range was used.");
        return loaded;
    }
    loaded.state.locked = true;
    loaded.state.source_collection_identity =
        *identity;
    loaded.state.limits = restored_limits;
    return loaded;
}

bool SaveSpectrumViewportState(
    const std::filesystem::path& path,
    const SpectrumViewportState& state,
    std::string* error_message)
{
    if (path.empty()) {
        if (error_message != nullptr) {
            *error_message =
                "Spectrum viewport state path is empty.";
        }
        return false;
    }

    if (state.locked &&
        (state.source_collection_identity.empty() ||
         !LimitsAreUsable(state.limits))) {
        if (error_message != nullptr) {
            *error_message =
                "Locked spectrum viewport state is incomplete or invalid.";
        }
        return false;
    }

    nlohmann::json body = nlohmann::json::object({
        {"locked", nlohmann::json(state.locked)},
    });
    if (state.locked) {
        const std::optional<std::string> x_min =
            EncodeFiniteDouble(state.limits.x_min);
        const std::optional<std::string> x_max =
            EncodeFiniteDouble(state.limits.x_max);
        const std::optional<std::string> y_min =
            EncodeFiniteDouble(state.limits.y_min);
        const std::optional<std::string> y_max =
            EncodeFiniteDouble(state.limits.y_max);
        if (!x_min || !x_max || !y_min || !y_max) {
            if (error_message != nullptr) {
                *error_message =
                    "Spectrum viewport axis ranges could not be encoded.";
            }
            return false;
        }
        body.get_ref<nlohmann::json::object_t&>().emplace(
            "source_collection_identity",
            nlohmann::json(
                state.source_collection_identity));
        body.get_ref<nlohmann::json::object_t&>().emplace("x_min", nlohmann::json(*x_min));
        body.get_ref<nlohmann::json::object_t&>().emplace("x_max", nlohmann::json(*x_max));
        body.get_ref<nlohmann::json::object_t&>().emplace("y_min", nlohmann::json(*y_min));
        body.get_ref<nlohmann::json::object_t&>().emplace("y_max", nlohmann::json(*y_max));
    }

    return WriteVersionedJsonCacheDocument(
        path,
        kStateFormatKind,
        kStateSchemaVersion,
        "spectrum viewport state",
        body,
        error_message);
}

}  // namespace spectiary
