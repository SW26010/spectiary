#include "app/initial_source.h"
#include "app/runtime_paths.h"
#include "automation/automation_startup.h"
#include "platform/win32_process_launcher.h"
#include "platform/win32_text.h"
#include "ui/shell_ui.h"

#include <Windows.h>
#include <shellapi.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace spectiary {

struct ShellUiTestAccess {
    static void Drain(ShellUi& shell)
    {
        shell.DrainSourceLoads();
    }
};

}  // namespace spectiary

namespace {

using namespace std::chrono_literals;

constexpr wchar_t kChildMarkerEnvironment[] =
    L"SPECTIARY_WIN32_PROCESS_LAUNCH_TEST_MARKER";

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
    return spectiary::WideToUtf8(path.wstring());
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
            (L"spectiary-win32-process-launch-tests-" +
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

class ScopedCurrentDirectory {
public:
    explicit ScopedCurrentDirectory(
        const std::filesystem::path& path)
    {
        std::error_code error;
        previous_ = std::filesystem::current_path(error);
        Require(!error, "launcher test current directory should be readable");
        std::filesystem::current_path(path, error);
        Require(!error, "launcher test current directory should be set");
    }

    ~ScopedCurrentDirectory()
    {
        std::error_code ignored;
        std::filesystem::current_path(previous_, ignored);
    }

    ScopedCurrentDirectory(const ScopedCurrentDirectory&) = delete;
    ScopedCurrentDirectory& operator=(
        const ScopedCurrentDirectory&) = delete;

private:
    std::filesystem::path previous_;
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
        spectiary::QuoteWindowsCommandLineArgument(L"plain") ==
            L"plain",
        "plain Windows arguments should not be quoted unnecessarily");
    Require(
        spectiary::QuoteWindowsCommandLineArgument(L"file name.npy") ==
            L"\"file name.npy\"",
        "arguments containing spaces should be quoted");
    Require(
        spectiary::QuoteWindowsCommandLineArgument(
            L"folder with space\\") ==
            L"\"folder with space\\\\\"",
        "trailing backslashes should be doubled before a closing quote");
    Require(
        spectiary::QuoteWindowsCommandLineArgument(L"literal\"quote") ==
            L"\"literal\\\"quote\"",
        "embedded quotes should be escaped");

    const std::wstring executable =
        L"C:\\Program Files\\Spectiary\\Spectiary.exe";
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
            spectiary::QuoteWindowsCommandLineArgument(executable) +
            L" " +
            spectiary::QuoteWindowsCommandLineArgument(value);
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
    output << "wav,loglam,flux\n"
           << "5001,3.1,2\n"
           << "5000,3.0,1\n";
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
        const spectiary::CurrentExecutableLaunchResult result =
            spectiary::LaunchCurrentExecutableWithSource(source_path);
        Require(
            result.succeeded(),
            result.diagnostic.empty()
                ? "current executable should be launched"
                : result.diagnostic);
        Require(
            result.executable_path ==
                spectiary::CurrentExecutablePath(),
            "launch result should report the current executable path");
    }

    std::error_code absolute_error;
    const std::filesystem::path expected_source_path =
        source_path.is_absolute()
        ? source_path
        : std::filesystem::absolute(
              source_path,
              absolute_error);
    Require(
        !absolute_error,
        "launcher test source path should resolve absolutely");
    const std::string expected = PathText(expected_source_path);
    Require(
        WaitForFileContents(marker_path, expected, 5s),
        "the production startup loader should activate the exact Unicode source path");
}

void TestFileAndDirectorySources()
{
    TemporaryDirectory temporary;
    const std::filesystem::path file_path =
        temporary.path() / L"source file 文件.csv";
    WriteFixture(file_path);
    const std::filesystem::path directory_path =
        temporary.path() / L"source folder 目录";
    std::error_code directory_error;
    std::filesystem::create_directories(directory_path, directory_error);
    Require(!directory_error, "directory source fixture should be created");
    WriteFixture(directory_path / L"spectrum file.csv");

    TestLaunchCurrentExecutableWithSource(
        file_path,
        temporary.path() / L"observed file path 文件.txt");
    TestLaunchCurrentExecutableWithSource(
        directory_path,
        temporary.path() / L"observed directory path 目录.txt");

    const std::filesystem::path leading_dash_file =
        temporary.path() / L"-source file 文件.csv";
    WriteFixture(leading_dash_file);
    {
        ScopedCurrentDirectory current_directory(
            temporary.path());
        TestLaunchCurrentExecutableWithSource(
            leading_dash_file.filename(),
            temporary.path() /
                L"observed leading dash path 文件.txt");
    }
}

void TestInvalidSourceIsStructured()
{
    const spectiary::CurrentExecutableLaunchResult result =
        spectiary::LaunchCurrentExecutableWithSource({});
    Require(
        result.failure ==
            spectiary::CurrentExecutableLaunchFailure::InvalidSourcePath,
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
        temporary.path() / L"source file 文件.csv";
    const spectiary::CurrentExecutableLaunchResult result =
        spectiary::LaunchExecutableWithSource(
            executable_path,
            source_path);
    Require(
        result.failure ==
            spectiary::CurrentExecutableLaunchFailure::
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
    if (argc != 3 || argv[2] == nullptr) {
        return 2;
    }

    const spectiary::SpectiaryCommandLine command_line =
        spectiary::ParseCurrentProcessSpectiaryCommandLine();
    if (!command_line.error_message.empty() ||
        !command_line.force_new_instance ||
        !command_line.initial_source ||
        command_line.initial_source->wstring() != argv[2]) {
        return 3;
    }

    try {
        const std::filesystem::path production_executable =
            spectiary::CurrentExecutablePath().parent_path() /
            L"Spectiary.exe";
        if (!std::filesystem::is_regular_file(
                production_executable)) {
            return 4;
        }

        spectiary::RuntimePathInputs inputs =
            spectiary::CurrentProcessRuntimePathInputs(
                production_executable);
        inputs.application_data_root_override =
            marker->parent_path() / L"startup loader state";
        const spectiary::SpectiaryStartup startup =
            spectiary::PrepareSpectiaryStartup(std::move(inputs));
        spectiary::ShellUi shell(startup);
        spectiary::OpenInitialSource(
            shell,
            command_line.initial_source);

        const auto deadline =
            std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline) {
            spectiary::ShellUiTestAccess::Drain(shell);
            const spectiary::ShellRuntimeResourceObservation observation =
                shell.runtime_resource_observation();
            if (observation.idle()) {
                const spectiary::SpectrumSnapshotHandle snapshot =
                    shell.current_snapshot();
                if (!snapshot ||
                    snapshot->source.path !=
                        *command_line.initial_source) {
                    return 5;
                }

                std::ofstream output(
                    *marker,
                    std::ios::binary | std::ios::trunc);
                if (!output) {
                    return 6;
                }
                output << PathText(snapshot->source.path);
                return output.good() ? 0 : 7;
            }
            std::this_thread::sleep_for(10ms);
        }
    } catch (const std::exception&) {
        return 8;
    } catch (...) {
        return 9;
    }
    return 10;
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
