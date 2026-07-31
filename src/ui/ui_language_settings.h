#pragma once

#include "ui/ui_text.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace specforge {

struct UiLanguageSettingsLoadResult {
    UiLanguage language = UiLanguage::English;
    std::string warning;
};

[[nodiscard]] std::filesystem::path DefaultUiLanguageSettingsPath();
[[nodiscard]] std::string_view UiLanguageSettingValue(
    UiLanguage language) noexcept;
[[nodiscard]] std::optional<UiLanguage> ParseUiLanguageSettingValue(
    std::string_view value) noexcept;
[[nodiscard]] UiLanguageSettingsLoadResult LoadUiLanguageSettings(
    const std::filesystem::path& path);
[[nodiscard]] bool SaveUiLanguageSettings(
    const std::filesystem::path& path,
    UiLanguage language,
    std::string* error_message = nullptr);

}  // namespace specforge
