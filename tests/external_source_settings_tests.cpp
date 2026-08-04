#include "ui/external_source_settings.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

namespace {

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

class TemporaryDirectory {
public:
    TemporaryDirectory()
    {
        const auto suffix =
            std::chrono::steady_clock::now()
                .time_since_epoch()
                .count();
        path_ = std::filesystem::temp_directory_path() /
            ("specforge-external-source-settings-tests-" +
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

void WriteFile(
    const std::filesystem::path& path,
    std::string_view contents)
{
    std::ofstream stream(path);
    stream << contents;
    Require(stream.good(), "external source settings fixture should write");
}

std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream stream(path);
    return std::string(
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>());
}

void TestMissingSettingsDefaultToDisabled()
{
    Require(
        specforge::DefaultExternalSourceSettingsPath().filename() ==
            "external-source-settings.json",
        "external source settings should use a dedicated settings file");

    TemporaryDirectory temporary;
    const specforge::ExternalSourceSettingsLoadResult loaded =
        specforge::LoadExternalSourceSettings(
            temporary.path() / "missing.json");
    Require(
        !loaded.settings.open_external_fits_as_folder &&
            loaded.warning.empty(),
        "missing external source settings should default to disabled");
}

void TestExternalFitsFolderPreferenceRoundTrips()
{
    TemporaryDirectory temporary;
    const std::filesystem::path path =
        temporary.path() / "external-source-settings.json";

    for (const bool enabled : {false, true}) {
        std::string error;
        Require(
            specforge::SaveExternalSourceSettings(
                path,
                {.open_external_fits_as_folder = enabled},
                &error),
            "external source settings should save");
        Require(
            error.empty(),
            "successful external source settings save should not report an error");
        const std::string document = ReadFile(path);
        Require(
            document.find(
                std::string{"\"open_external_fits_as_folder\": "} +
                (enabled ? "true" : "false")) !=
                std::string::npos,
            "external FITS folder preference should persist as a boolean");
        Require(
            document.find("include_subfolders") ==
                std::string::npos,
            "the deferred subfolder option must not be persisted");

        const specforge::ExternalSourceSettingsLoadResult loaded =
            specforge::LoadExternalSourceSettings(path);
        Require(
            loaded.settings.open_external_fits_as_folder == enabled &&
                loaded.warning.empty(),
            "external FITS folder preference should round-trip without warning");
    }
}

void TestInvalidSettingsWarnAndUseDisabledDefault()
{
    TemporaryDirectory temporary;
    const std::filesystem::path path =
        temporary.path() / "external-source-settings.json";

    WriteFile(path, R"({"format_kind":)");
    specforge::ExternalSourceSettingsLoadResult loaded =
        specforge::LoadExternalSourceSettings(path);
    Require(
        !loaded.settings.open_external_fits_as_folder &&
            !loaded.warning.empty(),
        "damaged external source settings should warn and disable the preference");

    WriteFile(
        path,
        R"({"format_kind":"specforge.external_source.settings","schema_version":1,"open_external_fits_as_folder":"true"})");
    loaded = specforge::LoadExternalSourceSettings(path);
    Require(
        !loaded.settings.open_external_fits_as_folder &&
            !loaded.warning.empty(),
        "a non-boolean external source preference should warn and use the disabled default");
}

}  // namespace

int main()
{
    TestMissingSettingsDefaultToDisabled();
    TestExternalFitsFolderPreferenceRoundTrips();
    TestInvalidSettingsWarnAndUseDisabledDefault();
    return 0;
}
