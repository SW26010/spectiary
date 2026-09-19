#pragma once

#include "overlays/spectral_line_list.h"
#include <nlohmann/json_fwd.hpp>
#include <filesystem>
#include <ostream>
#include <string_view>

namespace spectiary {
inline constexpr int kSpectralLineListSchemaVersion = 1;
inline constexpr std::string_view kSpectralLineListFormat = "spectiary.spectral_line_list";

struct SpectralLineListParseResult {
    std::optional<SpectralLineList> list;
    std::string error;
};

[[nodiscard]] SpectralLineListParseResult ParseSpectralLineListJson(std::string_view bytes);
[[nodiscard]] SpectralLineListParseResult LoadSpectralLineListFromPath(const std::filesystem::path& path);
[[nodiscard]] SpectralLineListParseResult LoadPackagedPublicSpectralLineList(const std::filesystem::path& path);
[[nodiscard]] bool WriteSpectralLineListJson(const SpectralLineList& list, std::ostream& stream, std::string& error);
[[nodiscard]] bool SaveSpectralLineListToPathAtomic(const std::filesystem::path& path, const SpectralLineList& list, std::string& error);

// Shared closed subrecord codecs for canonical documents and internal overlays.
// Decode throws on wire shape errors; callers catch at their document boundary.
[[nodiscard]] line_list::GroupingView DecodeLineListGroupingView(const nlohmann::json& value);
[[nodiscard]] line_list::ColorScheme DecodeLineListColorScheme(const nlohmann::json& value);
[[nodiscard]] nlohmann::json EncodeLineListGroupingView(const line_list::GroupingView& value);
[[nodiscard]] nlohmann::json EncodeLineListColorScheme(const line_list::ColorScheme& value);
} // namespace spectiary
