#pragma once

#include "ui/ui_text.h"

#include <filesystem>
#include <string>

namespace specforge {

struct UiLanguageSettingsLoadResult {
    UiLanguage language = UiLanguage::English;
    std::string warning;
};

[[nodiscard]] std::filesystem::path DefaultUiLanguageSettingsPath();
[[nodiscard]] UiLanguageSettingsLoadResult LoadUiLanguageSettings(
    const std::filesystem::path& path);
[[nodiscard]] bool SaveUiLanguageSettings(
    const std::filesystem::path& path,
    UiLanguage language,
    std::string* error_message = nullptr);

}  // namespace specforge
