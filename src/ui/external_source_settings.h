#pragma once

#include <filesystem>
#include <string>

namespace specforge {

inline constexpr bool kDefaultOpenExternalFitsAsFolder = false;

struct ExternalSourceSettings {
    bool open_external_fits_as_folder =
        kDefaultOpenExternalFitsAsFolder;
};

struct ExternalSourceSettingsLoadResult {
    ExternalSourceSettings settings;
    std::string warning;
};

[[nodiscard]] std::filesystem::path DefaultExternalSourceSettingsPath();
[[nodiscard]] ExternalSourceSettingsLoadResult
LoadExternalSourceSettings(const std::filesystem::path& path);
[[nodiscard]] bool SaveExternalSourceSettings(
    const std::filesystem::path& path,
    const ExternalSourceSettings& settings,
    std::string* error_message = nullptr);

}  // namespace specforge
