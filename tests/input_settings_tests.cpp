#include "ui/input_settings.h"

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
            ("specforge-input-settings-tests-" +
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
    Require(stream.good(), "input settings fixture should write");
}

std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream stream(path);
    return std::string(
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>());
}

void TestMissingSettingsDefaultToLiveNavigation()
{
    Require(
        specforge::DefaultInputSettingsPath().filename() ==
            "input-settings.json",
        "input behavior should use a dedicated settings file");

    TemporaryDirectory temporary;
    const specforge::InputSettingsLoadResult loaded =
        specforge::LoadInputSettings(
            temporary.path() / "missing.json");
    Require(
        loaded.settings.live_numeric_navigation &&
            loaded.warning.empty(),
        "missing input settings should silently enable live numeric navigation");
}

void TestLiveNavigationRoundTrips()
{
    TemporaryDirectory temporary;
    const std::filesystem::path path =
        temporary.path() / "input-settings.json";

    for (const bool enabled : {false, true}) {
        std::string error;
        Require(
            specforge::SaveInputSettings(
                path,
                {.live_numeric_navigation = enabled},
                &error),
            "input behavior should save");
        Require(
            error.empty(),
            "successful input settings save should not report an error");
        Require(
            ReadFile(path).find(
                std::string{"\"live_numeric_navigation\": "} +
                (enabled ? "true" : "false")) !=
                std::string::npos,
            "live numeric navigation should persist as a boolean");
        const specforge::InputSettingsLoadResult loaded =
            specforge::LoadInputSettings(path);
        Require(
            loaded.settings.live_numeric_navigation == enabled &&
                loaded.warning.empty(),
            "input behavior should round-trip without warning");
    }
}

void TestInvalidSettingsWarnAndUseTheEnabledDefault()
{
    TemporaryDirectory temporary;
    const std::filesystem::path path =
        temporary.path() / "input-settings.json";

    WriteFile(path, R"({"format_kind":)");
    specforge::InputSettingsLoadResult loaded =
        specforge::LoadInputSettings(path);
    Require(
        loaded.settings.live_numeric_navigation &&
            !loaded.warning.empty(),
        "damaged input settings should warn and enable live navigation");

    WriteFile(
        path,
        R"({"format_kind":"specforge.input.settings","schema_version":1,"live_numeric_navigation":"false"})");
    loaded = specforge::LoadInputSettings(path);
    Require(
        loaded.settings.live_numeric_navigation &&
            !loaded.warning.empty(),
        "a non-boolean live navigation value should warn and use the enabled default");
}

}  // namespace

int main()
{
    TestMissingSettingsDefaultToLiveNavigation();
    TestLiveNavigationRoundTrips();
    TestInvalidSettingsWarnAndUseTheEnabledDefault();
    return 0;
}
