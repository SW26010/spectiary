#include "app/runtime_paths.h"
#include "platform/win32_process_launcher.h"
#include "platform/win32_text.h"

#include <Windows.h>
#include <shellapi.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

constexpr wchar_t kChildMarkerEnvironment[] =
    L"SPECFORGE_WIN32_PROCESS_LAUNCH_TEST_MARKER";

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

std::optional<std::filesystem::path> EnvironmentPath(
    const wchar_t* name)
{
    const DWORD required =
        GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0) {
        return std::nullopt;
    }
    std::wstring value(required, L'\0');
    const DWORD written = GetEnvironmentVariableW(
        name,
        value.data(),
        required);
    if (written == 0 || written >= required) {
        return std::nullopt;
    }
    value.resize(written);
    return value.empty()
        ? std::nullopt
        : std::optional<std::filesystem::path>{
              std::move(value)};
}

std::string PathText(const std::filesystem::path& path)
{
    return specforge::WideToUtf8(path.wstring());
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
            (L"specforge-win32-process-launch-tests-" +
             std::to_wstring(GetCurrentProcessId()) +
             L"-启动 contract-" +
             std::to_wstring(suffix));
        std::error_code error;
        std::filesystem::create_directories(path_, error);
        Require(!error, "launcher test directory should be created");
    }

    ~TemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

class ScopedEnvironmentVariable {
public:
    ScopedEnvironmentVariable(
        const wchar_t* name,
        const std::filesystem::path& value)
        : name_(name),
          previous_(EnvironmentPath(name))
    {
        Require(
            SetEnvironmentVariableW(name_, value.c_str()) != FALSE,
            "launcher test environment variable should be set");
    }

    ~ScopedEnvironmentVariable()
    {
        if (previous_) {
            (void)SetEnvironmentVariableW(
                name_,
                previous_->c_str());
        } else {
            (void)SetEnvironmentVariableW(name_, nullptr);
        }
    }

    ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
    ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) = delete;

private:
    const wchar_t* name_;
    std::optional<std::filesystem::path> previous_;
};

std::vector<std::wstring> ParseCommandLine(
    const std::wstring& command_line)
{
    int argument_count = 0;
    LPWSTR* raw_arguments = CommandLineToArgvW(
        command_line.c_str(),
        &argument_count);
    Require(
        raw_arguments != nullptr,
        "Windows command line should be parseable");

    std::vector<std::wstring> arguments;
    arguments.reserve(static_cast<std::size_t>(argument_count));
    for (int index = 0; index < argument_count; ++index) {
        arguments.emplace_back(raw_arguments[index]);
    }
    LocalFree(raw_arguments);
    return arguments;
}

void TestWindowsArgumentQuotingRoundTrips()
{
    Require(
        specforge::QuoteWindowsCommandLineArgument(L"plain") ==
            L"plain",
        "plain Windows arguments should not be quoted unnecessarily");
    Require(
        specforge::QuoteWindowsCommandLineArgument(L"file name.npy") ==
            L"\"file name.npy\"",
        "arguments containing spaces should be quoted");
    Require(
        specforge::QuoteWindowsCommandLineArgument(
            L"folder with space\\") ==
            L"\"folder with space\\\\\"",
        "trailing backslashes should be doubled before a closing quote");
    Require(
        specforge::QuoteWindowsCommandLineArgument(L"literal\"quote") ==
            L"\"literal\\\"quote\"",
        "embedded quotes should be escaped");

    const std::wstring executable =
        L"C:\\Program Files\\SpecForge\\SpecForge.exe";
    const std::vector<std::wstring> values = {
        L"",
        L"plain",
        L"C:\\data folder\\spectrum file.npy",
        L"C:\\数据\\光谱 文件.fits",
        L"C:\\folder with spaces\\",
        L"contains \"quote\" and \\\\ trailing\\",
    };
    for (const std::wstring& value : values) {
        const std::wstring command_line =
            specforge::QuoteWindowsCommandLineArgument(executable) +
            L" " +
            specforge::QuoteWindowsCommandLineArgument(value);
        const std::vector<std::wstring> parsed =
            ParseCommandLine(command_line);
        Require(
            parsed.size() == 2U && parsed[0] == executable &&
                parsed[1] == value,
            "Windows command line quoting should preserve the complete Unicode argument");
    }
}

std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

bool WaitForFileContents(
    const std::filesystem::path& path,
    std::string_view expected,
    std::chrono::milliseconds timeout)
{
    const auto deadline =
        std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        std::error_code error;
        if (std::filesystem::is_regular_file(path, error) &&
            !error &&
            ReadFile(path) == expected) {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

void WriteFixture(const std::filesystem::path& path)
{
    std::ofstream output(path, std::ios::binary);
    Require(output.good(), "launcher source fixture should be writable");
    output << "fixture\n";
}

void TestLaunchCurrentExecutableWithSource(
    const std::filesystem::path& source_path,
    const std::filesystem::path& marker_path)
{
    std::error_code remove_error;
    std::filesystem::remove(marker_path, remove_error);
    Require(!remove_error, "launcher marker should be reset");

    {
        ScopedEnvironmentVariable marker(
            kChildMarkerEnvironment,
            marker_path);
        const specforge::CurrentExecutableLaunchResult result =
            specforge::LaunchCurrentExecutableWithSource(source_path);
        Require(
            result.succeeded(),
            result.diagnostic.empty()
                ? "current executable should be launched"
                : result.diagnostic);
        Require(
            result.executable_path ==
                specforge::CurrentExecutablePath(),
            "launch result should report the current executable path");
    }

    const std::string expected = PathText(source_path);
    Require(
        WaitForFileContents(marker_path, expected, 5s),
        "launched child should receive the exact Unicode source path");
}

void TestFileAndDirectorySources()
{
    TemporaryDirectory temporary;
    const std::filesystem::path file_path =
        temporary.path() / L"source file 文件.npy";
    WriteFixture(file_path);
    const std::filesystem::path directory_path =
        temporary.path() / L"source folder 目录";
    std::error_code directory_error;
    std::filesystem::create_directories(directory_path, directory_error);
    Require(!directory_error, "directory source fixture should be created");

    TestLaunchCurrentExecutableWithSource(
        file_path,
        temporary.path() / L"observed file path 文件.txt");
    TestLaunchCurrentExecutableWithSource(
        directory_path,
        temporary.path() / L"observed directory path 目录.txt");
}

void TestInvalidSourceIsStructured()
{
    const specforge::CurrentExecutableLaunchResult result =
        specforge::LaunchCurrentExecutableWithSource({});
    Require(
        result.failure ==
            specforge::CurrentExecutableLaunchFailure::InvalidSourcePath,
        "an empty source should be rejected before process creation");
    Require(
        !result.succeeded() && !result.diagnostic.empty(),
        "invalid source failure should contain a diagnostic");
}

void TestProcessCreationFailureIsStructured()
{
    TemporaryDirectory temporary;
    const std::filesystem::path executable_path =
        temporary.path() / L"missing executable 不存在.exe";
    const std::filesystem::path source_path =
        temporary.path() / L"source file 文件.npy";
    const specforge::CurrentExecutableLaunchResult result =
        specforge::LaunchExecutableWithSource(
            executable_path,
            source_path);
    Require(
        result.failure ==
            specforge::CurrentExecutableLaunchFailure::
                ProcessCreationFailed,
        "CreateProcess failure should be typed");
    Require(
        result.win32_error != 0 &&
            result.executable_path == executable_path,
        "CreateProcess failure should preserve the Win32 error and executable path");
    Require(
        result.diagnostic.find(PathText(executable_path)) !=
            std::string::npos,
        "CreateProcess failure should identify the executable path");
}

int RunChildIfRequested(int argc, wchar_t** argv)
{
    const std::optional<std::filesystem::path> marker =
        EnvironmentPath(kChildMarkerEnvironment);
    if (!marker) {
        return -1;
    }
    if (argc != 2 || argv[1] == nullptr) {
        return 2;
    }

    std::ofstream output(*marker, std::ios::binary | std::ios::trunc);
    if (!output) {
        return 3;
    }
    const std::string source_text =
        specforge::WideToUtf8(argv[1]);
    if (source_text.empty()) {
        return 4;
    }
    output << source_text;
    return output.good() ? 0 : 5;
}

}  // namespace

int wmain(int argc, wchar_t** argv)
{
    const int child_result = RunChildIfRequested(argc, argv);
    if (child_result >= 0) {
        return child_result;
    }

    TestWindowsArgumentQuotingRoundTrips();
    TestFileAndDirectorySources();
    TestInvalidSourceIsStructured();
    TestProcessCreationFailureIsStructured();
    return 0;
}
