#include "ui/spectrum_view_state_cache_io.h"

#include "app/local_user_state.h"
#include "app/local_user_state_json.h"
#include "app/local_user_state_paths.h"

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

constexpr const char* kStateFormatKind =
    "specforge.spectrum_view.state";
constexpr int kStateSchemaVersion = 1;

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
    const JsonValue& root,
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

std::filesystem::path DefaultSpectrumViewStateCachePath()
{
    return DefaultLocalUserStatePath(
        local_user_state_paths::kSpectrumViewState);
}

SpectrumViewStateCacheLoadResult LoadSpectrumViewStateCache(
    const std::filesystem::path& path)
{
    SpectrumViewStateCacheLoadResult loaded;
    VersionedJsonCacheLoadResult result =
        LoadVersionedJsonCacheFile(
            path,
            kStateFormatKind,
            {kStateSchemaVersion},
            "spectrum view state cache");
    loaded.warning = std::move(result.warning);
    loaded.diagnostic_detail =
        std::move(result.diagnostic_detail);
    if (!result.document) {
        return loaded;
    }

    const JsonValue& root = result.document->root;
    const JsonValue* locked =
        JsonObjectMember(root, "locked");
    if (locked == nullptr ||
        locked->kind != JsonValue::Kind::Bool) {
        loaded.warning =
            "Spectrum view state cache member 'locked' must be boolean; automatic range was used.";
        return loaded;
    }
    if (!locked->bool_value) {
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
        loaded.warning =
            "Locked spectrum view state is incomplete; automatic range was used.";
        return loaded;
    }

    SpectrumViewStateCache restored{
        .locked = true,
        .source_collection_identity = *identity,
        .limits = {
            .x_min = *x_min,
            .x_max = *x_max,
            .y_min = *y_min,
            .y_max = *y_max,
        },
    };
    if (!LimitsAreUsable(restored.limits)) {
        loaded.warning =
            "Locked spectrum view state has invalid axis ranges; automatic range was used.";
        return loaded;
    }
    loaded.state = std::move(restored);
    return loaded;
}

bool SaveSpectrumViewStateCache(
    const std::filesystem::path& path,
    const SpectrumViewStateCache& state,
    std::string* error_message)
{
    if (path.empty()) {
        if (error_message != nullptr) {
            *error_message =
                "Spectrum view state cache path is empty.";
        }
        return false;
    }

    if (!state.locked) {
        return WriteVersionedJsonCacheDocument(
            path,
            kStateFormatKind,
            kStateSchemaVersion,
            "spectrum view state cache",
            JsonObjectValue({
                {"locked", JsonBoolValue(false)},
            }),
            error_message);
    }
    if (state.source_collection_identity.empty() ||
        !LimitsAreUsable(state.limits)) {
        if (error_message != nullptr) {
            *error_message =
                "Locked spectrum view state is incomplete or invalid.";
        }
        return false;
    }

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
                "Spectrum view axis ranges could not be encoded.";
        }
        return false;
    }

    return WriteVersionedJsonCacheDocument(
        path,
        kStateFormatKind,
        kStateSchemaVersion,
        "spectrum view state cache",
        JsonObjectValue({
            {"locked", JsonBoolValue(true)},
            {"source_collection_identity",
             JsonStringValue(
                 state.source_collection_identity)},
            {"x_min", JsonStringValue(*x_min)},
            {"x_max", JsonStringValue(*x_max)},
            {"y_min", JsonStringValue(*y_min)},
            {"y_max", JsonStringValue(*y_max)},
        }),
        error_message);
}

}  // namespace specforge
