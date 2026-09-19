#include "profile/profile_settings.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
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
        path_ = std::filesystem::temp_directory_path() /
                ("spectiary-profile-settings-tests-" + std::to_string(std::rand()));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

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
    Require(
        stream.good(),
        "profile settings fixture should write");
}

class ScopedWideEnvironmentVariable {
public:
    ScopedWideEnvironmentVariable(const wchar_t* name, const wchar_t* value)
        : name_(name)
    {
        wchar_t* raw_previous = nullptr;
        std::size_t previous_size = 0;
        if (_wdupenv_s(
                &raw_previous,
                &previous_size,
                name_.c_str()) == 0 &&
            raw_previous != nullptr) {
            std::unique_ptr<wchar_t, decltype(&std::free)> previous(
                raw_previous,
                std::free);
            previous_ = std::wstring(previous.get());
        }
        Require(
            _wputenv_s(name_.c_str(), value) == 0,
            "wide environment variable should be configurable");
    }

    ~ScopedWideEnvironmentVariable()
    {
        (void)_wputenv_s(
            name_.c_str(),
            previous_ ? previous_->c_str() : L"");
    }

    ScopedWideEnvironmentVariable(
        const ScopedWideEnvironmentVariable&) = delete;
    ScopedWideEnvironmentVariable& operator=(
        const ScopedWideEnvironmentVariable&) = delete;

private:
    std::wstring name_;
    std::optional<std::wstring> previous_;
};

void TestMissingSettingsUseDefaultDirectory()
{
    TemporaryDirectory temporary;
    const std::filesystem::path default_directory = temporary.path() / "default-logs";
    const spectiary::ProfileSettingsLoadResult loaded =
        spectiary::LoadProfileSettings(spectiary::RuntimePaths{},
            temporary.path() / "missing.json");
    const spectiary::ProfileSettings& settings =
        loaded.settings;
    Require(
        loaded.warning.empty(),
        "missing profile settings should be a healthy default");
    const spectiary::ProfileOutputDirectoryResolution resolution =
        spectiary::ResolveProfileOutputDirectory(settings, default_directory, std::nullopt);

    Require(
        resolution.directory == default_directory,
        "missing settings should retain the storage-profile default directory");
    Require(
        resolution.source == spectiary::ProfileOutputDirectorySource::Default,
        "missing settings should identify the default source");
}

void TestCustomDirectoryRoundTrips()
{
    TemporaryDirectory temporary;
    const std::filesystem::path settings_path = temporary.path() / "profile-settings.json";
    const std::filesystem::path custom_directory =
        temporary.path() / L"\u81EA\u5B9A\u4E49 profile";
    const spectiary::ProfileSettings expected{.output_directory = custom_directory};
    std::string error;

    Require(
        spectiary::SaveProfileSettings(spectiary::RuntimePaths{}, settings_path, expected, &error),
        "custom profile output directory should save");
    Require(error.empty(), "successful settings save should not report an error");
    const std::string stable_output =
        ReadFile(settings_path);
    Require(
        spectiary::SaveProfileSettings(spectiary::RuntimePaths{},
            settings_path,
            expected,
            &error) &&
            ReadFile(settings_path) == stable_output,
        "profile settings output should be byte-stable");
    Require(
        spectiary::LoadProfileSettings(spectiary::RuntimePaths{}, settings_path).settings ==
            expected,
        "custom profile output directory should round-trip as Unicode");
}

void TestLegacyCompactSettingsRemainReadable()
{
    TemporaryDirectory temporary;
    const std::filesystem::path settings_path =
        temporary.path() / "profile-settings.json";
    WriteFile(
        settings_path,
        R"({"format_kind":"spectiary.profile_settings","schema_version":1,"output_directory":{"path_kind":"absolute","path":"C:/legacy/profiles"}})");
    Require(
        spectiary::LoadProfileSettings(spectiary::RuntimePaths{}, settings_path)
                .settings.output_directory ==
            std::optional<std::filesystem::path>{
                "C:/legacy/profiles"},
        "legacy compact profile settings should remain readable");
}

void TestResetToDefaultRoundTrips()
{
    TemporaryDirectory temporary;
    const std::filesystem::path settings_path = temporary.path() / "profile-settings.json";
    Require(
        spectiary::SaveProfileSettings(spectiary::RuntimePaths{},
            settings_path,
            {.output_directory = temporary.path() / "custom"}),
        "custom settings fixture should save");
    Require(
        spectiary::SaveProfileSettings(spectiary::RuntimePaths{}, settings_path, {}),
        "reset profile settings should save explicitly");

    const spectiary::ProfileSettings loaded =
        spectiary::LoadProfileSettings(spectiary::RuntimePaths{}, settings_path).settings;
    Require(!loaded.output_directory, "reset settings should restore the default directory");
}

void TestEnvironmentOverrideWins()
{
    const spectiary::ProfileSettings settings{
        .output_directory = "C:/Spectiary/user-profile-directory",
    };
    const std::filesystem::path environment_directory =
        "C:/Spectiary/environment-profile-directory";
    const spectiary::ProfileOutputDirectoryResolution resolution =
        spectiary::ResolveProfileOutputDirectory(
            settings,
            "C:/Spectiary/default-profile-directory",
            environment_directory);

    Require(
        resolution.directory == environment_directory,
        "environment override should outrank the saved user setting");
    Require(
        resolution.source == spectiary::ProfileOutputDirectorySource::Environment,
        "environment override should be identified for the settings UI");
}

void TestUnicodeEnvironmentOverrideRoundTrips()
{
    const std::filesystem::path expected =
        L"C:\\\u6570\u636E\\\u65E5\u5FD7";
    ScopedWideEnvironmentVariable profile_directory(
        L"SPECTIARY_PROFILE_DIR",
        expected.c_str());

    const std::optional<std::filesystem::path> actual =
        spectiary::ProfileOutputDirectoryEnvironmentOverride();
    Require(
        actual && *actual == expected,
        "Unicode environment override should round-trip without code-page conversion");
}

void TestMalformedSettingsAreIgnored()
{
    TemporaryDirectory temporary;
    const std::filesystem::path settings_path = temporary.path() / "profile-settings.json";
    {
        std::ofstream stream(settings_path);
        stream << R"({"format_kind":"spectiary.profile_settings","schema_version":1,"output_directory":42})";
    }

    const spectiary::ProfileSettingsLoadResult loaded =
        spectiary::LoadProfileSettings(spectiary::RuntimePaths{}, settings_path);
    Require(
        !loaded.settings.output_directory,
        "malformed output directory should fall back safely");
    Require(
        !loaded.warning.empty(),
        "present but invalid profile settings should report a load warning");
}

void TestCorruptSettingsAreIgnored()
{
    TemporaryDirectory temporary;
    const std::filesystem::path settings_path =
        temporary.path() / "profile-settings.json";
    WriteFile(settings_path, "{ invalid json");
    const spectiary::ProfileSettingsLoadResult loaded =
        spectiary::LoadProfileSettings(spectiary::RuntimePaths{}, settings_path);
    Require(
        !loaded.settings.output_directory,
        "corrupt profile settings should fall back safely");
    Require(
        !loaded.warning.empty(),
        "corrupt profile settings should report a warning");
}

}  // namespace

int main()
{
    try {
        TestMissingSettingsUseDefaultDirectory();
        TestCustomDirectoryRoundTrips();
        TestLegacyCompactSettingsRemainReadable();
        TestResetToDefaultRoundTrips();
        TestEnvironmentOverrideWins();
        TestUnicodeEnvironmentOverrideRoundTrips();
        TestMalformedSettingsAreIgnored();
        TestCorruptSettingsAreIgnored();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
