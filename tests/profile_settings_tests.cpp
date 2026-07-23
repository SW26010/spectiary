#include "profile/profile_settings.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
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
                ("specforge-profile-settings-tests-" + std::to_string(std::rand()));
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
    const specforge::ProfileSettings settings =
        specforge::LoadProfileSettings(temporary.path() / "missing.json");
    const specforge::ProfileOutputDirectoryResolution resolution =
        specforge::ResolveProfileOutputDirectory(settings, default_directory, std::nullopt);

    Require(
        resolution.directory == default_directory,
        "missing settings should retain the release-profile default directory");
    Require(
        resolution.source == specforge::ProfileOutputDirectorySource::Default,
        "missing settings should identify the default source");
}

void TestCustomDirectoryRoundTrips()
{
    TemporaryDirectory temporary;
    const std::filesystem::path settings_path = temporary.path() / "profile-settings.json";
    const std::filesystem::path custom_directory =
        temporary.path() / L"\u81EA\u5B9A\u4E49 profile";
    const specforge::ProfileSettings expected{.output_directory = custom_directory};
    std::string error;

    Require(
        specforge::SaveProfileSettings(settings_path, expected, &error),
        "custom profile output directory should save");
    Require(error.empty(), "successful settings save should not report an error");
    Require(
        specforge::LoadProfileSettings(settings_path) == expected,
        "custom profile output directory should round-trip as Unicode");
}

void TestResetToDefaultRoundTrips()
{
    TemporaryDirectory temporary;
    const std::filesystem::path settings_path = temporary.path() / "profile-settings.json";
    Require(
        specforge::SaveProfileSettings(
            settings_path,
            {.output_directory = temporary.path() / "custom"}),
        "custom settings fixture should save");
    Require(
        specforge::SaveProfileSettings(settings_path, {}),
        "reset profile settings should save explicitly");

    const specforge::ProfileSettings loaded =
        specforge::LoadProfileSettings(settings_path);
    Require(!loaded.output_directory, "reset settings should restore the default directory");
}

void TestEnvironmentOverrideWins()
{
    const specforge::ProfileSettings settings{
        .output_directory = "C:/SpecForge/user-profile-directory",
    };
    const std::filesystem::path environment_directory =
        "C:/SpecForge/environment-profile-directory";
    const specforge::ProfileOutputDirectoryResolution resolution =
        specforge::ResolveProfileOutputDirectory(
            settings,
            "C:/SpecForge/default-profile-directory",
            environment_directory);

    Require(
        resolution.directory == environment_directory,
        "environment override should outrank the saved user setting");
    Require(
        resolution.source == specforge::ProfileOutputDirectorySource::Environment,
        "environment override should be identified for the settings UI");
}

void TestUnicodeEnvironmentOverrideRoundTrips()
{
    const std::filesystem::path expected =
        L"C:\\\u6570\u636E\\\u65E5\u5FD7";
    ScopedWideEnvironmentVariable profile_directory(
        L"SPECFORGE_PROFILE_DIR",
        expected.c_str());

    const std::optional<std::filesystem::path> actual =
        specforge::ProfileOutputDirectoryEnvironmentOverride();
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
        stream << R"({"format_kind":"specforge.profile_settings","schema_version":1,"output_directory":42})";
    }

    const specforge::ProfileSettings loaded =
        specforge::LoadProfileSettings(settings_path);
    Require(!loaded.output_directory, "malformed output directory should fall back safely");
}

}  // namespace

int main()
{
    try {
        TestMissingSettingsUseDefaultDirectory();
        TestCustomDirectoryRoundTrips();
        TestResetToDefaultRoundTrips();
        TestEnvironmentOverrideWins();
        TestUnicodeEnvironmentOverrideRoundTrips();
        TestMalformedSettingsAreIgnored();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
