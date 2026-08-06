#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace specforge {

struct CurrentExecutablePathResult {
    std::filesystem::path path;
    std::uint32_t win32_error = 0;

    [[nodiscard]] bool resolved() const noexcept
    {
        return !path.empty();
    }
};

enum class CurrentExecutableLaunchFailure {
    None,
    InvalidSourcePath,
    ExecutablePathUnavailable,
    ProcessCreationFailed,
};

struct CurrentExecutableLaunchResult {
    CurrentExecutableLaunchFailure failure =
        CurrentExecutableLaunchFailure::None;
    std::uint32_t win32_error = 0;
    std::filesystem::path executable_path;
    std::string diagnostic;

    [[nodiscard]] bool succeeded() const noexcept
    {
        return failure == CurrentExecutableLaunchFailure::None;
    }
};

[[nodiscard]] CurrentExecutablePathResult
ResolveCurrentExecutablePath();

[[nodiscard]] std::wstring QuoteWindowsCommandLineArgument(
    std::wstring_view value);

[[nodiscard]] CurrentExecutableLaunchResult
LaunchExecutableWithSource(
    const std::filesystem::path& executable_path,
    const std::filesystem::path& source_path);

[[nodiscard]] CurrentExecutableLaunchResult
LaunchCurrentExecutableWithSource(
    const std::filesystem::path& source_path);

}  // namespace specforge
