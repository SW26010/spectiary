#include "ui/ui_language_settings.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

class TemporaryDirectory {
public:
    TemporaryDirectory()
    {
        const auto suffix = std::chrono::steady_clock::now()
                                .time_since_epoch()
                                .count();
        path_ = std::filesystem::temp_directory_path() /
                ("specforge-ui-language-settings-tests-" +
                 std::to_string(suffix));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream stream(path);
    return std::string(
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>());
}

void WriteFile(
    const std::filesystem::path& path,
    std::string_view contents)
{
    std::ofstream stream(path);
    stream << contents;
    Require(stream.good(), "language settings fixture should write");
}

void TestDefaultPathUsesDedicatedFile()
{
    const std::filesystem::path path =
        specforge::DefaultUiLanguageSettingsPath();
    Require(
        path.filename() == "ui-language.json",
        "UI language should use its own settings file");
}

void TestMissingFileDefaultsToEnglishWithoutWarning()
{
    TemporaryDirectory temporary;
    const specforge::UiLanguageSettingsLoadResult loaded =
        specforge::LoadUiLanguageSettings(
            temporary.path() / "missing.json");

    Require(
        loaded.language == specforge::UiLanguage::English,
        "missing language settings should default to English");
    Require(
        loaded.warning.empty(),
        "a missing optional language settings file should not warn");
}

void TestSupportedLanguagesRoundTripWithStableValues()
{
    TemporaryDirectory temporary;
    const std::filesystem::path path =
        temporary.path() / "ui-language.json";

    std::string error;
    Require(
        specforge::SaveUiLanguageSettings(
            path,
            specforge::UiLanguage::English,
            &error),
        "English language setting should save");
    Require(error.empty(), "successful English save should not report an error");
    Require(
        ReadFile(path).find(R"("language": "en")") !=
            std::string::npos,
        "English should persist as en");
    specforge::UiLanguageSettingsLoadResult loaded =
        specforge::LoadUiLanguageSettings(path);
    Require(
        loaded.language == specforge::UiLanguage::English &&
            loaded.warning.empty(),
        "English should reload without warning");

    Require(
        specforge::SaveUiLanguageSettings(
            path,
            specforge::UiLanguage::SimplifiedChinese,
            &error),
        "Simplified Chinese language setting should save");
    Require(
        ReadFile(path).find(R"("language": "zh-Hans")") !=
            std::string::npos,
        "Simplified Chinese should persist as zh-Hans");
    loaded = specforge::LoadUiLanguageSettings(path);
    Require(
        loaded.language ==
                specforge::UiLanguage::SimplifiedChinese &&
            loaded.warning.empty(),
        "Simplified Chinese should reload without warning");
}

void TestUnknownLanguageFallsBackWithWarning()
{
    TemporaryDirectory temporary;
    const std::filesystem::path path =
        temporary.path() / "ui-language.json";
    WriteFile(
        path,
        R"({"format_kind":"specforge.ui_language.settings","schema_version":1,"language":"fr"})");

    const specforge::UiLanguageSettingsLoadResult loaded =
        specforge::LoadUiLanguageSettings(path);
    Require(
        loaded.language == specforge::UiLanguage::English,
        "unknown language should fall back to English");
    Require(
        !loaded.warning.empty(),
        "unknown language should produce a warning");
}

void TestDamagedJsonFallsBackWithWarning()
{
    TemporaryDirectory temporary;
    const std::filesystem::path path =
        temporary.path() / "ui-language.json";
    WriteFile(path, R"({"format_kind":)");

    const specforge::UiLanguageSettingsLoadResult loaded =
        specforge::LoadUiLanguageSettings(path);
    Require(
        loaded.language == specforge::UiLanguage::English,
        "damaged JSON should fall back to English");
    Require(
        !loaded.warning.empty(),
        "damaged JSON should produce a warning");
}

void TestUnsupportedSchemaFallsBackWithWarning()
{
    TemporaryDirectory temporary;
    const std::filesystem::path path =
        temporary.path() / "ui-language.json";
    WriteFile(
        path,
        R"({"format_kind":"specforge.ui_language.settings","schema_version":2,"language":"zh-Hans"})");

    const specforge::UiLanguageSettingsLoadResult loaded =
        specforge::LoadUiLanguageSettings(path);
    Require(
        loaded.language == specforge::UiLanguage::English,
        "unsupported schema should fall back to English");
    Require(
        !loaded.warning.empty(),
        "unsupported schema should produce a warning");
}

void TestInvalidLanguageCannotBeSaved()
{
    TemporaryDirectory temporary;
    std::string error;
    Require(
        !specforge::SaveUiLanguageSettings(
            temporary.path() / "ui-language.json",
            specforge::UiLanguage::Count,
            &error),
        "the language sentinel should not be persisted");
    Require(
        !error.empty(),
        "rejecting an invalid language should report an error");
}

}  // namespace

int main()
{
    try {
        TestDefaultPathUsesDedicatedFile();
        TestMissingFileDefaultsToEnglishWithoutWarning();
        TestSupportedLanguagesRoundTripWithStableValues();
        TestUnknownLanguageFallsBackWithWarning();
        TestDamagedJsonFallsBackWithWarning();
        TestUnsupportedSchemaFallsBackWithWarning();
        TestInvalidLanguageCannotBeSaved();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
